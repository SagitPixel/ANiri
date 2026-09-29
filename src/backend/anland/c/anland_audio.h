#ifndef ANLAND_AUDIO_H
#define ANLAND_AUDIO_H

/*
 * Producer-side audio engine.
 *
 * Owns a persistent PipeWire thread-loop with two streams that live for the whole
 * KWin session, independent of whether a consumer is connected:
 *
 *   - an Audio/Sink ("anland-speaker") whose process callback writes desktop playback
 *     PCM to the audio socket (heard on the Android speaker);
 *   - a virtual Audio/Source ("anland-mic") fed from microphone PCM read from the
 *     audio socket (so Linux apps can record the Android mic).
 *
 * The speaker sink is kept permanently runnable and unsuspendable (node.always-process,
 * pause-on-idle/suspend-on-idle off, no session suspend timeout). Android's AAudio
 * playback stream only advances while PCM keeps arriving, so a process cycle with no
 * captured PCM sends one short period of digital silence in the negotiated format
 * instead of nothing; that is what keeps the native stream, and therefore the PipeWire
 * link, from settling into "paused" forever. Real PCM always displaces the silence.
 *
 * The streams are NEVER torn down on consumer disconnect: while detached the capture
 * stream simply drops its PCM and the source feeds silence, so PipeWire (and every
 * recording app) never sees the device disappear. Only the socket fd is hot-swapped:
 * anland_audio_set_fd(fd) on (re)connect, anland_audio_set_fd(-1) on fallback. The fd
 * is borrowed from display_producer; the engine owns a CLOEXEC duplicate while attached.
 * A fresh attachment re-arms the keep-alive and waits for the consumer's format
 * announcements, so disconnect/reconnect needs no compositor restart.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Create the thread-loop and both streams. Returns 0 on success, -1 on failure.
 * Idempotent: a second call while already started is a no-op returning 0. */
int  anland_audio_start(void);

/* Stop and destroy the engine. Safe to call when not started. */
void anland_audio_stop(void);

/* Point the engine at the current audio socket, or -1 to detach (drop/silence).
 * Re-pointing resets the mic buffer and re-announces the playback format. */
void anland_audio_set_fd(int audio_fd);

#ifdef __cplusplus
}
#endif

#endif
