# Android APK Build for sk_app
# Provides add_apk() function to build APK files from native libraries.
# This module is standalone and can be used independently of sk_app.
#
# Functions:
#   add_apk()              - Create APK build target from a shared library
#   apk_add_java_sources() - Add Java source files to an APK target

###############################################################################
## Include guard and directory capture
###############################################################################

if(_ANDROID_APK_INCLUDED)
	return()
endif()
set(_ANDROID_APK_INCLUDED TRUE)

# Capture the directory at include-time for use in functions
set(_SK_APP_ANDROID_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "sk_app android directory")

###############################################################################
## Find SDK, NDK, and JDK paths
###############################################################################

# Find the Android SDK
if(NOT DEFINED ANDROID_SDK_ROOT)
	if(DEFINED ENV{ANDROID_HOME})
		set(ANDROID_SDK_ROOT $ENV{ANDROID_HOME})
	elseif(DEFINED ENV{ANDROID_SDK_ROOT})
		set(ANDROID_SDK_ROOT $ENV{ANDROID_SDK_ROOT})
	elseif(DEFINED ANDROID_HOME)
		set(ANDROID_SDK_ROOT ${ANDROID_HOME})
	elseif(DEFINED CMAKE_ANDROID_NDK)
		# NDK is typically at $SDK/ndk/<version>, derive SDK from it
		get_filename_component(_ndk_parent "${CMAKE_ANDROID_NDK}" DIRECTORY)
		get_filename_component(_sdk_candidate "${_ndk_parent}" DIRECTORY)
		if(EXISTS "${_sdk_candidate}/platform-tools")
			set(ANDROID_SDK_ROOT "${_sdk_candidate}")
		endif()
	endif()
endif()
if (NOT EXISTS "${ANDROID_SDK_ROOT}/platform-tools")
	message(FATAL_ERROR "Android SDK not found. Set ANDROID_HOME or ANDROID_SDK_ROOT, or ensure CMAKE_ANDROID_NDK points to an NDK inside the SDK.")
endif()
# Cache SDK root for use in deferred functions
set(ANDROID_SDK_ROOT "${ANDROID_SDK_ROOT}" CACHE INTERNAL "Android SDK root")

# Find a build-tools folder in the Android SDK that matches our CMAKE_SYSTEM_VERSION
if(ANDROID_BUILD_TOOLS_VERSION AND EXISTS "${ANDROID_SDK_ROOT}/build-tools/${ANDROID_BUILD_TOOLS_VERSION}")
	set(ANDROID_BUILD_TOOLS_PATH "${ANDROID_SDK_ROOT}/build-tools/${ANDROID_BUILD_TOOLS_VERSION}")
else()
	file(GLOB BUILD_TOOLS_VERSIONS LIST_DIRECTORIES true "${ANDROID_SDK_ROOT}/build-tools/*")
	foreach(VERSION_DIR ${BUILD_TOOLS_VERSIONS})
		get_filename_component(VERSION_NAME ${VERSION_DIR} NAME)
		if(VERSION_NAME MATCHES "${CMAKE_SYSTEM_VERSION}\.")
			set(ANDROID_BUILD_TOOLS_PATH "${VERSION_DIR}")
			break()
		endif()
	endforeach()
endif()
if(NOT EXISTS "${ANDROID_BUILD_TOOLS_PATH}")
	message(STATUS "Can't find build-tools matching ANDROID_BUILD_TOOLS_VERSION ${ANDROID_BUILD_TOOLS_VERSION} or CMAKE_SYSTEM_VERSION ${CMAKE_SYSTEM_VERSION}")
endif()

# Find JDK bin folder
if(NOT DEFINED JAVA_HOME)
	if(DEFINED ENV{JAVA_HOME})
		set(JAVA_HOME $ENV{JAVA_HOME})
	endif()
endif()
if (DEFINED JAVA_HOME)
	cmake_path(APPEND JAVA_HOME_BIN ${JAVA_HOME} "bin")
