//
// sk_app - Internal header
// Private structures and platform abstraction layer

#ifndef SKA_INTERNAL_H
#define SKA_INTERNAL_H

#include "sk_app.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

// ============================================================================
// Memory Allocation
// ============================================================================

// Internal allocator state
typedef struct ska_allocator_t {
	ska_alloc_fn   alloc;
	ska_realloc_fn realloc;
	ska_free_fn    free;
	void*          user_data;
} ska_allocator_t;

extern ska_allocator_t g_ska_allocator;

// Internal wrapper functions - use these instead of malloc/calloc/realloc/free
void* ska_malloc(size_t size);
void* ska_calloc(size_t count, size_t size);
void* ska_realloc(void* ptr, size_t size);
void  ska_free(void* ptr);
char* ska_strdup(const char* str);

// POSIX includes for Linux/macOS
#if defined(SKA_PLATFORM_LINUX) || defined(SKA_PLATFORM_MACOS)
	#include <unistd.h>
	#include <dlfcn.h>
#endif

#ifdef SKA_PLATFORM_WEB
	#include <unistd.h>
#endif

// Platform-specific includes
#ifdef SKA_PLATFORM_WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
	#include <windowsx.h>
#endif

// Only the X11 backend needs Xlib types; a Wayland-only build must compile on
// a machine with no X11 headers at all.
#ifdef SKA_LINUX_X11
	#include <X11/Xlib.h>
	#include <X11/Xutil.h>
	#include <X11/Xatom.h>
	#include <X11/extensions/Xrandr.h>
	#include <X11/extensions/sync.h>
	#include <X11/cursorfont.h>
	#include <X11/Xcursor/Xcursor.h>
#endif

#ifdef SKA_PLATFORM_MACOS
	#ifdef __OBJC__
		#import <Cocoa/Cocoa.h>
	#else
		typedef void* id;
	#endif
#endif

#ifdef SKA_PLATFORM_ANDROID
	#include <android/native_window.h>
	#include <android/native_activity.h>
	#include <android/looper.h>
	#include <android/log.h>
#endif

// ============================================================================
// Subset of Vulkan headers that we use
// ============================================================================

#if defined(_WIN32)
	#define VKAPI_CALL __stdcall
	#define VKAPI_PTR  VKAPI_CALL
#elif defined(__ANDROID__) && defined(__ARM_ARCH) && __ARM_ARCH < 7
	#error "Vulkan is not supported for the 'armeabi' NDK ABI"
#elif defined(__ANDROID__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7 && defined(__ARM_32BIT_STATE)
	#define VKAPI_ATTR __attribute__((pcs("aapcs-vfp")))
	#define VKAPI_PTR  VKAPI_ATTR
#else
	// On other platforms, use the default calling convention
	#define VKAPI_ATTR
	#define VKAPI_PTR
#endif

typedef enum VkResult {
	VK_SUCCESS = 0,
	VK_RESULT_MAX_ENUM = 0x7FFFFFFF
} VkResult;

typedef struct VkInstance_T* VkInstance;
typedef uint32_t VkFlags;

#if defined(__LP64__) || defined(_WIN64) || (defined(__x86_64__) && !defined(__ILP32__) ) || defined(_M_X64) || defined(__ia64) || defined (_M_IA64) || defined(__aarch64__) || defined(__powerpc64__) || (defined(__riscv) && __riscv_xlen == 64)
	#define VK_USE_64_BIT_PTR_DEFINES 1
	typedef struct VkSurfaceKHR_T *VkSurfaceKHR;
#else
	#define VK_USE_64_BIT_PTR_DEFINES 0
	typedef uint64_t VkSurfaceKHR;
#endif

