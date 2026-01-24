//
// sk_app - Linux X11 platform backend

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "ska_internal.h"

#ifdef SKA_PLATFORM_LINUX

#include <X11/keysym.h>
#include <X11/Xresource.h>
#include <X11/extensions/Xfixes.h>
#include <locale.h>
#include <stdlib.h>
#include <sys/select.h>
#include <unistd.h>

// Scancode translation table (X11 keycodes to ska_scancode_)
static ska_scancode_ ska_x11_scancode_table[256];

// Map KeySym to scancode (layout-independent)
static ska_scancode_ ska_keysym_to_scancode(KeySym keysym) {
	// Letters (uppercase and lowercase)
	if (keysym >= XK_a && keysym <= XK_z) {
		return ska_scancode_a + (keysym - XK_a);
	}
	if (keysym >= XK_A && keysym <= XK_Z) {
		return ska_scancode_a + (keysym - XK_A);
	}

	// Numbers
	if (keysym >= XK_1 && keysym <= XK_9) {
		return ska_scancode_1 + (keysym - XK_1);
	}
	if (keysym == XK_0) return ska_scancode_0;

	// Function keys
	if (keysym >= XK_F1 && keysym <= XK_F12) {
		return ska_scancode_f1 + (keysym - XK_F1);
	}

	// Special keys
	switch (keysym) {
		case XK_Return: return ska_scancode_return;
		case XK_Escape: return ska_scancode_escape;
		case XK_BackSpace: return ska_scancode_backspace;
		case XK_Tab: return ska_scancode_tab;
		case XK_space: return ska_scancode_space;
		case XK_minus: return ska_scancode_minus;
		case XK_equal: return ska_scancode_equals;
		case XK_bracketleft: return ska_scancode_leftbracket;
		case XK_bracketright: return ska_scancode_rightbracket;
		case XK_backslash: return ska_scancode_backslash;
		case XK_semicolon: return ska_scancode_semicolon;
		case XK_apostrophe: return ska_scancode_apostrophe;
		case XK_grave: return ska_scancode_grave;
		case XK_comma: return ska_scancode_comma;
		case XK_period: return ska_scancode_period;
		case XK_slash: return ska_scancode_slash;
		case XK_Caps_Lock: return ska_scancode_capslock;

		// Navigation
		case XK_Print: return ska_scancode_printscreen;
		case XK_Scroll_Lock: return ska_scancode_scrolllock;
		case XK_Pause: return ska_scancode_pause;
		case XK_Insert: return ska_scancode_insert;
		case XK_Home: return ska_scancode_home;
		case XK_Page_Up: return ska_scancode_pageup;
		case XK_Delete: return ska_scancode_delete;
		case XK_End: return ska_scancode_end;
		case XK_Page_Down: return ska_scancode_pagedown;
		case XK_Right: return ska_scancode_right;
		case XK_Left: return ska_scancode_left;
		case XK_Down: return ska_scancode_down;
		case XK_Up: return ska_scancode_up;

		// Modifiers
		case XK_Control_L: return ska_scancode_lctrl;
		case XK_Shift_L: return ska_scancode_lshift;
		case XK_Alt_L: return ska_scancode_lalt;
		case XK_Super_L: return ska_scancode_lgui;
		case XK_Control_R: return ska_scancode_rctrl;
		case XK_Shift_R: return ska_scancode_rshift;
		case XK_Alt_R: return ska_scancode_ralt;
		case XK_Super_R: return ska_scancode_rgui;

		default: return ska_scancode_unknown;
	}
}

static void ska_init_scancode_table(void) {
	// Initialize all to unknown - the table is now populated dynamically
	// based on KeySyms during event processing for layout independence
	for (int32_t i = 0; i < 256; i++) {
		ska_x11_scancode_table[i] = ska_scancode_unknown;
	}
}

static ska_window_t* ska_find_window_by_xwindow(Window xwin) {
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (g_ska.windows[i] && g_ska.windows[i]->xwindow == xwin) {
			return g_ska.windows[i];
		}
	}
	return NULL;
}

// Forward declaration for file dialog check
static void ska_linux_check_file_dialog(void);

