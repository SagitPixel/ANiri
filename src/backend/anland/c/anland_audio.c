#define _GNU_SOURCE
#include "anland_audio.h"
#include "common/protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/hook.h>

/* Default formats until the consumer negotiates the real device formats (protocol.h):
 * playback stereo L,R, mic mono, both S16LE. */
#define DEFAULT_RATE          48000
#define DEFAULT_PLAY_CHANNELS 2
#define DEFAULT_CAP_CHANNELS  1
/* ~1s of stereo S16 mic audio; bounds added latency, old samples drop on overflow.
 * Sized for the worst case (stereo) so the ring never under-allocates. */
#define MIC_RING_BYTES        (48000 * 2 * (int)sizeof(int16_t))
#define MAX_DGRAM             (64 * 1024)
/* Retry cadence of the periodic tick (also the RT-event drain point). */
#define TICK_SECS             1
/* How long the sender thread waits for a wake-up when the playback queue is empty. It
 * also bounds how long a stop request can go unnoticed, so detach never hangs. */
#define SEND_IDLE_MS          100

/* Plausibility bounds for a consumer-announced format; anything outside is ignored in
 * favour of the previous (or default) format instead of wrecking the stream. */
#define MIN_AUDIO_RATE        8000
#define MAX_AUDIO_RATE      384000
#define MAX_AUDIO_CHANNELS       2
#define MAX_AUDIO_QUANTUM    65536

/* Keep-alive payload: one short block of digital silence in the negotiated format.
 * The Android consumer runs one AAudio playback stream per direction and only advances
 * it while PCM keeps arriving; without this its buffer underruns and the native stream
 * stops (which in turn leaves the PipeWire link -- and every app on it -- sitting in
 * paused forever). One short block of silence per PipeWire cycle keeps that native
 * stream primed without adding audible latency or meaningful traffic. Only ever sent
 * while a consumer audio socket is attached. */
#define KEEPALIVE_FRAMES      480
#define KEEPALIVE_BYTES       (KEEPALIVE_FRAMES * MAX_AUDIO_CHANNELS * (int)sizeof(int16_t))

/* Playback queue between the realtime process callback (producer) and the sender thread
 * (consumer), one whole AUDIO_MSG_PCM message per slot with a 4-byte length prefix.
 * 64 KiB is ~10 periods of 48 kHz/1024-frame stereo PCM: deep enough that a normal
 * scheduling hiccup never drops audio, shallow enough to bound the queued latency. When
 * it does fill up a whole message is dropped and counted, never split. Must stay a power
 * of two. */
#define SEND_RING_BYTES       (64 * 1024)

/* Edge-event bits published by the realtime process callback and consumed (logged) by
 * the loop thread. The callback must not do stdio, so it only sets a bit with release
 * ordering; the loop thread picks it up on its periodic tick and logs it. */
#define AUDIO_EV_FIRST_PCM    (1u << 0)   /* first PCM forwarded on this attachment */
#define AUDIO_EV_KEEPALIVE_ON (1u << 1)   /* silence keep-alive started */
#define AUDIO_EV_STREAMING    (1u << 2)   /* keep-alive -> real PCM transition */
#define AUDIO_EV_SEND_FAIL    (1u << 3)   /* sender thread reported a send error */
#define AUDIO_EV_SHORT_WRITE  (1u << 4)   /* sender thread reported a partial write */
#define AUDIO_EV_QUEUE_FULL   (1u << 5)   /* playback queue overflowed, message dropped */

/* ---- single-producer / single-consumer byte queue -------------------------------
 *
 * Producer: on_capture_process() on the PipeWire realtime data thread.
 * Consumer: sender_thread() -- the only thread that ever touches the Android socket.
 *
 * head/tail are monotonically increasing byte counters, only ever written by one side
 * each, so they are atomic while the buffer itself needs no synchronisation beyond the
 * acquire/release pairing: the producer stores data and then publishes head with release,
 * the consumer loads head with acquire before reading that data (and vice versa for
 * tail). No lock, no allocation, and no file descriptor is shared with the RT thread. */

/* First header field of every queued slot: the AUDIO_MSG_PCM header, so a slot can be
 * handed to sendmsg by pointing straight at the queue. */
struct send_slot {
    uint32_t       len;    /* total bytes of this slot including this field */
    struct audio_msg msg;
};

struct send_ring {
    uint8_t           *buf;
    size_t             size;      /* power of two */
    _Atomic uint64_t   head;      /* producer publishes here */
    _Atomic uint64_t   tail;      /* consumer publishes here */
};

struct anland_audio {
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
    struct spa_hook        core_listener;
    struct spa_source     *tick_timer;
    bool                   pw_connected;   /* core + streams are up (loop thread only) */

    struct pw_stream      *capture;   /* virtual Audio/Sink  -> socket (playback) */
    struct spa_hook        capture_listener;
    struct pw_stream      *source;    /* virtual Audio/Source <- socket (capture) */
    struct spa_hook        source_listener;

    /* Negotiated formats (the consumer owns the hardware and dictates these via
     * AUDIO_MSG_FORMAT). Defaults stand until the consumer's formats arrive. */
    uint32_t               play_rate, play_channels;   /* the Audio/Sink format   */
    uint32_t               cap_rate, cap_channels;      /* the Audio/Source format */
    /* Requested buffer (frames) per stream from the consumer's latency preset;
     * 0 = let PipeWire choose the graph quantum. Applied as node.latency. */
    uint32_t               play_quantum, cap_quantum;
    /* PAST-TENSE playback format gate, loop thread only: set once a
     * AUDIO_MSG_FORMAT with AUDIO_ROLE_PLAYBACK has been accepted on the current
     * attachment. It is what arms keepalive_len. A new attachment clears it. */
    bool                   play_format_known;

    /* ---- mic path, loop thread only ----------------------------------------- */
    /* The local end of the audio socketpair, watched by `io` for mic PCM and format
     * announcements. The sender thread never touches this one: it owns its own duplicate
     * (`sock_fd`). */
    int                    audio_fd;
    struct spa_source     *io;
    /* on_audio_readable() (loop thread) is the only writer and on_source_process()
     * (also loop thread, because the mic stream deliberately does NOT use
     * PW_STREAM_FLAG_RT_PROCESS) the only reader, so this ring needs no synchronisation
     * -- the original single-threaded design is preserved. */
    uint8_t               *ring;
    size_t                 ring_size, ring_head, ring_tail, ring_fill;

