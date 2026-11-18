/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* This is an OpenAL backend for Android using the native audio APIs based on OpenSL ES 1.0.1.
 * It is based on source code for the native-audio sample app bundled with NDK.
 */

#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dlfcn.h>

#include "alMain.h"
#include "AL/al.h"
#include "AL/alc.h"

#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>

#include <jni.h>


#define LOG_NDEBUG 0
#define LOG_TAG "OpenAL_SLES"

#if 1
#define LOGV(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#else
#define LOGV(...)
#endif

// for native audio
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "apportable_openal_funcs.h"

/* How long teardown waits for the mixer thread before detaching it and letting
 * teardown continue (ADR 0003, same budget as ADR 0001's AudioTrack path). */
#define OPENSLES_JOIN_TIMEOUT_MS 200

/* Number of mixer threads that have been started and not yet returned. Used to
 * keep the process-lifetime OpenSL objects alive while a thread detached by a
 * timed-out join can still touch them (ADR 0003). */
static volatile int mixer_threads_alive = 0;

#ifdef CLIENTE_OPENAL_STALL_PROBE
#include <sys/system_properties.h>
#include "stall_probe.h"

/* Debug-only mixer stall (ADR 0003).
 *
 * The real wedge is a mixer blocked on a buffer mutex held by the OpenSL
 * callback while its synchronous Enqueue talks to a stalled audio server,
 * which we cannot invoke from outside the app. This switch parks the mixer
 * thread for `ms` while it holds a buffer mutex, producing the same shape: the
 * thread cannot observe threadShouldRun, so an unbounded stop would wait for
 * it. Enable it at runtime with:
 *
 *     adb shell setprop debug.openal.opensles_stall_ms 8000
 *
 * and clear it with `setprop ... 0`.
 */
#define OPENSLES_STALL_PROPERTY "debug.openal.opensles_stall_ms"
#define OPENSLES_STALL_MAX_MS 600000u

static volatile unsigned int debug_opensles_stall_ms = 0;
static volatile int debug_opensles_stall_override = 0;
static volatile int debug_opensles_stall_active = 0;
static volatile int debug_opensles_detach_count = 0;
static volatile unsigned long long debug_opensles_mixed_bytes = 0;

static unsigned int debug_opensles_parse_ms(const char *value)
{
    char *end = NULL;
    unsigned long ms;

    if (value == NULL || value[0] == '\0')
    {
        return 0;
    }
    ms = strtoul(value, &end, 10);
    if (end == value || ms > OPENSLES_STALL_MAX_MS)
    {
        return 0;
    }
    return (unsigned int)ms;
}

static void debug_opensles_stall_refresh_from_property(void)
{
    char value[PROP_VALUE_MAX];

    if (debug_opensles_stall_override)
    {
        return;
    }
    value[0] = '\0';
    if (__system_property_get(OPENSLES_STALL_PROPERTY, value) > 0)
    {
        debug_opensles_stall_ms = debug_opensles_parse_ms(value);
    }
}

void alc_android_test_set_opensles_stall_ms(unsigned int ms)
{
    debug_opensles_stall_ms = (ms <= OPENSLES_STALL_MAX_MS) ? ms : 0;
    debug_opensles_stall_override = 1;
}

int alc_android_test_opensles_stall_active(void)
{
    return debug_opensles_stall_active;
}

int alc_android_test_opensles_detach_count(void)
{
    return debug_opensles_detach_count;
}

int alc_android_test_opensles_live_mixer_threads(void)
{
    return mixer_threads_alive;
}

unsigned long long alc_android_test_opensles_mixed_bytes(void)
{
    return debug_opensles_mixed_bytes;
}

/* Called from the mixer thread with a buffer mutex held. */
static void debug_opensles_stall_in_mixer(void)
{
    unsigned int ms;

    if (!debug_opensles_stall_override)
    {
        debug_opensles_stall_refresh_from_property();
    }
    ms = debug_opensles_stall_ms;
    if (ms == 0)
    {
        return;
    }
    debug_opensles_stall_active = 1;
    usleep((useconds_t)ms * 1000u);
    debug_opensles_stall_active = 0;
}
#else
static void debug_opensles_stall_in_mixer(void)
{
}
#endif

/*
#define MAKE_SYM_POINTER(sym) static typeof(sym) * p##sym = NULL
MAKE_SYM_POINTER(SL_IID_ENGINE);
MAKE_SYM_POINTER(SL_IID_ANDROIDSIMPLEBUFFERQUEUE);
MAKE_SYM_POINTER(SL_IID_PLAY);
MAKE_SYM_POINTER(SL_IID_BUFFERQUEUE);
MAKE_SYM_POINTER(slCreateEngine);
*/

