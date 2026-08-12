//
// sk_app - Linux Wayland platform backend
//
// One of two interchangeable Linux backends. Nothing here is called directly;
// ska_linux.c dispatches through ska_wl_vtable at the bottom of this file.

// _GNU_SOURCE rather than _POSIX_C_SOURCE: memfd_create is a glibc extension.
#define _GNU_SOURCE
#include "ska_internal.h"

#if defined(SKA_PLATFORM_LINUX) && defined(SKA_LINUX_WAYLAND)

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// The shim carries every generated protocol, see src/wayland/wayland-client.h
#include "wayland-client.h"
#include "ska_decor.h" // After the shim, see the header for why

#define SKA_WL_MAX_OUTPUTS 8

// fractional-scale-v1 reports scale as 120ths, so 120 is 100% and 180 is 150%.
#define SKA_WL_SCALE_UNIT 120

// ============================================================================
// State
// ============================================================================

typedef struct ska_wl_output_t {
	struct wl_output* output;
	uint32_t          registry_name;
	int32_t           scale;       // Integer scale, the pre-fractional fallback
	int32_t           refresh_mhz; // From wl_output.mode
} ska_wl_output_t;

typedef struct ska_wl_state_t {
	struct wl_display*    display;
	struct wl_registry*   registry;
	struct wl_compositor* compositor;
	struct wl_shm*        shm;
	struct wl_seat*       seat;
	struct xdg_wm_base*   wm_base;

	struct zxdg_decoration_manager_v1*     decoration_manager;
	struct wp_viewporter*                  viewporter;
	struct wp_fractional_scale_manager_v1* fractional_scale_manager;

	struct zwp_relative_pointer_manager_v1* relative_pointer_manager;
	struct zwp_pointer_constraints_v1*      pointer_constraints;
	struct zwp_relative_pointer_v1*         relative_pointer;
	struct zwp_locked_pointer_v1*           locked_pointer;

	struct libdecor* decor; // Set when sk_app draws frames instead of the compositor

	struct wl_data_device_manager* data_device_manager;
	struct wl_data_device*         data_device;
	struct wl_data_source*         data_source;   // Live while sk_app owns the selection
	struct wl_data_offer*          data_offer;    // Current clipboard offer from the compositor
	struct wl_data_offer*          pending_offer; // Announced but not yet claimed by an event
	const char*                    offer_mime;    // Which of the accepted types it advertised
	char*                          clipboard_text; // Copy of what sk_app put on the clipboard

	uint32_t last_input_serial; // set_selection must quote a real input event

	bool connection_lost; // Latched so the quit event is posted only once

	ska_wl_output_t outputs[SKA_WL_MAX_OUTPUTS];
	uint32_t        output_count;

	struct wl_pointer*  pointer;
	struct wl_keyboard* keyboard;

	struct xkb_context* xkb_context;
	struct xkb_keymap*  xkb_keymap;
	struct xkb_state*   xkb_state;

	struct xkb_compose_table* compose_table; // Dead-key sequences, as XIM does on X11
	struct xkb_compose_state* compose_state;

	struct wp_cursor_shape_manager_v1* cursor_shape_manager; // Compositor draws the cursor when present
	struct wp_cursor_shape_device_v1*  cursor_shape_device;
	struct wl_cursor_theme* cursor_theme;                    // Fallback, when it does not
	struct wl_surface*      cursor_surface;
	ska_system_cursor_      cursor;
	bool                    cursor_visible;
	uint32_t                pointer_enter_serial;

	ska_window_t* pointer_focus;
	ska_window_t* keyboard_focus;

	double relative_carry_x; // Sub-pixel locked-pointer motion, kept so slow drags register
	double relative_carry_y;

	// The compositor only states a rate and delay, so sk_app times the repeats
	int32_t       repeat_rate;      // Repeats per second, 0 disables
	int32_t       repeat_delay;     // Milliseconds before the first repeat
	uint32_t      repeat_key;       // XKB keycode being repeated, 0 for none
	ska_scancode_ repeat_scancode;
	uint32_t      repeat_next_ms;
} ska_wl_state_t;

typedef struct ska_wl_window_t {
	ska_window_t*        owner;
	struct wl_surface*   surface;
	struct xdg_surface*  xdg_surface;
	struct xdg_toplevel* xdg_toplevel;

	struct zxdg_toplevel_decoration_v1* decoration;
	bool                                server_decorated; // Compositor draws the frame
	struct libdecor_frame*              decor_frame;      // Set when sk_app draws it instead
	struct wl_buffer*                   placeholder; // Keeps the surface mapped until a renderer draws
	struct wp_viewport*                 viewport;
	struct wp_fractional_scale_v1*      fractional_scale;

	int32_t scale_120; // 120ths, matching fractional-scale-v1
	struct wl_output* outputs[SKA_WL_MAX_OUTPUTS];
	uint32_t          output_count;

	bool    ready;           // Creation finished, so changes are worth reporting
	bool    app_presents;    // A renderer owns the surface, so it carries commits
	int32_t reported_width;  // Logical size of the last resized event posted
	int32_t reported_height;
	bool    configured;      // First xdg_surface.configure has been acked
	bool    mapped;          // A buffer is attached, so the compositor shows it
	int32_t pending_width;   // From xdg_toplevel.configure, 0 means "you choose"
	int32_t pending_height;
	bool    maximized;
	bool    activated;
} ska_wl_window_t;

static ska_wl_state_t* ska_wl(void) { return g_ska.wl; }

static void ska_wl_post_window_event(ska_window_t* window, ska_event_ type, int32_t data1, int32_t data2) {
	ska_event_t event = {0};
	event.timestamp        = ska_time_get_elapsed_ms();
	event.type             = type;
	event.window.window_id = window->id;
	event.window.data1     = data1;
	event.window.data2     = data2;
	ska_post_event(&event);
}

// ============================================================================
// Scaling
// ============================================================================

static ska_wl_output_t* ska_wl_find_output(struct wl_output* output) {
	ska_wl_state_t* wl = ska_wl();
	for (uint32_t i = 0; i < wl->output_count; i++) {
		if (wl->outputs[i].output == output) return &wl->outputs[i];
	}
	return NULL;
}

// Largest integer scale among the outputs the surface touches, used only when
// the compositor has no fractional-scale-v1 to report an exact one.
static int32_t ska_wl_scale_from_outputs(ska_wl_window_t* win) {
	int32_t scale = 1;
	for (uint32_t i = 0; i < win->output_count; i++) {
		ska_wl_output_t* output = ska_wl_find_output(win->outputs[i]);
		if (output && output->scale > scale) scale = output->scale;
	}
	return scale * SKA_WL_SCALE_UNIT;
}

// Applies pending surface state for a window nothing is rendering into. Once a
// renderer owns the surface, its own commit carries this, and committing here
// would apply new geometry to the buffer it has already presented, which the
// compositor then stretches until the next frame.
static void ska_wl_commit_if_idle(ska_wl_window_t* win) {
	if (win->app_presents) return;
	wl_surface_commit(win->surface);
}

// Waits for the first configure on a surface, which is what makes it usable.
// Bounded, because a compositor that never answers would otherwise hang inside
// a public entry point with no way out.
static bool ska_wl_wait_configured(ska_wl_window_t* win, int32_t timeout_ms) {
	struct wl_display* display  = ska_wl()->display;
	uint64_t           deadline = ska_time_get_elapsed_ms() + (uint64_t)timeout_ms;

	while (!win->configured) {
		wl_display_flush(display);
		if (wl_display_dispatch_pending(display) < 0) return false;
		if (win->configured) break;

		int64_t  remaining = (int64_t)deadline - (int64_t)ska_time_get_elapsed_ms();
		if (remaining <= 0) return false;

		struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
		int32_t       ready = poll(&pfd, 1, (int)remaining);
		if (ready < 0)  return false;
		if (ready == 0) return false;
		if (wl_display_dispatch(display) < 0) return false;
	}
	return win->configured;
}

// Recomputes drawable size and tells the compositor how to read the buffer.
// Emits dpi_changed when the scale moved, resized when the pixel size did.
static void ska_wl_apply_scale(ska_wl_window_t* win) {
	ska_window_t* window = win->owner;
	float         scale  = (float)win->scale_120 / (float)SKA_WL_SCALE_UNIT;

	// Rounded up, so a window never resolves to fewer pixels than it covers
	int32_t drawable_w = (window->width  * win->scale_120 + SKA_WL_SCALE_UNIT - 1) / SKA_WL_SCALE_UNIT;
	int32_t drawable_h = (window->height * win->scale_120 + SKA_WL_SCALE_UNIT - 1) / SKA_WL_SCALE_UNIT;

	if (win->viewport) {
		// The buffer carries pixels; the viewport states the logical size it
		// covers. This is the only way to express a non-integer scale.
		wp_viewport_set_destination(win->viewport, window->width, window->height);
	} else if (win->scale_120 % SKA_WL_SCALE_UNIT == 0) {
		wl_surface_set_buffer_scale(win->surface, win->scale_120 / SKA_WL_SCALE_UNIT);
	}

	// Every caller updates the logical size and then lands here, so this is the
	// single place a resize is reported. A compositor resends configure with an
	// unchanged size throughout an interactive drag, and an app that rebuilds
	// its swapchain per event cannot afford to see those.
	bool dpi_changed  = window->dpi_scale != scale;
	bool size_changed = window->drawable_width != drawable_w    || window->drawable_height != drawable_h
	                 || win->reported_width   != window->width  || win->reported_height    != window->height;

	window->dpi_scale       = scale;
	window->drawable_width  = drawable_w;
	window->drawable_height = drawable_h;

	// The compositor reports the real scale and size while the window is still
	// being created, so those are its initial values rather than a change the
	// app could act on. Recording them keeps the first real change detectable.
	if (!win->ready) {
		win->reported_width  = window->width;
		win->reported_height = window->height;
		return;
	}

	if (dpi_changed) {
		ska_wl_post_window_event(window, ska_event_window_dpi_changed, (int32_t)(scale * 100.0f + 0.5f), 0);
	}
	if (size_changed) {
		win->reported_width  = window->width;
		win->reported_height = window->height;
		ska_wl_post_window_event(window, ska_event_window_resized, window->width, window->height);
	}
}

static void ska_wl_set_scale(ska_wl_window_t* win, int32_t scale_120) {
	if (scale_120 <= 0 || scale_120 == win->scale_120) return;
	win->scale_120 = scale_120;
	ska_wl_apply_scale(win);
	ska_wl_commit_if_idle(win);
}

static void ska_wl_fractional_scale_preferred(void* data, struct wp_fractional_scale_v1* fractional, uint32_t scale) {
	(void)fractional;
	ska_wl_set_scale((ska_wl_window_t*)data, (int32_t)scale);
}

static const struct wp_fractional_scale_v1_listener k_fractional_scale_listener = {
	.preferred_scale = ska_wl_fractional_scale_preferred,
};

// ============================================================================
// Outputs
// ============================================================================

static void ska_wl_output_geometry(void* data, struct wl_output* output, int32_t x, int32_t y,
                                   int32_t physical_width, int32_t physical_height, int32_t subpixel,
                                   const char* make, const char* model, int32_t transform) {
	(void)data; (void)output; (void)x; (void)y; (void)physical_width; (void)physical_height;
	(void)subpixel; (void)make; (void)model; (void)transform;
}

static void ska_wl_output_mode(void* data, struct wl_output* output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	(void)data; (void)width; (void)height;
	if (!(flags & WL_OUTPUT_MODE_CURRENT)) return;
	ska_wl_output_t* entry = ska_wl_find_output(output);
	if (entry) entry->refresh_mhz = refresh;
}