    /* ---- playback queue, shared between exactly two threads ------------------ */
    struct send_ring       send;
    /* Byte length of one silence block in the announced format, published to the RT
     * callback. 0 means "no playback consumer attached / no PLAYBACK format yet", which
     * is also the gate that stops the RT callback from enqueueing anything. */
    _Atomic size_t         keepalive_len;
    /* Attachment generation. Bumped (release) on every attach, detach and accepted
     * PLAYBACK format, after the corresponding keepalive_len store. The RT callback
     * caches it and resets its private state whenever it changes, so a detach/reconnect
     * between two process cycles is still observed. */
    _Atomic uint32_t       epoch;
    /* RT -> loop edge events, drained by the periodic tick. */
    _Atomic uint32_t       events;
    /* errno captured by the sender thread for the deferred log. */
    _Atomic int            send_errno;
    /* Set by the sender thread when it gives up (consumer gone / framing broken); the
     * loop thread tears the attachment down in response. */
    _Atomic bool           stop_requested;

    /* ---- sender thread ------------------------------------------------------- */
    /* Owned by the loop thread: created in attach(), joined in detach_finish(). */
    pthread_t              sender;
    bool                   sender_started;
    int                    sock_fd;        /* -1 when detached; owned by the sender thread
                                            * for as long as it runs, closed by that thread
                                            * (or by the loop thread if it never started) */
    int                    ctl_read;       /* sender thread wakes on this */
    int                    ctl_write;      /* poked by the RT callback to request a flush */
    _Atomic bool           sender_stop;

    /* ---- keep-alive payload, immutable after start --------------------------- */
    uint8_t                silence[KEEPALIVE_BYTES];

    /* ---- RT callback private state, reset via `epoch` ------------------------ */
    uint32_t               rt_epoch_seen;  /* epoch this RT state belongs to */
    bool                   rt_pcm_seen;
    bool                   rt_keepalive_on;

    uint8_t                rx[MAX_DGRAM];
};

static struct anland_audio *g_audio = NULL;

static int connect_stream(struct pw_stream *stream, enum spa_direction direction,
                          uint32_t rate, uint32_t channels, uint32_t quantum,
                          bool rt_process);
static const struct spa_pod *build_format(struct spa_pod_builder *bld,
                                          uint32_t rate, uint32_t channels);
static void set_latency(struct pw_stream *stream, uint32_t quantum, uint32_t rate);

/* ---- send queue (SPSC, lock-free) ---- */

static bool send_ring_init(struct send_ring *r, size_t size)
{
    r->buf = malloc(size);
    if (!r->buf)
        return false;
    r->size = size;
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
    return true;
}

static size_t send_ring_used(const struct send_ring *r)
{
    return (size_t)(atomic_load_explicit(&r->head, memory_order_acquire) -
                    atomic_load_explicit(&r->tail, memory_order_relaxed));
}

/* Producer. Copies one whole message plus its 4-byte length prefix, or reports failure so
 * the caller can count a drop. Never splits a message and never blocks. */
static bool send_ring_push(struct send_ring *r, const struct audio_msg *hdr,
                           const uint8_t *payload, size_t payload_len)
{
    size_t total = sizeof(uint32_t) + sizeof(*hdr) + payload_len;
    uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);

    if (total > r->size || (size_t)(head - tail) + total > r->size)
        return false;

    uint32_t len = (uint32_t)total;
    uint8_t *dst = r->buf;
    size_t start = (size_t)(head & (r->size - 1));

    struct iovec src[3] = {
        { .iov_base = &len, .iov_len = sizeof(len) },
        { .iov_base = (void *)hdr, .iov_len = sizeof(*hdr) },
        { .iov_base = (void *)payload, .iov_len = payload_len },
    };
    size_t off = start;
    for (size_t i = 0; i < 3; i++) {
        const uint8_t *p = src[i].iov_base;
        size_t n = src[i].iov_len;
        if (n > r->size - off) {
            size_t first = r->size - off;
            memcpy(dst + off, p, first);
            memcpy(dst, p + first, n - first);
        } else {
            memcpy(dst + off, p, n);
        }
        off = (off + n) & (r->size - 1);
    }

    atomic_store_explicit(&r->head, head + total, memory_order_release);
    return true;
}

/* Consumer: locate the next whole slot. Returns false when the queue is empty (or holds a
 * slot that is still being published), otherwise *off is the absolute byte offset of the
 * slot start and *len its total length. The slot may wrap the end of the buffer, so the
 * caller works from the offset, not from a pointer. Valid until send_ring_consume(). */
static bool send_ring_peek(struct send_ring *r, size_t *off, size_t *len)
{
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);

    if (head == tail)
        return false;
    if ((size_t)(head - tail) < sizeof(struct send_slot))
        return false;   /* the length prefix is not fully published yet */

    size_t start = (size_t)(tail & (r->size - 1));
    uint32_t len32;
    if (sizeof(len32) > r->size - start) {
        /* The length prefix itself straddles the wrap. */
        size_t first = r->size - start;
        memcpy(&len32, r->buf + start, first);
        memcpy((uint8_t *)&len32 + first, r->buf, sizeof(len32) - first);
    } else {
        memcpy(&len32, r->buf + start, sizeof(len32));
    }

    if (len32 < sizeof(struct send_slot) || (size_t)(head - tail) < len32)
        return false;   /* not a whole slot yet: retry on the next pass */

    *off = start;
    *len = len32;
    return true;
}

static void send_ring_consume(struct send_ring *r, size_t len)
{
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    atomic_store_explicit(&r->tail, tail + len, memory_order_release);
}

/* ---- mic ring buffer (loop thread only) ---- */

static void ring_reset(struct anland_audio *a)
{
    a->ring_head = a->ring_tail = a->ring_fill = 0;
}

