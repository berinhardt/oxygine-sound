/**
 * OpenAL cross platform audio library
 * Copyright (C) 2010 by Chris Robinson
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *  Boston, MA  02111-1307, USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <jni.h>
#include <pthread.h>
#include "alMain.h"
#include "AL/al.h"
#include "AL/alc.h"

#include "apportable_openal_funcs.h"

/* Number of mixer threads that have been started and not yet returned. Used to
 * keep the process-lifetime JNI state alive while a thread detached by a
 * timed-out join can still touch it (ticket 04). */
static volatile int mixer_threads_alive = 0;

#ifdef CLIENTE_OPENAL_STALL_PROBE
#include <sys/system_properties.h>
#include "stall_probe.h"

/* Debug-only audio-sink stall (ticket 02, ADR 0001).
 *
 * The real trigger is an audio server that stops draining the track, which we
 * cannot invoke from outside the app. This switch simulates the same sink
 * behaviour from inside the backend: while it is set, a blocking write parks
 * the mixer thread for `debug_stall_ms` (like AudioTrack.write() on a wedged
 * server) and a non-blocking write reports a full sink (returns 0), so the fix
 * in tickets 03/04 has a reproducible red/green signal.
 *
 * Compiled only when CLIENTE_OPENAL_STALL_PROBE is defined; the release build
 * type does not define it. For a debug app build, enable it at runtime with:
 *
 *     adb shell setprop debug.openal.audiotrack_stall_ms 8000
 *
 * and clear it with `setprop ... 0`.
 */
#define STALL_PROPERTY "debug.openal.audiotrack_stall_ms"
#define STALL_OUTSIDE_PROPERTY "debug.openal.audiotrack_stall_outside_ms"
#define STALL_MAX_MS 600000u

static volatile unsigned int debug_stall_ms = 0;
/* Second stall shape (ticket 04): park the mixer thread *outside*
 * audiotrack_write, immediately before the JNI write call. A real wedge inside
 * AudioTrack.write is covered by the first switch; this one exercises the
 * bounded join when the thread is blocked anywhere else, so a timed-out join
 * and the ownership transfer are the paths actually taken. */
static volatile unsigned int debug_stall_outside_ms = 0;
/* Non-zero once the mixer thread has reached either simulated stall. Sticky for
 * the life of the process (reset on playback reset) so a probe can synchronise
 * with the thread. */
static volatile int debug_stall_active = 0;
/* Set when a test harness calls the corresponding setter. The system property
 * must then not override the test's choice (the property is for a debug app
 * build, not for the probe). */
static volatile int debug_stall_override = 0;
static volatile int debug_stall_outside_override = 0;

static unsigned int debug_parse_stall_ms(const char *value)
{
    char *end = NULL;
    unsigned long ms;

    if (value == NULL || value[0] == '\0')
    {
        return 0;
    }
    ms = strtoul(value, &end, 10);
    if (end == value || ms > STALL_MAX_MS)
    {
        return 0;
    }
    return (unsigned int)ms;
}

static void debug_stall_refresh_from_property(void)
{
    char value[PROP_VALUE_MAX];

    if (!debug_stall_override)
    {
        value[0] = '\0';
        if (__system_property_get(STALL_PROPERTY, value) > 0)
        {
            debug_stall_ms = debug_parse_stall_ms(value);
        }
    }

    if (!debug_stall_outside_override)
    {
        value[0] = '\0';
        if (__system_property_get(STALL_OUTSIDE_PROPERTY, value) > 0)
        {
            debug_stall_outside_ms = debug_parse_stall_ms(value);
        }
    }
}

void alc_android_test_set_stall_ms(unsigned int ms)
{
    debug_stall_ms = (ms <= STALL_MAX_MS) ? ms : 0;
    debug_stall_override = 1;
}

void alc_android_test_set_outside_stall_ms(unsigned int ms)
{
    debug_stall_outside_ms = (ms <= STALL_MAX_MS) ? ms : 0;
    debug_stall_outside_override = 1;
}

int alc_android_test_stall_active(void)
{
    return debug_stall_active;
}

/* Ticket 04 test seams: how many joins timed out (the thread was detached and
 * took ownership), and how many mixer threads are still alive. */
static volatile int debug_detach_count = 0;

int alc_android_test_detach_count(void)
{
    return debug_detach_count;
}

int alc_android_test_live_mixer_threads(void)
{
    return mixer_threads_alive;
}
#endif

