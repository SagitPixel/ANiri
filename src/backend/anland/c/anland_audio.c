#define _GNU_SOURCE
#include "anland_audio.h"
#include "common/protocol.h"

#include <errno.h>
#include <fcntl.h>
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
/* Retry cadence after the PipeWire connection is lost (sound service restart, etc). */
#define RECONNECT_SECS    1

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
 * paused forever). One short period of silence per PipeWire cycle keeps that native
 * stream primed without adding audible latency or meaningful traffic. Only ever sent
 * while a consumer audio socket is attached. */
#define KEEPALIVE_FRAMES      480
#define KEEPALIVE_BYTES       (KEEPALIVE_FRAMES * MAX_AUDIO_CHANNELS * (int)sizeof(int16_t))

/* Edge-event bits published by the realtime process callback and consumed (logged) by
 * the loop thread. The callback must not do stdio, so it only sets a bit with release
 * ordering; the loop thread picks it up on its periodic tick and logs it. */
#define AUDIO_EV_FIRST_PCM    (1u << 0)   /* first PCM forwarded on this attachment */
#define AUDIO_EV_KEEPALIVE_ON (1u << 1)   /* silence keep-alive started */
#define AUDIO_EV_STREAMING    (1u << 2)   /* keep-alive -> real PCM transition */
#define AUDIO_EV_SEND_FAIL    (1u << 3)   /* sendmsg reported a non-transient error */
#define AUDIO_EV_SHORT_WRITE  (1u << 4)   /* sendmsg did not write the whole message */

struct anland_audio {
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
    struct spa_hook        core_listener;
    struct spa_source     *reconnect_timer;
    bool                   pw_connected;   /* core + streams are up */

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

    /* ---- transport, loop thread only ---------------------------------------- */
    int                    audio_fd;  /* owned duplicate; -1 when detached */
    struct spa_source     *io;        /* loop io source watching audio_fd for reads */

    /* Wake-up pipe used by the realtime process callback to ask the loop thread for a
     * detach. Written from the RT thread (one non-blocking byte), drained on the loop
     * thread; the loop thread owns both descriptors. */
    int                    notify_read, notify_write;
    struct spa_source     *notify;

    /* Digital-silence keep-alive payload: constant after start, only ever read by the
     * realtime process callback, so it needs no synchronisation. */
    uint8_t                silence[KEEPALIVE_BYTES];

    /* ---- state shared with the realtime process callback -------------------- */
    /* The speaker stream is the only stream created with PW_STREAM_FLAG_RT_PROCESS, so
     * its process callback runs on the PipeWire realtime data thread and is NOT covered
     * by the thread-loop lock. Everything it touches lives in this block and is either
     * immutable after start (silence[] above) or atomic. Kept together so the RT thread
     * does not bounce cache lines with the loop thread's mutable state.
     *   keepalive_len   silence payload length for the announced format (0 = disabled)
     *   stop_requested  the consumer died / framing broke; the loop thread must drop it
     *   detach_serial   attachment generation that stop_requested refers to
     *   events          RT -> loop edge events, drained by the periodic tick
     *   last_errno      errno captured at the failing sendmsg, for the deferred log
     *   pcm_seen        RT-private: real PCM was already forwarded on this attachment
     *   keepalive_on    RT-private: silence is currently being fed                  */
    _Atomic size_t         keepalive_len;
    _Atomic bool           stop_requested;
    _Atomic uint32_t       attach_serial;   /* bumped on every successful attach */
    _Atomic uint32_t       detach_serial;   /* attachment stop_requested refers to */
    _Atomic uint32_t       events;
    _Atomic int            last_errno;
    bool                   pcm_seen;
    bool                   keepalive_on;

    /* ---- mic ring + loop-thread-only scratch --------------------------------- */
    /* on_audio_readable() (loop thread) is the only writer and on_source_process()
     * (also loop thread, because the mic stream deliberately does NOT use
     * PW_STREAM_FLAG_RT_PROCESS) the only reader, so the ring needs no synchronisation
     * -- the original single-threaded design is preserved. */
    uint8_t               *ring;
    size_t                 ring_size, ring_head, ring_tail, ring_fill;

    /* Silence length in the announced format, mirrored into keepalive_len for the RT
     * callback under the thread-loop lock. Loop thread only. */
    size_t                 silence_bytes;

