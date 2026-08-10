//
// sk_app - Linux X11 platform backend
//
// One of two interchangeable Linux backends. Nothing here is called directly;
// ska_linux.c dispatches through ska_x11_vtable at the bottom of this file.

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "ska_internal.h"

#if defined(SKA_PLATFORM_LINUX) && defined(SKA_LINUX_X11)

#include <X11/keysym.h>
#include <X11/Xresource.h>
#include <X11/XKBlib.h>
#include <locale.h>
#include <stdlib.h>
#include <unistd.h>

// Rewrites every Xlib call below to a dlsym'd pointer, so must stay last.
#include "ska_x11_dyn.h"

// Used by ska_x11_window_create, which is defined ahead of it.
float ska_x11_get_dpi_scale(const ska_window_t* window);

// File-scope backend state, up here so ska_x11_shutdown can reset all of it;
// a re-init must not reuse XIDs or flags from a dead connection.
static Cursor             g_x_cursors[ska_system_cursor_count_];
static Cursor             g_x_invisible_cursor;
static ska_system_cursor_ g_current_cursor = ska_system_cursor_arrow;
static bool               g_x_relative;
static int32_t            g_x_xi2_opcode = -1;
static double             g_x_rel_carry_x;
static double             g_x_rel_carry_y;

static ska_window_t* ska_find_window_by_xwindow(Window xwin) {
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (g_ska.windows[i] && g_ska.windows[i]->xwindow == xwin) {
			return g_ska.windows[i];
		}
	}
	return NULL;
}

bool ska_x11_init(void) {
	if (!ska_x11_dyn_load()) {
		return false;
	}

	// Set locale for X11
	setlocale(LC_ALL, "");
	XSetLocaleModifiers("");

	g_ska.cached_dpi_scale = 0.0f;

	// Display, atoms, XIM, and root-window event selection are deferred to
	// ska_linux_ensure_x_display so headless callers (CI, library use without a
	// window) can succeed at ska_init.
	return true;
}

// Lazily opens the X11 display and per-display state (atoms, XIM, Xrm).
// Idempotent on success: once g_ska.x_display is set, returns true without
// side effects. Failures are not memoized, so each call with no X server
// re-runs XOpenDisplay and re-sets the error. Only ska_x11_window_create
// triggers this today; window-bound APIs reach a display through that path.
static bool ska_linux_ensure_x_display(void) {
	if (g_ska.x_display) {
		return true;
	}

	Display* display = XOpenDisplay(NULL);
	if (!display) {
		ska_set_error("Failed to open X11 display");
		return false;
	}

	g_ska.x_display = display;
	g_ska.x_screen  = DefaultScreen(display);
	g_ska.x_root    = RootWindow(display, g_ska.x_screen);

	// Suppress X11's synthetic KeyRelease that precedes every auto-repeat
	// KeyPress while a key is held. Without this, held-key polling alternates
	// between "pressed" and "released" at the X server's auto-repeat rate
	// (~25-30 Hz), so held movement keys produce stuttery half-rate motion
	// because half of the sk_steps observe the key as briefly released.
	// XKB has supported detectable auto-repeat since the late 90s; the
	// supported_rtrn out-param is checked for completeness but a False from
	// it just means we fall back to the historical noisy behavior.
	Bool xkb_detectable_supported = False;
	XkbSetDetectableAutoRepeat(display, True, &xkb_detectable_supported);

	g_ska.xim = XOpenIM(display, NULL, NULL, NULL);
	if (!g_ska.xim) {
		ska_log(ska_log_warn, "Failed to open X Input Method");
	}

	// Required before any XrmGetResource call (DPI queries).
	XrmInitialize();

	g_ska.wm_protocols                = XInternAtom(display, "WM_PROTOCOLS", False);
	g_ska.wm_delete_window            = XInternAtom(display, "WM_DELETE_WINDOW", False);
	g_ska.net_wm_state                = XInternAtom(display, "_NET_WM_STATE", False);
	g_ska.net_wm_state_fullscreen     = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
	g_ska.net_wm_state_maximized_vert = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
	g_ska.net_wm_state_maximized_horz = XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
	g_ska.resource_manager            = XInternAtom(display, "RESOURCE_MANAGER", False);

	// Watch root for RESOURCE_MANAGER property changes (xrdb-driven DPI changes).
	XSelectInput(display, g_ska.x_root, PropertyChangeMask);

	return true;
}