bool ska_platform_init(void) {
	// Set locale for X11
	setlocale(LC_ALL, "");
	XSetLocaleModifiers("");

	g_ska.x_display = XOpenDisplay(NULL);
	if (!g_ska.x_display) {
		ska_set_error("Failed to open X11 display");
		return false;
	}

	g_ska.x_screen = DefaultScreen(g_ska.x_display);
	g_ska.x_root = RootWindow(g_ska.x_display, g_ska.x_screen);

	// Initialize input method
	g_ska.xim = XOpenIM(g_ska.x_display, NULL, NULL, NULL);
	if (!g_ska.xim) {
		ska_log(ska_log_warn, "Failed to open X Input Method");
	}

	// Initialize Xrm database (required before using XrmGetResource for DPI queries)
	XrmInitialize();

	// Get WM atoms
	g_ska.wm_protocols                = XInternAtom(g_ska.x_display, "WM_PROTOCOLS", False);
	g_ska.wm_delete_window            = XInternAtom(g_ska.x_display, "WM_DELETE_WINDOW", False);
	g_ska.net_wm_state                = XInternAtom(g_ska.x_display, "_NET_WM_STATE", False);
	g_ska.net_wm_state_fullscreen     = XInternAtom(g_ska.x_display, "_NET_WM_STATE_FULLSCREEN", False);
	g_ska.net_wm_state_maximized_vert = XInternAtom(g_ska.x_display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
	g_ska.net_wm_state_maximized_horz = XInternAtom(g_ska.x_display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
	g_ska.resource_manager            = XInternAtom(g_ska.x_display, "RESOURCE_MANAGER", False);

	// Watch root window for property changes (for DPI change detection via xrdb)
	XSelectInput(g_ska.x_display, g_ska.x_root, PropertyChangeMask);

	// Cache initial DPI scale
	g_ska.cached_dpi_scale = 0.0f; // Will be set on first window creation

	// Initialize scancode table
	ska_init_scancode_table();

	// Check for XInput2
	int32_t xi_event, xi_error;
	if (!XQueryExtension(g_ska.x_display, "XInputExtension", &g_ska.xi_opcode, &xi_event, &xi_error)) {
		ska_log(ska_log_warn, "XInput extension not available");
	}

	// Detect XWayland (X11 running on Wayland compositor)
	// This affects mouse warp behavior workarounds
	g_ska.is_xwayland = getenv("WAYLAND_DISPLAY") != NULL;

	return true;
}

void ska_platform_shutdown(void) {
	if (g_ska.xim) {
		XCloseIM(g_ska.xim);
		g_ska.xim = NULL;
	}

	if (g_ska.x_display) {
		XCloseDisplay(g_ska.x_display);
		g_ska.x_display = NULL;
	}
}

bool ska_platform_window_create(
	ska_window_t* window,
	const char* title,
	int32_t x, int32_t y,
	int32_t w, int32_t h,
	uint32_t flags
) {
	// Set window attributes
	XSetWindowAttributes wa = {0};
	wa.event_mask = KeyPressMask | KeyReleaseMask |
					ButtonPressMask | ButtonReleaseMask |
					PointerMotionMask |
					EnterWindowMask | LeaveWindowMask |
					FocusChangeMask |
					StructureNotifyMask |
					ExposureMask;
	wa.colormap = XCreateColormap(g_ska.x_display, g_ska.x_root,
								   DefaultVisual(g_ska.x_display, g_ska.x_screen),
								   AllocNone);

	// Center window if requested
	if (x == -1 || y == -1) {
		Screen* screen = DefaultScreenOfDisplay(g_ska.x_display);
		x = (WidthOfScreen(screen) - w) / 2;
		y = (HeightOfScreen(screen) - h) / 2;
	}

	// Create window
	window->xwindow = XCreateWindow(
		g_ska.x_display,
		g_ska.x_root,
		x, y, w, h,
		0,
		CopyFromParent,
		InputOutput,
		CopyFromParent,
		CWEventMask | CWColormap,
		&wa
	);

	if (!window->xwindow) {
		ska_set_error("Failed to create X11 window");
		return false;
	}

	// Set window title
	XStoreName(g_ska.x_display, window->xwindow, title);
	XSetIconName(g_ska.x_display, window->xwindow, title);

	// Set WM_CLASS for desktop file matching (important for Wayland icon support)
	// The res_name should match StartupWMClass in the .desktop file
	XClassHint* class_hint = XAllocClassHint();
	if (class_hint) {
		class_hint->res_name = (char*)title;   // Instance name
		class_hint->res_class = (char*)title;  // Class name
		XSetClassHint(g_ska.x_display, window->xwindow, class_hint);
		XFree(class_hint);
	}

	// Set WM protocols
	XSetWMProtocols(g_ska.x_display, window->xwindow, &g_ska.wm_delete_window, 1);

	// Create input context
	if (g_ska.xim) {
		window->xic = XCreateIC(
			g_ska.xim,
			XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
			XNClientWindow, window->xwindow,
			XNFocusWindow, window->xwindow,
			NULL
		);
	}

	// Store title
	window->title = strdup(title);

	// Apply window flags
	if (flags & ska_window_borderless) {
		// Remove decorations using MWM hints
		struct {
			unsigned long flags;
			unsigned long functions;
			unsigned long decorations;
			long input_mode;
			unsigned long status;
		} hints = {0};

		hints.flags = 2; // MWM_HINTS_DECORATIONS
		hints.decorations = 0;

		Atom mwm_hints = XInternAtom(g_ska.x_display, "_MOTIF_WM_HINTS", False);
		XChangeProperty(g_ska.x_display, window->xwindow, mwm_hints, mwm_hints,
					   32, PropModeReplace, (unsigned char*)&hints, 5);
	}

	// Set size hints
	XSizeHints* size_hints = XAllocSizeHints();
	if (size_hints) {
		size_hints->flags = PPosition | PSize;
		if (!(flags & ska_window_resizable)) {
			size_hints->flags |= PMinSize | PMaxSize;
			size_hints->min_width = size_hints->max_width = w;
			size_hints->min_height = size_hints->max_height = h;
		}
		XSetWMNormalHints(g_ska.x_display, window->xwindow, size_hints);
		XFree(size_hints);
	}

	// Ensure it knows its process id
	Atom  net_wm_pid = XInternAtom(g_ska.x_display, "_NET_WM_PID", False);
	pid_t pid        = getpid();
	XChangeProperty(g_ska.x_display, window->xwindow, net_wm_pid, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&pid, 1);

	window->x = x;
	window->y = y;
	window->width = w;
	window->height = h;
	window->drawable_width = w;
	window->drawable_height = h;
	window->dpi_scale = ska_platform_get_dpi_scale(window);

	// Cache DPI scale for change detection (first window sets it)
	if (g_ska.cached_dpi_scale == 0.0f) {
		g_ska.cached_dpi_scale = window->dpi_scale;
	}

	return true;
}

void ska_platform_window_destroy(ska_window_t* window) {
	if (window->xic) {
		XDestroyIC(window->xic);
	}

	if (window->xwindow) {
		XDestroyWindow(g_ska.x_display, window->xwindow);
		XFlush(g_ska.x_display);
	}
}

void ska_platform_window_set_title(ska_window_t* window, const char* title) {
	if (window->title) {
		free(window->title);
	}
	window->title = strdup(title);
	XStoreName(g_ska.x_display, window->xwindow, title);
	XSetIconName(g_ska.x_display, window->xwindow, title);
	XFlush(g_ska.x_display);
}

void ska_platform_get_frame_extents(const ska_window_t* window, int32_t* out_left, int32_t* out_right, int32_t* out_top, int32_t* out_bottom) {
	int32_t left = 0, right = 0, top = 0, bottom = 0;

	if (window && window->xwindow) {
		Atom net_frame_extents = XInternAtom(g_ska.x_display, "_NET_FRAME_EXTENTS", False);
		Atom actual_type;
		int32_t actual_format;
		unsigned long nitems, bytes_after;
		unsigned char* data = NULL;

		if (XGetWindowProperty(g_ska.x_display, window->xwindow, net_frame_extents,
		                       0, 4, False, XA_CARDINAL,
		                       &actual_type, &actual_format, &nitems, &bytes_after, &data) == Success) {
			if (data && nitems == 4) {
				long* extents = (long*)data;
				left   = (int32_t)extents[0];
				right  = (int32_t)extents[1];
				top    = (int32_t)extents[2];
				bottom = (int32_t)extents[3];
			}
			if (data) XFree(data);
		}
	}

	if (out_left)   *out_left   = left;
	if (out_right)  *out_right  = right;
	if (out_top)    *out_top    = top;
	if (out_bottom) *out_bottom = bottom;
}

void ska_platform_window_set_frame_position(ska_window_t* window, int32_t x, int32_t y) {
	XMoveWindow(g_ska.x_display, window->xwindow, x, y);
	XFlush(g_ska.x_display);

	// Update cached content position
	int32_t left, top;
	ska_platform_get_frame_extents(window, &left, NULL, &top, NULL);
	window->x = x + left;
	window->y = y + top;
}

void ska_platform_window_set_frame_size(ska_window_t* window, int32_t w, int32_t h) {
	XResizeWindow(g_ska.x_display, window->xwindow, w, h);
	XFlush(g_ska.x_display);
}

void ska_platform_window_show(ska_window_t* window) {
	XMapWindow(g_ska.x_display, window->xwindow);
	XFlush(g_ska.x_display);
	window->is_visible = true;
}

void ska_platform_window_hide(ska_window_t* window) {
	XUnmapWindow(g_ska.x_display, window->xwindow);
	XFlush(g_ska.x_display);
	window->is_visible = false;
}

void ska_platform_window_maximize(ska_window_t* window) {
	XEvent event = {0};
	event.type = ClientMessage;
	event.xclient.window = window->xwindow;
	event.xclient.message_type = g_ska.net_wm_state;
	event.xclient.format = 32;
	event.xclient.data.l[0] = 1; // _NET_WM_STATE_ADD
	event.xclient.data.l[1] = g_ska.net_wm_state_maximized_vert;
	event.xclient.data.l[2] = g_ska.net_wm_state_maximized_horz;

	XSendEvent(g_ska.x_display, g_ska.x_root, False,
			   SubstructureNotifyMask | SubstructureRedirectMask, &event);
	XFlush(g_ska.x_display);
}

void ska_platform_window_minimize(ska_window_t* window) {
	XIconifyWindow(g_ska.x_display, window->xwindow, g_ska.x_screen);
	XFlush(g_ska.x_display);
}

void ska_platform_window_restore(ska_window_t* window) {
	// First, remove maximize state if window is maximized
	XEvent event = {0};
	event.type = ClientMessage;
	event.xclient.window = window->xwindow;
	event.xclient.message_type = g_ska.net_wm_state;
	event.xclient.format = 32;
	event.xclient.data.l[0] = 0; // _NET_WM_STATE_REMOVE
	event.xclient.data.l[1] = g_ska.net_wm_state_maximized_vert;
	event.xclient.data.l[2] = g_ska.net_wm_state_maximized_horz;

	XSendEvent(g_ska.x_display, g_ska.x_root, False,
			   SubstructureNotifyMask | SubstructureRedirectMask, &event);

	// Then ensure window is mapped (in case it was minimized)
	XMapWindow(g_ska.x_display, window->xwindow);
	XFlush(g_ska.x_display);
}

void ska_platform_window_raise(ska_window_t* window) {
	XRaiseWindow(g_ska.x_display, window->xwindow);
	// Only set focus if window is visible and actually mapped
	if (window->is_visible) {
		// Sync and verify the window is actually mapped before setting focus
		XSync(g_ska.x_display, False);

		XWindowAttributes attrs;
		if (XGetWindowAttributes(g_ska.x_display, window->xwindow, &attrs) &&
		    attrs.map_state == IsViewable) {
			XSetInputFocus(g_ska.x_display, window->xwindow, RevertToPointerRoot, CurrentTime);
		}
	}
	XFlush(g_ska.x_display);
}

void ska_platform_window_get_drawable_size(ska_window_t* window, int32_t* opt_out_width, int32_t* opt_out_height) {
	// For X11, drawable size equals window size unless using high-DPI
	window->drawable_width = window->width;
	window->drawable_height = window->height;
	(void)opt_out_width;
	(void)opt_out_height;
}

float ska_platform_get_dpi_scale(const ska_window_t* window) {
	(void)window;

	// Query Xft.dpi from Xresources (this is how GNOME/KDE/etc communicate scaling)
	char* resource_string = XResourceManagerString(g_ska.x_display);
	if (resource_string) {
		XrmDatabase db = XrmGetStringDatabase(resource_string);
		if (db) {
			XrmValue value;
			char* type = NULL;
			if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value)) {
				if (type && strcmp(type, "String") == 0 && value.addr) {
					float dpi = (float)atof(value.addr);
					XrmDestroyDatabase(db);
					if (dpi > 0) {
						return dpi / 96.0f; // 96 DPI is the baseline (100% scale)
					}
				}
			}
			XrmDestroyDatabase(db);
		}
	}

	// Fallback: try to get DPI from screen dimensions
	// This is less reliable but better than nothing
	int32_t screen_width_px = DisplayWidth(g_ska.x_display, g_ska.x_screen);
	int32_t screen_width_mm = DisplayWidthMM(g_ska.x_display, g_ska.x_screen);
	if (screen_width_mm > 0) {
		float dpi = (float)screen_width_px / ((float)screen_width_mm / 25.4f);
		// Only use this if it's significantly different from 96
		// (some systems report incorrect physical dimensions)
		if (dpi >= 120.0f) {
			return dpi / 96.0f;
		}
	}

	return 1.0f;
}