// engine interfaces
static SLObjectItf engineObject = NULL;
static SLEngineItf engineEngine;

// output mix interfaces
static SLObjectItf outputMixObject = NULL;

// JNI stuff so we can get the runtime OS version number
static JavaVM* javaVM = NULL;

static int alc_opensles_get_android_api()
{
    jclass androidVersionClass = NULL;
    jfieldID androidSdkIntField = NULL;
    int androidApiLevel = 0;
    JNIEnv* env = NULL;

    (*javaVM)->GetEnv(javaVM, (void**)&env, JNI_VERSION_1_4);
    androidVersionClass = (*env)->FindClass(env, "android/os/Build$VERSION");
    if (androidVersionClass)
    {
        androidSdkIntField = (*env)->GetStaticFieldID(env, androidVersionClass, "SDK_INT", "I");
        if (androidSdkIntField != NULL)
        {
            androidApiLevel = (int)((*env)->GetStaticIntField(env, androidVersionClass, androidSdkIntField));
        }
        (*env)->DeleteLocalRef(env, androidVersionClass);
    }
    LOGV("API:%d", androidApiLevel);
    return androidApiLevel;
}
static char *androidModel = NULL;
static char *alc_opensles_get_android_model()
{
    if (!androidModel) {
        jclass androidBuildClass = NULL;
        jfieldID androidModelField = NULL;
        jstring androidModelString = NULL;
        int androidApiLevel = 0;
        JNIEnv* env = NULL;

        (*javaVM)->GetEnv(javaVM, (void**)&env, JNI_VERSION_1_4);
        (*env)->PushLocalFrame(env, 5);
        androidBuildClass = (*env)->FindClass(env, "android/os/Build");
        if (androidBuildClass)
        {
            androidModelField = (*env)->GetStaticFieldID(env, androidBuildClass, "MODEL", "Ljava/lang/String;");
            androidModelString = (*env)->GetStaticObjectField(env, androidBuildClass, androidModelField);
            const char *unichars = (*env)->GetStringUTFChars(env, androidModelString, NULL);
            if (!(*env)->ExceptionOccurred(env))
            {
                jsize sz = (*env)->GetStringLength(env, androidModelString);
                androidModel = malloc(sz+1);
                if (androidModel) {
                    strncpy(androidModel, unichars, sz);
                    androidModel[sz] = '\0';
                }
            }
            (*env)->ReleaseStringUTFChars(env, androidModelString, unichars);
        }
        (*env)->PopLocalFrame(env, NULL);
    }
    LOGV("Model:%s", androidModel);
    return androidModel;
}

static long timespecdiff(struct timespec *starttime, struct timespec *finishtime)
{
  long msec;
  msec=(finishtime->tv_sec-starttime->tv_sec)*1000;
  msec+=(finishtime->tv_nsec-starttime->tv_nsec)/1000000;
  return msec;
 }

// Cannot be a constant because we need to tweak differently depending on OS version.
static size_t bufferCount = 8;
static size_t bufferSize = (1024*4);
static size_t defaultBufferSize = (1024*4);
static size_t premixCount = 3;
#define bufferSizeMax (1024*4)

typedef enum {
    OUTPUT_BUFFER_STATE_UNKNOWN,
    OUTPUT_BUFFER_STATE_FREE,
    OUTPUT_BUFFER_STATE_MIXED,
    OUTPUT_BUFFER_STATE_ENQUEUED,
} outputBuffer_state_t;

typedef struct outputBuffer_s {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    outputBuffer_state_t state;
    char buffer[bufferSizeMax];
} outputBuffer_t;

// Will dynamically create the number of buffers (array elements) based on OS version.


typedef struct {
    pthread_t playbackThread;
    char threadShouldRun;
    char threadIsReady;
    char lastBufferEnqueued;
    char lastBufferMixed;

    /* Ownership handshake (ADR 0003). The mixer thread detaches itself on
     * entry, so the stopper never owns the thread handle. A stop waits for
     * `thread_exited` with a bound; on timeout it marks `detached` and the
     * device PlaybackDetached, and from then on the thread owns the playback
     * state until it exits. `detached` also forbids starting a second mixer
     * while the old one can still run. */
    pthread_mutex_t exit_mutex;
    pthread_cond_t  exit_cond;
    int detached;
    int thread_exited;

    outputBuffer_t *outputBuffers;

    // buffer queue player interfaces
    SLObjectItf bqPlayerObject;
    SLPlayItf bqPlayerPlay;
    SLAndroidSimpleBufferQueueItf bqPlayerBufferQueue;
} opesles_data_t;
#define MAX_DEVICES 3
static ALCdevice *deviceList[MAX_DEVICES] = {NULL};
static pthread_mutex_t deviceListMutex = PTHREAD_MUTEX_INITIALIZER;

