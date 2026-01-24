//
// sk_app - Test Suite
//
// Automated test suite for sk_app API. Tests all accessible functionality
// and reports pass/fail results. Some features require user interaction
// and are only exercised in interactive mode.
//
// Usage:
//   ./simple_window              # Interactive mode with all features
//   ./simple_window --test       # Automated test mode (exits after tests)
//   ./simple_window --test -v    # Verbose test output
//

#include <sk_app.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

// ============================================================================
// Test Framework
// ============================================================================

static int32_t g_tests_passed = 0;
static int32_t g_tests_failed = 0;
static int32_t g_tests_skipped = 0;
static bool    g_verbose = false;

#define TEST_PASS(name) do { \
	g_tests_passed++; \
	if (g_verbose) ska_log(ska_log_info, "[PASS] %s", name); \
} while(0)

#define TEST_FAIL(name, reason) do { \
	g_tests_failed++; \
	ska_log(ska_log_error, "[FAIL] %s: %s", name, reason); \
} while(0)

#define TEST_SKIP(name, reason) do { \
	g_tests_skipped++; \
	if (g_verbose) ska_log(ska_log_warn, "[SKIP] %s: %s", name, reason); \
} while(0)

#define TEST_ASSERT(name, condition) do { \
	if (condition) { TEST_PASS(name); } \
	else { TEST_FAIL(name, "assertion failed"); } \
} while(0)

#define TEST_ASSERT_MSG(name, condition, msg) do { \
	if (condition) { TEST_PASS(name); } \
	else { TEST_FAIL(name, msg); } \
} while(0)

// ============================================================================
// Test Categories
// ============================================================================

static void test_version(void) {
	ska_log(ska_log_info, "\n=== Version Tests ===");

	TEST_ASSERT("SKA_VERSION_MAJOR defined", SKA_VERSION_MAJOR >= 0);
	TEST_ASSERT("SKA_VERSION_MINOR defined", SKA_VERSION_MINOR >= 0);
	TEST_ASSERT("SKA_VERSION_PATCH defined", SKA_VERSION_PATCH >= 0);
}

static void test_error_handling(void) {
	ska_log(ska_log_info, "\n=== Error Handling Tests ===");

	// ska_error_get should return NULL when no error
	// Note: After successful init, error should be clear
	const char* err = ska_error_get();
	TEST_ASSERT_MSG("ska_error_get returns NULL or valid string",
		err == NULL || strlen(err) >= 0, "invalid error pointer");
}

static void test_timing(void) {
	ska_log(ska_log_info, "\n=== Timing Tests ===");

	// Test elapsed time functions
	double t1_s = ska_time_get_elapsed_s();
	uint64_t t1_ns = ska_time_get_elapsed_ns();

	TEST_ASSERT("ska_time_get_elapsed_s returns positive", t1_s >= 0.0);
	TEST_ASSERT("ska_time_get_elapsed_ns returns positive", t1_ns > 0);

	// Test sleep
	ska_time_sleep(10);
	double t2_s = ska_time_get_elapsed_s();
	uint64_t t2_ns = ska_time_get_elapsed_ns();

	TEST_ASSERT("ska_time_sleep advances time (seconds)", t2_s > t1_s);
	TEST_ASSERT("ska_time_sleep advances time (nanoseconds)", t2_ns > t1_ns);

	// Check consistency between ns and s
	double diff_s = t2_s - t1_s;
	double diff_ns_as_s = (double)(t2_ns - t1_ns) / 1000000000.0;
	double tolerance = 0.01; // 10ms tolerance
	TEST_ASSERT_MSG("time functions are consistent",
		fabs(diff_s - diff_ns_as_s) < tolerance,
		"ns and s measurements differ significantly");
}

