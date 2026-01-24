//
// sk_app - Comprehensive API Example
//
// Demonstrates all sk_app API features including:
// - Initialization and error handling
// - Window creation and management
// - Event handling (keyboard, mouse, window, text, file dialog)
// - Input state queries
// - Text input and virtual keyboard
// - File I/O utilities
// - File dialogs (platform native pickers)
// - Timing functions

#include <sk_app.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

// Callback for ska_dir_iterate demonstration
static bool dir_iterate_callback(void* context, const ska_dir_entry_t* entry) {
	int32_t* count = (int32_t*)context;
	(*count)++;
	ska_log(ska_log_info, "[DIR]   %s%s (%zu bytes)",
		entry->name,
		entry->is_dir ? "/" : "",
		entry->size);
	return true;  // Continue iteration
}

int32_t main(int argc, char** argv) {
	// Parse command line arguments
	int32_t test_frames = 0;  // 0 = disabled, >0 = exit after N frames
	for (int32_t i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-test") == 0 || strcmp(argv[i], "--test") == 0) {
			test_frames = 10;  // Default: exit after 10 frames
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			printf("Usage: %s [options]\n", argv[0]);
			printf("  -test, --test   Run in test mode (exit after 10 frames)\n");
			printf("  -h, --help      Show this help\n");
			return 0;
		}
	}

	ska_log(ska_log_info, "sk_app Comprehensive API Example");
	ska_log(ska_log_info, "=================================");
	ska_log(ska_log_info, "Version: %d.%d.%d", SKA_VERSION_MAJOR, SKA_VERSION_MINOR, SKA_VERSION_PATCH);
	ska_log(ska_log_info, "");

// ========================================================================
// WORKING DIRECTORY
// ========================================================================

	char cwd[1024];
	if (ska_get_cwd(cwd, sizeof(cwd))) {
		ska_log(ska_log_info, "[CWD] Initial: %s", cwd);
	}

	if (ska_set_cwd(NULL)) {
		ska_log(ska_log_info, "[CWD] Set to executable directory");
		if (ska_get_cwd(cwd, sizeof(cwd))) {
			ska_log(ska_log_info, "[CWD] New: %s", cwd);
		}
	} else {
		ska_log(ska_log_warn, "[CWD] Failed to set: %s", ska_error_get());
	}
	ska_log(ska_log_info, "");

// ========================================================================
// INITIALIZATION
// ========================================================================

	if (!ska_init()) {
		ska_log(ska_log_error, "Failed to initialize sk_app: %s\n", ska_error_get());
		return 1;
	}
	ska_log(ska_log_info, "[INIT] sk_app initialized successfully");

// ========================================================================
// WINDOW CREATION
// ========================================================================

	ska_window_t* window = ska_window_create(
		"sk_app - Comprehensive Example",
		SKA_WINDOWPOS_CENTERED,
		SKA_WINDOWPOS_CENTERED,
		800, 600,
		ska_window_resizable | ska_window_highdpi
	);

	if (!window) {
		ska_log(ska_log_error, "Failed to create window: %s\n", ska_error_get());
		ska_shutdown();
		return 1;
	}

	ska_log(ska_log_info, "[WINDOW] Window created successfully");
	ska_log(ska_log_info, "[WINDOW] Window ID: %u", ska_window_get_id   (window));
	ska_log(ska_log_info, "[WINDOW] Title: %s",     ska_window_get_title(window));

	// Test window ID lookup
	ska_window_id_t win_id      = ska_window_get_id (window);
	ska_window_t*   win_from_id = ska_window_from_id(win_id);
	if (win_from_id == window) {
		ska_log(ska_log_info, "[WINDOW] Window ID lookup successful");
	}

	// Get initial window properties
	int32_t win_x, win_y, win_w, win_h, draw_w, draw_h;
	ska_window_get_content_position(window, &win_x,  &win_y);
	ska_window_get_content_size    (window, &win_w,  &win_h);
	ska_window_get_drawable_size   (window, &draw_w, &draw_h);
	uint32_t flags = ska_window_get_flags(window);

	ska_log(ska_log_info, "[WINDOW] Position:     (%d,%d)", win_x,  win_y);
	ska_log(ska_log_info, "[WINDOW] Size:          %dx%d",  win_w,  win_h);
	ska_log(ska_log_info, "[WINDOW] Drawable size: %dx%d",  draw_w, draw_h);
	ska_log(ska_log_info, "[WINDOW] DPI scale:     %.2f",   ska_window_get_dpi_scale(window));
	ska_log(ska_log_info, "[WINDOW] Refresh rate:  %.2f Hz", ska_window_get_refresh_rate(window));
	ska_log(ska_log_info, "[WINDOW] Flags:         0x%08X", flags);