    uint8_t                rx[MAX_DGRAM];
};

static struct anland_audio *g_audio = NULL;

static int connect_stream(struct pw_stream *stream, enum spa_direction direction,
                          uint32_t rate, uint32_t channels, uint32_t quantum,
                          bool rt_process);
static const struct spa_pod *build_format(struct spa_pod_builder *bld,
                                          uint32_t rate, uint32_t channels);
static void set_latency(struct pw_stream *stream, uint32_t quantum, uint32_t rate);

/* ---- mic ring buffer (single-threaded: loop thread only) ---- */

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

/* The caller holds the thread-loop lock, or is executing on that loop. The io
 * source owns audio_fd, so destroying it also closes the duplicated descriptor. */
static void detach_audio_fd_locked(struct anland_audio *a)
{
    struct spa_source *io = a->io;
    a->io = NULL;
    a->audio_fd = -1;
    /* Publish "no keep-alive" first: the realtime callback gates on keepalive_len alone,
     * so it can never send a payload whose length was not published for this attachment. */
    atomic_store_explicit(&a->keepalive_len, 0, memory_order_release);
    atomic_store_explicit(&a->stop_requested, false, memory_order_relaxed);
    a->pcm_seen = false;
    a->keepalive_on = false;
    ring_reset(a);
    if (io)
        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), io);
}

/* Ask the loop thread to drop the transport. Called from the realtime process callback,
 * where destroying a loop source is not allowed, so this only raises an atomic flag and
 * pokes a pipe: write(2) of one byte onto a non-blocking pipe is a plain syscall with no
 * allocation and no sleeping, and is the documented way to wake an event loop from
 * another thread. PipeWire exposes no API that is documented as safe to call from a
 * process callback on the data thread (pw_loop_update_timer() included), so an explicit
 * byte pipe is used instead of reaching into the loop implementation.
 * The failing attachment's serial is remembered so a detach requested against a dead
 * socket can never tear down a socket installed in the meantime. */
static void defer_detach(struct anland_audio *a)
{
    if (atomic_exchange_explicit(&a->stop_requested, true, memory_order_relaxed))
        return;   /* already asked; the loop thread owns the rest */
    atomic_store_explicit(&a->detach_serial,
                          atomic_load_explicit(&a->attach_serial, memory_order_relaxed),
                          memory_order_relaxed);
    if (a->notify_write >= 0) {
        const uint8_t b = 1;
        ssize_t n = write(a->notify_write, &b, sizeof(b));
        (void)n;   /* EAGAIN means a wake-up is already pending, which is enough */
    }
}

/* Loop thread: consume the wake-up byte and perform the deferred detach. */
static void on_notify(void *data, int fd, uint32_t mask)
{
    struct anland_audio *a = data;
    uint8_t buf[64];
    (void)mask;
    if (fd != a->notify_read)
        return;
    ssize_t n = read(fd, buf, sizeof(buf));
    (void)n;

    if (!atomic_load_explicit(&a->stop_requested, memory_order_acquire))
        return;
    atomic_store_explicit(&a->stop_requested, false, memory_order_relaxed);
    if (!a->io ||
        atomic_load_explicit(&a->detach_serial, memory_order_relaxed) !=
            atomic_load_explicit(&a->attach_serial, memory_order_relaxed))
        return;   /* already replaced (or dropped) by a newer attachment */
    detach_audio_fd_locked(a);
}

/* ---- stream process callbacks (run on the PipeWire thread loop) ---- */

/* Logs the paused/streaming/error transitions that matter when the speaker graph will
 * not leave "paused". Not per-buffer, so it is safe on the loop thread. */
static void on_capture_state_changed(void *data, enum pw_stream_state old,
                                     enum pw_stream_state state, const char *error)
{
    (void)data;
    fprintf(stderr, "anland: speaker stream %s -> %s%s%s\n",
            pw_stream_state_as_string(old), pw_stream_state_as_string(state),
            error ? ": " : "", error ? error : "");
}

