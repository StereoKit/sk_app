//
// sk_app - Android platform backend

#include "ska_internal.h"

#ifdef SKA_PLATFORM_ANDROID

#include <android_native_app_glue.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/asset_manager.h>
#include <dlfcn.h>
#include <unistd.h>

// Scancode translation table (Android key codes to ska_scancode_)
static ska_scancode_ ska_android_scancode_table[256];

// ============================================================================
// Cached JNI references for window management (freeform/DEX mode)
// ============================================================================
// Method IDs and field IDs are stable once looked up and can be cached.
// Classes must be converted to global references to survive across JNI calls.

static struct {
	// NativeActivity -> Window
	jmethodID   activity_getWindow;
	// Window methods
	jmethodID   window_getAttributes;
	jmethodID   window_setAttributes;
	jmethodID   window_getDecorView;
	// View methods
	jmethodID   view_getWidth;
	jmethodID   view_getHeight;
	// LayoutParams fields
	jfieldID    lp_x;
	jfieldID    lp_y;
	jfieldID    lp_width;
	jfieldID    lp_height;
} g_jni_cache = {0};

// Get JNIEnv for the current thread, attaching if necessary.
// Both android_main and user threads are long-lived, so we attach once and
// leave attached for the thread's lifetime.
static JNIEnv* ska_jni_get_env(void) {
	if (!g_ska.android_app || !g_ska.android_app->activity) {
		return NULL;
	}

	JavaVM* jvm = g_ska.android_app->activity->vm;
	JNIEnv* env = NULL;

	// GetEnv returns JNI_OK if already attached, JNI_EDETACHED otherwise
	jint result = (*jvm)->GetEnv(jvm, (void**)&env, JNI_VERSION_1_6);

	if (result == JNI_EDETACHED) {
		if ((*jvm)->AttachCurrentThread(jvm, &env, NULL) != JNI_OK) {
			return NULL;
		}
	}

	return env;
}