static const ALCchar android_device[] = "Android Default";

JavaVM* alcGetJavaVM(void);
static JNIEnv* env;

static jclass cAudioTrack = NULL;

static jmethodID mAudioTrack;
static jmethodID mGetMinBufferSize;
static jmethodID mPlay;
static jmethodID mPause;
static jmethodID mStop;
static jmethodID mRelease;
static jmethodID mWrite;
static jmethodID mWriteNonBlocking;

static int suspended = 0;
static int audioTrackPlaying = 0;
static int audioTrackWasPlaying = 0;

typedef struct
{
    pthread_t thread;
    volatile int running;

    /* Snapshot of the ALCdevice fields the mixer thread needs. On a timed-out
     * join the thread outlives alcDestroyContext()/alcCloseDevice(), so after
     * the detach it must not dereference the ALCdevice at all (ADR 0001). */
    ALCdevice *device;
    unsigned int frequency;
    unsigned int frameSize;
    unsigned int bytesPerSecond;
    int sampleRateInHz;
    int channelConfig;
    int audioFormat;
    int numUpdates;

    /* Ownership handshake (ticket 04). When `detached` is set the mixer thread
     * owns this struct (and its AudioTrack/JNI state) until it exits, and
     * android_close_playback() must not free it. */
    pthread_mutex_t lock;
    pthread_cond_t exited;
    int thread_started;
    int detached;
    int thread_exited;
} AndroidData;

#define STREAM_MUSIC 3
#define CHANNEL_CONFIGURATION_MONO 2
#define CHANNEL_CONFIGURATION_STEREO 3
#define ENCODING_PCM_8BIT 3
#define ENCODING_PCM_16BIT 2
#define MODE_STREAM 1
/* android.media.AudioTrack.WRITE_NON_BLOCKING (API 23+). */
#define WRITE_NON_BLOCKING 1

/* How long teardown waits for the mixer thread before detaching it and letting
 * teardown continue (ADR 0001: low hundreds of milliseconds). */
#define AUDIOTRACK_JOIN_TIMEOUT_MS 200

/* The mixer loop's worst-case observation latency is one pacing sleep. It must
 * stay below the join budget, or every normal shutdown would take the detach
 * path. 20 ms gives a 10x margin. */
#define AUDIOTRACK_PACE_MAX_USEC 20000u
#if (AUDIOTRACK_PACE_MAX_USEC / 1000u) >= AUDIOTRACK_JOIN_TIMEOUT_MS
#error "pacing sleep must stay below the audiotrack join budget"
#endif

static AndroidData *android_data_alloc(void)
{
    AndroidData *data = (AndroidData*)calloc(1, sizeof(*data));

    if (data != NULL)
    {
        pthread_condattr_t attr;

        pthread_mutex_init(&data->lock, NULL);
        pthread_condattr_init(&attr);
        /* Use a monotonic clock for the bounded wait so a wall-clock change
         * cannot stretch or collapse the join budget. */
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&data->exited, &attr);
        pthread_condattr_destroy(&attr);
    }
    return data;
}

static void android_data_free(AndroidData *data)
{
    if (data != NULL)
    {
        pthread_cond_destroy(&data->exited);
        pthread_mutex_destroy(&data->lock);
        free(data);
    }
}

/* All mixer writes go through here. With `nonBlocking` set this is the
 * AudioTrack.write(byte[], int, int, WRITE_NON_BLOCKING) overload: it returns
 * as soon as the track reports how much it accepted, so the mixer loop can
 * always re-test `running` and observe shutdown (ticket 03, ADR 0001). A short
 * write means the sink is full; the caller drops the remainder instead of
 * waiting for the sink to drain.
 *
 * The debug stall switch turns this call into the shape of a sink that has
 * stopped accepting data:
 *   - blocking write (nonBlocking == 0): park for the stall window, like
 *     AudioTrack.write() on a wedged audio server;
 *   - non-blocking write (nonBlocking != 0): report a full sink immediately so
 *     the loop can re-test `running` (ticket 03).
 * In a release build neither stall branch exists; the non-blocking overload is
 * used like any other write. */
