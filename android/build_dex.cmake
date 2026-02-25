# build_dex.cmake - Find all .class files and run d8 to produce classes.dex
# Usage: cmake -DD8=<d8_path> -DANDROID_JAR=<jar> -DCLASS_DIR=<dir> -P build_dex.cmake
#
# Globs all .class files under CLASS_DIR, including compiler-generated inner
# classes (e.g. Foo$1.class) that aren't known to CMake at configure time.

if(NOT D8 OR NOT ANDROID_JAR OR NOT CLASS_DIR)
	message(FATAL_ERROR "D8, ANDROID_JAR, and CLASS_DIR are all required")
endif()

file(GLOB_RECURSE CLASS_FILES "${CLASS_DIR}/*.class")
if(NOT CLASS_FILES)
	message(FATAL_ERROR "No .class files found in ${CLASS_DIR}")
endif()

execute_process(
	COMMAND ${D8} --lib ${ANDROID_JAR} --release --output ${CLASS_DIR} ${CLASS_FILES}
	RESULT_VARIABLE D8_RESULT
)
if(NOT D8_RESULT EQUAL 0)
	message(FATAL_ERROR "d8 failed with exit code ${D8_RESULT}")
endif()
