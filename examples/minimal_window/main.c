// minimal_window - Bare-bones sk_app example
//
// Demonstrates the minimum code needed to create a window with sk_app:
// initialization, window creation, event loop, and cleanup.

#include <sk_app.h>

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

	// Main loop
	bool running = true;
	while (running) {
		ska_event_t event;
		while (ska_event_poll(&event)) {
			switch (event.type) {
				case ska_event_quit:
				case ska_event_window_close:
					running = false;
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

		// Rendering would go here
		// For now, just sleep to avoid busy-waiting
		ska_time_sleep(16);
	}

	// Cleanup
	ska_window_destroy(window);
	ska_shutdown();

	return 0;
}