static jint audiotrack_write(JNIEnv *jniEnv, AndroidData *data, jobject track,
                             jarray buffer, int bufferSizeInBytes,
                             int nonBlocking)
{
    (void)data;
#ifdef CLIENTE_OPENAL_STALL_PROBE
    if (debug_stall_ms > 0)
    {
        debug_stall_active = 1;
        if (nonBlocking)
        {
            return 0;
        }

        usleep(debug_stall_ms * 1000u);

        /* The buffer is intentionally discarded: the sink was not draining. */
        return bufferSizeInBytes;
    }
#endif

    if (nonBlocking)
    {
        return (*jniEnv)->CallNonvirtualIntMethod(jniEnv, track, cAudioTrack,
                                                  mWriteNonBlocking,
                                                  buffer, 0, bufferSizeInBytes,
                                                  WRITE_NON_BLOCKING);
    }

    return (*jniEnv)->CallNonvirtualIntMethod(jniEnv, track, cAudioTrack, mWrite,
                                              buffer, 0, bufferSizeInBytes);
}

/* A non-blocking write cannot pace the mixer any more: when the sink is full
 * the write returns immediately and the loop would spin, mixing buffers as
 * fast as the CPU allows and dropping all of them. Sleep for the play time of
 * the bytes that were not accepted so we keep producing audio at the sink's
 * rate without ever waiting for the sink itself.
 *
 * The sleep is capped at AUDIOTRACK_PACE_MAX_USEC, which also bounds how long
 * the mixer can go without observing `running`; it must stay below the join
 * budget (ticket 04). The ALCdevice fields are read from the AndroidData
 * snapshot because this can run after a timed-out join freed the device. */
static void audiotrack_pace_dropped(const AndroidData *data,
                                    unsigned int unwrittenBytes)
{
    unsigned int usec;

    if (data->bytesPerSecond == 0 || unwrittenBytes == 0)
    {
        return;
    }

    usec = (unsigned int)((unsigned long long)unwrittenBytes * 1000000ull /
                          data->bytesPerSecond);
    if (usec == 0)
    {
        usec = 1;
    }
    if (usec > AUDIOTRACK_PACE_MAX_USEC)
    {
        usec = AUDIOTRACK_PACE_MAX_USEC;
    }
    usleep(usec);
}

static void android_mixer_thread_exit(AndroidData *data)
{
    int detached;

    pthread_mutex_lock(&data->lock);
    data->thread_exited = 1;
    detached = data->detached;
    pthread_cond_signal(&data->exited);
    pthread_mutex_unlock(&data->lock);

    mixer_threads_alive--;

    /* A timed-out join handed ownership to this thread: release the struct
     * ourselves. Otherwise the stopper (which observes thread_exited and
     * detaches) and android_close_playback() still own it. */
    if (detached)
    {
        android_data_free(data);
    }
}

