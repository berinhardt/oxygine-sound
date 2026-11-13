LOCAL_PATH := $(call my-dir)

# OpenAL is compiled from the vendored source in dependencies/openal-android
# (module defined at the end of this file). It used to be a checked-in
# prebuilt binary, which silently ignored source edits; see
# docs/adr/0001-bounded-sound-teardown.md.
OPENAL_ANDROID_MAKEFILE := $(LOCAL_PATH)/dependencies/openal-android/jni/Android.mk

include $(CLEAR_VARS)

LOCAL_MODULE    := oxygine-sound_static
LOCAL_MODULE_FILENAME := liboxygine-sound


SRC := src/sound

LOCAL_SRC_FILES := \
		$(SRC)/MemoryStream.cpp \
		$(SRC)/OggStream.cpp \
		$(SRC)/ResSound.cpp \
		$(SRC)/SoundHandle.cpp \
		$(SRC)/SoundInstance.cpp \
		$(SRC)/SoundPlayer.cpp \
		$(SRC)/SoundSystem.cpp \
		$(SRC)/WavStream.cpp \
		$(SRC)/oal/SoundHandleOAL.cpp \
		$(SRC)/oal/SoundOAL.cpp \
		$(SRC)/oal/SoundSystemOAL.cpp \
		$(SRC)/oal/StaticSoundHandleOAL.cpp \
		$(SRC)/oal/StaticStreamOAL.cpp \
		$(SRC)/oal/StreamingSoundHandleOAL.cpp \
		$(SRC)/null/SoundSystemNull.cpp \
		$(SRC)/null/SoundNull.cpp \
		$(SRC)/null/SoundHandleNull.cpp


LOCAL_CFLAGS := -DOXYGINE_SOUND=1

OXYGINE_SRC := $(LOCAL_PATH)/../oxygine-framework/oxygine/src

LOCAL_C_INCLUDES := $(OXYGINE_SRC)/ \
					$(LOCAL_PATH)/src \
					$(LOCAL_PATH)/dependencies/openal-android/jni/OpenAL/include \
					$(LOCAL_PATH)/dependencies/tremor/Tremor \
					$(LOCAL_PATH)/dependencies/libogg/include
LOCAL_C_INCLUDES += $(LOCAL_PATH)/../glm/

LOCAL_EXPORT_C_INCLUDES += $(LOCAL_PATH)/src
LOCAL_EXPORT_CFLAGS += $(LOCAL_CFLAGS)
LOCAL_WHOLE_STATIC_LIBRARIES := libogg openal tremor
LOCAL_EXPORT_LDLIBS     := -lOpenSLES -ldl -llog

include $(BUILD_STATIC_LIBRARY)

$(call import-module, SDL)
$(call import-module, oxygine-sound/dependencies/libogg)
$(call import-module, oxygine-sound/dependencies/tremor)

# Build `openal` from the vendored source. Keep this last: the included
# makefile sets its own LOCAL_PATH and module state.
include $(OPENAL_ANDROID_MAKEFILE)