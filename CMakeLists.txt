cmake_minimum_required(VERSION 3.19)
project(sk_app VERSION 1.0.0)

# Options
option(SKA_BUILD_SHARED   "Build shared library"       OFF)
option(SKA_BUILD_EXAMPLES "Build example applications" ON)

# Release build optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
	add_compile_options(
		-fno-exceptions
		-fno-unwind-tables
		-fno-asynchronous-unwind-tables
		-fdata-sections
		-ffunction-sections
	)

	add_link_options(
		-Wl,--gc-sections
		-Wl,--strip-all
	)
endif()

###############################################################################
# Create sk_app library target
###############################################################################

# Create library (shared or static)
if(SKA_BUILD_SHARED)
	add_library(sk_app SHARED)
	target_compile_definitions(sk_app
		PRIVATE SKA_BUILD_SHARED SKA_EXPORT
		INTERFACE SKA_BUILD_SHARED
	)
else()
	add_library(sk_app STATIC)
endif()

# C standard
set_target_properties(sk_app PROPERTIES
	C_STANDARD 11
	C_STANDARD_REQUIRED ON
	C_EXTENSIONS OFF
)

# Add common sources
target_sources(sk_app PRIVATE
	src/ska_common.c
	src/ska_event.c
	src/ska_input.c
	src/ska_text.c
	src/ska_file.c
	src/ska_wgpu.c
)

