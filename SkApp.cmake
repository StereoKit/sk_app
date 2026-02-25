# sk_app CMake Support
# Provides add_skapp() for building sk_app-based applications on all platforms
#
# Usage:
#   add_skapp(<target_name>
#       [PACKAGE_NAME <com.example.app>]  # Required on Android, ignored elsewhere
#       [APP_NAME "My App"]               # Optional, defaults to target name
#       [MIN_SDK <version>]               # Android only: minimum SDK version
#       [TARGET_SDK <version>]            # Android only: target SDK version
#       [MANIFEST <path>]                 # Android only: custom manifest (defaults to sk_app's)
#       [RESOURCES <path>]                # Android only: custom resources directory
#       [LIB_NAME <name>]                 # Android only: native library name
#   )
#   target_sources(<target_name> PRIVATE main.c)
#   target_skapp_assets(<target_name> <path> [<path>...])
#
# After calling add_skapp(), use ${target}_ASSETS_DIR to output compiled shaders
# or other generated files - they'll be included in the final app package.
#
# On Android: Creates shared library + APK with SkAppActivity, assets packed in APK
# On other platforms: Creates executable, assets copied to Assets/ folder next to it

# Add asset directories to be bundled with the app (always syncs to catch new files)
function(target_skapp_assets SKA_TARGET)
	get_target_property(ASSETS_DIR ${SKA_TARGET} SKAPP_ASSETS_DIR)
	get_target_property(ASSETS_STAMP ${SKA_TARGET} SKAPP_ASSETS_STAMP)
	get_target_property(ASSET_COUNT ${SKA_TARGET} SKAPP_ASSET_COUNT)
	if(NOT ASSET_COUNT)
		set(ASSET_COUNT 0)
	endif()

	# Use smart copy script on Android (touches stamp only when content changes)
	# Use simple copy on desktop (no stamp needed, assets are just files next to exe)
	if(ASSETS_STAMP)
		set(COPY_SCRIPT "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cmake/copy_assets.cmake")
	elseif(CMAKE_VERSION VERSION_GREATER_EQUAL "3.26")
		set(COPY_CMD copy_directory_if_different)
	else()
		set(COPY_CMD copy_directory)
	endif()

	foreach(ASSET_DIR ${ARGN})
		math(EXPR ASSET_COUNT "${ASSET_COUNT} + 1")
		if(ASSETS_STAMP)
			# Use smart copy script that only touches stamp when content changes
			add_custom_target(${SKA_TARGET}-asset-${ASSET_COUNT}
				COMMAND ${CMAKE_COMMAND}
					-DSRC_DIR=${ASSET_DIR}
					-DDEST_DIR=${ASSETS_DIR}
					-DSTAMP_FILE=${ASSETS_STAMP}
					-P ${COPY_SCRIPT}
				COMMENT "Syncing assets from ${ASSET_DIR}"
			)
		else()
			# Simple directory copy for desktop
			add_custom_target(${SKA_TARGET}-asset-${ASSET_COUNT}
				COMMAND ${CMAKE_COMMAND} -E ${COPY_CMD} ${ASSET_DIR} ${ASSETS_DIR}
				COMMENT "Syncing assets from ${ASSET_DIR}"
			)
		endif()
		# Asset sync runs after main target (catches shader compilation, etc.)
		add_dependencies(${SKA_TARGET}-asset-${ASSET_COUNT} ${SKA_TARGET})
		add_dependencies(${SKA_TARGET}-package ${SKA_TARGET}-asset-${ASSET_COUNT})
	endforeach()

	set_target_properties(${SKA_TARGET} PROPERTIES SKAPP_ASSET_COUNT ${ASSET_COUNT})
endfunction()

# Link sk_app only if it isn't already embedded in an upstream shared library.
# Called via cmake_language(DEFER ...) at end-of-directory so that all
# target_link_libraries() calls in the caller's CMakeLists.txt are visible.
# Avoids a duplicate static copy of g_ska when a dep like libStereoKitC.so
# already links sk_app — two copies means one copy has an uninitialized g_ska.
function(_skapp_link_sk_app SKA_TARGET)
	get_target_property(link_libs ${SKA_TARGET} LINK_LIBRARIES)
	if(link_libs)
		foreach(dep ${link_libs})
			if(TARGET ${dep})
				get_target_property(dep_type  ${dep} TYPE)
				get_target_property(dep_links ${dep} LINK_LIBRARIES)
				if(dep_type STREQUAL "SHARED_LIBRARY" AND dep_links AND "sk_app" IN_LIST dep_links)
					message(STATUS "sk_app: ${SKA_TARGET} skipping sk_app link (already in ${dep})")
					return()
				endif()
			endif()
		endforeach()
	endif()
	target_link_libraries(${SKA_TARGET} PRIVATE sk_app)
endfunction()