typedef void (*deviceListFn)(ALCdevice *);

static void devlist_add(ALCdevice *pDevice) {
    int i;
    pthread_mutex_lock(&(deviceListMutex));
    for (i = 0; i < MAX_DEVICES; i++) {
        if (deviceList[i] == pDevice) {
            break;
        } else if (deviceList[i] == NULL) {
            deviceList[i] = pDevice;
            break;
        }
    }
    pthread_mutex_unlock(&(deviceListMutex));
}

static void devlist_remove(ALCdevice *pDevice) {
    int i;
    pthread_mutex_lock(&(deviceListMutex));
    for (i = 0; i < MAX_DEVICES; i++) {
        if (deviceList[i] == pDevice) {
            deviceList[i] = NULL;
        }
    }
    pthread_mutex_unlock(&(deviceListMutex));
}

static void devlist_process(deviceListFn mapFunction) {
    int i;
    pthread_mutex_lock(&(deviceListMutex));
    for (i = 0; i < MAX_DEVICES; i++) {
        if (deviceList[i]) {
            pthread_mutex_unlock(&(deviceListMutex));
            mapFunction(deviceList[i]);
            pthread_mutex_lock(&(deviceListMutex));
        }
    }
    pthread_mutex_unlock(&(deviceListMutex));
}


static void mixer_thread_exit(opesles_data_t *devState)
{
    pthread_mutex_lock(&devState->exit_mutex);
    devState->thread_exited = 1;
    pthread_cond_signal(&devState->exit_cond);
    pthread_mutex_unlock(&devState->exit_mutex);

    mixer_threads_alive--;
    pthread_exit(0);
}

static void *playback_function(void * context) {
    LOGV("playback_function started");
    outputBuffer_t *buffer = NULL;
    SLresult result;
    struct timespec ts;
    assert(NULL != context);
    ALCdevice *pDevice = (ALCdevice *) context;
    opesles_data_t *devState = (opesles_data_t *) pDevice->ExtraData;
    unsigned int bufferIndex = devState->lastBufferMixed;

    ALint frameSize = FrameSizeFromDevFmt(pDevice->FmtChans, pDevice->FmtType);

    // Show a sensible name for the thread in debug tools
    prctl(PR_SET_NAME, (unsigned long)"OpenAL/sl/m", 0, 0, 0);

    /* Detach on entry so the stopper never owns the thread handle. A stop that
     * times out must be able to walk away from this thread; a pthread_join()
     * there would race with the thread exiting (ADR 0003). */
    pthread_detach(pthread_self());

    while (1) {
        if (devState->threadShouldRun == 0) {
            mixer_thread_exit(devState);
        }

        bufferIndex = (++bufferIndex) % bufferCount;
        buffer = &(devState->outputBuffers[bufferIndex]);

        pthread_mutex_lock(&(buffer->mutex));


        while (1) {
            if (devState->threadShouldRun == 0) {
                pthread_mutex_unlock(&(buffer->mutex));
                mixer_thread_exit(devState);
            }

            // This is a little hacky, but here we avoid mixing too much data
            if (buffer->state == OUTPUT_BUFFER_STATE_FREE) {
                int i = (bufferIndex - premixCount) % bufferCount;
                outputBuffer_t *buffer1 = &(devState->outputBuffers[i]);
                if (buffer1->state == OUTPUT_BUFFER_STATE_ENQUEUED ||
                    buffer1->state == OUTPUT_BUFFER_STATE_FREE) {
                    break;
                }
            }

            // No buffer available, wait for a buffer to become available
            // or until playback is stopped/suspended
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 5000000;
            pthread_cond_timedwait(&(buffer->cond), &(buffer->mutex), &ts);
        }
        devState->threadIsReady = 1;

        debug_opensles_stall_in_mixer();

        aluMixData(pDevice, buffer->buffer, bufferSize/frameSize);
#ifdef CLIENTE_OPENAL_STALL_PROBE
        debug_opensles_mixed_bytes += bufferSize;
#endif
        buffer->state = OUTPUT_BUFFER_STATE_MIXED;
        pthread_cond_signal(&(buffer->cond));
        pthread_mutex_unlock(&(buffer->mutex));

        devState->lastBufferMixed = bufferIndex;
    }
}