static void ring_write(struct anland_audio *a, const uint8_t *p, size_t n)
{
    if (n > a->ring_size) {           /* keep only the newest ring_size bytes */
        p += n - a->ring_size;
        n = a->ring_size;
    }
    if (a->ring_fill + n > a->ring_size) {   /* drop oldest to make room */
        size_t drop = a->ring_fill + n - a->ring_size;
        a->ring_tail = (a->ring_tail + drop) % a->ring_size;
        a->ring_fill -= drop;
    }
    size_t first = a->ring_size - a->ring_head;
    if (first > n)
        first = n;
    memcpy(a->ring + a->ring_head, p, first);
    memcpy(a->ring, p + first, n - first);
    a->ring_head = (a->ring_head + n) % a->ring_size;
    a->ring_fill += n;
}

static size_t ring_read(struct anland_audio *a, uint8_t *p, size_t n)
{
    size_t got = n < a->ring_fill ? n : a->ring_fill;
    size_t first = a->ring_size - a->ring_tail;
    if (first > got)
        first = got;
    memcpy(p, a->ring + a->ring_tail, first);
    memcpy(p + first, a->ring, got - first);
    a->ring_tail = (a->ring_tail + got) % a->ring_size;
    a->ring_fill -= got;
    return got;
}

/* ---- sender thread: the only place the Android socket is touched ---- */

/* Poke the sender thread. Safe from any thread: one byte onto a non-blocking pipe, no
 * allocation and no sleeping. EAGAIN means a wake-up is already pending, which is enough. */
static void sender_wake(struct anland_audio *a)
{
    if (a->ctl_write < 0)
        return;
    const uint8_t b = 1;
    ssize_t n = write(a->ctl_write, &b, sizeof(b));
    (void)n;
}

/* Send one queued message. Returns 0 when the slot was fully sent (and consumed),
 * -EAGAIN when the kernel had no room (slot kept for a later pass), -EPIPE when the
 * consumer is gone and the attachment should be dropped. */
static int sender_send_one(struct anland_audio *a, int fd)
{
    size_t off = 0, len = 0;
    if (!send_ring_peek(&a->send, &off, &len))
        return 0;

    /* One slot is one whole AUDIO_MSG_PCM message. It is a single contiguous run unless it
     * wrapped the end of the queue, in which case two iovecs describe it and sendmsg()
     * still puts exactly one message on the socket -- SEQPACKET framing is preserved. */
    struct iovec iov[2];
    size_t n_iov = 1;
    iov[0].iov_base = a->send.buf + off;
    iov[0].iov_len = len;
    if (off + len > a->send.size) {
        iov[0].iov_len = a->send.size - off;
        iov[1].iov_base = a->send.buf;
        iov[1].iov_len = len - iov[0].iov_len;
        n_iov = 2;
    }
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = n_iov };

    ssize_t n = sendmsg(fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return -EAGAIN;   /* keep the slot, let the queue push back */
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN)
            return -EPIPE;
        atomic_store_explicit(&a->send_errno, errno, memory_order_relaxed);
        atomic_fetch_or_explicit(&a->events, AUDIO_EV_SEND_FAIL, memory_order_release);
        return -EPIPE;   /* unknown failure: drop the attachment, do not spin on it */
    }
    if (n != (ssize_t)len) {
        /* One whole message per AUDIO_MSG_PCM on a message socket: a short write means the
         * framing can no longer be trusted. */
        atomic_fetch_or_explicit(&a->events, AUDIO_EV_SHORT_WRITE, memory_order_release);
        return -EPIPE;
    }
    send_ring_consume(&a->send, len);
    return 0;
}

/* Owns the transport fd for its whole lifetime. Exits only after the loop thread has
 * signalled a stop, so the loop thread can close/discard the fd immediately after the
 * join without any window in which this thread (or the RT callback) could still use it. */
static void *sender_thread(void *data)
{
    struct anland_audio *a = data;
    const int fd = a->sock_fd;
    uint8_t drain[64];

    while (!atomic_load_explicit(&a->sender_stop, memory_order_acquire)) {
        int res = 0;
        while (!atomic_load_explicit(&a->sender_stop, memory_order_acquire) &&
               (res = sender_send_one(a, fd)) == 0)
            ;   /* drain the queue */

        if (res == -EPIPE) {
            atomic_store_explicit(&a->stop_requested, true, memory_order_release);
            sender_wake(a);   /* ask the loop thread to finish the detach */
            break;
        }
        if (res == -EAGAIN) {
            /* Kernel send buffer full: wait for writability, still bounded so a stop
             * request is never missed for long. */
            struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
            poll(&pfd, 1, SEND_IDLE_MS);
            continue;
        }

        /* Queue empty: sleep until woken, or until the idle timeout re-checks stop. */
        struct pollfd pfd = { .fd = a->ctl_read, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, SEND_IDLE_MS) > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(a->ctl_read, drain, sizeof(drain));
            (void)n;
        }
    }

    /* Last flush so a stop request does not silence an already queued period. */
    while (sender_send_one(a, fd) == 0)
        ;
    close(fd);
    return NULL;
}

/* ---- transport attach / detach (loop thread; the caller holds the loop lock) ---- */

/* The io source owns the *local* audio_fd, so destroying it also closes it; the sender
 * thread owns the duplicate it was handed and closes that one itself. */
static void detach_audio_fd_locked(struct anland_audio *a)
{
    struct spa_source *io = a->io;
    a->io = NULL;
    a->audio_fd = -1;

    /* Disarm the keep-alive BEFORE the sender is torn down, so the RT callback cannot
     * enqueue a fresh period for a consumer we are about to forget. The epoch bump makes
     * the RT callback reset its private state even if it never observed the 0. */
    atomic_store_explicit(&a->keepalive_len, 0, memory_order_release);
    atomic_fetch_add_explicit(&a->epoch, 1, memory_order_release);
    a->play_format_known = false;

    if (a->sender_started) {
        atomic_store_explicit(&a->sender_stop, true, memory_order_release);
        sender_wake(a);
        pthread_join(a->sender, NULL);   /* bounded: the sender polls with SEND_IDLE_MS */
        a->sender_started = false;
    } else if (a->sock_fd >= 0) {
        close(a->sock_fd);   /* never started, so nobody else can own it */
    }
    a->sock_fd = -1;
    atomic_store_explicit(&a->stop_requested, false, memory_order_relaxed);

    /* Drain anything the sender left behind (queue is empty after the join; this also
     * clears the case where the sender never started). */
    send_ring_consume(&a->send, send_ring_used(&a->send));

    ring_reset(a);
    if (io)
        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), io);
}