/* Desktop audio captured from the default sink's monitor -> push to the socket so
 * the consumer plays it.
 *
 * The Android consumer owns a real AAudio playback stream that only advances while
 * PCM keeps arriving; if it ever runs dry it stops, and the PipeWire node it is fed
 * from is then left permanently paused -- apps on the link never leave "paused" and
 * the graph never reaches streaming. So a cycle with no captured PCM (nothing produced
 * yet, or a genuinely quiet period) still sends one short block of digital silence in
 * the negotiated format. Real PCM always wins: whenever the graph produced a period we
 * forward exactly that period and nothing else, so silence never displaces audio and no
 * fixed-size block is spliced into a live stream. Only ever sent while a consumer audio
 * socket is attached and only after that consumer announced its PLAYBACK format.
 * Dropped (still drained) while detached.
 *
 * THIS RUNS ON THE PIPEWIRE REALTIME DATA THREAD (PW_STREAM_FLAG_RT_PROCESS) and is not
 * covered by the thread-loop lock, so it must not take locks, allocate, log, or call
 * into the loop. Everything it touches is either read-only here (silence[], which is
 * written once before the stream is connected) or atomic; observable state changes are
 * published as edge bits in `events` for the loop thread to log. */
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

    const uint8_t *payload = NULL;
    size_t size = 0;

    if (d->data && d->chunk->size > 0) {
        /* Real PCM (or upstream silence) at the graph's current period. Sending exactly
         * what was dequeued keeps byte counts identical to the plain pass-through path,
         * so no fixed-size silence is ever spliced into a live stream -- no added
         * latency, no A/V drift. */
        payload = (uint8_t *)d->data + d->chunk->offset;
        size = d->chunk->size;
        uint32_t ev = AUDIO_EV_FIRST_PCM;
        if (a->keepalive_on)
            ev = AUDIO_EV_STREAMING;      /* silence -> real PCM transition */
        else if (a->pcm_seen)
            ev = 0;                       /* steady state: nothing to report */
        atomic_fetch_or_explicit(&a->events, ev, memory_order_release);
        a->keepalive_on = false;
        a->pcm_seen = true;
    } else {
        /* No captured PCM at all this cycle: the graph is either not producing yet or has
         * genuinely gone quiet. Feed the consumer one short block of digital silence in
         * the announced format so its native playback stream stays primed and the node
         * can reach streaming instead of sitting paused forever. keepalive_len is 0 until
         * a consumer announced its PLAYBACK format, which is exactly the gate we want.
         * The payload lives in this object and is never written here, so the pointer stays
         * valid; the length is atomic. */
        size = atomic_load_explicit(&a->keepalive_len, memory_order_acquire);
        if (size > 0) {
            payload = a->silence;
            if (!a->keepalive_on) {
                atomic_fetch_or_explicit(&a->events, AUDIO_EV_KEEPALIVE_ON,
                                         memory_order_release);
                a->keepalive_on = true;
            }
        }
    }

    if (payload) {
        struct audio_msg h = { .type = AUDIO_MSG_PCM, .size = (uint32_t)size };
        struct iovec iov[2] = {
            { .iov_base = &h, .iov_len = sizeof(h) },
            { .iov_base = (void *)payload, .iov_len = size },
        };
        struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
        /* MSG_DONTWAIT makes this a non-blocking send: it never waits for socket buffer
         * space. It is still a syscall that takes the socket lock and allocates an skb
         * (from a per-CPU cache at this message size), so it is not lock-free in the
         * strictest sense -- but it never sleeps, and it is the same pattern the upstream
         * Anland bridge uses on this path. One whole message per period; drop on EAGAIN. */
        ssize_t n = sendmsg(a->audio_fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                /* transient back-pressure: skip this period */
            } else if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
                defer_detach(a);   /* consumer went away */
            } else {
                /* Keep the errno for the deferred log; the loop thread cannot know it. */
                atomic_store_explicit(&a->last_errno, errno, memory_order_relaxed);
                atomic_fetch_or_explicit(&a->events, AUDIO_EV_SEND_FAIL,
                                         memory_order_release);
            }
        } else if (n != (ssize_t)(sizeof(h) + size)) {
            /* The protocol carries one whole message per AUDIO_MSG_PCM and the socket is a
             * message socket, so a short write means the framing is no longer reliable. */
            atomic_fetch_or_explicit(&a->events, AUDIO_EV_SHORT_WRITE, memory_order_release);
            defer_detach(a);
        }
    }
    pw_stream_queue_buffer(a->capture, b);
}

