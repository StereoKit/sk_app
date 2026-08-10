//
// sk_app + sk_renderer - Spinning Cube Example
//
// Demonstrates integration with sk_renderer for Vulkan-based rendering,
// with a simple spinning cube using custom shaders.

#include <sk_app.h>
#include <sk_renderer.h>
#include <float_math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

///////////////////////////////////////////////////////////////////////////////
// System buffer (matches common.hlsli)
///////////////////////////////////////////////////////////////////////////////

#define SU_MAX_VIEWS 6
typedef struct {
	float4x4 view          [SU_MAX_VIEWS];
	float4x4 view_inv      [SU_MAX_VIEWS];
	float4x4 projection    [SU_MAX_VIEWS];
	float4x4 projection_inv[SU_MAX_VIEWS];
	float4x4 viewproj      [SU_MAX_VIEWS];
	float4   cam_pos       [SU_MAX_VIEWS];
	float4   cam_dir       [SU_MAX_VIEWS];
	float4   cubemap_info;
	float    time;
	uint32_t view_count;
	uint32_t _pad[2];
} system_buffer_t;

///////////////////////////////////////////////////////////////////////////////
// Cube mesh creation
///////////////////////////////////////////////////////////////////////////////

typedef struct {
	skr_vec3_t position;
	skr_vec3_t normal;
	skr_vec2_t uv;
	uint32_t   color;
} vertex_t;

static uint32_t color_to_u32(skr_vec4_t c) {
	return ((uint32_t)(c.x * 255) <<  0) |
	       ((uint32_t)(c.y * 255) <<  8) |
	       ((uint32_t)(c.z * 255) << 16) |
	       ((uint32_t)(c.w * 255) << 24);
}

// Global vertex type (must persist for the lifetime of meshes using it)
static skr_vert_type_t g_vertex_type = {0};

static skr_mesh_t create_cube_mesh(float size, const skr_vec4_t face_colors[6]) {
	float h = size * 0.5f;
	skr_vec4_t white = {1, 1, 1, 1};
	vertex_t verts[24];
	uint32_t idx[36];

	for (int32_t f = 0; f < 6; f++) {
		int32_t a = f / 2;                  // axis: 0=X, 1=Y, 2=Z
		float   n = 1 - (f & 1) * 2;        // normal sign: +1 or -1
		uint32_t col = color_to_u32(face_colors ? face_colors[f] : white);

		for (int32_t c = 0; c < 4; c++) {
			int32_t u = ((c + 1) >> 1) & 1, v = c >> 1;  // UV corners: (0,0),(1,0),(1,1),(0,1)
			float p[3] = {0}, nm[3] = {0};
			p[a] = n * h;
			p[(a + 1) % 3] = n * (u * 2 - 1) * h;        // flip with normal for CCW winding
			p[(a + 2) % 3] = (v * 2 - 1) * h;
			nm[a] = n;
			verts[f * 4 + c] = (vertex_t){{p[0], p[1], p[2]}, {nm[0], nm[1], nm[2]}, {u, v}, col};
		}

		int32_t b = f * 4, i = f * 6;
		idx[i  ] = b; idx[i+1] = b+1; idx[i+2] = b+2;
		idx[i+3] = b; idx[i+4] = b+2; idx[i+5] = b+3;
	}

	skr_mesh_t mesh = {0};
	skr_mesh_create(&g_vertex_type, skr_index_fmt_u32, verts, 24, idx, 36, &mesh);
	return mesh;
}

///////////////////////////////////////////////////////////////////////////////
// Checkerboard texture creation
///////////////////////////////////////////////////////////////////////////////

static skr_tex_t create_checkerboard_texture(int32_t resolution, int32_t square_size, uint32_t color1, uint32_t color2) {
	uint32_t* pixels = (uint32_t*)malloc(resolution * resolution * sizeof(uint32_t));

	for (int32_t y = 0; y < resolution; y++) {
		for (int32_t x = 0; x < resolution; x++) {
			bool check = ((x / square_size) + (y / square_size)) % 2 == 0;
			pixels[y * resolution + x] = check ? color1 : color2;
		}
	}

	skr_tex_t tex = {0};
	skr_tex_sampler_t sampler = {skr_tex_sample_linear, skr_tex_address_clamp};
	skr_tex_create(skr_tex_fmt_rgba32_srgb, 0, sampler, (skr_vec3i_t){resolution, resolution, 1}, 1, 1, &(skr_tex_data_t){.data = pixels, .mip_count = 1, .layer_count = 1}, &tex); // No mips - checkerboard averages poorly

	free(pixels);
	return tex;
}