/* ---- stream process callbacks ---- */

/* Logs the paused/streaming/error transitions that matter when the speaker graph will
 * not leave "paused". Runs on the loop thread. */
static void on_capture_state_changed(void *data, enum pw_stream_state old,
                                     enum pw_stream_state state, const char *error)
{
    (void)data;
    fprintf(stderr, "anland: speaker stream %s -> %s%s%s\n",
            pw_stream_state_as_string(old), pw_stream_state_as_string(state),
            error ? ": " : "", error ? error : "");
}

/* Desktop playback PCM -> playback queue, drained by the sender thread.
 *
 * The Android consumer owns a real AAudio playback stream that only advances while PCM
 * keeps arriving; if it ever runs dry it stops, and the PipeWire node it is fed from is
 * then left permanently paused -- apps on the link never leave "paused" and the graph
 * never reaches streaming. So a cycle with no captured PCM (nothing produced yet, or a
 * genuinely quiet period) still queues one short block of digital silence in the
 * announced format. Real PCM always wins: whenever the graph produced a period we forward
 * exactly that period and nothing else, so silence never displaces audio and no fixed-size
 * block is spliced into a live stream.
 *
 * THIS RUNS ON THE PIPEWIRE REALTIME DATA THREAD (PW_STREAM_FLAG_RT_PROCESS) and is not
 * covered by the thread-loop lock. It therefore touches no file descriptor, no loop
 * object and no lock: only this stream's SPA buffers, immutable fields, the lock-free
 * queue, and atomics. Sending is the sender thread's job. */
static void on_capture_process(void *data)
{
    struct anland_audio *a = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->capture);
    if (!b)
        return;

    struct spa_data *d = &b->buffer->datas[0];
    if (!d->chunk) {
        pw_stream_queue_buffer(a->capture, b);
        return;
    }

    /* A new attachment (or a fresh PLAYBACK format) invalidates the RT-private state, so
     * a consumer that reconnected between two cycles is still handled correctly. */
    uint32_t epoch = atomic_load_explicit(&a->epoch, memory_order_acquire);
    if (epoch != a->rt_epoch_seen) {
        a->rt_epoch_seen = epoch;
        a->rt_pcm_seen = false;
        a->rt_keepalive_on = false;
    }

    const uint8_t *payload = NULL;
    size_t size = 0;

    if (d->data && d->chunk->size > 0) {
        /* Real PCM (or upstream silence) at the graph's current period. Queueing exactly
         * what was dequeued keeps byte counts identical to the plain pass-through path,
         * so no fixed-size silence is ever spliced into a live stream -- no added
         * latency, no A/V drift. */
        payload = (uint8_t *)d->data + d->chunk->offset;
        size = d->chunk->size;
        uint32_t ev = AUDIO_EV_FIRST_PCM;
        if (a->rt_keepalive_on)
            ev = AUDIO_EV_STREAMING;      /* silence -> real PCM transition */
        else if (a->rt_pcm_seen)
            ev = 0;                       /* steady state: nothing to report */
        atomic_fetch_or_explicit(&a->events, ev, memory_order_release);
        a->rt_keepalive_on = false;
        a->rt_pcm_seen = true;
    } else {
        /* No captured PCM at all this cycle. keepalive_len is 0 unless a consumer is
         * attached AND has announced its PLAYBACK format, which is exactly the gate that
         * keeps us from queueing silence for nobody. The payload lives in this object and
         * is never written here, so the pointer stays valid; the length is atomic. */
        size = atomic_load_explicit(&a->keepalive_len, memory_order_acquire);
        if (size > 0 && size <= sizeof(a->silence)) {
            payload = a->silence;
            if (!a->rt_keepalive_on) {
                atomic_fetch_or_explicit(&a->events, AUDIO_EV_KEEPALIVE_ON,
                                         memory_order_release);
                a->rt_keepalive_on = true;
            }
        } else {
            size = 0;
        }
    }

    if (payload) {
        struct audio_msg h = { .type = AUDIO_MSG_PCM, .size = (uint32_t)size };
        if (!send_ring_push(&a->send, &h, payload, size))
            atomic_fetch_or_explicit(&a->events, AUDIO_EV_QUEUE_FULL, memory_order_release);
        else
            sender_wake(a);
    }
    pw_stream_queue_buffer(a->capture, b);
}

/* Fill the virtual mic source from the ring buffer (fed by Android). Silence-pads when the
 * ring underruns or while detached, so the source never glitches/vanishes.
 *
 * The mic stream is deliberately created WITHOUT PW_STREAM_FLAG_RT_PROCESS, so this runs
 * on the PipeWire thread loop -- the same thread as on_audio_readable(), which is the only
 * writer of the ring. The ring therefore stays single-threaded exactly as designed and
 * needs no lock. Keep it that way: adding RT_PROCESS here would introduce a data race on
 * ring_head/ring_tail/ring_fill without a lock-free ring rewrite. */
static void on_source_process(void *data)
{
    struct anland_audio *a = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->source);
    if (!b)
        return;

    struct spa_data *d = &b->buffer->datas[0];
    if (!d->data || !d->chunk) {
        pw_stream_queue_buffer(a->source, b);
        return;
    }
    const uint32_t stride = sizeof(int16_t) * a->cap_channels;
    uint32_t frames = d->maxsize / stride;
    if (b->requested && b->requested < frames)
        frames = b->requested;
    uint32_t bytes = frames * stride;

    size_t got = ring_read(a, d->data, bytes);
    if (got < bytes)
        memset((uint8_t *)d->data + got, 0, bytes - got);

    d->chunk->offset = 0;
    d->chunk->stride = stride;
    d->chunk->size = bytes;
    pw_stream_queue_buffer(a->source, b);
}

static const struct pw_stream_events capture_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_capture_process,
    .state_changed = on_capture_state_changed,
};

static const struct pw_stream_events source_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_source_process,
};