static void test_working_directory(void) {
	ska_log(ska_log_info, "\n=== Working Directory Tests ===");

	char cwd[1024];

	// Test getting current working directory
	bool got_cwd = ska_get_cwd(cwd, sizeof(cwd));
	TEST_ASSERT("ska_get_cwd succeeds", got_cwd);
	TEST_ASSERT_MSG("ska_get_cwd returns non-empty path",
		got_cwd && strlen(cwd) > 0, "empty path returned");

	if (g_verbose && got_cwd) {
		ska_log(ska_log_info, "  Current directory: %s", cwd);
	}

	// Test with NULL path (set to executable directory)
	bool set_cwd = ska_set_cwd(NULL);
#ifdef SKA_PLATFORM_ANDROID
	// Android doesn't support changing working directory
	TEST_SKIP("ska_set_cwd(NULL)", "not supported on Android");
#else
	TEST_ASSERT("ska_set_cwd(NULL) succeeds", set_cwd);
#endif

	// Test with invalid path
	bool set_invalid = ska_set_cwd("/nonexistent/path/that/should/not/exist");
	TEST_ASSERT("ska_set_cwd with invalid path fails", !set_invalid);

	// Verify error was set
	const char* err = ska_error_get();
	TEST_ASSERT_MSG("ska_error_get returns message after failure",
		err != NULL && strlen(err) > 0, "no error message set");
}

static void test_file_io(void) {
	ska_log(ska_log_info, "\n=== File I/O Tests ===");

	const char* test_file = "ska_test_suite.txt";
	const char* test_data = "Hello from sk_app test suite!\nLine 2.\n";
	const char* binary_file = "ska_test_binary.bin";
	uint8_t binary_data[] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};

	// Test text file write
	bool wrote_text = ska_file_write_text(test_file, test_data);
	TEST_ASSERT("ska_file_write_text succeeds", wrote_text);

	// Test file exists
	if (wrote_text) {
		bool exists = ska_file_exists(test_file);
		TEST_ASSERT("ska_file_exists returns true for existing file", exists);

		// Test file size
		size_t size = ska_file_size(test_file);
		TEST_ASSERT_MSG("ska_file_size returns correct size",
			size == strlen(test_data), "size mismatch");

		// Test text file read
		char* read_data = NULL;
		bool read_ok = ska_file_read_text(test_file, &read_data);
		TEST_ASSERT("ska_file_read_text succeeds", read_ok);

		if (read_ok && read_data) {
			bool content_match = strcmp(read_data, test_data) == 0;
			TEST_ASSERT("ska_file_read_text returns correct content", content_match);
			ska_file_free_data(read_data);
		}
	}

	// Test binary file write
	bool wrote_binary = ska_file_write(binary_file, binary_data, sizeof(binary_data));
	TEST_ASSERT("ska_file_write succeeds", wrote_binary);

	if (wrote_binary) {
		// Test binary file read
		void* read_binary = NULL;
		size_t read_size = 0;
		bool read_ok = ska_file_read(binary_file, &read_binary, &read_size);
		TEST_ASSERT("ska_file_read succeeds", read_ok);
		TEST_ASSERT_MSG("ska_file_read returns correct size",
			read_size == sizeof(binary_data), "size mismatch");

		if (read_ok && read_binary) {
			bool content_match = memcmp(read_binary, binary_data, sizeof(binary_data)) == 0;
			TEST_ASSERT("ska_file_read returns correct content", content_match);
			ska_file_free_data(read_binary);
		}
	}

	// Test non-existent file
	TEST_ASSERT("ska_file_exists returns false for non-existent file",
		!ska_file_exists("nonexistent_file_12345.xyz"));

	// Test empty file write
	bool wrote_empty = ska_file_write("ska_test_empty.bin", NULL, 0);
	TEST_ASSERT("ska_file_write with size=0 succeeds", wrote_empty);
	if (wrote_empty) {
		TEST_ASSERT("empty file has size 0", ska_file_size("ska_test_empty.bin") == 0);
	}

	// Cleanup - don't fail tests on cleanup
	remove(test_file);
	remove(binary_file);
	remove("ska_test_empty.bin");
}

// Callback for directory iteration test
static bool dir_iterate_callback(void* context, const ska_dir_entry_t* entry) {
	int32_t* count = (int32_t*)context;
	(*count)++;
	return true; // Continue iteration
}

static void test_directory_iteration(void) {
	ska_log(ska_log_info, "\n=== Directory Iteration Tests ===");

	int32_t entry_count = 0;
	bool result = ska_dir_iterate(".", &entry_count, dir_iterate_callback);

	TEST_ASSERT("ska_dir_iterate on '.' succeeds", result);
	TEST_ASSERT_MSG("ska_dir_iterate finds entries", entry_count > 0,
		"no entries found in current directory");

	if (g_verbose) {
		ska_log(ska_log_info, "  Found %d entries in current directory", entry_count);
	}

	// Test invalid directory
	int32_t invalid_count = 0;
	bool invalid_result = ska_dir_iterate("/nonexistent/path", &invalid_count, dir_iterate_callback);
	TEST_ASSERT("ska_dir_iterate on invalid path fails", !invalid_result);
}

