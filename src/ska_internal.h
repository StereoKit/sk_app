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

// Platform-specific includes
#ifdef SKA_PLATFORM_WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
	#include <windowsx.h>
#endif

#ifdef SKA_PLATFORM_LINUX
	#include <X11/Xlib.h>
	#include <X11/Xutil.h>
	#include <X11/Xatom.h>
	#include <X11/extensions/Xrandr.h>
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
	VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR    = 1000009000,
	VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR  = 1000008000,
	VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK    = 1000123000,
	VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT    = 1000217000,
} VkStructureType;

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
// Input State
// ============================================================================

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
	int32_t mouse_xrel;
	int32_t mouse_yrel;
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
	bool has_focus;
	bool mouse_inside;

	// Platform-specific data
#ifdef SKA_PLATFORM_WIN32
	HWND hwnd;
	HDC hdc;
	bool tracking_mouse_leave;
#endif

#ifdef SKA_PLATFORM_LINUX
	Window xwindow;
	XIC xic;
#endif

#ifdef SKA_PLATFORM_MACOS
	id ns_window;   // NSWindow*
	id ns_view;     // NSView*
#endif

#ifdef SKA_PLATFORM_ANDROID
	ANativeWindow* native_window;
#endif

	void* user_data;
};

// ============================================================================
// Global State
// ============================================================================

typedef struct ska_state_t {
	bool initialized;
	char error_msg[512];
	uint64_t start_time;

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
	Display* x_display;
	int32_t x_screen;
	Window x_root;
	Atom wm_protocols;
	Atom wm_delete_window;
	Atom net_wm_state;
	Atom net_wm_state_fullscreen;
	Atom net_wm_state_maximized_vert;
	Atom net_wm_state_maximized_horz;
	Atom resource_manager; // For DPI change detection
	XIM xim;
	float cached_dpi_scale; // Track DPI changes
	bool is_xwayland;       // Running under XWayland (X11 on Wayland)
#endif

#ifdef SKA_PLATFORM_MACOS
	id ns_app;           // NSApplication*
	id ns_delegate;      // Application delegate
	bool app_activated;
#endif

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
void ska_platform_get_frame_extents(const ska_window_t* window, int32_t* out_left, int32_t* out_right, int32_t* out_top, int32_t* out_bottom);

// Platform-specific input
void ska_platform_warp_mouse(ska_window_t* ref_window, int32_t x, int32_t y);
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
