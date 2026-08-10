//
// sk_app - Web (Emscripten) platform implementation
//
// A window maps to an HTML canvas element. The first window binds to the
// page's default canvas (Module.canvas / #canvas) when one exists; additional
// windows create their own canvas elements appended to <body>. Browser events
// are delivered through emscripten/html5.h callbacks, which push into the
// shared ska_event_t queue just like the other platform backends.
//
// Headless friendly: without a DOM (e.g. running under node), init succeeds
// and everything except window creation keeps working, mirroring the headless
// X11 behavior on Linux.

#include "ska_internal.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <math.h>

// ============================================================================
// Local State
// ============================================================================

static ska_system_cursor_ g_web_cursor          = ska_system_cursor_arrow;
static bool               g_web_cursor_visible  = true;
static bool               g_web_pointer_locked  = false;
static char*              g_web_clipboard_cache = NULL;

static bool ska_web_has_dom(void) {
	return EM_ASM_INT({ return (typeof document !== 'undefined') ? 1 : 0; }) != 0;
}

// ============================================================================
// Scancode Mapping (KeyboardEvent.code -> ska_scancode_)
// ============================================================================

static ska_scancode_ ska_web_code_to_scancode(const char* code) {
	// Letters: "KeyA".."KeyZ"
	if (code[0] == 'K' && code[1] == 'e' && code[2] == 'y' &&
	    code[3] >= 'A' && code[3] <= 'Z' && code[4] == '\0') {
		return (ska_scancode_)(ska_scancode_a + (code[3] - 'A'));
	}

	// Numbers: "Digit0".."Digit9"
	if (strncmp(code, "Digit", 5) == 0 && code[5] >= '0' && code[5] <= '9' && code[6] == '\0') {
		return code[5] == '0' ? ska_scancode_0
		                      : (ska_scancode_)(ska_scancode_1 + (code[5] - '1'));
	}

	// Function keys: "F1".."F12"
	if (code[0] == 'F' && code[1] >= '1' && code[1] <= '9') {
		if (code[2] == '\0') {
			return (ska_scancode_)(ska_scancode_f1 + (code[1] - '1'));
		}
		if (code[1] == '1' && code[2] >= '0' && code[2] <= '2' && code[3] == '\0') {
			return (ska_scancode_)(ska_scancode_f10 + (code[2] - '0'));
		}
	}

	static const struct {
		const char*   code;
		ska_scancode_ scancode;
	} named[] = {
		{ "Enter",          ska_scancode_return       },
		{ "NumpadEnter",    ska_scancode_return       },
		{ "Escape",         ska_scancode_escape       },
		{ "Backspace",      ska_scancode_backspace    },
		{ "Tab",            ska_scancode_tab          },
		{ "Space",          ska_scancode_space        },
		{ "Minus",          ska_scancode_minus        },
		{ "Equal",          ska_scancode_equals       },
		{ "BracketLeft",    ska_scancode_leftbracket  },
		{ "BracketRight",   ska_scancode_rightbracket },
		{ "Backslash",      ska_scancode_backslash    },
		{ "Semicolon",      ska_scancode_semicolon    },
		{ "Quote",          ska_scancode_apostrophe   },
		{ "Backquote",      ska_scancode_grave        },
		{ "Comma",          ska_scancode_comma        },
		{ "Period",         ska_scancode_period       },
		{ "Slash",          ska_scancode_slash        },
		{ "CapsLock",       ska_scancode_capslock     },
		{ "PrintScreen",    ska_scancode_printscreen  },
		{ "ScrollLock",     ska_scancode_scrolllock   },
		{ "Pause",          ska_scancode_pause        },
		{ "Insert",         ska_scancode_insert       },
		{ "Home",           ska_scancode_home         },
		{ "PageUp",         ska_scancode_pageup       },
		{ "Delete",         ska_scancode_delete       },
		{ "End",            ska_scancode_end          },
		{ "PageDown",       ska_scancode_pagedown     },
		{ "ArrowRight",     ska_scancode_right        },
		{ "ArrowLeft",      ska_scancode_left         },
		{ "ArrowDown",      ska_scancode_down         },
		{ "ArrowUp",        ska_scancode_up           },
		{ "ControlLeft",    ska_scancode_lctrl        },
		{ "ShiftLeft",      ska_scancode_lshift       },
		{ "AltLeft",        ska_scancode_lalt         },
		{ "MetaLeft",       ska_scancode_lgui         },
		{ "OSLeft",         ska_scancode_lgui         },
		{ "ControlRight",   ska_scancode_rctrl        },
		{ "ShiftRight",     ska_scancode_rshift       },
		{ "AltRight",       ska_scancode_ralt         },
		{ "MetaRight",      ska_scancode_rgui         },
		{ "OSRight",        ska_scancode_rgui         },
	};

	for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
		if (strcmp(code, named[i].code) == 0) {
			return named[i].scancode;
		}
	}

	return ska_scancode_unknown;
}