static void test_kvpstore(void) {
	ska_log(ska_log_info, "\n=== Key-Value Store Tests ===");

	ska_kvpstore_set_app_name("ska_test_suite");
	TEST_PASS("ska_kvpstore_set_app_name"); // No return value to check

	// Test small data
	uint8_t small_data[] = {0x01, 0x02, 0x03, 0x04};
	bool saved_small = ska_kvpstore_save("test_small", small_data, sizeof(small_data));
	TEST_ASSERT("ska_kvpstore_save (4 bytes) succeeds", saved_small);

	if (saved_small) {
		uint8_t loaded[4] = {0};
		size_t loaded_size = 0;
		bool loaded_ok = ska_kvpstore_load("test_small", loaded, sizeof(loaded), &loaded_size);
		TEST_ASSERT("ska_kvpstore_load succeeds", loaded_ok);
		TEST_ASSERT_MSG("ska_kvpstore_load returns correct size",
			loaded_size == sizeof(small_data), "size mismatch");
		TEST_ASSERT("ska_kvpstore_load returns correct data",
			loaded_ok && memcmp(loaded, small_data, sizeof(small_data)) == 0);
	}

	// Test size query
	size_t queried_size = 0;
	bool size_query = ska_kvpstore_load("test_small", NULL, 0, &queried_size);
	TEST_ASSERT("ska_kvpstore_load size-only query succeeds", size_query);
	TEST_ASSERT_MSG("ska_kvpstore_load size-only returns correct size",
		queried_size == sizeof(small_data), "size mismatch");

	// Test delete
	bool deleted = ska_kvpstore_delete("test_small");
	TEST_ASSERT("ska_kvpstore_delete succeeds", deleted);

	// Verify deletion
	size_t after_delete_size = 0;
	bool after_delete = ska_kvpstore_load("test_small", NULL, 0, &after_delete_size);
	TEST_ASSERT("ska_kvpstore_load after delete fails", !after_delete);

	// Test larger data (256 bytes)
	uint8_t medium_data[256];
	for (int32_t i = 0; i < 256; i++) medium_data[i] = (uint8_t)i;

	bool saved_medium = ska_kvpstore_save("test_medium", medium_data, sizeof(medium_data));
	TEST_ASSERT("ska_kvpstore_save (256 bytes) succeeds", saved_medium);

	if (saved_medium) {
		uint8_t loaded[256] = {0};
		size_t loaded_size = 0;
		bool loaded_ok = ska_kvpstore_load("test_medium", loaded, sizeof(loaded), &loaded_size);
		TEST_ASSERT("ska_kvpstore_load (256 bytes) succeeds", loaded_ok);
		TEST_ASSERT("ska_kvpstore_load (256 bytes) data matches",
			loaded_ok && memcmp(loaded, medium_data, sizeof(medium_data)) == 0);
		ska_kvpstore_delete("test_medium"); // Cleanup
	}
}

static void test_window_creation(ska_window_t** out_window) {
	ska_log(ska_log_info, "\n=== Window Creation Tests ===");

	// Test basic window creation
	ska_window_t* window = ska_window_create(
		"sk_app Test Suite",
		SKA_WINDOWPOS_CENTERED,
		SKA_WINDOWPOS_CENTERED,
		800, 600,
		ska_window_resizable | ska_window_highdpi
	);
	TEST_ASSERT("ska_window_create succeeds", window != NULL);

	if (!window) {
		ska_log(ska_log_error, "  Error: %s", ska_error_get());
		*out_window = NULL;
		return;
	}

	*out_window = window;

	// Test window ID
	ska_window_id_t id = ska_window_get_id(window);
	TEST_ASSERT("ska_window_get_id returns non-zero", id != 0);

	// Test window from ID
	ska_window_t* from_id = ska_window_from_id(id);
	TEST_ASSERT("ska_window_from_id returns same window", from_id == window);

	// Test invalid ID lookup
	ska_window_t* invalid = ska_window_from_id(99999);
	TEST_ASSERT("ska_window_from_id returns NULL for invalid ID", invalid == NULL);

	// Test window title
	const char* title = ska_window_get_title(window);
	TEST_ASSERT("ska_window_get_title returns non-NULL", title != NULL);
	TEST_ASSERT("ska_window_get_title returns correct title",
		title != NULL && strcmp(title, "sk_app Test Suite") == 0);

	// Test set title
	ska_window_set_title(window, "New Title");
	title = ska_window_get_title(window);
	TEST_ASSERT("ska_window_set_title works",
		title != NULL && strcmp(title, "New Title") == 0);
	ska_window_set_title(window, "sk_app Test Suite"); // Restore

	// Test flags
	uint32_t flags = ska_window_get_flags(window);
	TEST_ASSERT("ska_window_get_flags includes resizable",
		(flags & ska_window_resizable) != 0);
	TEST_ASSERT("ska_window_get_flags includes highdpi",
		(flags & ska_window_highdpi) != 0);
}