# Include directories
target_include_directories(sk_app
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Compiler warnings
if(MSVC)
	target_compile_options(sk_app PRIVATE /W4 /WX-)
else()
	target_compile_options(sk_app PRIVATE
		-Wall -Wextra -Wpedantic
		-Wno-unused-parameter
		-Wno-missing-field-initializers
	)
endif()

###############################################################################
# Platform-specific configuration
###############################################################################

if(WIN32)
	set(SKA_PLATFORM "WIN32")
	target_compile_definitions(sk_app PRIVATE SKA_PLATFORM_WIN32)

	# Windows-specific sources
	target_sources(sk_app PRIVATE src/ska_win32.c)

	# Windows libraries
	target_link_libraries(sk_app PRIVATE winmm)

	# Static link MinGW runtime for cross-compilation
	if(MINGW)
		target_link_options(sk_app PRIVATE -static-libgcc -static-libstdc++ -static)
	endif()

elseif(ANDROID)
	set(SKA_PLATFORM "ANDROID")
	target_compile_definitions(sk_app PRIVATE SKA_PLATFORM_ANDROID)

	# ANDROID_PLATFORM is an official NDK toolchain variable, set via
	# -DANDROID_PLATFORM=android-NN. We parse the API level from it.
	if(NOT DEFINED ANDROID_PLATFORM)
		message(FATAL_ERROR "ANDROID_PLATFORM is not set. Use -DANDROID_PLATFORM=android-NN with the NDK toolchain.")
	endif()
	string(REGEX REPLACE "android-" "" SKA_API_LEVEL "${ANDROID_PLATFORM}")

	# Android-specific sources
	target_sources(sk_app PRIVATE src/ska_android.c)

	# Glue header needed for struct android_app* type in both modes
	target_include_directories(sk_app PRIVATE
		${CMAKE_ANDROID_NDK}/sources/android/native_app_glue
	)
	target_link_libraries(sk_app PRIVATE android log)

	# Standalone entry point target: provides android_main() and the glue.
	# Standalone targets link both sk_app and sk_app_entrypoint.
	# Library-mode targets (C#, etc.) link only sk_app.
	add_library(sk_app_entrypoint STATIC
		src/ska_android_entry.c
		${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
	)
	target_include_directories(sk_app_entrypoint PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src
		${CMAKE_CURRENT_SOURCE_DIR}/include
		${CMAKE_ANDROID_NDK}/sources/android/native_app_glue
	)
	target_compile_definitions(sk_app_entrypoint PRIVATE SKA_PLATFORM_ANDROID)
	target_link_libraries(sk_app_entrypoint PRIVATE android log)

	# Include Android APK build function
	include(${CMAKE_CURRENT_SOURCE_DIR}/android/AndroidAPK.cmake)

	# Build sk_app.jar — pre-compiled Java classes for library-mode consumers
	# (e.g. StereoKit NuGet). Standalone apps use apk_add_java_sources() instead.
	find_program(JAR NAMES jar REQUIRED PATHS ${JAVA_HOME_BIN})

	set(SKA_JAR_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/jar_sk_app")
	set(SKA_JAR_OUTPUT    "${CMAKE_CURRENT_BINARY_DIR}/sk_app.jar")
	set(SKA_ANDROID_JAR   "${ANDROID_SDK_ROOT}/platforms/android-${SKA_API_LEVEL}/android.jar")
	if(NOT EXISTS "${SKA_ANDROID_JAR}")
		message(FATAL_ERROR "Android SDK jar not found at ${SKA_ANDROID_JAR}. "
			"Ensure android-${SKA_API_LEVEL} platform is installed.")
	endif()
	set(SKA_JAVA_DIR      "${CMAKE_CURRENT_SOURCE_DIR}/android/java")
	set(SKA_JAVA_SOURCES
		${SKA_JAVA_DIR}/net/stereokit/sk_app/SkAppActivity.java
		${SKA_JAVA_DIR}/net/stereokit/sk_app/SkAppResultFragment.java
	)

	add_custom_command(
		OUTPUT  ${SKA_JAR_OUTPUT}
		DEPENDS ${SKA_JAVA_SOURCES}
		COMMAND ${CMAKE_COMMAND} -E make_directory ${SKA_JAR_BUILD_DIR}
		COMMAND ${JAVAC}
			-source 8 -target 8
			-classpath ${SKA_ANDROID_JAR}
			-d ${SKA_JAR_BUILD_DIR}
			${SKA_JAVA_SOURCES}
		COMMAND ${JAR} cf ${SKA_JAR_OUTPUT} -C ${SKA_JAR_BUILD_DIR} .
		COMMENT "Building sk_app.jar"
		VERBATIM
	)
	add_custom_target(sk_app_jar ALL DEPENDS ${SKA_JAR_OUTPUT})

elseif(APPLE)
	set(SKA_PLATFORM "MACOS")
	target_compile_definitions(sk_app PRIVATE SKA_PLATFORM_MACOS)

	# macOS-specific sources
	target_sources(sk_app PRIVATE src/ska_macos.m)

	# macOS framework dependencies
	target_link_libraries(sk_app PUBLIC
		"-framework Cocoa"
		"-framework IOKit"
		"-framework CoreVideo"
		"-framework QuartzCore"
		"-framework UniformTypeIdentifiers"
	)

elseif(EMSCRIPTEN)
	set(SKA_PLATFORM "WEB")
	target_compile_definitions(sk_app PRIVATE SKA_PLATFORM_WEB)

	# Web-specific sources
	target_sources(sk_app PRIVATE src/ska_web.c)

	# EM_ASM requires GNU C mode (-std=gnu11), uses $0-style identifiers, and
	# expands to variadic macros that trip -Wpedantic when called with no args
	set_target_properties(sk_app PROPERTIES C_EXTENSIONS ON)
	target_compile_options(sk_app PRIVATE
		-Wno-dollar-in-identifier-extension
		-Wno-variadic-macro-arguments-omitted
	)

elseif(UNIX)
	set(SKA_PLATFORM "LINUX")
	target_compile_definitions(sk_app PRIVATE SKA_PLATFORM_LINUX)

	# Linux-specific sources. The display server is a runtime choice here, so
	# ska_linux.c dispatches every ska_platform_* call to a backend, and
	# ska_linux_common.c holds the parts that need no display server at all.
	target_sources(sk_app PRIVATE
		src/ska_linux.c
		src/ska_linux_common.c
	)
	target_link_libraries(sk_app PRIVATE ${CMAKE_DL_LIBS})

	###########################################################################
	# X11 backend (optional, detected)
	###########################################################################

	# X11 is dlopened, not linked, so one binary runs on X11, Wayland, and
	# headless systems. Only the headers are needed at build time, and ska_linux.c
	# is written for either backend being absent, so a machine with no X11 headers
	# builds a Wayland-only sk_app rather than failing.
	#
	# The extension headers are required too (ska_x11_dyn.h includes them);
	# checking here turns a missing -dev package into a clean "backend: no"
	# instead of a compile error mid-build.
	find_package(X11)
	set(SKA_LINUX_X11_FOUND FALSE)
	if(X11_FOUND AND X11_Xrandr_FOUND AND X11_Xcursor_FOUND AND X11_Xi_FOUND AND X11_Xext_FOUND)
		set(SKA_LINUX_X11_FOUND TRUE)
		target_compile_definitions(sk_app PRIVATE SKA_LINUX_X11)
		target_sources(sk_app PRIVATE
			src/x11/ska_x11.c
			src/x11/ska_x11_dyn.c
		)
		target_include_directories(sk_app PRIVATE ${X11_INCLUDE_DIR})
	endif()

	###########################################################################
	# Wayland backend (optional, detected)
	###########################################################################

	include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/SkAppWayland.cmake)
	ska_wayland_detect()

	if(NOT SKA_LINUX_WAYLAND_FOUND)
		message(WARNING
			"sk_app: building without the Wayland backend. wayland-client, xkbcommon "
			"or libdecor-0 development headers are missing, so this binary runs on "
			"X11 and XWayland only.")
	endif()

	if(SKA_LINUX_WAYLAND_FOUND)
		target_compile_definitions(sk_app PRIVATE SKA_LINUX_WAYLAND)
		target_sources(sk_app PRIVATE
			src/wayland/ska_wayland.c
			src/wayland/ska_wl_dyn.c
			src/wayland/ska_decor.c
		)

		# Protocol code is committed rather than generated at build time; all
		# protocols land in this one file. Refresh with
		# tools/gen_wayland_protocols.sh.
		target_sources(sk_app PRIVATE src/wayland/wayland-protocols.c)

		# Generated code is third-party output and does not answer to sk_app's
		# warning settings.
		set_source_files_properties(src/wayland/wayland-protocols.c
			PROPERTIES COMPILE_OPTIONS "-w")

		# Must precede the system paths: src/wayland holds a wayland-client.h
		# that deliberately shadows the real one, which is what lets sk_app
		# dlopen libwayland. See that file for the reasoning.
		target_include_directories(sk_app BEFORE PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src/wayland
		)
		target_include_directories(sk_app PRIVATE ${WAYLAND_INCLUDE_DIRS})
		set(SKA_WAYLAND_STATUS "yes")
	else()
		set(SKA_WAYLAND_STATUS "no (wayland-client, xkbcommon, or libdecor headers not found)")
	endif()

else()
	message(FATAL_ERROR "Unsupported platform")
endif()

message(STATUS "Building for platform: ${SKA_PLATFORM}")

# Include add_skapp() helper function
include(${CMAKE_CURRENT_SOURCE_DIR}/SkApp.cmake)

###############################################################################
# Install rules
###############################################################################

include(GNUInstallDirs)

set(SK_APP_INSTALL_TARGETS sk_app)

install(TARGETS ${SK_APP_INSTALL_TARGETS}
	EXPORT sk_app-targets
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)


install(FILES include/sk_app.h
	DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT sk_app-targets
	FILE sk_app-config.cmake
	NAMESPACE ska::
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sk_app
)

###############################################################################
# Examples
###############################################################################

if(SKA_BUILD_EXAMPLES)
	add_subdirectory(examples)
endif()

###############################################################################
# Configuration summary
###############################################################################

message(STATUS "")
message(STATUS "sk_app configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Platform: ${SKA_PLATFORM}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
if(SKA_PLATFORM STREQUAL "LINUX")
	if(SKA_LINUX_X11_FOUND)
		message(STATUS "  X11 backend: yes")
	else()
		message(STATUS "  X11 backend: no (X11, Xrandr, Xcursor, or Xi headers not found)")
	endif()
	message(STATUS "  Wayland backend: ${SKA_WAYLAND_STATUS}")
	if(NOT SKA_LINUX_X11_FOUND AND NOT SKA_LINUX_WAYLAND_FOUND)
		message(FATAL_ERROR "sk_app: neither X11 nor Wayland headers were found, so there is no Linux backend to build.")
	endif()
endif()
message(STATUS "  Shared library: ${SKA_BUILD_SHARED}")
message(STATUS "  Build examples: ${SKA_BUILD_EXAMPLES}")
message(STATUS "")