// KeyboardEvent.key is a single UTF-8 codepoint for printable keys ("a", "é")
// and a name for everything else ("Enter", "ArrowLeft").
static bool ska_web_key_is_text(const char* key) {
	if (key[0] == '\0') return false;

	// Count codepoints (bytes that are not UTF-8 continuation bytes)
	int32_t codepoints = 0;
	for (const char* p = key; *p; p++) {
		if (((uint8_t)*p & 0xC0) != 0x80) codepoints++;
	}
	if (codepoints != 1) return false;

	// Single ASCII control chars aren't text
	return (uint8_t)key[0] >= 0x20;
}

// ============================================================================
// Event Callbacks
// ============================================================================

// Keyboard events are page-level; route them to the focused window, or the
// first window when none is focused.
static ska_window_t* ska_web_key_target_window(void) {
	ska_window_t* first = NULL;
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		ska_window_t* win = g_ska.windows[i];
		if (!win) continue;
		if (win->has_focus) return win;
		if (!first) first = win;
	}
	return first;
}

static bool ska_web_on_key(int event_type, const EmscriptenKeyboardEvent* e, void* user_data) {
	(void)user_data;
	ska_window_t* window = ska_web_key_target_window();
	if (!window) return false;

	bool          pressed  = event_type == EMSCRIPTEN_EVENT_KEYDOWN;
	ska_scancode_ scancode = ska_web_code_to_scancode(e->code);

	ska_event_t event = {0};
	event.timestamp          = ska_time_get_elapsed_ms();
	event.type               = pressed ? ska_event_key_down : ska_event_key_up;
	event.keyboard.window_id = window->id;
	event.keyboard.pressed   = pressed;
	event.keyboard.repeat    = e->repeat;
	event.keyboard.scancode  = scancode;

	// Update keyboard state FIRST (before deriving modifiers)
	if (scancode != ska_scancode_unknown) {
		g_ska.input_state.keyboard[scancode] = pressed ? 1 : 0;
	}

	// Derive modifier state from tracked keyboard state, matching X11/Win32
	uint16_t mods = ska_input_state_derive_modifiers(&g_ska.input_state);
	event.keyboard.modifiers        = mods;
	g_ska.input_state.key_modifiers = mods;

	ska_post_event(&event);

	// Text input comes from the same keydown: e->key holds the produced
	// character for printable keys (no IME/composition support yet)
	if (pressed && !e->ctrlKey && !e->metaKey && !e->altKey && ska_web_key_is_text(e->key)) {
		ska_event_t text_event = {0};
		text_event.timestamp      = event.timestamp;
		text_event.type           = ska_event_text_input;
		text_event.text.window_id = window->id;
		strncpy(text_event.text.text, e->key, sizeof(text_event.text.text) - 1);
		ska_post_event(&text_event);
	}

	// Consume (preventDefault) so Tab/Backspace/Space/arrows don't scroll or
	// navigate the page, but leave browser-level shortcuts alone
	if (e->metaKey) return false;
	if (scancode == ska_scancode_f5 || scancode == ska_scancode_f11 || scancode == ska_scancode_f12) return false;
	return true;
}

static void ska_web_set_window_focus(ska_window_t* window, bool focused) {
	if (window->has_focus == focused) return;
	window->has_focus = focused;

	ska_event_t event = {0};
	event.timestamp        = ska_time_get_elapsed_ms();
	event.type             = focused ? ska_event_window_focus_gained : ska_event_window_focus_lost;
	event.window.window_id = window->id;
	ska_post_event(&event);
}