float ska_platform_get_refresh_rate(const ska_window_t* window) {
	(void)window;

	// Use XRandR to get the current screen refresh rate
	XRRScreenConfiguration* config = XRRGetScreenInfo(g_ska.x_display, g_ska.x_root);
	if (!config) {
		return 0.0f;
	}

	short rate = XRRConfigCurrentRate(config);
	XRRFreeScreenConfigInfo(config);

	return (float)rate;
}

void ska_platform_warp_mouse(ska_window_t* ref_window, int32_t x, int32_t y) {

	// XWayland workaround: XWarpPointer doesn't work correctly on XWayland
	// without hiding/showing the cursor. This fix is from Blender:
	// https://developer.blender.org/T53004#467383
	// https://github.com/blender/blender/blob/a8f7d41d3898/intern/ghost/intern/GHOST_SystemX11.cpp#L1680
	if (g_ska.is_xwayland) {
		XFixesHideCursor(g_ska.x_display, ref_window->xwindow);
	}

	XWarpPointer(g_ska.x_display, None, ref_window->xwindow, 0, 0, 0, 0, x, y);

	if (g_ska.is_xwayland) {
		XFixesShowCursor(g_ska.x_display, ref_window->xwindow);
	}

	XFlush(g_ska.x_display);
}

// Cursor cache and state (shared between ska_platform_set_cursor and ska_platform_show_cursor)
static Cursor g_x_cursors[ska_system_cursor_count_] = {0};
static ska_system_cursor_ g_current_cursor = ska_system_cursor_arrow;

void ska_platform_set_cursor(ska_system_cursor_ cursor) {
	// Freedesktop cursor specification names
	const char* xcursor_names[] = {
		[ska_system_cursor_arrow]      = "default",
		[ska_system_cursor_ibeam]      = "text",
		[ska_system_cursor_wait]       = "wait",
		[ska_system_cursor_crosshair]  = "crosshair",
		[ska_system_cursor_waitarrow]  = "progress",
		[ska_system_cursor_sizenwse]   = "nwse-resize",
		[ska_system_cursor_sizenesw]   = "nesw-resize",
		[ska_system_cursor_sizewe]     = "ew-resize",
		[ska_system_cursor_sizens]     = "ns-resize",
		[ska_system_cursor_sizeall]    = "all-scroll",
		[ska_system_cursor_no]         = "not-allowed",
		[ska_system_cursor_hand]       = "pointer",
	};

	// X11 cursor font fallbacks
	const uint32_t x11_cursors[] = {
		[ska_system_cursor_arrow]      = XC_left_ptr,
		[ska_system_cursor_ibeam]      = XC_xterm,
		[ska_system_cursor_wait]       = XC_watch,
		[ska_system_cursor_crosshair]  = XC_crosshair,
		[ska_system_cursor_waitarrow]  = XC_watch,
		[ska_system_cursor_sizenwse]   = XC_top_left_corner,
		[ska_system_cursor_sizenesw]   = XC_top_right_corner,
		[ska_system_cursor_sizewe]     = XC_sb_h_double_arrow,
		[ska_system_cursor_sizens]     = XC_sb_v_double_arrow,
		[ska_system_cursor_sizeall]    = XC_fleur,
		[ska_system_cursor_no]         = XC_X_cursor,
		[ska_system_cursor_hand]       = XC_hand2,
	};

	if (cursor >= ska_system_cursor_count_) {
		return;
	}

	if (g_x_cursors[cursor] == None) {
		// Try themed cursor first
		g_x_cursors[cursor] = XcursorLibraryLoadCursor(g_ska.x_display, xcursor_names[cursor]);

		// Fall back to X11 cursor font
		if (g_x_cursors[cursor] == None) {
			g_x_cursors[cursor] = XCreateFontCursor(g_ska.x_display, x11_cursors[cursor]);
		}
	}

	// Remember current cursor for ska_platform_show_cursor
	g_current_cursor = cursor;

	// Apply to all windows
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (g_ska.windows[i]) {
			XDefineCursor(g_ska.x_display, g_ska.windows[i]->xwindow, g_x_cursors[cursor]);
		}
	}
	XFlush(g_ska.x_display);
}