SLresult alc_opensles_init_extradata(ALCdevice *pDevice)
{
    opesles_data_t *devState = NULL;
    int i;
    devState = malloc(sizeof(opesles_data_t));
    if (!devState) {
        return SL_RESULT_MEMORY_FAILURE;
    }
    bzero(devState, sizeof(opesles_data_t));
    devState->outputBuffers = (outputBuffer_t*) malloc(sizeof(outputBuffer_t)*bufferCount);
    if (!devState->outputBuffers) {
        free(devState);
        return SL_RESULT_MEMORY_FAILURE;
    }
    pDevice->ExtraData = devState;
    bzero(devState->outputBuffers, sizeof(outputBuffer_t)*bufferCount);
    devState->lastBufferEnqueued = -1;
    devState->lastBufferMixed = -1;

    /* Bounded-stop handshake (ADR 0003). The condition variable uses the
     * monotonic clock so a wall-clock change cannot stretch or collapse the
     * join budget. */
    {
        pthread_condattr_t exitAttr;
        pthread_mutex_init(&(devState->exit_mutex), (pthread_mutexattr_t*) NULL);
        pthread_condattr_init(&exitAttr);
        pthread_condattr_setclock(&exitAttr, CLOCK_MONOTONIC);
        pthread_cond_init(&(devState->exit_cond), &exitAttr);
        pthread_condattr_destroy(&exitAttr);
    }

    for (i = 0; i < bufferCount; i++) {
        if (pthread_mutex_init(&(devState->outputBuffers[i].mutex), (pthread_mutexattr_t*) NULL) != 0) {
            LOGV("Error on init of mutex");
            free(devState->outputBuffers);
            free(devState);
            return SL_RESULT_UNKNOWN_ERROR;
        }
        if (pthread_cond_init(&(devState->outputBuffers[i].cond), (pthread_condattr_t*) NULL) != 0) {
            LOGV("Error on init of cond");
            free(devState->outputBuffers);
            free(devState);
            return SL_RESULT_UNKNOWN_ERROR;
        }
        devState->outputBuffers[i].state = OUTPUT_BUFFER_STATE_FREE;
    }
    // For the Android suspend/resume functionaly, keep track of all device contexts
    devlist_add(pDevice);
    return SL_RESULT_SUCCESS;
}

static void start_playback(ALCdevice *pDevice) {
    opesles_data_t *devState = NULL;
	int i;

    if (pDevice->ExtraData == NULL) {
        alc_opensles_init_extradata(pDevice);
        devState = pDevice->ExtraData;
        assert(devState != NULL);
    } else {
        devState = (opesles_data_t *) pDevice->ExtraData;
    }


    if (devState->threadShouldRun == 1) {
        // Gratuitous resume
        return;
    }

    /* A stop that timed out handed the playback state to the detached mixer
     * thread. Do not revive it and do not start a second mixer: two threads
     * would mix into the same output buffers. `threadShouldRun` stays 0 so the
     * old thread exits as soon as it can; audio resumes on the next
     * start_playback() after it has really exited (ADR 0003). */
    pthread_mutex_lock(&(devState->exit_mutex));
    if (devState->detached && !devState->thread_exited) {
        pthread_mutex_unlock(&(devState->exit_mutex));
        LOGV("start_playback: previous mixer thread is still detached; deferring restart");
        return;
    }
    devState->detached = 0;
    devState->thread_exited = 0;
    devState->threadShouldRun = 1;
    /* The device was only marked for deferred free because of the old, now
     * gone mixer thread. */
    pDevice->PlaybackDetached = AL_FALSE;
    pthread_mutex_unlock(&(devState->exit_mutex));
    devState->threadIsReady = 0;

    // start/restart playback thread
    pthread_attr_t playbackThreadAttr;
    pthread_attr_init(&playbackThreadAttr);
    //struct sched_param playbackThreadParam;
    //playbackThreadParam.sched_priority = sched_get_priority_max(SCHED_RR);
    //pthread_attr_setschedpolicy(&playbackThreadAttr, SCHED_RR);
    //pthread_attr_setschedparam(&playbackThreadAttr, &playbackThreadParam);
    mixer_threads_alive++;
    if (pthread_create(&(devState->playbackThread), &playbackThreadAttr, playback_function,  (void *) pDevice) != 0) {
        devState->threadShouldRun = 0;
        mixer_threads_alive--;
        return;
    }

    /* Wait for the new mixer to produce its first buffer, but never without a
     * bound: if the sink is stalled no buffer becomes free and the mixer stays
     * in its buffer wait, and spinning here would hang the pause/resume thread
     * instead (ADR 0003). */
    {
        struct timespec startDeadline;
        clock_gettime(CLOCK_MONOTONIC, &startDeadline);
        startDeadline.tv_nsec += (long)OPENSLES_JOIN_TIMEOUT_MS * 1000000L;
        while (startDeadline.tv_nsec >= 1000000000L)
        {
            startDeadline.tv_sec += 1;
            startDeadline.tv_nsec -= 1000000000L;
        }
        while (devState->threadShouldRun && (0 == devState->threadIsReady))
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > startDeadline.tv_sec ||
                (now.tv_sec == startDeadline.tv_sec && now.tv_nsec >= startDeadline.tv_nsec))
            {
                break;
            }
            sched_yield();
        }
    }
}

