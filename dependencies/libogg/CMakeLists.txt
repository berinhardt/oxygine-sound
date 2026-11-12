cmake_minimum_required(VERSION 3.16)

# The two translation units Android.mk compiles, and the only ones this build
# needs: the Ogg headers under include/ travel with the target as a usage
# requirement instead of being exported to a parent scope. Android.mk lists
# framing.c first and the archiver keeps that order.
add_library(libogg STATIC
	src/framing.c
	src/bitwise.c)

# ndk-build's module is `libogg` but its archive is libogg.a (the module name
# already carries the prefix, and the make build does not add a second one);
# CMake would name this liblibogg.a. The gate compares archive names.
set_target_properties(libogg PROPERTIES OUTPUT_NAME ogg)

target_include_directories(libogg PUBLIC include)