void ska_platform_show_cursor(bool show) {
	if (show) {
		// Restore the current cursor (don't use XUndefineCursor which resets to parent's cursor)
		for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
			if (g_ska.windows[i]) {
				XDefineCursor(g_ska.x_display, g_ska.windows[i]->xwindow, g_x_cursors[g_current_cursor]);
			}
		}
	} else {
		// Create invisible cursor
		static Cursor invisible_cursor = None;
		if (invisible_cursor == None) {
			char data[1] = {0};
			Pixmap blank = XCreateBitmapFromData(g_ska.x_display, g_ska.x_root, data, 1, 1);
			XColor color = {0};
			invisible_cursor = XCreatePixmapCursor(g_ska.x_display, blank, blank, &color, &color, 0, 0);
			XFreePixmap(g_ska.x_display, blank);
		}

		for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
			if (g_ska.windows[i]) {
				XDefineCursor(g_ska.x_display, g_ska.windows[i]->xwindow, invisible_cursor);
			}
		}
	}
	XFlush(g_ska.x_display);
}

bool ska_platform_set_relative_mouse_mode(bool enabled) {
	// Just hide cursor and center it in relative mode
	ska_platform_show_cursor(!enabled);
	return true;
}

void ska_platform_pump_events(void) {
	while (XPending(g_ska.x_display)) {
		XEvent xev;
		XNextEvent(g_ska.x_display, &xev);

		// Filter through input method first
		if (XFilterEvent(&xev, None)) {
			continue;
		}

		// Handle root window events (DPI change detection)
		if (xev.xany.window == g_ska.x_root) {
			if (xev.type == PropertyNotify && xev.xproperty.atom == g_ska.resource_manager) {
				// RESOURCE_MANAGER changed - check if DPI scale changed
				float new_scale = ska_platform_get_dpi_scale(NULL);
				if (new_scale != g_ska.cached_dpi_scale && g_ska.cached_dpi_scale > 0.0f) {
					g_ska.cached_dpi_scale = new_scale;

					// Send DPI changed event to all windows
					for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
						ska_window_t* win = g_ska.windows[i];
						if (win) {
							win->dpi_scale = new_scale;

							ska_event_t event = {0};
							event.timestamp          = (uint32_t)ska_time_get_elapsed_ms();
							event.type               = ska_event_window_dpi_changed;
							event.window.window_id   = win->id;
							event.window.data1       = (int32_t)(new_scale * 100.0f + 0.5f);
							ska_post_event(&event);
						}
					}
				}
			}
			continue;
		}

		ska_window_t* window = ska_find_window_by_xwindow(xev.xany.window);
		if (!window) {
			continue;
		}

		ska_event_t event = {0};
		event.timestamp = (uint32_t)ska_time_get_elapsed_ms();

		switch (xev.type) {
		case KeyPress:
		case KeyRelease: {
			event.type = (xev.type == KeyPress) ? ska_event_key_down : ska_event_key_up;
			event.keyboard.window_id = window->id;
			event.keyboard.pressed = (xev.type == KeyPress);
			event.keyboard.repeat = false; // X11 sends release+press for repeats

			// Convert keycode to KeySym for layout-independent mapping
			KeySym keysym = XLookupKeysym(&xev.xkey, 0);
			event.keyboard.scancode = ska_keysym_to_scancode(keysym);

			// Cache the scancode in the table for faster lookups
			if (event.keyboard.scancode != ska_scancode_unknown) {
				ska_x11_scancode_table[xev.xkey.keycode] = event.keyboard.scancode;
			}

			// Update keyboard state FIRST (before deriving modifiers)
			if (event.keyboard.scancode != ska_scancode_unknown) {
				g_ska.input_state.keyboard[event.keyboard.scancode] = event.keyboard.pressed ? 1 : 0;
			}

			// Derive modifier state from tracked keyboard state (post-event).
			// This matches Win32's GetKeyState() behavior and avoids X11's quirk
			// where xkey.state contains pre-event modifier state.
			uint16_t mods = 0;
			if (g_ska.input_state.keyboard[ska_scancode_lshift] || g_ska.input_state.keyboard[ska_scancode_rshift]) mods |= ska_keymod_shift;
			if (g_ska.input_state.keyboard[ska_scancode_lctrl]  || g_ska.input_state.keyboard[ska_scancode_rctrl])  mods |= ska_keymod_ctrl;
			if (g_ska.input_state.keyboard[ska_scancode_lalt]   || g_ska.input_state.keyboard[ska_scancode_ralt])   mods |= ska_keymod_alt;
			if (g_ska.input_state.keyboard[ska_scancode_lgui]   || g_ska.input_state.keyboard[ska_scancode_rgui])   mods |= ska_keymod_gui;
			event.keyboard.modifiers = mods;
			g_ska.input_state.key_modifiers = mods;

			ska_post_event(&event);

			// Handle text input
			if (xev.type == KeyPress && window->xic) {
				char buffer[32];
				KeySym keysym_text;
				Status status;
				int32_t len = Xutf8LookupString(window->xic, &xev.xkey, buffer, sizeof(buffer) - 1, &keysym_text, &status);
				if (len > 0 && (status == XLookupChars || status == XLookupBoth)) {
					buffer[len] = '\0';
					event.type = ska_event_text_input;
					event.text.window_id = window->id;
					strncpy(event.text.text, buffer, sizeof(event.text.text) - 1);
					ska_post_event(&event);
				}
			}
			break;
		}			case ButtonPress:
			case ButtonRelease: {
				if (xev.xbutton.button >= Button4 && xev.xbutton.button <= 7) {
					// Mouse wheel (vertical: Button4/Button5, horizontal: Button6/Button7)
					if (xev.type == ButtonPress) {
						event.type = ska_event_mouse_wheel;
						event.mouse_wheel.window_id = window->id;

						if (xev.xbutton.button == Button4 || xev.xbutton.button == Button5) {
							// Vertical scroll
							event.mouse_wheel.x = 0;
							event.mouse_wheel.y = (xev.xbutton.button == Button4) ? 1 : -1;
							event.mouse_wheel.precise_x = 0.0f;
							event.mouse_wheel.precise_y = (float)event.mouse_wheel.y;
						} else {
							// Horizontal scroll (Button6 = left, Button7 = right)
							event.mouse_wheel.x = (xev.xbutton.button == 6) ? -1 : 1;
							event.mouse_wheel.y = 0;
							event.mouse_wheel.precise_x = (float)event.mouse_wheel.x;
							event.mouse_wheel.precise_y = 0.0f;
						}
						ska_post_event(&event);
					}
				} else {
					// Mouse button
					event.type = (xev.type == ButtonPress) ? ska_event_mouse_button_down : ska_event_mouse_button_up;
					event.mouse_button.window_id = window->id;

					// Map X11 button numbers to ska_mouse_button_ values
					// X11: 1-3 = left/middle/right, 8-9 = back/forward (side buttons)
					// ska: 1-3 = left/middle/right, 4-5 = x1/x2 (side buttons)
					ska_mouse_button_ button;
					switch (xev.xbutton.button) {
						case Button1: button = ska_mouse_button_left;   break;
						case Button2: button = ska_mouse_button_middle; break;
						case Button3: button = ska_mouse_button_right;  break;
						case 8:       button = ska_mouse_button_x1;     break; // Back
						case 9:       button = ska_mouse_button_x2;     break; // Forward
						default:      button = xev.xbutton.button;      break;
					}
					event.mouse_button.button = button;
					event.mouse_button.pressed = (xev.type == ButtonPress);
					event.mouse_button.clicks = 1;
					event.mouse_button.x = xev.xbutton.x;
					event.mouse_button.y = xev.xbutton.y;

					// Update button state
					uint32_t button_mask = (1 << (button - 1));
					if (event.mouse_button.pressed) {
						g_ska.input_state.mouse_buttons |= button_mask;
					} else {
						g_ska.input_state.mouse_buttons &= ~button_mask;
					}

					ska_post_event(&event);
				}
				break;
			}

			case MotionNotify: {
				event.type = ska_event_mouse_motion;
				event.mouse_motion.window_id = window->id;
				event.mouse_motion.x = xev.xmotion.x;
				event.mouse_motion.y = xev.xmotion.y;
				event.mouse_motion.xrel = xev.xmotion.x - g_ska.input_state.mouse_x;
				event.mouse_motion.yrel = xev.xmotion.y - g_ska.input_state.mouse_y;

				g_ska.input_state.mouse_x = xev.xmotion.x;
				g_ska.input_state.mouse_y = xev.xmotion.y;
				g_ska.input_state.mouse_xrel = event.mouse_motion.xrel;
				g_ska.input_state.mouse_yrel = event.mouse_motion.yrel;

				ska_post_event(&event);
				break;
			}

			case EnterNotify:
				event.type = ska_event_window_mouse_enter;
				event.window.window_id = window->id;
				window->mouse_inside = true;
				ska_post_event(&event);
				break;

			case LeaveNotify:
				event.type = ska_event_window_mouse_leave;
				event.window.window_id = window->id;
				window->mouse_inside = false;
				ska_post_event(&event);
				break;

			case FocusIn:
				event.type = ska_event_window_focus_gained;
				event.window.window_id = window->id;
				window->has_focus = true;
				if (window->xic) {
					XSetICFocus(window->xic);
				}
				ska_post_event(&event);
				break;

			case FocusOut:
				event.type = ska_event_window_focus_lost;
				event.window.window_id = window->id;
				window->has_focus = false;
				if (window->xic) {
					XUnsetICFocus(window->xic);
				}
				ska_post_event(&event);
				break;

			case ConfigureNotify:
				if (xev.xconfigure.width != window->width || xev.xconfigure.height != window->height) {
					event.type = ska_event_window_resized;
					event.window.window_id = window->id;
					event.window.data1 = xev.xconfigure.width;
					event.window.data2 = xev.xconfigure.height;
					window->width = xev.xconfigure.width;
					window->height = xev.xconfigure.height;
					window->drawable_width = xev.xconfigure.width;
					window->drawable_height = xev.xconfigure.height;
					ska_post_event(&event);
				}
				{
					// ConfigureNotify gives position relative to parent (WM frame).
					// Translate to root coordinates for actual screen position.
					Window child;
					int32_t root_x, root_y;
					XTranslateCoordinates(g_ska.x_display, window->xwindow, g_ska.x_root, 0, 0, &root_x, &root_y, &child);

					if (root_x != window->x || root_y != window->y) {
						event.type = ska_event_window_moved;
						event.window.window_id = window->id;
						event.window.data1 = root_x;
						event.window.data2 = root_y;
						window->x = root_x;
						window->y = root_y;
						ska_post_event(&event);
					}
				}
				break;

			case MapNotify:
				if (!window->is_visible) {
					event.type = ska_event_window_shown;
					event.window.window_id = window->id;
					window->is_visible = true;
					ska_post_event(&event);
				}
				break;

			case UnmapNotify:
				if (window->is_visible) {
					event.type = ska_event_window_hidden;
					event.window.window_id = window->id;
					window->is_visible = false;
					ska_post_event(&event);
				}
				break;

			case ClientMessage:
				if (xev.xclient.message_type == g_ska.wm_protocols &&
					(Atom)xev.xclient.data.l[0] == g_ska.wm_delete_window) {
					event.type = ska_event_window_close;
					event.window.window_id = window->id;
					window->should_close = true;
					ska_post_event(&event);
				}
				break;

			case SelectionRequest: {
				// Handle clipboard data requests from other applications
				XSelectionRequestEvent* req = &xev.xselectionrequest;

				// If property is None, use the target as the property (some apps do this)
				Atom property = req->property;
				if (property == None) {
					property = req->target;
				}

				XEvent response;
				memset(&response, 0, sizeof(response));
				response.xselection.type = SelectionNotify;
				response.xselection.requestor = req->requestor;
				response.xselection.selection = req->selection;
				response.xselection.target = req->target;
				response.xselection.time = req->time;
				response.xselection.property = None;

				Atom clipboard_atom = XInternAtom(g_ska.x_display, "CLIPBOARD", False);
				Atom utf8_atom = XInternAtom(g_ska.x_display, "UTF8_STRING", False);
				Atom text_atom = XInternAtom(g_ska.x_display, "TEXT", False);
				Atom string_atom = XA_STRING;
				Atom targets_atom = XInternAtom(g_ska.x_display, "TARGETS", False);
				Atom text_plain_atom = XInternAtom(g_ska.x_display, "text/plain", False);
				Atom text_plain_utf8_atom = XInternAtom(g_ska.x_display, "text/plain;charset=utf-8", False);
				Atom property_atom = XInternAtom(g_ska.x_display, "SKA_CLIPBOARD_DATA", False);

				if (req->selection == clipboard_atom) {
					// Handle TARGETS request - tell requestor what formats we support
					if (req->target == targets_atom) {
						Atom supported_targets[] = {
							targets_atom,
							utf8_atom,
							text_atom,
							string_atom,
							text_plain_atom,
							text_plain_utf8_atom
						};
						XChangeProperty(
							g_ska.x_display, req->requestor, property,
							XA_ATOM, 32, PropModeReplace,
							(unsigned char*)supported_targets, 6
						);
						response.xselection.property = property;
					}
					// Handle UTF8_STRING, TEXT, STRING, or MIME type requests
					else if (req->target == utf8_atom || req->target == text_atom || req->target == string_atom ||
					         req->target == text_plain_atom || req->target == text_plain_utf8_atom) {
						// Check if we have clipboard data stored
						Atom actual_type;
						int32_t actual_format;
						unsigned long nitems, bytes_after;
						unsigned char* data = NULL;

						int32_t result = XGetWindowProperty(
							g_ska.x_display, window->xwindow, property_atom,
							0, 0x1FFFFFFF, False, AnyPropertyType,
							&actual_type, &actual_format, &nitems, &bytes_after, &data
						);

						if (result == Success && data && nitems > 0) {
							// We have data - send it to the requestor
							XChangeProperty(
								g_ska.x_display, req->requestor, property,
								req->target, 8, PropModeReplace,
								data, nitems
							);
							response.xselection.property = property;
							XFree(data);
						}
					}
				}

				XSendEvent(g_ska.x_display, req->requestor, False, 0, &response);
				XFlush(g_ska.x_display);
				break;
			}
		}
	}

	// Check for file dialog completion
	ska_linux_check_file_dialog();
}

