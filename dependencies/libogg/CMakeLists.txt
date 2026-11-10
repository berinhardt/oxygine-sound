cmake_minimum_required(VERSION 3.16)

# The two translation units Android.mk compiles, and the only ones this build
# needs: the Ogg headers under include/ travel with the target as a usage
# requirement instead of being exported to a parent scope.
add_library(libogg STATIC
	src/bitwise.c
	src/framing.c)

target_include_directories(libogg PUBLIC include)
