cmake_minimum_required(VERSION 3.16)

# The two translation units this build needs; their order is not load-bearing.
# The Ogg headers under include/ travel with the target as a usage requirement.
add_library(libogg STATIC
	src/framing.c
	src/bitwise.c)

# The target is named libogg, so CMake would call the archive liblibogg.a;
# OUTPUT_NAME keeps the libogg.a name. Nothing links it by file name.
set_target_properties(libogg PROPERTIES OUTPUT_NAME ogg)

target_include_directories(libogg PUBLIC include)