// ========================================================================
// KVP STORE DEMONSTRATION
// ========================================================================

	ska_log(ska_log_info, "[KVPSTORE] Testing persistent key-value storage...");
	ska_kvpstore_set_app_name("simple_window_test");

	// Test 1: Small data (4 bytes)
	{
		uint8_t small_data[] = {0xDE, 0xAD, 0xBE, 0xEF};
		if (ska_kvpstore_save("test_small", small_data, sizeof(small_data))) {
			ska_log(ska_log_info, "[KVPSTORE] Saved 4 bytes to 'test_small'");

			uint8_t loaded[4] = {0};
			size_t loaded_size = 0;
			if (ska_kvpstore_load("test_small", loaded, sizeof(loaded), &loaded_size)) {
				bool match = (loaded_size == sizeof(small_data)) &&
				             (memcmp(loaded, small_data, sizeof(small_data)) == 0);
				ska_log(match ? ska_log_info : ska_log_error,
					"[KVPSTORE] Load 'test_small': size=%zu, match=%s",
					loaded_size, match ? "YES" : "NO");
			} else {
				ska_log(ska_log_error, "[KVPSTORE] Failed to load 'test_small': %s", ska_error_get());
			}
		} else {
			ska_log(ska_log_error, "[KVPSTORE] Failed to save 'test_small': %s", ska_error_get());
		}
	}

	// Test 2: Medium data (256 bytes)
	{
		uint8_t medium_data[256];
		for (int32_t i = 0; i < 256; i++) medium_data[i] = (uint8_t)i;

		if (ska_kvpstore_save("test_medium", medium_data, sizeof(medium_data))) {
			ska_log(ska_log_info, "[KVPSTORE] Saved 256 bytes to 'test_medium'");

			uint8_t loaded[256] = {0};
			size_t loaded_size = 0;
			if (ska_kvpstore_load("test_medium", loaded, sizeof(loaded), &loaded_size)) {
				bool match = (loaded_size == sizeof(medium_data)) &&
				             (memcmp(loaded, medium_data, sizeof(medium_data)) == 0);
				ska_log(match ? ska_log_info : ska_log_error,
					"[KVPSTORE] Load 'test_medium': size=%zu, match=%s",
					loaded_size, match ? "YES" : "NO");
			} else {
				ska_log(ska_log_error, "[KVPSTORE] Failed to load 'test_medium': %s", ska_error_get());
			}
		} else {
			ska_log(ska_log_error, "[KVPSTORE] Failed to save 'test_medium': %s", ska_error_get());
		}
	}

	// Test 3: Large data (8KB)
	{
		size_t large_size = 8 * 1024;
		uint8_t* large_data = (uint8_t*)malloc(large_size);
		for (size_t i = 0; i < large_size; i++) large_data[i] = (uint8_t)(i * 7);

		if (ska_kvpstore_save("test_large", large_data, large_size)) {
			ska_log(ska_log_info, "[KVPSTORE] Saved 8KB to 'test_large'");

			// First query size only
			size_t queried_size = 0;
			if (ska_kvpstore_load("test_large", NULL, 0, &queried_size)) {
				ska_log(ska_log_info, "[KVPSTORE] Size query: %zu bytes", queried_size);
			}

			uint8_t* loaded = (uint8_t*)malloc(large_size);
			size_t loaded_size = 0;
			if (ska_kvpstore_load("test_large", loaded, large_size, &loaded_size)) {
				bool match = (loaded_size == large_size) &&
				             (memcmp(loaded, large_data, large_size) == 0);
				ska_log(match ? ska_log_info : ska_log_error,
					"[KVPSTORE] Load 'test_large': size=%zu, match=%s",
					loaded_size, match ? "YES" : "NO");
			} else {
				ska_log(ska_log_error, "[KVPSTORE] Failed to load 'test_large': %s", ska_error_get());
			}
			free(loaded);
		} else {
			ska_log(ska_log_error, "[KVPSTORE] Failed to save 'test_large': %s", ska_error_get());
		}
		free(large_data);
	}

	// Test 4: Delete
	{
		if (ska_kvpstore_delete("test_small")) {
			ska_log(ska_log_info, "[KVPSTORE] Deleted 'test_small'");

			// Verify it's gone
			size_t size = 0;
			if (!ska_kvpstore_load("test_small", NULL, 0, &size)) {
				ska_log(ska_log_info, "[KVPSTORE] Verified 'test_small' no longer exists");
			} else {
				ska_log(ska_log_error, "[KVPSTORE] 'test_small' still exists after delete!");
			}
		}
	}

	ska_log(ska_log_info, "[KVPSTORE] Tests complete\n");