static void test_window_geometry(ska_window_t* window) {
	if (!window) {
		TEST_SKIP("window geometry tests", "no window available");
		return;
	}

	ska_log(ska_log_info, "\n=== Window Geometry Tests ===");

	// Test content size
	int32_t w, h;
	ska_window_get_content_size(window, &w, &h);
	TEST_ASSERT("ska_window_get_content_size returns positive width", w > 0);
	TEST_ASSERT("ska_window_get_content_size returns positive height", h > 0);

	if (g_verbose) {
		ska_log(ska_log_info, "  Content size: %dx%d", w, h);
	}

	// Test content position
	int32_t x, y;
	ska_window_get_content_position(window, &x, &y);
	TEST_PASS("ska_window_get_content_position"); // Position can be any value

	if (g_verbose) {
		ska_log(ska_log_info, "  Content position: (%d, %d)", x, y);
	}

	// Test frame size
	int32_t fw, fh;
	ska_window_get_frame_size(window, &fw, &fh);
	TEST_ASSERT("ska_window_get_frame_size returns positive width", fw > 0);
	TEST_ASSERT("ska_window_get_frame_size returns positive height", fh > 0);
	TEST_ASSERT("frame size >= content size (width)", fw >= w);
	TEST_ASSERT("frame size >= content size (height)", fh >= h);

	if (g_verbose) {
		ska_log(ska_log_info, "  Frame size: %dx%d", fw, fh);
	}

	// Test frame position
	int32_t fx, fy;
	ska_window_get_frame_position(window, &fx, &fy);
	TEST_PASS("ska_window_get_frame_position"); // Position can be any value

	if (g_verbose) {
		ska_log(ska_log_info, "  Frame position: (%d, %d)", fx, fy);
	}

	// Test drawable size
	int32_t dw, dh;
	ska_window_get_drawable_size(window, &dw, &dh);
	TEST_ASSERT("ska_window_get_drawable_size returns positive width", dw > 0);
	TEST_ASSERT("ska_window_get_drawable_size returns positive height", dh > 0);
	TEST_ASSERT("drawable size >= content size (width)", dw >= w);
	TEST_ASSERT("drawable size >= content size (height)", dh >= h);

	if (g_verbose) {
		ska_log(ska_log_info, "  Drawable size: %dx%d", dw, dh);
	}

	// Test DPI scale
	float dpi = ska_window_get_dpi_scale(window);
	TEST_ASSERT("ska_window_get_dpi_scale returns positive", dpi > 0.0f);
	TEST_ASSERT("ska_window_get_dpi_scale returns reasonable value", dpi >= 0.5f && dpi <= 10.0f);

	if (g_verbose) {
		ska_log(ska_log_info, "  DPI scale: %.2f", dpi);
	}

	// Test refresh rate
	float refresh = ska_window_get_refresh_rate(window);
	// Refresh rate of 0 is allowed (unavailable)
	TEST_ASSERT("ska_window_get_refresh_rate returns non-negative", refresh >= 0.0f);

	if (g_verbose) {
		ska_log(ska_log_info, "  Refresh rate: %.2f Hz", refresh);
	}

	// Test setting content size
	ska_window_set_content_size(window, 640, 480);
	ska_time_sleep(50); // Give window manager time to process
	ska_window_get_content_size(window, &w, &h);
	// Window managers may not honor exact size, so just check it changed or stayed valid
	TEST_ASSERT("ska_window_set_content_size results in valid size", w > 0 && h > 0);

	// Test setting content position
	ska_window_set_content_position(window, 100, 100);
	ska_time_sleep(50);
	TEST_PASS("ska_window_set_content_position"); // Can't verify position reliably

	// Test setting frame size
	ska_window_set_frame_size(window, 800, 600);
	ska_time_sleep(50);
	ska_window_get_frame_size(window, &fw, &fh);
	TEST_ASSERT("ska_window_set_frame_size results in valid size", fw > 0 && fh > 0);

	// Test setting frame position
	ska_window_set_frame_position(window, 50, 50);
	ska_time_sleep(50);
	TEST_PASS("ska_window_set_frame_position"); // Can't verify position reliably
}