void ska_x11_shutdown(void) {
	// Teardown is conditional: ska_linux_ensure_x_display may never have been
	// called (headless run), in which case xim/display are NULL and there is
	// nothing X11-side to release.
	if (g_ska.xim) {
		XCloseIM(g_ska.xim);
		g_ska.xim = NULL;
	}

	if (g_ska.x_display) {
		XCloseDisplay(g_ska.x_display);
		g_ska.x_display = NULL;
	}

	// Server-side handles died with the display; a re-init must not see them
	memset(g_x_cursors, 0, sizeof(g_x_cursors));
	g_x_invisible_cursor = None;
	g_current_cursor     = ska_system_cursor_arrow;
	g_x_relative         = false;
	g_x_xi2_opcode       = -1;

	ska_x11_dyn_unload();
}

bool ska_x11_window_create(
	ska_window_t* window,
	const char* title,
	int32_t x, int32_t y,
	int32_t w, int32_t h,
	uint32_t flags
) {
	// First windowing call wins the right to fail loudly if there's no X server;
	// later calls reuse the already-opened display.
	if (!ska_linux_ensure_x_display()) {
		return false;
	}

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

	// w and h arrive as screen coordinates, so the scale has to be resolved
	// before anything is sized. window->flags is already set by the caller,
	// which is what ska_x11_scale reads.
	window->dpi_scale = ska_x11_get_dpi_scale(window);
	int32_t pixel_w   = ska_to_pixels(window, w);
	int32_t pixel_h   = ska_to_pixels(window, h);

	// Center window if requested. Explicit positions arrive as screen
	// coordinates and convert like the size; the centered ones are computed in
	// pixels already.
	if (x == -1 || y == -1) {
		Screen* screen = DefaultScreenOfDisplay(g_ska.x_display);
		x = (WidthOfScreen(screen) - pixel_w) / 2;
		y = (HeightOfScreen(screen) - pixel_h) / 2;
	} else {
		x = ska_to_pixels(window, x);
		y = ska_to_pixels(window, y);
	}

	// Create window
	window->xwindow = XCreateWindow(
		g_ska.x_display,
		g_ska.x_root,
		x, y, pixel_w, pixel_h,
		0,
		CopyFromParent,
		InputOutput,
		CopyFromParent,
		CWEventMask | CWColormap,
		&wa
	);

	if (!window->xwindow) {
		// The colormap was created before the window and has no other owner
		XFreeColormap(g_ska.x_display, wa.colormap);
		ska_set_error("Failed to create X11 window");
		return false;
	}

	// Set window title
	XStoreName(g_ska.x_display, window->xwindow, title);
	XSetIconName(g_ska.x_display, window->xwindow, title);

	// Set WM_CLASS for desktop file matching (StartupWMClass), which is how
	// the window finds its icon.
	const char* app_id = g_ska.app_id ? g_ska.app_id : title;
	XClassHint* class_hint = XAllocClassHint();
	if (class_hint) {
		class_hint->res_name = (char*)app_id;   // Instance name
		class_hint->res_class = (char*)app_id;  // Class name
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
	window->title = ska_strdup(title);

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
			// Size hints are a pixel constraint on the X window
			size_hints->flags |= PMinSize | PMaxSize;
			size_hints->min_width  = size_hints->max_width  = pixel_w;
			size_hints->min_height = size_hints->max_height = pixel_h;
		}
		XSetWMNormalHints(g_ska.x_display, window->xwindow, size_hints);
		XFree(size_hints);
	}

	// Fullscreen: set the state property before mapping so the window manager
	// maps the window directly into fullscreen (no windowed->fullscreen hop).
	// The window resizes to the output; a ska_event_window_resized follows.
	if (flags & ska_window_fullscreen) {
		XChangeProperty(g_ska.x_display, window->xwindow,
		                g_ska.net_wm_state, XA_ATOM, 32, PropModeReplace,
		                (unsigned char*)&g_ska.net_wm_state_fullscreen, 1);
	}

	// Ensure it knows its process id. Format 32 makes Xlib read a whole long per
	// element, so a 4-byte pid_t here reads past the variable; it only works on
	// little-endian, where the stray high bytes get truncated away.
	Atom net_wm_pid = XInternAtom(g_ska.x_display, "_NET_WM_PID", False);
	long pid        = (long)getpid();
	XChangeProperty(g_ska.x_display, window->xwindow, net_wm_pid, XA_CARDINAL, 32, PropModeReplace, (unsigned char*)&pid, 1);

	window->x               = ska_to_logical(window, x);
	window->y               = ska_to_logical(window, y);
	window->width           = w;       // Screen coordinates, as requested
	window->height          = h;
	window->drawable_width  = pixel_w; // Actual X window size
	window->drawable_height = pixel_h;

	// Cache DPI scale for change detection (first window sets it)
	if (g_ska.cached_dpi_scale == 0.0f) {
		g_ska.cached_dpi_scale = window->dpi_scale;
	}

	return true;
}