/////////////////////////////////////////
// X11 specific subset of Vulkan header
/////////////////////////////////////////

typedef VkFlags VkXlibSurfaceCreateFlagsKHR;
typedef struct VkXlibSurfaceCreateInfoKHR {
	VkStructureType                sType;
	const void*                    pNext;
	VkXlibSurfaceCreateFlagsKHR    flags;
	Display*                       dpy;
	Window                         window;
} VkXlibSurfaceCreateInfoKHR;

typedef VkResult (VKAPI_PTR *PFN_vkCreateXlibSurfaceKHR)(VkInstance instance, const VkXlibSurfaceCreateInfoKHR* pCreateInfo, const /*VkAllocationCallbacks*/ void* pAllocator, VkSurfaceKHR* pSurface);

/////////////////////////////////////////

const char** ska_platform_vk_get_instance_extensions(uint32_t* out_count) {
	static const char* extensions[] = {
		"VK_KHR_surface",
		"VK_KHR_xlib_surface"
	};
	*out_count = 2;
	return extensions;
}

bool ska_platform_vk_create_surface(const ska_window_t* window, VkInstance instance, VkSurfaceKHR* out_surface) {
	void* module = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!module) module = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
	if (!module) {
		ska_set_error("Failed to load Vulkan .so");
		return false;
	}

	union {
		void* obj;
		PFN_vkGetInstanceProcAddr func;
	} vkGetInstanceProcAddr_ptr;

	vkGetInstanceProcAddr_ptr.obj = dlsym(module, "vkGetInstanceProcAddr");
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = vkGetInstanceProcAddr_ptr.func;
	if (!vkGetInstanceProcAddr) {
		ska_set_error("Failed to load vkGetInstanceProcAddr");
		return false;
	}

	PFN_vkCreateXlibSurfaceKHR vkCreateXlibSurfaceKHR = (PFN_vkCreateXlibSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateXlibSurfaceKHR");
	if (!vkCreateXlibSurfaceKHR) {
		ska_set_error("Failed to load vkCreateXlibSurfaceKHR");
		return false;
	}

	VkXlibSurfaceCreateInfoKHR create_info = {0};
	create_info.sType  = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
	create_info.dpy    = g_ska.x_display;
	create_info.window = window->xwindow;

	VkResult result = vkCreateXlibSurfaceKHR(instance, &create_info, NULL, out_surface);
	if (result != VK_SUCCESS) {
		ska_set_error("Failed to create Vulkan Xlib surface: %d", result);
		return false;
	}

	return true;
}