endif()

message(STATUS "APK build var - ANDROID_SDK_ROOT         : ${ANDROID_SDK_ROOT}")
message(STATUS "APK build var - CMAKE_ANDROID_NDK        : ${CMAKE_ANDROID_NDK}")
message(STATUS "APK build var - ANDROID_BUILD_TOOLS_PATH : ${ANDROID_BUILD_TOOLS_PATH}")
message(STATUS "APK build var - JAVA_HOME_BIN            : ${JAVA_HOME_BIN}")
message(STATUS "APK build var - CMAKE_ANDROID_ARCH_ABI   : ${CMAKE_ANDROID_ARCH_ABI}")
message(STATUS "APK build var - CMAKE_SYSTEM_VERSION     : ${CMAKE_SYSTEM_VERSION}")

###############################################################################
## Build tools
###############################################################################

set(AAPT2    "${ANDROID_BUILD_TOOLS_PATH}/aapt2" CACHE INTERNAL "Android AAPT2 tool")
set(AAPT     "${ANDROID_BUILD_TOOLS_PATH}/aapt" CACHE INTERNAL "Android AAPT tool")
set(ZIPALIGN "${ANDROID_BUILD_TOOLS_PATH}/zipalign" CACHE INTERNAL "Android zipalign tool")
set(APKSIGN  "${ANDROID_BUILD_TOOLS_PATH}/apksigner" CACHE INTERNAL "Android apksigner tool")
set(D8       "${ANDROID_BUILD_TOOLS_PATH}/d8" CACHE INTERNAL "Android D8 tool")
find_program(JAVAC   NAMES javac   REQUIRED PATHS ${JAVA_HOME_BIN})
find_program(KEYTOOL NAMES keytool REQUIRED PATHS ${JAVA_HOME_BIN})

###############################################################################
## Keystore for signing APKs (default debug keystore)
###############################################################################

# Set default keystore variables
set(DEFAULT_KEYSTORE       "${CMAKE_CURRENT_LIST_DIR}/debug.keystore")
set(DEFAULT_KEYSTORE_ALIAS "androiddebugkey")
set(DEFAULT_KEYSTORE_PASS  "android")
set(DEFAULT_KEY_ALIAS_PASS "android")

# Check if keystore variables are provided, otherwise use defaults
set(KEYSTORE       "${DEFAULT_KEYSTORE}"       CACHE STRING "Path to the keystore")
set(KEY_ALIAS      "${DEFAULT_KEYSTORE_ALIAS}" CACHE STRING "Alias for the key")
set(KEYSTORE_PASS  "${DEFAULT_KEYSTORE_PASS}"  CACHE STRING "Password for the keystore")
set(KEY_ALIAS_PASS "${DEFAULT_KEY_ALIAS_PASS}" CACHE STRING "Password for the key")

if(NOT EXISTS "${KEYSTORE}")
	message(STATUS "APK keystore not found, generating new keystore...")
	execute_process(COMMAND ${KEYTOOL}
		-genkeypair -v
		-keyalg RSA -keysize 2048 -validity 10000
		-keystore "${KEYSTORE}" -alias "${KEY_ALIAS}"
		-storepass "${KEYSTORE_PASS}" -keypass "${KEY_ALIAS_PASS}"
		-dname "CN=Android Debug,O=Android,C=US"
		RESULT_VARIABLE KEYTOOL_RESULT)
	if(NOT KEYTOOL_RESULT EQUAL "0")
		message(FATAL_ERROR "Failed to create keystore")
	endif()
endif()

message(STATUS "APK using keystore: ${KEYSTORE} with alias ${KEY_ALIAS}")

###############################################################################
## Version information (used by APKs)
###############################################################################

set(APK_VERSION_NAME "${CMAKE_PROJECT_VERSION}" CACHE INTERNAL "APK version name")
math(EXPR APK_VERSION_CODE "${PROJECT_VERSION_MAJOR} * 10000 + ${PROJECT_VERSION_MINOR} * 100 + ${PROJECT_VERSION_PATCH}")
set(APK_VERSION_CODE "${APK_VERSION_CODE}" CACHE INTERNAL "APK version code")

