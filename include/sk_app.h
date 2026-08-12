// sk_app - Lightweight cross-platform application framework
//
// Provides window management, input handling, and Vulkan/WebGPU surface
// creation for Win32, Linux, macOS, Android, and Web (Emscripten) platforms.
//
// License: MIT

#ifndef SK_APP_H
#define SK_APP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>  // For size_t

#ifdef __cplusplus
extern "C" {
#endif

// Platform detection (only define if not already defined by build system)
#if !defined(SKA_PLATFORM_WIN32) && !defined(SKA_PLATFORM_LINUX) && \
	!defined(SKA_PLATFORM_MACOS) && !defined(SKA_PLATFORM_ANDROID) && \
	!defined(SKA_PLATFORM_WEB)
	#if defined(__EMSCRIPTEN__)
		#define SKA_PLATFORM_WEB
	#elif defined(_WIN32)
		#define SKA_PLATFORM_WIN32
	#elif defined(__ANDROID__)
		#define SKA_PLATFORM_ANDROID
	#elif defined(__linux__)
		#define SKA_PLATFORM_LINUX
	#elif defined(__APPLE__)
		#include <TargetConditionals.h>
		#if TARGET_OS_MAC
			#define SKA_PLATFORM_MACOS
		#endif
	#endif
#endif

// API export/import
#if defined(SKA_PLATFORM_WIN32)
	#ifdef SKA_BUILD_SHARED
		#ifdef SKA_EXPORT
			#define SKA_API __declspec(dllexport)
		#else
			#define SKA_API __declspec(dllimport)
		#endif
	#else
		#define SKA_API
	#endif
#else
	#define SKA_API __attribute__((visibility("default")))
#endif

// Version
#define SKA_VERSION_MAJOR 0
#define SKA_VERSION_MINOR 1
#define SKA_VERSION_PATCH 0

// Forward declarations
typedef struct ska_window_t ska_window_t;
typedef        uint32_t     ska_window_id_t;

// ============================================================================
// Memory Allocators
// ============================================================================

// Custom memory allocator function signatures.
// If provided, all sk_app memory operations will use these functions.
typedef void* (*ska_alloc_fn)(size_t size, void* user_data);
typedef void* (*ska_realloc_fn)(void* ptr, size_t size, void* user_data);
typedef void  (*ska_free_fn)(void* ptr, void* user_data);

// Which display server the Linux backend should use.
// Ignored on every other platform.
typedef enum ska_linux_backend_ {
	ska_linux_backend_auto    = 0, // Prefer Wayland, fall back to X11 (default)
	ska_linux_backend_wayland = 1, // Require Wayland; ska_init fails if unavailable
	ska_linux_backend_x11     = 2, // Require X11; ska_init fails if unavailable
} ska_linux_backend_;

// Settings for ska_init
typedef struct ska_settings_t {
	ska_alloc_fn   alloc;                  // Custom allocator (NULL for default malloc)
	ska_realloc_fn realloc;                // Custom reallocator (NULL for default realloc)
	ska_free_fn    free;                   // Custom free (NULL for default free)
	void*          alloc_user_data;        // User data passed to all allocator calls
	bool           external_frame_driver;  // An external loop drives frames instead of ska_run(), suppresses the web blocking-loop detector (web only, see ska_run)

	// Display server preference (Linux only, ignored elsewhere). Zero is
	// ska_linux_backend_auto, so zero-initialized settings get the default.
	// The SKA_VIDEODRIVER environment variable ("wayland" or "x11") overrides
	// this, which lets an unmodified binary be tested against either backend.
	ska_linux_backend_ linux_backend;

	// Stable application identifier, used where the OS matches windows to an
	// application rather than showing text: the Wayland xdg app_id and the X11
	// WM_CLASS, which are how a window finds its .desktop file and icon
	// (StartupWMClass). NULL falls back to each window's title.
	const char* app_id;
} ska_settings_t;

// ============================================================================
// Initialization
// ============================================================================

// Initialize the sk_app library.
// Initializes platform-specific subsystems (X11/Win32/etc), event queue, and input state.
// Can be called multiple times safely (returns error if already initialized).
// On Android, preserves the android_app pointer set by ska_android_set_app().
//
// @param opt_settings Optional settings for custom allocators, etc. Pass NULL for defaults.
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_init(const ska_settings_t* opt_settings);

// Shutdown the sk_app library.
// Automatically destroys any remaining windows, then cleans up platform resources.
// Safe to call even if not initialized (no-op).
SKA_API void ska_shutdown(void);

// Get the last error message.
// Returned pointer is valid until the next error occurs or ska_shutdown() is called.
// Thread-local storage is not used, so not thread-safe in multi-threaded contexts.
//
// @return Error message string (UTF-8), or NULL if no error occurred
SKA_API const char* ska_error_get(void);

// ============================================================================
// Main Loop
// ============================================================================

// Per-frame callback for ska_run().
// Return true to keep running, false to end the loop.
typedef bool (*ska_frame_fn)(void* user_data);

// Run the application's main loop, calling frame once per frame.
//
// This is the portable replacement for a hand-written `while` loop: on native
// platforms it is exactly `while (frame(user_data)) {}`, but on the web the
// browser owns the event loop, so the frame callback is driven by
// requestAnimationFrame via emscripten_set_main_loop instead. Apps that want
// to run everywhere should structure their main loop as a frame callback and
// call this.
//
// Hand-written `while` loops remain fully supported on native platforms. In
// WASM builds they cannot work (the browser never regains control to deliver
// input or paint), so sk_app detects a blocking loop at runtime and raises an
// error directing you here instead of silently freezing the tab.
//
// Platform behavior:
// - Native: returns once frame returns false. No pacing is applied; pace with
//   vsync/present in your renderer, or ska_time_sleep() for windowing-only apps.
// - Web: does NOT return. The browser paces frames (requestAnimationFrame),
//   and ska_run() unwinds the stack instead of returning, so code after the
//   call never executes. When frame returns false the loop is cancelled, but
//   the page keeps running. Put cleanup in the frame callback if you need it.
//
// @param frame Called once per frame (required, not NULL)
// @param user_data Passed through to every frame call (can be NULL)
SKA_API void ska_run(ska_frame_fn frame, void* user_data);

// ============================================================================
// Window Management
// ============================================================================

// Window flags
//
// DPI is not opt-in: every window renders at the display's native resolution.
// Window sizes are always screen coordinates, so on a scaled display a window
// covers more pixels than the number given. Query
// ska_window_get_drawable_size for the real framebuffer size and
// ska_window_get_dpi_scale for the factor to raster fonts and UI by;
// ska_event_window_dpi_changed and ska_event_window_resized both mean those
// answers moved.
typedef enum ska_window_ {
	ska_window_resizable      = 0x00000001,
	ska_window_borderless     = 0x00000002,
	ska_window_maximized      = 0x00000004,
	ska_window_minimized      = 0x00000008,
	ska_window_hidden         = 0x00000010,
	ska_window_fullscreen     = 0x00000020,
	ska_window_always_on_top  = 0x00000040,
} ska_window_;

// Window position constants
#define SKA_WINDOWPOS_UNDEFINED ((int32_t)0x1FFF0000)
#define SKA_WINDOWPOS_CENTERED  ((int32_t)0x2FFF0000)

// Rectangle structure
typedef struct ska_rect_t {
	int32_t x;
	int32_t y;
	int32_t w;
	int32_t h;
} ska_rect_t;

// Create a new window.
// Window is initially visible unless ska_window_hidden flag is set.
// Defaults to "sk_app window" if title is NULL, 640x480 if dimensions <= 0.
// SKA_WINDOWPOS_UNDEFINED maps to (100,100), SKA_WINDOWPOS_CENTERED is platform-centered.
// Maximum of SKA_MAX_WINDOWS (16) can be created simultaneously.
//
// @param title Window title (UTF-8), copied internally
// @param x X position in screen coordinates (or SKA_WINDOWPOS_UNDEFINED/SKA_WINDOWPOS_CENTERED)
// @param y Y position in screen coordinates (or SKA_WINDOWPOS_UNDEFINED/SKA_WINDOWPOS_CENTERED)
// @param width Window width in screen coordinates (not pixels on high-DPI)
// @param height Window height in screen coordinates (not pixels on high-DPI)
// @param flags Bitwise OR of ska_window_ flags
// @return Window handle, or NULL on failure (check ska_error_get())
SKA_API ska_window_t* ska_window_create(
	const char* title,
	int32_t x, int32_t y,
	int32_t width, int32_t height,
	uint32_t flags
);

// Destroy a window.
// Frees platform resources (HWND/Window/etc) and internal memory.
// Safe to pass NULL (no-op). Window handle becomes invalid after this call.
//
// @param ref_window Window to destroy
SKA_API void ska_window_destroy(ska_window_t* ref_window);

// Get window ID (for event handling).
// IDs are stable for the lifetime of the window and used in event structures.
// Returns 0 if window is NULL.
//
// @param window Window handle
// @return Unique window ID (never reused during program lifetime)
SKA_API ska_window_id_t ska_window_get_id(const ska_window_t* window);

// Get window from ID.
// Performs linear search through active windows (max 16), so O(n) complexity.
//
// @param id Window ID from an event
// @return Window handle, or NULL if window was destroyed or ID is invalid
SKA_API ska_window_t* ska_window_from_id(ska_window_id_t id);

// Set window title.
// Title string is copied internally. Safe to pass NULL for either parameter (no-op).
//
// @param ref_window Window handle
// @param title New title (UTF-8)
SKA_API void ska_window_set_title(ska_window_t* ref_window, const char* title);

// Get window title.
// Returns pointer to internally stored title string.
//
// @param window Window handle
// @return Window title (UTF-8), valid until ska_window_set_title() or ska_window_destroy()
SKA_API const char* ska_window_get_title(const ska_window_t* window);

// ============================================================================
// Window Frame Position/Size (includes title bar and borders)
// ============================================================================

// Set window frame position.
// Positions the entire window including title bar and borders.
// May not take effect immediately on some platforms (window managers can override).
// No-op on Wayland, which gives clients no control over placement and no way to
// read back where the compositor put them; the getters report 0 and
// ska_event_window_moved never fires.
// No-op on Android when using a Service context (no window to position).
//
// @param ref_window Window handle
// @param x New X position of frame's top-left corner in screen coordinates
// @param y New Y position of frame's top-left corner in screen coordinates
SKA_API void ska_window_set_frame_position(ska_window_t* ref_window, int32_t x, int32_t y);

// Get window frame position.
// Returns the position of the entire window including title bar and borders.
//
// @param window Window handle
// @param opt_out_x Output X position of frame (can be NULL)
// @param opt_out_y Output Y position of frame (can be NULL)
SKA_API void ska_window_get_frame_position(const ska_window_t* window, int32_t* opt_out_x, int32_t* opt_out_y);

// Set window frame size.
// Sets the size of the entire window including title bar and borders.
// Triggers ska_event_window_resized when content size actually changes.
// No-op on Android when using a Service context (no window to resize).
//
// @param ref_window Window handle
// @param width New width of entire frame in screen coordinates
// @param height New height of entire frame in screen coordinates
SKA_API void ska_window_set_frame_size(ska_window_t* ref_window, int32_t width, int32_t height);

// Get window frame size.
// Returns the size of the entire window including title bar and borders.
//
// @param window Window handle
// @param opt_out_width Output width of frame (can be NULL)
// @param opt_out_height Output height of frame (can be NULL)
SKA_API void ska_window_get_frame_size(const ska_window_t* window, int32_t* opt_out_width, int32_t* opt_out_height);

// ============================================================================
// Window Content Position/Size (client area, excludes decorations)
// ============================================================================

// Set window content position.
// Positions the window so that the content area's top-left is at (x, y).
// The frame will be positioned above/left of this point to accommodate decorations.
//
// @param ref_window Window handle
// @param x New X position of content area in screen coordinates
// @param y New Y position of content area in screen coordinates
SKA_API void ska_window_set_content_position(ska_window_t* ref_window, int32_t x, int32_t y);

// Get window content position.
// Returns the position of the content area (excludes title bar and borders).
// This is where your rendered content actually appears on screen.
//
// @param window Window handle
// @param opt_out_x Output X position of content area (can be NULL)
// @param opt_out_y Output Y position of content area (can be NULL)
SKA_API void ska_window_get_content_position(const ska_window_t* window, int32_t* opt_out_x, int32_t* opt_out_y);

// Set window content size.
// Sets the size of the content area (excludes title bar and borders).
// Triggers ska_event_window_resized when size actually changes.
//
// @param ref_window Window handle
// @param width New content width in screen coordinates
// @param height New content height in screen coordinates
SKA_API void ska_window_set_content_size(ska_window_t* ref_window, int32_t width, int32_t height);

// Get window content size.
// Returns the size of the content area (excludes title bar and borders).
// This is the logical size, not the physical pixel size.
//
// @param window Window handle
// @param opt_out_width Output content width (can be NULL)
// @param opt_out_height Output content height (can be NULL)
SKA_API void ska_window_get_content_size(const ska_window_t* window, int32_t* opt_out_width, int32_t* opt_out_height);

// Get window drawable size in pixels (may differ from content size on high-DPI).
// Use this for framebuffer/viewport sizing in rendering code.
// On standard DPI displays, this equals ska_window_get_content_size().
// On high-DPI displays (Retina, etc), this is typically 2x the content size.
//
// @param ref_window Window handle
// @param opt_out_width Output width in pixels (can be NULL)
// @param opt_out_height Output height in pixels (can be NULL)
SKA_API void ska_window_get_drawable_size(ska_window_t* ref_window, int32_t* opt_out_width, int32_t* opt_out_height);

// Get the DPI scale factor for a window.
// Returns the OS-level UI scaling factor (e.g., 1.0 for 100%, 1.5 for 150%, 2.0 for 200%).
// This is useful for scaling UI elements like fonts to match the user's display preferences.
// Note: This is different from DisplayFramebufferScale which handles pixel density.
//
// Platform behavior:
// - Linux X11: Reads Xft.dpi from Xresources, falls back to 96 DPI as baseline
// - Win32: Uses GetDpiForWindow() or GetDpiForMonitor(), baseline is 96 DPI
// - macOS: Returns 1.0 (macOS handles scaling transparently via backingScaleFactor)
// - Android: Uses display density from configuration
//
// @param window Window handle
// @return DPI scale factor (1.0 = 100% scale, 1.5 = 150%, etc.), returns 1.0 on error
SKA_API float ska_window_get_dpi_scale(const ska_window_t* window);

// Get the refresh rate of the monitor displaying the window.
// Returns the refresh rate in Hz (e.g., 60.0, 144.0, 59.94).
// On multi-monitor setups, returns the rate of the monitor where the window
// is primarily displayed.
//
// Platform notes:
// - Linux/X11: Uses XRandR to query the current CRTC refresh rate
// - Win32: Uses EnumDisplaySettings on the monitor containing the window
// - macOS: Uses CGDisplayModeGetRefreshRate on the window's screen
// - Android: Uses JNI to call Display.getRefreshRate(). Returns 0 from a
//   Service context (requires Activity for getWindowManager()).
//
// @param window Window handle
// @return Refresh rate in Hz, or 0.0f if unavailable
SKA_API float ska_window_get_refresh_rate(const ska_window_t* window);

// Show window.
// Maps the window to the display. Generates ska_event_window_shown.
//
// @param ref_window Window handle
SKA_API void ska_window_show(ska_window_t* ref_window);

// Hide window.
// Unmaps the window from display. Generates ska_event_window_hidden.
//
// @param ref_window Window handle
SKA_API void ska_window_hide(ska_window_t* ref_window);

// Maximize window.
// Requests window manager to maximize window (fills screen but keeps taskbar/decorations).
// May not be honored on all platforms or by all window managers.
//
// @param ref_window Window handle
SKA_API void ska_window_maximize(ska_window_t* ref_window);

// Minimize window.
// Iconifies window to taskbar/dock. Generates ska_event_window_minimized.
//
// @param ref_window Window handle
SKA_API void ska_window_minimize(ska_window_t* ref_window);

// Restore window from maximized/minimized state.
// Returns window to normal size and visibility. Generates ska_event_window_restored.
//
// @param ref_window Window handle
SKA_API void ska_window_restore(ska_window_t* ref_window);

// Set or clear fullscreen state on a window.
// This is a request, not a guarantee: on Linux/X11 it asks the window
// manager for _NET_WM_STATE_FULLSCREEN, on the web browsers defer it until
// a user gesture. Watch ska_window_get_fullscreen for the result; when
// granted, the window resizes to cover its output and a
// ska_event_window_resized follows. A fullscreen window that exactly covers
// one output allows Wayland compositors to scan it out directly, skipping
// the composite pass (lower latency, no compositor GPU contention).
// Equivalent to the ska_window_fullscreen creation flag. On macOS this is
// the native fullscreen Space, with its animated transition.
// Not yet implemented on Windows; Android windows are always fullscreen.
//
// @param ref_window Window handle
// @param fullscreen true to enter fullscreen, false to return to windowed
SKA_API void ska_window_set_fullscreen(ska_window_t* ref_window, bool fullscreen);

// Get the window's live fullscreen state, as reported by the platform.
// This tracks reality rather than the last request: it flips once a
// ska_window_set_fullscreen request is actually granted, and also follows
// fullscreen changes made from outside the app, like a window manager
// shortcut. Stays false where set_fullscreen is unimplemented
// (Windows); always true on Android.
//
// @param window Window handle
// @return true if the window currently covers its output
SKA_API bool ska_window_get_fullscreen(const ska_window_t* window);

// Raise window above other windows.
// Brings window to front and gives it input focus.
// On X11, also calls XSetInputFocus() to ensure keyboard events are received.
//
// @param ref_window Window handle
SKA_API void ska_window_raise(ska_window_t* ref_window);

// Get window flags.
// Returns the flags passed to ska_window_create().
// Note: flags are not updated when window state changes (e.g., user maximizes window).
//
// @param window Window handle
// @return Creation flags (ska_window_), or 0 if window is NULL
SKA_API uint32_t ska_window_get_flags(const ska_window_t* window);

// ============================================================================
// Event System
// ============================================================================

// Event types
typedef enum ska_event_ {
	ska_event_none = 0,

	// Application events
	ska_event_quit,
	ska_event_app_lowmemory,
	ska_event_app_background,
	ska_event_app_foreground,

	// Window events
	ska_event_window_shown,
	ska_event_window_hidden,
	ska_event_window_moved,
	ska_event_window_resized,
	ska_event_window_minimized,
	ska_event_window_maximized,
	ska_event_window_restored,
	ska_event_window_mouse_enter,
	ska_event_window_mouse_leave,
	ska_event_window_focus_gained,
	ska_event_window_focus_lost,
	ska_event_window_close,
	ska_event_window_dpi_changed, // DPI/scale factor changed (e.g., moved to different monitor)

	// Keyboard events
	ska_event_key_down,
	ska_event_key_up,
	ska_event_text_input,

	// Mouse events
	ska_event_mouse_motion,
	ska_event_mouse_button_down,
	ska_event_mouse_button_up,
	ska_event_mouse_wheel,

	// File dialog events
	ska_event_file_dialog,
} ska_event_;

// Keyboard scancodes (physical keys)
typedef enum ska_scancode_ {
	ska_scancode_unknown = 0,

	// Letters
	ska_scancode_a = 4,
	ska_scancode_b, ska_scancode_c, ska_scancode_d, ska_scancode_e,
	ska_scancode_f, ska_scancode_g, ska_scancode_h, ska_scancode_i,
	ska_scancode_j, ska_scancode_k, ska_scancode_l, ska_scancode_m,
	ska_scancode_n, ska_scancode_o, ska_scancode_p, ska_scancode_q,
	ska_scancode_r, ska_scancode_s, ska_scancode_t, ska_scancode_u,
	ska_scancode_v, ska_scancode_w, ska_scancode_x, ska_scancode_y,
	ska_scancode_z,

	// Numbers
	ska_scancode_1, ska_scancode_2, ska_scancode_3, ska_scancode_4,
	ska_scancode_5, ska_scancode_6, ska_scancode_7, ska_scancode_8,
	ska_scancode_9, ska_scancode_0,

	// Function keys
	ska_scancode_return,
	ska_scancode_escape,
	ska_scancode_backspace,
	ska_scancode_tab,
	ska_scancode_space,

	// Symbols
	ska_scancode_minus,
	ska_scancode_equals,
	ska_scancode_leftbracket,
	ska_scancode_rightbracket,
	ska_scancode_backslash,
	ska_scancode_semicolon,
	ska_scancode_apostrophe,
	ska_scancode_grave,
	ska_scancode_comma,
	ska_scancode_period,
	ska_scancode_slash,

	ska_scancode_capslock,

	ska_scancode_f1, ska_scancode_f2, ska_scancode_f3, ska_scancode_f4,
	ska_scancode_f5, ska_scancode_f6, ska_scancode_f7, ska_scancode_f8,
	ska_scancode_f9, ska_scancode_f10, ska_scancode_f11, ska_scancode_f12,

	ska_scancode_printscreen,
	ska_scancode_scrolllock,
	ska_scancode_pause,
	ska_scancode_insert,

	// Navigation
	ska_scancode_home,
	ska_scancode_pageup,
	ska_scancode_delete,
	ska_scancode_end,
	ska_scancode_pagedown,
	ska_scancode_right,
	ska_scancode_left,
	ska_scancode_down,
	ska_scancode_up,

	// Modifiers
	ska_scancode_lctrl = 224,
	ska_scancode_lshift,
	ska_scancode_lalt,
	ska_scancode_lgui,
	ska_scancode_rctrl,
	ska_scancode_rshift,
	ska_scancode_ralt,
	ska_scancode_rgui,

	ska_scancode_count = 512
} ska_scancode_;

// Key modifiers
typedef enum ska_keymod_ {
	ska_keymod_none   = 0x0000,
	ska_keymod_lshift = 0x0001,
	ska_keymod_rshift = 0x0002,
	ska_keymod_shift  = 0x0003,
	ska_keymod_lctrl  = 0x0040,
	ska_keymod_rctrl  = 0x0080,
	ska_keymod_ctrl   = 0x00C0,
	ska_keymod_lalt   = 0x0100,
	ska_keymod_ralt   = 0x0200,
	ska_keymod_alt    = 0x0300,
	ska_keymod_lgui   = 0x0400,
	ska_keymod_rgui   = 0x0800,
	ska_keymod_gui    = 0x0C00,
} ska_keymod_;

// Mouse buttons
typedef enum ska_mouse_button_ {
	ska_mouse_button_left   = 1,
	ska_mouse_button_middle = 2,
	ska_mouse_button_right  = 3,
	ska_mouse_button_x1     = 4,
	ska_mouse_button_x2     = 5,
} ska_mouse_button_;

// Event structures
//
// Window event data interpretation by event type:
// - ska_event_window_resized:    data1 = new width,  data2 = new height
// - ska_event_window_moved:      data1 = new x,      data2 = new y
// - ska_event_window_dpi_changed: data1 = new scale percentage (e.g., 150 = 1.5x)
typedef struct ska_event_window_t {
	ska_window_id_t   window_id;
	int32_t           data1;
	int32_t           data2;
} ska_event_window_t;

typedef struct ska_event_keyboard_t {
	ska_window_id_t   window_id;
	bool              pressed;
	bool              repeat;
	ska_scancode_     scancode;
	uint16_t          modifiers;
} ska_event_keyboard_t;

typedef struct ska_event_text_t {
	ska_window_id_t   window_id;
	char              text[32];  // UTF-8 text
} ska_event_text_t;

typedef struct ska_event_mouse_motion_t {
	ska_window_id_t   window_id;
	int32_t           x;
	int32_t           y;
	int32_t           xrel;
	int32_t           yrel;
} ska_event_mouse_motion_t;

typedef struct ska_event_mouse_button_t {
	ska_window_id_t   window_id;
	ska_mouse_button_ button;
	bool              pressed;
	uint8_t           clicks;
	int32_t           x;
	int32_t           y;
} ska_event_mouse_button_t;

typedef struct ska_event_mouse_wheel_t {
	ska_window_id_t   window_id;
	int32_t           x;
	int32_t           y;
	float             precise_x;
	float             precise_y;
} ska_event_mouse_wheel_t;

// File dialog types
typedef uint32_t ska_file_dialog_id_t;

typedef enum ska_file_dialog_ {
	ska_file_dialog_open,           // Open existing file(s)
	ska_file_dialog_save,           // Save/create new file
	ska_file_dialog_open_folder,    // Pick a folder (desktop only, no-op on Android)
} ska_file_dialog_;

typedef struct ska_event_file_dialog_t {
	ska_file_dialog_id_t  id;           // Matches ska_file_dialog_show() return value
	const char*           title;        // Title passed to ska_file_dialog_show()
	bool                  cancelled;    // true if user cancelled/dismissed the dialog
	int32_t               count;        // Number of selected paths (0 if cancelled)
	void*                 _internal;    // Internal data, do not access directly
} ska_event_file_dialog_t;

// Main event structure
typedef struct ska_event_t {
	ska_event_ type;
	uint32_t   timestamp;
	union {
		ska_event_window_t       window;
		ska_event_keyboard_t     keyboard;
		ska_event_text_t         text;
		ska_event_mouse_motion_t mouse_motion;
		ska_event_mouse_button_t mouse_button;
		ska_event_mouse_wheel_t  mouse_wheel;
		ska_event_file_dialog_t  file_dialog;
	};
} ska_event_t;

// Poll for events.
// Pumps platform events first, then dequeues from internal ring buffer (256 events max).
// Text input events are automatically pushed to the text queue for ska_text_consume().
// Non-blocking: returns immediately if queue is empty.
//
// Web note: polling from a blocking `while` main loop freezes the tab, so
// WASM builds detect that pattern and raise a runtime error; drive your loop
// with ska_run() instead. Polling from inside a ska_run frame (or any other
// callback) is fine.
//
// @param out_event Pointer to event structure to fill (required, not NULL)
// @return true if event was retrieved, false if no events available
SKA_API bool ska_event_poll(ska_event_t* out_event);

// Wait for an event (blocks until event is available).
// Equivalent to ska_event_wait_timeout(out_event, -1).
// Polls with 1ms sleep intervals, so CPU-friendly but not perfectly responsive.
//
// @param out_event Pointer to event structure to fill (required, not NULL)
// @return true if event was retrieved, false on error
SKA_API bool ska_event_wait(ska_event_t* out_event);

// Wait for an event with timeout.
// Busy-waits with ska_event_poll() + ska_time_sleep(1ms) until event or timeout.
// timeout_ms=0 is equivalent to ska_event_poll(), timeout_ms=-1 waits forever.
//
// @param out_event Pointer to event structure to fill (required, not NULL)
// @param timeout_ms Timeout in milliseconds (0 = poll only, -1 = wait forever)
// @return true if event was retrieved, false if timeout expired or error
SKA_API bool ska_event_wait_timeout(ska_event_t* out_event, int32_t timeout_ms);

// ============================================================================
// Input State Query
// ============================================================================

// Get keyboard state snapshot.
// Returns pointer to internal array indexed by ska_scancode_ values.
// Array size is ska_scancode_count (512), populated by key events during ska_event_poll().
// Pointer is valid for program lifetime (points to global state), not just until next poll.
//
// @param opt_out_num_keys If not NULL, receives ska_scancode_count (512)
// @return Array of key states (1 = pressed, 0 = released), never NULL
SKA_API const uint8_t* ska_keyboard_get_state(int32_t* opt_out_num_keys);

// Get current keyboard modifiers.
// Bitmask updated by key events, includes both left/right variants and combined flags.
//
// @return Current modifier flags (ska_keymod_), bitwise OR of active modifiers
SKA_API uint16_t ska_keyboard_get_modifiers(void);

// Get mouse position relative to window.
// Position updated by mouse motion events during ska_event_poll().
//
// @param opt_out_x Output X position in window coordinates (can be NULL)
// @param opt_out_y Output Y position in window coordinates (can be NULL)
// @return Button state bitmask: bit N set if button (N+1) is pressed (e.g., bit 0 = left button)
SKA_API uint32_t ska_mouse_get_state(int32_t* opt_out_x, int32_t* opt_out_y);

// Get global mouse position (desktop coordinates).
// Currently not implemented: returns same as ska_mouse_get_state() (window-relative position).
// Reserved for future platform-specific implementation (would query OS directly).
//
// @param opt_out_x Output X position in desktop coordinates (can be NULL)
// @param opt_out_y Output Y position in desktop coordinates (can be NULL)
// @return Button state bitmask
SKA_API uint32_t ska_mouse_get_global_state(int32_t* opt_out_x, int32_t* opt_out_y);

// System cursor shapes
typedef enum ska_system_cursor_ {
	ska_system_cursor_arrow = 0,
	ska_system_cursor_ibeam,
	ska_system_cursor_wait,
	ska_system_cursor_crosshair,
	ska_system_cursor_waitarrow,
	ska_system_cursor_sizenwse,
	ska_system_cursor_sizenesw,
	ska_system_cursor_sizewe,
	ska_system_cursor_sizens,
	ska_system_cursor_sizeall,
	ska_system_cursor_no,
	ska_system_cursor_hand,
	ska_system_cursor_count_
} ska_system_cursor_;

// Set mouse cursor to a system cursor shape.
// Changes the cursor appearance for all windows.
// Platform support: Win32, X11. On Android, this is a no-op.
//
// @param cursor System cursor shape to set
SKA_API void ska_cursor_set(ska_system_cursor_ cursor);

// Show or hide mouse cursor.
// On X11, creates invisible cursor from 1x1 transparent pixmap when hiding.
// Affects all windows created by this library.
//
// @param show true to show cursor, false to hide
SKA_API void ska_cursor_show(bool show);

// Enable or disable relative mouse mode (for FPS games, etc).
// In relative mode, cursor is hidden and motion is not clamped to window bounds.
// Motion arrives as xrel/yrel on ska_event_mouse_motion, unaccelerated, while
// the absolute position stays put. This is the only pointer-capture mechanism:
// there is deliberately no pointer warp, which Wayland and browsers cannot do.
//
// @param enabled true to enable, false to disable
// @return true on success, false on failure
SKA_API bool ska_mouse_set_relative_mode(bool enabled);

// Get mouse motion accumulated since the last call to this function, and clear
// it. Suits a per-frame poller, where several motion events can land between
// reads. In relative mode this is the unaccelerated delta and the absolute
// position does not move, so it is the only motion source there.
//
// @param opt_out_x Accumulated horizontal motion, may be NULL
// @param opt_out_y Accumulated vertical motion, may be NULL
SKA_API void ska_mouse_get_delta(int32_t* opt_out_x, int32_t* opt_out_y);

// Get relative mouse mode state.
// Returns the state set by ska_mouse_set_relative_mode().
//
// @return true if relative mode is enabled, false otherwise
SKA_API bool ska_mouse_get_relative_mode(void);

// ============================================================================
// Vulkan Support
// ============================================================================

// Get required Vulkan instance extensions for the platform.
// Returns platform-specific extensions (e.g., VK_KHR_surface + VK_KHR_xlib_surface on X11).
//
// @param out_count Output number of extensions (required, not NULL)
// @return Array of extension name strings (static lifetime), or NULL on error
SKA_API const char** ska_vk_get_instance_extensions(uint32_t* out_count);

// Create Vulkan surface for a window.
// Dynamically loads vkCreateXlibSurfaceKHR (or platform equivalent) via vkGetInstanceProcAddr.
// Requires instance created with extensions from ska_vk_get_instance_extensions().
//
// @param window Window handle (required, not NULL)
// @param instance Vulkan instance handle (VkInstance cast to void*)
// @param out_surface Output Vulkan surface (VkSurfaceKHR* cast to void*), written on success
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_vk_create_surface(
	const ska_window_t* window,
	void* instance,
	void* out_surface
);

// ============================================================================
// WebGPU Support
// ============================================================================

// Create a WebGPU surface for a window.
// Implemented on every platform, not just web: native builds fill in the
// Win32/Xlib/Metal/ANativeWindow WGPUSurfaceDescriptor chain (for use with
// Dawn or wgpu-native), and web builds use the canvas-selector descriptor.
//
// sk_app does not link a WebGPU implementation itself; it declares
// wgpuInstanceCreateSurface and expects the application to link one (Dawn,
// wgpu-native, or Emscripten's emdawnwebgpu port). The descriptor layout
// matches the official webgpu-headers, as shipped by all three.
//
// @param window Window handle (required, not NULL)
// @param instance WebGPU instance handle (WGPUInstance cast to void*)
// @param out_surface Output surface (WGPUSurface* cast to void*), written on success
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_wgpu_create_surface(
	const ska_window_t* window,
	void* instance,
	void* out_surface
);

// ============================================================================
// Platform-Specific Window Handles
// ============================================================================

// Get platform-specific window handle.
// Returns the underlying OS window handle for interop with other libraries.
// Cast to appropriate type based on platform.
//
// Win32: HWND (cast from void*)
// Linux X11: Window (cast to unsigned long via uintptr_t)
// Linux Wayland: struct wl_surface*
// macOS: NSWindow* (id type)
// Android: ANativeWindow*
// Web: CSS selector string for the window's canvas (const char*)
//
// @param window Window handle
// @return Platform-specific handle, or NULL if window is NULL
SKA_API void* ska_window_get_native_handle(const ska_window_t* window);

#ifdef SKA_PLATFORM_WIN32
// Get Win32 HINSTANCE.
// Returns the module handle used for window class registration.
//
// @return HINSTANCE handle (cast to void*)
SKA_API void* ska_win32_get_hinstance(void);
#endif

#ifdef SKA_PLATFORM_LINUX
// Get X11 Display pointer (if using X11).
// Returns the display connection shared by all windows.
//
// @return Display* (cast to void*), or NULL when the Wayland backend is active
SKA_API void* ska_linux_get_x11_display(void);

// Get Wayland display pointer (if using Wayland).
// Returns the connection shared by all windows.
//
// @return struct wl_display* (cast to void*), or NULL when X11 is active
SKA_API void* ska_linux_get_wayland_display(void);

// Get the display server backend actually in use, which may differ from the
// ska_settings_t preference (auto-selection, or an SKA_VIDEODRIVER override).
// Never returns ska_linux_backend_auto once ska_init has succeeded.
//
// Callers need this to interpret ska_window_get_native_handle(), whose return
// type differs per backend: an X11 Window id under X11, a wl_surface* under
// Wayland.
//
// @return The resolved backend, or ska_linux_backend_auto if not initialized
SKA_API ska_linux_backend_ ska_linux_get_backend(void);
#endif

#ifdef SKA_PLATFORM_WEB
// Get the CSS selector for the window's canvas element (e.g. "#canvas").
// The first window binds to the page's default canvas (Module.canvas / #canvas)
// when one exists; additional windows create their own canvas elements.
// Useful for handing the canvas to other web APIs.
//
// @param window Window handle
// @return Selector string (valid for the window's lifetime), or NULL
SKA_API const char* ska_web_get_canvas_selector(const ska_window_t* window);
#endif

#ifdef SKA_PLATFORM_ANDROID
// Set Android app pointer (standalone mode).
// Links the android_native_app_glue struct for native window and event handling.
// Called automatically by sk_app_entrypoint; you only call this if providing
// your own android_main().
//
// @param app android_app* from android_native_app_glue (cast to void*)
SKA_API void ska_android_set_app(void* app);

// Set Android Context and optional JavaVM for JNI operations (library mode).
// Accepts any Context subclass: Activity, Service, Application, etc.
// In standalone mode this is called automatically from the glue Activity.
// In library mode the host must call this before ska_init().
//
// @param context jobject for any android.content.Context (cast to void*)
// @param java_vm JavaVM* (cast to void*), or NULL to use auto-discovery
SKA_API void ska_android_set_context(void* context, void* java_vm);

// Get the JavaVM pointer for JNI operations.
// Uses JNI_GetCreatedJavaVMs() internally — works in both standalone and library mode.
//
// @return JavaVM* (cast to void*), or NULL if no JVM
SKA_API void* ska_android_get_vm(void);

// Get the stored Context jobject.
// Returns whatever was passed to ska_android_set_context() (or the NativeActivity
// in standalone mode). Usable for JNI calls to Java APIs.
//
// @return jobject (cast to void*), or NULL if not set
SKA_API void* ska_android_get_activity(void);

// Get a JNIEnv* for the calling thread.
// Calls AttachCurrentThread internally (no-op if already attached).
//
// @return JNIEnv* (cast to void*), or NULL
SKA_API void* ska_android_get_jni_env(void);

// ---- Lifecycle / Input Injection (library mode) ----
// These functions let an external host (C#, Java, etc.) push Android lifecycle
// and input events into sk_app's event queue. In standalone mode the glue
// callbacks call these internally.

typedef enum ska_android_event_ {
	ska_android_event_resume,
	ska_android_event_pause,
	ska_android_event_destroy,
	ska_android_event_focus_gained,
	ska_android_event_focus_lost,
	ska_android_event_low_memory,
} ska_android_event_;

// Push a lifecycle event.
SKA_API void ska_android_on_event(ska_android_event_ event);

// Notify that the native window has been created. Triggers ska_event_window_shown.
// @param native_window ANativeWindow* (cast to void*)
SKA_API void ska_android_on_window_created(void *native_window);

// Notify that the native window has been destroyed. Triggers ska_event_window_hidden.
SKA_API void ska_android_on_window_destroyed(void);

// Notify that the window has been resized (freeform / DEX mode).
SKA_API void ska_android_on_window_resized(int32_t width, int32_t height);

// Push a Java MotionEvent or KeyEvent into the input queue.
// Extracts action/coordinates/keycode internally via JNI.
// @param java_input_event jobject for MotionEvent or KeyEvent (cast to void*)
// @return true if the event was consumed
SKA_API bool ska_android_on_input(void *java_input_event);
#endif

// ============================================================================
// Text Input Queue
// ============================================================================

// Check if text input is available in the queue.
// Queue is populated automatically from ska_event_text_input events during ska_event_poll().
//
// @return true if characters are available to consume, false if queue is empty
SKA_API bool ska_text_has_input(void);

// Consume one Unicode character from the text input queue.
// Converts UTF-8 text from ska_event_text_input events to UTF-32 codepoints.
// Ring buffer holds up to 256 codepoints; older input is dropped if buffer fills.
//
// @return Unicode codepoint (UTF-32), or 0 if queue is empty
SKA_API uint32_t ska_text_consume(void);

// Peek at the next Unicode character without consuming it.
// Useful for lookahead without removing the character from the queue.
//
// @return Unicode codepoint (UTF-32), or 0 if queue is empty
SKA_API uint32_t ska_text_peek(void);

// Clear the text input queue.
// Discards all pending text input. Useful when switching input contexts.
SKA_API void ska_text_reset(void);

// ============================================================================
// Virtual Keyboard (Mobile)
// ============================================================================

// Text input context hints for virtual keyboard
typedef enum ska_text_input_type_ {
	ska_text_input_type_text = 0,      // Normal text
	ska_text_input_type_number,        // Numeric input
	ska_text_input_type_phone,         // Phone number
	ska_text_input_type_email,         // Email address
	ska_text_input_type_url,           // URL
	ska_text_input_type_password,      // Password (masked)
} ska_text_input_type_;

// Show or hide virtual keyboard (mobile platforms).
// On Android, hints to OS what keyboard layout to show (email, number pad, etc).
// Requires an Activity context — no-op from an Android Service (no window for
// keyboard focus).
// On desktop (Linux X11, Win32, macOS), this is a no-op (always false).
//
// @param visible true to show, false to hide
// @param type Type of text input expected (only used when showing on Android)
SKA_API void ska_virtual_keyboard_show(bool visible, ska_text_input_type_ type);

// Check if virtual keyboard is currently visible.
// Returns the state set by ska_virtual_keyboard_show().
//
// @return true if virtual keyboard is shown (Android only), always false on desktop
SKA_API bool ska_virtual_keyboard_is_visible(void);

// ============================================================================
// File I/O Utilities
// ============================================================================

// Read entire file into memory.
// Allocates buffer with malloc(); caller must free with ska_file_free_data().
// Binary mode (no newline translation). Uses fseek/ftell for size, so not suitable for pipes.
//
// @param filename Path to file (UTF-8)
// @param out_data Output pointer to file data (required, not NULL), receives malloc'd buffer
// @param out_size Output size in bytes (can be NULL if you don't need size)
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_file_read(const char* filename, void** out_data, size_t* out_size);

// Read text file into a null-terminated string.
// Calls ska_file_read() then reallocs to add '\0' terminator.
// Caller must free the returned string with ska_file_free_data().
//
// @param filename Path to file (UTF-8)
// @param out_text Output pointer to null-terminated string (required, not NULL)
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_file_read_text(const char* filename, char** out_text);

// Read asset file into memory.
// On Android: Uses AAssetManager to read from APK assets folder.
// On other platforms: Looks for file in "assets/" or "Assets/" folder relative to executable.
// Allocates buffer with malloc(); caller must free with ska_file_free_data().
//
// @param asset_name Asset path relative to assets folder (UTF-8), e.g. "shaders/vert.spv"
// @param out_data Output pointer to file data (required, not NULL), receives malloc'd buffer
// @param out_size Output size in bytes (can be NULL if you don't need size)
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_asset_read(const char* asset_name, void** out_data, size_t* out_size);

// Get the size of an asset in bytes without reading its contents.
// Follows the same lookup order as ska_asset_read.
//
// @param asset_name Asset path relative to assets folder (UTF-8)
// @return Size in bytes, or 0 if the asset is not found or empty.
SKA_API size_t ska_asset_size(const char* asset_name);

// Read asset file into a null-terminated string.
// Calls ska_asset_read() then reallocs to add '\0' terminator.
// Caller must free the returned string with ska_file_free_data().
//
// @param asset_name Asset path relative to assets folder (UTF-8)
// @param out_text Output pointer to null-terminated string (required, not NULL)
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_asset_read_text(const char* asset_name, char** out_text);

// Write data to file.
// Binary mode (no newline translation). Creates or truncates file.
// If size is 0, creates empty file (data can be NULL in this case).
//
// @param filename Path to file (UTF-8)
// @param data Data to write (can be NULL if size is 0)
// @param size Size in bytes
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_file_write(const char* filename, const void* data, size_t size);

// Write null-terminated string to file.
// Writes strlen(text) bytes (does not write the null terminator).
// Equivalent to ska_file_write(filename, text, strlen(text)).
//
// @param filename Path to file (UTF-8)
// @param text Text to write (UTF-8), required not NULL
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_file_write_text(const char* filename, const char* text);

// Free data returned by ska_file_read() or ska_file_read_text().
// Just calls free(). Safe to pass NULL (no-op).
//
// @param data Pointer returned by ska_file_read() or ska_file_read_text()
SKA_API void ska_file_free_data(void* data);

// Check if file exists.
// Uses access() on POSIX, _access() on Windows. Checks F_OK (file existence).
//
// @param filename Path to file (UTF-8)
// @return true if file exists and is accessible, false otherwise
SKA_API bool ska_file_exists(const char* filename);

// Get file size without reading it.
// Uses stat() on POSIX, _stat() on Windows. Returns 0 on error (can't distinguish from empty file).
//
// @param filename Path to file (UTF-8)
// @return File size in bytes, or 0 on failure (ambiguous with empty file)
SKA_API size_t ska_file_size(const char* filename);

// Directory entry information passed to ska_dir_iterate callback
typedef struct ska_dir_entry_t {
	const char* name;      // Entry name only, not full path
	bool        is_dir;    // true if directory, false if file
	size_t      size;      // File size in bytes (0 for directories)
} ska_dir_entry_t;

// Callback for ska_dir_iterate. Return true to continue, false to stop iteration.
typedef bool (*ska_dir_iterate_fn)(void* context, const ska_dir_entry_t* entry);

// Iterate over directory entries, invoking callback for each entry.
// Non-recursive by default; callers can recurse by calling ska_dir_iterate() from the callback.
// Skips "." and ".." entries automatically.
// On Android, this only works for filesystem paths, not assets.
//
// @param path Directory path (UTF-8)
// @param opt_context User data passed to callback (can be NULL)
// @param callback Function called for each entry (required, not NULL)
// @return true on success (including when callback stops iteration), false on failure
SKA_API bool ska_dir_iterate(const char* path, void* opt_context, ska_dir_iterate_fn callback);

// Get current working directory.
// Writes the path to ref_buffer (null-terminated). On failure, ref_buffer[0] is set to '\0'.
//
// @param ref_buffer Buffer to receive the path (UTF-8)
// @param buffer_size Size of buffer in bytes (should include space for null terminator)
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_get_cwd(char* ref_buffer, size_t buffer_size);

// Set current working directory.
// If path is NULL, sets the working directory to the executable's directory.
// On Android, this function always fails (Android apps don't have traditional working directories).
//
// @param opt_path Directory path (UTF-8), or NULL to use executable's directory
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_set_cwd(const char* opt_path);

// Get the path to the current executable.
// Writes the path to ref_buffer (null-terminated). On failure, ref_buffer[0] is set to '\0'.
// On Android, this function always fails (APKs don't have a traditional executable path).
//
// @param ref_buffer Buffer to receive the path (UTF-8)
// @param buffer_size Size of buffer in bytes (should include space for null terminator)
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_get_exe_path(char* ref_buffer, size_t buffer_size);

// ============================================================================
// Key-Value Persistent Storage
// ============================================================================

// Set the application name used for storage paths. Must be called before using
// other kvpstore functions. The name should be filesystem-safe (alphanumeric,
// underscores, hyphens). Defaults to "sk_app" if not called.
//
// Storage locations per platform:
// - Linux: ~/.config/<app_name>/<key>
// - Windows: Registry HKEY_CURRENT_USER\Software\<app_name>
// - macOS: NSUserDefaults (keys prefixed with <app_name>.)
// - Android: SharedPreferences file named <app_name>
//
// @param app_name Application name (UTF-8), copied internally
SKA_API void ska_kvpstore_set_app_name(const char* app_name);

// Save data to persistent storage. Creates storage location as needed.
// Maximum recommended size is 64KB per key.
//
// @param key Storage key (alphanumeric, underscores, hyphens; max 64 chars)
// @param data Data to save (can be NULL if size is 0)
// @param size Size of data in bytes
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_kvpstore_save(const char* key, const void* data, size_t size);

// Load data from persistent storage.
// To query size only, pass opt_buffer=NULL and buffer_size=0.
//
// @param key Storage key
// @param opt_buffer Buffer to receive data (can be NULL for size query)
// @param buffer_size Size of buffer in bytes (0 for size query)
// @param opt_out_size Receives actual data size (can be NULL)
// @return true on success, false if key not found or error (check ska_error_get())
SKA_API bool ska_kvpstore_load(const char* key, void* opt_buffer, size_t buffer_size, size_t* opt_out_size);

// Delete data from persistent storage.
//
// @param key Storage key to delete
// @return true on success or if key didn't exist, false on error
SKA_API bool ska_kvpstore_delete(const char* key);

// ============================================================================
// Clipboard Support
// ============================================================================

// Get clipboard text. Returned string is malloc'd and must be freed by the caller.
// Returns NULL if clipboard is empty or unavailable.
//
// @return Allocated UTF-8 string (caller must free), or NULL if empty/unavailable
SKA_API char* ska_clipboard_get_text(void);

// Set clipboard text.
// Copies the provided text to the system clipboard. Text must be null-terminated UTF-8.
//
// @param text Text to copy to clipboard (UTF-8), required not NULL
// @return true on success, false on failure (check ska_error_get())
SKA_API bool ska_clipboard_set_text(const char* text);

// ============================================================================
// File Dialog
// ============================================================================

// File type filter for dialogs.
// Provide mime, exts, or both. The library translates as needed per platform:
// - Desktop uses exts (space-separated: "*.png *.jpg"). If only mime given,
//   common types are auto-translated (image/* -> *.png *.jpg etc.)
// - Android uses mime (MIME type: "image/*"). If only exts given, uses "*/*".
typedef struct ska_file_filter_t {
	const char* name;       // Display name, e.g., "Images", "Text Files"
	const char* mime;       // MIME type for Android: "image/*", "text/plain", "*/*"
	const char* exts;       // Space-separated extensions for desktop: "*.png *.jpg *.gif"
} ska_file_filter_t;

// File dialog request configuration
typedef struct ska_file_dialog_request_t {
	ska_file_dialog_          type;           // Dialog type (open, save, folder)
	const char*               title;          // Dialog title (stored for matching in events)
	const char*               default_name;   // Suggested filename for save dialogs (can be NULL)
	const ska_file_filter_t*  filters;        // Array of file filters (can be NULL)
	int32_t                   filter_count;   // Number of filters
	bool                      allow_multiple; // Allow selecting multiple files (open only)
} ska_file_dialog_request_t;

// Check if file dialogs are available on this platform.
// On desktop platforms, always returns true.
// On Android, checks if a system file picker Activity is available. Returns
// false from an Android Service context (file dialogs require an Activity).
//
// @param type Dialog type to check availability for
// @return true if file dialogs of this type are supported, false otherwise
SKA_API bool ska_file_dialog_available(ska_file_dialog_ type);

// Show a file dialog (async).
// Returns immediately. Result delivered via ska_event_file_dialog event.
// The title string is copied internally and available in the result event.
//
// Platform behavior:
// - Linux X11: Uses zenity, kdialog, or xdg-desktop-portal (in order of preference)
// - Win32: Uses IFileOpenDialog/IFileSaveDialog (Vista+ common item dialogs)
// - macOS: Uses NSOpenPanel/NSSavePanel
// - Android: Uses ACTION_OPEN_DOCUMENT/ACTION_CREATE_DOCUMENT intents via SAF.
//   Requires an Activity context — returns 0 from an Android Service.
//
// @param request Dialog configuration (required, not NULL)
// @return Non-zero dialog ID on success, 0 on failure (check ska_error_get())
SKA_API ska_file_dialog_id_t ska_file_dialog_show(const ska_file_dialog_request_t* request);

// Get selected path from file dialog result.
// On desktop: returns filesystem path (e.g., "/home/user/file.txt", "C:\\Users\\...")
// On Android: returns content URI (e.g., "content://com.android.providers...")
// Pointer valid until ska_file_dialog_free_result() is called.
//
// @param result Event data from ska_event_file_dialog
// @param index Path index [0, count)
// @return Path string (UTF-8), or NULL if index out of range
SKA_API const char* ska_file_dialog_get_path(const ska_event_file_dialog_t* result, int32_t index);

// Free resources associated with a file dialog result.
// Must be called after processing the event to release memory.
// Safe to call multiple times or with zeroed struct.
// Warning: Logs a warning if a new file dialog result arrives before the previous
// one was freed (helps catch memory leaks during development).
//
// @param ref_result Event data to free
SKA_API void ska_file_dialog_free_result(ska_event_file_dialog_t* ref_result);

// ============================================================================
// Utilities
// ============================================================================

// Get elapsed time in nanoseconds since ska_init().
// Returns monotonic time (not affected by system clock changes).
// Platform: Win32 uses QueryPerformanceCounter, Linux/Android uses clock_gettime(CLOCK_MONOTONIC),
// macOS uses mach_absolute_time().
//
// @return Nanoseconds since ska_init() was called
SKA_API uint64_t ska_time_get_elapsed_ns(void);

// Get elapsed time in seconds since ska_init().
// Convenience wrapper: ska_time_get_elapsed_ns() / 1,000,000,000.0
//
// @return Seconds since ska_init() was called (sub-microsecond precision)
SKA_API double ska_time_get_elapsed_s(void);

// Sleep for specified milliseconds.
// Uses Sleep() on Win32, usleep() on POSIX. Not high-precision (typical resolution: ~1-15ms).
//
// @param ms Milliseconds to sleep (approximate)
SKA_API void ska_time_sleep(uint32_t ms);

// ============================================================================
// Logging
// ============================================================================

// Log levels
typedef enum ska_log_ {
	ska_log_info = 0,
	ska_log_warn,
	ska_log_error,
} ska_log_;

// Cross-platform logging function.
// Android: uses __android_log_vprint() to logcat with tag "sk_app"
// Desktop: prints to stdout (info/warn) or stderr (error) with level prefix like "[ERROR] "
// Automatically appends newline on desktop, not needed on Android.
//
// @param level Log level (ska_log_info, ska_log_warn, ska_log_error)
// @param fmt Printf-style format string (UTF-8)
// @param ... Format arguments
SKA_API void ska_log(ska_log_ level, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#endif // SK_APP_H