///////////////////////////////////////////////////////////////////////////////
// Shader loading
///////////////////////////////////////////////////////////////////////////////

static skr_shader_t load_shader(const char* filename) {
	void*  data = NULL;
	size_t size = 0;

	if (!ska_asset_read(filename, &data, &size)) {
		fprintf(stderr, "Failed to load shader file: %s\n", filename);
		return (skr_shader_t){0};
	}

	skr_shader_t shader = {0};
	skr_shader_create(data, (uint32_t)size, &shader);
	free(data);

	return shader;
}

///////////////////////////////////////////////////////////////////////////////
// Depth buffer helper
///////////////////////////////////////////////////////////////////////////////

static void recreate_depth_buffer(skr_tex_t* depth_buffer, skr_surface_t* surface) {
	if (skr_tex_is_valid(depth_buffer)) {
		skr_tex_destroy(depth_buffer);
	}
	skr_vec2i_t       size       = skr_surface_get_size(surface);
	skr_tex_sampler_t no_sampler = {skr_tex_sample_point, skr_tex_address_clamp};
	skr_tex_create(skr_tex_fmt_depth32, skr_tex_flags_writeable, no_sampler, (skr_vec3i_t){size.x, size.y, 1}, 1, 1, NULL, depth_buffer);
}

///////////////////////////////////////////////////////////////////////////////
// Main
///////////////////////////////////////////////////////////////////////////////

