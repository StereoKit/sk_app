//
// sk_app + sk_renderer + Dear ImGui Example
//
// Demonstrates Dear ImGui integration with sk_app and sk_renderer

#include <sk_app.h>
#include <sk_renderer.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "imgui_impl_sk_app.h"
#include "imgui_impl_sk_renderer.h"

// Window geometry for persistence
typedef struct window_geometry_t {
	int32_t x, y, w, h;
	uint32_t magic; // Simple validation
} window_geometry_t;

#define WINDOW_GEOMETRY_MAGIC 0x57494E47 // "WING"

int32_t main(int argc, char** argv) {
	(void)argc;
	(void)argv;

	printf("sk_app + sk_renderer + Dear ImGui Example\n");
	printf("==========================================\n\n");

	// Initialize sk_app
	if (!ska_init()) {
		fprintf(stderr, "Failed to initialize sk_app: %s\n", ska_error_get());
		return 1;
	}

	printf("[INIT] sk_app initialized\n");

	// Set up persistent storage
	ska_kvpstore_set_app_name("imgui_example");

	// Try to load saved window geometry
	int32_t win_x = SKA_WINDOWPOS_CENTERED;
	int32_t win_y = SKA_WINDOWPOS_CENTERED;
	int32_t win_w = 1280;
	int32_t win_h = 720;

	window_geometry_t saved_geom = {0};
	size_t geom_size = 0;
	if (ska_kvpstore_load("window_geometry", &saved_geom, sizeof(saved_geom), &geom_size)) {
		if (geom_size == sizeof(saved_geom) && saved_geom.magic == WINDOW_GEOMETRY_MAGIC) {
			win_x = saved_geom.x;
			win_y = saved_geom.y;
			win_w = saved_geom.w;
			win_h = saved_geom.h;
			printf("[KVPSTORE] Restored window geometry: %dx%d at (%d, %d)\n", win_w, win_h, win_x, win_y);
		}
	}

	// Create window with saved or default geometry
	ska_window_t* window = ska_window_create(
		"Dear ImGui + sk_app + sk_renderer",
		win_x, win_y,
		win_w, win_h,
		ska_window_resizable
	);

	if (!window) {
		fprintf(stderr, "Failed to create window: %s\n", ska_error_get());
		ska_shutdown();
		return 1;
	}

	printf("[WINDOW] Window created\n");

	// Get required Vulkan instance extensions
	uint32_t     ext_count  = 0;
	const char** extensions = ska_vk_get_instance_extensions(&ext_count);
	if (!extensions) {
		fprintf(stderr, "Failed to get Vulkan extensions\n");
		ska_window_destroy(window);
		ska_shutdown();
		return 1;
	}

	// Initialize sk_renderer
	skr_settings_t settings = {
		.app_name                 = "imgui_sk_app_example",
		.app_version              = 1,
		.enable_validation        = true,
		.required_extensions      = extensions,
		.required_extension_count = ext_count,
	};

	if (!skr_init(settings)) {
		skr_log(skr_log_critical, "Failed to initialize sk_renderer!");
		ska_window_destroy(window);
		ska_shutdown();
		return 1;
	}

	printf("[RENDERER] sk_renderer initialized\n");

	// Create Vulkan surface
	VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
	if (!ska_vk_create_surface(window, skr_get_vk_instance(), &vk_surface)) {
		fprintf(stderr, "Failed to create Vulkan surface\n");
		skr_shutdown();
		ska_window_destroy(window);
		ska_shutdown();
		return 1;
	}

	printf("[VULKAN] Surface created\n");

	// Create sk_renderer surface
	skr_surface_t surface = {0};
	if (skr_surface_create(vk_surface, &surface) != skr_err_success || surface.surface == VK_NULL_HANDLE) {
		skr_log(skr_log_critical, "Failed to create sk_renderer surface!");
		vkDestroySurfaceKHR(skr_get_vk_instance(), vk_surface, NULL);
		skr_shutdown();
		ska_window_destroy(window);
		ska_shutdown();
		return 1;
	}

	printf("[RENDERER] Surface created\n");

	// Initialize Dear ImGui
	igCreateContext(NULL);
	ImGuiIO* io = igGetIO();
	io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable keyboard controls

	// Initialize ImGui backends
	if (!ImGui_ImplSkApp_Init(window)) {
		fprintf(stderr, "Failed to initialize ImGui sk_app backend\n");
		igDestroyContext(NULL);
		skr_surface_destroy(&surface);
		skr_shutdown();
		ska_window_destroy(window);
		ska_shutdown();
		return 1;
	}

	// Scale UI for high-DPI displays
	float dpi_scale = ImGui_ImplSkApp_GetDpiScale();
	printf("[DPI] Scale factor: %.2f\n", dpi_scale);
	if (dpi_scale != 1.0f) {
		// Scale fonts (rebuild at larger size for crisp text)
		ImFontConfig* font_config = ImFontConfig_ImFontConfig();
		font_config->SizePixels = 13.0f * dpi_scale;
		ImFontAtlas_AddFontDefault(io->Fonts, font_config);
		ImFontConfig_destroy(font_config);

		// Scale UI element sizes (padding, borders, etc.)
		ImGuiStyle_ScaleAllSizes(igGetStyle(), dpi_scale);
	}

	if (!ImGui_ImplSkRenderer_Init()) {
		fprintf(stderr, "Failed to initialize ImGui sk_renderer backend\n");
		ImGui_ImplSkApp_Shutdown();
		igDestroyContext(NULL);
		skr_surface_destroy(&surface);
		skr_shutdown();
		ska_window_destroy(window);
		ska_shutdown();
		return 1;
	}

	printf("[IMGUI] Dear ImGui initialized\n");
	printf("\n[CONTROLS] ESC to exit\n");
	printf("[INFO] This demo shows the Dear ImGui demo window\n\n");

	// Demo state
	bool show_demo_window = true;
	bool show_another_window = false;
	ImVec4 clear_color = {0.1f, 0.1f, 0.2f, 1.0f};
	float f = 0.0f;
	int32_t counter = 0;
	static char text_buffer[4096] = "Test copy/paste here.\nTry Ctrl+C, Ctrl+V, Ctrl+X, Ctrl+A.\n\nMultiple lines supported.";

	// Main rendering loop
	uint32_t frame   = 0;
	bool     running = true;
	while (running) {
		ska_event_t event;

		// Process events
		while (ska_event_poll(&event)) {
			// Pass event to ImGui first
			ImGui_ImplSkApp_ProcessEvent(&event);

			switch (event.type) {
				case ska_event_quit:
				case ska_event_window_close:
					printf("[EVENT] Quit requested\n");
					running = false;
					break;

				case ska_event_window_resized:
					printf("[EVENT] Window resized to %dx%d\n", event.window.data1, event.window.data2);
					skr_surface_resize(&surface);
					break;

				case ska_event_key_down:
					if (event.keyboard.scancode == ska_scancode_escape) {
						running = false;
					}
					break;

				default:
					break;
			}
		}

		// Start ImGui frame
		ImGui_ImplSkRenderer_NewFrame();
		ImGui_ImplSkApp_NewFrame();
		igNewFrame();

		// 1. Show the Dear ImGui demo window
		if (show_demo_window) {
			igShowDemoWindow(&show_demo_window);
		}

		// 2. Show a simple custom window
		{
			igBegin("Hello from sk_app!", NULL, 0);
			igText("This is Dear ImGui running on:");
			igBulletText("sk_app for windowing/input");
			igBulletText("sk_renderer for Vulkan rendering");
			igSeparator();

			if (igButton("Demo Window", (ImVec2){0, 0})) {
				show_demo_window = !show_demo_window;
			}
			igSameLine(0, -1);
			if (igButton("Another Window", (ImVec2){0, 0})) {
				show_another_window = !show_another_window;
			}

			igSliderFloat("Float", &f, 0.0f, 1.0f, "%.3f", 0);
			igColorEdit3("Clear color", (float*)&clear_color, 0);

			if (igButton("Counter Button", (ImVec2){0, 0})) {
				counter++;
			}
			igSameLine(0, -1);
			igText("counter = %d", counter);

			igText("Application average %.3f ms/frame (%.1f FPS)",
				   1000.0f / io->Framerate, io->Framerate);

			igSeparator();
			igText("Clipboard Test:");
			igInputTextMultiline("##clipboard_test", text_buffer, sizeof(text_buffer),
				(ImVec2){-1, 100}, ImGuiInputTextFlags_AllowTabInput, NULL, NULL);

			igEnd();
		}

		// 3. Show another simple window
		if (show_another_window) {
			igBegin("Another Window", &show_another_window, 0);
			igText("Hello from another window!");
			if (igButton("Close Me", (ImVec2){0, 0})) {
				show_another_window = false;
			}
			igEnd();
		}

		// Finalize ImGui rendering
		igRender();

		// Begin rendering frame
		skr_renderer_frame_begin();

		// Get next swapchain image
		skr_tex_t*   render_target  = NULL;
		skr_acquire_ acquire_result = skr_surface_next_tex(&surface, &render_target);

		if (acquire_result == skr_acquire_success && render_target) {
			// Upload ImGui mesh data (must be outside render pass)
			ImGui_ImplSkRenderer_PrepareDrawData();

			// Begin render pass
			skr_vec4_t skr_clear_color = {
				clear_color.x,
				clear_color.y,
				clear_color.z,
				clear_color.w
			};

			skr_renderer_begin_pass(
				render_target,
				NULL,
				NULL,
				skr_clear_all,
				skr_clear_color,
				1.0f,
				0
			);

			// Set viewport
			skr_vec2i_t size = skr_surface_get_size(&surface);
			skr_renderer_set_viewport((skr_rect_t ){0, 0, (float)size.x, (float)size.y});
			skr_renderer_set_scissor ((skr_recti_t){0, 0, size.x, size.y});

			// Render ImGui
			ImGui_ImplSkRenderer_RenderDrawData(size.x, size.y);

			// End render pass
			skr_renderer_end_pass();

			// End frame with surface synchronization
			skr_surface_t* surfaces[] = {&surface};
			skr_renderer_frame_end(surfaces, 1);

			// Present
			skr_surface_present(&surface);

			// Progress indicator
			frame++;
		} else {
			// Failed to acquire swapchain image
			skr_renderer_frame_end(NULL, 0);

			if (acquire_result == skr_acquire_needs_resize) {
				skr_surface_resize(&surface);
			} else if (acquire_result != skr_acquire_success) {
				ska_time_sleep(16);
			}
		}
	}

	// Save window geometry before cleanup
	{
		int32_t x, y, w, h;
		ska_window_get_frame_position(window, &x, &y);
		ska_window_get_content_size(window, &w, &h);

		window_geometry_t geom = {
			.x = x,
			.y = y,
			.w = w,
			.h = h,
			.magic = WINDOW_GEOMETRY_MAGIC
		};

		if (ska_kvpstore_save("window_geometry", &geom, sizeof(geom))) {
			printf("[KVPSTORE] Saved window geometry: %dx%d at (%d, %d)\n", w, h, x, y);
		}
	}

	// Cleanup
	printf("\n[CLEANUP] Shutting down...\n");

	vkDeviceWaitIdle(skr_get_vk_device());

	ImGui_ImplSkRenderer_Shutdown();
	ImGui_ImplSkApp_Shutdown();
	igDestroyContext(NULL);

	printf("[CLEANUP] ImGui shutdown complete\n");

	skr_surface_destroy(&surface);
	skr_shutdown();

	printf("[CLEANUP] sk_renderer shutdown complete\n");

	ska_window_destroy(window);
	ska_shutdown();

	printf("[CLEANUP] sk_app shutdown complete\n");
	printf("\nTotal frames rendered: %u\n", frame);

	return 0;
}