static void stop_playback(ALCdevice *pDevice) {
    opesles_data_t *devState = (opesles_data_t *) pDevice->ExtraData;
    if (devState->threadShouldRun == 1) {
      struct timespec deadline;

      devState->threadShouldRun = 0;

      clock_gettime(CLOCK_MONOTONIC, &deadline);
      deadline.tv_nsec += (long)OPENSLES_JOIN_TIMEOUT_MS * 1000000L;
      while (deadline.tv_nsec >= 1000000000L)
      {
          deadline.tv_sec += 1;
          deadline.tv_nsec -= 1000000000L;
      }

      /* Never wait for the mixer thread without a bound: it can be blocked on
       * a buffer mutex held by the OpenSL callback, and that callback can be
       * stuck in a synchronous Enqueue against a stalled audio server. The UI
       * thread must not wait for that (ADR 0003). */
      pthread_mutex_lock(&(devState->exit_mutex));
      while (!devState->thread_exited)
      {
          if (pthread_cond_timedwait(&(devState->exit_cond), &(devState->exit_mutex), &deadline) != 0)
          {
              break;
          }
      }
      if (!devState->thread_exited)
      {
          /* The thread may still be inside aluMixData() or blocked on a buffer
           * mutex and will keep using devState and the device until it exits.
           * Mark the device so alcCloseDevice() leaks it instead of freeing it,
           * and forbid a restart until the thread has really exited. */
          devState->detached = 1;
          pDevice->PlaybackDetached = AL_TRUE;
#ifdef CLIENTE_OPENAL_STALL_PROBE
          debug_opensles_detach_count++;
#endif
          LOGV("stop_playback: mixer thread detached after %d ms", OPENSLES_JOIN_TIMEOUT_MS);
      }
      pthread_mutex_unlock(&(devState->exit_mutex));
    }
    return;
}

// this callback handler is called every time a buffer finishes playing
static void opensles_callback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
    ALCdevice *pDevice = (ALCdevice *) context;
    opesles_data_t *devState = (opesles_data_t *) pDevice->ExtraData;
    unsigned int bufferIndex = devState->lastBufferEnqueued;
    unsigned int i;
    struct timespec ts;
    int rc;
    SLresult result;
    outputBuffer_t *buffer = NULL;

    bufferIndex = (++bufferIndex) % bufferCount;
    buffer = &(devState->outputBuffers[bufferIndex]);

    pthread_mutex_lock(&(buffer->mutex));
    // We will block until 'next' buffer has mixed audio, but first flag oldest equeued buffer as free
    for (i = 1; i <= bufferCount; i++) {
        unsigned int j = (devState->lastBufferEnqueued+i) % bufferCount;
        outputBuffer_t *bufferFree = &(devState->outputBuffers[j]);
        if (bufferFree->state == OUTPUT_BUFFER_STATE_ENQUEUED) {
            bufferFree->state = OUTPUT_BUFFER_STATE_FREE;
            break;
        }
    }
    while (buffer->state != OUTPUT_BUFFER_STATE_MIXED) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000;
        rc = pthread_cond_timedwait(&(buffer->cond), &(buffer->mutex), &ts);
        if (rc != 0) {
            if (devState->threadShouldRun == 0) {
                // we are probably suspended
                pthread_mutex_unlock(&(buffer->mutex));
                return;
            }
        }
    }

    if (devState->bqPlayerBufferQueue) {
        result = (*devState->bqPlayerBufferQueue)->Enqueue(devState->bqPlayerBufferQueue, buffer->buffer, bufferSize);
        if (SL_RESULT_SUCCESS == result) {
            buffer->state = OUTPUT_BUFFER_STATE_ENQUEUED;
            devState->lastBufferEnqueued = bufferIndex;
            pthread_cond_signal(&(buffer->cond));
        } else {
            bufferIndex--;
        }
    }
    pthread_mutex_unlock(&(buffer->mutex));
}


static const ALCchar opensles_device[] = "OpenSL ES";

// Apportable extensions
SLresult alc_opensles_create_native_audio_engine()
{
    if (engineObject)
        return SL_RESULT_SUCCESS;

    SLresult result;

    // create engine
    result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    assert(SL_RESULT_SUCCESS == result);

    // realize the engine
    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);

    // get the engine interface, which is needed in order to create other objects
    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    assert(SL_RESULT_SUCCESS == result);

    // create output mix
    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 0, NULL, NULL);
    assert(SL_RESULT_SUCCESS == result);

    // realize the output mix
    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);

    return result;
}