void ska_x11_window_destroy(ska_window_t* ref_window) {
	if (ref_window->xic) {
		XDestroyIC(ref_window->xic);
	}

	if (ref_window->xwindow) {
		XDestroyWindow(g_ska.x_display, ref_window->xwindow);
		XFlush(g_ska.x_display);
	}
}

void ska_x11_window_set_title(ska_window_t* ref_window, const char* title) {
	if (ref_window->title) {
		ska_free(ref_window->title);
	}
	ref_window->title = ska_strdup(title);
	XStoreName(g_ska.x_display, ref_window->xwindow, title);
	XSetIconName(g_ska.x_display, ref_window->xwindow, title);
	XFlush(g_ska.x_display);
}

void ska_x11_get_frame_extents(const ska_window_t* window, int32_t* opt_out_left, int32_t* opt_out_right, int32_t* opt_out_top, int32_t* opt_out_bottom) {
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

	// _NET_FRAME_EXTENTS is in pixels, but the caller adds these to the content
	// size, which is in screen coordinates.
	if (opt_out_left)   *opt_out_left   = ska_to_logical(window, left);
	if (opt_out_right)  *opt_out_right  = ska_to_logical(window, right);
	if (opt_out_top)    *opt_out_top    = ska_to_logical(window, top);
	if (opt_out_bottom) *opt_out_bottom = ska_to_logical(window, bottom);
}

void ska_x11_window_set_frame_position(ska_window_t* ref_window, int32_t x, int32_t y) {
	XMoveWindow(g_ska.x_display, ref_window->xwindow,
		ska_to_pixels(ref_window, x), ska_to_pixels(ref_window, y));
	XFlush(g_ska.x_display);

	// Update cached content position
	int32_t left, top;
	ska_x11_get_frame_extents(ref_window, &left, NULL, &top, NULL);
	ref_window->x = x + left;
	ref_window->y = y + top;
}

void ska_x11_window_set_frame_size(ska_window_t* ref_window, int32_t w, int32_t h) {
	XResizeWindow(g_ska.x_display, ref_window->xwindow,
		(uint32_t)ska_to_pixels(ref_window, w), (uint32_t)ska_to_pixels(ref_window, h));
	XFlush(g_ska.x_display);
}

void ska_x11_window_show(ska_window_t* ref_window) {
	XMapWindow(g_ska.x_display, ref_window->xwindow);
	XFlush(g_ska.x_display);
	ref_window->is_visible = true;
}

void ska_x11_window_hide(ska_window_t* ref_window) {
	XUnmapWindow(g_ska.x_display, ref_window->xwindow);
	XFlush(g_ska.x_display);
	ref_window->is_visible = false;
}

void ska_x11_window_maximize(ska_window_t* ref_window) {
	XEvent event = {0};
	event.type = ClientMessage;
	event.xclient.window = ref_window->xwindow;
	event.xclient.message_type = g_ska.net_wm_state;
	event.xclient.format = 32;
	event.xclient.data.l[0] = 1; // _NET_WM_STATE_ADD
	event.xclient.data.l[1] = g_ska.net_wm_state_maximized_vert;
	event.xclient.data.l[2] = g_ska.net_wm_state_maximized_horz;

	XSendEvent(g_ska.x_display, g_ska.x_root, False,
			   SubstructureNotifyMask | SubstructureRedirectMask, &event);
	XFlush(g_ska.x_display);
}

void ska_x11_window_minimize(ska_window_t* ref_window) {
	XIconifyWindow(g_ska.x_display, ref_window->xwindow, g_ska.x_screen);
	XFlush(g_ska.x_display);
}

