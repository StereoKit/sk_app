# Renderer Example (requires Vulkan and sk_renderer)

###############################################################################
# App definition
###############################################################################

add_skapp(renderer_example
	PACKAGE_NAME net.stereokit.renderer_example
	APP_NAME "Renderer Example"
)
skr_copy_moltenvk         (renderer_example)
target_skapp_assets       (renderer_example ${CMAKE_CURRENT_SOURCE_DIR}/assets)
target_sources            (renderer_example PRIVATE renderer_example.c)
target_link_libraries     (renderer_example PRIVATE sk_renderer)
target_include_directories(renderer_example PRIVATE
	${sk_renderer_SOURCE_DIR}/example/tools
)

if(UNIX AND NOT APPLE)
	target_link_libraries(renderer_example PRIVATE m)
endif()
if(MINGW)
	target_link_options(renderer_example PRIVATE -static-libgcc -static)
endif()

###############################################################################
# Assets and shader compilation
###############################################################################

# Compile shaders to intermediate location (mirrors final structure)
skshaderc_compile_assets(renderer_example ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders "" _
	assets/shaders/default.hlsl
)

# Add source assets and compiled shaders to the app
target_skapp_assets(renderer_example ${CMAKE_CURRENT_BINARY_DIR}/generated)

message(STATUS "Building sk_renderer example")