/* Apply a consumer-announced format. The virtual device stays continuously online and is
 * hot-plugged (disconnect + reconnect) ONLY when the format actually changes versus what
 * the stream is currently running -- an unchanged announcement (the common case, including
 * every consumer reconnect that re-sends the same format) is a no-op, so the node never
 * churns and plasma-pa keeps resolving the default sink/source.
 *
 * Defaults are role-correct: a field left 0 means "device default", which must equal the
 * value the stream was built with, otherwise the comparison below would treat an unset
 * field as a change and re-plug on every announcement. (One bug was a CAPTURE announce
 * with channels==0 defaulting to 2, never matching the mono source.)
 *
 * Every change is applied IN PLACE -- the node object (anland-speaker / anland-mic) is
 * never destroyed, so WirePlumber/plasma-pa keep their default reference and never log
 * "No object for name anland-speaker". A rate/channels change renegotiates the port format
 * via pw_stream_update_params(); a quantum-only change (AAudio's framesPerBurst
 * legitimately varies between opens) just updates node.latency. Neither disconnects.
 *
 * Runs on the loop thread, so the pw_stream calls are safe. */
static bool valid_format(const struct audio_format *f)
{
    return (f->role == AUDIO_ROLE_PLAYBACK || f->role == AUDIO_ROLE_CAPTURE) &&
           f->format == AUDIO_FORMAT_S16LE &&
           f->rate >= MIN_AUDIO_RATE && f->rate <= MAX_AUDIO_RATE &&
           f->channels > 0 && f->channels <= MAX_AUDIO_CHANNELS &&
           f->quantum <= MAX_AUDIO_QUANTUM;
}

/* Bytes of S16 silence the keep-alive path queues when a cycle produced no PCM at all.
 * The length is derived from the announced quantum but is deliberately CLAMPED to
 * KEEPALIVE_FRAMES (480 frames ~= 10 ms at 48 kHz): the payload is a small priming block,
 * not a whole negotiated period. So it is not "exactly one AAudio burst" when the consumer
 * asks for a larger quantum -- do not describe it that way. Raising the cap would mean
 * queueing proportionally more silence per cycle for no benefit.
 *
 * The result is also bounded by sizeof(a->silence) (channels are already limited by
 * valid_format()), so the process callback can never overrun the buffer. */
static size_t silence_bytes_for(uint32_t quantum, uint32_t channels)
{
    uint32_t frames = quantum ? quantum : KEEPALIVE_FRAMES;
    if (frames > KEEPALIVE_FRAMES)
        frames = KEEPALIVE_FRAMES;
    size_t bytes = (size_t)frames * channels * sizeof(int16_t);
    return bytes > KEEPALIVE_BYTES ? KEEPALIVE_BYTES : bytes;
}

/* Arm the keep-alive for the current attachment. Called ONLY once
 * play_format_known is true, i.e. the consumer actually announced its playback format on
 * this attachment, so a bare transport attach (or a stream rebuild with no consumer)
 * leaves keepalive_len at 0 and the RT callback queueing nothing. */
static void arm_keepalive(struct anland_audio *a)
{
    size_t bytes = silence_bytes_for(a->play_quantum, a->play_channels);
    /* Publish the length first, then the epoch: the RT callback acquires the epoch and
     * only then trusts the length it reads. */
    atomic_store_explicit(&a->keepalive_len, bytes, memory_order_release);
    atomic_fetch_add_explicit(&a->epoch, 1, memory_order_release);
}

static void apply_format(struct anland_audio *a, const struct audio_format *f)
{
    const bool playback = (f->role == AUDIO_ROLE_PLAYBACK);
    const uint32_t rate = f->rate ? f->rate : DEFAULT_RATE;
    const uint32_t channels = f->channels ? f->channels
                                          : (playback ? DEFAULT_PLAY_CHANNELS : DEFAULT_CAP_CHANNELS);

    uint32_t *cur_rate     = playback ? &a->play_rate : &a->cap_rate;
    uint32_t *cur_channels = playback ? &a->play_channels : &a->cap_channels;
    uint32_t *cur_quantum  = playback ? &a->play_quantum : &a->cap_quantum;
    struct pw_stream *stream = playback ? a->capture : a->source;

    const bool format_changed = (rate != *cur_rate || channels != *cur_channels);
    const bool quantum_changed = (f->quantum != *cur_quantum);

    *cur_rate = rate;
    *cur_channels = channels;
    *cur_quantum = f->quantum;
    if (playback) {
        /* The consumer has told us what to play. Record the fact FIRST, then arm: a
         * consumer whose device already matches our defaults (48000/2, the common case)
         * would otherwise take the "unchanged" early return below and never arm at all. */
        a->play_format_known = true;
        arm_keepalive(a);
    }

    if (!format_changed && !quantum_changed)
        return;   /* unchanged -> keep the device online, no hot-plug */

    fprintf(stderr, "anland: audio %s format %u Hz, %u ch, S16LE, quantum %u\n",
            playback ? "playback" : "capture", rate, channels, f->quantum);

    if (!a->pw_connected || !stream)
        return;   /* build_pw() will pick up the new values when it (re)creates the stream */

    if (format_changed) {
        /* Renegotiate the port format on the LIVE stream. update_params keeps the node
         * object alive (same id) and re-runs format negotiation, so the default sink/
         * source reference is never lost -- no churn, no plasma-pa "No object" spam. */
        set_latency(stream, f->quantum, rate);
        uint8_t buffer[1024];
        struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const struct spa_pod *params[1] = { build_format(&bld, rate, channels) };
        pw_stream_update_params(stream, params, 1);
    } else if (f->quantum > 0) {
        /* Latency-only tweak: node.latency in place, nothing renegotiates. */
        set_latency(stream, f->quantum, rate);
    }
}