void ska_x11_window_set_fullscreen(ska_window_t* ref_window, bool fullscreen) {
	if (ref_window->is_visible) {
		// Mapped windows change state via a client message to the root window
		XEvent event = {0};
		event.type = ClientMessage;
		event.xclient.window = ref_window->xwindow;
		event.xclient.message_type = g_ska.net_wm_state;
		event.xclient.format = 32;
		event.xclient.data.l[0] = fullscreen ? 1 : 0; // _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE
		event.xclient.data.l[1] = g_ska.net_wm_state_fullscreen;

		XSendEvent(g_ska.x_display, g_ska.x_root, False,
				   SubstructureNotifyMask | SubstructureRedirectMask, &event);
	} else {
		// Unmapped windows take the state property directly
		if (fullscreen) {
			XChangeProperty(g_ska.x_display, ref_window->xwindow,
			                g_ska.net_wm_state, XA_ATOM, 32, PropModeReplace,
			                (unsigned char*)&g_ska.net_wm_state_fullscreen, 1);
		} else {
			XDeleteProperty(g_ska.x_display, ref_window->xwindow, g_ska.net_wm_state);
		}
	}
	XFlush(g_ska.x_display);
}

void ska_x11_window_restore(ska_window_t* ref_window) {
	// First, remove maximize state if window is maximized
	XEvent event = {0};
	event.type = ClientMessage;
	event.xclient.window = ref_window->xwindow;
	event.xclient.message_type = g_ska.net_wm_state;
	event.xclient.format = 32;
	event.xclient.data.l[0] = 0; // _NET_WM_STATE_REMOVE
	event.xclient.data.l[1] = g_ska.net_wm_state_maximized_vert;
	event.xclient.data.l[2] = g_ska.net_wm_state_maximized_horz;

	XSendEvent(g_ska.x_display, g_ska.x_root, False,
			   SubstructureNotifyMask | SubstructureRedirectMask, &event);

	// Then ensure window is mapped (in case it was minimized)
	XMapWindow(g_ska.x_display, ref_window->xwindow);
	XFlush(g_ska.x_display);
}

void ska_x11_window_raise(ska_window_t* ref_window) {
	XRaiseWindow(g_ska.x_display, ref_window->xwindow);
	// Only set focus if window is visible and actually mapped
	if (ref_window->is_visible) {
		// Sync and verify the window is actually mapped before setting focus
		XSync(g_ska.x_display, False);

		XWindowAttributes attrs;
		if (XGetWindowAttributes(g_ska.x_display, ref_window->xwindow, &attrs) &&
		    attrs.map_state == IsViewable) {
			XSetInputFocus(g_ska.x_display, ref_window->xwindow, RevertToPointerRoot, CurrentTime);
		}
	}
	XFlush(g_ska.x_display);
}

void ska_x11_window_get_drawable_size(ska_window_t* ref_window, int32_t* opt_out_width, int32_t* opt_out_height) {
	if (opt_out_width)  *opt_out_width  = ref_window->drawable_width;
	if (opt_out_height) *opt_out_height = ref_window->drawable_height;
}

float ska_x11_get_dpi_scale(const ska_window_t* window) {
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

float ska_x11_get_refresh_rate(const ska_window_t* window) {
	(void)window;

	// Callable before any window exists, and the display is opened lazily
	if (!g_ska.x_display) return 0.0f;

	// Use XRandR to get the current screen refresh rate
	XRRScreenConfiguration* config = XRRGetScreenInfo(g_ska.x_display, g_ska.x_root);
	if (!config) {
		return 0.0f;
	}

	short rate = XRRConfigCurrentRate(config);
	XRRFreeScreenConfigInfo(config);

	return (float)rate;
}

void ska_x11_set_cursor(ska_system_cursor_ cursor) {
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

	// No-op when no display has been opened (headless run, no windows).
	// We don't latch the requested cursor: with no windows there's nothing to
	// apply it to later anyway, and ska_x11_show_cursor's restore path
	// also bails when there's no display.
	if (!g_ska.x_display) {
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

	// Remember current cursor for ska_x11_show_cursor
	g_current_cursor = cursor;

	// Apply to all windows
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (g_ska.windows[i]) {
			XDefineCursor(g_ska.x_display, g_ska.windows[i]->xwindow, g_x_cursors[cursor]);
		}
	}
	XFlush(g_ska.x_display);
}

void ska_x11_show_cursor(bool show) {
	if (!g_ska.x_display) {
		return;
	}

	if (show) {
		// Restore the current cursor (don't use XUndefineCursor which resets to parent's cursor)
		for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
			if (g_ska.windows[i]) {
				XDefineCursor(g_ska.x_display, g_ska.windows[i]->xwindow, g_x_cursors[g_current_cursor]);
			}
		}
	} else {
		// Create invisible cursor
		if (g_x_invisible_cursor == None) {
			char data[1] = {0};
			Pixmap blank = XCreateBitmapFromData(g_ska.x_display, g_ska.x_root, data, 1, 1);
			XColor color = {0};
			g_x_invisible_cursor = XCreatePixmapCursor(g_ska.x_display, blank, blank, &color, &color, 0, 0);
			XFreePixmap(g_ska.x_display, blank);
		}

		for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
			if (g_ska.windows[i]) {
				XDefineCursor(g_ska.x_display, g_ska.windows[i]->xwindow, g_x_invisible_cursor);
			}
		}
	}
	XFlush(g_ska.x_display);
}

