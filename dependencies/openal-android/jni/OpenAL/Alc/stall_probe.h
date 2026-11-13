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

/* Non-zero once the mixer thread has attempted a write against the simulated
 * stalled sink (whether the write blocked or reported a full sink). */
int alc_android_test_stall_active(void);

#ifdef __cplusplus
}
#endif

#endif /* CLIENTE_OPENAL_STALL_PROBE */
#endif /* OPENAL_STALL_PROBE_H */