###############################################################################
## apk_add_java_sources: Add Java source files to an APK target
###############################################################################
#
# Usage:
#   apk_add_java_sources(<target_name> <source_dir>
#       <file1.java> [file2.java] ...
#   )
#
# The Java files should be specified with their package-relative paths,
# e.g., "net/stereokit/sk_app/SkAppActivity.java"
#
# Java sources are automatically compiled at the end of directory processing
# via cmake_language(DEFER).
function(apk_add_java_sources APK_TARGET JAVA_SOURCE_DIR)
	if(NOT TARGET ${APK_TARGET}-apk)
		message(FATAL_ERROR "apk_add_java_sources: ${APK_TARGET}-apk target not found. Call add_apk() first.")
	endif()

	# Get existing sources and append new ones
	# Format: "source_dir:relative_path" for each source
	get_target_property(EXISTING_SOURCES ${APK_TARGET}-apk APK_JAVA_SOURCES)
	if(NOT EXISTING_SOURCES)
		set(EXISTING_SOURCES "")
	endif()

	foreach(JAVA_REL_PATH ${ARGN})
		list(APPEND EXISTING_SOURCES "${JAVA_SOURCE_DIR}:${JAVA_REL_PATH}")
	endforeach()

	set_target_properties(${APK_TARGET}-apk PROPERTIES APK_JAVA_SOURCES "${EXISTING_SOURCES}")
endfunction()

###############################################################################
## Internal: Build classes.dex from registered Java sources (or Empty.class fallback)
##
## Called automatically at the end of directory processing via cmake_language(DEFER).
###############################################################################
function(_apk_build_dex APK_TARGET)
	get_target_property(APK_BUILD_DIR ${APK_TARGET}-apk APK_BUILD_DIR)
	get_target_property(APK_MIN_SDK ${APK_TARGET}-apk APK_MIN_SDK)
	get_target_property(JAVA_SOURCES ${APK_TARGET}-apk APK_JAVA_SOURCES)

	# Find Android SDK jar for compilation
	set(ANDROID_JAR "${ANDROID_SDK_ROOT}/platforms/android-${APK_MIN_SDK}/android.jar")
	if(NOT EXISTS "${ANDROID_JAR}")
		set(ANDROID_JAR "${ANDROID_SDK_ROOT}/platforms/android-32/android.jar")
	endif()

	set(DEX_INPUTS "")
	set(DEX_DEPENDS "")

	# Process Java sources if any, otherwise use Empty.class placeholder
	if(JAVA_SOURCES)
		set(JAVA_SRC_FILES "")
		set(JAVA_CLASS_FILES "")

		foreach(SOURCE_ENTRY ${JAVA_SOURCES})
			# Parse "source_dir:relative_path" format
			string(REPLACE ":" ";" SOURCE_PARTS "${SOURCE_ENTRY}")
			list(GET SOURCE_PARTS 0 SOURCE_DIR)
			list(GET SOURCE_PARTS 1 REL_PATH)

			# Copy source to build directory
			get_filename_component(REL_DIR "${REL_PATH}" DIRECTORY)
			file(MAKE_DIRECTORY "${APK_BUILD_DIR}/src/${REL_DIR}")
			configure_file(
				"${SOURCE_DIR}/${REL_PATH}"
				"${APK_BUILD_DIR}/src/${REL_PATH}"
				COPYONLY
			)

			list(APPEND JAVA_SRC_FILES "${APK_BUILD_DIR}/src/${REL_PATH}")

			# Determine class file output path
			string(REPLACE ".java" ".class" CLASS_PATH "${REL_PATH}")
			list(APPEND JAVA_CLASS_FILES "${APK_BUILD_DIR}/obj/${CLASS_PATH}")
		endforeach()

		# Compile all additional Java sources
		add_custom_command(
			OUTPUT ${JAVA_CLASS_FILES}
			DEPENDS ${JAVA_SRC_FILES}
			COMMAND ${JAVAC}
				-source 1.8 -target 1.8
				-bootclasspath ${ANDROID_JAR}
				-d ${APK_BUILD_DIR}/obj
				-sourcepath ${APK_BUILD_DIR}/src
				${JAVA_SRC_FILES}
			COMMENT "Compiling Java sources for ${APK_TARGET}"
			VERBATIM
		)

		list(APPEND DEX_INPUTS ${JAVA_CLASS_FILES})
		list(APPEND DEX_DEPENDS ${JAVA_CLASS_FILES})
	else()
		# No Java sources provided, use Empty.class placeholder
		set(DEX_INPUTS "${APK_BUILD_DIR}/obj/Empty.class")
		set(DEX_DEPENDS "${APK_BUILD_DIR}/obj/Empty.class")
	endif()

	# Build classes.dex from all inputs
	add_custom_command(
		OUTPUT ${APK_BUILD_DIR}/obj/classes.dex
		DEPENDS ${DEX_DEPENDS}
		COMMAND ${D8} --lib ${ANDROID_JAR} --release --output ${APK_BUILD_DIR}/obj ${DEX_INPUTS}
		COMMENT "Building classes.dex for ${APK_TARGET}"
		VERBATIM
	)