// XInput2 raw motion is the only unaccelerated, unclamped pointer source X11
// offers. It needs no warping, which is what made the old approach expensive:
// a warp per frame had to hide and show the cursor sprite to look right under
// XWayland, and that stalled the frame.

// Raw motion is delivered against the root window, so the event carries no
// window of its own and the focused one stands in.
static ska_window_t* ska_x11_focused_window(void) {
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (g_ska.windows[i] && g_ska.windows[i]->has_focus) return g_ska.windows[i];
	}
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (g_ska.windows[i]) return g_ska.windows[i];
	}
	return NULL;
}

static bool ska_x11_xi2_ready(void) {
	if (g_x_xi2_opcode >= 0) return true;
	if (!ska_x11_dyn_has_xi2()) return false;

	int32_t event = 0, error = 0, opcode = 0;
	if (!XQueryExtension(g_ska.x_display, "XInputExtension", &opcode, &event, &error)) return false;

	// Raw events are the 2.0 baseline, so nothing newer is required
	int32_t major = 2, minor = 0;
	if (XIQueryVersion(g_ska.x_display, &major, &minor) != Success) return false;

	g_x_xi2_opcode = opcode;
	return true;
}

static void ska_x11_xi2_select(bool enabled) {
	unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {0};
	if (enabled) XISetMask(bits, XI_RawMotion);

	XIEventMask mask = {
		.deviceid = XIAllMasterDevices,
		.mask_len = sizeof(bits),
		.mask     = bits,
	};
	XISelectEvents(g_ska.x_display, g_ska.x_root, &mask, 1);
}

bool ska_x11_set_relative_mouse_mode(bool enabled) {
	// Disabling a mode that was never on must not open a display to do it
	if (!enabled && !g_x_relative) return true;
	if (!ska_linux_ensure_x_display()) return false;

	if (!enabled) {
		ska_x11_xi2_select(false);
		XUngrabPointer(g_ska.x_display, CurrentTime);
		g_x_relative = false;
		// Back to the app's own choice, which entering relative mode overrode
		ska_x11_show_cursor(g_ska.input_state.cursor_visible);
		XFlush(g_ska.x_display);
		return true;
	}
	if (g_x_relative) return true;

	if (!ska_x11_xi2_ready()) {
		ska_set_error("ska_mouse_set_relative_mode: XInput2 is unavailable");
		return false;
	}

	// The grab is what keeps the pointer inside the window and the events ours,
	// so motion past the edge still arrives as raw deltas.
	ska_window_t* window = ska_x11_focused_window();
	Window        xwin   = window ? window->xwindow : g_ska.x_root;
	int32_t grab = XGrabPointer(g_ska.x_display, xwin, True,
		ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
		GrabModeAsync, GrabModeAsync, xwin, None, CurrentTime);
	if (grab != GrabSuccess) {
		// Fails routinely: another client's grab, or a window not yet viewable
		ska_set_error("ska_mouse_set_relative_mode: pointer grab failed (%d)", grab);
		return false;
	}

	ska_x11_xi2_select(true);
	ska_x11_show_cursor(false);
	g_x_rel_carry_x = 0.0;
	g_x_rel_carry_y = 0.0;
	g_x_relative    = true;
	XFlush(g_ska.x_display);
	return true;
}