// ========================================================================
// FILE I/O DEMONSTRATION
// ========================================================================

	const char* test_file = "ska_test.txt";
	const char* test_data = "Hello from sk_app!\nThis is a test file.\n";

	ska_log(ska_log_info, "\n[FILE] Testing file I/O...");

	// Write text file
	if (ska_file_write_text(test_file, test_data)) {
		ska_log(ska_log_info, "[FILE] Text file written successfully");

		// Check if file exists
		if (ska_file_exists(test_file)) {
			size_t file_size = ska_file_size(test_file);
			ska_log(ska_log_info, "[FILE] File exists, size: %zu bytes", file_size);

			// Read text file
			char* read_data = NULL;
			if (ska_file_read_text(test_file, &read_data)) {
				ska_log(ska_log_info, "[FILE] Read text: %s", read_data);
				ska_file_free_data(read_data);
			}
		}
	}

	// Write binary file
	uint8_t binary_data[] = {0xDE, 0xAD, 0xBE, 0xEF};
	if (ska_file_write("ska_test.bin", binary_data, sizeof(binary_data))) {
		ska_log(ska_log_info, "[FILE] Binary file written successfully");

		// Read binary file
		size_t read_size   = 0;
		void*  read_binary = NULL;
		if (ska_file_read("ska_test.bin", &read_binary, &read_size)) {
			ska_log(ska_log_info, "[FILE] Binary file read successfully (%zu bytes)", read_size);
			ska_file_free_data(read_binary);
		}
	}

	// Directory iteration test
	ska_log(ska_log_info, "[DIR] Testing directory iteration on current directory:");
	int32_t entry_count = 0;
	if (ska_dir_iterate(".", &entry_count, dir_iterate_callback)) {
		ska_log(ska_log_info, "[DIR] Iteration complete: %d entries found", entry_count);
	} else {
		ska_log(ska_log_warn, "[DIR] Iteration failed: %s", ska_error_get());
	}

// ========================================================================
// VULKAN EXTENSIONS (if available)
// ========================================================================

#ifdef SKA_ENABLE_VULKAN
	uint32_t     ext_count  = 0;
	const char** extensions = ska_vk_get_instance_extensions(&ext_count);
	if (extensions) {
		ska_log(ska_log_info, "\n[VULKAN] Required instance extensions (%u):", ext_count);
		for (uint32_t i = 0; i < ext_count; i++) {
			ska_log(ska_log_info, "[VULKAN]   - %s", extensions[i]);
		}
	}
#endif

// ========================================================================
// NATIVE HANDLES (platform-specific)
// ========================================================================

	ska_log(ska_log_info, "\n[NATIVE] Platform-specific handles:");
	void* native_handle = ska_window_get_native_handle(window);
	ska_log(ska_log_info, "[NATIVE] Window handle: %p", native_handle);

#ifdef SKA_PLATFORM_WIN32
	void* hinstance = ska_win32_get_hinstance();
	ska_log(ska_log_info, "[NATIVE] HINSTANCE: %p", hinstance);
#elif defined(SKA_PLATFORM_LINUX)
	void* x11_display = ska_linux_get_x11_display();
	ska_log(ska_log_info, "[NATIVE] X11 Display: %p", x11_display);
#endif

// ========================================================================
// CONTROLS DISPLAY
// ========================================================================

	ska_log(ska_log_info, "\n[CONTROLS] Available commands:");
	ska_log(ska_log_info, "  ESC       - Exit application");
	ska_log(ska_log_info, "  T         - Show virtual keyboard");
	ska_log(ska_log_info, "  M         - Maximize window");
	ska_log(ska_log_info, "  N         - Minimize window");
	ska_log(ska_log_info, "  R         - Restore window");
	ska_log(ska_log_info, "  H         - Hide window (2 seconds)");
	ska_log(ska_log_info, "  P         - Set window position");
	ska_log(ska_log_info, "  S         - Set window size");
	ska_log(ska_log_info, "  SPACE     - Rename window title");
	ska_log(ska_log_info, "  C         - Toggle cursor visibility");
	ska_log(ska_log_info, "  V         - Toggle relative mouse mode");
	ska_log(ska_log_info, "  W         - Warp mouse to center");
	ska_log(ska_log_info, "  F         - Open file dialog");
	ska_log(ska_log_info, "  G         - Save file dialog");
	ska_log(ska_log_info, "  Mouse     - Move and click");
	ska_log(ska_log_info, "  Wheel     - Scroll\n");