static bool ska_web_on_mouse(int event_type, const EmscriptenMouseEvent* e, void* user_data) {
	ska_window_t* window = (ska_window_t*)user_data;

	ska_event_t event = {0};
	event.timestamp = ska_time_get_elapsed_ms();

	switch (event_type) {
	case EMSCRIPTEN_EVENT_MOUSEMOVE: {
		int32_t x, y, xrel, yrel;
		if (g_web_pointer_locked) {
			// The absolute position stays frozen in relative mode, matching
			// the other backends; accumulating movementX would wander
			// unbounded past the window edges.
			xrel = (int32_t)e->movementX;
			yrel = (int32_t)e->movementY;
			x    = g_ska.input_state.mouse_x;
			y    = g_ska.input_state.mouse_y;
		} else {
			x    = (int32_t)e->targetX;
			y    = (int32_t)e->targetY;
			xrel = x - g_ska.input_state.mouse_x;
			yrel = y - g_ska.input_state.mouse_y;
		}

		event.type                   = ska_event_mouse_motion;
		event.mouse_motion.window_id = window->id;
		event.mouse_motion.x         = x;
		event.mouse_motion.y         = y;
		event.mouse_motion.xrel      = xrel;
		event.mouse_motion.yrel      = yrel;

		g_ska.input_state.mouse_x    = x;
		g_ska.input_state.mouse_y    = y;
		ska_input_add_relative(xrel, yrel);

		ska_post_event(&event);
		break;
	}

	case EMSCRIPTEN_EVENT_MOUSEDOWN:
	case EMSCRIPTEN_EVENT_MOUSEUP: {
		bool pressed = event_type == EMSCRIPTEN_EVENT_MOUSEDOWN;

		// Clicking a canvas focuses its window
		if (pressed && !window->has_focus) {
			for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
				if (g_ska.windows[i] && g_ska.windows[i] != window) {
					ska_web_set_window_focus(g_ska.windows[i], false);
				}
			}
			ska_web_set_window_focus(window, true);
		}

		// DOM buttons: 0=left, 1=middle, 2=right, 3=back, 4=forward
		ska_mouse_button_ button;
		switch (e->button) {
			case 0:  button = ska_mouse_button_left;   break;
			case 1:  button = ska_mouse_button_middle; break;
			case 2:  button = ska_mouse_button_right;  break;
			case 3:  button = ska_mouse_button_x1;     break;
			case 4:  button = ska_mouse_button_x2;     break;
			default: button = (ska_mouse_button_)(e->button + 1); break;
		}

		event.type                    = pressed ? ska_event_mouse_button_down : ska_event_mouse_button_up;
		event.mouse_button.window_id  = window->id;
		event.mouse_button.button     = button;
		event.mouse_button.pressed    = pressed;
		event.mouse_button.clicks     = 1;
		event.mouse_button.x          = (int32_t)e->targetX;
		event.mouse_button.y          = (int32_t)e->targetY;

		uint32_t button_mask = 1u << (button - 1);
		if (pressed) g_ska.input_state.mouse_buttons |=  button_mask;
		else         g_ska.input_state.mouse_buttons &= ~button_mask;

		ska_post_event(&event);
		break;
	}

	case EMSCRIPTEN_EVENT_MOUSEENTER:
		event.type             = ska_event_window_mouse_enter;
		event.window.window_id = window->id;
		window->mouse_inside   = true;
		ska_post_event(&event);
		break;

	case EMSCRIPTEN_EVENT_MOUSELEAVE:
		event.type             = ska_event_window_mouse_leave;
		event.window.window_id = window->id;
		window->mouse_inside   = false;
		ska_post_event(&event);
		break;

	default:
		return false;
	}

	return true;
}

static bool ska_web_on_wheel(int event_type, const EmscriptenWheelEvent* e, void* user_data) {
	(void)event_type;
	ska_window_t* window = (ska_window_t*)user_data;

	// Normalize so one mouse-wheel notch is roughly +-1
	float scale;
	switch (e->deltaMode) {
		case DOM_DELTA_PIXEL: scale = 1.0f / 100.0f; break;
		case DOM_DELTA_LINE:  scale = 1.0f / 3.0f;   break;
		default:              scale = 1.0f;          break;
	}

	float precise_x = -(float)e->deltaX * scale;
	float precise_y = -(float)e->deltaY * scale;

	ska_event_t event = {0};
	event.timestamp             = ska_time_get_elapsed_ms();
	event.type                  = ska_event_mouse_wheel;
	event.mouse_wheel.window_id = window->id;
	event.mouse_wheel.x         = precise_x > 0.0f ? 1 : (precise_x < 0.0f ? -1 : 0);
	event.mouse_wheel.y         = precise_y > 0.0f ? 1 : (precise_y < 0.0f ? -1 : 0);
	event.mouse_wheel.precise_x = precise_x;
	event.mouse_wheel.precise_y = precise_y;
	ska_post_event(&event);

	return true; // Prevent the page from scrolling
}

static bool ska_web_on_visibility(int event_type, const EmscriptenVisibilityChangeEvent* e, void* user_data) {
	(void)event_type; (void)user_data;

	ska_event_t event = {0};
	event.timestamp = ska_time_get_elapsed_ms();
	event.type      = e->hidden ? ska_event_app_background : ska_event_app_foreground;
	ska_post_event(&event);
	return false;
}

static bool ska_web_on_pointerlockchange(int event_type, const EmscriptenPointerlockChangeEvent* e, void* user_data) {
	(void)event_type; (void)user_data;

	// Tracks both our own requests and the user pressing Escape
	g_web_pointer_locked                  = e->isActive;
	g_ska.input_state.relative_mouse_mode = e->isActive;
	return false;
}

// ============================================================================
// Platform Init/Shutdown
// ============================================================================

bool ska_platform_init(void) {
	g_ska.web_default_canvas_used = false;
	g_ska.web_cached_dpr          = (float)emscripten_get_device_pixel_ratio();

	if (ska_web_has_dom()) {
		emscripten_set_keydown_callback (EMSCRIPTEN_EVENT_TARGET_WINDOW,   NULL, false, ska_web_on_key);
		emscripten_set_keyup_callback   (EMSCRIPTEN_EVENT_TARGET_WINDOW,   NULL, false, ska_web_on_key);
		emscripten_set_visibilitychange_callback                          (NULL, false, ska_web_on_visibility);
		emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, false, ska_web_on_pointerlockchange);

		// Heartbeat for blocking-loop detection: this counter only advances
		// when the browser gets control of the main thread. See
		// ska_web_check_blocking_loop.
		EM_ASM({
			Module.skaRafCounter = 0;
			var tick = function() {
				Module.skaRafCounter++;
				requestAnimationFrame(tick);
			};
			requestAnimationFrame(tick);
		});
	} else {
		ska_log(ska_log_warn, "sk_app web: no DOM available, running headless (no windows/input)");
	}

	return true;
}