// Raw valuators are in device units and can be fractional, so the leftover is
// carried rather than dropped, which is what keeps a slow drag registering.
static void ska_x11_handle_raw_motion(const XIRawEvent* raw) {
	ska_window_t* window = ska_x11_focused_window();
	if (!window) return;

	const double* value = raw->raw_values;
	double        dx    = 0.0;
	double        dy    = 0.0;
	if (XIMaskIsSet(raw->valuators.mask, 0)) dx = *value++;
	if (XIMaskIsSet(raw->valuators.mask, 1)) dy = *value++;

	g_x_rel_carry_x += dx;
	g_x_rel_carry_y += dy;

	int32_t rel_x = (int32_t)g_x_rel_carry_x;
	int32_t rel_y = (int32_t)g_x_rel_carry_y;
	g_x_rel_carry_x -= rel_x;
	g_x_rel_carry_y -= rel_y;
	if (rel_x == 0 && rel_y == 0) return;

	ska_event_t event = {0};
	event.timestamp                = ska_time_get_elapsed_ms();
	event.type                     = ska_event_mouse_motion;
	event.mouse_motion.window_id   = window->id;
	event.mouse_motion.x           = g_ska.input_state.mouse_x;
	event.mouse_motion.y           = g_ska.input_state.mouse_y;
	event.mouse_motion.xrel        = rel_x;
	event.mouse_motion.yrel        = rel_y;

	ska_input_add_relative(rel_x, rel_y);
	ska_post_event(&event);
}

typedef struct {
	const XEvent* release;
	bool          found;
} ska_x11_keyrepeat_check_t;

// Predicate for XCheckIfEvent. Always returns False so the matched event
// stays in the queue (we want to consume the paired KeyPress normally on
// the next pump iteration). XCheckIfEvent walks the entire queue calling
// the predicate on each event, so an interleaved MotionNotify or Expose
// between the release and its paired press will not hide the match.
static Bool ska_x11_keyrepeat_predicate(Display* display, XEvent* peek, XPointer arg) {
	(void)display;
	ska_x11_keyrepeat_check_t* d = (ska_x11_keyrepeat_check_t*)arg;
	if (peek->type            == KeyPress                          &&
	    peek->xkey.window     == d->release->xkey.window           &&
	    peek->xkey.keycode    == d->release->xkey.keycode          &&
	    peek->xkey.time - d->release->xkey.time < 2) {
		d->found = true;
	}
	return False;
}