// ========== Text Input Platform Functions ==========

void ska_platform_show_virtual_keyboard(bool visible, ska_text_input_type_ type) {
	// Linux X11 - desktop platform, no virtual keyboard
	(void)visible;
	(void)type;
}

// ========== Clipboard Platform Functions ==========

char* ska_platform_clipboard_get_text(void) {
	if (!g_ska.x_display) {
		return NULL;
	}

	Atom clipboard_atom = XInternAtom(g_ska.x_display, "CLIPBOARD",   False);
	Atom utf8_atom      = XInternAtom(g_ska.x_display, "UTF8_STRING", False);
	Atom property_atom  = XInternAtom(g_ska.x_display, "XSEL_DATA",   False);

	// Find a window to use for selection requests (use first available window)
	Window window = None;
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (g_ska.windows[i]) {
			window = g_ska.windows[i]->xwindow;
			break;
		}
	}
	if (window == None) return NULL;

	// Check if we own the clipboard - if so, read directly from our stored data
	// to avoid a deadlock where we'd be waiting for ourselves to respond
	Window owner = XGetSelectionOwner(g_ska.x_display, clipboard_atom);
	if (owner == window) {
		Atom data_property = XInternAtom(g_ska.x_display, "SKA_CLIPBOARD_DATA", False);
		Atom actual_type;
		int32_t actual_format;
		unsigned long nitems, bytes_after;
		unsigned char* data = NULL;

		int32_t result = XGetWindowProperty(
			g_ska.x_display, window, data_property,
			0, 0x1FFFFFFF, False, AnyPropertyType,
			&actual_type, &actual_format, &nitems, &bytes_after, &data
		);

		if (result != Success || !data || nitems == 0) {
			if (data) XFree(data);
			return NULL;
		}

		char* text = (char*)malloc(nitems + 1);
		if (text) {
			memcpy(text, data, nitems);
			text[nitems] = '\0';
		}

		XFree(data);
		return text;
	}

	// Request clipboard content from external owner
	XConvertSelection(g_ska.x_display, clipboard_atom, utf8_atom, property_atom, window, CurrentTime);
	XFlush(g_ska.x_display);

	// Wait for SelectionNotify event with timeout
	XEvent event;
	bool received = false;
	const uint64_t timeout_ms = 500;
	const uint64_t start_time = ska_time_get_elapsed_ms();

	while (ska_time_get_elapsed_ms() - start_time < timeout_ms) {
		if (XCheckTypedWindowEvent(g_ska.x_display, window, SelectionNotify, &event)) {
			received = true;
			break;
		}
		ska_time_sleep(1);
	}

	if (!received || event.xselection.property == None) {
		return NULL;
	}

	// Get the property data
	Atom actual_type;
	int32_t actual_format;
	unsigned long nitems, bytes_after;
	unsigned char* data = NULL;

	int32_t result = XGetWindowProperty(
		g_ska.x_display, window, property_atom,
		0, 0x1FFFFFFF, False, AnyPropertyType,
		&actual_type, &actual_format, &nitems, &bytes_after, &data
	);

	if (result != Success || !data || nitems == 0) {
		if (data) XFree(data);
		XDeleteProperty(g_ska.x_display, window, property_atom);
		return NULL;
	}

	char* text = (char*)malloc(nitems + 1);
	if (text) {
		memcpy(text, data, nitems);
		text[nitems] = '\0';
	}

	XFree(data);
	XDeleteProperty(g_ska.x_display, window, property_atom);

	return text;
}

