//
// sk_app - Linux backend dispatch
//
// Linux is the one platform where the windowing system is a runtime choice.
// This file owns every ska_platform_* entry point that differs between X11 and
// Wayland, and forwards each through the vtable selected by ska_platform_init.
//
// Entry points that behave identically on both backends live in
// ska_linux_common.c and implement ska_platform_* directly.

#define _POSIX_C_SOURCE 200809L
#include "ska_internal.h"

#ifdef SKA_PLATFORM_LINUX

#include <stdlib.h>
#include <string.h>

// A backend compiled out has no vtable, so these are genuinely optional.
#ifdef SKA_LINUX_X11
	#define SKA_X11_VTABLE (&ska_x11_vtable)
#else
	#define SKA_X11_VTABLE ((const ska_linux_vtable_t*)NULL)
#endif

#ifdef SKA_LINUX_WAYLAND
	extern const ska_linux_vtable_t ska_wl_vtable;
	#define SKA_WL_VTABLE (&ska_wl_vtable)
#else
	#define SKA_WL_VTABLE ((const ska_linux_vtable_t*)NULL)
#endif

// ============================================================================
// Backend Selection
// ============================================================================

// SKA_VIDEODRIVER wins over the ska_settings_t preference so an unmodified
// binary can be pointed at either backend for testing.
static ska_linux_backend_ ska_linux_requested_backend(ska_linux_backend_ preference) {
	const char* env = getenv("SKA_VIDEODRIVER");
	if (!env || env[0] == '\0')      return preference;
	if (strcmp(env, "wayland") == 0) return ska_linux_backend_wayland;
	if (strcmp(env, "x11")     == 0) return ska_linux_backend_x11;

	ska_log(ska_log_warn, "SKA_VIDEODRIVER='%s' not recognized, expected 'wayland' or 'x11'; ignoring", env);
	return preference;
}

// Publishes the vtable before running init so the backend can use the normal
// ska_platform_* paths during startup, and rolls back if init fails.
static bool ska_linux_try_backend(const ska_linux_vtable_t* vtable, ska_linux_backend_ id) {
	if (!vtable) return false;

	g_ska.lnx     = vtable;
	g_ska.backend = id;
	if (vtable->init()) {
		ska_log(ska_log_info, "sk_app using the %s backend", vtable->name);
		return true;
	}

	g_ska.lnx     = NULL;
	g_ska.backend = ska_linux_backend_auto;
	return false;
}

bool ska_platform_init(void) {
	ska_linux_backend_ requested = ska_linux_requested_backend(g_ska.backend);

	// An explicit request is a requirement, not a hint. Substituting the other
	// backend would silently change the type of every native handle.
	if (requested == ska_linux_backend_wayland) {
		if (ska_linux_try_backend(SKA_WL_VTABLE, ska_linux_backend_wayland)) return true;
		ska_set_error("Wayland backend was requested but is not available");
		g_ska.backend = ska_linux_backend_auto;
		return false;
	}
	if (requested == ska_linux_backend_x11) {
		if (ska_linux_try_backend(SKA_X11_VTABLE, ska_linux_backend_x11)) return true;
		ska_set_error("X11 backend was requested but is not available");
		g_ska.backend = ska_linux_backend_auto;
		return false;
	}

	// The X11 backend defers opening its display, so it succeeds with no X
	// server running. That is what lets headless callers through ska_init.
	if (ska_linux_try_backend(SKA_WL_VTABLE, ska_linux_backend_wayland)) return true;
	if (ska_linux_try_backend(SKA_X11_VTABLE, ska_linux_backend_x11))    return true;

	// The settings preference is still sitting in g_ska.backend, and
	// ska_linux_get_backend documents auto for a failed init.
	ska_set_error("No usable Linux display backend, tried Wayland and X11");
	g_ska.backend = ska_linux_backend_auto;
	return false;
}

