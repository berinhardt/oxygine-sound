LOCAL_PATH := $(call my-dir)

# The Android app ships exactly one sound engine: the SDL2 backend in
# src/sound/sdl. The null backend is always linked because it owns
# SoundSystem::instance and is the no-device fallback. The shared OpenAL
# sources in src/sound/oal stay in the tree for the iOS build and for the
# desktop backend that deps/oxygine-sound/CMakeLists.txt builds; they are not
# compiled for Android (sdl-audio#15 retired the vendored Android OpenAL tree).

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
		$(SRC)/null/SoundSystemNull.cpp \
		$(SRC)/null/SoundNull.cpp \
		$(SRC)/null/SoundHandleNull.cpp \
		$(SRC)/SdlDeviceStart.cpp \
		$(SRC)/sdl/Mixer.cpp \
		$(SRC)/sdl/SoundPCM.cpp \
		$(SRC)/sdl/SoundSDL.cpp \
		$(SRC)/sdl/SoundHandleSDL.cpp \
		$(SRC)/sdl/SoundSystemSDL.cpp \
		$(SRC)/sdl/VoiceActivity.cpp


LOCAL_CFLAGS := -DOXYGINE_SOUND=1

OXYGINE_SRC := $(LOCAL_PATH)/../oxygine-framework/oxygine/src

LOCAL_C_INCLUDES := $(OXYGINE_SRC)/ \
					$(LOCAL_PATH)/src \
					$(LOCAL_PATH)/dependencies/tremor/Tremor \
					$(LOCAL_PATH)/dependencies/libogg/include
LOCAL_C_INCLUDES += $(LOCAL_PATH)/../glm/
LOCAL_C_INCLUDES += $(LOCAL_PATH)/../SDL/include

LOCAL_WHOLE_STATIC_LIBRARIES := libogg tremor
# SDL2 itself is linked by the app's LOCAL_SHARED_LIBRARIES := SDL2.
LOCAL_EXPORT_LDLIBS := -ldl -llog

LOCAL_EXPORT_C_INCLUDES += $(LOCAL_PATH)/src
LOCAL_EXPORT_CFLAGS += $(LOCAL_CFLAGS)

include $(BUILD_STATIC_LIBRARY)

$(call import-module, SDL)
$(call import-module, oxygine-sound/dependencies/libogg)
$(call import-module, oxygine-sound/dependencies/tremor)