// Backend functions, in same order as type BackendFuncs
static ALCboolean opensles_open_playback(ALCdevice *pDevice, const ALCchar *deviceName)
{
    LOGV("opensles_open_playback pDevice=%p, deviceName=%s", pDevice, deviceName);

    // Check if probe has linked the opensl symbols

    alc_opensles_probe(DEVICE_PROBE);

    /*
    if (pslCreateEngine == NULL) {
        alc_opensles_probe(DEVICE_PROBE);
        if (pslCreateEngine == NULL) {
            return ALC_FALSE;
        }
    }
    */

    if (pDevice->ExtraData == NULL) {
        alc_opensles_init_extradata(pDevice);
    }

    // create the engine and output mix objects
    alc_opensles_create_native_audio_engine();

    return ALC_TRUE;
}


static void opensles_close_playback(ALCdevice *pDevice)
{
    LOGV("opensles_close_playback pDevice=%p", pDevice);
    opesles_data_t *devState = (opesles_data_t *) pDevice->ExtraData;

    if (devState == NULL) {
        return;
    }

    if (devState->detached) {
        int exited;

        pthread_mutex_lock(&(devState->exit_mutex));
        exited = devState->thread_exited;
        pthread_mutex_unlock(&(devState->exit_mutex));

        if (!exited) {
            /* The mixer thread outlived the bounded stop and may still be
             * inside aluMixData() or blocked on a buffer mutex held by the
             * OpenSL callback. Destroying the player would race with that
             * callback, so leak the player here; alcCloseDevice() honours
             * PlaybackDetached and leaks the device too (ADR 0003). */
            LOGV("opensles_close_playback: mixer thread still detached; leaking player and device");
            devlist_remove(pDevice);
            return;
        }

        /* The thread exited after the stop timed out, so the player and the
         * device can be torn down normally again. */
        pDevice->PlaybackDetached = AL_FALSE;
    }

    // shut down the native audio system

    // destroy buffer queue audio player object, and invalidate all associated interfaces
    if (devState->bqPlayerObject != NULL) {
        (*devState->bqPlayerObject)->Destroy(devState->bqPlayerObject);
        devState->bqPlayerObject = NULL;
        devState->bqPlayerPlay = NULL;
        devState->bqPlayerBufferQueue = NULL;
    }



    devlist_remove(pDevice);
}

static ALCboolean opensles_reset_playback(ALCdevice *pDevice)
{
    if (pDevice == NULL) {
        LOGE("Received a NULL ALCdevice! Returning ALC_FALSE from opensles_reset_playback");
        return ALC_FALSE;
    }
    LOGV("opensles_reset_playback pDevice=%p", pDevice);
    opesles_data_t *devState;
    unsigned bits = BytesFromDevFmt(pDevice->FmtType) * 8;
    unsigned channels = ChannelsFromDevFmt(pDevice->FmtChans);
    unsigned samples = pDevice->UpdateSize;
    unsigned size = samples * channels * bits / 8;
	SLuint32 sampling_rate = pDevice->Frequency * 1000;
	SLresult result;
    LOGV("bits=%u, channels=%u, samples=%u, size=%u, freq=%u", bits, channels, samples, size, pDevice->Frequency);
    if (pDevice->Frequency <= 22050) {
        bufferSize = defaultBufferSize / 2;
    }

    devState = (opesles_data_t *) pDevice->ExtraData;

    // create buffer queue audio player

    // configure audio source
    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
//    SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1,
    SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, 2, sampling_rate,
        SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_LEFT|SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource audioSrc = {&loc_bufq, &format_pcm};

    // configure audio sink
    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};
    SLDataSink audioSnk = {&loc_outmix, NULL};

    // create audio player
    LOGV("create audio player");
    const SLInterfaceID ids[1] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean req[1] = {SL_BOOLEAN_TRUE};
    result = (*engineEngine)->CreateAudioPlayer(engineEngine, &devState->bqPlayerObject, &audioSrc, &audioSnk,
        1, ids, req);
    if ((result != SL_RESULT_SUCCESS) || (devState->bqPlayerObject == NULL)) {
        //RELEASE_LOG("create audio player is null or errored: %lx", result);
        return ALC_FALSE;
    }

    // realize the player
    result = (*devState->bqPlayerObject)->Realize(devState->bqPlayerObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);

    // get the play interface
    result = (*devState->bqPlayerObject)->GetInterface(devState->bqPlayerObject, SL_IID_PLAY, &devState->bqPlayerPlay);
    assert(SL_RESULT_SUCCESS == result);

    // get the buffer queue interface
    result = (*devState->bqPlayerObject)->GetInterface(devState->bqPlayerObject, SL_IID_BUFFERQUEUE,
            &devState->bqPlayerBufferQueue);
    if ((result != SL_RESULT_SUCCESS) || (devState->bqPlayerBufferQueue == NULL)) {
        //RELEASE_LOG("get the buffer queue interface is null or errored: %lx", result);
        return ALC_FALSE;
    }

    // register callback on the buffer queue
    result = (*devState->bqPlayerBufferQueue)->RegisterCallback(devState->bqPlayerBufferQueue, opensles_callback, (void *) pDevice);
    assert(SL_RESULT_SUCCESS == result);

    // playback_lock = createThreadLock();
    start_playback(pDevice);

    // set the player's state to playing
    result = (*devState->bqPlayerPlay)->SetPlayState(devState->bqPlayerPlay, SL_PLAYSTATE_PLAYING);
    assert(SL_RESULT_SUCCESS == result);

    // enqueue the first buffer to kick off the callbacks
    result = (*devState->bqPlayerBufferQueue)->Enqueue(devState->bqPlayerBufferQueue, "\0", 1);
    assert(SL_RESULT_SUCCESS == result);


    SetDefaultWFXChannelOrder(pDevice);
    devlist_add(pDevice);

    return ALC_TRUE;
}