void ska_platform_shutdown(void) {
	if (ska_web_has_dom()) {
		emscripten_html5_remove_all_event_listeners();
	}

	if (g_web_clipboard_cache) {
		ska_free(g_web_clipboard_cache);
		g_web_clipboard_cache = NULL;
	}
}

// ============================================================================
// Window Management
// ============================================================================

static float ska_web_pixel_scale(const ska_window_t* window) {
	(void)window;
	return (float)emscripten_get_device_pixel_ratio();
}

// A resizable window on the page's default canvas means "the browser owns the
// layout": CSS decides the canvas size (viewport-fill, set at create), the
// pump's CSS-size polling turns layout changes into resize events, and this
// code only maintains the backing store. Fixed-size windows and secondary
// canvases keep explicit CSS dimensions.
static bool ska_web_layout_driven(const ska_window_t* window) {
	return (window->flags & ska_window_resizable) != 0 && !window->owns_canvas;
}

// Apply CSS size + backing store size from window->width/height
static void ska_web_apply_size(ska_window_t* window) {
	float scale = ska_web_pixel_scale(window);
	window->drawable_width  = (int32_t)lroundf((float)window->width  * scale);
	window->drawable_height = (int32_t)lroundf((float)window->height * scale);

	if (!ska_web_layout_driven(window))
		emscripten_set_element_css_size(window->canvas_selector, window->width, window->height);
	emscripten_set_canvas_element_size(window->canvas_selector, window->drawable_width, window->drawable_height);
}

bool ska_platform_window_create(ska_window_t* ref_window, const char* title, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t flags) {
	(void)x; (void)y; (void)flags; // flags are read from ref_window->flags

	if (!ska_web_has_dom()) {
		ska_set_error("ska_window_create: no DOM available (headless environment)");
		return false;
	}

	// First window binds to the page's default canvas when one exists;
	// additional windows get their own canvas elements appended to <body>
	bool has_default = EM_ASM_INT({
		return ((typeof Module !== 'undefined' && Module['canvas']) ||
		        document.getElementById('canvas')) ? 1 : 0;
	}) != 0;

	if (has_default && !g_ska.web_default_canvas_used) {
		snprintf(ref_window->canvas_selector, sizeof(ref_window->canvas_selector), "#canvas");
		g_ska.web_default_canvas_used = true;
	} else {
		char element_id[48];
		snprintf(element_id, sizeof(element_id), "ska-window-%u", ref_window->id);
		EM_ASM({
			var canvas = document.createElement('canvas');
			canvas.id = UTF8ToString($0);
			canvas.style.display = 'block';
			document.body.appendChild(canvas);
		}, element_id);
		snprintf(ref_window->canvas_selector, sizeof(ref_window->canvas_selector), "#%s", element_id);
		ref_window->owns_canvas = true;
	}

	ref_window->title     = ska_strdup(title);
	ref_window->x         = 0;
	ref_window->y         = 0;
	ref_window->width     = w;
	ref_window->height    = h;
	ref_window->dpi_scale = (float)emscripten_get_device_pixel_ratio();

	// Layout-driven windows (see ska_web_layout_driven) fill the viewport and
	// take their size from CSS; everything else keeps the requested size
	if (ska_web_layout_driven(ref_window)) {
		EM_ASM({
			var canvas = document.querySelector(UTF8ToString($0));
			if (canvas) {
				document.body.style.margin   = '0';
				document.body.style.overflow = 'hidden';
				canvas.style.width  = '100vw';
				canvas.style.height = '100vh';
			}
		}, ref_window->canvas_selector);

		double css_w = 0, css_h = 0;
		emscripten_get_element_css_size(ref_window->canvas_selector, &css_w, &css_h);
		if (css_w > 0 && css_h > 0) {
			ref_window->width  = (int32_t)lround(css_w);
			ref_window->height = (int32_t)lround(css_h);
		}
	}
	ska_web_apply_size(ref_window);

	// Per-canvas input callbacks
	const char* sel = ref_window->canvas_selector;
	emscripten_set_mousedown_callback (sel, ref_window, false, ska_web_on_mouse);
	emscripten_set_mouseup_callback   (sel, ref_window, false, ska_web_on_mouse);
	emscripten_set_mousemove_callback (sel, ref_window, false, ska_web_on_mouse);
	emscripten_set_mouseenter_callback(sel, ref_window, false, ska_web_on_mouse);
	emscripten_set_mouseleave_callback(sel, ref_window, false, ska_web_on_mouse);
	emscripten_set_wheel_callback     (sel, ref_window, false, ska_web_on_wheel);

	// Capture the pointer while a button is held: the browser then retargets
	// the whole gesture — including the release — to the canvas, so drags
	// that leave the canvas (or the browser window entirely) still deliver
	// their mouseup through the emscripten callbacks above. The dataset flag
	// keeps a re-bound default canvas from stacking duplicate listeners.
	EM_ASM({
		var canvas = document.querySelector(UTF8ToString($0));
		if (canvas && !canvas.dataset.skaPointerCapture) {
			canvas.dataset.skaPointerCapture = '1';
			canvas.addEventListener('pointerdown', function (e) {
				if (canvas.setPointerCapture) canvas.setPointerCapture(e.pointerId);
			});
		}
	}, sel);

	emscripten_set_window_title(title);

	// Single-page app: the first window starts focused
	if (g_ska.window_count == 1) {
		ref_window->has_focus = true;
	}

	if (flags & ska_window_fullscreen) {
		ska_platform_window_set_fullscreen(ref_window, true);
	}

	return true;
}

