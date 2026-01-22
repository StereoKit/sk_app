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

###############################################################################
# sk_renderer dependency
###############################################################################

message(STATUS "Fetching sk_renderer...")

# Disable sk_renderer's example app and shader compiler to avoid build complexity
set(SKR_BUILD_EXAMPLES OFF CACHE BOOL "Disable sk_renderer examples" FORCE)
set(SKR_BUILD_SKSHADERC OFF CACHE BOOL "Disable shader compilation" FORCE)
FetchContent_Declare(
	sk_renderer
	GIT_REPOSITORY https://github.com/StereoKit/sk_renderer.git
	GIT_TAG        main
	#GIT_SHALLOW   TRUE
)
FetchContent_MakeAvailable(sk_renderer)
target_link_libraries     (imgui_example PRIVATE sk_renderer)
target_include_directories(imgui_example PRIVATE
	${sk_renderer_SOURCE_DIR}/example/imgui_backend
)
skr_copy_moltenvk(imgui_example)

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
