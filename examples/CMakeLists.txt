# Examples
#
# Each example is in its own subdirectory with its own CMakeLists.txt
# This makes it easy to add new examples and keep them organized

include(FetchContent)

###############################################################################
# sk_renderer dependency
###############################################################################

# sk_renderer is Vulkan-based and doesn't build for the web yet, so the
# examples that depend on it are skipped there
if(NOT EMSCRIPTEN)
	# This is shared by a couple different samples
	message(STATUS "Fetching sk_renderer...")

	# Disable sk_renderer's example app to avoid build complexity (we use the shader compiler in other projects here)
	set(SKR_BUILD_EXAMPLES OFF CACHE BOOL "Disable sk_renderer examples" FORCE)
	set(SKSHADERC_ENABLE_GLSLANG OFF CACHE BOOL "Disable glslang for faster build" FORCE)
	FetchContent_Declare(
		sk_renderer
		GIT_REPOSITORY https://github.com/StereoKit/sk_renderer.git
		GIT_TAG        v2026.7.13
		GIT_SHALLOW    TRUE
	)
	FetchContent_MakeAvailable(sk_renderer)
	#set(sk_renderer_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../sk_renderer)
	#add_subdirectory(${sk_renderer_SOURCE_DIR} ${sk_renderer_SOURCE_DIR}/build)

	add_subdirectory(renderer_example)
	add_subdirectory(imgui_example)
endif()

add_subdirectory(test_window)
add_subdirectory(minimal_window)