void ska_platform_window_destroy(ska_window_t* ref_window) {
	if (!ska_web_has_dom()) return;

	const char* sel = ref_window->canvas_selector;
	emscripten_set_mousedown_callback (sel, NULL, false, NULL);
	emscripten_set_mouseup_callback   (sel, NULL, false, NULL);
	emscripten_set_mousemove_callback (sel, NULL, false, NULL);
	emscripten_set_mouseenter_callback(sel, NULL, false, NULL);
	emscripten_set_mouseleave_callback(sel, NULL, false, NULL);
	emscripten_set_wheel_callback     (sel, NULL, false, NULL);

	if (ref_window->owns_canvas) {
		EM_ASM({
			var canvas = document.querySelector(UTF8ToString($0));
			if (canvas && canvas.parentNode) canvas.parentNode.removeChild(canvas);
		}, sel);
	} else {
		g_ska.web_default_canvas_used = false;
	}
}

void ska_platform_window_set_title(ska_window_t* ref_window, const char* title) {
	if (ref_window->title) ska_free(ref_window->title);
	ref_window->title = ska_strdup(title);
	if (ska_web_has_dom()) {
		emscripten_set_window_title(title);
	}
}

void ska_platform_get_frame_extents(const ska_window_t* window, int32_t* opt_out_left, int32_t* opt_out_right, int32_t* opt_out_top, int32_t* opt_out_bottom) {
	(void)window;
	if (opt_out_left)   *opt_out_left   = 0;
	if (opt_out_right)  *opt_out_right  = 0;
	if (opt_out_top)    *opt_out_top    = 0;
	if (opt_out_bottom) *opt_out_bottom = 0;
}

void ska_platform_window_set_frame_position(ska_window_t* ref_window, int32_t x, int32_t y) {
	// Canvas position is determined by page layout, not the app
	(void)ref_window; (void)x; (void)y;
}

void ska_platform_window_set_frame_size(ska_window_t* ref_window, int32_t w, int32_t h) {
	if (!ska_web_has_dom()) return;
	if (w == ref_window->width && h == ref_window->height) return;

	ref_window->width  = w;
	ref_window->height = h;
	ska_web_apply_size(ref_window);

	ska_event_t event = {0};
	event.timestamp        = ska_time_get_elapsed_ms();
	event.type             = ska_event_window_resized;
	event.window.window_id = ref_window->id;
	event.window.data1     = w;
	event.window.data2     = h;
	ska_post_event(&event);
}

static void ska_web_set_canvas_visible(ska_window_t* window, bool visible) {
	EM_ASM({
		var canvas = document.querySelector(UTF8ToString($0));
		if (canvas) canvas.style.visibility = $1 ? 'visible' : 'hidden';
	}, window->canvas_selector, visible ? 1 : 0);
}

void ska_platform_window_show(ska_window_t* ref_window) {
	if (!ska_web_has_dom()) return;
	ska_web_set_canvas_visible(ref_window, true);
	ref_window->is_visible = true;

	ska_event_t event = {0};
	event.timestamp        = ska_time_get_elapsed_ms();
	event.type             = ska_event_window_shown;
	event.window.window_id = ref_window->id;
	ska_post_event(&event);
}

void ska_platform_window_hide(ska_window_t* ref_window) {
	if (!ska_web_has_dom()) return;
	ska_web_set_canvas_visible(ref_window, false);
	ref_window->is_visible = false;

	ska_event_t event = {0};
	event.timestamp        = ska_time_get_elapsed_ms();
	event.type             = ska_event_window_hidden;
	event.window.window_id = ref_window->id;
	ska_post_event(&event);
}

void ska_platform_window_maximize(ska_window_t* ref_window) {
	// No window manager on the web; page layout owns canvas geometry
	(void)ref_window;
}

void ska_platform_window_minimize(ska_window_t* ref_window) {
	(void)ref_window;
}

void ska_platform_window_restore(ska_window_t* ref_window) {
	(void)ref_window;
}