typedef void (VKAPI_PTR *PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (VKAPI_PTR *PFN_vkGetInstanceProcAddr)(VkInstance instance, const char* pName);

// Structure types used across platforms
typedef enum VkStructureType {
	VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR     = 1000004000,
	VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR  = 1000006000,
	VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR    = 1000009000,
	VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR  = 1000008000,
	VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK    = 1000123000,
	VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT    = 1000217000,
} VkStructureType;

// ============================================================================
// Subset of WebGPU headers that we use
// ============================================================================
//
// Same minimal-header approach as the Vulkan subset above: these declarations
// match the layout of the official webgpu-headers as shipped by Dawn,
// wgpu-native, and Emscripten's emdawnwebgpu port, so sk_app has no build
// dependency on webgpu.h. sk_app does not provide a WebGPU implementation;
// wgpuInstanceCreateSurface is resolved from whichever one the application
// links. Only ska_wgpu.c references it, so apps that never call
// ska_wgpu_create_surface never pull in the dependency.

typedef struct WGPUInstanceImpl* WGPUInstance;
typedef struct WGPUSurfaceImpl*  WGPUSurface;

typedef enum WGPUSType {
	WGPUSType_SurfaceSourceMetalLayer                   = 0x00000004,
	WGPUSType_SurfaceSourceWindowsHWND                  = 0x00000005,
	WGPUSType_SurfaceSourceXlibWindow                   = 0x00000006,
	WGPUSType_SurfaceSourceWaylandSurface               = 0x00000007,
	WGPUSType_SurfaceSourceAndroidNativeWindow          = 0x00000008,
	WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector = 0x00040000,
	WGPUSType_Force32                                   = 0x7FFFFFFF,
} WGPUSType;

typedef struct WGPUChainedStruct {
	struct WGPUChainedStruct* next;
	WGPUSType                 sType;
} WGPUChainedStruct;

// Sentinel length for null-terminated WGPUStringView strings
#define WGPU_STRLEN SIZE_MAX

typedef struct WGPUStringView {
	const char* data;
	size_t      length;
} WGPUStringView;

typedef struct WGPUSurfaceDescriptor {
	WGPUChainedStruct* nextInChain;
	WGPUStringView     label;
} WGPUSurfaceDescriptor;

typedef struct WGPUSurfaceSourceMetalLayer {
	WGPUChainedStruct chain;
	void*             layer;     // CAMetalLayer*
} WGPUSurfaceSourceMetalLayer;

typedef struct WGPUSurfaceSourceWindowsHWND {
	WGPUChainedStruct chain;
	void*             hinstance;
	void*             hwnd;
} WGPUSurfaceSourceWindowsHWND;

typedef struct WGPUSurfaceSourceXlibWindow {
	WGPUChainedStruct chain;
	void*             display;
	uint64_t          window;
} WGPUSurfaceSourceXlibWindow;

typedef struct WGPUSurfaceSourceWaylandSurface {
	WGPUChainedStruct chain;
	void*             display; // wl_display*
	void*             surface; // wl_surface*
} WGPUSurfaceSourceWaylandSurface;

typedef struct WGPUSurfaceSourceAndroidNativeWindow {
	WGPUChainedStruct chain;
	void*             window;    // ANativeWindow*
} WGPUSurfaceSourceAndroidNativeWindow;

typedef struct WGPUEmscriptenSurfaceSourceCanvasHTMLSelector {
	WGPUChainedStruct chain;
	WGPUStringView    selector;
} WGPUEmscriptenSurfaceSourceCanvasHTMLSelector;

// Provided by the application's WebGPU implementation
extern WGPUSurface wgpuInstanceCreateSurface(WGPUInstance instance, const WGPUSurfaceDescriptor* descriptor);

// ============================================================================
// Event Queue
// ============================================================================

#define SKA_EVENT_QUEUE_SIZE 512

typedef struct ska_event_queue_t {
	ska_event_t events[SKA_EVENT_QUEUE_SIZE];
	int32_t read_pos;
	int32_t write_pos;
	int32_t count;
} ska_event_queue_t;

void ska_event_queue_init(ska_event_queue_t* queue);
bool ska_event_queue_push(ska_event_queue_t* queue, const ska_event_t* event);
bool ska_event_queue_pop(ska_event_queue_t* queue, ska_event_t* event);
bool ska_event_queue_is_empty(const ska_event_queue_t* queue);
void ska_event_queue_clear(ska_event_queue_t* queue);

// ============================================================================
// Text Input Queue
// ============================================================================

#define SKA_TEXT_QUEUE_SIZE 256

typedef struct ska_text_queue_t {
	uint32_t codepoints[SKA_TEXT_QUEUE_SIZE];  // UTF-32 codepoints
	int32_t read_pos;
	int32_t write_pos;
	int32_t count;
} ska_text_queue_t;

// Internal text queue functions
void ska_text_queue_init(ska_text_queue_t* queue);
void ska_text_queue_push_utf8(ska_text_queue_t* queue, const char* utf8);

// ============================================================================
// Input State
// ============================================================================

typedef struct ska_input_state_t {
	uint8_t keyboard[ska_scancode_count];
	uint16_t key_modifiers;

	int32_t mouse_x;
	int32_t mouse_y;
	int32_t mouse_xrel;      // Last motion event's delta
	int32_t mouse_yrel;
	int32_t mouse_delta_x;   // Accumulated since the last ska_mouse_get_delta
	int32_t mouse_delta_y;
	uint32_t mouse_buttons;

	bool relative_mouse_mode;
	bool cursor_visible;

	// Text input
	ska_text_queue_t text_queue;
	ska_text_input_type_ text_input_type;
	bool virtual_keyboard_visible;
} ska_input_state_t;

void ska_input_state_init(ska_input_state_t* state);
void ska_input_state_reset(ska_input_state_t* state);

// Records one motion delta, both as the latest and into the polled accumulator
void ska_input_add_relative(int32_t xrel, int32_t yrel);

// Modifier mask implied by the tracked key state, for platforms that derive it
// rather than querying the OS.
uint16_t ska_input_state_derive_modifiers(const ska_input_state_t* state);

// Queues a key up per held key and a button up per held button, then clears the
// tracked state.
void ska_input_release_all(ska_window_id_t window_id);

// ============================================================================
// Window Structure
// ============================================================================

#define SKA_MAX_WINDOWS 16

struct ska_window_t {
	ska_window_id_t id;
	uint32_t flags;
	char* title;

	int32_t x, y;
	int32_t width, height;
	int32_t drawable_width, drawable_height;
	float   dpi_scale; // Cached DPI scale factor (1.0 = 100%)

	bool should_close;
	bool is_visible;
	bool is_fullscreen; // Live platform-reported state, not the last request
	bool has_focus;
	bool mouse_inside;

	// Platform-specific data
#ifdef SKA_PLATFORM_WIN32
	HWND hwnd;
	HDC hdc;
	bool tracking_mouse_leave;
	DWORD           saved_style;     // Window style before fullscreen
	WINDOWPLACEMENT saved_placement; // Windowed placement before fullscreen
#endif

#ifdef SKA_PLATFORM_LINUX
	#ifdef SKA_LINUX_X11
	Window xwindow;
	XIC    xic;
	XSyncCounter x_sync_counter; // _NET_WM_SYNC_REQUEST resize handshake, 0 without XSync
	XSyncValue   x_sync_value;   // Value the current configure gets acked with
	bool         x_sync_pending; // Sync request seen, awaiting its ConfigureNotify
	bool         x_sync_ack;     // Configure handled, counter update due next pump
	#endif
	struct ska_wl_window_t* wl; // Wayland backend state, see ska_wayland.c
#endif

#ifdef SKA_PLATFORM_MACOS
	id ns_window;   // NSWindow*
	id ns_view;     // NSView*
#endif

#ifdef SKA_PLATFORM_ANDROID
	ANativeWindow* native_window;
#endif

#ifdef SKA_PLATFORM_WEB
	char canvas_selector[64]; // CSS selector for the window's canvas element
	bool owns_canvas;         // Canvas element was created (and is removed) by us
#endif

	void* user_data;
};

// ============================================================================
// Linux Backend Dispatch
// ============================================================================

#ifdef SKA_PLATFORM_LINUX

// X11 and Wayland are both compiled in when available and chosen at runtime,
// so ska_linux.c forwards every ska_platform_* entry point through this table.
// Entry points that behave identically on both live in ska_linux_common.c.
typedef struct ska_linux_vtable_t {
	const char* name; // "x11" or "wayland", for logging

	bool  (*init)                     (void);
	void  (*shutdown)                 (void);

	bool  (*window_create)            (ska_window_t* ref_window, const char* title, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t flags);
	void  (*window_destroy)           (ska_window_t* ref_window);
	void  (*window_set_title)         (ska_window_t* ref_window, const char* title);
	void  (*window_set_frame_position)(ska_window_t* ref_window, int32_t x, int32_t y);
	void  (*window_set_frame_size)    (ska_window_t* ref_window, int32_t w, int32_t h);
	void  (*window_show)              (ska_window_t* ref_window);
	void  (*window_hide)              (ska_window_t* ref_window);
	void  (*window_maximize)          (ska_window_t* ref_window);
	void  (*window_minimize)          (ska_window_t* ref_window);
	void  (*window_restore)           (ska_window_t* ref_window);
	void  (*window_set_fullscreen)    (ska_window_t* ref_window, bool fullscreen);
	void  (*window_raise)             (ska_window_t* ref_window);
	void  (*window_get_drawable_size) (ska_window_t* ref_window, int32_t* opt_out_width, int32_t* opt_out_height);
	void  (*get_frame_extents)        (const ska_window_t* window, int32_t* opt_out_left, int32_t* opt_out_right, int32_t* opt_out_top, int32_t* opt_out_bottom);
	float (*get_dpi_scale)            (const ska_window_t* window);
	float (*get_refresh_rate)         (const ska_window_t* window);

	void  (*show_cursor)              (bool show);
	void  (*set_cursor)               (ska_system_cursor_ cursor);
	bool  (*set_relative_mouse_mode)  (bool enabled);

	void  (*pump_events)              (void);

	const char** (*vk_get_instance_extensions)(uint32_t* out_count);
	bool         (*vk_create_surface)         (const ska_window_t* window, VkInstance instance, VkSurfaceKHR* out_surface);

	char* (*clipboard_get_text)       (void);
	bool  (*clipboard_set_text)       (const char* text);
} ska_linux_vtable_t;

#ifdef SKA_LINUX_X11
extern const ska_linux_vtable_t ska_x11_vtable;
#endif

// Polls the pending zenity/kdialog subprocess. Each backend calls this from
// pump_events, including on headless paths with no display connection.
void ska_linux_check_file_dialog(void);

// Layout-independent keysym to scancode mapping, shared because X11 and
// xkbcommon use identical keysym values.
ska_scancode_ ska_linux_keysym_to_scancode(uint32_t keysym);

// Entry point into the Vulkan loader, cached after the first call. NULL when
// no loader is installed.
PFN_vkGetInstanceProcAddr ska_linux_vk_get_proc_addr(void);

// One optional piece of a backend, for the summary each one logs at init.
typedef struct ska_linux_optional_t {
	const char* name;   // The library or protocol that is absent
	bool        present;
	const char* effect; // What the app loses without it, in the user's terms
} ska_linux_optional_t;

// Logs the absent entries as info, one line each, and nothing at all when the
// backend has everything. Missing pieces are expected rather than wrong, so
// these are not warnings.
void ska_linux_log_optional(const char* backend, const ska_linux_optional_t* items, uint32_t count);

#ifdef SKA_LINUX_WAYLAND
// Native handle accessors, reached from the public API in ska_common.c rather
// than through the vtable, since they have no X11-side counterpart to dispatch.
void* ska_wl_get_native_handle(const ska_window_t* window);

// Says a renderer now attaches and commits this window's surface, so sk_app
// stops committing pending state on its own.
void  ska_wl_mark_presenting  (const ska_window_t* window);
void* ska_wl_get_display(void);
#endif

#endif // SKA_PLATFORM_LINUX

// ============================================================================
// Logical and Pixel Coordinates
// ============================================================================
//
// Window sizes, positions, and mouse coordinates are screen coordinates, while
// the drawable size and the underlying OS window are pixels. On a scaled
// display those differ, so backends convert at the API boundary.
//
// The Wayland backend does not use these: the compositor hands it logical
// coordinates directly, and its scale is an exact 120ths integer rather than
// this float.

static inline float ska_window_scale(const ska_window_t* window) {
	if (!window) return 1.0f;
	return window->dpi_scale > 0.0f ? window->dpi_scale : 1.0f;
}

// Rounds up, so a window never resolves to fewer pixels than it covers. Done by
// hand rather than with ceilf, which would drag libm in for one operation.
static inline int32_t ska_to_pixels(const ska_window_t* window, int32_t logical) {
	float scale = ska_window_scale(window);
	if (scale == 1.0f) return logical;

	float   exact = (float)logical * scale;
	int32_t whole = (int32_t)exact;
	return exact > (float)whole ? whole + 1 : whole;
}

static inline int32_t ska_to_logical(const ska_window_t* window, int32_t pixels) {
	float scale = ska_window_scale(window);
	if (scale == 1.0f) return pixels;
	return (int32_t)((float)pixels / scale + 0.5f);
}

// ============================================================================
// Global State
// ============================================================================

typedef struct ska_state_t {
	bool initialized;
	char error_msg[512];
	uint64_t start_time;
	char* app_id; // From ska_settings_t, NULL when unset; window title stands in

	ska_window_t* windows[SKA_MAX_WINDOWS];
	uint32_t window_count;
	ska_window_id_t next_window_id;

	ska_event_queue_t event_queue;
	bool              event_queue_was_empty; // Frame boundary detection for auto-clearing text
	ska_input_state_t input_state;

	// Platform-specific state
#ifdef SKA_PLATFORM_WIN32
	HINSTANCE hinstance;
	WNDCLASSEXW window_class;
	bool window_class_registered;
#endif

#ifdef SKA_PLATFORM_LINUX
	ska_linux_backend_        backend; // Resolved by ska_platform_init
	const ska_linux_vtable_t* lnx;     // NULL only before init and after shutdown

	struct ska_wl_state_t* wl; // Wayland backend state, see ska_wayland.c

	#ifdef SKA_LINUX_X11
	Display* x_display;
	int32_t x_screen;
	Window x_root;
	Atom wm_protocols;
	Atom wm_delete_window;
	Atom net_wm_state;
	Atom net_wm_state_fullscreen;
	Atom net_wm_state_maximized_vert;
	Atom net_wm_state_maximized_horz;
	Atom net_wm_sync_request;
	Atom net_wm_sync_request_counter;
	Atom resource_manager; // For DPI change detection
	XIM xim;
	float cached_dpi_scale; // Track DPI changes
	#endif
#endif

#ifdef SKA_PLATFORM_MACOS
	id ns_app;           // NSApplication*
	id ns_delegate;      // Application delegate
	bool app_activated;
#endif

#ifdef SKA_PLATFORM_WEB
	bool  web_default_canvas_used; // A window has claimed the page's #canvas
	float web_cached_dpr;          // Track devicePixelRatio changes
#endif

	// An external loop is driving frames instead of ska_run(); suppresses the
	// web blocking-loop detector. See ska_set_external_frame_driver.
	bool external_frame_driver;

#ifdef SKA_PLATFORM_ANDROID
	struct android_app* android_app;
	void* android_context;  // jobject global ref — any Context (Activity, Service, etc.)
	void* java_vm;          // JavaVM*, cached at init
	void* asset_manager;    // AAssetManager*, extracted from android_app or Context
	bool android_is_activity; // true when android_context is an Activity, false for Service/plain Context
	bool app_has_focus;
	bool app_is_visible;
#endif

} ska_state_t;

extern ska_state_t g_ska;

// ============================================================================
// Internal Functions
// ============================================================================

// Common
void ska_set_error(const char* fmt, ...);
ska_window_t* ska_window_alloc(void);
void ska_window_free(ska_window_t* ref_window);
void ska_post_event(const ska_event_t* event);

// KVP Store internal functions (for platform implementations)
const char* ska_kvpstore_get_app_name(void);
bool        ska_kvpstore_validate_key(const char* key);

// Platform-specific initialization
bool ska_platform_init(void);
void ska_platform_shutdown(void);

// Platform-specific window operations
bool ska_platform_window_create(ska_window_t* ref_window, const char* title, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t flags);
void ska_platform_window_destroy(ska_window_t* ref_window);
void ska_platform_window_set_title(ska_window_t* ref_window, const char* title);
void ska_platform_window_set_frame_position(ska_window_t* ref_window, int32_t x, int32_t y);
void ska_platform_window_set_frame_size(ska_window_t* ref_window, int32_t w, int32_t h);
void ska_platform_window_show(ska_window_t* ref_window);
void ska_platform_window_hide(ska_window_t* ref_window);
void ska_platform_window_maximize(ska_window_t* ref_window);
void ska_platform_window_minimize(ska_window_t* ref_window);
void ska_platform_window_restore(ska_window_t* ref_window);
void ska_platform_window_set_fullscreen(ska_window_t* ref_window, bool fullscreen);
void ska_platform_window_raise(ska_window_t* ref_window);
void ska_platform_window_get_drawable_size(ska_window_t* ref_window, int32_t* opt_out_width, int32_t* opt_out_height);
float ska_platform_get_dpi_scale(const ska_window_t* window);
float ska_platform_get_refresh_rate(const ska_window_t* window);

// Platform-specific frame extents (title bar, borders)
// Returns the size of window decorations: left, right, top (title bar), bottom
void ska_platform_get_frame_extents(const ska_window_t* window, int32_t* opt_out_left, int32_t* opt_out_right, int32_t* opt_out_top, int32_t* opt_out_bottom);

// Platform-specific input
void ska_platform_show_cursor(bool show);
void ska_platform_set_cursor(ska_system_cursor_ cursor);
bool ska_platform_set_relative_mouse_mode(bool enabled);

// Platform-specific text input (mobile only)
void ska_platform_show_virtual_keyboard(bool visible, ska_text_input_type_ type);

// Platform-specific event processing
void ska_platform_pump_events(void);

// Vulkan support
const char** ska_platform_vk_get_instance_extensions(uint32_t* out_count);
bool         ska_platform_vk_create_surface         (const ska_window_t* window, VkInstance instance, VkSurfaceKHR* out_surface);

// Clipboard support
char* ska_platform_clipboard_get_text(void);
bool  ska_platform_clipboard_set_text(const char* text);

// ============================================================================
// File Dialog Internal Structures
// ============================================================================

#define SKA_MAX_FILE_DIALOGS 8
#define SKA_MAX_DIALOG_PATHS 10

// Internal storage for file dialog result paths
typedef struct ska_file_dialog_result_t {
	ska_file_dialog_id_t id;
	char*                title;         // Copied from request
	char**               paths;         // Array of path strings
	int32_t              path_count;
	bool                 cancelled;
	bool                 freed;         // Leak tracking
} ska_file_dialog_result_t;

// File dialog state (added to g_ska via extern)
typedef struct ska_file_dialog_state_t {
	ska_file_dialog_id_t       next_id;
	ska_file_dialog_result_t   results[SKA_MAX_FILE_DIALOGS];
	int32_t                    result_count;
	int32_t                    leaked_count;  // Results delivered but not freed
} ska_file_dialog_state_t;

extern ska_file_dialog_state_t g_ska_file_dialog;

// Internal file dialog functions
ska_file_dialog_result_t* ska_file_dialog_result_alloc(ska_file_dialog_id_t id, const char* title);
void                      ska_file_dialog_result_add_path(ska_file_dialog_result_t* result, const char* path);
void                      ska_file_dialog_result_complete(ska_file_dialog_result_t* result, bool cancelled);

// File filter helpers - get platform-appropriate pattern from filter
// Returns exts if available, otherwise translates common MIME types to extensions
// Returns static string, do not free. Returns "*" if no pattern available.
const char* ska_filter_get_exts(const ska_file_filter_t* filter);

// Returns mime if available, otherwise "*/*"
const char* ska_filter_get_mime(const ska_file_filter_t* filter);

// Platform-specific file dialog
bool ska_platform_file_dialog_available(ska_file_dialog_ type);
bool ska_platform_file_dialog_show(ska_file_dialog_id_t id, const ska_file_dialog_request_t* request);

// Utility functions
uint64_t ska_get_time_ns(void);

// Internal helper for event timestamps (milliseconds, wraps at ~49 days)
static inline uint32_t ska_time_get_elapsed_ms(void) {
	return (uint32_t)(ska_time_get_elapsed_ns() / 1000000ULL);
}

// Platform-specific utilities
#ifdef SKA_PLATFORM_WIN32
wchar_t* ska_utf8_to_wide(const char* utf8);
char* ska_wide_to_utf8(const wchar_t* wide);
void ska_free_string(void* str);
#endif

#ifdef SKA_PLATFORM_ANDROID
// Read a content:// URI via ContentResolver. Returns the file data in the same
// format as ska_file_read.
bool ska_android_content_read(const char* uri, void** out_data, size_t* out_size);
// Write bytes to a content:// URI via ContentResolver, mirroring ska_file_write.
bool ska_android_content_write(const char* uri, const void* data, size_t size);
#endif

#endif // SKA_INTERNAL_H
