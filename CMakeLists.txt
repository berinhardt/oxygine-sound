cmake_minimum_required (VERSION 3.16)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

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
	# On this platform AL and Ogg arrive as linker libraries rather than as
	# targets of this project; tremor is a target and is linked as one.
	target_link_libraries(oxygine-sound PUBLIC -lopenal tremor -logg)

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