static void test_window_state(ska_window_t* window) {
	if (!window) {
		TEST_SKIP("window state tests", "no window available");
		return;
	}

	ska_log(ska_log_info, "\n=== Window State Tests ===");

	// Test native handle
	void* native = ska_window_get_native_handle(window);
	TEST_ASSERT("ska_window_get_native_handle returns non-NULL", native != NULL);

#ifdef SKA_PLATFORM_LINUX
	void* display = ska_linux_get_x11_display();
	TEST_ASSERT("ska_linux_get_x11_display returns non-NULL", display != NULL);

	void* wayland = ska_linux_get_wayland_display();
	// Wayland not implemented, should return NULL
	TEST_ASSERT("ska_linux_get_wayland_display returns NULL (not implemented)", wayland == NULL);
#endif

#ifdef SKA_PLATFORM_WIN32
	void* hinstance = ska_win32_get_hinstance();
	TEST_ASSERT("ska_win32_get_hinstance returns non-NULL", hinstance != NULL);
#endif

	// Test window show/hide
	ska_window_hide(window);
	ska_time_sleep(50);
	TEST_PASS("ska_window_hide"); // No return value to check

	ska_window_show(window);
	ska_time_sleep(50);
	TEST_PASS("ska_window_show");

	// Test window raise
	ska_window_raise(window);
	TEST_PASS("ska_window_raise");
}

static void test_input_state(void) {
	ska_log(ska_log_info, "\n=== Input State Tests ===");

	// Test keyboard state
	int32_t num_keys = 0;
	const uint8_t* keyboard = ska_keyboard_get_state(&num_keys);
	TEST_ASSERT("ska_keyboard_get_state returns non-NULL", keyboard != NULL);
	TEST_ASSERT("ska_keyboard_get_state returns ska_scancode_count keys",
		num_keys == ska_scancode_count);

	// Test keyboard modifiers
	uint16_t mods = ska_keyboard_get_modifiers();
	TEST_PASS("ska_keyboard_get_modifiers"); // Any value is valid

	// Test mouse state
	int32_t mx, my;
	uint32_t buttons = ska_mouse_get_state(&mx, &my);
	TEST_PASS("ska_mouse_get_state"); // Any values are valid

	if (g_verbose) {
		ska_log(ska_log_info, "  Mouse position: (%d, %d), buttons: 0x%X", mx, my, buttons);
	}

	// Test global mouse state
	int32_t gx, gy;
	ska_mouse_get_global_state(&gx, &gy);
	TEST_PASS("ska_mouse_get_global_state");

	// Test relative mouse mode
	bool relative = ska_mouse_get_relative_mode();
	TEST_ASSERT("ska_mouse_get_relative_mode returns false initially", !relative);

	bool set_relative = ska_mouse_set_relative_mode(true);
	TEST_ASSERT("ska_mouse_set_relative_mode(true) succeeds", set_relative);

	relative = ska_mouse_get_relative_mode();
	TEST_ASSERT("ska_mouse_get_relative_mode returns true after set", relative);

	ska_mouse_set_relative_mode(false);
	relative = ska_mouse_get_relative_mode();
	TEST_ASSERT("ska_mouse_get_relative_mode returns false after reset", !relative);
}

static void test_cursor(void) {
	ska_log(ska_log_info, "\n=== Cursor Tests ===");

	// Test cursor visibility
	ska_cursor_show(false);
	TEST_PASS("ska_cursor_show(false)");

	ska_cursor_show(true);
	TEST_PASS("ska_cursor_show(true)");

	// Test cursor shapes
	ska_cursor_set(ska_system_cursor_arrow);
	TEST_PASS("ska_cursor_set(arrow)");

	ska_cursor_set(ska_system_cursor_ibeam);
	TEST_PASS("ska_cursor_set(ibeam)");

	ska_cursor_set(ska_system_cursor_hand);
	TEST_PASS("ska_cursor_set(hand)");

	ska_cursor_set(ska_system_cursor_crosshair);
	TEST_PASS("ska_cursor_set(crosshair)");

	// Restore default
	ska_cursor_set(ska_system_cursor_arrow);
}