static void opensles_stop_playback(ALCdevice *pDevice)
{
    LOGV("opensles_stop_playback device=%p", pDevice);
    stop_playback(pDevice);
}

static ALCboolean opensles_open_capture(ALCdevice *pDevice, const ALCchar *deviceName)
{
    LOGV("opensles_open_capture  device=%p, deviceName=%s", pDevice, deviceName);
    return ALC_FALSE;
}

static void opensles_close_capture(ALCdevice *pDevice)
{
    LOGV("opensles_closed_capture device=%p", pDevice);
}

static void opensles_start_capture(ALCdevice *pDevice)
{
    LOGV("opensles_start_capture device=%p", pDevice);
}

static void opensles_stop_capture(ALCdevice *pDevice)
{
    LOGV("opensles_stop_capture device=%p", pDevice);
}

static void opensles_capture_samples(ALCdevice *pDevice, ALCvoid *pBuffer, ALCuint lSamples)
{
    LOGV("opensles_capture_samples device=%p, pBuffer=%p, lSamples=%u", pDevice, pBuffer, lSamples);
}

static ALCuint opensles_available_samples(ALCdevice *pDevice)
{
    LOGV("opensles_available_samples device=%p", pDevice);
    return 0;
}

// table of backend function pointers

BackendFuncs opensles_funcs = {
    opensles_open_playback,
    opensles_close_playback,
    opensles_reset_playback,
    opensles_stop_playback,
    opensles_open_capture,
    opensles_close_capture,
    opensles_start_capture,
    opensles_stop_capture,
    opensles_capture_samples,
    opensles_available_samples
};

// global entry points called from XYZZY


static void suspend_device(ALCdevice *pDevice) {
    SLresult result;
    if (pDevice) {
        opesles_data_t *devState = (opesles_data_t *) pDevice->ExtraData;
        if (devState->bqPlayerPlay) {
            result = (*devState->bqPlayerPlay)->SetPlayState(devState->bqPlayerPlay, SL_PLAYSTATE_PAUSED);
            if ((SL_RESULT_SUCCESS == result) && (devState->bqPlayerBufferQueue)) {
                result = (*devState->bqPlayerBufferQueue)->Clear(devState->bqPlayerBufferQueue);
                assert(SL_RESULT_SUCCESS == result);
            }
        }
        stop_playback(pDevice);
    }
}

static void resume_device(ALCdevice *pDevice) {
    SLresult result;
    if (pDevice) {
        opesles_data_t *devState = (opesles_data_t *) pDevice->ExtraData;
        if (devState->bqPlayerPlay) {
            result = (*devState->bqPlayerPlay)->SetPlayState(devState->bqPlayerPlay, SL_PLAYSTATE_PLAYING);
            // Pump some blank data into the buffer to stimulate the callback
            if ((SL_RESULT_SUCCESS == result) && (devState->bqPlayerBufferQueue)) {
                result = (*devState->bqPlayerBufferQueue)->Enqueue(devState->bqPlayerBufferQueue, "\0", 1);
                assert(SL_RESULT_SUCCESS == result);
            }
        }
        start_playback(pDevice);
     }
 }

void alc_opensles_suspend()
{
    devlist_process(&suspend_device);
}

void alc_opensles_resume()
{
    devlist_process(&resume_device);
}