/* Fill the virtual mic source from the ring buffer (fed by Android). Silence-pads
 * when the ring underruns or while detached, so the source never glitches/vanishes.
 *
 * The mic stream is deliberately created WITHOUT PW_STREAM_FLAG_RT_PROCESS, so this runs
 * on the PipeWire thread loop -- the same thread as on_audio_readable(), which is the
 * only writer of the ring. The ring therefore stays single-threaded exactly as designed
 * and needs no lock. Keep it that way: adding RT_PROCESS here would introduce a data
 * race on ring_head/ring_tail/ring_fill without a lock-free ring rewrite. */
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

/* Apply a consumer-announced format. The virtual device stays continuously online
 * and is hot-plugged (disconnect + reconnect) ONLY when the format actually changes
 * versus what the stream is currently running -- an unchanged announcement (the common
 * case, including every consumer reconnect that re-sends the same format) is a no-op,
 * so the node never churns and plasma-pa keeps resolving the default sink/source.
 *
 * Defaults are role-correct: a field left 0 means "device default", which must equal
 * the value the stream was built with, otherwise the comparison below would treat an
 * unset field as a change and re-plug on every announcement. (One bug was a CAPTURE
 * announce with channels==0 defaulting to 2, never matching the mono source.)
 *
 * Every change is applied IN PLACE -- the node object (anland-speaker / anland-mic) is
 * never destroyed, so WirePlumber/plasma-pa keep their default reference and never log
 * "No object for name anland-speaker". A rate/channels change renegotiates the port
 * format via pw_stream_update_params(); a quantum-only change (AAudio's framesPerBurst
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

/* Bytes of S16 silence the keep-alive path hands the consumer when a cycle produced no
 * PCM at all, published to the realtime callback through keepalive_len.
 *
 * The length is derived from the announced quantum but is deliberately CLAMPED to
 * KEEPALIVE_FRAMES (480 frames ~= 10 ms at 48 kHz): the payload is a small priming
 * block, not a whole negotiated period. So it is not "exactly one AAudio burst" when the
 * consumer asks for a larger quantum -- do not describe it that way. Raising the cap
 * would mean sending proportionally more silence per cycle for no benefit.
 *
 * The result is also bounded by sizeof(a->silence) (channels are already limited by
 * valid_format()), so the process callback can never overrun the buffer.
 * Called on the loop thread; the release store is what makes the payload publication
 * visible to the realtime callback (see also detach_audio_fd_locked()). */