static void ska_jni_cache_init(void) {
	JNIEnv* env = ska_jni_get_env();
	if (!env) {
		return;
	}

	// NativeActivity.getWindow()
	jclass activity_class = (*env)->FindClass(env, "android/app/NativeActivity");
	g_jni_cache.activity_getWindow = (*env)->GetMethodID(env, activity_class, "getWindow", "()Landroid/view/Window;");

	// Window methods
	jclass window_class = (*env)->FindClass(env, "android/view/Window");
	g_jni_cache.window_getAttributes = (*env)->GetMethodID(env, window_class, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;");
	g_jni_cache.window_setAttributes = (*env)->GetMethodID(env, window_class, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V");
	g_jni_cache.window_getDecorView  = (*env)->GetMethodID(env, window_class, "getDecorView", "()Landroid/view/View;");

	// View methods
	jclass view_class = (*env)->FindClass(env, "android/view/View");
	g_jni_cache.view_getWidth  = (*env)->GetMethodID(env, view_class, "getWidth", "()I");
	g_jni_cache.view_getHeight = (*env)->GetMethodID(env, view_class, "getHeight", "()I");

	// LayoutParams fields
	jclass lp_class = (*env)->FindClass(env, "android/view/WindowManager$LayoutParams");
	g_jni_cache.lp_x      = (*env)->GetFieldID(env, lp_class, "x", "I");
	g_jni_cache.lp_y      = (*env)->GetFieldID(env, lp_class, "y", "I");
	g_jni_cache.lp_width  = (*env)->GetFieldID(env, lp_class, "width", "I");
	g_jni_cache.lp_height = (*env)->GetFieldID(env, lp_class, "height", "I");
}

static void ska_jni_cache_shutdown(void) {
	// Detach current thread if attached (user thread calls this during shutdown)
	if (g_ska.android_app && g_ska.android_app->activity) {
		JavaVM* jvm = g_ska.android_app->activity->vm;
		JNIEnv* env = NULL;
		if ((*jvm)->GetEnv(jvm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) {
			(*jvm)->DetachCurrentThread(jvm);
		}
	}
	memset(&g_jni_cache, 0, sizeof(g_jni_cache));
}

// Helper to get window position via JNI (for freeform/DEX mode)
static void ska_android_get_window_position(ska_window_t* window) {
	if (!window) {
		return;
	}

	JNIEnv* env = ska_jni_get_env();
	if (!env) {
		return;
	}

	jobject jwindow = (*env)->CallObjectMethod(env, g_ska.android_app->activity->clazz, g_jni_cache.activity_getWindow);

	if (jwindow) {
		jobject layout_params = (*env)->CallObjectMethod(env, jwindow, g_jni_cache.window_getAttributes);

		if (layout_params) {
			window->x = (*env)->GetIntField(env, layout_params, g_jni_cache.lp_x);
			window->y = (*env)->GetIntField(env, layout_params, g_jni_cache.lp_y);

			(*env)->DeleteLocalRef(env, layout_params);
		}
		(*env)->DeleteLocalRef(env, jwindow);
	}
}

static void ska_init_scancode_table(void) {
	// Initialize all to unknown
	for (int32_t i = 0; i < 256; i++) {
		ska_android_scancode_table[i] = ska_scancode_unknown;
	}

	// Letters
	ska_android_scancode_table[AKEYCODE_A] = ska_scancode_a;
	ska_android_scancode_table[AKEYCODE_B] = ska_scancode_b;
	ska_android_scancode_table[AKEYCODE_C] = ska_scancode_c;
	ska_android_scancode_table[AKEYCODE_D] = ska_scancode_d;
	ska_android_scancode_table[AKEYCODE_E] = ska_scancode_e;
	ska_android_scancode_table[AKEYCODE_F] = ska_scancode_f;
	ska_android_scancode_table[AKEYCODE_G] = ska_scancode_g;
	ska_android_scancode_table[AKEYCODE_H] = ska_scancode_h;
	ska_android_scancode_table[AKEYCODE_I] = ska_scancode_i;
	ska_android_scancode_table[AKEYCODE_J] = ska_scancode_j;
	ska_android_scancode_table[AKEYCODE_K] = ska_scancode_k;
	ska_android_scancode_table[AKEYCODE_L] = ska_scancode_l;
	ska_android_scancode_table[AKEYCODE_M] = ska_scancode_m;
	ska_android_scancode_table[AKEYCODE_N] = ska_scancode_n;
	ska_android_scancode_table[AKEYCODE_O] = ska_scancode_o;
	ska_android_scancode_table[AKEYCODE_P] = ska_scancode_p;
	ska_android_scancode_table[AKEYCODE_Q] = ska_scancode_q;
	ska_android_scancode_table[AKEYCODE_R] = ska_scancode_r;
	ska_android_scancode_table[AKEYCODE_S] = ska_scancode_s;
	ska_android_scancode_table[AKEYCODE_T] = ska_scancode_t;
	ska_android_scancode_table[AKEYCODE_U] = ska_scancode_u;
	ska_android_scancode_table[AKEYCODE_V] = ska_scancode_v;
	ska_android_scancode_table[AKEYCODE_W] = ska_scancode_w;
	ska_android_scancode_table[AKEYCODE_X] = ska_scancode_x;
	ska_android_scancode_table[AKEYCODE_Y] = ska_scancode_y;
	ska_android_scancode_table[AKEYCODE_Z] = ska_scancode_z;

	// Numbers
	ska_android_scancode_table[AKEYCODE_0] = ska_scancode_0;
	ska_android_scancode_table[AKEYCODE_1] = ska_scancode_1;
	ska_android_scancode_table[AKEYCODE_2] = ska_scancode_2;
	ska_android_scancode_table[AKEYCODE_3] = ska_scancode_3;
	ska_android_scancode_table[AKEYCODE_4] = ska_scancode_4;
	ska_android_scancode_table[AKEYCODE_5] = ska_scancode_5;
	ska_android_scancode_table[AKEYCODE_6] = ska_scancode_6;
	ska_android_scancode_table[AKEYCODE_7] = ska_scancode_7;
	ska_android_scancode_table[AKEYCODE_8] = ska_scancode_8;
	ska_android_scancode_table[AKEYCODE_9] = ska_scancode_9;

	// Function keys
	ska_android_scancode_table[AKEYCODE_ENTER] = ska_scancode_return;
	ska_android_scancode_table[AKEYCODE_ESCAPE] = ska_scancode_escape;
	ska_android_scancode_table[AKEYCODE_BACK] = ska_scancode_escape;  // Back button = Escape
	ska_android_scancode_table[AKEYCODE_DEL] = ska_scancode_backspace;
	ska_android_scancode_table[AKEYCODE_TAB] = ska_scancode_tab;
	ska_android_scancode_table[AKEYCODE_SPACE] = ska_scancode_space;

	// Symbols
	ska_android_scancode_table[AKEYCODE_MINUS] = ska_scancode_minus;
	ska_android_scancode_table[AKEYCODE_EQUALS] = ska_scancode_equals;
	ska_android_scancode_table[AKEYCODE_LEFT_BRACKET] = ska_scancode_leftbracket;
	ska_android_scancode_table[AKEYCODE_RIGHT_BRACKET] = ska_scancode_rightbracket;
	ska_android_scancode_table[AKEYCODE_BACKSLASH] = ska_scancode_backslash;
	ska_android_scancode_table[AKEYCODE_SEMICOLON] = ska_scancode_semicolon;
	ska_android_scancode_table[AKEYCODE_APOSTROPHE] = ska_scancode_apostrophe;
	ska_android_scancode_table[AKEYCODE_GRAVE] = ska_scancode_grave;
	ska_android_scancode_table[AKEYCODE_COMMA] = ska_scancode_comma;
	ska_android_scancode_table[AKEYCODE_PERIOD] = ska_scancode_period;
	ska_android_scancode_table[AKEYCODE_SLASH] = ska_scancode_slash;

	ska_android_scancode_table[AKEYCODE_CAPS_LOCK] = ska_scancode_capslock;

	// F keys
	ska_android_scancode_table[AKEYCODE_F1] = ska_scancode_f1;
	ska_android_scancode_table[AKEYCODE_F2] = ska_scancode_f2;
	ska_android_scancode_table[AKEYCODE_F3] = ska_scancode_f3;
	ska_android_scancode_table[AKEYCODE_F4] = ska_scancode_f4;
	ska_android_scancode_table[AKEYCODE_F5] = ska_scancode_f5;
	ska_android_scancode_table[AKEYCODE_F6] = ska_scancode_f6;
	ska_android_scancode_table[AKEYCODE_F7] = ska_scancode_f7;
	ska_android_scancode_table[AKEYCODE_F8] = ska_scancode_f8;
	ska_android_scancode_table[AKEYCODE_F9] = ska_scancode_f9;
	ska_android_scancode_table[AKEYCODE_F10] = ska_scancode_f10;
	ska_android_scancode_table[AKEYCODE_F11] = ska_scancode_f11;
	ska_android_scancode_table[AKEYCODE_F12] = ska_scancode_f12;

	// Navigation
	ska_android_scancode_table[AKEYCODE_MOVE_HOME] = ska_scancode_home;
	ska_android_scancode_table[AKEYCODE_PAGE_UP] = ska_scancode_pageup;
	ska_android_scancode_table[AKEYCODE_FORWARD_DEL] = ska_scancode_delete;
	ska_android_scancode_table[AKEYCODE_MOVE_END] = ska_scancode_end;
	ska_android_scancode_table[AKEYCODE_PAGE_DOWN] = ska_scancode_pagedown;
	ska_android_scancode_table[AKEYCODE_DPAD_RIGHT] = ska_scancode_right;
	ska_android_scancode_table[AKEYCODE_DPAD_LEFT] = ska_scancode_left;
	ska_android_scancode_table[AKEYCODE_DPAD_DOWN] = ska_scancode_down;
	ska_android_scancode_table[AKEYCODE_DPAD_UP] = ska_scancode_up;

	// Modifiers
	ska_android_scancode_table[AKEYCODE_CTRL_LEFT] = ska_scancode_lctrl;
	ska_android_scancode_table[AKEYCODE_SHIFT_LEFT] = ska_scancode_lshift;
	ska_android_scancode_table[AKEYCODE_ALT_LEFT] = ska_scancode_lalt;
	ska_android_scancode_table[AKEYCODE_META_LEFT] = ska_scancode_lgui;
	ska_android_scancode_table[AKEYCODE_CTRL_RIGHT] = ska_scancode_rctrl;
	ska_android_scancode_table[AKEYCODE_SHIFT_RIGHT] = ska_scancode_rshift;
	ska_android_scancode_table[AKEYCODE_ALT_RIGHT] = ska_scancode_ralt;
	ska_android_scancode_table[AKEYCODE_META_RIGHT] = ska_scancode_rgui;
}

static uint16_t ska_android_get_modifiers(int32_t meta_state) {
	uint16_t mods = 0;
	if (meta_state & AMETA_SHIFT_ON) mods |= ska_keymod_shift;
	if (meta_state & AMETA_CTRL_ON) mods |= ska_keymod_ctrl;
	if (meta_state & AMETA_ALT_ON) mods |= ska_keymod_alt;
	if (meta_state & AMETA_META_ON) mods |= ska_keymod_gui;
	return mods;
}

// Command handler for app lifecycle events
static void ska_android_handle_cmd(struct android_app* app, int32_t cmd) {
	ska_event_t event = {0};
	event.timestamp = (uint32_t)ska_time_get_elapsed_ms();

	switch (cmd) {
		case APP_CMD_INIT_WINDOW:
			// Window has been created
			if (app->window != NULL && g_ska.window_count > 0) {
				ska_window_t* window = g_ska.windows[0];
				if (window) {
					window->native_window = app->window;

					int32_t width  = ANativeWindow_getWidth(app->window);
					int32_t height = ANativeWindow_getHeight(app->window);
					int32_t format = ANativeWindow_getFormat(app->window);

					window->width           = width;
					window->height          = height;
					window->drawable_width  = width;
					window->drawable_height = height;
					window->dpi_scale       = ska_platform_get_dpi_scale(window);

					// Get position (relevant in freeform/DEX mode)
					ska_android_get_window_position(window);

					event.type = ska_event_window_shown;
					event.window.window_id = window->id;
					window->is_visible = true;
					ska_post_event(&event);

					ska_log(ska_log_info, "Android window created: %dx%d at (%d,%d) (format=%d, dpi_scale=%.2f)",
						width, height, window->x, window->y, format, window->dpi_scale);
				}
			}
			break;

		case APP_CMD_TERM_WINDOW:
			// Window is being destroyed
			if (g_ska.window_count > 0) {
				ska_window_t* window = g_ska.windows[0];
				if (window) {
					event.type = ska_event_window_hidden;
					event.window.window_id = window->id;
					window->is_visible = false;
					ska_post_event(&event);

					window->native_window = NULL;
				}
			}
			break;

		case APP_CMD_WINDOW_RESIZED:
		case APP_CMD_CONFIG_CHANGED:
			// Window has been resized or moved (freeform/DEX mode)
			if (app->window != NULL && g_ska.window_count > 0) {
				ska_window_t* window = g_ska.windows[0];
				if (window) {
					int32_t width  = ANativeWindow_getWidth(app->window);
					int32_t height = ANativeWindow_getHeight(app->window);

					// Also update position (may change in freeform mode)
					int32_t old_x = window->x;
					int32_t old_y = window->y;
					ska_android_get_window_position(window);

					// Post move event if position changed
					if (window->x != old_x || window->y != old_y) {
						event.type = ska_event_window_moved;
						event.window.window_id = window->id;
						event.window.data1 = window->x;
						event.window.data2 = window->y;
						ska_post_event(&event);
					}

					if (width != window->width || height != window->height) {
						event.type = ska_event_window_resized;
						event.window.window_id = window->id;
						event.window.data1     = width;
						event.window.data2     = height;
						window->width           = width;
						window->height          = height;
						window->drawable_width  = width;
						window->drawable_height = height;
						ska_post_event(&event);

						ska_log(ska_log_info, "Android window resized: %dx%d", width, height);
					}
				}
			}
			break;

		case APP_CMD_GAINED_FOCUS:
			g_ska.app_has_focus = true;
			if (g_ska.window_count > 0) {
				ska_window_t* window = g_ska.windows[0];
				if (window) {
					event.type = ska_event_window_focus_gained;
					event.window.window_id = window->id;
					window->has_focus = true;
					ska_post_event(&event);
				}
			}
			break;

		case APP_CMD_LOST_FOCUS:
			g_ska.app_has_focus = false;
			if (g_ska.window_count > 0) {
				ska_window_t* window = g_ska.windows[0];
				if (window) {
					event.type = ska_event_window_focus_lost;
					event.window.window_id = window->id;
					window->has_focus = false;
					ska_post_event(&event);
				}
			}
			break;

		case APP_CMD_PAUSE:
			event.type = ska_event_app_background;
			g_ska.app_is_visible = false;
			ska_post_event(&event);
			ska_log(ska_log_info, "App paused");
			break;

		case APP_CMD_RESUME:
			event.type = ska_event_app_foreground;
			g_ska.app_is_visible = true;
			ska_post_event(&event);
			ska_log(ska_log_info, "App resumed");
			break;

		case APP_CMD_LOW_MEMORY:
			event.type = ska_event_app_lowmemory;
			ska_post_event(&event);
			ska_log(ska_log_warn, "Low memory warning");
			break;

		case APP_CMD_DESTROY:
			event.type = ska_event_quit;
			ska_post_event(&event);
			ska_log(ska_log_info, "App destroy requested");
			break;
	}
}

// Input event handler
static int32_t ska_android_handle_input(struct android_app* app, AInputEvent* input_event) {
	if (g_ska.window_count == 0) {
		return 0;
	}

	ska_window_t* window = g_ska.windows[0];
	if (!window) {
		return 0;
	}

	ska_event_t event = {0};
	event.timestamp = (uint32_t)ska_time_get_elapsed_ms();

	int32_t event_type = AInputEvent_getType(input_event);

	if (event_type == AINPUT_EVENT_TYPE_KEY) {
		// Keyboard event
		int32_t action = AKeyEvent_getAction(input_event);
		int32_t keycode = AKeyEvent_getKeyCode(input_event);
		int32_t meta_state = AKeyEvent_getMetaState(input_event);

		bool pressed = (action == AKEY_EVENT_ACTION_DOWN);
		bool repeat = (action == AKEY_EVENT_ACTION_MULTIPLE);

		event.type = pressed ? ska_event_key_down : ska_event_key_up;
		event.keyboard.window_id = window->id;
		event.keyboard.pressed = pressed;
		event.keyboard.repeat = repeat;
		event.keyboard.scancode = ska_android_scancode_table[keycode];
		event.keyboard.modifiers = ska_android_get_modifiers(meta_state);

		// Update input state
		if (event.keyboard.scancode != ska_scancode_unknown) {
			g_ska.input_state.keyboard[event.keyboard.scancode] = pressed ? 1 : 0;
		}
		g_ska.input_state.key_modifiers = event.keyboard.modifiers;

		ska_post_event(&event);
		return 1;

	} else if (event_type == AINPUT_EVENT_TYPE_MOTION) {
		// Touch/Mouse event
		int32_t action = AMotionEvent_getAction(input_event);
		int32_t action_masked = action & AMOTION_EVENT_ACTION_MASK;
		int32_t source = AInputEvent_getSource(input_event);

		// Get coordinates - Android provides float precision
		// Note: getX/getY return coordinates relative to the window
		// getXOffset/getYOffset are NOT position offsets - they're historical offsets
		// for batch events. We should use getX/getY directly for window-relative coords.
		float x_float = AMotionEvent_getX(input_event, 0);
		float y_float = AMotionEvent_getY(input_event, 0);

		// Round to avoid cumulative positioning errors
		int32_t x = (int32_t)(x_float + 0.5f);
		int32_t y = (int32_t)(y_float + 0.5f);

		// Handle mouse scroll wheel
		if (action_masked == AMOTION_EVENT_ACTION_SCROLL) {
			float vscroll = AMotionEvent_getAxisValue(input_event, AMOTION_EVENT_AXIS_VSCROLL, 0);
			float hscroll = AMotionEvent_getAxisValue(input_event, AMOTION_EVENT_AXIS_HSCROLL, 0);

			event.type = ska_event_mouse_wheel;
			event.mouse_wheel.window_id = window->id;
			event.mouse_wheel.x = (int32_t)hscroll;
			event.mouse_wheel.y = (int32_t)vscroll;
			event.mouse_wheel.precise_x = hscroll;
			event.mouse_wheel.precise_y = vscroll;

			ska_post_event(&event);
			return 1;
		}

		// Handle mouse button events (for mice/trackpads connected to Android)
		if ((source & AINPUT_SOURCE_MOUSE) && action_masked == AMOTION_EVENT_ACTION_BUTTON_PRESS) {
			int32_t button_state = AMotionEvent_getButtonState(input_event);
			ska_mouse_button_ button = ska_mouse_button_left;

			if (button_state & AMOTION_EVENT_BUTTON_PRIMARY) {
				button = ska_mouse_button_left;
			} else if (button_state & AMOTION_EVENT_BUTTON_SECONDARY) {
				button = ska_mouse_button_right;
			} else if (button_state & AMOTION_EVENT_BUTTON_TERTIARY) {
				button = ska_mouse_button_middle;
			} else if (button_state & AMOTION_EVENT_BUTTON_BACK) {
				button = ska_mouse_button_x1;
			} else if (button_state & AMOTION_EVENT_BUTTON_FORWARD) {
				button = ska_mouse_button_x2;
			}

			event.type = ska_event_mouse_button_down;
			event.mouse_button.window_id = window->id;
			event.mouse_button.button = button;
			event.mouse_button.pressed = true;
			event.mouse_button.clicks = 1;
			event.mouse_button.x = (int32_t)x;
			event.mouse_button.y = (int32_t)y;

			g_ska.input_state.mouse_buttons |= (1 << (button - 1));

			ska_post_event(&event);
			return 1;
		}

		if ((source & AINPUT_SOURCE_MOUSE) && action_masked == AMOTION_EVENT_ACTION_BUTTON_RELEASE) {
			int32_t button_state = AMotionEvent_getButtonState(input_event);
			ska_mouse_button_ button = ska_mouse_button_left;

			// Note: button_state reflects the state AFTER release, so we need to check what was released
			// For simplicity, we'll just use the last button pressed
			if (button_state & AMOTION_EVENT_BUTTON_PRIMARY) {
				button = ska_mouse_button_left;
			} else if (button_state & AMOTION_EVENT_BUTTON_SECONDARY) {
				button = ska_mouse_button_right;
			} else if (button_state & AMOTION_EVENT_BUTTON_TERTIARY) {
				button = ska_mouse_button_middle;
			} else if (button_state & AMOTION_EVENT_BUTTON_BACK) {
				button = ska_mouse_button_x1;
			} else if (button_state & AMOTION_EVENT_BUTTON_FORWARD) {
				button = ska_mouse_button_x2;
			}

			event.type = ska_event_mouse_button_up;
			event.mouse_button.window_id = window->id;
			event.mouse_button.button = button;
			event.mouse_button.pressed = false;
			event.mouse_button.clicks = 1;
			event.mouse_button.x = (int32_t)x;
			event.mouse_button.y = (int32_t)y;

			g_ska.input_state.mouse_buttons &= ~(1 << (button - 1));

			ska_post_event(&event);
			return 1;
		}

		// Handle mouse hover motion (when no button is pressed)
		if ((source & AINPUT_SOURCE_MOUSE) && action_masked == AMOTION_EVENT_ACTION_HOVER_MOVE) {
			event.type = ska_event_mouse_motion;
			event.mouse_motion.window_id = window->id;
			event.mouse_motion.x = (int32_t)x;
			event.mouse_motion.y = (int32_t)y;
			event.mouse_motion.xrel = (int32_t)x - g_ska.input_state.mouse_x;
			event.mouse_motion.yrel = (int32_t)y - g_ska.input_state.mouse_y;

			g_ska.input_state.mouse_x = (int32_t)x;
			g_ska.input_state.mouse_y = (int32_t)y;
			g_ska.input_state.mouse_xrel = event.mouse_motion.xrel;
			g_ska.input_state.mouse_yrel = event.mouse_motion.yrel;

			ska_post_event(&event);
			return 1;
		}

		switch (action_masked) {
			case AMOTION_EVENT_ACTION_DOWN:
				// Primary touch down = Mouse motion + Left button down
				// First send motion event to update position
				if ((int32_t)x != g_ska.input_state.mouse_x || (int32_t)y != g_ska.input_state.mouse_y) {
					event.type = ska_event_mouse_motion;
					event.mouse_motion.window_id = window->id;
					event.mouse_motion.x = (int32_t)x;
					event.mouse_motion.y = (int32_t)y;
					event.mouse_motion.xrel = (int32_t)x - g_ska.input_state.mouse_x;
					event.mouse_motion.yrel = (int32_t)y - g_ska.input_state.mouse_y;

					g_ska.input_state.mouse_x = (int32_t)x;
					g_ska.input_state.mouse_y = (int32_t)y;
					g_ska.input_state.mouse_xrel = event.mouse_motion.xrel;
					g_ska.input_state.mouse_yrel = event.mouse_motion.yrel;

					ska_post_event(&event);
				}

				// Then send button down
				event.type = ska_event_mouse_button_down;
				event.mouse_button.window_id = window->id;
				event.mouse_button.button = ska_mouse_button_left;
				event.mouse_button.pressed = true;
				event.mouse_button.clicks = 1;
				event.mouse_button.x = (int32_t)x;
				event.mouse_button.y = (int32_t)y;

				g_ska.input_state.mouse_buttons |= (1 << (ska_mouse_button_left - 1));

				ska_post_event(&event);
				return 1;

			case AMOTION_EVENT_ACTION_UP:
				// Primary touch up = Mouse motion + Left button up
				// First send motion event to update position
				if ((int32_t)x != g_ska.input_state.mouse_x || (int32_t)y != g_ska.input_state.mouse_y) {
					event.type = ska_event_mouse_motion;
					event.mouse_motion.window_id = window->id;
					event.mouse_motion.x = (int32_t)x;
					event.mouse_motion.y = (int32_t)y;
					event.mouse_motion.xrel = (int32_t)x - g_ska.input_state.mouse_x;
					event.mouse_motion.yrel = (int32_t)y - g_ska.input_state.mouse_y;

					g_ska.input_state.mouse_x = (int32_t)x;
					g_ska.input_state.mouse_y = (int32_t)y;
					g_ska.input_state.mouse_xrel = event.mouse_motion.xrel;
					g_ska.input_state.mouse_yrel = event.mouse_motion.yrel;

					ska_post_event(&event);
				}

				// Then send button up
				event.type = ska_event_mouse_button_up;
				event.mouse_button.window_id = window->id;
				event.mouse_button.button = ska_mouse_button_left;
				event.mouse_button.pressed = false;
				event.mouse_button.clicks = 1;
				event.mouse_button.x = (int32_t)x;
				event.mouse_button.y = (int32_t)y;

				g_ska.input_state.mouse_buttons &= ~(1 << (ska_mouse_button_left - 1));

				ska_post_event(&event);
				return 1;

			case AMOTION_EVENT_ACTION_MOVE:
				// Touch/Mouse move = Mouse motion
				// Note: For touch, this only fires while touching. For mouse, this fires when dragging.
				event.type = ska_event_mouse_motion;
				event.mouse_motion.window_id = window->id;
				event.mouse_motion.x = (int32_t)x;
				event.mouse_motion.y = (int32_t)y;
				event.mouse_motion.xrel = (int32_t)x - g_ska.input_state.mouse_x;
				event.mouse_motion.yrel = (int32_t)y - g_ska.input_state.mouse_y;

				g_ska.input_state.mouse_x = (int32_t)x;
				g_ska.input_state.mouse_y = (int32_t)y;
				g_ska.input_state.mouse_xrel = event.mouse_motion.xrel;
				g_ska.input_state.mouse_yrel = event.mouse_motion.yrel;

				ska_post_event(&event);
				return 1;
		}
	}

	return 0;
}

bool ska_platform_init(void) {
	if (!g_ska.android_app) {
		ska_set_error("android_app not set - call ska_android_set_app() before ska_init()");
		return false;
	}

	// Set up callbacks
	g_ska.android_app->onAppCmd     = ska_android_handle_cmd;
	g_ska.android_app->onInputEvent = ska_android_handle_input;

	// Initialize scancode table
	ska_init_scancode_table();

	// Cache JNI method/field IDs for window management
	ska_jni_cache_init();

	ska_log(ska_log_info, "Android platform initialized");

	return true;
}

void ska_platform_shutdown(void) {
	ska_jni_cache_shutdown();

	if (g_ska.android_app) {
		g_ska.android_app->onAppCmd     = NULL;
		g_ska.android_app->onInputEvent = NULL;
	}

	ska_log(ska_log_info, "Android platform shutdown");
}

void* ska_android_get_vm(void) {
	if (g_ska.android_app && g_ska.android_app->activity) {
		return g_ska.android_app->activity->vm;
	}
	return NULL;
}

void* ska_android_get_activity(void) {
	if (g_ska.android_app && g_ska.android_app->activity) {
		return g_ska.android_app->activity->clazz;
	}
	return NULL;
}

bool ska_platform_window_create(
	ska_window_t* window,
	const char* title,
	int32_t x, int32_t y,
	int32_t w, int32_t h,
	uint32_t flags
) {
	(void)x; (void)y; (void)w; (void)h; (void)flags;

	// On Android, we don't create windows - the system provides one
	// We'll use the ANativeWindow when it becomes available

	window->title = strdup(title ? title : "sk_app");

	// Window dimensions will be set when we get APP_CMD_INIT_WINDOW
	window->native_window = NULL;
	window->is_visible = false;

	ska_log(ska_log_info, "Android window stub created, waiting for native window");

	// Wait for the native window to become available by polling the event loop
	// The android_main() thread will process APP_CMD_INIT_WINDOW and set window->native_window
	while (window->native_window == NULL) {
		ska_time_sleep(10);  // Sleep 10ms between checks to avoid busy-waiting

		// Safety check: if app is being destroyed, bail out
		if (g_ska.android_app && g_ska.android_app->destroyRequested) {
			ska_set_error("App destroy requested while waiting for window");
			return false;
		}
	}

	ska_log(ska_log_info, "Native window is now available: %dx%d", window->width, window->height);

	return true;
}

void ska_platform_window_destroy(ska_window_t* window) {
	// On Android, we don't destroy the window - the system manages it
	// Just clear our reference
	window->native_window = NULL;
}

void ska_platform_window_set_title(ska_window_t* window, const char* title) {
	// Android doesn't support changing window title at runtime
	if (window->title) {
		free(window->title);
	}
	window->title = strdup(title);
}

void ska_platform_window_set_frame_position(ska_window_t* window, int32_t x, int32_t y) {
	// In freeform/DEX mode, we can change window position via WindowManager.LayoutParams
	JNIEnv* env = ska_jni_get_env();
	if (!env) {
		return;
	}

	jobject jwindow = (*env)->CallObjectMethod(env, g_ska.android_app->activity->clazz, g_jni_cache.activity_getWindow);

	if (jwindow) {
		jobject layout_params = (*env)->CallObjectMethod(env, jwindow, g_jni_cache.window_getAttributes);

		if (layout_params) {
			(*env)->SetIntField(env, layout_params, g_jni_cache.lp_x, x);
			(*env)->SetIntField(env, layout_params, g_jni_cache.lp_y, y);

			(*env)->CallVoidMethod(env, jwindow, g_jni_cache.window_setAttributes, layout_params);

			window->x = x;
			window->y = y;

			(*env)->DeleteLocalRef(env, layout_params);
		}
		(*env)->DeleteLocalRef(env, jwindow);
	}
}

void ska_platform_window_set_frame_size(ska_window_t* window, int32_t w, int32_t h) {
	// In freeform/DEX mode, we can change window size via WindowManager.LayoutParams
	(void)window; // Size change reflected via APP_CMD_WINDOW_RESIZED callback

	JNIEnv* env = ska_jni_get_env();
	if (!env) {
		return;
	}

	jobject jwindow = (*env)->CallObjectMethod(env, g_ska.android_app->activity->clazz, g_jni_cache.activity_getWindow);

	if (jwindow) {
		jobject layout_params = (*env)->CallObjectMethod(env, jwindow, g_jni_cache.window_getAttributes);

		if (layout_params) {
			(*env)->SetIntField(env, layout_params, g_jni_cache.lp_width, w);
			(*env)->SetIntField(env, layout_params, g_jni_cache.lp_height, h);

			(*env)->CallVoidMethod(env, jwindow, g_jni_cache.window_setAttributes, layout_params);

			(*env)->DeleteLocalRef(env, layout_params);
		}
		(*env)->DeleteLocalRef(env, jwindow);
	}
}

void ska_platform_get_frame_extents(const ska_window_t* window, int32_t* out_left, int32_t* out_right, int32_t* out_top, int32_t* out_bottom) {
	// In freeform/DEX mode, windows may have decorations (title bar, borders)
	// We get these via getWindow().getDecorView() dimensions vs content view
	if (out_left)   *out_left   = 0;
	if (out_right)  *out_right  = 0;
	if (out_top)    *out_top    = 0;
	if (out_bottom) *out_bottom = 0;

	if (!window || !window->native_window) {
		return;
	}

	JNIEnv* env = ska_jni_get_env();
	if (!env) {
		return;
	}

	jobject jwindow = (*env)->CallObjectMethod(env, g_ska.android_app->activity->clazz, g_jni_cache.activity_getWindow);

	if (jwindow) {
		jobject decor_view = (*env)->CallObjectMethod(env, jwindow, g_jni_cache.window_getDecorView);

		if (decor_view) {
			jint decor_width  = (*env)->CallIntMethod(env, decor_view, g_jni_cache.view_getWidth);
			jint decor_height = (*env)->CallIntMethod(env, decor_view, g_jni_cache.view_getHeight);

			// Get content (client) area size from native window
			int32_t content_width  = ANativeWindow_getWidth(window->native_window);
			int32_t content_height = ANativeWindow_getHeight(window->native_window);

			// Frame extents = decor size - content size
			// For Android, decorations are typically only at the top (title bar)
			int32_t total_h_diff = decor_width - content_width;
			int32_t total_v_diff = decor_height - content_height;

			// Assume symmetric horizontal borders (if any)
			int32_t left = total_h_diff / 2;
			if (out_left)   *out_left   = left;
			if (out_right)  *out_right  = total_h_diff - left;
			// Title bar at top, no bottom border typically
			if (out_top)    *out_top    = total_v_diff;
			if (out_bottom) *out_bottom = 0;

			(*env)->DeleteLocalRef(env, decor_view);
		}
		(*env)->DeleteLocalRef(env, jwindow);
	}
}

void ska_platform_window_show(ska_window_t* window) {
	// Android windows are always visible when active
	window->is_visible = true;
}

void ska_platform_window_hide(ska_window_t* window) {
	// Cannot hide Android windows
	window->is_visible = false;
}

void ska_platform_window_maximize(ska_window_t* window) {
	// Android windows are always maximized
	(void)window;
}

void ska_platform_window_minimize(ska_window_t* window) {
	// Cannot minimize Android windows programmatically
	(void)window;
}

void ska_platform_window_restore(ska_window_t* window) {
	// No-op on Android
	(void)window;
}

void ska_platform_window_raise(ska_window_t* window) {
	// Android windows are always on top
	(void)window;
}

void ska_platform_window_get_drawable_size(ska_window_t* window, int32_t* opt_out_width, int32_t* opt_out_height) {
	// Drawable size equals window size on Android
	(void)window;
	(void)opt_out_width;
	(void)opt_out_height;
}

float ska_platform_get_dpi_scale(const ska_window_t* window) {
	(void)window;

	// Get display density from Android configuration
	if (g_ska.android_app && g_ska.android_app->config) {
		int32_t density = AConfiguration_getDensity(g_ska.android_app->config);
		if (density > 0 && density != ACONFIGURATION_DENSITY_NONE) {
			// Android uses 160 DPI as baseline (mdpi)
			return (float)density / 160.0f;
		}
	}

	return 1.0f;
}

float ska_platform_get_refresh_rate(const ska_window_t* window) {
	(void)window;

	JNIEnv* env = ska_jni_get_env();
	if (!env) {
		return 0.0f;
	}

	// Get WindowManager: activity.getWindowManager()
	jclass    activity_class     = (*env)->FindClass  (env, "android/app/NativeActivity");
	jmethodID get_window_manager = (*env)->GetMethodID(env, activity_class,
		"getWindowManager", "()Landroid/view/WindowManager;");

	jobject window_manager = (*env)->CallObjectMethod(env, g_ska.android_app->activity->clazz, get_window_manager);
	if (!window_manager) {
		return 0.0f;
	}

	// Get Display: windowManager.getDefaultDisplay()
	jclass    wm_class            = (*env)->FindClass  (env, "android/view/WindowManager");
	jmethodID get_default_display = (*env)->GetMethodID(env, wm_class, "getDefaultDisplay", "()Landroid/view/Display;");

	jobject display = (*env)->CallObjectMethod(env, window_manager, get_default_display);
	(*env)->DeleteLocalRef(env, window_manager);
	if (!display) {
		return 0.0f;
	}

	// Get refresh rate: display.getRefreshRate()
	jclass    display_class    = (*env)->FindClass  (env, "android/view/Display");
	jmethodID get_refresh_rate = (*env)->GetMethodID(env, display_class,
		"getRefreshRate", "()F");

	jfloat rate = (*env)->CallFloatMethod(env, display, get_refresh_rate);
	(*env)->DeleteLocalRef(env, display);

	return (float)rate;
}

void ska_platform_warp_mouse(ska_window_t* window, int32_t x, int32_t y) {
	// Cannot warp cursor on touchscreen
	(void)window; (void)x; (void)y;
}

void ska_platform_set_cursor(ska_system_cursor_ cursor) {
	// No cursor on touchscreen
	(void)cursor;
}

void ska_platform_show_cursor(bool show) {
	// No cursor on touchscreen
	(void)show;
}

bool ska_platform_set_relative_mouse_mode(bool enabled) {
	// Not applicable on touchscreen
	(void)enabled;
	return false;
}

// Forward declaration for file dialog check
static void ska_android_check_file_dialog(void);

void ska_platform_pump_events(void) {
	// On Android, events are already being pumped by the android_main() loop
	// in the main thread. The user's main() runs in a separate thread and
	// consumes events from the thread-safe event queue.
	// Calling ALooper_pollOnce() here would fail with "No looper for this thread!"
	// because the user thread doesn't have a looper attached.

	// Check for file dialog completion (requires Java helper for full implementation)
	ska_android_check_file_dialog();
}

/////////////////////////////////////////
// Android specific subset of Vulkan header
/////////////////////////////////////////

typedef VkFlags VkAndroidSurfaceCreateFlagsKHR;
typedef struct VkAndroidSurfaceCreateInfoKHR {
	VkStructureType                   sType;
	const void*                       pNext;
	VkAndroidSurfaceCreateFlagsKHR    flags;
	ANativeWindow*                    window;
} VkAndroidSurfaceCreateInfoKHR;

typedef VkResult (VKAPI_PTR *PFN_vkCreateAndroidSurfaceKHR)(VkInstance instance, const VkAndroidSurfaceCreateInfoKHR* pCreateInfo, const /*VkAllocationCallbacks*/ void* pAllocator, VkSurfaceKHR* pSurface);

/////////////////////////////////////////

const char** ska_platform_vk_get_instance_extensions(uint32_t* out_count) {
	static const char* extensions[] = {
		"VK_KHR_surface",
		"VK_KHR_android_surface"
	};
	*out_count = 2;
	return extensions;
}

bool ska_platform_vk_create_surface(const ska_window_t* window, VkInstance instance, VkSurfaceKHR* out_surface) {
	if (!window->native_window) {
		ska_set_error("Native window not available");
		return false;
	}

	void* module = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
	if (!module) {
		ska_set_error("Failed to load Vulkan .so");
		return false;
	}

	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(module, "vkGetInstanceProcAddr");
	if (!vkGetInstanceProcAddr) {
		ska_set_error("Failed to load vkGetInstanceProcAddr");
		return false;
	}

	PFN_vkCreateAndroidSurfaceKHR vkCreateAndroidSurfaceKHR = (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR");
	if (!vkCreateAndroidSurfaceKHR) {
		ska_set_error("Failed to load vkCreateAndroidSurfaceKHR");
		return false;
	}

	VkAndroidSurfaceCreateInfoKHR create_info = {0};
	create_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
	create_info.window = window->native_window;

	VkResult result = vkCreateAndroidSurfaceKHR(instance, &create_info, NULL, out_surface);
	if (result != VK_SUCCESS) {
		ska_set_error("Failed to create Vulkan Android surface: %d", result);
		return false;
	}

	return true;
}

// ========== Text Input Platform Functions ==========

void ska_platform_show_virtual_keyboard(bool visible, ska_text_input_type_ type) {
	// Android - JNI implementation to show/hide soft keyboard
	if (!g_ska.android_app) {
		return;
	}

	JNIEnv* env;
	JavaVM* jvm = g_ska.android_app->activity->vm;
	(*jvm)->AttachCurrentThread(jvm, &env, NULL);

	if (visible) {
		// Show soft keyboard using InputMethodManager
		jclass activity_class = (*env)->FindClass(env, "android/app/NativeActivity");
		jmethodID get_system_service = (*env)->GetMethodID(env, activity_class,
			"getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");

		jstring service_name = (*env)->NewStringUTF(env, "input_method");
		jobject imm = (*env)->CallObjectMethod(env, g_ska.android_app->activity->clazz,
			get_system_service, service_name);

		if (imm) {
			jclass imm_class = (*env)->FindClass(env, "android/view/inputmethod/InputMethodManager");
			jmethodID show_soft_input = (*env)->GetMethodID(env, imm_class,
				"showSoftInput", "(Landroid/view/View;I)Z");

			// Get the window's decor view
			jmethodID get_window = (*env)->GetMethodID(env, activity_class,
				"getWindow", "()Landroid/view/Window;");
			jobject window = (*env)->CallObjectMethod(env, g_ska.android_app->activity->clazz,
				get_window);

			if (window) {
				jclass window_class = (*env)->FindClass(env, "android/view/Window");
				jmethodID get_decor_view = (*env)->GetMethodID(env, window_class,
					"getDecorView", "()Landroid/view/View;");
				jobject decor_view = (*env)->CallObjectMethod(env, window, get_decor_view);

				if (decor_view) {
					(*env)->CallBooleanMethod(env, imm, show_soft_input, decor_view, 0);
					(*env)->DeleteLocalRef(env, decor_view);
				}
				(*env)->DeleteLocalRef(env, window);
			}
			(*env)->DeleteLocalRef(env, imm);
		}
		(*env)->DeleteLocalRef(env, service_name);
	} else {
		// Hide soft keyboard
		jclass activity_class = (*env)->FindClass(env, "android/app/NativeActivity");
		jmethodID get_system_service = (*env)->GetMethodID(env, activity_class,
			"getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");

		jstring service_name = (*env)->NewStringUTF(env, "input_method");
		jobject imm = (*env)->CallObjectMethod(env, g_ska.android_app->activity->clazz,
			get_system_service, service_name);

		if (imm) {
			jclass imm_class = (*env)->FindClass(env, "android/view/inputmethod/InputMethodManager");
			jmethodID hide_soft_input = (*env)->GetMethodID(env, imm_class,
				"hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z");

			// Get window token
			jmethodID get_window = (*env)->GetMethodID(env, activity_class,
				"getWindow", "()Landroid/view/Window;");
			jobject window = (*env)->CallObjectMethod(env, g_ska.android_app->activity->clazz,
				get_window);

			if (window) {
				jclass window_class = (*env)->FindClass(env, "android/view/Window");
				jmethodID get_decor_view = (*env)->GetMethodID(env, window_class,
					"getDecorView", "()Landroid/view/View;");
				jobject decor_view = (*env)->CallObjectMethod(env, window, get_decor_view);

				if (decor_view) {
					jclass view_class = (*env)->FindClass(env, "android/view/View");
					jmethodID get_window_token = (*env)->GetMethodID(env, view_class,
						"getWindowToken", "()Landroid/os/IBinder;");
					jobject token = (*env)->CallObjectMethod(env, decor_view, get_window_token);

					if (token) {
						(*env)->CallBooleanMethod(env, imm, hide_soft_input, token, 0);
						(*env)->DeleteLocalRef(env, token);
					}
					(*env)->DeleteLocalRef(env, decor_view);
				}
				(*env)->DeleteLocalRef(env, window);
			}
			(*env)->DeleteLocalRef(env, imm);
		}
		(*env)->DeleteLocalRef(env, service_name);
	}

	(*jvm)->DetachCurrentThread(jvm);
	(void)type; // TODO: Use type to set input mode
}

// ========== Android Entry Point ==========

//
// Forward declaration of user's main function.
// Users write a standard main(int argc, char** argv) just like on desktop.
extern int32_t main(int argc, char** argv);

//
// Android native activity entry point.
// This is provided by the library - users do NOT define this.
//
// The library automatically:
// 1. Sets up the android_app pointer
// 2. Waits for the window to be ready
// 3. Calls the user's main() function in a separate thread
// 4. Manages the Android event loop

#include <pthread.h>

typedef struct {
	struct android_app* app;
	bool user_main_finished;
	int32_t user_main_result;
} android_main_state_t;

static android_main_state_t g_android_main_state = {0};

// Thread function that runs the user's main()
static void* ska_android_user_main_thread(void* arg) {
	(void)arg;

	// Call user's main function with empty args
	char* argv[] = {"sk_app", NULL};
	g_android_main_state.user_main_result = main(1, argv);
	g_android_main_state.user_main_finished = true;

	// Request app destruction when main() returns
	ANativeActivity_finish(g_android_main_state.app->activity);

	return NULL;
}

// ========== Clipboard Platform Functions ==========

char* ska_platform_clipboard_get_text(void) {
	if (!g_ska.android_app) {
		return NULL;
	}

	JNIEnv* env;
	JavaVM* jvm = g_ska.android_app->activity->vm;
	(*jvm)->AttachCurrentThread(jvm, &env, NULL);

	// Get ClipboardManager
	jclass activity_class = (*env)->FindClass(env, "android/app/NativeActivity");
	jmethodID get_system_service = (*env)->GetMethodID(env, activity_class,
		"getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");

	jstring service_name = (*env)->NewStringUTF(env, "clipboard");
	jobject clipboard_manager = (*env)->CallObjectMethod(env, g_ska.android_app->activity->clazz,
		get_system_service, service_name);

	if (!clipboard_manager) {
		(*jvm)->DetachCurrentThread(jvm);
		return NULL;
	}

	// Check if clipboard has text
	jclass clipboard_class = (*env)->FindClass(env, "android/content/ClipboardManager");
	jmethodID has_primary_clip = (*env)->GetMethodID(env, clipboard_class,
		"hasPrimaryClip", "()Z");
	jboolean has_clip = (*env)->CallBooleanMethod(env, clipboard_manager, has_primary_clip);

	if (!has_clip) {
		(*jvm)->DetachCurrentThread(jvm);
		return NULL;
	}

	// Get primary clip
	jmethodID get_primary_clip = (*env)->GetMethodID(env, clipboard_class,
		"getPrimaryClip", "()Landroid/content/ClipData;");
	jobject clip_data = (*env)->CallObjectMethod(env, clipboard_manager, get_primary_clip);

	if (!clip_data) {
		(*jvm)->DetachCurrentThread(jvm);
		return NULL;
	}

	// Get first item
	jclass clip_data_class = (*env)->FindClass(env, "android/content/ClipData");
	jmethodID get_item_at = (*env)->GetMethodID(env, clip_data_class,
		"getItemAt", "(I)Landroid/content/ClipData$Item;");
	jobject clip_item = (*env)->CallObjectMethod(env, clip_data, get_item_at, 0);

	if (!clip_item) {
		(*jvm)->DetachCurrentThread(jvm);
		return NULL;
	}

	// Get text from item
	jclass clip_item_class = (*env)->FindClass(env, "android/content/ClipData$Item");
	jmethodID get_text = (*env)->GetMethodID(env, clip_item_class,
		"getText", "()Ljava/lang/CharSequence;");
	jobject char_sequence = (*env)->CallObjectMethod(env, clip_item, get_text);

	if (!char_sequence) {
		(*jvm)->DetachCurrentThread(jvm);
		return NULL;
	}

	// Convert CharSequence to String
	jclass char_sequence_class = (*env)->FindClass(env, "java/lang/CharSequence");
	jmethodID to_string = (*env)->GetMethodID(env, char_sequence_class,
		"toString", "()Ljava/lang/String;");
	jstring text_string = (jstring)(*env)->CallObjectMethod(env, char_sequence, to_string);

	if (!text_string) {
		(*jvm)->DetachCurrentThread(jvm);
		return NULL;
	}

	// Convert to UTF-8
	const char* utf8_text = (*env)->GetStringUTFChars(env, text_string, NULL);
	if (!utf8_text) {
		(*jvm)->DetachCurrentThread(jvm);
		return NULL;
	}

	// Allocate and copy the text
	size_t len = strlen(utf8_text);
	char* result = (char*)malloc(len + 1);
	if (result) {
		memcpy(result, utf8_text, len + 1);
	}

	(*env)->ReleaseStringUTFChars(env, text_string, utf8_text);
	(*jvm)->DetachCurrentThread(jvm);

	return result;
}

bool ska_platform_clipboard_set_text(const char* text) {
	if (!g_ska.android_app || !text) {
		ska_set_error("ska_platform_clipboard_set_text: invalid app or text");
		return false;
	}

	JNIEnv* env;
	JavaVM* jvm = g_ska.android_app->activity->vm;
	(*jvm)->AttachCurrentThread(jvm, &env, NULL);

	// Get ClipboardManager
	jclass activity_class = (*env)->FindClass(env, "android/app/NativeActivity");
	jmethodID get_system_service = (*env)->GetMethodID(env, activity_class,
		"getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");

	jstring service_name = (*env)->NewStringUTF(env, "clipboard");
	jobject clipboard_manager = (*env)->CallObjectMethod(env, g_ska.android_app->activity->clazz,
		get_system_service, service_name);

	if (!clipboard_manager) {
		(*jvm)->DetachCurrentThread(jvm);
		ska_set_error("ska_platform_clipboard_set_text: failed to get clipboard manager");
		return false;
	}

	// Create ClipData
	jclass clip_data_class = (*env)->FindClass(env, "android/content/ClipData");
	jmethodID new_plain_text = (*env)->GetStaticMethodID(env, clip_data_class,
		"newPlainText", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Landroid/content/ClipData;");

	jstring label = (*env)->NewStringUTF(env, "text");
	jstring text_string = (*env)->NewStringUTF(env, text);

	jobject clip_data = (*env)->CallStaticObjectMethod(env, clip_data_class,
		new_plain_text, label, text_string);

	if (!clip_data) {
		(*jvm)->DetachCurrentThread(jvm);
		ska_set_error("ska_platform_clipboard_set_text: failed to create clip data");
		return false;
	}

	// Set primary clip
	jclass clipboard_class = (*env)->FindClass(env, "android/content/ClipboardManager");
	jmethodID set_primary_clip = (*env)->GetMethodID(env, clipboard_class,
		"setPrimaryClip", "(Landroid/content/ClipData;)V");

	(*env)->CallVoidMethod(env, clipboard_manager, set_primary_clip, clip_data);

	(*jvm)->DetachCurrentThread(jvm);
	return true;
}

void android_main(struct android_app* app) {
	// Set the android_app pointer in the global state
	g_ska.android_app = app;
	g_android_main_state.app = app;
	g_android_main_state.user_main_finished = false;
	g_android_main_state.user_main_result = 0;

	pthread_t user_thread;
	bool user_thread_started = false;

	// Start user's main() thread immediately
	// It will wait for the window to become available in ska_platform_window_create()
	pthread_create(&user_thread, NULL, ska_android_user_main_thread, NULL);
	user_thread_started = true;

	// Main event loop
	while (1) {
		int32_t events;
		struct android_poll_source* source;

		// Poll for events (non-blocking since user thread is running)
		while (ALooper_pollOnce(0, NULL, &events, (void**)&source) >= 0) {
			// Process this event
			if (source != NULL) {
				source->process(app, source);
			}

			// Check if we are exiting
			if (app->destroyRequested != 0) {
				if (user_thread_started && !g_android_main_state.user_main_finished) {
					// Wait for user thread to finish
					pthread_join(user_thread, NULL);
				}
				return;
			}
		}

		// If user's main finished, exit
		if (g_android_main_state.user_main_finished) {
			pthread_join(user_thread, NULL);
			return;
		}

		// Small sleep to avoid busy-waiting
		ska_time_sleep(1);
	}
}

// ========== Asset I/O (Android) ==========

SKA_API bool ska_asset_read(const char* asset_name, void** out_data, size_t* out_size) {
	if (!asset_name) {
		ska_set_error("ska_asset_read: NULL asset_name");
		return false;
	}

	if (!out_data) {
		ska_set_error("ska_asset_read: NULL out_data");
		return false;
	}

	if (!g_ska.android_app || !g_ska.android_app->activity || !g_ska.android_app->activity->assetManager) {
		ska_set_error("ska_asset_read: AAssetManager not available");
		return false;
	}

	AAssetManager* asset_manager = g_ska.android_app->activity->assetManager;

	AAsset* asset = AAssetManager_open(asset_manager, asset_name, AASSET_MODE_BUFFER);
	if (!asset) {
		ska_set_error("ska_asset_read: Failed to open asset '%s'", asset_name);
		return false;
	}

	off_t asset_length = AAsset_getLength(asset);
	if (asset_length < 0) {
		ska_set_error("ska_asset_read: Failed to get asset length for '%s'", asset_name);
		AAsset_close(asset);
		return false;
	}

	void* data = malloc((size_t)asset_length);
	if (!data) {
		ska_set_error("ska_asset_read: Failed to allocate %ld bytes", (long)asset_length);
		AAsset_close(asset);
		return false;
	}

	int bytes_read = AAsset_read(asset, data, (size_t)asset_length);
	AAsset_close(asset);

	if (bytes_read != asset_length) {
		ska_set_error("ska_asset_read: Read %d bytes, expected %ld", bytes_read, (long)asset_length);
		free(data);
		return false;
	}

	*out_data = data;
	if (out_size) {
		*out_size = (size_t)asset_length;
	}

	return true;
}

SKA_API bool ska_asset_read_text(const char* asset_name, char** out_text) {
	if (!asset_name) {
		ska_set_error("ska_asset_read_text: NULL asset_name");
		return false;
	}

	if (!out_text) {
		ska_set_error("ska_asset_read_text: NULL out_text");
		return false;
	}

	size_t file_size = 0;
	void* data = NULL;
	if (!ska_asset_read(asset_name, &data, &file_size)) {
		return false;
	}

	// Allocate +1 for null terminator
	char* text = (char*)realloc(data, file_size + 1);
	if (!text) {
		ska_set_error("ska_asset_read_text: Failed to allocate null terminator");
		free(data);
		return false;
	}

	text[file_size] = '\0';
	*out_text = text;
	return true;
}

// ============================================================================
// File Dialog
// ============================================================================

// Cached JNI references for file dialog
static struct {
	jclass    intent_class;
	jmethodID intent_init;
	jmethodID intent_setType;
	jmethodID intent_addCategory;
	jmethodID intent_putExtra_bool;
	jclass    pm_class;
	jmethodID pm_queryIntentActivities;
	jmethodID activity_getPackageManager;
	jmethodID activity_startActivityForResult;
	bool      initialized;
} g_file_dialog_jni = {0};

// Pending file dialog state
typedef struct {
	ska_file_dialog_id_t id;
	char*                title;
	bool                 active;
} ska_android_file_dialog_t;

static ska_android_file_dialog_t g_android_file_dialog = {0};

static void ska_file_dialog_jni_init(void) {
	if (g_file_dialog_jni.initialized) return;

	JNIEnv* env = ska_jni_get_env();
	if (!env) return;

	// Intent class and methods
	jclass intent_class = (*env)->FindClass(env, "android/content/Intent");
	g_file_dialog_jni.intent_class = (*env)->NewGlobalRef(env, intent_class);
	g_file_dialog_jni.intent_init = (*env)->GetMethodID(env, intent_class, "<init>", "(Ljava/lang/String;)V");
	g_file_dialog_jni.intent_setType = (*env)->GetMethodID(env, intent_class, "setType", "(Ljava/lang/String;)Landroid/content/Intent;");
	g_file_dialog_jni.intent_addCategory = (*env)->GetMethodID(env, intent_class, "addCategory", "(Ljava/lang/String;)Landroid/content/Intent;");
	g_file_dialog_jni.intent_putExtra_bool = (*env)->GetMethodID(env, intent_class, "putExtra", "(Ljava/lang/String;Z)Landroid/content/Intent;");

	// PackageManager
	jclass pm_class = (*env)->FindClass(env, "android/content/pm/PackageManager");
	g_file_dialog_jni.pm_class = (*env)->NewGlobalRef(env, pm_class);
	g_file_dialog_jni.pm_queryIntentActivities = (*env)->GetMethodID(env, pm_class, "queryIntentActivities", "(Landroid/content/Intent;I)Ljava/util/List;");

	// Activity methods
	jclass activity_class = (*env)->FindClass(env, "android/app/Activity");
	g_file_dialog_jni.activity_getPackageManager = (*env)->GetMethodID(env, activity_class, "getPackageManager", "()Landroid/content/pm/PackageManager;");
	g_file_dialog_jni.activity_startActivityForResult = (*env)->GetMethodID(env, activity_class, "startActivityForResult", "(Landroid/content/Intent;I)V");

	g_file_dialog_jni.initialized = true;
}

bool ska_platform_file_dialog_available(ska_file_dialog_ type) {
	if (!g_ska.android_app || !g_ska.android_app->activity) {
		return false;
	}

	ska_file_dialog_jni_init();
	JNIEnv* env = ska_jni_get_env();
	if (!env || !g_file_dialog_jni.initialized) {
		return false;
	}

	// Create the appropriate intent action string
	const char* action;
	switch (type) {
		case ska_file_dialog_open:
			action = "android.intent.action.OPEN_DOCUMENT";
			break;
		case ska_file_dialog_save:
			action = "android.intent.action.CREATE_DOCUMENT";
			break;
		case ska_file_dialog_open_folder:
			action = "android.intent.action.OPEN_DOCUMENT_TREE";
			break;
		default:
			return false;
	}

	// Create Intent
	jstring action_str = (*env)->NewStringUTF(env, action);
	jobject intent = (*env)->NewObject(env, g_file_dialog_jni.intent_class, g_file_dialog_jni.intent_init, action_str);
	(*env)->DeleteLocalRef(env, action_str);

	if (!intent) {
		return false;
	}

	// Set type to */* for general query
	jstring type_str = (*env)->NewStringUTF(env, "*/*");
	(*env)->CallObjectMethod(env, intent, g_file_dialog_jni.intent_setType, type_str);
	(*env)->DeleteLocalRef(env, type_str);

	// Add CATEGORY_OPENABLE
	jstring category_str = (*env)->NewStringUTF(env, "android.intent.category.OPENABLE");
	(*env)->CallObjectMethod(env, intent, g_file_dialog_jni.intent_addCategory, category_str);
	(*env)->DeleteLocalRef(env, category_str);

	// Get PackageManager and query
	jobject activity = g_ska.android_app->activity->clazz;
	jobject pm = (*env)->CallObjectMethod(env, activity, g_file_dialog_jni.activity_getPackageManager);

	if (!pm) {
		(*env)->DeleteLocalRef(env, intent);
		return false;
	}

	// Query activities that can handle this intent
	jobject resolve_list = (*env)->CallObjectMethod(env, pm, g_file_dialog_jni.pm_queryIntentActivities, intent, 0);

	bool available = false;
	if (resolve_list) {
		// Check if list is not empty
		jclass list_class = (*env)->FindClass(env, "java/util/List");
		jmethodID size_method = (*env)->GetMethodID(env, list_class, "size", "()I");
		int size = (*env)->CallIntMethod(env, resolve_list, size_method);
		available = (size > 0);
		(*env)->DeleteLocalRef(env, resolve_list);
	}

	(*env)->DeleteLocalRef(env, pm);
	(*env)->DeleteLocalRef(env, intent);

	return available;
}

bool ska_platform_file_dialog_show(ska_file_dialog_id_t id, const ska_file_dialog_request_t* request) {
	// Launch file picker using startActivityForResult.
	// The result is received in SkAppActivity.onActivityResult() which calls
	// our JNI callback nativeOnFileDialogResult().
	//
	// IMPORTANT: This requires the app to use SkAppActivity instead of NativeActivity
	// in AndroidManifest.xml. See AndroidFileDialog.cmake for setup instructions.

	if (g_android_file_dialog.active) {
		ska_set_error("File dialog already active");
		return false;
	}

	ska_file_dialog_jni_init();
	JNIEnv* env = ska_jni_get_env();
	if (!env || !g_file_dialog_jni.initialized) {
		ska_set_error("JNI not initialized for file dialog");
		return false;
	}

	// Determine intent action
	const char* action;
	switch (request->type) {
		case ska_file_dialog_open:
			action = "android.intent.action.OPEN_DOCUMENT";
			break;
		case ska_file_dialog_save:
			action = "android.intent.action.CREATE_DOCUMENT";
			break;
		case ska_file_dialog_open_folder:
			action = "android.intent.action.OPEN_DOCUMENT_TREE";
			break;
		default:
			ska_set_error("Unknown file dialog type");
			return false;
	}

	// Create Intent
	jstring action_str = (*env)->NewStringUTF(env, action);
	jobject intent = (*env)->NewObject(env, g_file_dialog_jni.intent_class, g_file_dialog_jni.intent_init, action_str);
	(*env)->DeleteLocalRef(env, action_str);

	if (!intent) {
		ska_set_error("Failed to create file picker intent");
		return false;
	}

	// Set MIME type
	const char* mime_type = "*/*";
	if (request->filters && request->filter_count > 0) {
		// Use first filter's MIME type
		mime_type = ska_filter_get_mime(&request->filters[0]);
	}
	jstring type_str = (*env)->NewStringUTF(env, mime_type);
	(*env)->CallObjectMethod(env, intent, g_file_dialog_jni.intent_setType, type_str);
	(*env)->DeleteLocalRef(env, type_str);

	// Add CATEGORY_OPENABLE for file intents (not folder)
	if (request->type != ska_file_dialog_open_folder) {
		jstring category_str = (*env)->NewStringUTF(env, "android.intent.category.OPENABLE");
		(*env)->CallObjectMethod(env, intent, g_file_dialog_jni.intent_addCategory, category_str);
		(*env)->DeleteLocalRef(env, category_str);
	}

	// Allow multiple selection if requested
	if (request->allow_multiple && request->type == ska_file_dialog_open) {
		jstring extra_str = (*env)->NewStringUTF(env, "android.intent.extra.ALLOW_MULTIPLE");
		(*env)->CallObjectMethod(env, intent, g_file_dialog_jni.intent_putExtra_bool, extra_str, JNI_TRUE);
		(*env)->DeleteLocalRef(env, extra_str);
	}

	// Allocate result entry (must be done before launching activity so JNI callback can find it)
	ska_file_dialog_result_t* result = ska_file_dialog_result_alloc(id, request->title);
	if (!result) {
		(*env)->DeleteLocalRef(env, intent);
		ska_set_error("Failed to allocate file dialog result");
		return false;
	}

	// Store state
	g_android_file_dialog.id = id;
	g_android_file_dialog.title = request->title ? strdup(request->title) : NULL;
	g_android_file_dialog.active = true;

	// Launch the activity with result
	// Request code encodes our dialog ID with a marker (0x5B00 prefix)
	// Lower byte is the dialog ID (0-255), upper byte is the marker
	int request_code = 0x5B00 | (id & 0xFF);

	jobject activity = g_ska.android_app->activity->clazz;
	(*env)->CallVoidMethod(env, activity, g_file_dialog_jni.activity_startActivityForResult, intent, request_code);
	(*env)->DeleteLocalRef(env, intent);

	// Check for exception
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
		g_android_file_dialog.active = false;
		if (g_android_file_dialog.title) {
			free(g_android_file_dialog.title);
			g_android_file_dialog.title = NULL;
		}
		ska_set_error("Failed to start file picker activity");
		return false;
	}

	ska_log(ska_log_info, "File dialog launched (id=%u)", id);

	return true;
}

// Called from ska_platform_pump_events - currently a no-op
// Full implementation requires Java callback to set result
static void ska_android_check_file_dialog(void) {
	// Without Java helper, we can't receive activity results
	// If dialog is active for too long without result, assume it was cancelled
	// (This is a workaround - proper implementation needs Java integration)
}

// ============================================================================
// JNI Callback for File Dialog Results (called from SkAppActivity.java)
// ============================================================================

#include <jni.h>

// JNI function name: Java_net_stereokit_sk_1app_SkAppActivity_nativeOnFileDialogResult
// Note: underscore in package name "sk_app" is escaped as "_1" in JNI
JNIEXPORT void JNICALL
Java_net_stereokit_sk_1app_SkAppActivity_nativeOnFileDialogResult(
	JNIEnv* env,
	jclass clazz,
	jint dialog_id,
	jobjectArray uris,
	jboolean cancelled
) {
	(void)clazz;

	ska_log(ska_log_info, "JNI callback: dialog_id=%d, cancelled=%d", dialog_id, cancelled);

	// Find the pending result for this dialog
	ska_file_dialog_result_t* result = NULL;
	for (int32_t i = 0; i < g_ska_file_dialog.result_count; i++) {
		if (g_ska_file_dialog.results[i].id == (ska_file_dialog_id_t)dialog_id) {
			result = &g_ska_file_dialog.results[i];
			break;
		}
	}

	if (!result) {
		ska_log(ska_log_warn, "File dialog result for unknown dialog ID %d", dialog_id);
		return;
	}

	// Process URIs if not cancelled
	if (!cancelled && uris != NULL) {
		jsize uri_count = (*env)->GetArrayLength(env, uris);

		for (jsize i = 0; i < uri_count; i++) {
			jstring uri_jstr = (jstring)(*env)->GetObjectArrayElement(env, uris, i);
			if (uri_jstr) {
				const char* uri_utf = (*env)->GetStringUTFChars(env, uri_jstr, NULL);
				if (uri_utf) {
					ska_file_dialog_result_add_path(result, uri_utf);
					(*env)->ReleaseStringUTFChars(env, uri_jstr, uri_utf);
				}
				(*env)->DeleteLocalRef(env, uri_jstr);
			}
		}
	}

	// Mark complete and post event
	ska_file_dialog_result_complete(result, cancelled);

	// Clear pending state
	g_android_file_dialog.active = false;
	if (g_android_file_dialog.title) {
		free(g_android_file_dialog.title);
		g_android_file_dialog.title = NULL;
	}

	ska_log(ska_log_info, "File dialog completed: %d paths, cancelled=%d",
		result->path_count, result->cancelled);
}

// ============================================================================
// KVP Store (Android: SharedPreferences with Base64 encoding)
// ============================================================================

// JNI cache for kvpstore
static struct {
	jmethodID activity_getSharedPreferences;
	jmethodID prefs_getString;
	jmethodID prefs_edit;
	jmethodID editor_putString;
	jmethodID editor_remove;
	jmethodID editor_apply;
	jclass    base64_class;
	jmethodID base64_encodeToString;
	jmethodID base64_decode;
	bool      initialized;
} g_kvp_jni = {0};

static bool ska_kvpstore_jni_init(void) {
	if (g_kvp_jni.initialized) return true;

	JNIEnv* env = ska_jni_get_env();
	if (!env) return false;

	// NativeActivity.getSharedPreferences(String name, int mode)
	jclass activity_class = (*env)->FindClass(env, "android/app/NativeActivity");
	g_kvp_jni.activity_getSharedPreferences = (*env)->GetMethodID(
		env, activity_class, "getSharedPreferences",
		"(Ljava/lang/String;I)Landroid/content/SharedPreferences;"
	);

	// SharedPreferences methods
	jclass prefs_class = (*env)->FindClass(env, "android/content/SharedPreferences");
	g_kvp_jni.prefs_getString = (*env)->GetMethodID(
		env, prefs_class, "getString",
		"(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"
	);
	g_kvp_jni.prefs_edit = (*env)->GetMethodID(
		env, prefs_class, "edit",
		"()Landroid/content/SharedPreferences$Editor;"
	);

	// SharedPreferences.Editor methods
	jclass editor_class = (*env)->FindClass(env, "android/content/SharedPreferences$Editor");
	g_kvp_jni.editor_putString = (*env)->GetMethodID(
		env, editor_class, "putString",
		"(Ljava/lang/String;Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;"
	);
	g_kvp_jni.editor_remove = (*env)->GetMethodID(
		env, editor_class, "remove",
		"(Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;"
	);
	g_kvp_jni.editor_apply = (*env)->GetMethodID(
		env, editor_class, "apply", "()V"
	);

	// Base64 for encoding binary data
	jclass base64_local = (*env)->FindClass(env, "android/util/Base64");
	g_kvp_jni.base64_class = (*env)->NewGlobalRef(env, base64_local);
	(*env)->DeleteLocalRef(env, base64_local);

	g_kvp_jni.base64_encodeToString = (*env)->GetStaticMethodID(
		env, g_kvp_jni.base64_class, "encodeToString",
		"([BI)Ljava/lang/String;"
	);
	g_kvp_jni.base64_decode = (*env)->GetStaticMethodID(
		env, g_kvp_jni.base64_class, "decode",
		"(Ljava/lang/String;I)[B"
	);

	g_kvp_jni.initialized = true;
	return true;
}

SKA_API bool ska_kvpstore_save(const char* key, const void* data, size_t size) {
	if (!ska_kvpstore_validate_key(key)) return false;
	if (!data && size > 0) {
		ska_set_error("ska_kvpstore_save: NULL data with non-zero size");
		return false;
	}

	if (!g_ska.android_app || !g_ska.android_app->activity) {
		ska_set_error("ska_kvpstore: android activity not available");
		return false;
	}

	if (!ska_kvpstore_jni_init()) {
		ska_set_error("ska_kvpstore: JNI initialization failed");
		return false;
	}

	JNIEnv* env = ska_jni_get_env();
	if (!env) return false;

	jobject activity = g_ska.android_app->activity->clazz;

	// Get SharedPreferences
	jstring prefs_name = (*env)->NewStringUTF(env, ska_kvpstore_get_app_name());
	jobject prefs = (*env)->CallObjectMethod(env, activity,
		g_kvp_jni.activity_getSharedPreferences, prefs_name, 0 /* MODE_PRIVATE */);

	// Encode data to Base64
	jbyteArray byte_array = (*env)->NewByteArray(env, (jsize)size);
	if (data && size > 0) {
		(*env)->SetByteArrayRegion(env, byte_array, 0, (jsize)size, (const jbyte*)data);
	}

	jstring encoded = (*env)->CallStaticObjectMethod(env, g_kvp_jni.base64_class,
		g_kvp_jni.base64_encodeToString, byte_array, 0 /* DEFAULT */);

	// Save to SharedPreferences
	jstring jkey = (*env)->NewStringUTF(env, key);
	jobject editor = (*env)->CallObjectMethod(env, prefs, g_kvp_jni.prefs_edit);
	(*env)->CallObjectMethod(env, editor, g_kvp_jni.editor_putString, jkey, encoded);
	(*env)->CallVoidMethod(env, editor, g_kvp_jni.editor_apply);

	// Cleanup local refs
	(*env)->DeleteLocalRef(env, prefs_name);
	(*env)->DeleteLocalRef(env, prefs);
	(*env)->DeleteLocalRef(env, byte_array);
	(*env)->DeleteLocalRef(env, encoded);
	(*env)->DeleteLocalRef(env, jkey);
	(*env)->DeleteLocalRef(env, editor);

	return true;
}

SKA_API bool ska_kvpstore_load(const char* key, void* opt_buffer, size_t buffer_size, size_t* opt_out_size) {
	if (!ska_kvpstore_validate_key(key)) return false;

	if (!g_ska.android_app || !g_ska.android_app->activity) {
		ska_set_error("ska_kvpstore: android activity not available");
		return false;
	}

	if (!ska_kvpstore_jni_init()) {
		ska_set_error("ska_kvpstore: JNI initialization failed");
		return false;
	}

	JNIEnv* env = ska_jni_get_env();
	if (!env) return false;

	jobject activity = g_ska.android_app->activity->clazz;

	// Get SharedPreferences
	jstring prefs_name = (*env)->NewStringUTF(env, ska_kvpstore_get_app_name());
	jobject prefs = (*env)->CallObjectMethod(env, activity,
		g_kvp_jni.activity_getSharedPreferences, prefs_name, 0);

	// Get stored string
	jstring jkey = (*env)->NewStringUTF(env, key);
	jstring encoded = (*env)->CallObjectMethod(env, prefs, g_kvp_jni.prefs_getString, jkey, NULL);

	if (!encoded) {
		(*env)->DeleteLocalRef(env, prefs_name);
		(*env)->DeleteLocalRef(env, prefs);
		(*env)->DeleteLocalRef(env, jkey);
		return false;
	}

	// Decode Base64
	jbyteArray decoded = (*env)->CallStaticObjectMethod(env, g_kvp_jni.base64_class,
		g_kvp_jni.base64_decode, encoded, 0);

	jsize data_size = (*env)->GetArrayLength(env, decoded);

	if (opt_out_size) {
		*opt_out_size = (size_t)data_size;
	}

	// Copy to buffer if provided
	if (opt_buffer && buffer_size > 0) {
		jsize copy_size = (buffer_size < (size_t)data_size) ? (jsize)buffer_size : data_size;
		(*env)->GetByteArrayRegion(env, decoded, 0, copy_size, (jbyte*)opt_buffer);
	}

	// Cleanup
	(*env)->DeleteLocalRef(env, prefs_name);
	(*env)->DeleteLocalRef(env, prefs);
	(*env)->DeleteLocalRef(env, jkey);
	(*env)->DeleteLocalRef(env, encoded);
	(*env)->DeleteLocalRef(env, decoded);

	return true;
}

SKA_API bool ska_kvpstore_delete(const char* key) {
	if (!ska_kvpstore_validate_key(key)) return false;

	if (!g_ska.android_app || !g_ska.android_app->activity) {
		ska_set_error("ska_kvpstore: android activity not available");
		return false;
	}

	if (!ska_kvpstore_jni_init()) {
		ska_set_error("ska_kvpstore: JNI initialization failed");
		return false;
	}

	JNIEnv* env = ska_jni_get_env();
	if (!env) return false;

	jobject activity = g_ska.android_app->activity->clazz;

	// Get SharedPreferences
	jstring prefs_name = (*env)->NewStringUTF(env, ska_kvpstore_get_app_name());
	jobject prefs = (*env)->CallObjectMethod(env, activity,
		g_kvp_jni.activity_getSharedPreferences, prefs_name, 0);

	// Remove key
	jstring jkey = (*env)->NewStringUTF(env, key);
	jobject editor = (*env)->CallObjectMethod(env, prefs, g_kvp_jni.prefs_edit);
	(*env)->CallObjectMethod(env, editor, g_kvp_jni.editor_remove, jkey);
	(*env)->CallVoidMethod(env, editor, g_kvp_jni.editor_apply);

	// Cleanup
	(*env)->DeleteLocalRef(env, prefs_name);
	(*env)->DeleteLocalRef(env, prefs);
	(*env)->DeleteLocalRef(env, jkey);
	(*env)->DeleteLocalRef(env, editor);

	return true;
}

#endif // SKA_PLATFORM_ANDROID