void ska_platform_window_set_fullscreen(ska_window_t* ref_window, bool fullscreen) {
	if (!ska_web_has_dom()) return;

	if (fullscreen) {
		EmscriptenFullscreenStrategy strategy = {0};
		strategy.scaleMode                 = EMSCRIPTEN_FULLSCREEN_SCALE_STRETCH;
		strategy.canvasResolutionScaleMode = EMSCRIPTEN_FULLSCREEN_CANVAS_SCALE_HIDEF;
		strategy.filteringMode             = EMSCRIPTEN_FULLSCREEN_FILTERING_DEFAULT;
		// Deferred: browsers only allow fullscreen from a user gesture, so the
		// request is queued until the next input event if needed
		emscripten_request_fullscreen_strategy(ref_window->canvas_selector, true, &strategy);
	} else {
		emscripten_exit_fullscreen();
	}
	// The resulting canvas size change is picked up by pump_events polling
}

void ska_platform_window_raise(ska_window_t* ref_window) {
	(void)ref_window;
}

void ska_platform_window_get_drawable_size(ska_window_t* ref_window, int32_t* opt_out_width, int32_t* opt_out_height) {
	if (ska_web_has_dom()) {
		int w = 0, h = 0;
		if (emscripten_get_canvas_element_size(ref_window->canvas_selector, &w, &h) == EMSCRIPTEN_RESULT_SUCCESS) {
			ref_window->drawable_width  = w;
			ref_window->drawable_height = h;
		}
	}
	if (opt_out_width)  *opt_out_width  = ref_window->drawable_width;
	if (opt_out_height) *opt_out_height = ref_window->drawable_height;
}

float ska_platform_get_dpi_scale(const ska_window_t* window) {
	(void)window;
	return (float)emscripten_get_device_pixel_ratio();
}

float ska_platform_get_refresh_rate(const ska_window_t* window) {
	(void)window;
	// Browsers expose no display-mode API; requestAnimationFrame paces frames
	// to the display anyway, so report the common default
	return 60.0f;
}

// ============================================================================
// Input
// ============================================================================

static const char* ska_web_cursor_css_name(ska_system_cursor_ cursor) {
	switch (cursor) {
		case ska_system_cursor_arrow:     return "default";
		case ska_system_cursor_ibeam:     return "text";
		case ska_system_cursor_wait:      return "wait";
		case ska_system_cursor_crosshair: return "crosshair";
		case ska_system_cursor_waitarrow: return "progress";
		case ska_system_cursor_sizenwse:  return "nwse-resize";
		case ska_system_cursor_sizenesw:  return "nesw-resize";
		case ska_system_cursor_sizewe:    return "ew-resize";
		case ska_system_cursor_sizens:    return "ns-resize";
		case ska_system_cursor_sizeall:   return "move";
		case ska_system_cursor_no:        return "not-allowed";
		case ska_system_cursor_hand:      return "pointer";
		default:                          return "default";
	}
}

static void ska_web_apply_cursor(void) {
	if (!ska_web_has_dom()) return;

	const char* css = g_web_cursor_visible ? ska_web_cursor_css_name(g_web_cursor) : "none";
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (!g_ska.windows[i]) continue;
		EM_ASM({
			var canvas = document.querySelector(UTF8ToString($0));
			if (canvas) canvas.style.cursor = UTF8ToString($1);
		}, g_ska.windows[i]->canvas_selector, css);
	}
}

void ska_platform_set_cursor(ska_system_cursor_ cursor) {
	g_web_cursor = cursor;
	ska_web_apply_cursor();
}

void ska_platform_show_cursor(bool show) {
	g_web_cursor_visible = show;
	ska_web_apply_cursor();
}

bool ska_platform_set_relative_mouse_mode(bool enabled) {
	if (!ska_web_has_dom()) {
		ska_set_error("ska_mouse_set_relative_mode: no DOM available");
		return false;
	}

	if (enabled) {
		ska_window_t* window = ska_web_key_target_window();
		if (!window) {
			ska_set_error("ska_mouse_set_relative_mode: no window available");
			return false;
		}
		// Deferred: browsers only allow pointer lock from a user gesture
		emscripten_request_pointerlock(window->canvas_selector, true);
	} else {
		emscripten_exit_pointerlock();
	}
	return true;
}

void ska_platform_show_virtual_keyboard(bool visible, ska_text_input_type_ type) {
	(void)visible; (void)type;
}

// ============================================================================
// Event Processing
// ============================================================================

// Old-style blocking main loops (`while (running) { ska_event_poll(...); }`)
// can never work in a browser: the main thread never returns to the event
// loop, so no input arrives, nothing paints, and the tab just freezes. That
// pattern is still fully supported on native, so instead of removing it we
// detect it here and fail with a pointer to the fix.
//
// Detection: a requestAnimationFrame heartbeat (installed in ska_platform_init)
// only advances when the browser gets control. If ska_event_poll keeps getting
// called while the heartbeat is frozen, the app is spinning in a blocking
// loop. Thresholds are generous so legitimate synchronous bursts (startup
// code, short ska_event_wait_timeout calls) never trip it.
#define SKA_WEB_BLOCK_MIN_POLLS   200
#define SKA_WEB_BLOCK_MIN_SECONDS 3.0