function(add_skapp SKA_TARGET)
	cmake_parse_arguments(SKA "" "PACKAGE_NAME;APP_NAME;MIN_SDK;TARGET_SDK;MANIFEST;RESOURCES;LIB_NAME" "" ${ARGN})

	if(NOT SKA_TARGET)
		message(FATAL_ERROR "add_skapp: target name is required as first argument")
	endif()
	if(NOT SKA_APP_NAME)
		set(SKA_APP_NAME "${SKA_TARGET}")
	endif()

	if(NOT ANDROID)
		# Desktop: executable with assets copied directly to output folder
		add_executable(${SKA_TARGET})

		# Assets go directly to output folder (no staging needed)
		set(SKA_ASSETS_DIR "$<TARGET_FILE_DIR:${SKA_TARGET}>/Assets")
		set_target_properties(${SKA_TARGET} PROPERTIES SKAPP_ASSETS_DIR "${SKA_ASSETS_DIR}")

		# Package target ensures assets are synced (depends on main target for output dir)
		add_custom_target(${SKA_TARGET}-package ALL)
		add_dependencies(${SKA_TARGET}-package ${SKA_TARGET})
	else()
		# Android: create shared library + APK with SkAppActivity
		if(NOT SKA_PACKAGE_NAME)
			message(FATAL_ERROR "add_skapp: PACKAGE_NAME is required for Android builds")
		endif()

		add_library(${SKA_TARGET} SHARED)

		# Android needs staging directory and stamp file for APK packaging
		set(SKA_ASSETS_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets_${SKA_TARGET}")
		set(SKA_ASSETS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/assets_${SKA_TARGET}.stamp")
		file(MAKE_DIRECTORY "${SKA_ASSETS_DIR}")
		file(TOUCH "${SKA_ASSETS_STAMP}")
		set_target_properties(${SKA_TARGET} PROPERTIES
			SKAPP_ASSETS_DIR "${SKA_ASSETS_DIR}"
			SKAPP_ASSETS_STAMP "${SKA_ASSETS_STAMP}"
		)

		# Package target for Android (APK build will depend on this)
		add_custom_target(${SKA_TARGET}-package ALL)
		add_dependencies(${SKA_TARGET}-package ${SKA_TARGET})

		if(NOT SKA_MANIFEST)
			set(SKA_MANIFEST "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/android/AndroidManifest_SkApp.xml")
		elseif(NOT IS_ABSOLUTE "${SKA_MANIFEST}")
			set(SKA_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/${SKA_MANIFEST}")
		endif()

		set(APK_ARGS
			PACKAGE_NAME ${SKA_PACKAGE_NAME}
			APP_NAME "${SKA_APP_NAME}"
			MANIFEST ${SKA_MANIFEST}
			ACTIVITY "net.stereokit.sk_app.SkAppActivity"
			ASSETS ${SKA_ASSETS_DIR}
			ASSETS_STAMP ${SKA_ASSETS_STAMP}
		)
		if(SKA_MIN_SDK)
			list(APPEND APK_ARGS MIN_SDK ${SKA_MIN_SDK})
		endif()
		if(SKA_TARGET_SDK)
			list(APPEND APK_ARGS TARGET_SDK ${SKA_TARGET_SDK})
		endif()
		if(SKA_RESOURCES)
			list(APPEND APK_ARGS RESOURCES ${SKA_RESOURCES})
		endif()
		if(SKA_LIB_NAME)
			list(APPEND APK_ARGS LIB_NAME ${SKA_LIB_NAME})
		endif()

		add_apk(${SKA_TARGET} ${APK_ARGS} SKIP_ENTRYPOINT)
		target_link_libraries(${SKA_TARGET} PRIVATE -Wl,--whole-archive sk_app_entrypoint -Wl,--no-whole-archive)
		add_dependencies(${SKA_TARGET}-apk ${SKA_TARGET}-package)

		apk_add_java_sources(${SKA_TARGET} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/android/java"
			"net/stereokit/sk_app/SkAppActivity.java"
			"net/stereokit/sk_app/SkAppResultFragment.java"
		)

		message(STATUS "sk_app Android target configured: ${SKA_TARGET}")
		message(STATUS "  Package: ${SKA_PACKAGE_NAME}")
		message(STATUS "  Activity: net.stereokit.sk_app.SkAppActivity")
	endif()

	# Defer until end-of-directory so all target_link_libraries() calls are
	# done. _skapp_link_sk_app will skip linking if a SHARED dep already
	# embeds sk_app, preventing duplicate static copies with separate g_ska.
	# Note: EVAL CODE is needed to capture SKA_TARGET by value (CMake DEFER quirk)
	cmake_language(EVAL CODE "
		cmake_language(DEFER
			DIRECTORY [[${CMAKE_CURRENT_SOURCE_DIR}]]
			CALL _skapp_link_sk_app [[${SKA_TARGET}]])")

	# Export assets dir path to parent scope for convenience
	set(${SKA_TARGET}_ASSETS_DIR "${SKA_ASSETS_DIR}" PARENT_SCOPE)
endfunction()