static void* thread_function(void* arg)
{
    AndroidData* data = (AndroidData*)arg;
    ALCdevice* device = data->device;
    JavaVM *javaVM = alcGetJavaVM();
    JNIEnv *threadEnv = NULL;
    jobject track;
    jarray buffer;
    int sampleRateInHz;
    int channelConfig;
    int audioFormat;
    int bufferSizeInBytes;
    int bufferSizeInSamples;

    if ((*javaVM)->AttachCurrentThread(javaVM, &threadEnv, NULL) != JNI_OK ||
        threadEnv == NULL)
    {
        android_mixer_thread_exit(data);
        return NULL;
    }

    (*threadEnv)->PushLocalFrame(threadEnv, 2);

    sampleRateInHz = data->sampleRateInHz;
    channelConfig = data->channelConfig;
    audioFormat = data->audioFormat;

    bufferSizeInBytes = (*threadEnv)->CallStaticIntMethod(threadEnv, cAudioTrack,
        mGetMinBufferSize, sampleRateInHz, channelConfig, audioFormat);

    // Suggestion from Eric Wing <ewmailing@gmail.com>
    /* According to the author Martins Mozelko, I should multiply bufferSizeInBytes to tune performance.
       Say, multiply by 2.
       But making this number smaller seems to reduce latency...
       I have tried dividing by 2, 4, and 8. 8 refuses to play any sound.
       It seems that this just divides out the multiplication of NumUpdates (default=4)
       which returns it to min buffer size.
       bufferSizeInBytes is used in multiple places and
       bufferSizeInSamples is tied directly to bufferSizeInBytes though, so we need to be careful
       about what we want to change.
       I'm assuming Martins is correct and this is the indeed the place we want to change it.
       Dividing out the bufferSizeInSamples separately and skipping the multiply did not work.
       Omitting the multiply and not dividing did work, but the buffers may be unnecessarily large.
      */
    bufferSizeInBytes = bufferSizeInBytes / data->numUpdates;

    bufferSizeInSamples = bufferSizeInBytes / (int)data->frameSize;

    track = (*threadEnv)->NewObject(threadEnv, cAudioTrack, mAudioTrack,
        STREAM_MUSIC, sampleRateInHz, channelConfig, audioFormat, data->numUpdates * bufferSizeInBytes, MODE_STREAM);

    (*threadEnv)->CallNonvirtualVoidMethod(threadEnv, track, cAudioTrack, mPlay);
    audioTrackPlaying = 1;

    buffer = (*threadEnv)->NewByteArray(threadEnv, bufferSizeInBytes);

    while (data->running)
    {
        if (suspended) {
            if (audioTrackPlaying) {
                (*threadEnv)->CallNonvirtualVoidMethod(threadEnv, track, cAudioTrack, mPause);
                audioTrackPlaying = 0;
            }
            usleep(5000);
            continue;
        } else if (!audioTrackPlaying) {
            (*threadEnv)->CallNonvirtualVoidMethod(threadEnv, track, cAudioTrack, mPlay);
            audioTrackPlaying = 1;
        }

        void* pBuffer = (*threadEnv)->GetPrimitiveArrayCritical(threadEnv, buffer, NULL);

        if (pBuffer)
        {
            jint written;

            /* The device is only valid while the joiner is still waiting. On a
             * timed-out join the stopper has returned and may already have
             * destroyed it, so re-test `running` before touching it. */
            if (!data->running)
            {
                (*threadEnv)->ReleasePrimitiveArrayCritical(threadEnv, buffer, pBuffer, 0);
                break;
            }

            aluMixData(device, pBuffer, bufferSizeInSamples);
            (*threadEnv)->ReleasePrimitiveArrayCritical(threadEnv, buffer, pBuffer, 0);

#ifdef CLIENTE_OPENAL_STALL_PROBE
            /* Ticket 04 stall shape: block outside audiotrack_write. The
             * thread is wedged before the JNI call, so the bounded join must
             * time out, detach, and leave the track/JNI state to this thread
             * until it wakes and exits. */
            if (debug_stall_outside_ms > 0)
            {
                debug_stall_active = 1;
                usleep(debug_stall_outside_ms * 1000u);
            }
#endif

            /* Never block on the sink: if it cannot take the whole chunk, the
             * remainder is dropped and the thread re-tests `running` after at
             * most one buffer of pacing. */
            written = audiotrack_write(threadEnv, data, track, buffer,
                                       bufferSizeInBytes, 1);
            if (written < 0)
            {
                written = 0;
            }
            if ((int)written < bufferSizeInBytes)
            {
                audiotrack_pace_dropped(data,
                                        (unsigned int)(bufferSizeInBytes - (int)written));
            }
        }
        else
        {
            AL_PRINT("Failed to get pointer to array bytes");
        }
    }

    (*threadEnv)->CallNonvirtualVoidMethod(threadEnv, track, cAudioTrack, mStop);
    (*threadEnv)->CallNonvirtualVoidMethod(threadEnv, track, cAudioTrack, mRelease);
    audioTrackPlaying = 0;

    (*threadEnv)->PopLocalFrame(threadEnv, NULL);

    (*javaVM)->DetachCurrentThread(javaVM);

    android_mixer_thread_exit(data);
    return NULL;
}

