cmake_minimum_required(VERSION 3.16)

# The two translation units Android.mk compiles, and the only ones this build
# needs: the Ogg headers under include/ travel with the target as a usage
# requirement instead of being exported to a parent scope. Android.mk lists
# framing.c first and the archiver keeps that order.
add_library(libogg STATIC
	src/framing.c
	src/bitwise.c)

target_include_directories(libogg PUBLIC include)