bool ska_platform_clipboard_set_text(const char* text) {
	if (!g_ska.x_display || !text) {
		ska_set_error("ska_platform_clipboard_set_text: invalid display or text");
		return false;
	}

	Atom clipboard_atom = XInternAtom(g_ska.x_display, "CLIPBOARD", False);

	// Find a window to use as selection owner
	Window window = None;
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (g_ska.windows[i]) {
			window = g_ska.windows[i]->xwindow;
			break;
		}
	}

	if (window == None) {
		ska_set_error("ska_platform_clipboard_set_text: no window available");
		return false;
	}

	// Store the text in a window property
	Atom property_atom = XInternAtom(g_ska.x_display, "SKA_CLIPBOARD_DATA", False);
	Atom utf8_atom = XInternAtom(g_ska.x_display, "UTF8_STRING", False);

	XChangeProperty(
		g_ska.x_display, window, property_atom,
		utf8_atom, 8, PropModeReplace,
		(const unsigned char*)text, strlen(text)
	);

	// Take ownership of the clipboard
	XSetSelectionOwner(g_ska.x_display, clipboard_atom, window, CurrentTime);
	XFlush(g_ska.x_display);

	// Verify ownership
	Window owner = XGetSelectionOwner(g_ska.x_display, clipboard_atom);
	if (owner != window) {
		ska_set_error("ska_platform_clipboard_set_text: failed to acquire clipboard ownership");
		return false;
	}

	return true;
}

// ========== File Dialog ==========

#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

// Pending file dialog process
typedef struct {
	pid_t                    pid;
	int                      pipe_fd;
	ska_file_dialog_id_t     id;
	char*                    title;
	bool                     active;
} ska_linux_file_dialog_t;

static ska_linux_file_dialog_t g_linux_file_dialog = {0};

// Check which file dialog tool is available
typedef enum {
	SKA_LINUX_DIALOG_NONE = 0,
	SKA_LINUX_DIALOG_ZENITY,
	SKA_LINUX_DIALOG_KDIALOG,
} ska_linux_dialog_tool_;

static ska_linux_dialog_tool_ ska_linux_get_dialog_tool(void) {
	// Check for zenity first (GTK, most common)
	if (system("which zenity > /dev/null 2>&1") == 0) {
		return SKA_LINUX_DIALOG_ZENITY;
	}
	// Check for kdialog (KDE)
	if (system("which kdialog > /dev/null 2>&1") == 0) {
		return SKA_LINUX_DIALOG_KDIALOG;
	}
	return SKA_LINUX_DIALOG_NONE;
}

bool ska_platform_file_dialog_available(ska_file_dialog_ type) {
	(void)type; // All types supported if we have a dialog tool
	return ska_linux_get_dialog_tool() != SKA_LINUX_DIALOG_NONE;
}

bool ska_platform_file_dialog_show(ska_file_dialog_id_t id, const ska_file_dialog_request_t* request) {
	if (g_linux_file_dialog.active) {
		ska_set_error("File dialog already active");
		return false;
	}

	ska_linux_dialog_tool_ tool = ska_linux_get_dialog_tool();
	if (tool == SKA_LINUX_DIALOG_NONE) {
		ska_set_error("No file dialog tool available (install zenity or kdialog)");
		return false;
	}

	// Create pipe for reading result
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		ska_set_error("Failed to create pipe: %s", strerror(errno));
		return false;
	}

	pid_t pid = fork();
	if (pid == -1) {
		ska_set_error("Failed to fork: %s", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return false;
	}

	if (pid == 0) {
		// Child process
		close(pipefd[0]); // Close read end
		dup2(pipefd[1], STDOUT_FILENO); // Redirect stdout to pipe
		close(pipefd[1]);

		// Build command arguments
		if (tool == SKA_LINUX_DIALOG_ZENITY) {
			char* args[32];
			int argc = 0;
			args[argc++] = "zenity";

			switch (request->type) {
				case ska_file_dialog_open:
					args[argc++] = "--file-selection";
					if (request->allow_multiple) {
						args[argc++] = "--multiple";
						args[argc++] = "--separator=\n";
					}
					break;
				case ska_file_dialog_save:
					args[argc++] = "--file-selection";
					args[argc++] = "--save";
					args[argc++] = "--confirm-overwrite";
					break;
				case ska_file_dialog_open_folder:
					args[argc++] = "--file-selection";
					args[argc++] = "--directory";
					break;
			}

			if (request->title) {
				args[argc++] = "--title";
				args[argc++] = (char*)request->title;
			}

			if (request->default_name && request->type == ska_file_dialog_save) {
				args[argc++] = "--filename";
				args[argc++] = (char*)request->default_name;
			}

			// Add file filters
			for (int32_t i = 0; i < request->filter_count && argc < 28; i++) {
				static char filter_buf[8][256];
				const char* exts = ska_filter_get_exts(&request->filters[i]);
				snprintf(filter_buf[i], sizeof(filter_buf[i]), "%s | %s",
				         request->filters[i].name, exts);
				args[argc++] = "--file-filter";
				args[argc++] = filter_buf[i];
			}

			args[argc] = NULL;
			execvp("zenity", args);
		}
		else if (tool == SKA_LINUX_DIALOG_KDIALOG) {
			char* args[32];
			int argc = 0;
			args[argc++] = "kdialog";

			switch (request->type) {
				case ska_file_dialog_open:
					if (request->allow_multiple) {
						args[argc++] = "--getopenfilename";
						args[argc++] = ".";
						args[argc++] = "--multiple";
						args[argc++] = "--separate-output";
					} else {
						args[argc++] = "--getopenfilename";
						args[argc++] = ".";
					}
					break;
				case ska_file_dialog_save:
					args[argc++] = "--getsavefilename";
					args[argc++] = request->default_name ? (char*)request->default_name : ".";
					break;
				case ska_file_dialog_open_folder:
					args[argc++] = "--getexistingdirectory";
					args[argc++] = ".";
					break;
			}

			if (request->title) {
				args[argc++] = "--title";
				args[argc++] = (char*)request->title;
			}

			args[argc] = NULL;
			execvp("kdialog", args);
		}

		// If exec fails
		_exit(1);
	}

	// Parent process
	close(pipefd[1]); // Close write end

	// Set pipe to non-blocking
	int flags = fcntl(pipefd[0], F_GETFL, 0);
	fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

	g_linux_file_dialog.pid = pid;
	g_linux_file_dialog.pipe_fd = pipefd[0];
	g_linux_file_dialog.id = id;
	g_linux_file_dialog.title = request->title ? strdup(request->title) : NULL;
	g_linux_file_dialog.active = true;

	return true;
}