/* Mic PCM / format announcements arriving from the consumer. Runs on the loop thread. */
static void on_audio_readable(void *data, int fd, uint32_t mask)
{
    struct anland_audio *a = data;
    if (fd != a->audio_fd)
        return;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
        detach_audio_fd_locked(a);
        return;
    }
    if (!(mask & SPA_IO_IN))
        return;

    for (;;) {
        /* MSG_TRUNC: a datagram larger than rx is truncated but reports its real size,
         * so a stray oversized frame can never be mistaken for a well-formed one. */
        ssize_t n = recv(fd, a->rx, sizeof(a->rx), MSG_DONTWAIT | MSG_TRUNC);
        if (n == 0) {
            detach_audio_fd_locked(a);
            break;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                detach_audio_fd_locked(a);
            break;
        }
        if ((size_t)n < sizeof(struct audio_msg) || (size_t)n > sizeof(a->rx))
            continue;
        struct audio_msg h;
        memcpy(&h, a->rx, sizeof(h));
        size_t avail = (size_t)n - sizeof(struct audio_msg);

        if (h.type == AUDIO_MSG_FORMAT) {
            /* Framing is trusted only when the header and the payload agree exactly. */
            if (h.size != sizeof(struct audio_format) || h.size != avail)
                continue;
            struct audio_format f;
            memcpy(&f, a->rx + sizeof(struct audio_msg), sizeof(f));
            if (valid_format(&f))
                apply_format(a, &f);
            else
                fprintf(stderr, "anland: ignoring invalid audio format "
                                "(rate=%u ch=%u fmt=%u role=%u quantum=%u)\n",
                        f.rate, f.channels, f.format, f.role, f.quantum);
            continue;
        }
        if (h.type != AUDIO_MSG_PCM || h.size != avail)
            continue;
        if (a->cap_channels > 0 &&
            h.size % (sizeof(int16_t) * a->cap_channels) != 0)
            continue;   /* partial frame: drop rather than shift the mic by a sample */
        ring_write(a, a->rx + sizeof(struct audio_msg), h.size);
    }
}

/* ---- PipeWire connection lifecycle (build / teardown / auto-reconnect) ---- */

/* True periodic tick: pw_loop_update_timer() with a non-NULL interval re-arms itself, so
 * this only has to be armed once (anland_audio_start) and keeps firing forever. It drives
 * both the reconnect retry and the drain of the realtime callback's edge events. */
static void arm_tick(struct anland_audio *a)
{
    struct timespec val = { .tv_sec = TICK_SECS, .tv_nsec = 0 };
    pw_loop_update_timer(pw_thread_loop_get_loop(a->loop), a->tick_timer,
                         &val, &val, false);
}

/* Fatal, non-recoverable error on the core proxy means the sound service connection
 * was lost (e.g. pipewire/wireplumber restarted). Drop the dead core+streams and let the
 * periodic tick rebuild; the audio socket / mic ring are untouched so the consumer side
 * keeps working and resumes the moment PipeWire is back. */
static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    struct anland_audio *a = data;
    (void)seq;
    (void)message;
    if (id == PW_ID_CORE && res == -EPIPE) {
        a->pw_connected = false;   /* the periodic tick notices and rebuilds */
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

/* Build an S16LE EnumFormat POD for rate/channels into the caller's builder. Stereo is
 * L,R (FL,FR); anything else collapses to a plain mono channel. */
static const struct spa_pod *build_format(struct spa_pod_builder *bld,
                                          uint32_t rate, uint32_t channels)
{
    struct spa_audio_info_raw info = {
        .format = SPA_AUDIO_FORMAT_S16_LE,
        .rate = rate,
        .channels = channels,
    };
    if (channels >= 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    }
    return spa_format_audio_raw_build(bld, SPA_PARAM_EnumFormat, &info);
}

/* node.latency = "quantum/rate" asks PipeWire to run this node at that buffer size.
 * quantum == 0 leaves the graph default. Applied in place -- never re-plugs the node. */
static void set_latency(struct pw_stream *stream, uint32_t quantum, uint32_t rate)
{
    if (quantum == 0)
        return;
    char latency[32];
    snprintf(latency, sizeof(latency), "%u/%u", quantum, rate);
    struct spa_dict_item items[] = {
        SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, latency),
    };
    struct spa_dict dict = SPA_DICT_INIT(items, 1);
    pw_stream_update_properties(stream, &dict);
}

/* flags are per-stream: the speaker asks for PW_STREAM_FLAG_RT_PROCESS (its callback is
 * written to be lock/alloc/log-free and must run inside the graph's own cycle), while the
 * mic must NOT use it so that its process callback stays on the PipeWire thread loop, i.e.
 * the same thread that fills the mic ring from the socket. `rt_process` selects that
 * explicitly instead of forcing RT semantics on both streams. */
static int connect_stream(struct pw_stream *stream, enum spa_direction direction,
                          uint32_t rate, uint32_t channels, uint32_t quantum,
                          bool rt_process)
{
    uint32_t flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS;

    set_latency(stream, quantum, rate);

    if (rt_process)
        flags |= PW_STREAM_FLAG_RT_PROCESS;

    uint8_t buffer[1024];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1] = { build_format(&bld, rate, channels) };

    return pw_stream_connect(stream, direction, PW_ID_ANY, flags, params, 1);
}

/* Tear down the core proxy and both streams, leaving the loop, context, timers, mic ring,
 * send queue and transport intact. Idempotent. */
static void teardown_pw(struct anland_audio *a)
{
    if (a->capture) {
        spa_hook_remove(&a->capture_listener);
        pw_stream_destroy(a->capture);
        a->capture = NULL;
    }
    if (a->source) {
        spa_hook_remove(&a->source_listener);
        pw_stream_destroy(a->source);
        a->source = NULL;
    }
    if (a->core) {
        spa_hook_remove(&a->core_listener);
        pw_core_disconnect(a->core);
        a->core = NULL;
    }
}