endfunction()

###############################################################################
## add_apk: Create an APK build target from a shared library target
###############################################################################
#
# Usage:
#   add_apk(<target_name>
#       PACKAGE_NAME <com.example.package>
#       [APP_NAME <display_name>]
#       [MIN_SDK <version>]
#       [TARGET_SDK <version>]
#       [MANIFEST <path/to/AndroidManifest.xml>]
#       [RESOURCES <path/to/resources>]
#       [ASSETS <path/to/assets>]
#       [ASSETS_STAMP <path/to/stamp_file>]
#       [LIB_NAME <native_lib_name>]
#       [ACTIVITY <fully.qualified.ActivityName>]
#   )
#
# Creates targets:
#   <target_name>-apk     - Build the APK
#   <target_name>-install - Install APK to device via adb
#   <target_name>-run     - Install and launch the app
#
# Java sources can be added via apk_add_java_sources() and are automatically
# compiled at the end of directory processing.
function(add_apk APK_TARGET)
	# Parse remaining arguments
	cmake_parse_arguments(APK
		"SKIP_ENTRYPOINT"  # Options (boolean flags)
		"PACKAGE_NAME;APP_NAME;MIN_SDK;TARGET_SDK;MANIFEST;RESOURCES;ASSETS;ASSETS_STAMP;LIB_NAME;ACTIVITY"  # Single-value args
		""  # Multi-value args
		${ARGN}
	)

	# Validate required arguments
	if(NOT APK_TARGET)
		message(FATAL_ERROR "add_apk: target name is required as first argument")
	endif()
	if(NOT APK_PACKAGE_NAME)
		message(FATAL_ERROR "add_apk: PACKAGE_NAME argument is required")
	endif()
	if(NOT TARGET ${APK_TARGET})
		message(FATAL_ERROR "add_apk: TARGET '${APK_TARGET}' is not a valid CMake target")
	endif()

	# Verify target is a shared library
	get_target_property(TARGET_TYPE ${APK_TARGET} TYPE)
	if(NOT ${TARGET_TYPE} STREQUAL "SHARED_LIBRARY")
		message(FATAL_ERROR "add_apk: TARGET '${APK_TARGET}' must be a SHARED_LIBRARY (found ${TARGET_TYPE})")
	endif()

	# Set defaults
	if(NOT APK_MIN_SDK)
		set(APK_MIN_SDK 32)
	endif()
	if(NOT APK_TARGET_SDK)
		set(APK_TARGET_SDK 33)
	endif()
	if(NOT APK_MANIFEST)
		set(APK_MANIFEST "${_SK_APP_ANDROID_DIR}/AndroidManifest.xml")
	endif()
	if(NOT APK_RESOURCES)
		set(APK_RESOURCES "${_SK_APP_ANDROID_DIR}/resources")
	endif()
	if(NOT APK_LIB_NAME)
		set(APK_LIB_NAME "${APK_TARGET}")
	endif()
	if(NOT APK_APP_NAME)
		set(APK_APP_NAME "${APK_TARGET}")
	endif()
	if(NOT APK_ACTIVITY)
		set(APK_ACTIVITY "android.app.NativeActivity")
	endif()

	# Validate paths exist
	if(NOT EXISTS "${APK_MANIFEST}")
		message(FATAL_ERROR "add_apk: MANIFEST file not found: ${APK_MANIFEST}")
	endif()
	if(NOT EXISTS "${APK_RESOURCES}")
		message(FATAL_ERROR "add_apk: RESOURCES directory not found: ${APK_RESOURCES}")
	endif()
	if(APK_ASSETS AND NOT EXISTS "${APK_ASSETS}")
		message(FATAL_ERROR "add_apk: ASSETS directory not found: ${APK_ASSETS}")
	endif()

	message(STATUS "Configuring APK: ${APK_TARGET}")
	message(STATUS "  Package: ${APK_PACKAGE_NAME}")
	message(STATUS "  Min SDK: ${APK_MIN_SDK}, Target SDK: ${APK_TARGET_SDK}")
	if(APK_ASSETS)
		message(STATUS "  Assets: ${APK_ASSETS}")
	endif()

	###########################################################################
	## Add Android glue code to target
	###########################################################################

	target_link_libraries(${APK_TARGET} PRIVATE android log)
	if(NOT APK_SKIP_ENTRYPOINT)
		target_include_directories(${APK_TARGET} PUBLIC ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue)
		target_sources(${APK_TARGET} PRIVATE
			${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
	endif()

	###########################################################################
	## Get list of shared libraries to pack
	###########################################################################

	get_target_property(PROJECT_LIBRARIES ${APK_TARGET} LINK_LIBRARIES)
	set(APK_SRC_LIBRARIES $<TARGET_FILE:${APK_TARGET}>)
	set(APK_COPY_LIBRARIES lib/${CMAKE_ANDROID_ARCH_ABI}/$<TARGET_FILE_NAME:${APK_TARGET}>)
	foreach(CURR ${PROJECT_LIBRARIES})
		if (TARGET ${CURR})
			get_target_property(CURR_TARGET_TYPE ${CURR} TYPE)
			if(${CURR_TARGET_TYPE} STREQUAL "SHARED_LIBRARY")
				list(APPEND APK_SRC_LIBRARIES $<TARGET_FILE:${CURR}>)
				list(APPEND APK_COPY_LIBRARIES lib/${CMAKE_ANDROID_ARCH_ABI}/$<TARGET_FILE_NAME:${CURR}>)
			endif()
		endif()
	endforeach()

	###########################################################################
	## Set up build directories and paths
	###########################################################################

	set(APK_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/apk_${APK_TARGET}")
	set(APK_BASE      ${APK_BUILD_DIR}/${APK_TARGET}.1.base)
	set(APK_UNALIGNED ${APK_BUILD_DIR}/${APK_TARGET}.2.unaligned)
	set(APK_UNSIGNED  ${APK_BUILD_DIR}/${APK_TARGET}.3.unsigned)
	set(OUTPUT_APK    ${CMAKE_CURRENT_BINARY_DIR}/${APK_TARGET}.apk)

	# Make folders
	file(MAKE_DIRECTORY
		"${APK_BUILD_DIR}/obj"
		"${APK_BUILD_DIR}/resources/values"
		"${APK_BUILD_DIR}/lib/${CMAKE_ANDROID_ARCH_ABI}")

	# Set manifest variables for substitution
	set(ANDROID_PACKAGE_NAME ${APK_PACKAGE_NAME})
	set(ANDROID_MIN_SDK_VERSION ${APK_MIN_SDK})
	set(ANDROID_TARGET_SDK_VERSION ${APK_TARGET_SDK})
	set(APK_LIB_NAME ${APK_LIB_NAME})

	# Configure AndroidManifest.xml
	configure_file(
		${APK_MANIFEST}
		${APK_BUILD_DIR}/obj/AndroidManifest.xml
		@ONLY)

	# Generate custom strings.xml with the app name
	file(WRITE ${APK_BUILD_DIR}/resources/values/strings.xml
"<?xml version=\"1.0\" encoding=\"utf-8\"?>
<resources>
	<string name=\"app_name\">${APK_APP_NAME}</string>
</resources>
")

	###########################################################################
	## Compile Empty.java placeholder (used only when no other Java sources)
	###########################################################################

	# Generate and compile Empty.java in a single command to avoid configure-time files
	set(EMPTY_JAVA_FILE "${APK_BUILD_DIR}/src/android/Empty.java")
	set(GENERATE_SCRIPT "${_SK_APP_ANDROID_DIR}/generate_empty_java.cmake")
	add_custom_command(
		OUTPUT  ${APK_BUILD_DIR}/obj/Empty.class
		COMMAND ${CMAKE_COMMAND} -DOUTPUT_FILE=${EMPTY_JAVA_FILE} -P ${GENERATE_SCRIPT}
		COMMAND ${JAVAC} -d ${APK_BUILD_DIR}/obj -sourcepath src ${EMPTY_JAVA_FILE}
		COMMENT "Compiling Empty.java placeholder for ${APK_TARGET}"
		VERBATIM
	)

	###########################################################################
	## Compile Android resources
	###########################################################################

	# Compile shared resources (icons, etc.)
	add_custom_command(
		OUTPUT  ${APK_BUILD_DIR}/obj/apk_resources_shared.zip
		DEPENDS ${APK_RESOURCES}
		COMMAND ${AAPT2} compile
			--dir ${APK_RESOURCES}
			-o ${APK_BUILD_DIR}/obj/apk_resources_shared.zip
		COMMENT "Compiling shared resources for ${APK_TARGET}" )

	# Compile per-APK resources (custom app name)
	add_custom_command(
		OUTPUT  ${APK_BUILD_DIR}/obj/apk_resources_custom.zip
		DEPENDS ${APK_BUILD_DIR}/resources/values/strings.xml
		COMMAND ${AAPT2} compile
			--dir ${APK_BUILD_DIR}/resources
			-o ${APK_BUILD_DIR}/obj/apk_resources_custom.zip
		COMMENT "Compiling custom resources for ${APK_TARGET}" )

	###########################################################################
	## Assemble base APK (resources + manifest)
	###########################################################################

	# Build aapt2 link command with optional assets
	set(AAPT2_LINK_ARGS
		-o ${APK_BASE}
		--manifest ${APK_BUILD_DIR}/obj/AndroidManifest.xml
		-I ${ANDROID_SDK_ROOT}/platforms/android-${APK_MIN_SDK}/android.jar
		${APK_BUILD_DIR}/obj/apk_resources_shared.zip
		${APK_BUILD_DIR}/obj/apk_resources_custom.zip
	)
	if(APK_ASSETS)
		list(APPEND AAPT2_LINK_ARGS -A ${APK_ASSETS})
	endif()

	# Build dependencies for base APK
	set(APK_BASE_DEPENDS
		${APK_BUILD_DIR}/obj/classes.dex
		${APK_BUILD_DIR}/obj/apk_resources_shared.zip
		${APK_BUILD_DIR}/obj/apk_resources_custom.zip
		${APK_BUILD_DIR}/obj/AndroidManifest.xml
	)
	if(APK_ASSETS_STAMP)
		list(APPEND APK_BASE_DEPENDS ${APK_ASSETS_STAMP})
	endif()

	add_custom_command(
		DEPENDS ${APK_BASE_DEPENDS}
		OUTPUT
			${APK_BASE}
		COMMAND ${CMAKE_COMMAND} -E rm -f ${APK_BASE}
		COMMAND ${AAPT2} link ${AAPT2_LINK_ARGS}
		COMMAND cd ${APK_BUILD_DIR}/obj && ${AAPT} add ${APK_BASE} classes.dex
		COMMENT "Building base APK for ${APK_TARGET}")

	###########################################################################
	## Assemble final APK (add native libraries, align, sign)
	###########################################################################

	add_custom_command(
		DEPENDS
			${APK_TARGET}
			${APK_BASE}
		OUTPUT
			${OUTPUT_APK}
		COMMAND ${CMAKE_COMMAND} -E rm -f
			${APK_UNALIGNED}
			${APK_UNSIGNED}
			${OUTPUT_APK}
		COMMAND ${CMAKE_COMMAND} -E copy
			${APK_SRC_LIBRARIES}
			${APK_BUILD_DIR}/lib/${CMAKE_ANDROID_ARCH_ABI}/
		COMMAND ${CMAKE_COMMAND} -E copy ${APK_BASE} ${APK_UNALIGNED}
		COMMAND cd ${APK_BUILD_DIR} && ${AAPT} add ${APK_UNALIGNED} ${APK_COPY_LIBRARIES}
		COMMAND ${ZIPALIGN} -p 4 ${APK_UNALIGNED} ${APK_UNSIGNED}
		COMMAND ${APKSIGN} sign --ks ${KEYSTORE} --ks-key-alias ${KEY_ALIAS} --ks-pass pass:${KEYSTORE_PASS} --key-pass pass:${KEY_ALIAS_PASS} --out ${OUTPUT_APK} ${APK_UNSIGNED}
		COMMENT "Building final APK: ${APK_TARGET}.apk")

	###########################################################################
	## Create CMake targets
	###########################################################################

	# APK build target (part of ALL so it builds by default)
	add_custom_target(${APK_TARGET}-apk ALL
		DEPENDS
			${APK_TARGET}
			${OUTPUT_APK}
		COMMENT "Building Android APK: ${APK_TARGET}.apk")

	# Install target (adb install)
	add_custom_target(${APK_TARGET}-install
		DEPENDS ${APK_TARGET}-apk
		COMMAND ${ANDROID_SDK_ROOT}/platform-tools/adb install -r ${OUTPUT_APK}
		COMMENT "Installing ${APK_TARGET}.apk to device")

	# Run target (install + launch)
	add_custom_target(${APK_TARGET}-run
		DEPENDS ${APK_TARGET}-apk
		COMMAND ${ANDROID_SDK_ROOT}/platform-tools/adb install -r ${OUTPUT_APK}
		COMMAND ${ANDROID_SDK_ROOT}/platform-tools/adb shell am start -n ${APK_PACKAGE_NAME}/${APK_ACTIVITY}
		COMMENT "Installing and running ${APK_TARGET}.apk")

	# Set target properties for introspection and _apk_build_dex
	set_target_properties(${APK_TARGET}-apk PROPERTIES
		APK_OUTPUT_PATH "${OUTPUT_APK}"
		APK_PACKAGE_NAME "${APK_PACKAGE_NAME}"
		APK_BUILD_DIR "${APK_BUILD_DIR}"
		APK_MIN_SDK "${APK_MIN_SDK}"
		APK_ASSETS_DIR "${APK_ASSETS}"
	)

	# Defer DEX build to end of directory processing, allowing apk_add_java_sources()
	# calls to accumulate before compilation.
	# Note: EVAL CODE is needed to capture APK_TARGET by value (CMake DEFER quirk)
	cmake_language(EVAL CODE "
		cmake_language(DEFER
			DIRECTORY [[${CMAKE_CURRENT_SOURCE_DIR}]]
			ID _apk_dex_${APK_TARGET}
			CALL _apk_build_dex [[${APK_TARGET}]])")

endfunction()