static void ska_wl_output_scale(void* data, struct wl_output* output, int32_t factor) {
	(void)data;
	ska_wl_output_t* entry = ska_wl_find_output(output);
	if (entry) entry->scale = factor;
}

static void ska_wl_output_done(void* data, struct wl_output* output) {
	(void)data; (void)output;
	// Re-evaluate every window on the integer fallback path, since an output
	// scale change can move a surface's effective scale.
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		ska_window_t* window = g_ska.windows[i];
		if (window && window->wl && !window->wl->fractional_scale) {
			ska_wl_set_scale(window->wl, ska_wl_scale_from_outputs(window->wl));
		}
	}
}

static void ska_wl_output_name       (void* d, struct wl_output* o, const char* n) { (void)d; (void)o; (void)n; }
static void ska_wl_output_description(void* d, struct wl_output* o, const char* n) { (void)d; (void)o; (void)n; }

static const struct wl_output_listener k_output_listener = {
	.geometry    = ska_wl_output_geometry,
	.mode        = ska_wl_output_mode,
	.done        = ska_wl_output_done,
	.scale       = ska_wl_output_scale,
	.name        = ska_wl_output_name,
	.description = ska_wl_output_description,
};

// wl_surface.enter and leave track which outputs a surface touches, which is
// what the integer fallback needs to pick a scale.
static void ska_wl_surface_enter(void* data, struct wl_surface* surface, struct wl_output* output) {
	(void)surface;
	ska_wl_window_t* win = data;
	if (win->output_count >= SKA_WL_MAX_OUTPUTS) return;

	for (uint32_t i = 0; i < win->output_count; i++) {
		if (win->outputs[i] == output) return;
	}
	win->outputs[win->output_count++] = output;
	if (!win->fractional_scale) ska_wl_set_scale(win, ska_wl_scale_from_outputs(win));
}

static void ska_wl_surface_leave(void* data, struct wl_surface* surface, struct wl_output* output) {
	(void)surface;
	ska_wl_window_t* win = data;
	for (uint32_t i = 0; i < win->output_count; i++) {
		if (win->outputs[i] != output) continue;
		win->outputs[i] = win->outputs[win->output_count - 1];
		win->output_count--;
		break;
	}
	if (!win->fractional_scale) ska_wl_set_scale(win, ska_wl_scale_from_outputs(win));
}

static void ska_wl_surface_preferred_buffer_scale    (void* d, struct wl_surface* s, int32_t f) { (void)d; (void)s; (void)f; }
static void ska_wl_surface_preferred_buffer_transform(void* d, struct wl_surface* s, uint32_t t) { (void)d; (void)s; (void)t; }

static const struct wl_surface_listener k_surface_listener = {
	.enter                     = ska_wl_surface_enter,
	.leave                     = ska_wl_surface_leave,
	.preferred_buffer_scale    = ska_wl_surface_preferred_buffer_scale,
	.preferred_buffer_transform = ska_wl_surface_preferred_buffer_transform,
};

// ============================================================================
// Placeholder Surface Content
// ============================================================================

// A surface with no buffer is never mapped, so a window with no renderer would
// never appear. The buffer must outlive this call, since attach is pending.
static bool ska_wl_attach_placeholder(ska_wl_window_t* win, int32_t width, int32_t height) {
	// A surface needs a buffer to be mapped at all, but not one the size of the
	// window: the viewport already scales whatever is attached up to the window
	// size. Without a viewport the buffer is the surface size, so it has to be
	// the real one. The compositor holds this until it is replaced, and a
	// maximized window at full resolution is tens of megabytes to fill and keep.
	//
	// Idea: an ska_window_create_with_swapchain would drop this entirely. It
	// only exists because a window has to be on screen before a renderer exists
	// to draw into it, so the renderer's first buffer could map the surface
	// instead of standing in for it.
	if (win->viewport) {
		width  = 1;
		height = 1;
	}

	int32_t stride = width * 4;
	int32_t size   = stride * height;
	if (size <= 0) return false;

	int fd = memfd_create("sk_app-placeholder", MFD_CLOEXEC);
	if (fd < 0) return false;
	if (ftruncate(fd, size) != 0) {
		close(fd);
		return false;
	}

	uint32_t* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		close(fd);
		return false;
	}
	for (int32_t i = 0; i < width * height; i++) {
		pixels[i] = 0xFF000000;
	}

	struct wl_shm_pool* pool = wl_shm_create_pool(ska_wl()->shm, fd, size);
	if (win->placeholder) wl_buffer_destroy(win->placeholder);
	win->placeholder = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	munmap(pixels, size);
	close(fd);

	wl_surface_attach(win->surface, win->placeholder, 0, 0);
	// wl_surface_damage rather than damage_buffer, which needs wl_surface v4
	wl_surface_damage(win->surface, 0, 0, width, height);
	return true;
}

// ============================================================================
// Protocol Listeners
// ============================================================================

static void ska_wl_wm_base_ping(void* data, struct xdg_wm_base* wm_base, uint32_t serial) {
	(void)data;
	xdg_wm_base_pong(wm_base, serial);
}
static const struct xdg_wm_base_listener k_wm_base_listener = {
	.ping = ska_wl_wm_base_ping,
};

// Asking for server-side decorations does not mean getting them; the reply says
// what the compositor chose, and some always answer client-side.
static void ska_wl_decoration_configure(void* data, struct zxdg_toplevel_decoration_v1* decoration, uint32_t mode) {
	(void)decoration;
	ska_wl_window_t* win = data;

	bool server_side = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
	if (server_side == win->server_decorated) return;
	win->server_decorated = server_side;

	if (!server_side) {
		ska_log(ska_log_warn, "Wayland: compositor requires client-side decorations, window has no frame");
	}
}

static const struct zxdg_toplevel_decoration_v1_listener k_decoration_listener = {
	.configure = ska_wl_decoration_configure,
};

// xdg_surface.configure is the compositor agreeing on a size. Applying the
// pending toplevel size here, after the ack, is what makes the window real.
static void ska_wl_xdg_surface_configure(void* data, struct xdg_surface* xdg_surface, uint32_t serial) {
	ska_wl_window_t* win    = data;
	ska_window_t*    window = win->owner;

	xdg_surface_ack_configure(xdg_surface, serial);

	int32_t width  = win->pending_width;
	int32_t height = win->pending_height;
	if (width <= 0 || height <= 0) {
		// Compositor left the size to us, so keep what we have
		width  = window->width;
		height = window->height;
	}

	window->width  = width;
	window->height = height;
	// Recomputes the drawable size, and reports the resize if either changed
	ska_wl_apply_scale(win);

	win->configured = true;

	// A surface is on screen exactly when it has a buffer, so this is what
	// honours ska_window_hidden and what show and hide toggle. Once a renderer
	// owns the surface its own buffers do the mapping, and attaching the
	// placeholder would stomp a presented frame.
	if (window->is_visible && !win->mapped && !win->app_presents) {
		win->mapped = ska_wl_attach_placeholder(win, window->drawable_width, window->drawable_height);
		wl_surface_commit(win->surface);
	} else {
		ska_wl_commit_if_idle(win);
	}
}

static const struct xdg_surface_listener k_xdg_surface_listener = {
	.configure = ska_wl_xdg_surface_configure,
};

static void ska_wl_toplevel_configure(void* data, struct xdg_toplevel* toplevel, int32_t width, int32_t height, struct wl_array* states) {
	(void)toplevel;
	ska_wl_window_t* win = data;

	win->pending_width  = width;
	win->pending_height = height;

	bool maximized = false;
	bool fullscreen = false;
	bool activated = false;
	uint32_t* state;
	wl_array_for_each(state, states) {
		switch (*state) {
			case XDG_TOPLEVEL_STATE_MAXIMIZED:  maximized  = true; break;
			case XDG_TOPLEVEL_STATE_FULLSCREEN: fullscreen = true; break;
			case XDG_TOPLEVEL_STATE_ACTIVATED:  activated  = true; break;
			default: break;
		}
	}

	if (activated != win->activated) {
		win->owner->has_focus = activated;
		ska_wl_post_window_event(win->owner,
			activated ? ska_event_window_focus_gained : ska_event_window_focus_lost, 0, 0);
		if (!activated) {
			ska_input_release_all(win->owner->id);
		}
	}

	win->maximized             = maximized;
	win->activated             = activated;
	win->owner->is_fullscreen  = fullscreen;
}

static void ska_wl_toplevel_close(void* data, struct xdg_toplevel* toplevel) {
	(void)toplevel;
	ska_wl_window_t* win = data;
	win->owner->should_close = true;
	ska_wl_post_window_event(win->owner, ska_event_window_close, 0, 0);
}

static const struct xdg_toplevel_listener k_toplevel_listener = {
	.configure = ska_wl_toplevel_configure,
	.close     = ska_wl_toplevel_close,
};

// ============================================================================
// Keyboard
// ============================================================================

static ska_window_t* ska_wl_window_from_surface(struct wl_surface* surface) {
	if (!surface) return NULL;
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		ska_window_t* window = g_ska.windows[i];
		if (window && window->wl && window->wl->surface == surface) {
			return window;
		}
	}
	return NULL;
}

// Wayland reports evdev keycodes; xkbcommon expects them offset by 8, the same
// convention X11 uses.
#define SKA_WL_XKB_KEYCODE(evdev) ((evdev) + 8)

static uint16_t ska_wl_modifiers(void) {
	struct xkb_state* state = ska_wl()->xkb_state;
	if (!state) return ska_keymod_none;

	uint16_t mods = ska_keymod_none;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= ska_keymod_shift;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL,  XKB_STATE_MODS_EFFECTIVE) > 0) mods |= ska_keymod_ctrl;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT,   XKB_STATE_MODS_EFFECTIVE) > 0) mods |= ska_keymod_alt;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO,  XKB_STATE_MODS_EFFECTIVE) > 0) mods |= ska_keymod_gui;
	return mods;
}

static ska_scancode_ ska_wl_scancode(uint32_t evdev_key) {
	struct xkb_keymap* keymap = ska_wl()->xkb_keymap;
	if (!keymap) return ska_scancode_unknown;

	// Level 0 of layout 0, which is what X11's XLookupKeysym(&event, 0) reads.
	// A state-aware lookup would return the shifted keysym, and shift+1 has no
	// scancode of its own, so every shifted key would come back unknown.
	const xkb_keysym_t* syms  = NULL;
	int32_t             count = xkb_keymap_key_get_syms_by_level(keymap, SKA_WL_XKB_KEYCODE(evdev_key), 0, 0, &syms);
	if (count <= 0) return ska_scancode_unknown;
	return ska_linux_keysym_to_scancode((uint32_t)syms[0]);
}

