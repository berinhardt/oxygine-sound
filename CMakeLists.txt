cmake_minimum_required (VERSION 3.10)
project (OXYGINESOUND)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

set(OXYGINESOUND_DEFINITIONS
	-DOXYGINE_SOUND=1)

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

#if(EMSCRIPTEN)
#	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
#	set(SRC ${SRC}
#		src/sound/emscripten/SoundSystemEmscripten.h
#		src/sound/emscripten/SoundSystemEmscripten.cpp
#		src/sound/emscripten/SoundHandleEmscripten.h
#		src/sound/emscripten/SoundHandleEmscripten.cpp
#		src/sound/emscripten/SoundEmscripten.h
#		src/sound/emscripten/SoundEmscripten.cpp
#
#	)

#else(EMSCRIPTEN)

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

#endif(EMSCRIPTEN)


add_library(oxygine-sound STATIC ${SRC})
if(EMSCRIPTEN)
	# The library itself is linked by name on the consumer's link line, so it
	# does not belong in this list of extra libraries.
	set(OXYGINESOUND_LIBS -lopenal tremor -logg)

else(EMSCRIPTEN)
	if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")

		set(OXYGINESOUND_LIBS oxygine-sound -lopenal tremor -logg)

	else()

		set(LIBOGG libogg_static)

		if (MSVC)
			if(NOT (MSVC_VERSION LESS 1900))
				set(LIBOGG ${LIBOGG}_vs2013)
			endif()
		endif()

		set(OXYGINESOUND_LIBS
			${CMAKE_CURRENT_SOURCE_DIR}/libs/win32/OpenAL32.lib
			${CMAKE_CURRENT_SOURCE_DIR}/libs/win32/${LIBOGG}.lib
			tremor
			oxygine-sound)

	endif()
endif(EMSCRIPTEN)



target_link_libraries(oxygine-sound oxygine-framework)
target_link_libraries(oxygine-sound -lopenal)
if (NOT EMSCRIPTEN)
   target_link_libraries(oxygine-sound -logg)
endif(NOT EMSCRIPTEN)


#target_link_libraries(oxyginemagicparticles)
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
set(OXYGINESOUND_LIBS ${OXYGINESOUND_LIBS} PARENT_SCOPE)
set(OXYGINESOUND_DEFINITIONS ${OXYGINESOUND_DEFINITIONS} PARENT_SCOPE)
