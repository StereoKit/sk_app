// minimal_window - Bare-bones sk_app example
//
// Demonstrates the minimum code needed to create a window with sk_app:
// initialization, window creation, frame loop, and cleanup.
//
// The main loop uses ska_run() with a per-frame callback, which runs the same
// app source on every platform: a plain while-loop on desktop, and a
// requestAnimationFrame-driven loop on the web.

#include <sk_app.h>

static bool app_frame(void* user_data) {
	(void)user_data;

	ska_event_t event;
	while (ska_event_poll(&event)) {
		switch (event.type) {
			case ska_event_quit:
			case ska_event_window_close:
				return false;

			case ska_event_key_down:
				if (event.keyboard.scancode == ska_scancode_escape) {
					return false;
				}
				break;

			default:
				break;
		}
	}

	// Rendering would go here
#ifndef SKA_PLATFORM_WEB
	// Nothing to render, so sleep to avoid busy-waiting. On the web,
	// requestAnimationFrame paces the loop instead.
	ska_time_sleep(16);
#endif

	return true;
}

int32_t main(int32_t argc, char** argv) {
	(void)argc;
	(void)argv;

	// Initialize sk_app
	if (!ska_init(NULL)) {
		ska_log(ska_log_error, "Failed to initialize: %s", ska_error_get());
		return 1;
	}

	// Create a window
	ska_window_t* window = ska_window_create(
		"Minimal Window",
		SKA_WINDOWPOS_CENTERED,
		SKA_WINDOWPOS_CENTERED,
		800, 600,
		ska_window_resizable
	);

	if (!window) {
		ska_log(ska_log_error, "Failed to create window: %s", ska_error_get());
		ska_shutdown();
		return 1;
	}

	// Main loop (does not return on the web)
	ska_run(app_frame, window);

	// Cleanup
	ska_window_destroy(window);
	ska_shutdown();

	return 0;
}