// ========================================================================
// FILE DIALOG AVAILABILITY CHECK
// ========================================================================

	ska_log(ska_log_info, "[FILE DIALOG] Availability:");
	ska_log(ska_log_info, "  Open file:   %s", ska_file_dialog_available(ska_file_dialog_open) ? "yes" : "no");
	ska_log(ska_log_info, "  Save file:   %s", ska_file_dialog_available(ska_file_dialog_save) ? "yes" : "no");
	ska_log(ska_log_info, "  Open folder: %s", ska_file_dialog_available(ska_file_dialog_open_folder) ? "yes" : "no");

// ========================================================================
// MAIN EVENT LOOP
// ========================================================================

	bool     running         = true;
	uint32_t frame           = 0;
	double   start_time      = ska_time_get_elapsed_s();
	bool     cursor_visible  = true;
	bool     text_input_mode = false;

	while (running) {
		ska_event_t event;

		// Process all pending events
		while (ska_event_poll(&event)) {
			switch (event.type) {
				case ska_event_quit:
					ska_log(ska_log_info, "[EVENT] Quit requested");
					running = false;
					break;

				case ska_event_window_close:
					ska_log(ska_log_info, "[EVENT] Window close requested");
					running = false;
					break;

				case ska_event_window_resized:
					ska_log(ska_log_info, "[EVENT] Window resized to %dx%d", event.window.data1, event.window.data2);
					break;

				case ska_event_window_moved:
					ska_log(ska_log_info, "[EVENT] Window moved to (%d, %d)", event.window.data1, event.window.data2);
					break;

				case ska_event_window_focus_gained:
					ska_log(ska_log_info, "[EVENT] Window gained focus");
					break;

				case ska_event_window_focus_lost:
					ska_log(ska_log_info, "[EVENT] Window lost focus");
					break;

				case ska_event_window_minimized:
					ska_log(ska_log_info, "[EVENT] Window minimized");
					break;

				case ska_event_window_maximized:
					ska_log(ska_log_info, "[EVENT] Window maximized");
					break;

				case ska_event_window_restored:
					ska_log(ska_log_info, "[EVENT] Window restored");
					break;

				case ska_event_key_down:
					if (!event.keyboard.repeat) {
						ska_log(ska_log_info, "[EVENT] Key down: scancode=%d, modifiers=0x%04X (ska_scancode_f=%d)",
							   event.keyboard.scancode, event.keyboard.modifiers, ska_scancode_f);

						// Handle specific keys
						switch (event.keyboard.scancode) {
							case ska_scancode_escape:
								if (text_input_mode) {
									ska_log(ska_log_info, "[ACTION] Hide virtual keyboard");
									ska_virtual_keyboard_show(false, ska_text_input_type_text);
									text_input_mode = false;
								} else {
									ska_log(ska_log_info, "[ACTION] Exiting...");
									running = false;
								}
								break;

							case ska_scancode_m:
								ska_log(ska_log_info, "[ACTION] Maximizing window");
								ska_window_maximize(window);
								break;

							case ska_scancode_n:
								ska_log(ska_log_info, "[ACTION] Minimizing window");
								ska_window_minimize(window);
								break;

							case ska_scancode_r:
								ska_log(ska_log_info, "[ACTION] Restoring window");
								ska_window_restore(window);
								break;

							case ska_scancode_h:
								ska_log(ska_log_info, "[ACTION] Hiding window for 2 seconds");
								ska_window_hide(window);
								ska_time_sleep(2000);
								ska_window_show(window);
								ska_window_raise(window);
								break;

							case ska_scancode_p:
								ska_log(ska_log_info, "[ACTION] Moving window content to (100, 100)");
								ska_window_set_content_position(window, 100, 100);
								break;

							case ska_scancode_s:
								ska_log(ska_log_info, "[ACTION] Resizing window content to 640x480");
								ska_window_set_content_size(window, 640, 480);
								break;

							case ska_scancode_space:
								{
									static int32_t title_count = 0;
									char new_title[64];
									snprintf(new_title, sizeof(new_title), "sk_app - Title #%d", ++title_count);
									ska_window_set_title(window, new_title);
									ska_log(ska_log_info, "[ACTION] Window title changed to: %s", new_title);
								}
								break;

							case ska_scancode_c:
								cursor_visible = !cursor_visible;
								ska_cursor_show(cursor_visible);
								ska_log(ska_log_info, "[ACTION] Cursor %s", cursor_visible ? "shown" : "hidden");
								break;

							case ska_scancode_v:
								{
									bool relative = ska_mouse_get_relative_mode();
									ska_mouse_set_relative_mode(!relative);
									ska_log(ska_log_info, "[ACTION] Relative mouse mode: %s\n", !relative ? "enabled" : "disabled");
								}
								break;

							case ska_scancode_t:
								if (!text_input_mode) {
									ska_log(ska_log_info, "[ACTION] Starting virtual keyboard (ESC to hide)");
									ska_virtual_keyboard_show(true, ska_text_input_type_text);
									text_input_mode = true;
								}
								break;

							case ska_scancode_w:
								ska_log(ska_log_info, "[ACTION] Warping mouse to center");
								ska_window_get_content_size(window, &win_w, &win_h);
								ska_mouse_warp             (window, win_w / 2, win_h / 2);
								break;

							case ska_scancode_f:
								{
									ska_log(ska_log_info, "[ACTION] Opening file dialog...");
									// Filter format: {name, mime, exts}
									// mime is used on Android, exts on desktop platforms
									// Provide both for cross-platform compatibility
									ska_file_filter_t filters[] = {
										{ "All Files",  "*/*",     "*" },
										{ "Images",     "image/*", "*.png *.jpg *.jpeg *.gif *.bmp" },
										{ "Text Files", "text/*",  "*.txt *.md *.c *.h" },
									};
									ska_file_dialog_request_t req = {
										.type           = ska_file_dialog_open,
										.title          = "Open File",
										.filters        = filters,
										.filter_count   = 3,
										.allow_multiple = true,
									};
									ska_file_dialog_id_t id = ska_file_dialog_show(&req);
									if (id) { ska_log(ska_log_info,  "[FILE DIALOG] Opened dialog id=%u", id); } 
									else    { ska_log(ska_log_error, "[FILE DIALOG] Failed: %s", ska_error_get()); }
								}
								break;

							case ska_scancode_g:
								{
									ska_log(ska_log_info, "[ACTION] Opening save dialog...");
									ska_file_filter_t filters[] = {
										{ "Text Files", "text/plain", "*.txt" },
									};
									ska_file_dialog_request_t req = {
										.type           = ska_file_dialog_save,
										.title          = "Save File",
										.default_name   = "untitled.txt",
										.filters        = filters,
										.filter_count   = 1,
										.allow_multiple = false,
									};
									ska_file_dialog_id_t id = ska_file_dialog_show(&req);
									if (id) { ska_log(ska_log_info,  "[FILE DIALOG] Opened save dialog id=%u", id); }
									else    { ska_log(ska_log_error, "[FILE DIALOG] Failed: %s", ska_error_get()); }
								}
								break;

							default:
								ska_log(ska_log_info, "[DEBUG] Unhandled scancode: %d", event.keyboard.scancode);
								break;
						}
					}
					break;

				case ska_event_key_up:
					ska_log(ska_log_info, "[EVENT] Key up: scancode=%d", event.keyboard.scancode);
					break;

				case ska_event_text_input:
					printf("[EVENT] Text input: \"%s\"\n", event.text.text);
					break;

				case ska_event_mouse_motion:
					if (frame % 120 == 0) {  // Print every 120 frames to reduce spam
						ska_log(ska_log_info, "[EVENT] Mouse motion: pos=(%d, %d), rel=(%d, %d)",
							   event.mouse_motion.x,    event.mouse_motion.y,
							   event.mouse_motion.xrel, event.mouse_motion.yrel);
					}
					break;

				case ska_event_mouse_button_down:
					ska_log(ska_log_info, "[EVENT] Mouse button down: button=%d at (%d, %d), clicks=%d",
						   event.mouse_button.button,
						   event.mouse_button.x, event.mouse_button.y,
						   event.mouse_button.clicks);
					break;

				case ska_event_mouse_button_up:
					ska_log(ska_log_info, "[EVENT] Mouse button up: button=%d\n", event.mouse_button.button);
					break;

				case ska_event_mouse_wheel:
					ska_log(ska_log_info, "[EVENT] Mouse wheel: delta=(%d, %d)\n", event.mouse_wheel.x, event.mouse_wheel.y);
					break;

				case ska_event_file_dialog:
					ska_log(ska_log_info, "[EVENT] File dialog result: id=%u, title=\"%s\", cancelled=%s, count=%d",
						   event.file_dialog.id,
						   event.file_dialog.title ? event.file_dialog.title : "(null)",
						   event.file_dialog.cancelled ? "yes" : "no",
						   event.file_dialog.count);
					if (!event.file_dialog.cancelled) {
						for (int32_t i = 0; i < event.file_dialog.count; i++) {
							const char* path = ska_file_dialog_get_path(&event.file_dialog, i);
							ska_log(ska_log_info, "[FILE DIALOG]   [%d] %s", i, path ? path : "(null)");
						}
					}
					// Free the result - important to avoid memory leaks!
					ska_file_dialog_free_result(&event.file_dialog);
					break;

				default:
					break;
			}
		}

// ====================================================================
// PERIODIC STATE QUERIES
// ====================================================================

		if (frame % 300 == 0 && frame > 0) {
			// Query mouse state
			int32_t  mouse_x, mouse_y;
			uint32_t mouse_buttons = ska_mouse_get_state(&mouse_x, &mouse_y);
			ska_log(ska_log_info, "[STATE] Mouse: pos=(%d, %d), buttons=0x%08X", mouse_x, mouse_y, mouse_buttons);

			// Query global mouse state
			int32_t global_x, global_y;
			ska_mouse_get_global_state(&global_x, &global_y);
			ska_log(ska_log_info, "[STATE] Global mouse pos: (%d, %d)", global_x, global_y);

			// Query keyboard state
			int32_t        num_keys = 0;
			const uint8_t* keyboard = ska_keyboard_get_state(&num_keys);
			ska_log(ska_log_info, "[STATE] Keyboard: %d keys total", num_keys);

			if (keyboard[ska_scancode_lctrl] || keyboard[ska_scancode_rctrl]) {
				ska_log(ska_log_info, "[STATE] Ctrl key is currently pressed");
			}
			if (keyboard[ska_scancode_lshift] || keyboard[ska_scancode_rshift]) {
				ska_log(ska_log_info, "[STATE] Shift key is currently pressed");
			}
			if (keyboard[ska_scancode_lalt] || keyboard[ska_scancode_ralt]) {
				ska_log(ska_log_info, "[STATE] Alt key is currently pressed");
			}

			// Query key modifiers
			uint16_t modifiers = ska_keyboard_get_modifiers();
			if (modifiers != 0) {
				ska_log(ska_log_info, "[STATE] Active modifiers: 0x%04X", modifiers);
			}

			// Query text input state
			if (ska_virtual_keyboard_is_visible()) {
				ska_log(ska_log_info, "[STATE] Virtual keyboard is visible");
			}

			// Check text queue
			if (ska_text_has_input()) {
				ska_log(ska_log_info, "[STATE] Text queue has pending input");
				uint32_t codepoint = ska_text_peek();
				ska_log(ska_log_info, "[STATE] Next codepoint: U+%04X", codepoint);
			}

			// Timing information
			double elapsed = ska_time_get_elapsed_s() - start_time;
			ska_log(ska_log_info, "[TIMING] Elapsed time: %.3f s, Frame: %u", elapsed, frame);
		}

		// Simulate frame timing - ~60 FPS
		ska_time_sleep(16);
		frame++;

		// Test mode: exit after N frames
		if (test_frames > 0 && frame >= (uint32_t)test_frames) {
			ska_log(ska_log_info, "[TEST] Exiting after %d frames", test_frames);
			running = false;
		}
	}

// ========================================================================
// CLEANUP
// ========================================================================

	ska_log(ska_log_info, "\n[CLEANUP] Shutting down...");

	// Hide virtual keyboard if visible
	if (ska_virtual_keyboard_is_visible()) {
		ska_virtual_keyboard_show(false, ska_text_input_type_text);
	}

	// Reset text queue
	ska_text_reset();

	// Destroy window
	ska_window_destroy(window);
	ska_log(ska_log_info, "[CLEANUP] Window destroyed");

	// Shutdown library
	ska_shutdown();
	ska_log(ska_log_info, "[CLEANUP] sk_app shutdown complete");

	ska_log(ska_log_info, "\nTotal frames rendered: %u", frame);

	return 0;
}
