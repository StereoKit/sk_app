# Dear ImGui Example (requires Vulkan and sk_renderer)

# Enable C++ for this example (Dear ImGui is C++)
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

###############################################################################
# App definition
###############################################################################

# Build the imgui example
add_skapp(imgui_example
	PACKAGE_NAME net.stereokit.imgui_example
	APP_NAME "ImGui Example"
)

target_sources(imgui_example PRIVATE
	main.c
)

if(UNIX AND NOT APPLE) # Link math library on Linux
	target_link_libraries(imgui_example PRIVATE m)
endif()
if(MINGW) # Static link MinGW runtime for cross-compilation
	target_link_options(imgui_example PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

target_link_libraries     (imgui_example PRIVATE sk_renderer)
target_include_directories(imgui_example PRIVATE
	${sk_renderer_SOURCE_DIR}/example/imgui_backend
)
skr_copy_moltenvk(imgui_example)

# imgui_impl_sk_renderer.cpp includes "imgui.hlsl.h". Through v2026.7.13
# sk_renderer committed that generated header to its source tree, so the
# include directory above was enough. As of v2026.8.7 the output moved to
# sk_renderer's build dir (a source-tree copy would ping-pong between backends,
# whose flags differ), and only its own example target compiles it — which we
# disable via SKR_BUILD_EXAMPLES. So compile the shader ourselves.
#
# skshaderc_compile_headers resolves shader paths against the *calling*
# CMakeLists' source dir, so the path has to be made relative to here rather
# than passed absolute. It adds OUTPUT_FOLDER to the target's include path.
if(SKR_USE_WEBGPU)
	set(_imgui_shader_target "-svsl -t w")
else()
	set(_imgui_shader_target "-t s")
endif()
file(RELATIVE_PATH _imgui_hlsl
	${CMAKE_CURRENT_SOURCE_DIR}
	${sk_renderer_SOURCE_DIR}/example/imgui_backend/imgui.hlsl
)
skshaderc_compile_headers(imgui_example ${CMAKE_CURRENT_BINARY_DIR}/imgui_shader
	"-o3 -e ${_imgui_shader_target}"
	"${_imgui_hlsl}"
)

###############################################################################
# cimgui dependency
###############################################################################

# cimgui C bindings for ImGui
message(STATUS "Fetching Dear ImGui...")
FetchContent_Declare(
	cimgui
	GIT_REPOSITORY https://github.com/cimgui/cimgui.git
	GIT_TAG        1.91.6
	GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cimgui)
target_sources(imgui_example PRIVATE
	${sk_renderer_SOURCE_DIR}/example/imgui_backend/imgui_impl_sk_renderer.cpp
	imgui_impl_sk_app.cpp
	${cimgui_SOURCE_DIR}/imgui/imgui.cpp
	${cimgui_SOURCE_DIR}/imgui/imgui_draw.cpp
	${cimgui_SOURCE_DIR}/imgui/imgui_tables.cpp
	${cimgui_SOURCE_DIR}/imgui/imgui_widgets.cpp
	${cimgui_SOURCE_DIR}/imgui/imgui_demo.cpp
	${cimgui_SOURCE_DIR}/cimgui.cpp
)
target_include_directories(imgui_example PRIVATE
	${cimgui_SOURCE_DIR}
	${cimgui_SOURCE_DIR}/imgui
)

message(STATUS "Building Dear ImGui example with sk_app backend")