static void ska_web_check_blocking_loop(void) {
	static int32_t last_heartbeat    = -1;
	static int32_t polls_since_yield = 0;
	static double  first_poll_time   = 0.0;

	// An external driver (an OpenXR runtime on WebXR, say) legitimately runs
	// without the window rAF heartbeat: an immersive session suspends
	// window.requestAnimationFrame and drives frames from
	// XRSession.requestAnimationFrame instead. Detecting that as a blocking
	// loop would force-exit a perfectly healthy app.
	if (g_ska.external_frame_driver) {
		polls_since_yield = 0;
		return;
	}

	int32_t heartbeat = EM_ASM_INT({ return Module.skaRafCounter | 0; });
	if (heartbeat != last_heartbeat) {
		// Browser got control since the last poll; not a blocking loop
		last_heartbeat    = heartbeat;
		polls_since_yield = 0;
		return;
	}

	if (polls_since_yield == 0) {
		first_poll_time = ska_time_get_elapsed_s();
	}
	polls_since_yield++;

	double stuck_s = ska_time_get_elapsed_s() - first_poll_time;
	if (polls_since_yield > SKA_WEB_BLOCK_MIN_POLLS && stuck_s > SKA_WEB_BLOCK_MIN_SECONDS) {
		ska_log(ska_log_error,
			"sk_app: the main thread has been polling events for %.1f seconds without "
			"yielding to the browser. A blocking main loop (while + ska_event_poll / "
			"ska_event_wait) cannot work in WASM builds - the browser never gets to "
			"deliver input or paint. Move your per-frame code into a callback and drive "
			"it with ska_run(frame_fn, user_data); it behaves identically on native and "
			"maps onto requestAnimationFrame on the web. See ska_run in sk_app.h.",
			stuck_s);
		emscripten_force_exit(1);
	}
}

// Browser events arrive asynchronously through the callbacks above and are
// already queued; pumping only needs to poll for things that have no DOM
// event: canvas CSS size changes (page layout, fullscreen) and
// devicePixelRatio changes (browser zoom, moving between monitors).
void ska_platform_pump_events(void) {
	if (!ska_web_has_dom()) return;

	ska_web_check_blocking_loop();

	float dpr = (float)emscripten_get_device_pixel_ratio();
	bool  dpr_changed = dpr != g_ska.web_cached_dpr;
	g_ska.web_cached_dpr = dpr;

	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		ska_window_t* window = g_ska.windows[i];
		if (!window) continue;

		double css_w = 0, css_h = 0;
		emscripten_get_element_css_size(window->canvas_selector, &css_w, &css_h);
		int32_t w = (int32_t)lround(css_w);
		int32_t h = (int32_t)lround(css_h);

		if (w > 0 && h > 0 && (w != window->width || h != window->height)) {
			window->width  = w;
			window->height = h;

			// Match the backing store to the new element size
			float scale = ska_web_pixel_scale(window);
			window->drawable_width  = (int32_t)lroundf((float)w * scale);
			window->drawable_height = (int32_t)lroundf((float)h * scale);
			emscripten_set_canvas_element_size(window->canvas_selector, window->drawable_width, window->drawable_height);

			ska_event_t event = {0};
			event.timestamp        = ska_time_get_elapsed_ms();
			event.type             = ska_event_window_resized;
			event.window.window_id = window->id;
			event.window.data1     = w;
			event.window.data2     = h;
			ska_post_event(&event);
		}

		if (dpr_changed) {
			window->dpi_scale = dpr;
			ska_web_apply_size(window);

			ska_event_t event = {0};
			event.timestamp        = ska_time_get_elapsed_ms();
			event.type             = ska_event_window_dpi_changed;
			event.window.window_id = window->id;
			event.window.data1     = (int32_t)(dpr * 100.0f + 0.5f);
			ska_post_event(&event);
		}
	}
}

// ============================================================================
// Main Loop
// ============================================================================

static ska_frame_fn g_web_run_frame     = NULL;
static void*        g_web_run_user_data = NULL;

static void ska_web_main_loop(void) {
	if (!g_web_run_frame(g_web_run_user_data)) {
		emscripten_cancel_main_loop();
	}
}

SKA_API void ska_run(ska_frame_fn frame, void* user_data) {
	if (!frame) return;
	g_web_run_frame     = frame;
	g_web_run_user_data = user_data;
	// simulate_infinite_loop=true: unwinds the stack, so this never returns
	emscripten_set_main_loop(ska_web_main_loop, 0, true);
}

// ============================================================================
// Vulkan Support (not available on the web)
// ============================================================================

const char** ska_platform_vk_get_instance_extensions(uint32_t* out_count) {
	ska_set_error("Vulkan is not available on the web; use ska_wgpu_create_surface instead");
	*out_count = 0;
	return NULL;
}

bool ska_platform_vk_create_surface(const ska_window_t* window, VkInstance instance, VkSurfaceKHR* out_surface) {
	(void)window; (void)instance; (void)out_surface;
	ska_set_error("Vulkan is not available on the web; use ska_wgpu_create_surface instead");
	return false;
}

// ============================================================================
// Clipboard Support
// ============================================================================