static void alc_opensles_set_java_vm(JavaVM *vm)
{
    // Called once and only once from JNI_OnLoad
    javaVM = vm;
    int i;
    char *android_model;
    char *low_buffer_models[] = {
        "GT-I9300",
        "GT-I9305",
        "SHV-E210",
        "SGH-T999",
        "SGH-I747",
        "SGH-N064",
        "SC-06D",
        "SGH-N035",
        "SC-03E",
        "SCH-R530",
        "SCH-I535",
        "SPH-L710",
        "GT-I9308",
        "SCH-I939",
        "Kindle Fire",
        NULL};

	if(NULL != javaVM)
	{
		int android_os_version = alc_opensles_get_android_api();
		// If running on 4.1 (Jellybean) or later, use 8 buffers to avoid breakup/stuttering.
		if(android_os_version >= 16)
		{
			premixCount = 5;
		}
		// Else, use 4 buffers to reduce latency
		else
		{
			premixCount = 1;
		}
        android_model = alc_opensles_get_android_model();
        for (i = 0; low_buffer_models[i] != NULL; i++) {
            if (strncmp(android_model, low_buffer_models[i], strlen(low_buffer_models[i])) == 0) {
                LOGV("Using less buffering");
                defaultBufferSize = 1024;
                bufferSize = 1024;
                premixCount = 1;
                break;
            }
        }
	}
}

void alc_opensles_init(BackendFuncs *func_list)
{
    LOGV("alc_opensles_init");

    struct stat statinfo;
    if (stat("/system/lib/libOpenSLES.so", &statinfo) != 0) {
        return;
    }

    *func_list = opensles_funcs;

	// We need the JavaVM for JNI so we can detect the OS version number at runtime.
	// This is because we need to use different bufferCount values for Android 4.1 vs. pre-4.1.
	// This must be set at constructor time before JNI_OnLoad is invoked.
	apportableOpenALFuncs.alc_android_set_java_vm = alc_opensles_set_java_vm;
}

void alc_opensles_deinit(void)
{
    LOGV("alc_opensles_deinit");

    /* A thread detached by a timed-out join still owns the device and can be
     * inside aluMixData(); tearing the engine down under it would be a
     * use-after-free. Leave the process-lifetime OpenSL objects alone until
     * process exit (ADR 0003). */
    if (mixer_threads_alive > 0) {
        LOGV("alc_opensles_deinit: %d mixer thread(s) still alive; leaving OpenSL objects", mixer_threads_alive);
        return;
    }

    // destroy output mix object, and invalidate all associated interfaces
    if (outputMixObject != NULL) {
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = NULL;
    }

    // destroy engine object, and invalidate all associated interfaces
    if (engineObject != NULL) {
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
        engineEngine = NULL;
    }
}

void alc_opensles_probe(int type)
{
    char *error;
    struct stat statinfo;
    /*
    if (stat("/system/lib/libOpenSLES.so", &statinfo) != 0) {
        LOGV("alc_opensles_probe OpenSLES support not found.");
        return;
    }
    */

    dlerror(); // Clear dl errors

    /*
    void *dlHandle = dlopen("/system/lib/libOpenSLES.so", RTLD_NOW | RTLD_GLOBAL);
    if (!dlHandle || (error = (typeof(error))dlerror()) != NULL) {
        LOGV("OpenSLES could not be loaded.");
        return;
    }
    */
/*
#define LOAD_SYM_POINTER(sym) \
    p##sym = sym;


    do { \
        p##sym = dlsym(dlHandle, #sym); \
        if((error=(typeof(error))dlerror()) != NULL) { \
            LOGV("alc_opensles_probe could not load %s, error: %s", #sym, error); \
            dlclose(dlHandle); \
            return; \
        } \
    } while(0)


    LOAD_SYM_POINTER(slCreateEngine);
    LOAD_SYM_POINTER(SL_IID_ENGINE);
    LOAD_SYM_POINTER(SL_IID_ANDROIDSIMPLEBUFFERQUEUE);
    LOAD_SYM_POINTER(SL_IID_PLAY);
    LOAD_SYM_POINTER(SL_IID_BUFFERQUEUE);
  */

    apportableOpenALFuncs.alc_android_suspend = alc_opensles_suspend;
    apportableOpenALFuncs.alc_android_resume = alc_opensles_resume;

    switch (type) {
    case DEVICE_PROBE:
        LOGV("alc_opensles_probe DEVICE_PROBE");
        AppendDeviceList(opensles_device);
        break;
    case ALL_DEVICE_PROBE:
        LOGV("alc_opensles_probe ALL_DEVICE_PROBE");
        AppendAllDeviceList(opensles_device);
        break;
    default:
        LOGV("alc_opensles_probe type=%d", type);
        break;
    }
}