// Called from ska_platform_pump_events to check for dialog completion
static void ska_linux_check_file_dialog(void) {
	if (!g_linux_file_dialog.active) return;

	int status;
	pid_t result = waitpid(g_linux_file_dialog.pid, &status, WNOHANG);

	if (result == 0) {
		// Still running
		return;
	}

	// Process completed or errored
	ska_file_dialog_result_t* dialog_result = ska_file_dialog_result_alloc(
		g_linux_file_dialog.id,
		g_linux_file_dialog.title
	);

	bool cancelled = true;
	if (result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		// Success - read paths from pipe
		cancelled = false;
		char buffer[4096];
		ssize_t total = 0;
		ssize_t n;

		// Read all available data
		while ((n = read(g_linux_file_dialog.pipe_fd, buffer + total, sizeof(buffer) - total - 1)) > 0) {
			total += n;
			if (total >= (ssize_t)sizeof(buffer) - 1) break;
		}
		buffer[total] = '\0';

		// Parse paths (separated by newlines)
		char* line = buffer;
		while (*line) {
			char* end = strchr(line, '\n');
			if (end) *end = '\0';

			if (*line) { // Skip empty lines
				ska_file_dialog_result_add_path(dialog_result, line);
			}

			if (end) {
				line = end + 1;
			} else {
				break;
			}
		}

		// If no paths were found, treat as cancelled
		if (dialog_result->path_count == 0) {
			cancelled = true;
		}
	}

	// Cleanup
	close(g_linux_file_dialog.pipe_fd);
	if (g_linux_file_dialog.title) {
		free(g_linux_file_dialog.title);
	}
	g_linux_file_dialog.active = false;

	// Post the result event
	ska_file_dialog_result_complete(dialog_result, cancelled);
}

// ========== Window Icon ==========

bool ska_platform_window_set_icon(ska_window_t* ref_window, const uint8_t* pixels, int32_t width, int32_t height) {
	if (!g_ska.x_display || !ref_window || !ref_window->xwindow) {
		ska_set_error("ska_platform_window_set_icon: invalid display or window");
		return false;
	}

	if (!pixels || width <= 0 || height <= 0) {
		ska_set_error("ska_platform_window_set_icon: invalid icon data");
		return false;
	}

	// _NET_WM_ICON format: array of unsigned long
	// [width, height, pixel_data...] where pixel data is ARGB packed into unsigned long
	size_t pixel_count = (size_t)width * (size_t)height;
	size_t data_size = 2 + pixel_count; // width + height + pixels

	unsigned long* icon_data = (unsigned long*)malloc(data_size * sizeof(unsigned long));
	if (!icon_data) {
		ska_set_error("ska_platform_window_set_icon: failed to allocate icon data");
		return false;
	}

	icon_data[0] = (unsigned long)width;
	icon_data[1] = (unsigned long)height;

	// Convert RGBA to ARGB (X11 expects ARGB in native byte order)
	for (size_t i = 0; i < pixel_count; i++) {
		uint8_t r = pixels[i * 4 + 0];
		uint8_t g = pixels[i * 4 + 1];
		uint8_t b = pixels[i * 4 + 2];
		uint8_t a = pixels[i * 4 + 3];
		// Pack as ARGB (alpha in high byte)
		icon_data[2 + i] = ((unsigned long)a << 24) | ((unsigned long)r << 16) |
		                   ((unsigned long)g << 8)  | ((unsigned long)b);
	}

	Atom net_wm_icon = XInternAtom(g_ska.x_display, "_NET_WM_ICON", False);

	XChangeProperty(
		g_ska.x_display,
		ref_window->xwindow,
		net_wm_icon,
		XA_CARDINAL,
		32,
		PropModeReplace,
		(unsigned char*)icon_data,
		(int)data_size
	);

	XFlush(g_ska.x_display);

	free(icon_data);
	return true;
}

// ============================================================================
// KVP Store (Linux: ~/.config/<app_name>/<key>)
// ============================================================================

#include <sys/stat.h>
#include <errno.h>
#include <pwd.h>
#include <limits.h>

static bool ska_kvpstore_get_path(const char* key, char* buffer, size_t buffer_size) {
	// Get config directory (XDG_CONFIG_HOME or ~/.config)
	const char* config_home = getenv("XDG_CONFIG_HOME");
	char config_dir[PATH_MAX];

	if (config_home && config_home[0] != '\0') {
		snprintf(config_dir, sizeof(config_dir), "%s", config_home);
	} else {
		const char* home = getenv("HOME");
		if (!home) {
			struct passwd* pw = getpwuid(getuid());
			if (pw) home = pw->pw_dir;
		}
		if (!home) {
			ska_set_error("ska_kvpstore: unable to determine home directory");
			return false;
		}
		snprintf(config_dir, sizeof(config_dir), "%s/.config", home);
	}

	// Create config dir if needed
	struct stat st;
	if (stat(config_dir, &st) != 0) {
		if (mkdir(config_dir, 0755) != 0 && errno != EEXIST) {
			ska_set_error("ska_kvpstore: failed to create config directory");
			return false;
		}
	}

	// Create app dir
	char app_dir[PATH_MAX];
	snprintf(app_dir, sizeof(app_dir), "%s/%s", config_dir, ska_kvpstore_get_app_name());
	if (stat(app_dir, &st) != 0) {
		if (mkdir(app_dir, 0755) != 0 && errno != EEXIST) {
			ska_set_error("ska_kvpstore: failed to create app directory");
			return false;
		}
	}

	snprintf(buffer, buffer_size, "%s/%s", app_dir, key);
	return true;
}

SKA_API bool ska_kvpstore_save(const char* key, const void* data, size_t size) {
	if (!ska_kvpstore_validate_key(key)) return false;
	if (!data && size > 0) {
		ska_set_error("ska_kvpstore_save: NULL data with non-zero size");
		return false;
	}

	char path[PATH_MAX];
	if (!ska_kvpstore_get_path(key, path, sizeof(path))) {
		return false;
	}

	return ska_file_write(path, data, size);
}

SKA_API bool ska_kvpstore_load(const char* key, void* opt_buffer, size_t buffer_size, size_t* opt_out_size) {
	if (!ska_kvpstore_validate_key(key)) return false;

	char path[PATH_MAX];
	if (!ska_kvpstore_get_path(key, path, sizeof(path))) {
		return false;
	}

	if (!ska_file_exists(path)) {
		return false;
	}

	size_t file_size = ska_file_size(path);
	if (opt_out_size) {
		*opt_out_size = file_size;
	}

	// Size query only
	if (!opt_buffer || buffer_size == 0) {
		return true;
	}

	// Load data
	void* file_data = NULL;
	size_t actual_size = 0;
	if (!ska_file_read(path, &file_data, &actual_size)) {
		return false;
	}

	size_t copy_size = (actual_size < buffer_size) ? actual_size : buffer_size;
	memcpy(opt_buffer, file_data, copy_size);
	ska_file_free_data(file_data);

	return true;
}

SKA_API bool ska_kvpstore_delete(const char* key) {
	if (!ska_kvpstore_validate_key(key)) return false;

	char path[PATH_MAX];
	if (!ska_kvpstore_get_path(key, path, sizeof(path))) {
		return false;
	}

	if (unlink(path) != 0 && errno != ENOENT) {
		ska_set_error("ska_kvpstore_delete: failed to delete '%s'", key);
		return false;
	}

	return true;
}

#endif // SKA_PLATFORM_LINUX