// Emits the key event plus any text it produces. Shared by real key presses and
// the synthesized repeats in pump_events.
static void ska_wl_emit_key(ska_window_t* window, uint32_t evdev_key, ska_scancode_ scancode, bool pressed, bool repeat) {
	ska_event_t event = {0};
	event.timestamp          = ska_time_get_elapsed_ms();
	event.type               = pressed ? ska_event_key_down : ska_event_key_up;
	event.keyboard.window_id = window->id;
	event.keyboard.scancode  = scancode;
	event.keyboard.pressed   = pressed;
	event.keyboard.repeat    = repeat;

	if (scancode != ska_scancode_unknown) {
		g_ska.input_state.keyboard[scancode] = pressed ? 1 : 0;
	}
	uint16_t mods = ska_wl_modifiers();
	event.keyboard.modifiers        = mods;
	g_ska.input_state.key_modifiers = mods;
	ska_post_event(&event);

	if (!pressed) return;

	// The keymap is genuinely optional (a failed compile only warns at init),
	// and the xkb calls below do not tolerate a NULL state.
	ska_wl_state_t* wl = ska_wl();
	if (!wl->xkb_state) return;

	char    text[32];
	int32_t len = 0;
	bool    composed = false;

	// A dead key produces no text on its own, and the key after it produces the
	// combined character instead of its own.
	if (wl->compose_state) {
		const xkb_keysym_t* syms = NULL;
		if (xkb_state_key_get_syms(wl->xkb_state, SKA_WL_XKB_KEYCODE(evdev_key), &syms) == 1 &&
		    xkb_compose_state_feed(wl->compose_state, syms[0]) == XKB_COMPOSE_FEED_ACCEPTED) {
			switch (xkb_compose_state_get_status(wl->compose_state)) {
				case XKB_COMPOSE_COMPOSING:
					return; // Mid-sequence, so nothing to emit yet
				case XKB_COMPOSE_CANCELLED:
					xkb_compose_state_reset(wl->compose_state);
					return;
				case XKB_COMPOSE_COMPOSED:
					len = xkb_compose_state_get_utf8(wl->compose_state, text, sizeof(text));
					xkb_compose_state_reset(wl->compose_state);
					composed = true;
					break;
				case XKB_COMPOSE_NOTHING:
					break; // Not part of a sequence, fall through to the plain path
			}
		}
	}

	if (!composed) {
		len = xkb_state_key_get_utf8(wl->xkb_state, SKA_WL_XKB_KEYCODE(evdev_key), text, sizeof(text));
	}
	// Control characters are key presses, not text
	if (len > 0 && len < (int32_t)sizeof(text) && (unsigned char)text[0] >= 0x20 && text[0] != 0x7F) {
		ska_event_t text_event = {0};
		text_event.timestamp      = event.timestamp;
		text_event.type           = ska_event_text_input;
		text_event.text.window_id = window->id;
		strncpy(text_event.text.text, text, sizeof(text_event.text.text) - 1);
		ska_post_event(&text_event);
	}
}

static void ska_wl_keyboard_keymap(void* data, struct wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size) {
	(void)data; (void)keyboard;
	ska_wl_state_t* wl = ska_wl();

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !wl->xkb_context) {
		close(fd);
		return;
	}

	char* map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		return;
	}

	struct xkb_keymap* keymap = xkb_keymap_new_from_string(wl->xkb_context, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	close(fd);
	if (!keymap) {
		ska_log(ska_log_warn, "Wayland: failed to compile the compositor keymap");
		return;
	}

	struct xkb_state* state = xkb_state_new(keymap);
	if (!state) {
		xkb_keymap_unref(keymap);
		return;
	}

	if (wl->xkb_state)  xkb_state_unref(wl->xkb_state);
	if (wl->xkb_keymap) xkb_keymap_unref(wl->xkb_keymap);
	wl->xkb_keymap = keymap;
	wl->xkb_state  = state;
}

static void ska_wl_keyboard_enter(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface, struct wl_array* keys) {
	(void)data; (void)keyboard; (void)keys;
	ska_wl()->last_input_serial = serial;
	ska_wl()->keyboard_focus    = ska_wl_window_from_surface(surface);
}

static void ska_wl_keyboard_leave(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surface) {
	(void)data; (void)keyboard; (void)serial;
	ska_wl_state_t* wl     = ska_wl();
	ska_window_t*   window = ska_wl_window_from_surface(surface);

	// Keys held at the moment focus leaves would otherwise stay stuck down, and
	// a half-finished compose sequence would surface in the next focused window.
	if (window) ska_input_release_all(window->id);
	if (wl->compose_state) xkb_compose_state_reset(wl->compose_state);
	wl->repeat_key      = 0;
	wl->keyboard_focus  = NULL;
}

static void ska_wl_keyboard_key(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	(void)data; (void)keyboard; (void)time;
	ska_wl_state_t* wl     = ska_wl();
	ska_window_t*   window = wl->keyboard_focus;
	wl->last_input_serial  = serial;
	if (!window) return;

	bool          pressed  = state == WL_KEYBOARD_KEY_STATE_PRESSED;
	ska_scancode_ scancode = ska_wl_scancode(key);
	ska_wl_emit_key(window, key, scancode, pressed, false);

	// Repeat is the client's responsibility here, and only the most recent
	// pressed key repeats.
	if (pressed && wl->repeat_rate > 0 && wl->xkb_keymap &&
	    xkb_keymap_key_repeats(wl->xkb_keymap, SKA_WL_XKB_KEYCODE(key))) {
		wl->repeat_key      = key;
		wl->repeat_scancode = scancode;
		wl->repeat_next_ms  = ska_time_get_elapsed_ms() + (uint32_t)wl->repeat_delay;
	} else if (!pressed && wl->repeat_key == key) {
		wl->repeat_key = 0;
	}
}

static void ska_wl_keyboard_modifiers(void* data, struct wl_keyboard* keyboard, uint32_t serial,
                                      uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
	(void)data; (void)keyboard; (void)serial;
	if (!ska_wl()->xkb_state) return;
	xkb_state_update_mask(ska_wl()->xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
	g_ska.input_state.key_modifiers = ska_wl_modifiers();
}

static void ska_wl_keyboard_repeat_info(void* data, struct wl_keyboard* keyboard, int32_t rate, int32_t delay) {
	(void)data; (void)keyboard;
	ska_wl()->repeat_rate  = rate;
	ska_wl()->repeat_delay = delay;
}

static const struct wl_keyboard_listener k_keyboard_listener = {
	.keymap      = ska_wl_keyboard_keymap,
	.enter       = ska_wl_keyboard_enter,
	.leave       = ska_wl_keyboard_leave,
	.key         = ska_wl_keyboard_key,
	.modifiers   = ska_wl_keyboard_modifiers,
	.repeat_info = ska_wl_keyboard_repeat_info,
};

// ============================================================================
// Pointer
// ============================================================================

// All defined further down, alongside the code they belong to.
static void ska_wl_apply_cursor(void);
static void ska_wl_release_pointer_lock(void);
static void ska_wl_init_data_device(void);
static void ska_wl_init_decorations(void);
void        ska_wl_window_destroy(ska_window_t* ref_window);

static void ska_wl_pointer_enter(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface, wl_fixed_t x, wl_fixed_t y) {
	(void)data; (void)pointer;
	ska_wl_state_t* wl     = ska_wl();
	ska_window_t*   ref_window = ska_wl_window_from_surface(surface);
	if (!ref_window) return;

	wl->pointer_enter_serial = serial;
	wl->last_input_serial    = serial;
	wl->pointer_focus        = ref_window;
	ref_window->mouse_inside     = true;

	g_ska.input_state.mouse_x = wl_fixed_to_int(x);
	g_ska.input_state.mouse_y = wl_fixed_to_int(y);

	// The compositor hands over cursor responsibility on enter, and a client
	// that sets nothing leaves the previous surface's cursor on screen.
	ska_wl_apply_cursor();
	ska_wl_post_window_event(ref_window, ska_event_window_mouse_enter, 0, 0);
}

static void ska_wl_pointer_leave(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surface) {
	(void)data; (void)pointer; (void)serial;
	ska_window_t* window = ska_wl_window_from_surface(surface);
	if (!window) return;

	window->mouse_inside   = false;
	ska_wl()->pointer_focus = NULL;
	ska_wl_post_window_event(window, ska_event_window_mouse_leave, 0, 0);
}

static void ska_wl_pointer_motion(void* data, struct wl_pointer* pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	(void)data; (void)pointer; (void)time;
	ska_window_t* window = ska_wl()->pointer_focus;
	if (!window) return;

	int32_t px = wl_fixed_to_int(x);
	int32_t py = wl_fixed_to_int(y);

	ska_event_t event = {0};
	event.timestamp              = ska_time_get_elapsed_ms();
	event.type                   = ska_event_mouse_motion;
	event.mouse_motion.window_id = window->id;
	event.mouse_motion.x         = px;
	event.mouse_motion.y         = py;
	event.mouse_motion.xrel      = px - g_ska.input_state.mouse_x;
	event.mouse_motion.yrel      = py - g_ska.input_state.mouse_y;

	g_ska.input_state.mouse_x    = px;
	g_ska.input_state.mouse_y    = py;
	ska_input_add_relative(event.mouse_motion.xrel, event.mouse_motion.yrel);
	ska_post_event(&event);
}

static void ska_wl_pointer_button(void* data, struct wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	(void)data; (void)pointer; (void)time;
	ska_window_t* window = ska_wl()->pointer_focus;
	ska_wl()->last_input_serial = serial;
	if (!window) return;

	// Values from linux/input-event-codes.h, which sk_app does not include for
	// five constants.
	ska_mouse_button_ mapped;
	switch (button) {
		case 0x110: mapped = ska_mouse_button_left;   break; // BTN_LEFT
		case 0x111: mapped = ska_mouse_button_right;  break; // BTN_RIGHT
		case 0x112: mapped = ska_mouse_button_middle; break; // BTN_MIDDLE
		case 0x113: mapped = ska_mouse_button_x1;     break; // BTN_SIDE
		case 0x114: mapped = ska_mouse_button_x2;     break; // BTN_EXTRA
		default: return;
	}

	bool pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;

	ska_event_t event = {0};
	event.timestamp              = ska_time_get_elapsed_ms();
	event.type                   = pressed ? ska_event_mouse_button_down : ska_event_mouse_button_up;
	event.mouse_button.window_id = window->id;
	event.mouse_button.button    = mapped;
	event.mouse_button.pressed   = pressed;
	event.mouse_button.clicks    = 1;
	event.mouse_button.x         = g_ska.input_state.mouse_x;
	event.mouse_button.y         = g_ska.input_state.mouse_y;

	uint32_t mask = 1u << (mapped - 1);
	if (pressed) g_ska.input_state.mouse_buttons |=  mask;
	else         g_ska.input_state.mouse_buttons &= ~mask;

	ska_post_event(&event);
}

static void ska_wl_pointer_axis(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
	(void)data; (void)pointer; (void)time;
	ska_window_t* window = ska_wl()->pointer_focus;
	if (!window) return;

	// Wayland measures scroll in surface units growing downward, the opposite
	// of the wheel-click convention sk_app reports.
	float scroll = -(float)wl_fixed_to_double(value) / 10.0f;

	ska_event_t event = {0};
	event.timestamp             = ska_time_get_elapsed_ms();
	event.type                  = ska_event_mouse_wheel;
	event.mouse_wheel.window_id = window->id;
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		event.mouse_wheel.precise_y = scroll;
		event.mouse_wheel.y         = (int32_t)(scroll > 0 ? (scroll + 0.5f) : (scroll - 0.5f));
	} else {
		event.mouse_wheel.precise_x = scroll;
		event.mouse_wheel.x         = (int32_t)(scroll > 0 ? (scroll + 0.5f) : (scroll - 0.5f));
	}
	ska_post_event(&event);
}

