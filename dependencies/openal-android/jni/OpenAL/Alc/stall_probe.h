/*
 * Test seam for the debug-only audio-sink stall switch (ticket 02).
 *
 * The implementation lives in audiotrack.c and is only compiled when
 * CLIENTE_OPENAL_STALL_PROBE is defined. That define is set by the app's
 * *debug* build type and by test/android/stall_probe; release builds do not
 * define it, so neither these symbols nor the stall code exist there.
 *
 * See docs/adr/0001-bounded-sound-teardown.md and
 * test/android/teardown_stall_check.sh.
 */
#ifndef OPENAL_STALL_PROBE_H
#define OPENAL_STALL_PROBE_H

#ifdef CLIENTE_OPENAL_STALL_PROBE

#ifdef __cplusplus
extern "C" {
#endif

/* Make the next mixer iterations behave as if the audio sink stopped
 * accepting data for `ms` milliseconds. 0 disables the simulation. */
void alc_android_test_set_stall_ms(unsigned int ms);

/* Block the mixer thread for `ms` milliseconds *outside* audiotrack_write,
 * immediately before the JNI write call. Exercises the timed-out join and the
 * ownership transfer (ticket 04). 0 disables the simulation. */
void alc_android_test_set_outside_stall_ms(unsigned int ms);

/* Non-zero once the mixer thread has attempted a write against the simulated
 * stalled sink (whether the write blocked or reported a full sink). */
int alc_android_test_stall_active(void);

/* How many stop operations timed out and detached the mixer thread (the
 * ownership-transfer path). */
int alc_android_test_detach_count(void);

/* How many mixer threads are still running or exiting. The probe waits for
 * this to reach 0 after teardown so a detached thread completes its own
 * cleanup before the process ends. */
int alc_android_test_live_mixer_threads(void);

/* Bytes handed to aluMixData() and bytes the AudioTrack sink accepted since
 * playback started. Normal playback must accept everything it mixes, up to
 * the one chunk still pending; if it accepts less, the backend is discarding
 * audio and every sound plays fast. */
unsigned long long alc_android_test_mixed_bytes(void);
unsigned long long alc_android_test_accepted_bytes(void);

/* OpenSL ES backend seams (ADR 0003). The OpenSL mixer is stopped and joined
 * on every background transition, so the bounded join has its own probe
 * scenario. */

/* Park the mixer thread for `ms` milliseconds while it holds a buffer mutex.
 * That is the shape of a mixer blocked behind the OpenSL callback's
 * synchronous Enqueue against a stalled audio server. 0 disables it. */
void alc_android_test_set_opensles_stall_ms(unsigned int ms);

/* Non-zero while the OpenSL mixer is inside the simulated stall. */
int alc_android_test_opensles_stall_active(void);

/* How many OpenSL stops timed out and detached the mixer thread. */
int alc_android_test_opensles_detach_count(void);

/* How many OpenSL mixer threads are still running or exiting. */
int alc_android_test_opensles_live_mixer_threads(void);

/* Bytes of audio handed to aluMixData() by the OpenSL mixer since the process
 * started. A resumed mixer must keep increasing it; a deferred restart (see
 * ADR 0003) intentionally does not. */
unsigned long long alc_android_test_opensles_mixed_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* CLIENTE_OPENAL_STALL_PROBE */
#endif /* OPENAL_STALL_PROBE_H */
