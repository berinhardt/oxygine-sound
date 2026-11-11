cmake_minimum_required (VERSION 3.16)

# Android keeps clang's default C++ dialect: the ndk-build baseline compiled
# this module at that default, and a byte comparison of the two builds needs
# the same codegen. The Emscripten and native graphs keep the C++11 pin.
if (NOT ANDROID)
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
endif()

set(SRC
	src/sound/ResSound.cpp
	src/sound/ResSound.h
	src/sound/Sound.h
	src/sound/SoundInstance.cpp
	src/sound/SoundInstance.h
	src/sound/SoundPlayer.cpp
	src/sound/SoundPlayer.h
	src/sound/SoundSystem.cpp
	src/sound/SoundSystem.h

	src/sound/null/SoundSystemNull.cpp
	src/sound/null/SoundSystemNull.h
	src/sound/null/SoundNull.cpp
	src/sound/null/SoundNull.h
	src/sound/null/SoundHandleNull.cpp
	src/sound/null/SoundHandleNull.h
	)

# src/sound/emscripten/ holds an alternative emscripten backend --
# SoundEmscripten, SoundHandleEmscripten, SoundSystemEmscripten and their
# headers -- that nothing in this file compiles. The Ogg/OAL list below is
# built on every platform, emscripten included; those sources are named here
# only so the unused backend stays discoverable.
add_subdirectory(dependencies/tremor tremor)

	set (SRC ${SRC}
		src/sound/OggStream.cpp
		src/sound/OggStream.h
		src/sound/MemoryStream.cpp
		src/sound/MemoryStream.h
		src/sound/WavStream.cpp
		src/sound/WavStream.h

		src/sound/oal/SoundHandleOAL.cpp
		src/sound/oal/SoundHandleOAL.h
		src/sound/oal/SoundOAL.cpp
		src/sound/oal/SoundOAL.h
		src/sound/oal/SoundSystemOAL.cpp
		src/sound/oal/SoundSystemOAL.h
		src/sound/oal/StreamingSoundHandleOAL.cpp
		src/sound/oal/StreamingSoundHandleOAL.h
		src/sound/oal/StaticSoundHandleOAL.cpp
		src/sound/oal/StaticSoundHandleOAL.h
		src/sound/oal/StaticStreamOAL.cpp
		src/sound/oal/StaticStreamOAL.h
		)

add_library(oxygine-sound STATIC ${SRC})
if(EMSCRIPTEN)
	# On this platform AL still arrives as a plain -l name rather than as a
	# target of this project, and tremor is a target and is linked as one. Ogg
	# is neither any more: it is requested as emcc's ogg port, which supplies
	# both its headers and libogg.a. The port is asked for on both lines
	# because the two lines do different work -- emcc installs a port's headers
	# from the compile path, and src/sound/OggStream.cpp (in the SRC list
	# above) includes ogg/ogg.h on this platform too, so the compile copy is
	# what puts that header in the sysroot, while the link copy is what adds
	# libogg.a to the link.
	target_link_libraries(oxygine-sound PUBLIC -lopenal tremor)
	target_compile_options(oxygine-sound PRIVATE -sUSE_OGG=1)
	target_link_options(oxygine-sound PUBLIC -sUSE_OGG=1)

elseif(ANDROID)
	# Android has neither library available as a plain name. OpenAL is a
	# per-ABI prebuilt static archive -- the exact one Android.mk links -- and it
	# is not in the tree for every ABI, so a missing archive has to fail here
	# rather than at link time. Ogg is built from source here, from the same two
	# files Android.mk compiles; tremor is built above on every platform.
	add_subdirectory(dependencies/libogg libogg)

	set(ANDROID_OPENAL_LIB
		${CMAKE_CURRENT_SOURCE_DIR}/dependencies/openal-android/obj/local/${ANDROID_ABI}/libopenal.a)
	if(NOT EXISTS ${ANDROID_OPENAL_LIB})
		message(FATAL_ERROR
			"Missing prebuilt OpenAL for ${ANDROID_ABI}: ${ANDROID_OPENAL_LIB}")
	endif()
	add_library(openal STATIC IMPORTED)
	set_target_properties(openal PROPERTIES IMPORTED_LOCATION ${ANDROID_OPENAL_LIB})

	# src/sound/OggStream.cpp includes ogg/ogg.h and the oal sources include
	# AL/al.h, so both directories have to be on the include path.
	target_include_directories(oxygine-sound PRIVATE
		dependencies/openal-android/jni/OpenAL/include
		dependencies/libogg/include)

	# Android.mk wrapped openal, libogg and tremor in
	# LOCAL_WHOLE_STATIC_LIBRARIES, and that is still required: OpenAL
	# registers its ring-buffer helpers (CreateRingBuffer, DestroyRingBuffer,
	# RingBufferSize, WriteRingBuffer, ReadRingBuffer) through internal
	# function pointers, so no object of theirs is referenced directly and a
	# plain link drops them.
	#
	# The bracketing is asked for through the linker's push/pop state rather
	# than "-Wl,--whole-archive"/"-Wl,--no-whole-archive": CMake deduplicates
	# identical link items, and the plain pair is already present in
	# oxygine-framework's interface, so the two pairs collapse into a single
	# one placed after libSDL2.so where it wraps nothing. Distinct spellings
	# keep each pair next to the libraries it applies to; this is the same
	# spelling CMake's own WHOLE_ARCHIVE link feature emits.
	target_link_libraries(oxygine-sound PUBLIC
		"-Wl,--push-state,--whole-archive" openal tremor libogg
		"-Wl,--pop-state"
		OpenSLES dl log)

else(EMSCRIPTEN)
	if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")

		target_link_libraries(oxygine-sound PUBLIC -lopenal tremor -logg)

	else()

		set(LIBOGG libogg_static)

		if (MSVC)
			if(NOT (MSVC_VERSION LESS 1900))
				set(LIBOGG ${LIBOGG}_vs2013)
			endif()
		endif()

		target_link_libraries(oxygine-sound PUBLIC
			${CMAKE_CURRENT_SOURCE_DIR}/libs/win32/OpenAL32.lib
			${CMAKE_CURRENT_SOURCE_DIR}/libs/win32/${LIBOGG}.lib
			tremor
			-lopenal
			-logg)

	endif()
endif(EMSCRIPTEN)



# These used to be plain-signature calls, which CMake treats as PUBLIC. The
# PUBLIC keyword says the same thing and is required here because a target
# linked with the keyword signature cannot also be linked with the plain one.
target_link_libraries(oxygine-sound PUBLIC oxygine-framework)


# The only directory a consumer ever saw: it now travels with the library as a
# usage requirement instead of being copied into the parent scope as a string.
target_include_directories(oxygine-sound PUBLIC src)
# Header directories the library's own sources compile against. The directory-scope
# call used to apply them to every target in this directory; they are
# implementation details of oxygine-sound, so they stay PRIVATE.
target_include_directories(oxygine-sound PRIVATE
	dependencies/tremor/Tremor
	dependencies/libogg/include)
if(NOT EMSCRIPTEN)
	target_include_directories(oxygine-sound PRIVATE dependencies/openal/include)
endif(NOT EMSCRIPTEN)