void ska_platform_shutdown(void) {
	g_ska.lnx->shutdown();
	g_ska.lnx     = NULL;
	g_ska.backend = ska_linux_backend_auto;
}

// ============================================================================
// Dispatch
// ============================================================================

bool ska_platform_window_create(ska_window_t* ref_window, const char* title, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t flags) {
	return g_ska.lnx->window_create(ref_window, title, x, y, w, h, flags);
}

void ska_platform_window_destroy(ska_window_t* ref_window) {
	g_ska.lnx->window_destroy(ref_window);
}

void ska_platform_window_set_title(ska_window_t* ref_window, const char* title) {
	g_ska.lnx->window_set_title(ref_window, title);
}

void ska_platform_window_set_frame_position(ska_window_t* ref_window, int32_t x, int32_t y) {
	g_ska.lnx->window_set_frame_position(ref_window, x, y);
}

void ska_platform_window_set_frame_size(ska_window_t* ref_window, int32_t w, int32_t h) {
	g_ska.lnx->window_set_frame_size(ref_window, w, h);
}

void ska_platform_window_show(ska_window_t* ref_window) {
	g_ska.lnx->window_show(ref_window);
}

void ska_platform_window_hide(ska_window_t* ref_window) {
	g_ska.lnx->window_hide(ref_window);
}

void ska_platform_window_maximize(ska_window_t* ref_window) {
	g_ska.lnx->window_maximize(ref_window);
}

void ska_platform_window_minimize(ska_window_t* ref_window) {
	g_ska.lnx->window_minimize(ref_window);
}

void ska_platform_window_restore(ska_window_t* ref_window) {
	g_ska.lnx->window_restore(ref_window);
}

void ska_platform_window_set_fullscreen(ska_window_t* ref_window, bool fullscreen) {
	g_ska.lnx->window_set_fullscreen(ref_window, fullscreen);
}

void ska_platform_window_raise(ska_window_t* ref_window) {
	g_ska.lnx->window_raise(ref_window);
}

void ska_platform_window_get_drawable_size(ska_window_t* ref_window, int32_t* opt_out_width, int32_t* opt_out_height) {
	g_ska.lnx->window_get_drawable_size(ref_window, opt_out_width, opt_out_height);
}

void ska_platform_get_frame_extents(const ska_window_t* window, int32_t* opt_out_left, int32_t* opt_out_right, int32_t* opt_out_top, int32_t* opt_out_bottom) {
	g_ska.lnx->get_frame_extents(window, opt_out_left, opt_out_right, opt_out_top, opt_out_bottom);
}

float ska_platform_get_dpi_scale(const ska_window_t* window) {
	return g_ska.lnx->get_dpi_scale(window);
}

float ska_platform_get_refresh_rate(const ska_window_t* window) {
	return g_ska.lnx->get_refresh_rate(window);
}

void ska_platform_show_cursor(bool show) {
	g_ska.lnx->show_cursor(show);
}

void ska_platform_set_cursor(ska_system_cursor_ cursor) {
	g_ska.lnx->set_cursor(cursor);
}

bool ska_platform_set_relative_mouse_mode(bool enabled) {
	return g_ska.lnx->set_relative_mouse_mode(enabled);
}

void ska_platform_pump_events(void) {
	g_ska.lnx->pump_events();
}

const char** ska_platform_vk_get_instance_extensions(uint32_t* out_count) {
	return g_ska.lnx->vk_get_instance_extensions(out_count);
}

bool ska_platform_vk_create_surface(const ska_window_t* window, VkInstance instance, VkSurfaceKHR* out_surface) {
	return g_ska.lnx->vk_create_surface(window, instance, out_surface);
}

char* ska_platform_clipboard_get_text(void) {
	return g_ska.lnx->clipboard_get_text();
}

bool ska_platform_clipboard_set_text(const char* text) {
	return g_ska.lnx->clipboard_set_text(text);
}

#endif // SKA_PLATFORM_LINUX