// navigator.clipboard is async-only, so reads return the last text set through
// this API (app-internal copy/paste works; external clipboard content is not
// visible until async paste support is added).
char* ska_platform_clipboard_get_text(void) {
	return g_web_clipboard_cache ? ska_strdup(g_web_clipboard_cache) : NULL;
}

bool ska_platform_clipboard_set_text(const char* text) {
	if (g_web_clipboard_cache) ska_free(g_web_clipboard_cache);
	g_web_clipboard_cache = ska_strdup(text);

	if (ska_web_has_dom()) {
		EM_ASM({
			if (typeof navigator !== 'undefined' && navigator.clipboard && navigator.clipboard.writeText) {
				navigator.clipboard.writeText(UTF8ToString($0)).catch(function(){});
			}
		}, text);
	}
	return true;
}

// ============================================================================
// File Dialogs (not yet supported)
// ============================================================================

bool ska_platform_file_dialog_available(ska_file_dialog_ type) {
	(void)type;
	return false;
}

bool ska_platform_file_dialog_show(ska_file_dialog_id_t id, const ska_file_dialog_request_t* request) {
	(void)id; (void)request;
	ska_set_error("File dialogs are not yet supported on the web");
	return false;
}

// ============================================================================
// Key-Value Persistent Storage (localStorage)
// ============================================================================

// Values are base64-encoded into localStorage. Falls back to an in-memory
// store when localStorage is unavailable (e.g. node, sandboxed iframes).

static void ska_web_kvp_full_key(const char* key, char* buffer, size_t buffer_size) {
	snprintf(buffer, buffer_size, "ska_%s_%s", ska_kvpstore_get_app_name(), key);
}

SKA_API bool ska_kvpstore_save(const char* key, const void* data, size_t size) {
	if (!ska_kvpstore_validate_key(key)) return false;
	if (!data && size > 0) {
		ska_set_error("ska_kvpstore_save: NULL data with non-zero size");
		return false;
	}

	char full_key[160];
	ska_web_kvp_full_key(key, full_key, sizeof(full_key));

	int ok = EM_ASM_INT({
		try {
			var key   = UTF8ToString($0);
			var bytes = HEAPU8.subarray($1, $1 + $2);
			var chars = "";
			for (var i = 0; i < bytes.length; i++) chars += String.fromCharCode(bytes[i]);
			var value = btoa(chars);
			if (typeof localStorage !== 'undefined') {
				localStorage.setItem(key, value);
			} else {
				Module.skaKvpStore = Module.skaKvpStore || {};
				Module.skaKvpStore[key] = value;
			}
			return 1;
		} catch (e) {
			return 0;
		}
	}, full_key, data, (int)size);

	if (!ok) {
		ska_set_error("ska_kvpstore_save: storage write failed for '%s'", key);
		return false;
	}
	return true;
}

SKA_API bool ska_kvpstore_load(const char* key, void* opt_buffer, size_t buffer_size, size_t* opt_out_size) {
	if (!ska_kvpstore_validate_key(key)) return false;

	char full_key[160];
	ska_web_kvp_full_key(key, full_key, sizeof(full_key));

	int size = EM_ASM_INT({
		try {
			var key   = UTF8ToString($0);
			var value = null;
			if (typeof localStorage !== 'undefined') {
				value = localStorage.getItem(key);
			} else if (Module.skaKvpStore && (key in Module.skaKvpStore)) {
				value = Module.skaKvpStore[key];
			}
			if (value === null || value === undefined) return -1;
			return atob(value).length;
		} catch (e) {
			return -1;
		}
	}, full_key);

	if (size < 0) {
		return false;
	}

	if (opt_out_size) {
		*opt_out_size = (size_t)size;
	}

	// Size query only
	if (!opt_buffer || buffer_size == 0) {
		return true;
	}

	EM_ASM({
		try {
			var key   = UTF8ToString($0);
			var value = null;
			if (typeof localStorage !== 'undefined') {
				value = localStorage.getItem(key);
			} else if (Module.skaKvpStore && (key in Module.skaKvpStore)) {
				value = Module.skaKvpStore[key];
			}
			if (value === null || value === undefined) return;
			var chars = atob(value);
			var count = Math.min(chars.length, $2);
			for (var i = 0; i < count; i++) HEAPU8[$1 + i] = chars.charCodeAt(i);
		} catch (e) {}
	}, full_key, opt_buffer, (int)buffer_size);

	return true;
}

SKA_API bool ska_kvpstore_delete(const char* key) {
	if (!ska_kvpstore_validate_key(key)) return false;

	char full_key[160];
	ska_web_kvp_full_key(key, full_key, sizeof(full_key));

	EM_ASM({
		try {
			var key = UTF8ToString($0);
			if (typeof localStorage !== 'undefined') {
				localStorage.removeItem(key);
			} else if (Module.skaKvpStore) {
				delete Module.skaKvpStore[key];
			}
		} catch (e) {}
	}, full_key);

	return true;
}

// ============================================================================
// Platform-Specific Exports
// ============================================================================

SKA_API const char* ska_web_get_canvas_selector(const ska_window_t* window) {
	if (!window) return NULL;
	return window->canvas_selector;
}