/* (Re)connect the core and (re)create both streams. Returns 0 on success. */
static int build_pw(struct anland_audio *a)
{
    a->core = pw_context_connect(a->context, NULL, 0);
    if (!a->core)
        return -1;
    pw_core_add_listener(a->core, &a->core_listener, &core_events, a);

    /* Re-arm the keep-alive only if the consumer is genuinely still attached AND already
     * announced its playback format: this can be a rebuild after the sound service went
     * away, in which case the consumer never re-announces. With no consumer, or with one
     * that has not spoken yet, keepalive_len must stay 0. */
    if (a->io && a->play_format_known)
        arm_keepalive(a);

    /* Own a virtual sink so the container has a real output device instead of only the
     * auto-null "Dummy Output": apps play into this Audio/Sink, WirePlumber makes it the
     * default (high priority beats auto_null), and on_capture_process receives the mixed
     * PCM directly -- no monitor capture, nothing bound to the dummy.
     *
     * The idle properties are what make remote playback work at all. Without them this
     * node is a follower that is only pulled while a driver is running it, so it sits
     * suspended (pw-top: RATE 0/QUANT 0) and every app on the link stays in "paused"
     * forever. always-process keeps the node runnable while unlinked (and implies
     * want-driver); pause-on-idle/suspend-on-idle keep it from being torn down between
     * tracks; session.suspend-timeout-seconds stops WirePlumber's session layer from
     * suspending it after a few idle seconds. Together they let the node reach
     * streaming -- and stay there -- while on_capture_process keeps feeding the consumer. */
    a->capture = pw_stream_new(a->core, "anland-speaker",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CLASS, "Audio/Sink",
            PW_KEY_NODE_NAME, "anland-speaker",
            PW_KEY_NODE_DESCRIPTION, "Anland remote speaker",
            PW_KEY_PRIORITY_SESSION, "1010",   /* outrank the auto-null dummy sink */
            PW_KEY_PRIORITY_DRIVER, "1010",
            PW_KEY_NODE_ALWAYS_PROCESS, "true",
            PW_KEY_NODE_PAUSE_ON_IDLE, "false",
            PW_KEY_NODE_SUSPEND_ON_IDLE, "false",
            "session.suspend-timeout-seconds", "0",
            NULL));
    if (!a->capture)
        return -1;
    pw_stream_add_listener(a->capture, &a->capture_listener, &capture_events, a);

    /* Expose the Android mic to Linux apps as a recordable source. */
    a->source = pw_stream_new(a->core, "anland-mic",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CLASS, "Audio/Source",
            PW_KEY_NODE_NAME, "anland-mic",
            PW_KEY_NODE_DESCRIPTION, "Anland remote microphone",
            PW_KEY_PRIORITY_SESSION, "1010",   /* outrank the auto-null dummy source */
            PW_KEY_PRIORITY_DRIVER, "1010",
            NULL));
    if (!a->source)
        return -1;
    pw_stream_add_listener(a->source, &a->source_listener, &source_events, a);

    /* Speaker: RT process callback. Its callback is lock/alloc/log-free and must be fed
     * inside the graph's own cycle or the driver can advance without the consumer ever
     * receiving a period. */
    if (connect_stream(a->capture, PW_DIRECTION_INPUT, a->play_rate, a->play_channels,
                       a->play_quantum, true) < 0)
        return -1;
    /* Mic: explicitly NOT RT. on_source_process() drains the ring that on_audio_readable()
     * fills, both on the thread loop, which is what the ring's single-threaded design
     * assumes; running it on the data thread would be a data race. */
    if (connect_stream(a->source, PW_DIRECTION_OUTPUT, a->cap_rate, a->cap_channels,
                       a->cap_quantum, false) < 0)
        return -1;

    return 0;
}

/* Loop thread: turn the realtime callback's edge bits into log lines (stdio is not
 * realtime-safe, so it cannot be done on the data thread) and act on a stop request raised
 * by the sender thread. One pass per tick is plenty for both. */
static void service_rt_state(struct anland_audio *a)
{
    uint32_t ev = atomic_exchange_explicit(&a->events, 0, memory_order_acquire);
    if (!ev)
        goto check_stop;
    if (ev & AUDIO_EV_KEEPALIVE_ON)
        fprintf(stderr, "anland: playback silent, keep-alive started (%zu bytes)\n",
                atomic_load_explicit(&a->keepalive_len, memory_order_relaxed));
    if (ev & AUDIO_EV_STREAMING)
        fprintf(stderr, "anland: playback streaming (real PCM)\n");
    else if (ev & AUDIO_EV_FIRST_PCM)
        fprintf(stderr, "anland: playback streaming (first PCM captured)\n");
    if (ev & AUDIO_EV_SEND_FAIL)
        fprintf(stderr, "anland: audio sendmsg failed: %s\n",
                strerror(atomic_load_explicit(&a->send_errno, memory_order_relaxed)));
    if (ev & AUDIO_EV_SHORT_WRITE)
        fprintf(stderr, "anland: short audio write, transport detached\n");
    if (ev & AUDIO_EV_QUEUE_FULL)
        fprintf(stderr, "anland: playback queue full, dropped one period\n");

check_stop:
    /* The sender thread gives up on a dead consumer by itself (it is the only thread that
     * touches the socket) and raises this flag; the loop thread owns the teardown. */
    if (a->io && atomic_load_explicit(&a->stop_requested, memory_order_acquire)) {
        fprintf(stderr, "anland: audio consumer went away, detaching transport\n");
        detach_audio_fd_locked(a);
    }
}

/* Periodic loop-thread tick: drains the RT edge events and retries the PipeWire
 * connection while the sound service is down. It re-arms itself (interval is set), so it
 * must be armed exactly once. */
static void on_tick(void *data, uint64_t expirations)
{
    struct anland_audio *a = data;
    (void)expirations;

    service_rt_state(a);

    if (a->pw_connected)
        return;

    teardown_pw(a);            /* clear any half-built state from a failed attempt */
    if (build_pw(a) == 0)
        a->pw_connected = true;
    else
        teardown_pw(a);        /* the periodic tick will try again */
}

/* ---- public API ---- */