static ALCboolean android_open_playback(ALCdevice *device, const ALCchar *deviceName)
{
    JavaVM *javaVM = alcGetJavaVM();
    (*javaVM)->AttachCurrentThread(javaVM, &env, NULL);
    AndroidData* data;

    if (!cAudioTrack)
    {
        /* Cache AudioTrack class and it's method id's
         * And do this only once!
         */

        cAudioTrack = (*env)->FindClass(env, "android/media/AudioTrack");
        if (!cAudioTrack)
        {
            AL_PRINT("android.media.AudioTrack class is not found. Are you running at least 1.5 version?");
            return ALC_FALSE;
        }

        cAudioTrack = (*env)->NewGlobalRef(env, cAudioTrack);

        mAudioTrack = (*env)->GetMethodID(env, cAudioTrack, "<init>", "(IIIIII)V");
        mGetMinBufferSize = (*env)->GetStaticMethodID(env, cAudioTrack, "getMinBufferSize", "(III)I");
        mPlay = (*env)->GetMethodID(env, cAudioTrack, "play", "()V");
        mPause = (*env)->GetMethodID(env, cAudioTrack, "pause", "()V");
        mStop = (*env)->GetMethodID(env, cAudioTrack, "stop", "()V");
        mRelease = (*env)->GetMethodID(env, cAudioTrack, "release", "()V");
        mWrite = (*env)->GetMethodID(env, cAudioTrack, "write", "([BII)I");
        mWriteNonBlocking = (*env)->GetMethodID(env, cAudioTrack, "write", "([BIII)I");
    }

    if (!deviceName)
    {
        deviceName = android_device;
    }
    else if (strcmp(deviceName, android_device) != 0)
    {
        return ALC_FALSE;
    }

    data = android_data_alloc();
    if (data == NULL)
    {
        return ALC_FALSE;
    }
    device->szDeviceName = strdup(deviceName);
    device->ExtraData = data;
    return ALC_TRUE;
}

static void android_close_playback(ALCdevice *device)
{
    AndroidData* data = (AndroidData*)device->ExtraData;
    int can_free = 1;

    if (data == NULL)
    {
        return;
    }

    /* Never free the struct while the mixer thread can still touch it. The
     * timeout path clears ExtraData and hands ownership to the thread, so a
     * non-NULL thread_started here that has not exited is a programming error;
     * leak rather than risk a use-after-free (ADR 0001, ticket 04). */
    pthread_mutex_lock(&data->lock);
    if (data->thread_started && !data->thread_exited)
    {
        can_free = 0;
    }
    pthread_mutex_unlock(&data->lock);

    device->ExtraData = NULL;
    if (can_free)
    {
        android_data_free(data);
    }
    else
    {
        AL_PRINT("android_close_playback: mixer thread still owns AndroidData");
    }
}

static ALCboolean android_reset_playback(ALCdevice *device)
{
    AndroidData* data = (AndroidData*)device->ExtraData;
    unsigned int numUpdates;

    // if (ChannelsFromDevFmt(device->FmtChans) >= 2)
    // {
    //     device->Format = BytesFromDevFmt(device->FmtType) >= 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_STEREO8;
    // }
    // else
    // {
    //     device->Format = BytesFromDevFmt(device->FmtType) >= 2 ? AL_FORMAT_MONO16 : AL_FORMAT_MONO8;
    // }

    SetDefaultChannelOrder(device);

#ifdef CLIENTE_OPENAL_STALL_PROBE
    debug_stall_refresh_from_property();
    debug_stall_active = 0;
#endif

    /* A timed-out stop handed the old struct to its detached thread and cleared
     * ExtraData; start a fresh one rather than reusing memory the thread owns. */
    if (data == NULL)
    {
        data = android_data_alloc();
        if (data == NULL)
        {
            return ALC_FALSE;
        }
        device->ExtraData = data;
    }

    /* Snapshot everything the mixer thread reads from the device before it can
     * outlive the device (ADR 0001, ticket 04). */
    numUpdates = device->NumUpdates > 0 ? (unsigned int)device->NumUpdates : 1;
    data->device = device;
    data->frequency = (unsigned int)device->Frequency;
    data->frameSize = (unsigned int)FrameSizeFromDevFmt(device->FmtChans, device->FmtType);
    data->bytesPerSecond = data->frequency * data->frameSize;
    data->sampleRateInHz = (int)data->frequency;
    data->channelConfig = ChannelsFromDevFmt(device->FmtChans) == 1
        ? CHANNEL_CONFIGURATION_MONO : CHANNEL_CONFIGURATION_STEREO;
    data->audioFormat = BytesFromDevFmt(device->FmtType) == 1
        ? ENCODING_PCM_8BIT : ENCODING_PCM_16BIT;
    data->numUpdates = (int)numUpdates;

    data->thread_started = 1;
    data->detached = 0;
    data->thread_exited = 0;
    data->running = 1;

    mixer_threads_alive++;
    if (pthread_create(&data->thread, NULL, thread_function, data) != 0)
    {
        data->thread_started = 0;
        data->running = 0;
        mixer_threads_alive--;
        return ALC_FALSE;
    }

    return ALC_TRUE;
}