static void test_text_input(void) {
	ska_log(ska_log_info, "\n=== Text Input Tests ===");

	// Clear any existing input
	ska_text_reset();
	TEST_PASS("ska_text_reset");

	// After reset, should have no input
	bool has_input = ska_text_has_input();
	TEST_ASSERT("ska_text_has_input returns false after reset", !has_input);

	// Peek should return 0 when empty
	uint32_t peeked = ska_text_peek();
	TEST_ASSERT("ska_text_peek returns 0 when empty", peeked == 0);

	// Consume should return 0 when empty
	uint32_t consumed = ska_text_consume();
	TEST_ASSERT("ska_text_consume returns 0 when empty", consumed == 0);

	// Test virtual keyboard state
	bool visible = ska_virtual_keyboard_is_visible();
	TEST_ASSERT("ska_virtual_keyboard_is_visible returns false initially", !visible);
}

static void test_clipboard(void) {
	ska_log(ska_log_info, "\n=== Clipboard Tests ===");

	const char* test_text = "sk_app clipboard test 12345";

	// Set clipboard
	bool set_ok = ska_clipboard_set_text(test_text);
	TEST_ASSERT("ska_clipboard_set_text succeeds", set_ok);

	if (set_ok) {
		// Get clipboard
		char* got_text = ska_clipboard_get_text();
		TEST_ASSERT("ska_clipboard_get_text returns non-NULL", got_text != NULL);

		if (got_text) {
			bool match = strcmp(got_text, test_text) == 0;
			TEST_ASSERT("ska_clipboard_get_text returns correct text", match);
			free(got_text);
		}
	}
}

static void test_events(ska_window_t* window) {
	if (!window) {
		TEST_SKIP("event tests", "no window available");
		return;
	}

	ska_log(ska_log_info, "\n=== Event System Tests ===");

	// Test event poll (non-blocking)
	ska_event_t event;
	// Just verify it doesn't crash - may or may not have events
	ska_event_poll(&event);
	TEST_PASS("ska_event_poll");

	// Test event wait with timeout (short timeout)
	bool got_event = ska_event_wait_timeout(&event, 10);
	TEST_PASS("ska_event_wait_timeout"); // Either result is valid

	if (g_verbose) {
		ska_log(ska_log_info, "  Event wait (10ms): %s", got_event ? "got event" : "timeout");
	}
}

static void test_vulkan_extensions(void) {
	ska_log(ska_log_info, "\n=== Vulkan Extension Tests ===");

	uint32_t ext_count = 0;
	const char** extensions = ska_vk_get_instance_extensions(&ext_count);

	TEST_ASSERT("ska_vk_get_instance_extensions returns non-NULL", extensions != NULL);
	TEST_ASSERT("ska_vk_get_instance_extensions returns at least 2 extensions", ext_count >= 2);

	if (g_verbose && extensions) {
		ska_log(ska_log_info, "  Vulkan extensions (%u):", ext_count);
		for (uint32_t i = 0; i < ext_count; i++) {
			ska_log(ska_log_info, "    - %s", extensions[i]);
		}
	}
}

static void test_file_dialogs(void) {
	ska_log(ska_log_info, "\n=== File Dialog Tests ===");

	// Test availability check
	bool open_available = ska_file_dialog_available(ska_file_dialog_open);
	bool save_available = ska_file_dialog_available(ska_file_dialog_save);
	bool folder_available = ska_file_dialog_available(ska_file_dialog_open_folder);

	TEST_PASS("ska_file_dialog_available(open)");
	TEST_PASS("ska_file_dialog_available(save)");
	TEST_PASS("ska_file_dialog_available(folder)");

	if (g_verbose) {
		ska_log(ska_log_info, "  Open file: %s", open_available ? "yes" : "no");
		ska_log(ska_log_info, "  Save file: %s", save_available ? "yes" : "no");
		ska_log(ska_log_info, "  Open folder: %s", folder_available ? "yes" : "no");
	}

	// Note: We don't actually open a dialog in automated tests as it requires user interaction
}