// Everything below exists only because a listener with a NULL member crashes
// when the compositor sends that event.
static void ska_wl_pointer_frame                  (void* d, struct wl_pointer* p) { (void)d; (void)p; }
static void ska_wl_pointer_axis_source            (void* d, struct wl_pointer* p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ska_wl_pointer_axis_stop              (void* d, struct wl_pointer* p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void ska_wl_pointer_axis_discrete          (void* d, struct wl_pointer* p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void ska_wl_pointer_axis_value120          (void* d, struct wl_pointer* p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static void ska_wl_pointer_axis_relative_direction(void* d, struct wl_pointer* p, uint32_t a, uint32_t dir) { (void)d; (void)p; (void)a; (void)dir; }

static const struct wl_pointer_listener k_pointer_listener = {
	.enter                   = ska_wl_pointer_enter,
	.leave                   = ska_wl_pointer_leave,
	.motion                  = ska_wl_pointer_motion,
	.button                  = ska_wl_pointer_button,
	.axis                    = ska_wl_pointer_axis,
	.frame                   = ska_wl_pointer_frame,
	.axis_source             = ska_wl_pointer_axis_source,
	.axis_stop               = ska_wl_pointer_axis_stop,
	.axis_discrete           = ska_wl_pointer_axis_discrete,
	.axis_value120           = ska_wl_pointer_axis_value120,
	.axis_relative_direction = ska_wl_pointer_axis_relative_direction,
};

// ============================================================================
// Seat
// ============================================================================

// release replaced destroy in seat version 3, and calling it on an older
// compositor is a protocol error.
static void ska_wl_release_pointer(struct wl_pointer* pointer) {
	if (wl_pointer_get_version(pointer) >= WL_POINTER_RELEASE_SINCE_VERSION) wl_pointer_release(pointer);
	else                                                                     wl_pointer_destroy(pointer);
}

static void ska_wl_release_keyboard(struct wl_keyboard* keyboard) {
	if (wl_keyboard_get_version(keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION) wl_keyboard_release(keyboard);
	else                                                                        wl_keyboard_destroy(keyboard);
}

static void ska_wl_seat_capabilities(void* data, struct wl_seat* seat, uint32_t capabilities) {
	(void)data;
	ska_wl_state_t* wl = ska_wl();

	bool has_pointer  = (capabilities & WL_SEAT_CAPABILITY_POINTER)  != 0;
	bool has_keyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;

	if (has_pointer && !wl->pointer) {
		wl->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(wl->pointer, &k_pointer_listener, NULL);

		// The device is bound to this pointer, so its lifetime matches
		if (wl->cursor_shape_manager) {
			wl->cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(wl->cursor_shape_manager, wl->pointer);
		}
	} else if (!has_pointer && wl->pointer) {
		if (wl->cursor_shape_device) {
			wp_cursor_shape_device_v1_destroy(wl->cursor_shape_device);
			wl->cursor_shape_device = NULL;
		}
		ska_wl_release_pointer(wl->pointer);
		wl->pointer = NULL;
	}

	if (has_keyboard && !wl->keyboard) {
		wl->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(wl->keyboard, &k_keyboard_listener, NULL);
	} else if (!has_keyboard && wl->keyboard) {
		ska_wl_release_keyboard(wl->keyboard);
		wl->keyboard = NULL;
	}
}

static void ska_wl_seat_name(void* data, struct wl_seat* seat, const char* name) {
	(void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener k_seat_listener = {
	.capabilities = ska_wl_seat_capabilities,
	.name         = ska_wl_seat_name,
};

static void ska_wl_registry_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
	(void)data;
	ska_wl_state_t* wl = ska_wl();

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		uint32_t bind_version = version < 4 ? version : 4;
		wl->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, bind_version);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		wl->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		// Single-seat: a second seat would strand the first's listeners
		if (wl->seat) return;
		// Version 5 covers the pointer axis events sk_app reads; binding higher
		// would mean handling events with no listener entry.
		uint32_t bind_version = version < 5 ? version : 5;
		wl->seat = wl_registry_bind(registry, name, &wl_seat_interface, bind_version);
		wl_seat_add_listener(wl->seat, &k_seat_listener, NULL);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		wl->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wl->wm_base, &k_wm_base_listener, NULL);
	} else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
		wl->decoration_manager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
		wl->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
	} else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
		wl->fractional_scale_manager = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
	} else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
		wl->relative_pointer_manager = wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
	} else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
		wl->cursor_shape_manager = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
	} else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
		wl->pointer_constraints = wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
	} else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
		// Version 3 for the cancelled event, which says when another client
		// takes the selection away.
		uint32_t bind_version = version < 3 ? version : 3;
		wl->data_device_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, bind_version);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		if (wl->output_count >= SKA_WL_MAX_OUTPUTS) return;
		// Version 2 for wl_output.scale, which the integer fallback needs
		uint32_t bind_version = version < 2 ? version : 2;
		ska_wl_output_t* entry = &wl->outputs[wl->output_count++];
		entry->output        = wl_registry_bind(registry, name, &wl_output_interface, bind_version);
		entry->registry_name = name;
		entry->scale         = 1;
		wl_output_add_listener(entry->output, &k_output_listener, NULL);
	}
}

static void ska_wl_registry_global_remove(void* data, struct wl_registry* registry, uint32_t name) {
	(void)data; (void)registry;
	ska_wl_state_t* wl = ska_wl();

	// Outputs are the globals that actually come and go, when a monitor is
	// unplugged or a mode changes.
	for (uint32_t i = 0; i < wl->output_count; i++) {
		if (wl->outputs[i].registry_name != name) continue;

		// Windows hold raw wl_output pointers for the scale fallback, so
		// those references have to go before the proxy does.
		for (uint32_t w = 0; w < SKA_MAX_WINDOWS; w++) {
			ska_wl_window_t* win = g_ska.windows[w] ? g_ska.windows[w]->wl : NULL;
			if (!win) continue;
			for (uint32_t o = 0; o < win->output_count; o++) {
				if (win->outputs[o] != wl->outputs[i].output) continue;
				win->outputs[o] = win->outputs[win->output_count - 1];
				win->output_count--;
				break;
			}
		}

		wl_output_destroy(wl->outputs[i].output);
		wl->outputs[i] = wl->outputs[wl->output_count - 1];
		wl->output_count--;
		return;
	}
}

static const struct wl_registry_listener k_registry_listener = {
	.global        = ska_wl_registry_global,
	.global_remove = ska_wl_registry_global_remove,
};

// ============================================================================
// Init and Shutdown
// ============================================================================

// What the compositor and the loaded libraries do not offer. Read after the
// registry, xkb, decorations, and the cursor fallback have all settled, so
// each entry reflects the state the app will actually run with.
static void ska_wl_log_optional(void) {
	ska_wl_state_t* wl = ska_wl();

	const ska_linux_optional_t items[] = {
		{ "xdg-decoration or libdecor",             wl->decoration_manager || wl->decor,                       "windows have no title bar or border" },
		{ "cursor-shape-v1 or a cursor theme",      wl->cursor_shape_manager || wl->cursor_theme,              "the cursor stays whatever the compositor draws" },
		{ "fractional-scale-v1 and viewporter",     wl->fractional_scale_manager && wl->viewporter,            "display scaling rounds to whole steps" },
		{ "pointer-constraints and relative-pointer", wl->pointer_constraints && wl->relative_pointer_manager, "relative mouse mode is unavailable" },
		{ "wl_data_device_manager",                 wl->data_device_manager != NULL,                           "the clipboard is unavailable" },
		{ "compose table for this locale",          wl->compose_state != NULL,                                 "dead keys do not combine into accented characters" },
	};

	ska_linux_log_optional("Wayland", items, sizeof(items) / sizeof(items[0]));
}