void ska_x11_pump_events(void) {
	// File-dialog subprocesses (zenity/kdialog) run independent of our X
	// connection, so they're still polled in headless runs.
	if (!g_ska.x_display) {
		ska_linux_check_file_dialog();
		return;
	}

	while (XPending(g_ska.x_display)) {
		XEvent xev;
		XNextEvent(g_ska.x_display, &xev);

		// Filter through input method first
		if (XFilterEvent(&xev, None)) {
			continue;
		}

		// Raw motion arrives as a cookie, whose data must be fetched and freed
		if (xev.type == GenericEvent && xev.xcookie.extension == g_x_xi2_opcode) {
			if (g_x_relative && XGetEventData(g_ska.x_display, &xev.xcookie)) {
				if (xev.xcookie.evtype == XI_RawMotion)
					ska_x11_handle_raw_motion((const XIRawEvent*)xev.xcookie.data);
				XFreeEventData(g_ska.x_display, &xev.xcookie);
			}
			continue;
		}

		// Handle root window events (DPI change detection)
		if (xev.xany.window == g_ska.x_root) {
			if (xev.type == PropertyNotify && xev.xproperty.atom == g_ska.resource_manager) {
				// RESOURCE_MANAGER changed - check if DPI scale changed
				float new_scale = ska_x11_get_dpi_scale(NULL);
				if (new_scale != g_ska.cached_dpi_scale && g_ska.cached_dpi_scale > 0.0f) {
					g_ska.cached_dpi_scale = new_scale;

					// Send DPI changed event to all windows
					for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
						ska_window_t* win = g_ska.windows[i];
						if (win) {
							win->dpi_scale = new_scale;

							// The X window keeps its pixel size, so a scale
							// change moves the content size instead. Apps
							// re-layout at the new scale and the window stays
							// put on screen.
							int32_t logical_w = ska_to_logical(win, win->drawable_width);
							int32_t logical_h = ska_to_logical(win, win->drawable_height);
							if (logical_w != win->width || logical_h != win->height) {
								win->width  = logical_w;
								win->height = logical_h;

								ska_event_t resize = {0};
								resize.timestamp        = (uint32_t)ska_time_get_elapsed_ms();
								resize.type             = ska_event_window_resized;
								resize.window.window_id = win->id;
								resize.window.data1     = logical_w;
								resize.window.data2     = logical_h;
								ska_post_event(&resize);
							}

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
			// Fallback for the held-key alternation bug: even with the per-
			// client XkbSetDetectableAutoRepeat from ska_linux_ensure_x_display
			// in place, some servers / WMs / XWayland-style proxies still
			// inject a KeyRelease+KeyPress pair for every auto-repeat tick.
			// The two halves of a pair land within ~1 server tick of each
			// other, so a sub-2ms time delta on the same window+keycode is
			// the SDL-proven signature of an auto-repeat (and well below any
			// physical release-then-press a human finger can produce).
			// Dropping the release half keeps g_ska.input_state.keyboard
			// continuously high for the entire hold, while the press still
			// flows through (carrying text input and a derived repeat flag).
			// XCheckIfEvent walks the whole queue rather than peeking only
			// the next event, so an interleaved MotionNotify between the
			// release and its paired press will not defeat the match.
			if (xev.type == KeyRelease) {
				XEvent dummy;
				ska_x11_keyrepeat_check_t check = { .release = &xev, .found = false };
				XCheckIfEvent(g_ska.x_display, &dummy, ska_x11_keyrepeat_predicate, (XPointer)&check);
				if (check.found) {
					break;
				}
			}

			event.type = (xev.type == KeyPress) ? ska_event_key_down : ska_event_key_up;
			event.keyboard.window_id = window->id;
			event.keyboard.pressed = (xev.type == KeyPress);

			// Convert keycode to KeySym for layout-independent mapping
			KeySym keysym = XLookupKeysym(&xev.xkey, 0);
			event.keyboard.scancode = ska_linux_keysym_to_scancode((uint32_t)keysym);

			// A press arriving while the key is already tracked as down is an
			// auto-repeat. With XkbSetDetectableAutoRepeat enabled the server
			// stops emitting the paired releases, so repeats look exactly like
			// this: a KeyPress on a still-held scancode. With the peek-suppress
			// fallback above, the same condition holds because the release
			// half was dropped before reaching the state update.
			event.keyboard.repeat = (xev.type == KeyPress) &&
				event.keyboard.scancode != ska_scancode_unknown &&
				g_ska.input_state.keyboard[event.keyboard.scancode] != 0;

			// Update keyboard state FIRST (before deriving modifiers)
			if (event.keyboard.scancode != ska_scancode_unknown) {
				g_ska.input_state.keyboard[event.keyboard.scancode] = event.keyboard.pressed ? 1 : 0;
			}

			// Derive modifier state from tracked keyboard state (post-event).
			// This matches Win32's GetKeyState() behavior and avoids X11's quirk
			// where xkey.state contains pre-event modifier state.
			uint16_t mods = ska_input_state_derive_modifiers(&g_ska.input_state);
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
		}
		case ButtonPress:
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
				event.mouse_button.x = ska_to_logical(window, xev.xbutton.x);
				event.mouse_button.y = ska_to_logical(window, xev.xbutton.y);

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
			// In relative mode the XI2 raw stream is the only motion source;
			// the grab still delivers MotionNotify, and counting both would
			// double every delta.
			if (g_x_relative) break;

			int32_t mouse_x = ska_to_logical(window, xev.xmotion.x);
			int32_t mouse_y = ska_to_logical(window, xev.xmotion.y);

			event.type = ska_event_mouse_motion;
			event.mouse_motion.window_id = window->id;
			event.mouse_motion.x = mouse_x;
			event.mouse_motion.y = mouse_y;
			event.mouse_motion.xrel = mouse_x - g_ska.input_state.mouse_x;
			event.mouse_motion.yrel = mouse_y - g_ska.input_state.mouse_y;

			g_ska.input_state.mouse_x = mouse_x;
			g_ska.input_state.mouse_y = mouse_y;
			ska_input_add_relative(event.mouse_motion.xrel, event.mouse_motion.yrel);

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
			// ConfigureNotify carries pixels; the content size and the
			// event payload are both screen coordinates.
			if (xev.xconfigure.width  != window->drawable_width ||
			    xev.xconfigure.height != window->drawable_height) {
				window->drawable_width  = xev.xconfigure.width;
				window->drawable_height = xev.xconfigure.height;
				window->width           = ska_to_logical(window, xev.xconfigure.width);
				window->height          = ska_to_logical(window, xev.xconfigure.height);

				event.type             = ska_event_window_resized;
				event.window.window_id = window->id;
				event.window.data1     = window->width;
				event.window.data2     = window->height;
				ska_post_event(&event);
			}
			{
				// ConfigureNotify gives position relative to parent (WM frame).
				// Translate to root coordinates for actual screen position.
				Window child;
				int32_t root_x, root_y;
				XTranslateCoordinates(g_ska.x_display, window->xwindow, g_ska.x_root, 0, 0, &root_x, &root_y, &child);

				// Root coordinates are pixels; x/y and the event payload
				// are screen coordinates, like every other position.
				int32_t logical_x = ska_to_logical(window, root_x);
				int32_t logical_y = ska_to_logical(window, root_y);
				if (logical_x != window->x || logical_y != window->y) {
					window->x = logical_x;
					window->y = logical_y;

					event.type = ska_event_window_moved;
					event.window.window_id = window->id;
					event.window.data1 = logical_x;
					event.window.data2 = logical_y;
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

const char** ska_x11_vk_get_instance_extensions(uint32_t* out_count) {
	static const char* extensions[] = {
		"VK_KHR_surface",
		"VK_KHR_xlib_surface"
	};
	*out_count = 2;
	return extensions;
}

bool ska_x11_vk_create_surface(const ska_window_t* window, VkInstance instance, VkSurfaceKHR* out_surface) {
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = ska_linux_vk_get_proc_addr();
	if (!vkGetInstanceProcAddr) return false;

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

// ========== Clipboard Platform Functions ==========

char* ska_x11_clipboard_get_text(void) {
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

		char* text = (char*)ska_malloc(nitems + 1);
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

	char* text = (char*)ska_malloc(nitems + 1);
	if (text) {
		memcpy(text, data, nitems);
		text[nitems] = '\0';
	}

	XFree(data);
	XDeleteProperty(g_ska.x_display, window, property_atom);

	return text;
}

bool ska_x11_clipboard_set_text(const char* text) {
	if (!g_ska.x_display || !text) {
		ska_set_error("ska_x11_clipboard_set_text: invalid display or text");
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
		ska_set_error("ska_x11_clipboard_set_text: no window available");
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
		ska_set_error("ska_x11_clipboard_set_text: failed to acquire clipboard ownership");
		return false;
	}

	return true;
}

// ============================================================================
// Backend Dispatch Table
// ============================================================================

const ska_linux_vtable_t ska_x11_vtable = {
	.name                      = "x11",

	.init                      = ska_x11_init,
	.shutdown                  = ska_x11_shutdown,

	.window_create             = ska_x11_window_create,
	.window_destroy            = ska_x11_window_destroy,
	.window_set_title          = ska_x11_window_set_title,
	.window_set_frame_position = ska_x11_window_set_frame_position,
	.window_set_frame_size     = ska_x11_window_set_frame_size,
	.window_show               = ska_x11_window_show,
	.window_hide               = ska_x11_window_hide,
	.window_maximize           = ska_x11_window_maximize,
	.window_minimize           = ska_x11_window_minimize,
	.window_restore            = ska_x11_window_restore,
	.window_set_fullscreen     = ska_x11_window_set_fullscreen,
	.window_raise              = ska_x11_window_raise,
	.window_get_drawable_size  = ska_x11_window_get_drawable_size,
	.get_frame_extents         = ska_x11_get_frame_extents,
	.get_dpi_scale             = ska_x11_get_dpi_scale,
	.get_refresh_rate          = ska_x11_get_refresh_rate,

	.show_cursor               = ska_x11_show_cursor,
	.set_cursor                = ska_x11_set_cursor,
	.set_relative_mouse_mode   = ska_x11_set_relative_mouse_mode,

	.pump_events               = ska_x11_pump_events,

	.vk_get_instance_extensions = ska_x11_vk_get_instance_extensions,
	.vk_create_surface          = ska_x11_vk_create_surface,

	.clipboard_get_text        = ska_x11_clipboard_get_text,
	.clipboard_set_text        = ska_x11_clipboard_set_text,
};

#endif // SKA_PLATFORM_LINUX && SKA_LINUX_X11