static void android_stop_playback(ALCdevice *device)
{
    AndroidData* data = (AndroidData*)device->ExtraData;
    int transfer = 0;

    if (data != NULL && data->running)
    {
        struct timespec deadline;
        int rc = 0;

        data->running = 0;

        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_nsec += (long)AUDIOTRACK_JOIN_TIMEOUT_MS * 1000000L;
        while (deadline.tv_nsec >= 1000000000L)
        {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }

        /* Never wait for the mixer thread without a bound: a thread wedged
         * outside audiotrack_write (a stalled JNI call, a wedged sink) must not
         * hold the UI thread in teardown (ADR 0001, ticket 04).
         *
         * bionic has no pthread_timedjoin_np (not in the NDK headers nor the
         * device libc), so this is the equivalent bounded join: the thread
         * signals `exited` on the way out and the stopper waits for it on a
         * monotonic clock. */
        pthread_mutex_lock(&data->lock);
        while (!data->thread_exited)
        {
            rc = pthread_cond_timedwait(&data->exited, &data->lock, &deadline);
            if (rc != 0)
            {
                break;
            }
        }
        if (!data->thread_exited)
        {
            data->detached = 1;
            transfer = 1;
        }
        pthread_mutex_unlock(&data->lock);

        if (transfer)
        {
            /* Ownership of AndroidData, the AudioTrack jobject and the
             * thread's JNI state moves to the mixer thread until it exits.
             * android_close_playback() must not free it. */
            device->ExtraData = NULL;
#ifdef CLIENTE_OPENAL_STALL_PROBE
            debug_detach_count++;
#endif
        }
        pthread_detach(data->thread);
    }
    suspended = 0;
}

static ALCboolean android_open_capture(ALCdevice *pDevice, const ALCchar *deviceName)
{
    (void)pDevice;
    (void)deviceName;
    return ALC_FALSE;
}

static void android_close_capture(ALCdevice *pDevice)
{
    (void)pDevice;
}

static void android_start_capture(ALCdevice *pDevice)
{
    (void)pDevice;
}

static void android_stop_capture(ALCdevice *pDevice)
{
    (void)pDevice;
}

static void android_capture_samples(ALCdevice *pDevice, ALCvoid *pBuffer, ALCuint lSamples)
{
    (void)pDevice;
    (void)pBuffer;
    (void)lSamples;
}

static ALCuint android_available_samples(ALCdevice *pDevice)
{
    (void)pDevice;
    return 0;
}

static const BackendFuncs android_funcs = {
    android_open_playback,
    android_close_playback,
    android_reset_playback,
    android_stop_playback,
    android_open_capture,
    android_close_capture,
    android_start_capture,
    android_stop_capture,
    android_capture_samples,
    android_available_samples
};

static void alc_audiotrack_suspend()
{
    suspended = 1;
    audioTrackWasPlaying = audioTrackPlaying;
}

static void alc_audiotrack_resume()
{
    suspended = 0;
    if (audioTrackWasPlaying)
    {
        while (!audioTrackPlaying)
        {
            sched_yield();
        }
        audioTrackWasPlaying = 0;
    }
}


void alc_audiotrack_init(BackendFuncs *func_list)
{
    *func_list = android_funcs;

    if (apportableOpenALFuncs.alc_android_suspend == NULL
		&& apportableOpenALFuncs.alc_android_set_java_vm == NULL) {
        apportableOpenALFuncs.alc_android_suspend = alc_audiotrack_suspend;
        apportableOpenALFuncs.alc_android_resume = alc_audiotrack_resume;
    }
}

void alc_audiotrack_deinit(void)
{
    /* A thread detached by a timed-out join still owns its AudioTrack and JNI
     * state until it exits. Leave the process-lifetime class ref and the
     * attached env alone rather than pull them from under it (ticket 04). */
    if (mixer_threads_alive > 0)
    {
        return;
    }

    /* release cached AudioTrack class */
    JavaVM *javaVM = alcGetJavaVM();
    if (env != NULL && cAudioTrack != NULL)
    {
        (*env)->DeleteGlobalRef(env, cAudioTrack);
    }
    (*javaVM)->DetachCurrentThread(javaVM);
    cAudioTrack = NULL;
}

void alc_audiotrack_probe(int type)
{
    if (type == DEVICE_PROBE)
    {
        AppendDeviceList(android_device);
    }
    else if (type == ALL_DEVICE_PROBE)
    {
        AppendAllDeviceList(android_device);
    }
}