bool ska_wl_init(void) {
	if (!ska_wl_dyn_load()) {
		return false;
	}

	ska_wl_state_t* wl = ska_calloc(1, sizeof(ska_wl_state_t));
	if (!wl) return false;

	wl->display = wl_display_connect(NULL);
	if (!wl->display) {
		ska_free(wl);
		ska_wl_dyn_unload();
		return false;
	}
	g_ska.wl = wl;

	wl->registry = wl_display_get_registry(wl->display);
	wl_registry_add_listener(wl->registry, &k_registry_listener, NULL);
	// The first roundtrip binds the globals; the events those binds provoke,
	// wl_seat.capabilities and wl_output.mode, only arrive on the second.
	wl_display_roundtrip(wl->display);
	wl_display_roundtrip(wl->display);

	// A compositor with no xdg_wm_base cannot give us a real window, so fail
	// here and let backend selection fall through to X11.
	if (!wl->compositor || !wl->shm || !wl->wm_base) {
		ska_log(ska_log_warn, "Wayland compositor is missing wl_compositor, wl_shm, or xdg_wm_base");
		wl_display_disconnect(wl->display);
		ska_free(wl);
		g_ska.wl = NULL;
		ska_wl_dyn_unload();
		return false;
	}

	ska_wl_init_data_device();

	// Before anything that dispatches: libdecor roundtrips internally, and the
	// keymap event for the seat bound above is already queued up.
	wl->cursor_visible = true;
	wl->cursor         = ska_system_cursor_arrow;
	wl->xkb_context    = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!wl->xkb_context) {
		ska_log(ska_log_warn, "Wayland: no xkb context, keyboard input is unavailable");
	} else {
		const char* locale = getenv("LC_ALL");
		if (!locale || !*locale) locale = getenv("LC_CTYPE");
		if (!locale || !*locale) locale = getenv("LANG");
		if (!locale || !*locale) locale = "C";

		// Missing compose data only costs dead keys, which the optional
		// summary reports rather than warning about
		wl->compose_table = xkb_compose_table_new_from_locale(wl->xkb_context, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
		if (wl->compose_table) {
			wl->compose_state = xkb_compose_state_new(wl->compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
		}
	}

	ska_wl_init_decorations();

	// Only needed where the compositor will not draw the cursor itself. Theme
	// and size follow the usual environment overrides, which is a guess at what
	// the desktop is actually using, so this is the fallback rather than the
	// first choice.
	if (!wl->cursor_shape_manager) {
		const char* theme_name = getenv("XCURSOR_THEME");
		const char* size_text  = getenv("XCURSOR_SIZE");
		int32_t     size       = size_text ? atoi(size_text) : 0;
		if (size <= 0) size = 24;

		wl->cursor_theme = wl_cursor_theme_load(theme_name, size, wl->shm);
		if (wl->cursor_theme) {
			wl->cursor_surface = wl_compositor_create_surface(wl->compositor);
		}
	}

	ska_wl_log_optional();
	return true;
}

void ska_wl_shutdown(void) {
	ska_wl_state_t* wl = ska_wl();
	if (!wl) return;

	if (wl->compose_state) xkb_compose_state_unref(wl->compose_state);
	if (wl->compose_table) xkb_compose_table_unref(wl->compose_table);
	if (wl->xkb_state)   xkb_state_unref(wl->xkb_state);
	if (wl->xkb_keymap)  xkb_keymap_unref(wl->xkb_keymap);
	if (wl->xkb_context) xkb_context_unref(wl->xkb_context);

	if (wl->cursor_shape_device)  wp_cursor_shape_device_v1_destroy (wl->cursor_shape_device);
	if (wl->cursor_shape_manager) wp_cursor_shape_manager_v1_destroy(wl->cursor_shape_manager);
	if (wl->cursor_surface) wl_surface_destroy(wl->cursor_surface);
	if (wl->cursor_theme)   wl_cursor_theme_destroy(wl->cursor_theme);

	if (wl->pointer)  ska_wl_release_pointer(wl->pointer);
	if (wl->keyboard) ska_wl_release_keyboard(wl->keyboard);
	if (wl->seat)     wl_seat_destroy(wl->seat);

	for (uint32_t i = 0; i < wl->output_count; i++) {
		wl_output_destroy(wl->outputs[i].output);
	}

	if (wl->decor) {
		libdecor_unref(wl->decor);
		ska_wl_decor_unload();
	}
	if (wl->clipboard_text)      ska_free(wl->clipboard_text);
	if (wl->data_source)         wl_data_source_destroy(wl->data_source);
	if (wl->pending_offer && wl->pending_offer != wl->data_offer) wl_data_offer_destroy(wl->pending_offer);
	if (wl->data_offer)          wl_data_offer_destroy(wl->data_offer);
	if (wl->data_device)         wl_data_device_destroy(wl->data_device);
	if (wl->data_device_manager) wl_data_device_manager_destroy(wl->data_device_manager);

	ska_wl_release_pointer_lock();
	if (wl->pointer_constraints)      zwp_pointer_constraints_v1_destroy(wl->pointer_constraints);
	if (wl->relative_pointer_manager) zwp_relative_pointer_manager_v1_destroy(wl->relative_pointer_manager);
	if (wl->fractional_scale_manager) wp_fractional_scale_manager_v1_destroy(wl->fractional_scale_manager);
	if (wl->viewporter)               wp_viewporter_destroy(wl->viewporter);
	if (wl->decoration_manager) zxdg_decoration_manager_v1_destroy(wl->decoration_manager);
	if (wl->wm_base)            xdg_wm_base_destroy(wl->wm_base);
	if (wl->shm)                wl_shm_destroy(wl->shm);
	if (wl->compositor)         wl_compositor_destroy(wl->compositor);
	if (wl->registry)           wl_registry_destroy(wl->registry);
	wl_display_disconnect(wl->display);

	ska_free(wl);
	g_ska.wl = NULL;
	ska_wl_dyn_unload();
}

// ============================================================================
// Window
// ============================================================================

// ============================================================================
// Client-Side Decorations
// ============================================================================

static void ska_wl_decor_error(struct libdecor* context, enum libdecor_error error, const char* message) {
	(void)context; (void)error;
	ska_log(ska_log_warn, "libdecor: %s", message);
}

static struct libdecor_interface k_decor_interface = {
	.error = ska_wl_decor_error,
};

// libdecor owns the xdg_surface, so this replaces the plain configure path
// rather than adding to it. The size it reports already excludes the frame.
static void ska_wl_decor_frame_configure(struct libdecor_frame* frame, struct libdecor_configuration* configuration, void* user_data) {
	ska_wl_window_t* win    = user_data;
	ska_window_t*    window = win->owner;

	int width  = 0;
	int height = 0;
	if (!libdecor_configuration_get_content_size(configuration, frame, &width, &height)) {
		// No size means the app chooses, so keep what it already has
		width  = window->width;
		height = window->height;
	}

	window->width  = width;
	window->height = height;
	ska_wl_apply_scale(win);

	enum libdecor_window_state state = LIBDECOR_WINDOW_STATE_NONE;
	if (libdecor_configuration_get_window_state(configuration, &state)) {
		bool activated = (state & LIBDECOR_WINDOW_STATE_ACTIVE) != 0;
		win->maximized           = (state & LIBDECOR_WINDOW_STATE_MAXIMIZED)  != 0;
		window->is_fullscreen    = (state & LIBDECOR_WINDOW_STATE_FULLSCREEN) != 0;
		if (activated != win->activated) {
			win->activated    = activated;
			window->has_focus = activated;
			ska_wl_post_window_event(window,
				activated ? ska_event_window_focus_gained : ska_event_window_focus_lost, 0, 0);
			if (!activated) ska_input_release_all(window->id);
		}
	}

	struct libdecor_state* decor_state = libdecor_state_new(width, height);
	libdecor_frame_commit(frame, decor_state, configuration);
	libdecor_state_free(decor_state);

	win->configured = true;
	if (window->is_visible && !win->mapped && !win->app_presents) {
		win->mapped = ska_wl_attach_placeholder(win, window->drawable_width, window->drawable_height);
		wl_surface_commit(win->surface);
	} else {
		ska_wl_commit_if_idle(win);
	}
}

static void ska_wl_decor_frame_close(struct libdecor_frame* frame, void* user_data) {
	(void)frame;
	ska_wl_window_t* win = user_data;
	win->owner->should_close = true;
	ska_wl_post_window_event(win->owner, ska_event_window_close, 0, 0);
}

// libdecor requires this commit to apply frame state, so it stays
// unconditional even once a renderer owns the surface.
static void ska_wl_decor_frame_commit(struct libdecor_frame* frame, void* user_data) {
	(void)frame;
	ska_wl_window_t* win = user_data;
	wl_surface_commit(win->surface);
}

static void ska_wl_decor_frame_dismiss_popup(struct libdecor_frame* f, const char* seat_name, void* user_data) {
	(void)f; (void)seat_name; (void)user_data;
}

static struct libdecor_frame_interface k_decor_frame_interface = {
	.configure     = ska_wl_decor_frame_configure,
	.close         = ska_wl_decor_frame_close,
	.commit        = ska_wl_decor_frame_commit,
	.dismiss_popup = ska_wl_decor_frame_dismiss_popup,
};

// Only loaded when the compositor will not decorate, so KDE and wlroots never
// pay for it. SKA_WAYLAND_FORCE_CSD takes this path anyway, to exercise it.
static void ska_wl_init_decorations(void) {
	ska_wl_state_t* wl = ska_wl();

	const char* force = getenv("SKA_WAYLAND_FORCE_CSD");
	bool force_csd = force && force[0] != '\0' && force[0] != '0';
	if (wl->decoration_manager && !force_csd) return;
	if (!ska_wl_decor_load()) return;

	wl->decor = libdecor_new(wl->display, &k_decor_interface);
	if (!wl->decor) {
		ska_wl_decor_unload();
		return;
	}
	ska_log(ska_log_info, "Wayland: drawing window frames with libdecor%s",
		wl->decoration_manager ? " (forced by SKA_WAYLAND_FORCE_CSD)" : " (no xdg-decoration)");
}

// Builds the xdg_surface, toplevel, and decoration for a surface, shared by
// create and the libdecor path.
// Drops the role objects while leaving the wl_surface alone, so the window can
// be rebuilt by ska_wl_build_toplevel without the app's native handle changing.
static void ska_wl_destroy_toplevel(ska_wl_window_t* win) {
	// The decoration has to go before its toplevel; the reverse order is the
	// orphaned protocol error, which kills the whole connection.
	if (win->decoration)   { zxdg_toplevel_decoration_v1_destroy(win->decoration); win->decoration = NULL; }
	if (win->decor_frame)  { libdecor_frame_unref (win->decor_frame);  win->decor_frame  = NULL; }
	if (win->xdg_toplevel) { xdg_toplevel_destroy (win->xdg_toplevel); win->xdg_toplevel = NULL; }
	if (win->xdg_surface)  { xdg_surface_destroy  (win->xdg_surface);  win->xdg_surface  = NULL; }
}

static void ska_wl_build_toplevel(ska_wl_window_t* win, int32_t w, int32_t h) {
	ska_wl_state_t* wl     = ska_wl();
	ska_window_t*   window = win->owner;
	uint32_t        flags  = window->flags;

	// libdecor creates and owns the xdg_surface, so the two paths are mutually
	// exclusive rather than layered.
	// app_id is how a compositor matches the window to its .desktop file, which
	// is also the only way a Wayland window gets an icon.
	const char* title  = window->title ? window->title : "";
	const char* app_id = g_ska.app_id  ? g_ska.app_id  : title;

	if (wl->decor && !(flags & ska_window_borderless)) {
		win->decor_frame = libdecor_decorate(wl->decor, win->surface, &k_decor_frame_interface, win);
		if (win->decor_frame) {
			libdecor_frame_set_title (win->decor_frame, title);
			libdecor_frame_set_app_id(win->decor_frame, app_id);
			if (!(flags & ska_window_resizable)) {
				libdecor_frame_set_min_content_size(win->decor_frame, w, h);
				libdecor_frame_set_max_content_size(win->decor_frame, w, h);
			}
			if (flags & ska_window_fullscreen) libdecor_frame_set_fullscreen(win->decor_frame, NULL);
			if (flags & ska_window_maximized)  libdecor_frame_set_maximized (win->decor_frame);

			libdecor_frame_map(win->decor_frame);
			return;
		}
		ska_log(ska_log_warn, "libdecor: could not decorate the window, falling back to no frame");
	}

	win->xdg_surface = xdg_wm_base_get_xdg_surface(wl->wm_base, win->surface);
	xdg_surface_add_listener(win->xdg_surface, &k_xdg_surface_listener, win);
	win->xdg_toplevel = xdg_surface_get_toplevel(win->xdg_surface);
	xdg_toplevel_add_listener(win->xdg_toplevel, &k_toplevel_listener, win);

	xdg_toplevel_set_title (win->xdg_toplevel, title);
	xdg_toplevel_set_app_id(win->xdg_toplevel, app_id);

	if (wl->decoration_manager && !wl->decor && !(flags & ska_window_borderless)) {
		win->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(wl->decoration_manager, win->xdg_toplevel);
		zxdg_toplevel_decoration_v1_add_listener(win->decoration, &k_decoration_listener, win);
		zxdg_toplevel_decoration_v1_set_mode(win->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	} else if (!wl->decoration_manager && !wl->decor && !(flags & ska_window_borderless)) {
		ska_log(ska_log_warn, "Wayland: no xdg-decoration support, window has no frame");
	}

	if (flags & ska_window_fullscreen) xdg_toplevel_set_fullscreen(win->xdg_toplevel, NULL);
	if (flags & ska_window_maximized)  xdg_toplevel_set_maximized (win->xdg_toplevel);
	if (!(flags & ska_window_resizable)) {
		xdg_toplevel_set_min_size(win->xdg_toplevel, w, h);
		xdg_toplevel_set_max_size(win->xdg_toplevel, w, h);
	}
}

bool ska_wl_window_create(ska_window_t* ref_window, const char* title, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t flags) {
	// Wayland gives clients no say in window placement
	(void)x;
	(void)y;

	ska_wl_state_t*  wl  = ska_wl();
	ska_wl_window_t* win = ska_calloc(1, sizeof(ska_wl_window_t));
	if (!win) {
		ska_set_error("ska_window_create: out of memory");
		return false;
	}
	win->owner     = ref_window;
	win->scale_120 = SKA_WL_SCALE_UNIT;
	ref_window->wl          = win;
	ref_window->title       = ska_strdup(title);

	win->surface = wl_compositor_create_surface(wl->compositor);
	if (!win->surface) {
		ska_set_error("ska_window_create: wl_compositor_create_surface failed");
		ska_free(win);
		ref_window->wl = NULL;
		return false;
	}
	wl_surface_add_listener(win->surface, &k_surface_listener, win);

	// fractional-scale-v1 gives an exact scale, but needs viewporter to express
	// the logical size a non-integer-scaled buffer covers.
	if (wl->fractional_scale_manager && wl->viewporter) {
		win->viewport = wp_viewporter_get_viewport(wl->viewporter, win->surface);
		win->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(wl->fractional_scale_manager, win->surface);
		wp_fractional_scale_v1_add_listener(win->fractional_scale, &k_fractional_scale_listener, win);
	}

	ska_wl_build_toplevel(win, w, h);

	ref_window->x               = 0;
	ref_window->y               = 0;
	ref_window->width           = w;
	ref_window->height          = h;
	ref_window->drawable_width  = w;
	ref_window->drawable_height = h;
	ref_window->dpi_scale       = 1.0f;
	// The configure handler maps based on this, so it must be set beforehand
	ref_window->is_visible      = !(flags & ska_window_hidden);

	// The surface is not usable until the compositor has configured it, so
	// block here rather than hand back a wl_surface a swapchain cannot use.
	wl_surface_commit(win->surface);
	if (!ska_wl_wait_configured(win, 2000)) {
		// The caller only frees the ska_window_t, so the surface, the toplevel,
		// and everything hung off them have to go here.
		ska_set_error("ska_window_create: no configure from the compositor");
		ska_wl_window_destroy(ref_window);
		return false;
	}

	win->ready = true;
	if (ref_window->is_visible) {
		ska_wl_post_window_event(ref_window, ska_event_window_shown, 0, 0);
	}
	return true;
}

void ska_wl_window_destroy(ska_window_t* ref_window) {
	ska_wl_window_t* win = ref_window->wl;
	if (!win) return;

	// Input state points at windows, so it has to let go before the window
	// does. A pointer lock also names this surface and must not outlive it.
	ska_wl_state_t* wl = ska_wl();
	if (wl->pointer_focus  == ref_window) wl->pointer_focus  = NULL;
	if (wl->keyboard_focus == ref_window) {
		wl->keyboard_focus = NULL;
		wl->repeat_key     = 0;
	}
	if (wl->locked_pointer) ska_wl_release_pointer_lock();

	ska_wl_destroy_toplevel(win);
	if (win->fractional_scale) wp_fractional_scale_v1_destroy(win->fractional_scale);
	if (win->viewport)         wp_viewport_destroy(win->viewport);
	if (win->surface)          wl_surface_destroy(win->surface);
	if (win->placeholder)      wl_buffer_destroy(win->placeholder);

	ska_free(win);
	ref_window->wl = NULL;
	wl_display_flush(ska_wl()->display);
}

void ska_wl_window_set_title(ska_window_t* ref_window, const char* title) {
	if (ref_window->title) ska_free(ref_window->title);
	ref_window->title = ska_strdup(title);
	// A hidden window has no role object; build_toplevel replays the title
	if      (ref_window->wl->decor_frame)  libdecor_frame_set_title(ref_window->wl->decor_frame, title);
	else if (ref_window->wl->xdg_toplevel) xdg_toplevel_set_title  (ref_window->wl->xdg_toplevel, title);
	wl_display_flush(ska_wl()->display);
}

// Clients can neither place their windows nor read back where they landed, so
// both halves are no-ops and ska_event_window_moved never fires.
void ska_wl_window_set_frame_position(ska_window_t* ref_window, int32_t x, int32_t y) {
	(void)ref_window; (void)x; (void)y;
}

void ska_wl_get_frame_extents(const ska_window_t* window, int32_t* opt_out_left, int32_t* opt_out_right, int32_t* opt_out_top, int32_t* opt_out_bottom) {
	// Server-side extents are never reported, and reporting libdecor's would
	// make the same call mean different things per compositor.
	(void)window;
	if (opt_out_left)   *opt_out_left   = 0;
	if (opt_out_right)  *opt_out_right  = 0;
	if (opt_out_top)    *opt_out_top    = 0;
	if (opt_out_bottom) *opt_out_bottom = 0;
}

void ska_wl_window_set_frame_size(ska_window_t* ref_window, int32_t w, int32_t h) {
	ska_wl_window_t* win = ref_window->wl;
	if (w <= 0 || h <= 0) return;

	// There is no request to resize a toplevel; the client just starts
	// rendering at the new size and reports it on the next commit.
	ref_window->width  = w;
	ref_window->height = h;
	ska_wl_apply_scale(win);

	// A fixed-size window is expressed as min == max, so both move with it
	if (!(ref_window->flags & ska_window_resizable)) {
		if (win->decor_frame) {
			libdecor_frame_set_min_content_size(win->decor_frame, w, h);
			libdecor_frame_set_max_content_size(win->decor_frame, w, h);
		} else if (win->xdg_toplevel) {
			xdg_toplevel_set_min_size(win->xdg_toplevel, w, h);
			xdg_toplevel_set_max_size(win->xdg_toplevel, w, h);
		}
	}

	if (win->decor_frame) {
		// libdecor owns the xdg_surface, so the new size is expressed by
		// committing a state rather than setting the geometry directly.
		struct libdecor_state* state = libdecor_state_new(w, h);
		libdecor_frame_commit(win->decor_frame, state, NULL);
		libdecor_state_free(state);
	} else if (win->xdg_surface) {
		xdg_surface_set_window_geometry(win->xdg_surface, 0, 0, w, h);
		ska_wl_commit_if_idle(win);
	}
	wl_display_flush(ska_wl()->display);
}

void ska_wl_window_show(ska_window_t* ref_window) {
	ska_wl_window_t* win = ref_window->wl;
	if (ref_window->is_visible) return;

	ref_window->is_visible = true;

	if (win->configured) {
		// Created hidden and never mapped: the initial configure was already
		// acked, so a buffer can go on right away. A renderer that already
		// owns the surface maps it with its own next frame instead.
		if (!win->app_presents) {
			win->mapped = ska_wl_attach_placeholder(win, ref_window->drawable_width, ref_window->drawable_height);
			wl_surface_commit(win->surface);
		} else {
			win->mapped = true;
		}
	} else {
		// Hiding destroyed the role objects, so this is the same handshake a
		// new window performs, and the configure it waits on is a fresh one.
		ska_wl_build_toplevel(win, ref_window->width, ref_window->height);
		wl_surface_commit(win->surface);
		ska_wl_wait_configured(win, 2000);
	}
	wl_display_flush(ska_wl()->display);
	ska_wl_post_window_event(ref_window, ska_event_window_shown, 0, 0);
}

void ska_wl_window_hide(ska_window_t* ref_window) {
	ska_wl_window_t* win = ref_window->wl;
	if (!ref_window->is_visible) return;

	// A null buffer unmaps the surface, and the role objects go with it. Keeping
	// them would leave the configures the compositor already sent addressed to a
	// surface it now treats as unconfigured, and acking one of those on show is
	// a fatal protocol error. Destroyed proxies drop their events instead.
	wl_surface_attach(win->surface, NULL, 0, 0);
	wl_surface_commit(win->surface);
	ska_wl_destroy_toplevel(win);
	wl_display_flush(ska_wl()->display);

	win->mapped        = false;
	win->configured    = false;
	ref_window->is_visible = false;
	ska_wl_post_window_event(ref_window, ska_event_window_hidden, 0, 0);
}

void ska_wl_window_maximize(ska_window_t* ref_window) {
	ref_window->flags |= ska_window_maximized;
	if      (ref_window->wl->decor_frame)  libdecor_frame_set_maximized(ref_window->wl->decor_frame);
	else if (ref_window->wl->xdg_toplevel) xdg_toplevel_set_maximized  (ref_window->wl->xdg_toplevel);
	wl_display_flush(ska_wl()->display);
}

void ska_wl_window_minimize(ska_window_t* ref_window) {
	// One-way: the compositor never reports a minimized state back
	if      (ref_window->wl->decor_frame)  libdecor_frame_set_minimized(ref_window->wl->decor_frame);
	else if (ref_window->wl->xdg_toplevel) xdg_toplevel_set_minimized  (ref_window->wl->xdg_toplevel);
	wl_display_flush(ska_wl()->display);
}

void ska_wl_window_restore(ska_window_t* ref_window) {
	ska_wl_window_t* win = ref_window->wl;
	ref_window->flags &= ~(uint32_t)(ska_window_maximized | ska_window_fullscreen);
	if (win->decor_frame) {
		if (ref_window->is_fullscreen) libdecor_frame_unset_fullscreen(win->decor_frame);
		if (win->maximized)            libdecor_frame_unset_maximized (win->decor_frame);
	} else if (win->xdg_toplevel) {
		if (ref_window->is_fullscreen) xdg_toplevel_unset_fullscreen(win->xdg_toplevel);
		if (win->maximized)            xdg_toplevel_unset_maximized (win->xdg_toplevel);
	}
	wl_display_flush(ska_wl()->display);
}

void ska_wl_window_set_fullscreen(ska_window_t* ref_window, bool fullscreen) {
	ska_wl_window_t* win = ref_window->wl;
	if (fullscreen) ref_window->flags |=  (uint32_t)ska_window_fullscreen;
	else            ref_window->flags &= ~(uint32_t)ska_window_fullscreen;
	if (win->decor_frame) {
		if (fullscreen) libdecor_frame_set_fullscreen  (win->decor_frame, NULL);
		else            libdecor_frame_unset_fullscreen(win->decor_frame);
	} else if (win->xdg_toplevel) {
		if (fullscreen) xdg_toplevel_set_fullscreen  (win->xdg_toplevel, NULL);
		else            xdg_toplevel_unset_fullscreen(win->xdg_toplevel);
	}
	wl_display_flush(ska_wl()->display);
}

void ska_wl_window_raise(ska_window_t* ref_window) {
	// Needs xdg-activation-v1 with a token from another surface, which sk_app
	// has no source for yet. Compositors reject self-activation anyway.
	(void)ref_window;
}

void ska_wl_window_get_drawable_size(ska_window_t* ref_window, int32_t* opt_out_width, int32_t* opt_out_height) {
	if (opt_out_width)  *opt_out_width  = ref_window->drawable_width;
	if (opt_out_height) *opt_out_height = ref_window->drawable_height;
}

void* ska_wl_get_native_handle(const ska_window_t* window) {
	return window->wl ? window->wl->surface : NULL;
}

void* ska_wl_get_display(void) {
	return ska_wl()->display;
}

float ska_wl_get_dpi_scale(const ska_window_t* window) {
	return window ? window->dpi_scale : 1.0f;
}

float ska_wl_get_refresh_rate(const ska_window_t* window) {
	ska_wl_state_t* wl = ska_wl();

	// The output the window sits on, falling back to the first one for a
	// window the compositor has not placed yet.
	struct wl_output* target = NULL;
	if (window && window->wl && window->wl->output_count > 0) {
		target = window->wl->outputs[0];
	}

	for (uint32_t i = 0; i < wl->output_count; i++) {
		if (target && wl->outputs[i].output != target) continue;
		return (float)wl->outputs[i].refresh_mhz / 1000.0f;
	}
	return 0.0f;
}

// ============================================================================
// Input
// ============================================================================

// A hidden cursor is a null surface, and this is only accepted while the pointer
// is over our surface, so it is re-applied on every enter.
static void ska_wl_apply_cursor(void) {
	ska_wl_state_t* wl = ska_wl();
	if (!wl->pointer) return;

	if (!wl->cursor_visible) {
		wl_pointer_set_cursor(wl->pointer, wl->pointer_enter_serial, NULL, 0, 0);
		return;
	}

	// Naming a shape lets the compositor draw its own cursor, which is the only
	// way to match the desktop's theme, size and scale. Loading a theme here
	// instead means guessing at all three.
	if (wl->cursor_shape_device) {
		static const uint32_t shapes[] = {
			[ska_system_cursor_arrow]     = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
			[ska_system_cursor_ibeam]     = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT,
			[ska_system_cursor_wait]      = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT,
			[ska_system_cursor_crosshair] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR,
			[ska_system_cursor_waitarrow] = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS,
			[ska_system_cursor_sizenwse]  = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE,
			[ska_system_cursor_sizenesw]  = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE,
			[ska_system_cursor_sizewe]    = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,
			[ska_system_cursor_sizens]    = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,
			[ska_system_cursor_sizeall]   = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ALL_SCROLL,
			[ska_system_cursor_no]        = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED,
			[ska_system_cursor_hand]      = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
		};
		wp_cursor_shape_device_v1_set_shape(wl->cursor_shape_device, wl->pointer_enter_serial, shapes[wl->cursor]);
		return;
	}

	if (!wl->cursor_theme || !wl->cursor_surface) return;

	// Freedesktop cursor names, matching the X11 backend's list
	static const char* names[] = {
		[ska_system_cursor_arrow]     = "default",
		[ska_system_cursor_ibeam]     = "text",
		[ska_system_cursor_wait]      = "wait",
		[ska_system_cursor_crosshair] = "crosshair",
		[ska_system_cursor_waitarrow] = "progress",
		[ska_system_cursor_sizenwse]  = "nwse-resize",
		[ska_system_cursor_sizenesw]  = "nesw-resize",
		[ska_system_cursor_sizewe]    = "ew-resize",
		[ska_system_cursor_sizens]    = "ns-resize",
		[ska_system_cursor_sizeall]   = "all-scroll",
		[ska_system_cursor_no]        = "not-allowed",
		[ska_system_cursor_hand]      = "pointer",
	};

	struct wl_cursor* cursor = wl_cursor_theme_get_cursor(wl->cursor_theme, names[wl->cursor]);
	if (!cursor && wl->cursor != ska_system_cursor_arrow) {
		cursor = wl_cursor_theme_get_cursor(wl->cursor_theme, "default");
	}
	if (!cursor || cursor->image_count == 0) return;

	// Animated cursors are not driven, so the first frame stands in
	struct wl_cursor_image* image  = cursor->images[0];
	struct wl_buffer*       buffer = wl_cursor_image_get_buffer(image);
	if (!buffer) return;

	wl_pointer_set_cursor(wl->pointer, wl->pointer_enter_serial, wl->cursor_surface,
		(int32_t)image->hotspot_x, (int32_t)image->hotspot_y);
	wl_surface_attach(wl->cursor_surface, buffer, 0, 0);
	wl_surface_damage(wl->cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
	wl_surface_commit(wl->cursor_surface);
}

void ska_wl_show_cursor(bool show) {
	ska_wl()->cursor_visible = show;
	ska_wl_apply_cursor();
	wl_display_flush(ska_wl()->display);
}

void ska_wl_set_cursor(ska_system_cursor_ cursor) {
	if (cursor >= ska_system_cursor_count_) return;
	ska_wl()->cursor = cursor;
	ska_wl_apply_cursor();
	wl_display_flush(ska_wl()->display);
}

// A locked pointer stops moving, so wl_pointer.motion goes quiet and this is the
// only source of movement. Unaccelerated, since acceleration skews mouse-look.
static void ska_wl_relative_motion(void* data, struct zwp_relative_pointer_v1* relative_pointer,
                                   uint32_t utime_hi, uint32_t utime_lo,
                                   wl_fixed_t dx, wl_fixed_t dy,
                                   wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel) {
	(void)data; (void)relative_pointer; (void)utime_hi; (void)utime_lo;
	(void)dx; (void)dy;

	ska_window_t* window = ska_wl()->pointer_focus;
	if (!window) return;

	ska_wl_state_t* wl = ska_wl();
	wl->relative_carry_x += wl_fixed_to_double(dx_unaccel);
	wl->relative_carry_y += wl_fixed_to_double(dy_unaccel);

	int32_t rel_x = (int32_t)wl->relative_carry_x;
	int32_t rel_y = (int32_t)wl->relative_carry_y;
	wl->relative_carry_x -= rel_x;
	wl->relative_carry_y -= rel_y;
	if (rel_x == 0 && rel_y == 0) return;

	ska_event_t event = {0};
	event.timestamp              = ska_time_get_elapsed_ms();
	event.type                   = ska_event_mouse_motion;
	event.mouse_motion.window_id = window->id;
	// The pointer is pinned, so the absolute position is whatever it was
	event.mouse_motion.x         = g_ska.input_state.mouse_x;
	event.mouse_motion.y         = g_ska.input_state.mouse_y;
	event.mouse_motion.xrel      = rel_x;
	event.mouse_motion.yrel      = rel_y;

	ska_input_add_relative(rel_x, rel_y);
	ska_post_event(&event);
}

static const struct zwp_relative_pointer_v1_listener k_relative_pointer_listener = {
	.relative_motion = ska_wl_relative_motion,
};

static void ska_wl_locked_pointer_locked  (void* d, struct zwp_locked_pointer_v1* p) { (void)d; (void)p; }
static void ska_wl_locked_pointer_unlocked(void* d, struct zwp_locked_pointer_v1* p) { (void)d; (void)p; }

static const struct zwp_locked_pointer_v1_listener k_locked_pointer_listener = {
	.locked   = ska_wl_locked_pointer_locked,
	.unlocked = ska_wl_locked_pointer_unlocked,
};

static void ska_wl_release_pointer_lock(void) {
	ska_wl_state_t* wl = ska_wl();
	wl->relative_carry_x = 0.0;
	wl->relative_carry_y = 0.0;
	if (wl->relative_pointer) {
		zwp_relative_pointer_v1_destroy(wl->relative_pointer);
		wl->relative_pointer = NULL;
	}
	if (wl->locked_pointer) {
		zwp_locked_pointer_v1_destroy(wl->locked_pointer);
		wl->locked_pointer = NULL;
	}
}

bool ska_wl_set_relative_mouse_mode(bool enabled) {
	ska_wl_state_t* wl = ska_wl();

	if (!enabled) {
		ska_wl_release_pointer_lock();
		// Back to the app's own choice, which entering relative mode overrode
		ska_wl_show_cursor(g_ska.input_state.cursor_visible);
		return true;
	}

	if (!wl->relative_pointer_manager || !wl->pointer_constraints || !wl->pointer) {
		ska_set_error("ska_mouse_set_relative_mode: compositor has no pointer-constraints support");
		return false;
	}

	// A lock names a surface but need not have the pointer over it yet, so any
	// window will do; the compositor activates the constraint once it is.
	ska_window_t* window = wl->pointer_focus ? wl->pointer_focus : wl->keyboard_focus;
	if (!window) {
		for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
			if (g_ska.windows[i] && g_ska.windows[i]->wl) {
				window = g_ska.windows[i];
				break;
			}
		}
	}
	if (!window || !window->wl) {
		ska_set_error("ska_mouse_set_relative_mode: no window to lock the pointer to");
		return false;
	}

	if (wl->locked_pointer) return true;

	// PERSISTENT so the lock re-arms itself if the compositor drops it, rather
	// than silently ending relative mode on the first alt-tab.
	wl->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(wl->pointer_constraints,
		window->wl->surface, wl->pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
	if (!wl->locked_pointer) {
		ska_set_error("ska_mouse_set_relative_mode: lock_pointer failed");
		return false;
	}
	zwp_locked_pointer_v1_add_listener(wl->locked_pointer, &k_locked_pointer_listener, NULL);

	wl->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(wl->relative_pointer_manager, wl->pointer);
	if (!wl->relative_pointer) {
		ska_wl_release_pointer_lock();
		ska_set_error("ska_mouse_set_relative_mode: get_relative_pointer failed");
		return false;
	}
	zwp_relative_pointer_v1_add_listener(wl->relative_pointer, &k_relative_pointer_listener, NULL);

	wl->relative_carry_x = 0.0;
	wl->relative_carry_y = 0.0;
	ska_wl_show_cursor(false);
	wl_display_flush(wl->display);
	return true;
}

// ============================================================================
// Event Pump
// ============================================================================

// The compositor sends one key event per physical press, so held-key repeats
// are generated here from the rate and delay it advertised.
static void ska_wl_pump_key_repeat(void) {
	ska_wl_state_t* wl = ska_wl();
	if (wl->repeat_key == 0 || wl->repeat_rate <= 0 || !wl->keyboard_focus) return;

	// Elapsed milliseconds wrap roughly every 49 days, so the comparison has to
	// be a signed difference; a plain < would stop repeating after a wrap.
	uint32_t now = ska_time_get_elapsed_ms();
	if ((int32_t)(now - wl->repeat_next_ms) < 0) return;

	uint32_t interval = (uint32_t)(1000 / wl->repeat_rate);
	if (interval == 0) interval = 1;

	// Catch up without a burst if the frame ran long
	uint32_t overdue = now - wl->repeat_next_ms;
	wl->repeat_next_ms = now + interval - (overdue % interval);

	ska_wl_emit_key(wl->keyboard_focus, wl->repeat_key, wl->repeat_scancode, true, true);
}

void ska_wl_pump_events(void) {
	struct wl_display* display = ska_wl()->display;

	// prepare_read fails while the queue holds events, so drain first, and each
	// success must be matched by exactly one read_events or cancel_read.
	while (wl_display_prepare_read(display) != 0) {
		// dispatch_pending stops draining once the display latches an error,
		// so without this the queue never empties and prepare_read never wins.
		if (wl_display_dispatch_pending(display) < 0) break;
	}
	wl_display_flush(display);

	struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
	if (poll(&pfd, 1, 0) > 0) {
		wl_display_read_events(display);
	} else {
		wl_display_cancel_read(display);
	}
	wl_display_dispatch_pending(display);

	// A dead connection never recovers, and without this the loop would spin
	// forever on a display that cannot deliver anything.
	if (!ska_wl()->connection_lost && wl_display_get_error(display) != 0) {
		ska_wl()->connection_lost = true;
		ska_log(ska_log_error, "Wayland: connection lost, the compositor is gone");

		ska_event_t event = {0};
		event.timestamp = ska_time_get_elapsed_ms();
		event.type      = ska_event_quit;
		ska_post_event(&event);
	}

	ska_wl_pump_key_repeat();
	ska_linux_check_file_dialog();
}

// ============================================================================
// Graphics Interop
// ============================================================================

typedef VkFlags VkWaylandSurfaceCreateFlagsKHR;
typedef struct VkWaylandSurfaceCreateInfoKHR {
	VkStructureType                sType;
	const void*                    pNext;
	VkWaylandSurfaceCreateFlagsKHR flags;
	struct wl_display*             display;
	struct wl_surface*             surface;
} VkWaylandSurfaceCreateInfoKHR;

typedef VkResult (VKAPI_PTR *PFN_vkCreateWaylandSurfaceKHR)(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR* pCreateInfo, const void* pAllocator, VkSurfaceKHR* pSurface);

const char** ska_wl_vk_get_instance_extensions(uint32_t* out_count) {
	static const char* extensions[] = { "VK_KHR_surface", "VK_KHR_wayland_surface" };
	if (out_count) *out_count = 2;
	return extensions;
}

// The WebGPU path builds its surface in ska_wgpu.c, so it says so from there.
void ska_wl_mark_presenting(const ska_window_t* window) {
	if (window && window->wl) window->wl->app_presents = true;
}

bool ska_wl_vk_create_surface(const ska_window_t* window, VkInstance instance, VkSurfaceKHR* out_surface) {
	if (!window->wl || !window->wl->surface) {
		ska_set_error("ska_vk_create_surface: window has no wl_surface");
		return false;
	}

	PFN_vkGetInstanceProcAddr get_proc_addr = ska_linux_vk_get_proc_addr();
	if (!get_proc_addr) return false;

	PFN_vkCreateWaylandSurfaceKHR create_surface =
		(PFN_vkCreateWaylandSurfaceKHR)get_proc_addr(instance, "vkCreateWaylandSurfaceKHR");
	if (!create_surface) {
		ska_set_error("ska_vk_create_surface: vkCreateWaylandSurfaceKHR not found, is VK_KHR_wayland_surface enabled?");
		return false;
	}

	VkWaylandSurfaceCreateInfoKHR info = {0};
	info.sType   = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
	info.display = ska_wl()->display;
	info.surface = window->wl->surface;

	if (create_surface(instance, &info, NULL, out_surface) != VK_SUCCESS) {
		ska_set_error("ska_vk_create_surface: vkCreateWaylandSurfaceKHR failed");
		return false;
	}

	// From here the swapchain attaches and commits, so sk_app must not
	window->wl->app_presents = true;
	return true;
}

// ============================================================================
// Clipboard
// ============================================================================

// Wayland offers the clipboard by MIME type, so anything a text-shaped source
// might advertise is accepted, preferring the explicitly UTF-8 spelling.
static const char* k_clipboard_mimes[] = {
	"text/plain;charset=utf-8",
	"text/plain",
	"UTF8_STRING",
	"STRING",
	"TEXT",
};
#define SKA_CLIPBOARD_MIME_COUNT (sizeof(k_clipboard_mimes) / sizeof(k_clipboard_mimes[0]))

// ========== Receiving ==========

// Mime announcements precede the event saying what the offer is for, so the best
// match is stored on the offer. k_clipboard_mimes is in preference order.
static void ska_wl_offer_offer(void* data, struct wl_data_offer* offer, const char* mime_type) {
	(void)data;
	for (size_t i = 0; i < SKA_CLIPBOARD_MIME_COUNT; i++) {
		if (strcmp(mime_type, k_clipboard_mimes[i]) != 0) continue;

		// The index lives in the proxy's user data, which is the same field the
		// listener's data argument writes. That is why this offer is listened to
		// with a NULL data: giving it one would silently overwrite the index.
		uintptr_t best = (uintptr_t)wl_data_offer_get_user_data(offer);
		if (best == 0 || i + 1 < best) {
			wl_data_offer_set_user_data(offer, (void*)(uintptr_t)(i + 1));
		}
		return;
	}
}

static void ska_wl_offer_source_actions(void* d, struct wl_data_offer* o, uint32_t a) { (void)d; (void)o; (void)a; }
static void ska_wl_offer_action        (void* d, struct wl_data_offer* o, uint32_t a) { (void)d; (void)o; (void)a; }

static const struct wl_data_offer_listener k_data_offer_listener = {
	.offer          = ska_wl_offer_offer,
	.source_actions = ska_wl_offer_source_actions,
	.action         = ska_wl_offer_action,
};

// A new offer arrives before the event that says what it is for, so it is only
// tracked here; wl_data_device.selection decides whether it is the clipboard.
static void ska_wl_device_data_offer(void* data, struct wl_data_device* device, struct wl_data_offer* offer) {
	(void)data; (void)device;
	ska_wl_state_t* wl = ska_wl();

	// An offer the compositor never follows up on, a drag and drop one for
	// instance, has no other owner to release it.
	if (wl->pending_offer && wl->pending_offer != wl->data_offer) {
		wl_data_offer_destroy(wl->pending_offer);
	}
	wl->pending_offer = offer;

	wl_data_offer_set_user_data(offer, NULL);
	wl_data_offer_add_listener(offer, &k_data_offer_listener, NULL);
}

static void ska_wl_device_selection(void* data, struct wl_data_device* device, struct wl_data_offer* offer) {
	(void)data; (void)device;
	ska_wl_state_t* wl = ska_wl();

	if (wl->data_offer && wl->data_offer != offer) {
		wl_data_offer_destroy(wl->data_offer);
	}
	if (wl->pending_offer == offer) wl->pending_offer = NULL;
	wl->data_offer = offer;
	wl->offer_mime = NULL;
	if (offer) {
		uintptr_t best = (uintptr_t)wl_data_offer_get_user_data(offer);
		if (best > 0) wl->offer_mime = k_clipboard_mimes[best - 1];
	}
}

static void ska_wl_device_enter (void* d, struct wl_data_device* dv, uint32_t s, struct wl_surface* su, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* o) { (void)d;(void)dv;(void)s;(void)su;(void)x;(void)y;(void)o; }
static void ska_wl_device_leave (void* d, struct wl_data_device* dv) { (void)d; (void)dv; }
static void ska_wl_device_motion(void* d, struct wl_data_device* dv, uint32_t t, wl_fixed_t x, wl_fixed_t y) { (void)d;(void)dv;(void)t;(void)x;(void)y; }
static void ska_wl_device_drop  (void* d, struct wl_data_device* dv) { (void)d; (void)dv; }

static const struct wl_data_device_listener k_data_device_listener = {
	.data_offer = ska_wl_device_data_offer,
	.enter      = ska_wl_device_enter,
	.leave      = ska_wl_device_leave,
	.motion     = ska_wl_device_motion,
	.drop       = ska_wl_device_drop,
	.selection  = ska_wl_device_selection,
};

// ========== Sending ==========

static void ska_wl_source_send(void* data, struct wl_data_source* source, const char* mime_type, int32_t fd) {
	(void)data; (void)source; (void)mime_type;

	// A consumer may read a prefix and close, raising SIGPIPE. Suppressed only
	// across the write, since the handler belongs to the host application.
	void (*previous)(int) = signal(SIGPIPE, SIG_IGN);

	const char* text = ska_wl()->clipboard_text;
	if (text) {
		size_t remaining = strlen(text);
		// A large paste can exceed the pipe buffer, so keep writing until the
		// reader has taken all of it.
		while (remaining > 0) {
			ssize_t written = write(fd, text, remaining);
			if (written <= 0) break;
			text      += written;
			remaining -= (size_t)written;
		}
	}
	close(fd);
	signal(SIGPIPE, previous);
}

static void ska_wl_source_cancelled(void* data, struct wl_data_source* source) {
	(void)data;
	// Another client took the selection, so sk_app is no longer the owner
	ska_wl_state_t* wl = ska_wl();
	if (wl->data_source == source) wl->data_source = NULL;
	wl_data_source_destroy(source);
}

static void ska_wl_source_target            (void* d, struct wl_data_source* s, const char* m) { (void)d;(void)s;(void)m; }
static void ska_wl_source_dnd_drop_performed(void* d, struct wl_data_source* s) { (void)d; (void)s; }
static void ska_wl_source_dnd_finished      (void* d, struct wl_data_source* s) { (void)d; (void)s; }
static void ska_wl_source_action            (void* d, struct wl_data_source* s, uint32_t a) { (void)d;(void)s;(void)a; }

static const struct wl_data_source_listener k_data_source_listener = {
	.target             = ska_wl_source_target,
	.send               = ska_wl_source_send,
	.cancelled          = ska_wl_source_cancelled,
	.dnd_drop_performed = ska_wl_source_dnd_drop_performed,
	.dnd_finished       = ska_wl_source_dnd_finished,
	.action             = ska_wl_source_action,
};

// Both the manager and the seat come from the registry, so the data device can
// only be made once the initial roundtrip has announced both.
static void ska_wl_init_data_device(void) {
	ska_wl_state_t* wl = ska_wl();
	if (!wl->data_device_manager || !wl->seat) return;
	wl->data_device = wl_data_device_manager_get_data_device(wl->data_device_manager, wl->seat);
	wl_data_device_add_listener(wl->data_device, &k_data_device_listener, NULL);
}

char* ska_wl_clipboard_get_text(void) {
	ska_wl_state_t* wl = ska_wl();

	// Reading back our own selection would deadlock, writing and reading one
	// pipe on one thread. The stored copy is the same bytes.
	if (wl->data_source && wl->clipboard_text) {
		return ska_strdup(wl->clipboard_text);
	}
	if (!wl->data_offer || !wl->offer_mime) return NULL;

	int fds[2];
	if (pipe(fds) != 0) return NULL;

	wl_data_offer_receive(wl->data_offer, wl->offer_mime, fds[1]);
	close(fds[1]);
	// The other client cannot answer until the request has actually gone out
	wl_display_flush(wl->display);

	size_t  capacity = 1024;
	size_t  length   = 0;
	char*   buffer   = ska_malloc(capacity);
	if (!buffer) {
		close(fds[0]);
		return NULL;
	}

	// Bounded, so a source that never answers cannot hang the caller
	uint32_t deadline = ska_time_get_elapsed_ms() + 500;
	for (;;) {
		struct pollfd pfd = { .fd = fds[0], .events = POLLIN };
		uint32_t now = ska_time_get_elapsed_ms();
		if (now >= deadline) break;

		int ready = poll(&pfd, 1, (int)(deadline - now));
		if (ready <= 0) break;

		if (length + 1 >= capacity) {
			capacity *= 2;
			char* grown = ska_realloc(buffer, capacity);
			if (!grown) break;
			buffer = grown;
		}
		ssize_t got = read(fds[0], buffer + length, capacity - length - 1);
		if (got <= 0) break; // 0 is the source closing its end
		length += (size_t)got;
	}
	close(fds[0]);

	if (length == 0) {
		ska_free(buffer);
		return NULL;
	}
	buffer[length] = '\0';
	return buffer;
}

bool ska_wl_clipboard_set_text(const char* text) {
	ska_wl_state_t* wl = ska_wl();
	if (!wl->data_device_manager || !wl->data_device) {
		ska_set_error("ska_clipboard_set_text: compositor has no data device");
		return false;
	}
	if (wl->last_input_serial == 0) {
		// Compositors reject a selection that is not backed by a real event,
		// which is what stops background clients stealing the clipboard.
		ska_set_error("ska_clipboard_set_text: no input serial yet, the window needs focus first");
		return false;
	}

	if (wl->clipboard_text) ska_free(wl->clipboard_text);
	wl->clipboard_text = ska_strdup(text);
	if (!wl->clipboard_text) return false;

	if (wl->data_source) wl_data_source_destroy(wl->data_source);
	wl->data_source = wl_data_device_manager_create_data_source(wl->data_device_manager);
	if (!wl->data_source) {
		ska_set_error("ska_clipboard_set_text: create_data_source failed");
		return false;
	}
	wl_data_source_add_listener(wl->data_source, &k_data_source_listener, NULL);
	for (size_t i = 0; i < SKA_CLIPBOARD_MIME_COUNT; i++) {
		wl_data_source_offer(wl->data_source, k_clipboard_mimes[i]);
	}

	wl_data_device_set_selection(wl->data_device, wl->data_source, wl->last_input_serial);
	wl_display_flush(wl->display);
	return true;
}

// ============================================================================
// Backend Dispatch Table
// ============================================================================

const ska_linux_vtable_t ska_wl_vtable = {
	.name                      = "wayland",

	.init                      = ska_wl_init,
	.shutdown                  = ska_wl_shutdown,

	.window_create             = ska_wl_window_create,
	.window_destroy            = ska_wl_window_destroy,
	.window_set_title          = ska_wl_window_set_title,
	.window_set_frame_position = ska_wl_window_set_frame_position,
	.window_set_frame_size     = ska_wl_window_set_frame_size,
	.window_show               = ska_wl_window_show,
	.window_hide               = ska_wl_window_hide,
	.window_maximize           = ska_wl_window_maximize,
	.window_minimize           = ska_wl_window_minimize,
	.window_restore            = ska_wl_window_restore,
	.window_set_fullscreen     = ska_wl_window_set_fullscreen,
	.window_raise              = ska_wl_window_raise,
	.window_get_drawable_size  = ska_wl_window_get_drawable_size,
	.get_frame_extents         = ska_wl_get_frame_extents,
	.get_dpi_scale             = ska_wl_get_dpi_scale,
	.get_refresh_rate          = ska_wl_get_refresh_rate,

	.show_cursor               = ska_wl_show_cursor,
	.set_cursor                = ska_wl_set_cursor,
	.set_relative_mouse_mode   = ska_wl_set_relative_mouse_mode,

	.pump_events               = ska_wl_pump_events,

	.vk_get_instance_extensions = ska_wl_vk_get_instance_extensions,
	.vk_create_surface          = ska_wl_vk_create_surface,

	.clipboard_get_text        = ska_wl_clipboard_get_text,
	.clipboard_set_text        = ska_wl_clipboard_set_text,
};

#endif // SKA_PLATFORM_LINUX && SKA_LINUX_WAYLAND