void anland_audio_set_fd(int audio_fd)
{
    struct anland_audio *a = g_audio;
    if (!a)
        return;

    /* Duplicate BEFORE touching engine state: display_producer retains the original fd,
     * so a failure here must not cost us a working attachment. */
    int owned_fd = -1;
    if (audio_fd >= 0) {
        owned_fd = fcntl(audio_fd, F_DUPFD_CLOEXEC, 3);
        if (owned_fd < 0) {
            fprintf(stderr, "anland: failed to duplicate audio fd: %s\n", strerror(errno));
            return;
        }
    }

    pw_thread_loop_lock(a->loop);

    detach_audio_fd_locked(a);

    if (owned_fd >= 0) {
        /* The consumer announces both device formats (AUDIO_MSG_FORMAT) right after this
         * socket comes up; on_audio_readable applies them. The keep-alive stays disarmed
         * until that PLAYBACK announcement has been seen on this attachment, so a bare
         * attach never queues silence for a consumer that is not listening yet. */
        a->io = pw_loop_add_io(pw_thread_loop_get_loop(a->loop), owned_fd,
                               SPA_IO_IN, true, on_audio_readable, a);
        if (!a->io) {
            close(owned_fd);
            fprintf(stderr, "anland: failed to register audio fd: %s\n", strerror(errno));
        } else {
            a->audio_fd = owned_fd;
            /* The sender owns this duplicate for its whole lifetime and closes it, so the
             * loop thread must NOT close it while the thread may still run. */
            a->sock_fd = owned_fd;
            atomic_store_explicit(&a->sender_stop, false, memory_order_relaxed);
            if (pthread_create(&a->sender, NULL, sender_thread, a) == 0) {
                a->sender_started = true;
                fprintf(stderr, "anland: audio transport attached\n");
            } else {
                /* Without a sender there is no playback path. Hand the duplicate to the
                 * never-started branch of the detach so it is closed exactly once, and
                 * drop the attachment rather than pretend it works. */
                detach_audio_fd_locked(a);
                fprintf(stderr, "anland: failed to start audio sender thread\n");
            }
        }
    } else {
        fprintf(stderr, "anland: audio transport detached\n");
    }

    pw_thread_loop_unlock(a->loop);
}

int anland_audio_start(void)
{
    if (g_audio)
        return 0;

    pw_init(NULL, NULL);

    struct anland_audio *a = calloc(1, sizeof(*a));
    if (!a)
        return -1;
    a->audio_fd = -1;
    a->sock_fd = -1;
    a->ctl_read = -1;
    a->ctl_write = -1;
    /* No consumer yet: keep-alive stays disarmed until a PLAYBACK format arrives. */
    atomic_store_explicit(&a->keepalive_len, 0, memory_order_relaxed);
    a->play_rate = DEFAULT_RATE;
    a->play_channels = DEFAULT_PLAY_CHANNELS;
    a->cap_rate = DEFAULT_RATE;
    a->cap_channels = DEFAULT_CAP_CHANNELS;
    a->ring_size = MIC_RING_BYTES;
    a->ring = malloc(a->ring_size);
    if (!a->ring)
        goto fail;
    if (!send_ring_init(&a->send, SEND_RING_BYTES))
        goto fail;

    a->loop = pw_thread_loop_new("anland-audio", NULL);
    if (!a->loop)
        goto fail;

    a->context = pw_context_new(pw_thread_loop_get_loop(a->loop), NULL, 0);
    if (!a->context)
        goto fail;

    a->tick_timer = pw_loop_add_timer(pw_thread_loop_get_loop(a->loop), on_tick, a);
    if (!a->tick_timer)
        goto fail;

    /* Control pipe: the sender thread sleeps on the read end and the RT callback pokes the
     * write end to ask for a flush. Both ends are non-blocking, so a poke from the
     * realtime thread can never block. Nothing is ever pre-written into it: the sender
     * reads it only when it is readable, so an empty pipe simply means "nothing to flush".
     * The pipe is read directly rather than through a loop source, so the sender thread is
     * the only reader and no loop callback can steal its wake-ups.
     * Teardown uses the sender's idle timeout (SEND_IDLE_MS) plus an explicit stop flag
     * instead of piping a stop byte, which is why the pipe may be full of pokes without
     * delaying a stop. */
    {
        int fds[2];
        if (pipe(fds) < 0)
            goto fail;
        a->ctl_read = fds[0];
        a->ctl_write = fds[1];
        for (int i = 0; i < 2; i++) {
            int fl = fcntl(fds[i], F_GETFL, 0);
            if (fl >= 0)
                fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
            int fdfl = fcntl(fds[i], F_GETFD, 0);
            if (fdfl >= 0)
                fcntl(fds[i], F_SETFD, fdfl | FD_CLOEXEC);
        }
    }

    if (pw_thread_loop_start(a->loop) < 0)
        goto fail;

    /* First connection attempt under the loop lock. If PipeWire is not up yet, the
     * periodic tick retries and audio comes up when the sound service appears. Either way
     * the engine object exists. */
    pw_thread_loop_lock(a->loop);
    if (build_pw(a) == 0)
        a->pw_connected = true;
    else
        teardown_pw(a);
    arm_tick(a);
    pw_thread_loop_unlock(a->loop);

    g_audio = a;
    return 0;

fail:
    if (a->loop) {
        pw_thread_loop_stop(a->loop);
        if (a->tick_timer)
            pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->tick_timer);
    }
    if (a->context)
        pw_context_destroy(a->context);
    if (a->loop)
        pw_thread_loop_destroy(a->loop);
    if (a->ctl_read >= 0)
        close(a->ctl_read);
    if (a->ctl_write >= 0)
        close(a->ctl_write);
    free(a->send.buf);
    free(a->ring);
    free(a);
    pw_deinit();
    return -1;
}

void anland_audio_stop(void)
{
    struct anland_audio *a = g_audio;
    if (!a)
        return;
    g_audio = NULL;

    /* Stop the callbacks first, then unwind in reverse creation order: streams, the
     * transport io source (which owns audio_fd and closes it), the sender thread (joined
     * inside detach_audio_fd_locked, which is why it holds the lock), the loop sources,
     * and only then the context/loop. Nothing may call back into a destroyed object. */
    if (a->loop)
        pw_thread_loop_stop(a->loop);
    teardown_pw(a);
    if (a->io)
        detach_audio_fd_locked(a);
    else if (a->sock_fd >= 0) {
        /* No io source (cannot normally happen): still stop and reap the sender. */
        atomic_store_explicit(&a->sender_stop, true, memory_order_release);
        sender_wake(a);
        if (a->sender_started)
            pthread_join(a->sender, NULL);
        a->sender_started = false;
        a->sock_fd = -1;
    }
    if (a->tick_timer)
        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->tick_timer);
    if (a->ctl_read >= 0)
        close(a->ctl_read);
    if (a->ctl_write >= 0)
        close(a->ctl_write);
    if (a->context)
        pw_context_destroy(a->context);
    if (a->loop)
        pw_thread_loop_destroy(a->loop);
    free(a->send.buf);
    free(a->ring);
    free(a);
    pw_deinit();
}