static void test_asset_reading(void) {
	ska_log(ska_log_info, "\n=== Asset Reading Tests ===");

	// Try to read a non-existent asset
	void* data = NULL;
	size_t size = 0;
	bool result = ska_asset_read("nonexistent_asset_12345.txt", &data, &size);

	TEST_ASSERT("ska_asset_read fails for non-existent asset", !result);

	// Try text version too
	char* text = NULL;
	result = ska_asset_read_text("nonexistent_asset_12345.txt", &text);
	TEST_ASSERT("ska_asset_read_text fails for non-existent asset", !result);

	// Note: Testing actual asset reading would require an asset to exist
	// which depends on the build setup
}

static void test_multiple_windows(void) {
	ska_log(ska_log_info, "\n=== Multiple Window Tests ===");

	// Create a second window
	ska_window_t* window2 = ska_window_create(
		"Test Window 2",
		SKA_WINDOWPOS_UNDEFINED,
		SKA_WINDOWPOS_UNDEFINED,
		400, 300,
		ska_window_resizable
	);

	TEST_ASSERT("second window creation succeeds", window2 != NULL);

	if (window2) {
		ska_window_id_t id2 = ska_window_get_id(window2);
		TEST_ASSERT("second window has unique ID", id2 != 0);

		// Verify lookup works
		ska_window_t* from_id = ska_window_from_id(id2);
		TEST_ASSERT("second window ID lookup works", from_id == window2);

		// Destroy second window
		ska_window_destroy(window2);
		TEST_PASS("second window destroyed");

		// Verify ID lookup fails after destroy
		from_id = ska_window_from_id(id2);
		TEST_ASSERT("destroyed window ID lookup returns NULL", from_id == NULL);
	}
}

static void test_mouse_warp(ska_window_t* window) {
	if (!window) {
		TEST_SKIP("mouse warp test", "no window available");
		return;
	}

	ska_log(ska_log_info, "\n=== Mouse Warp Tests ===");

	// Warp mouse to center of window
	int32_t w, h;
	ska_window_get_content_size(window, &w, &h);
	ska_mouse_warp(window, w / 2, h / 2);
	TEST_PASS("ska_mouse_warp to center");

	// Warp to corner
	ska_mouse_warp(window, 0, 0);
	TEST_PASS("ska_mouse_warp to origin");
}

// ============================================================================
// Interactive Demo (non-test mode)
// ============================================================================

static void run_interactive_demo(ska_window_t* window) {
	ska_log(ska_log_info, "\n=== Interactive Demo Mode ===");
	ska_log(ska_log_info, "Press ESC to exit, other keys for various tests:");
	ska_log(ska_log_info, "  M - Maximize    N - Minimize    R - Restore");
	ska_log(ska_log_info, "  C - Toggle cursor    V - Toggle relative mouse");
	ska_log(ska_log_info, "  F - File dialog (open)    G - File dialog (save)");
	ska_log(ska_log_info, "  T - Show virtual keyboard");

	bool running = true;
	uint32_t frame = 0;

	while (running) {
		ska_event_t event;

		while (ska_event_poll(&event)) {
			switch (event.type) {
				case ska_event_quit:
				case ska_event_window_close:
					running = false;
					break;

				case ska_event_key_down:
					if (!event.keyboard.repeat) {
						switch (event.keyboard.scancode) {
							case ska_scancode_escape:
								running = false;
								break;
							case ska_scancode_m:
								ska_window_maximize(window);
								break;
							case ska_scancode_n:
								ska_window_minimize(window);
								break;
							case ska_scancode_r:
								ska_window_restore(window);
								break;
							case ska_scancode_c:
								{
									static bool cursor_visible = true;
									cursor_visible = !cursor_visible;
									ska_cursor_show(cursor_visible);
								}
								break;
							case ska_scancode_v:
								{
									bool rel = ska_mouse_get_relative_mode();
									ska_mouse_set_relative_mode(!rel);
								}
								break;
							case ska_scancode_t:
								ska_virtual_keyboard_show(true, ska_text_input_type_text);
								break;
							case ska_scancode_f:
								{
									ska_file_filter_t filters[] = {
										{ "All Files", "*/*", "*" },
									};
									ska_file_dialog_request_t req = {
										.type = ska_file_dialog_open,
										.title = "Open File",
										.filters = filters,
										.filter_count = 1,
									};
									ska_file_dialog_show(&req);
								}
								break;
							case ska_scancode_g:
								{
									ska_file_filter_t filters[] = {
										{ "Text Files", "text/plain", "*.txt" },
									};
									ska_file_dialog_request_t req = {
										.type = ska_file_dialog_save,
										.title = "Save File",
										.default_name = "untitled.txt",
										.filters = filters,
										.filter_count = 1,
									};
									ska_file_dialog_show(&req);
								}
								break;
							default:
								break;
						}
					}
					break;

				case ska_event_file_dialog:
					ska_log(ska_log_info, "[FILE DIALOG] %s: %d files selected",
						event.file_dialog.title ? event.file_dialog.title : "(null)",
						event.file_dialog.count);
					for (int32_t i = 0; i < event.file_dialog.count; i++) {
						ska_log(ska_log_info, "  [%d] %s", i,
							ska_file_dialog_get_path(&event.file_dialog, i));
					}
					ska_file_dialog_free_result(&event.file_dialog);
					break;

				default:
					break;
			}
		}

		ska_time_sleep(16);
		frame++;
	}
}