int32_t main(int32_t argc, char** argv) {
	(void)argc;
	(void)argv;

	printf("sk_app + sk_renderer Cube Example\n");
	printf("==================================\n\n");

	// All resources declared upfront - zero-init means safe to destroy even if unused
	int32_t           result        = 1;
	ska_window_t*     window        = NULL;
	skr_surface_t     surface       = {0};
	skr_tex_t         depth_buffer  = {0};
	skr_mesh_t        cube_mesh     = {0};
	skr_tex_t         cube_texture  = {0};
	skr_shader_t      cube_shader   = {0};
	skr_material_t    cube_material = {0};
	skr_render_list_t render_list   = {0};

	// Initialize sk_app
	if (!ska_init(NULL)) {
		fprintf(stderr, "Failed to initialize sk_app: %s\n", ska_error_get());
		return 1;  // Can't goto cleanup - ska not initialized
	}
	ska_set_cwd(NULL);
	printf("[INIT] sk_app initialized\n");

	// Create window
	// The drawable size is the real pixel size, so this renders at native
	// resolution.
	window = ska_window_create("Spinning Cube", SKA_WINDOWPOS_CENTERED, SKA_WINDOWPOS_CENTERED, 1280, 720,
		ska_window_resizable);
	if (!window) { fprintf(stderr, "Failed to create window: %s\n", ska_error_get()); goto cleanup; }
	printf("[WINDOW] Window created\n");

	// Get required Vulkan instance extensions
	uint32_t     ext_count  = 0;
	const char** extensions = ska_vk_get_instance_extensions(&ext_count);
	if (!extensions) { fprintf(stderr, "Failed to get Vulkan extensions\n"); goto cleanup; }

	// Initialize sk_renderer
	skr_settings_t settings = {
		.app_name                 = "sk_app_renderer_example",
		.app_version              = 1,
		.enable_validation        = true,
		.required_extensions      = extensions,
		.required_extension_count = ext_count,
	};
	if (!skr_init(settings)) { skr_log(skr_log_critical, "Failed to initialize sk_renderer!"); goto cleanup; }
	printf("[RENDERER] sk_renderer initialized\n");

	// Create Vulkan surface
	VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
	if (!ska_vk_create_surface(window, skr_get_vk_instance(), &vk_surface)) { fprintf(stderr, "Failed to create Vulkan surface\n"); goto cleanup; }
	printf("[VULKAN] Surface created\n");

	// Wayland surfaces report no size of their own, so the swapchain is built
	// from this. Harmless on X11, where the surface reports a real extent and
	// this is ignored.
	skr_vec2i_t drawable = {0};
	ska_window_get_drawable_size(window, &drawable.x, &drawable.y);

	// Create sk_renderer surface
	if (skr_surface_create(vk_surface, drawable, &surface) != skr_err_success || surface.surface == VK_NULL_HANDLE) {
		skr_log(skr_log_critical, "Failed to create sk_renderer surface!");
		vkDestroySurfaceKHR(skr_get_vk_instance(), vk_surface, NULL);
		goto cleanup;
	}
	printf("[RENDERER] Surface created\n");

	// Create depth buffer
	recreate_depth_buffer(&depth_buffer, &surface);

	// Initialize vertex type
	skr_vert_component_t vert_components[] = {
		{skr_vertex_fmt_f32,             3, skr_semantic_position, 0, 0},
		{skr_vertex_fmt_f32,             3, skr_semantic_normal,   0, 0},
		{skr_vertex_fmt_f32,             2, skr_semantic_texcoord, 0, 0},
		{skr_vertex_fmt_ui8_normalized,  4, skr_semantic_color,    0, 0},
	};
	skr_vert_type_create(vert_components, 4, &g_vertex_type);

	// Create cube mesh with per-face colors (order: +X, -X, +Y, -Y, +Z, -Z)
	skr_vec4_t face_colors[6] = {
		{1.0f, 0.3f, 1.0f, 1.0f}, // +X Right  - Magenta
		{0.3f, 1.0f, 1.0f, 1.0f}, // -X Left   - Cyan
		{0.3f, 0.3f, 1.0f, 1.0f}, // +Y Top    - Blue
		{1.0f, 1.0f, 0.3f, 1.0f}, // -Y Bottom - Yellow
		{1.0f, 0.3f, 0.3f, 1.0f}, // +Z Front  - Red
		{0.3f, 1.0f, 0.3f, 1.0f}, // -Z Back   - Green
	};
	cube_mesh = create_cube_mesh(1.0f, face_colors);

	// Create checkerboard texture
	cube_texture = create_checkerboard_texture(256, 32, 0xFF404040, 0xFFFFFFFF);

	// Load shader
	cube_shader = load_shader("shaders/default.hlsl.sks");
	if (!skr_shader_is_valid(&cube_shader)) { fprintf(stderr, "Failed to load shader!\n"); goto cleanup; }

	// Create material
	skr_material_create((skr_material_info_t){
		.shader     = &cube_shader,
		.write_mask = skr_write_default,
		.depth_test = skr_compare_less,
	}, &cube_material);
	skr_material_set_tex(&cube_material, "tex", &cube_texture);

	// Create render list for batching draw calls
	skr_render_list_create(&render_list);

	printf("[SCENE] Cube mesh, texture, and material created\n");
	printf("\n[CONTROLS] ESC to exit, drag to rotate camera, scroll to zoom\n\n");

	// Camera state
	float  camera_yaw      = 0.4f;
	float  camera_pitch    = 0.3f;
	float  camera_distance = 5.0f;
	float3 camera_target   = {0.0f, 0.0f, 0.0f};
	bool   dragging        = false;
	int32_t last_mouse_x   = 0;
	int32_t last_mouse_y   = 0;

	// Main rendering loop
	uint32_t frame        = 0;
	double   start_time   = ska_time_get_elapsed_s();
	bool     running      = true;

	while (running) {
		ska_event_t event;

		// Process events
		while (ska_event_poll(&event)) {
			switch (event.type) {
				case ska_event_quit:
				case ska_event_window_close:
					printf("[EVENT] Quit requested\n");
					running = false;
					break;

				case ska_event_window_hidden:
					// Native window destroyed (screen off, backgrounded).
					// The VkSurfaceKHR is now invalid — stop rendering.
					printf("[EVENT] Window hidden — suspending rendering\n");
					if (skr_get_vk_device()) vkDeviceWaitIdle(skr_get_vk_device());
					skr_tex_destroy(&depth_buffer);
					skr_surface_destroy(&surface);
					break;

				case ska_event_window_shown:
					// New native window available — recreate Vulkan surface.
					// Skip the initial shown event: the surface was already
					// created during startup.
					if (skr_surface_is_valid(&surface)) break;
					printf("[EVENT] Window shown — recreating surface\n");
					vk_surface = VK_NULL_HANDLE;
					if (!ska_vk_create_surface(window, skr_get_vk_instance(), &vk_surface)) {
						fprintf(stderr, "Failed to recreate Vulkan surface\n");
						running = false;
						break;
					}
					skr_vec2i_t shown_size = {0};
					ska_window_get_drawable_size(window, &shown_size.x, &shown_size.y);
					if (skr_surface_create(vk_surface, shown_size, &surface) != skr_err_success) {
						fprintf(stderr, "Failed to recreate skr_surface\n");
						vkDestroySurfaceKHR(skr_get_vk_instance(), vk_surface, NULL);
						running = false;
						break;
					}
					recreate_depth_buffer(&depth_buffer, &surface);
					break;

				case ska_event_window_resized:
					printf("[EVENT] Window resized to %dx%d\n", event.window.data1, event.window.data2);
					break;

				case ska_event_key_down:
					if (event.keyboard.scancode == ska_scancode_escape) {
						running = false;
					}
					break;

				case ska_event_mouse_button_down:
					if (event.mouse_button.button == ska_mouse_button_left) {
						dragging = true;
						last_mouse_x = event.mouse_button.x;
						last_mouse_y = event.mouse_button.y;
					}
					break;

				case ska_event_mouse_button_up:
					if (event.mouse_button.button == ska_mouse_button_left) {
						dragging = false;
					}
					break;

				case ska_event_mouse_motion:
					if (dragging) {
						int32_t dx = event.mouse_motion.x - last_mouse_x;
						int32_t dy = event.mouse_motion.y - last_mouse_y;
						camera_yaw   -= dx * 0.005f;
						camera_pitch += dy * 0.005f;
						// Clamp pitch to avoid gimbal lock
						if (camera_pitch >  1.5f) camera_pitch =  1.5f;
						if (camera_pitch < -1.5f) camera_pitch = -1.5f;
						last_mouse_x = event.mouse_motion.x;
						last_mouse_y = event.mouse_motion.y;
					}
					break;

				case ska_event_mouse_wheel:
					camera_distance -= event.mouse_wheel.y * 0.5f;
					if (camera_distance <  1.5f) camera_distance =  1.5f;
					if (camera_distance > 20.0f) camera_distance = 20.0f;
					break;

				default:
					break;
			}
		}

		// Skip rendering when the native window is gone (screen off, etc.)
		if (!skr_surface_is_valid(&surface)) {
			ska_time_sleep(16);
			continue;
		}

		// Calculate time for animation
		double current_time = ska_time_get_elapsed_s();
		float  time         = (float)(current_time - start_time);

		// Calculate camera position from spherical coordinates
		float cos_pitch = cosf(camera_pitch);
		float sin_pitch = sinf(camera_pitch);
		float cos_yaw   = cosf(camera_yaw);
		float sin_yaw   = sinf(camera_yaw);

		float3 cam_position = {
			camera_target.x + camera_distance * cos_pitch * sin_yaw,
			camera_target.y + camera_distance * sin_pitch,
			camera_target.z + camera_distance * cos_pitch * cos_yaw
		};

		// Set up view and projection matrices. The drawable size is the
		// authority; the surface converges to it at acquire below.
		skr_vec2i_t size = {0};
		ska_window_get_drawable_size(window, &size.x, &size.y);
		float       aspect     = (float)size.x / (float)size.y;
		float4x4    projection = float4x4_perspective(60.0f * 3.14159265359f / 180.0f, aspect, 0.1f, 100.0f);
		float4x4    view       = float4x4_lookat(cam_position, camera_target, (float3){0.0f, 1.0f, 0.0f});
		float3      cam_dir    = float3_norm(float3_sub(camera_target, cam_position));

		// Set up system buffer for shaders
		system_buffer_t sys_buffer = {0};
		sys_buffer.view_count        = 1;
		sys_buffer.time              = time;
		sys_buffer.view          [0] = view;
		sys_buffer.projection    [0] = projection;
		sys_buffer.viewproj      [0] = float4x4_mul(projection, view);
		sys_buffer.view_inv      [0] = float4x4_invert(view);
		sys_buffer.projection_inv[0] = float4x4_invert(projection);
		sys_buffer.cam_pos       [0] = (float4){cam_position.x, cam_position.y, cam_position.z, 0.0f};
		sys_buffer.cam_dir       [0] = (float4){cam_dir.x,      cam_dir.y,      cam_dir.z,      0.0f};

		// Create spinning cube transform
		float4 rotation = float4_quat_from_euler((float3){time * 0.5f, time * 0.7f, time * 0.3f});
		float4x4 world  = float4x4_trs(
			(float3){0.0f, 0.0f, 0.0f},
			(float4){0,0,0,1},//rotation,
			(float3){1.5f, 1.5f, 1.5f}
		);

		// Add cube to render list
		skr_render_list_add(&render_list, &cube_mesh, &cube_material, &world, sizeof(float4x4), 1);

		// Begin rendering
		skr_renderer_frame_begin();

		// Get next swapchain image. Resizing right away and retrying keeps
		// frames flowing through an interactive drag, which delivers a new
		// size every frame.
		skr_tex_t*   render_target  = NULL;
		skr_acquire_ acquire_result = skr_surface_next_tex(&surface, size, &render_target);
		if (acquire_result == skr_acquire_needs_resize) {
			skr_surface_resize(&surface, size);
			recreate_depth_buffer(&depth_buffer, &surface);
			printf("[RESIZE] surface -> %dx%d\n", size.x, size.y);
			acquire_result = skr_surface_next_tex(&surface, size, &render_target);
		}

		if (acquire_result == skr_acquire_success && render_target) {
			// Set up and submit render pass
			skr_pass_t pass = {
				.color            = render_target,
				.depth            = &depth_buffer,
				.clear            = skr_clear_all,
				.clear_color      = {0.1f, 0.1f, 0.2f, 1.0f},
				.clear_depth      = 1.0f,
				.viewport         = {0, 0, (float)size.x, (float)size.y},
				.scissor          = {0, 0, size.x, size.y},
				.view_count       = sys_buffer.view_count,
				.views_correlated = true,
			};
			skr_pass_add_draw(&pass, &render_list, &sys_buffer, sizeof(system_buffer_t));
			skr_pass_submit(&pass);

			// End frame with surface synchronization
			skr_surface_t* surfaces[] = {&surface};
			skr_renderer_frame_end(surfaces, 1);

			// Present
			skr_surface_present(&surface);

			// Progress indicator
			frame++;
			if (frame % 120 == 0) {
				printf("[RENDER] Frame %u (%.1f FPS)\n", frame, frame / time);
			}
		} else {
			// Failed to acquire swapchain image (minimized, or an error)
			skr_renderer_frame_end(NULL, 0);
			ska_time_sleep(16);
		}

		// Clear render list for next frame
		skr_render_list_clear(&render_list);
	}
	result = 0;

cleanup:
	printf("\n[CLEANUP] Shutting down...\n");

	if (skr_get_vk_device()) vkDeviceWaitIdle(skr_get_vk_device());

	skr_render_list_destroy(&render_list);
	skr_material_destroy(&cube_material);
	skr_shader_destroy(&cube_shader);
	skr_tex_destroy(&cube_texture);
	skr_mesh_destroy(&cube_mesh);
	skr_vert_type_destroy(&g_vertex_type);
	skr_tex_destroy(&depth_buffer);
	skr_surface_destroy(&surface);
	skr_shutdown();

	ska_window_destroy(window);
	ska_shutdown();

	if (result == 0) printf("\nTotal frames rendered: %u\n", frame);
	return result;
}
