//
// sk_app - Android standalone entry point
//
// Provides android_main() and thread machinery for standalone Android apps
// using android_native_app_glue. This is compiled into the sk_app_entrypoint
// target. Standalone targets link both sk_app and sk_app_entrypoint; library-
// mode targets link only sk_app.

#include "ska_internal.h"

#ifdef SKA_PLATFORM_ANDROID

#include <android_native_app_glue.h>
#include <android/native_activity.h>
#include <pthread.h>

extern int32_t main(int argc, char** argv);

typedef struct {
	struct android_app* app;
	bool user_main_finished;
	int32_t user_main_result;
} android_main_state_t;

static android_main_state_t g_android_main_state = {0};

static void* ska_android_user_main_thread(void* arg) {
	(void)arg;

	char* argv[] = {"sk_app", NULL};
	g_android_main_state.user_main_result = main(1, argv);
	g_android_main_state.user_main_finished = true;

	ANativeActivity_finish(g_android_main_state.app->activity);

	return NULL;
}

void android_main(struct android_app* app) {
	ska_android_set_app(app);
	g_android_main_state.app = app;
	g_android_main_state.user_main_finished = false;
	g_android_main_state.user_main_result = 0;

	pthread_t user_thread;

	// Start user's main() thread immediately.
	// It will wait for the window in ska_platform_window_create().
	pthread_create(&user_thread, NULL, ska_android_user_main_thread, NULL);

	// Main event loop
	while (1) {
		int32_t events;
		struct android_poll_source* source;

		// Non-blocking poll since user thread is running
		while (ALooper_pollOnce(0, NULL, &events, (void**)&source) >= 0) {
			if (source != NULL) {
				source->process(app, source);
			}

			if (app->destroyRequested != 0) {
				if (!g_android_main_state.user_main_finished) {
					pthread_join(user_thread, NULL);
				}
				return;
			}
		}

		if (g_android_main_state.user_main_finished) {
			pthread_join(user_thread, NULL);
			return;
		}

		// Small sleep to avoid busy-waiting
		ska_time_sleep(1);
	}
}

#endif // SKA_PLATFORM_ANDROID