// ============================================================================
// Main
// ============================================================================

int32_t main(int32_t argc, char** argv) {
	// Parse arguments
	bool test_mode = false;

	for (int32_t i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-test") == 0 || strcmp(argv[i], "--test") == 0) {
			test_mode = true;
		} else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
			g_verbose = true;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			printf("Usage: %s [options]\n", argv[0]);
			printf("  -test, --test       Run automated tests and exit\n");
			printf("  -v, --verbose       Verbose test output\n");
			printf("  -h, --help          Show this help\n");
			return 0;
		}
	}

	ska_log(ska_log_info, "========================================");
	ska_log(ska_log_info, "  sk_app Test Suite");
	ska_log(ska_log_info, "  Version %d.%d.%d", SKA_VERSION_MAJOR, SKA_VERSION_MINOR, SKA_VERSION_PATCH);
	ska_log(ska_log_info, "========================================");
	ska_log(ska_log_info, "Mode: %s", test_mode ? "automated testing" : "interactive");
	if (g_verbose) {
		ska_log(ska_log_info, "Verbose output enabled");
	}

	// Run pre-init tests
	test_version();

	// Initialize
	ska_log(ska_log_info, "\n=== Initialization ===");
	if (!ska_init()) {
		ska_log(ska_log_error, "Failed to initialize sk_app: %s", ska_error_get());
		TEST_FAIL("ska_init", ska_error_get());
		goto print_summary;
	}
	TEST_PASS("ska_init");

	// Run tests that don't need a window
	test_error_handling();
	test_timing();
	test_working_directory();
	test_file_io();
	test_directory_iteration();
	test_kvpstore();
	test_asset_reading();
	test_vulkan_extensions();

	// Create window and run window-dependent tests
	ska_window_t* window = NULL;
	test_window_creation(&window);
	test_window_geometry(window);
	test_window_state(window);
	test_input_state();
	test_cursor();
	test_text_input();
	test_clipboard();
	test_events(window);
	test_file_dialogs();
	test_multiple_windows();
	test_mouse_warp(window);

	// Interactive mode or exit
	if (!test_mode && window) {
		run_interactive_demo(window);
	}

	// Cleanup
	ska_log(ska_log_info, "\n=== Cleanup ===");
	if (window) {
		ska_window_destroy(window);
		TEST_PASS("ska_window_destroy");
	}

	ska_shutdown();
	TEST_PASS("ska_shutdown");

print_summary:
	// Print summary
	ska_log(ska_log_info, "\n========================================");
	ska_log(ska_log_info, "  Test Summary");
	ska_log(ska_log_info, "========================================");
	ska_log(ska_log_info, "  Passed:  %d", g_tests_passed);
	ska_log(ska_log_info, "  Failed:  %d", g_tests_failed);
	ska_log(ska_log_info, "  Skipped: %d", g_tests_skipped);
	ska_log(ska_log_info, "  Total:   %d", g_tests_passed + g_tests_failed + g_tests_skipped);
	ska_log(ska_log_info, "========================================");

	if (g_tests_failed == 0) {
		ska_log(ska_log_info, "  [TEST RESULT: PASS]");
	} else {
		ska_log(ska_log_error, "  [TEST RESULT: FAIL]");
	}
	ska_log(ska_log_info, "========================================");

	return g_tests_failed > 0 ? 1 : 0;
}