static void update_silence_size(struct anland_audio *a)
{
    uint32_t frames = a->play_quantum ? a->play_quantum : KEEPALIVE_FRAMES;
    if (frames > KEEPALIVE_FRAMES)
        frames = KEEPALIVE_FRAMES;
    size_t bytes = (size_t)frames * a->play_channels * sizeof(int16_t);
    if (bytes > sizeof(a->silence))
        bytes = sizeof(a->silence);
    a->silence_bytes = bytes;
    atomic_store_explicit(&a->keepalive_len, bytes, memory_order_release);
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

    /* A PLAYBACK announcement means the consumer has opened (or is about to open) its
     * playback device: from here on the speaker stream must not be allowed to run dry, or
     * the native stream stops and the PipeWire node stays paused forever. Re-arm the
     * keep-alive length unconditionally -- not only on a format *change* -- because a
     * consumer whose device already matches our defaults (48000/2, the common case)
     * would otherwise take the early return below and never enable it at all. The
     * function is idempotent and publishes length before any consumer of it observes a
     * change, and its release store is what makes the payload visible to the realtime
     * callback. */
    const bool format_changed = (rate != *cur_rate || channels != *cur_channels);
    const bool quantum_changed = (f->quantum != *cur_quantum);

    *cur_rate = rate;
    *cur_channels = channels;
    *cur_quantum = f->quantum;
    if (playback)
        update_silence_size(a);

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

/* Arm the periodic tick. It doubles as the reconnect retry and as the drain point for the
 * realtime callback's edge events, so once it is armed in anland_audio_start() it is
 * always left armed (not stopped when connected) to keep those events flowing. */
static void arm_reconnect(struct anland_audio *a)
{
    struct timespec val = { .tv_sec = RECONNECT_SECS, .tv_nsec = 0 };
    pw_loop_update_timer(pw_thread_loop_get_loop(a->loop), a->reconnect_timer,
                         &val, NULL, false);
}

/* Fatal, non-recoverable error on the core proxy means the sound service connection
 * was lost (e.g. pipewire/wireplumber restarted). Drop the dead core+streams and
 * schedule a rebuild; the audio socket / mic ring are untouched so the consumer side
 * keeps working and resumes the moment PipeWire is back. */
static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    struct anland_audio *a = data;
    (void)seq;
    (void)message;
    if (id == PW_ID_CORE && res == -EPIPE) {
        a->pw_connected = false;
        arm_reconnect(a);   /* teardown + rebuild happens in the timer, not here */
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
 * mic must NOT use it so that its process callback stays on the PipeWire thread loop,
 * i.e. the same thread that fills the mic ring from the socket. `rt_process` selects
 * that explicitly instead of forcing RT semantics on both streams. */
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

/* Tear down the core proxy and both streams, leaving the loop, context, timer, mic
 * ring and audio-socket io source intact. Idempotent. */
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

    /* Re-publish the keep-alive length for the current (possibly default) format. The
     * speaker stream is recreated here, so if this is a rebuild after the sound service
     * went away the RT callback must be armed again from the stored format rather than
     * waiting for the consumer to repeat its announcement. */
    update_silence_size(a);

    /* Own a virtual sink so the container has a real output device instead of only
     * the auto-null "Dummy Output": apps play into this Audio/Sink, WirePlumber makes
     * it the default (high priority beats auto_null), and on_capture_process receives
     * the mixed PCM directly -- no monitor capture, nothing bound to the dummy.
     *
     * The idle properties are what make remote playback work at all. Without them this
     * node is a follower that is only pulled while a driver is running it, so it sits
     * suspended (pw-top: RATE 0/QUANT 0) and every app on the link stays in "paused"
     * forever. always-process keeps the node runnable while unlinked (and implies
     * want-driver); pause-on-idle/suspend-on-idle keep it from being torn down between
     * tracks; session.suspend-timeout-seconds stops WirePlumber's session layer from
     * suspending it after a few idle seconds. Together they let the node reach
     * streaming -- and stay there -- while on_capture_process keeps feeding the
     * consumer. */
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

    /* Speaker: RT process callback. Its callback is lock/alloc/log-free, and it must be
     * fed inside the graph's own cycle or the driver can advance without the consumer
     * ever receiving a period. */
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

/* Loop thread: turn the realtime callback's edge bits into log lines. Kept off the data
 * thread precisely because stdio is not realtime-safe; one pass per periodic tick is
 * plenty to observe silence/streaming transitions. */
static void drain_rt_events(struct anland_audio *a)
{
    uint32_t ev = atomic_exchange_explicit(&a->events, 0, memory_order_acquire);
    if (!ev)
        return;
    if (ev & AUDIO_EV_KEEPALIVE_ON)
        fprintf(stderr, "anland: playback silent, keep-alive started (%zu bytes)\n",
                a->silence_bytes);
    if (ev & AUDIO_EV_STREAMING)
        fprintf(stderr, "anland: playback streaming (real PCM)\n");
    else if (ev & AUDIO_EV_FIRST_PCM)
        fprintf(stderr, "anland: playback streaming (first PCM captured)\n");
    if (ev & AUDIO_EV_SEND_FAIL)
        fprintf(stderr, "anland: audio sendmsg failed: %s\n",
                strerror(atomic_load_explicit(&a->last_errno, memory_order_relaxed)));
    if (ev & AUDIO_EV_SHORT_WRITE)
        fprintf(stderr, "anland: short audio write, transport detached\n");
}

/* Periodic loop-thread tick: also used as the reconnect retry when the sound service is
 * not up yet. Always armed, so the RT edge events above are always drained. */
static void on_reconnect_timer(void *data, uint64_t expirations)
{
    struct anland_audio *a = data;
    (void)expirations;

    drain_rt_events(a);

    if (a->pw_connected)
        return;

    teardown_pw(a);            /* clear any half-built state from a failed attempt */
    if (build_pw(a) == 0) {
        a->pw_connected = true;
    } else {
        teardown_pw(a);
        arm_reconnect(a);      /* sound service still down -- keep retrying */
    }
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
         * socket comes up; on_audio_readable applies them and reconfigures the PipeWire
         * streams, so we don't dictate any format here. The loudspeaker keep-alive only
         * starts once that PLAYBACK announcement has been seen. */
        a->io = pw_loop_add_io(pw_thread_loop_get_loop(a->loop), owned_fd,
                               SPA_IO_IN, true, on_audio_readable, a);
        if (a->io) {
            a->audio_fd = owned_fd;
            atomic_fetch_add_explicit(&a->attach_serial, 1, memory_order_relaxed);
            fprintf(stderr, "anland: audio transport attached\n");
        } else {
            close(owned_fd);
            fprintf(stderr, "anland: failed to register audio fd: %s\n", strerror(errno));
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
    a->notify_read = -1;
    a->notify_write = -1;
    a->play_rate = DEFAULT_RATE;
    a->play_channels = DEFAULT_PLAY_CHANNELS;
    a->cap_rate = DEFAULT_RATE;
    a->cap_channels = DEFAULT_CAP_CHANNELS;
    update_silence_size(a);   /* valid fallback before any consumer announcement */
    a->ring_size = MIC_RING_BYTES;
    a->ring = malloc(a->ring_size);
    if (!a->ring)
        goto fail;

    a->loop = pw_thread_loop_new("anland-audio", NULL);
    if (!a->loop)
        goto fail;

    a->context = pw_context_new(pw_thread_loop_get_loop(a->loop), NULL, 0);
    if (!a->context)
        goto fail;

    a->reconnect_timer = pw_loop_add_timer(pw_thread_loop_get_loop(a->loop),
                                           on_reconnect_timer, a);
    if (!a->reconnect_timer)
        goto fail;

    /* Wake-up pipe for a detach requested by the realtime process callback. Both ends are
     * non-blocking CLCEXEC descriptors owned by this object; the read end is added to the
     * loop as a normal io source, the write end is only ever poked with one byte. */
    {
        int fds[2];
        if (pipe(fds) < 0)
            goto fail;
        for (int i = 0; i < 2; i++) {
            int fl = fcntl(fds[i], F_GETFL, 0);
            if (fl >= 0)
                fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
            int fdfl = fcntl(fds[i], F_GETFD, 0);
            if (fdfl >= 0)
                fcntl(fds[i], F_SETFD, fdfl | FD_CLOEXEC);
        }
        a->notify_read = fds[0];
        a->notify_write = fds[1];
    }
    a->notify = pw_loop_add_io(pw_thread_loop_get_loop(a->loop), a->notify_read,
                               SPA_IO_IN, false, on_notify, a);
    if (!a->notify)
        goto fail;

    if (pw_thread_loop_start(a->loop) < 0)
        goto fail;

    /* First connection attempt under the loop lock. If PipeWire is not up yet, fall
     * back to the reconnect timer instead of failing -- audio will come up when the
     * sound service appears. Either way the engine object exists. */
    pw_thread_loop_lock(a->loop);
    if (build_pw(a) == 0) {
        a->pw_connected = true;
        arm_reconnect(a);   /* keep the periodic tick armed to drain RT edge events */
    } else {
        teardown_pw(a);
        arm_reconnect(a);
    }
    pw_thread_loop_unlock(a->loop);

    g_audio = a;
    return 0;

fail:
    if (a->reconnect_timer)
        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->reconnect_timer);
    if (a->context)
        pw_context_destroy(a->context);
    if (a->loop)
        pw_thread_loop_destroy(a->loop);
    if (a->notify_read >= 0)
        close(a->notify_read);
    if (a->notify_write >= 0)
        close(a->notify_write);
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
     * transport io source (which owns audio_fd and closes it), the loop sources, and
     * only then the context/loop. Nothing may call back into a destroyed object. */
    if (a->loop)
        pw_thread_loop_stop(a->loop);
    teardown_pw(a);
    if (a->io)
        detach_audio_fd_locked(a);
    if (a->reconnect_timer)
        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->reconnect_timer);
    if (a->notify)
        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->notify);
    if (a->notify_read >= 0)
        close(a->notify_read);
    if (a->notify_write >= 0)
        close(a->notify_write);
    if (a->context)
        pw_context_destroy(a->context);
    if (a->loop)
        pw_thread_loop_destroy(a->loop);
    free(a->ring);
    free(a);
    pw_deinit();
}
