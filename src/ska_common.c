//
// sk_app - Common implementation

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "ska_internal.h"
#include <stdarg.h>
#include <time.h>

#ifdef SKA_PLATFORM_WIN32
#include <timeapi.h>
#else
#include <unistd.h>
#endif

#ifdef SKA_PLATFORM_WIN32
static LARGE_INTEGER g_qpc_frequency;
#endif

#ifdef SKA_PLATFORM_LINUX
#include <sys/time.h>
#include <unistd.h>
#endif

#ifdef SKA_PLATFORM_MACOS
#include <mach/mach_time.h>
#include <unistd.h>
#endif

// Global state
ska_state_t g_ska = {0};

// KVP Store app name
static char* g_kvpstore_app_name = NULL;

// ============================================================================
// Memory Allocation
// ============================================================================

// Default allocators using stdlib
static void* ska_default_alloc(size_t size, void* user_data) {
	(void)user_data;
	return malloc(size);
}

static void* ska_default_realloc(void* ptr, size_t size, void* user_data) {
	(void)user_data;
	return realloc(ptr, size);
}

static void ska_default_free(void* ptr, void* user_data) {
	(void)user_data;
	free(ptr);
}

// Global allocator state - initialized to defaults
ska_allocator_t g_ska_allocator = {
	.alloc     = ska_default_alloc,
	.realloc   = ska_default_realloc,
	.free      = ska_default_free,
	.user_data = NULL
};

void* ska_malloc(size_t size) {
	return g_ska_allocator.alloc(size, g_ska_allocator.user_data);
}

void* ska_calloc(size_t count, size_t size) {
	size_t total = count * size;
	void* ptr = g_ska_allocator.alloc(total, g_ska_allocator.user_data);
	if (ptr) memset(ptr, 0, total);
	return ptr;
}

void* ska_realloc(void* ptr, size_t size) {
	return g_ska_allocator.realloc(ptr, size, g_ska_allocator.user_data);
}

void ska_free(void* ptr) {
	if (ptr) g_ska_allocator.free(ptr, g_ska_allocator.user_data);
}

char* ska_strdup(const char* str) {
	if (!str) return NULL;
	size_t len = strlen(str) + 1;
	char* copy = (char*)ska_malloc(len);
	if (copy) memcpy(copy, str, len);
	return copy;
}

// ============================================================================
// Error Handling
// ============================================================================

void ska_set_error(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(g_ska.error_msg, sizeof(g_ska.error_msg), fmt, args);
	va_end(args);
	ska_log(ska_log_error, "%s", g_ska.error_msg);
}

SKA_API const char* ska_error_get(void) {
	return g_ska.error_msg[0] != '\0' ? g_ska.error_msg : NULL;
}

// ============================================================================
// KVP Store Common
// ============================================================================

SKA_API void ska_kvpstore_set_app_name(const char* app_name) {
	if (g_kvpstore_app_name) {
		ska_free(g_kvpstore_app_name);
		g_kvpstore_app_name = NULL;
	}
	if (app_name && app_name[0] != '\0') {
		g_kvpstore_app_name = ska_strdup(app_name);
	}
}

const char* ska_kvpstore_get_app_name(void) {
	return g_kvpstore_app_name ? g_kvpstore_app_name : "sk_app";
}

bool ska_kvpstore_validate_key(const char* key) {
	if (!key || key[0] == '\0') {
		ska_set_error("ska_kvpstore: key cannot be NULL or empty");
		return false;
	}

	size_t len = 0;
	for (const char* p = key; *p; p++, len++) {
		char c = *p;
		bool valid = (c >= 'a' && c <= 'z') ||
		             (c >= 'A' && c <= 'Z') ||
		             (c >= '0' && c <= '9') ||
		             c == '_' || c == '-';
		if (!valid) {
			ska_set_error("ska_kvpstore: invalid character '%c' in key", c);
			return false;
		}
	}

	if (len > 64) {
		ska_set_error("ska_kvpstore: key exceeds maximum length of 64 characters");
		return false;
	}

	return true;
}

// ============================================================================
// Initialization
// ============================================================================

SKA_API bool ska_init(const ska_settings_t* opt_settings) {
	if (g_ska.initialized) {
		ska_set_error("sk_app already initialized");
		return false;
	}

	// Set up allocators first (before any allocations)
	if (opt_settings) {
		bool has_alloc   = opt_settings->alloc   != NULL;
		bool has_realloc = opt_settings->realloc != NULL;
		bool has_free    = opt_settings->free    != NULL;

		// If any allocator is provided, all must be provided
		if (has_alloc || has_realloc || has_free) {
			if (!has_alloc || !has_realloc || !has_free) {
				ska_set_error("ska_init: if any allocator function is provided, all must be provided (alloc, realloc, free)");
				return false;
			}
			g_ska_allocator.alloc     = opt_settings->alloc;
			g_ska_allocator.realloc   = opt_settings->realloc;
			g_ska_allocator.free      = opt_settings->free;
			g_ska_allocator.user_data = opt_settings->alloc_user_data;
		} else {
			g_ska_allocator.alloc     = ska_default_alloc;
			g_ska_allocator.realloc   = ska_default_realloc;
			g_ska_allocator.free      = ska_default_free;
			g_ska_allocator.user_data = NULL;
		}
	} else {
		g_ska_allocator.alloc     = ska_default_alloc;
		g_ska_allocator.realloc   = ska_default_realloc;
		g_ska_allocator.free      = ska_default_free;
		g_ska_allocator.user_data = NULL;
	}

	memset(&g_ska, 0, sizeof(g_ska));
#ifdef SKA_PLATFORM_WIN32
	QueryPerformanceFrequency(&g_qpc_frequency);
#endif
	g_ska.start_time = ska_get_time_ns();
	g_ska.next_window_id = 1;

	ska_event_queue_init(&g_ska.event_queue);
	ska_input_state_init(&g_ska.input_state);

	if (!ska_platform_init()) {
		return false;
	}

	g_ska.initialized = true;
	ska_log(ska_log_info, "sk_app initialized");
	return true;
}

SKA_API void ska_shutdown(void) {
	if (!g_ska.initialized) {
		return;
	}

	// Destroy all windows
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (g_ska.windows[i]) {
			ska_window_destroy(g_ska.windows[i]);
		}
	}

	ska_platform_shutdown();

	// Cleanup KVP store
	if (g_kvpstore_app_name) {
		ska_free(g_kvpstore_app_name);
		g_kvpstore_app_name = NULL;
	}

	g_ska.initialized = false;
	ska_log(ska_log_info, "sk_app shutdown");
}

// ============================================================================
// Window Management
// ============================================================================

ska_window_t* ska_window_alloc(void) {
	if (g_ska.window_count >= SKA_MAX_WINDOWS) {
		ska_set_error("Maximum number of windows (%d) reached", SKA_MAX_WINDOWS);
		return NULL;
	}

	ska_window_t* window = (ska_window_t*)ska_calloc(1, sizeof(ska_window_t));
	if (!window) {
		ska_set_error("Failed to allocate window structure");
		return NULL;
	}

	window->id = g_ska.next_window_id++;
	window->is_visible = true;

	// Find free slot
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (!g_ska.windows[i]) {
			g_ska.windows[i] = window;
			g_ska.window_count++;
			return window;
		}
	}

	ska_free(window);
	ska_set_error("Internal error: no free window slot");
	return NULL;
}

void ska_window_free(ska_window_t* ref_window) {
	if (!ref_window) return;

	// Remove from window array
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (g_ska.windows[i] == ref_window) {
			g_ska.windows[i] = NULL;
			g_ska.window_count--;
			break;
		}
	}

	if (ref_window->title) {
		ska_free(ref_window->title);
	}

	ska_free(ref_window);
}

SKA_API ska_window_t* ska_window_create(
	const char* title,
	int32_t x, int32_t y,
	int32_t width, int32_t height,
	uint32_t flags
) {
	if (!g_ska.initialized) {
		ska_set_error("sk_app not initialized");
		return NULL;
	}

	if (!title) title = "sk_app window";
	if (width <= 0) width = 640;
	if (height <= 0) height = 480;

	ska_window_t* window = ska_window_alloc();
	if (!window) {
		return NULL;
	}

	window->flags = flags;
	window->width = width;
	window->height = height;

	if (x == SKA_WINDOWPOS_UNDEFINED) {
		x = 100; // Default position
	} else if (x == SKA_WINDOWPOS_CENTERED) {
		x = -1; // Platform will center
	}

	if (y == SKA_WINDOWPOS_UNDEFINED) {
		y = 100;
	} else if (y == SKA_WINDOWPOS_CENTERED) {
		y = -1;
	}

	window->x = x;
	window->y = y;

	if (!ska_platform_window_create(window, title, x, y, width, height, flags)) {
		ska_window_free(window);
		return NULL;
	}

	if (!(flags & ska_window_hidden)) {
		ska_platform_window_show(window);
	}

	return window;
}

SKA_API void ska_window_destroy(ska_window_t* ref_window) {
	if (!ref_window) return;

	ska_platform_window_destroy(ref_window);
	ska_window_free(ref_window);
}

SKA_API ska_window_id_t ska_window_get_id(const ska_window_t* window) {
	return window ? window->id : 0;
}

SKA_API ska_window_t* ska_window_from_id(ska_window_id_t id) {
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (g_ska.windows[i] && g_ska.windows[i]->id == id) {
			return g_ska.windows[i];
		}
	}
	return NULL;
}

SKA_API void ska_window_set_title(ska_window_t* ref_window, const char* title) {
	if (!ref_window || !title) return;
	ska_platform_window_set_title(ref_window, title);
}

SKA_API const char* ska_window_get_title(const ska_window_t* window) {
	return window ? window->title : NULL;
}

// ============================================================================
// Frame Position/Size (includes title bar and borders)
// ============================================================================

SKA_API void ska_window_set_frame_position(ska_window_t* ref_window, int32_t x, int32_t y) {
	if (!ref_window) return;
	ska_platform_window_set_frame_position(ref_window, x, y);
}

SKA_API void ska_window_get_frame_position(const ska_window_t* window, int32_t* opt_out_x, int32_t* opt_out_y) {
	if (!window) return;
	int32_t left, top;
	ska_platform_get_frame_extents(window, &left, NULL, &top, NULL);
	if (opt_out_x) *opt_out_x = window->x - left;
	if (opt_out_y) *opt_out_y = window->y - top;
}

SKA_API void ska_window_set_frame_size(ska_window_t* ref_window, int32_t width, int32_t height) {
	if (!ref_window) return;
	// Convert frame size to content size by subtracting decorations
	int32_t left, right, top, bottom;
	ska_platform_get_frame_extents(ref_window, &left, &right, &top, &bottom);
	int32_t content_w = width - left - right;
	int32_t content_h = height - top - bottom;
	if (content_w > 0 && content_h > 0) {
		ska_platform_window_set_frame_size(ref_window, content_w, content_h);
	}
}

SKA_API void ska_window_get_frame_size(const ska_window_t* window, int32_t* opt_out_width, int32_t* opt_out_height) {
	if (!window) return;
	int32_t left, right, top, bottom;
	ska_platform_get_frame_extents(window, &left, &right, &top, &bottom);
	if (opt_out_width)  *opt_out_width  = window->width + left + right;
	if (opt_out_height) *opt_out_height = window->height + top + bottom;
}

// ============================================================================
// Content Position/Size (client area, excludes decorations)
// ============================================================================

SKA_API void ska_window_set_content_position(ska_window_t* ref_window, int32_t x, int32_t y) {
	if (!ref_window) return;
	// Convert content position to frame position
	int32_t left, top;
	ska_platform_get_frame_extents(ref_window, &left, NULL, &top, NULL);
	ska_platform_window_set_frame_position(ref_window, x - left, y - top);
}

SKA_API void ska_window_get_content_position(const ska_window_t* window, int32_t* opt_out_x, int32_t* opt_out_y) {
	if (!window) return;
	// window->x and window->y already store content position
	if (opt_out_x) *opt_out_x = window->x;
	if (opt_out_y) *opt_out_y = window->y;
}

SKA_API void ska_window_set_content_size(ska_window_t* ref_window, int32_t width, int32_t height) {
	if (!ref_window) return;
	ska_platform_window_set_frame_size(ref_window, width, height);
}

SKA_API void ska_window_get_content_size(const ska_window_t* window, int32_t* opt_out_width, int32_t* opt_out_height) {
	if (!window) return;
	if (opt_out_width)  *opt_out_width  = window->width;
	if (opt_out_height) *opt_out_height = window->height;
}

SKA_API void ska_window_get_drawable_size(ska_window_t* ref_window, int32_t* opt_out_width, int32_t* opt_out_height) {
	if (!ref_window) return;
	ska_platform_window_get_drawable_size(ref_window, opt_out_width, opt_out_height);
}

SKA_API float ska_window_get_dpi_scale(const ska_window_t* window) {
	if (!window) return 1.0f;
	return ska_platform_get_dpi_scale(window);
}

SKA_API float ska_window_get_refresh_rate(const ska_window_t* window) {
	return ska_platform_get_refresh_rate(window);
}

SKA_API void ska_window_show(ska_window_t* ref_window) {
	if (!ref_window) return;
	ska_platform_window_show(ref_window);
}

SKA_API void ska_window_hide(ska_window_t* ref_window) {
	if (!ref_window) return;
	ska_platform_window_hide(ref_window);
}

SKA_API void ska_window_maximize(ska_window_t* ref_window) {
	if (!ref_window) return;
	ska_platform_window_maximize(ref_window);
}

SKA_API void ska_window_minimize(ska_window_t* ref_window) {
	if (!ref_window) return;
	ska_platform_window_minimize(ref_window);
}

SKA_API void ska_window_restore(ska_window_t* ref_window) {
	if (!ref_window) return;
	ska_platform_window_restore(ref_window);
}

SKA_API void ska_window_set_fullscreen(ska_window_t* ref_window, bool fullscreen) {
	if (!ref_window) return;
	ska_platform_window_set_fullscreen(ref_window, fullscreen);
}

SKA_API void ska_window_raise(ska_window_t* ref_window) {
	if (!ref_window) return;
	ska_platform_window_raise(ref_window);
}

SKA_API uint32_t ska_window_get_flags(const ska_window_t* window) {
	return window ? window->flags : 0;
}

SKA_API void* ska_window_get_native_handle(const ska_window_t* window) {
	if (!window) return NULL;

#ifdef SKA_PLATFORM_WIN32
	return (void*)window->hwnd;
#elif defined(SKA_PLATFORM_LINUX)
	return (void*)(uintptr_t)window->xwindow;
#elif defined(SKA_PLATFORM_MACOS)
	return window->ns_window;
#elif defined(SKA_PLATFORM_ANDROID)
	return window->native_window;
#elif defined(SKA_PLATFORM_WEB)
	return (void*)window->canvas_selector;
#else
	return NULL;
#endif
}

// ============================================================================
// Event System
// ============================================================================

void ska_post_event(const ska_event_t* event) {
	if (!ska_event_queue_push(&g_ska.event_queue, event)) {
		ska_log(ska_log_warn, "Event queue full, dropping event type %d", event->type);
	}
}

SKA_API bool ska_event_poll(ska_event_t* out_event) {
	if (!g_ska.initialized || !out_event) {
		return false;
	}

	// At frame start, discard any unconsumed text from the previous frame.
	// Apps that use text input will have already consumed it before calling
	// ska_event_poll again.
	if (g_ska.event_queue_was_empty) {
		ska_text_queue_t* tq = &g_ska.input_state.text_queue;
		tq->read_pos  = 0;
		tq->write_pos = 0;
		tq->count     = 0;
	}

	// Process platform events first
	ska_platform_pump_events();

	bool has_event = ska_event_queue_pop(&g_ska.event_queue, out_event);
	g_ska.event_queue_was_empty = !has_event;

	// Feed text input events to the text queue
	if (has_event && out_event->type == ska_event_text_input) {
		ska_text_queue_push_utf8(&g_ska.input_state.text_queue, out_event->text.text);
	}

	return has_event;
}

SKA_API bool ska_event_wait(ska_event_t* out_event) {
	return ska_event_wait_timeout(out_event, -1);
}

SKA_API bool ska_event_wait_timeout(ska_event_t* out_event, int32_t timeout_ms) {
	if (!g_ska.initialized || !out_event) {
		return false;
	}

	uint64_t start = ska_time_get_elapsed_ms();

	while (true) {
		if (ska_event_poll(out_event)) {
			return true;
		}

		if (timeout_ms == 0) {
			return false;
		}

		if (timeout_ms > 0) {
			uint64_t elapsed = ska_time_get_elapsed_ms() - start;
			if (elapsed >= (uint64_t)timeout_ms) {
				return false;
			}
		}

		ska_time_sleep(1);
	}
}

// ============================================================================
// Main Loop
// ============================================================================

// Web builds implement ska_run in ska_web.c on top of emscripten_set_main_loop
#ifndef SKA_PLATFORM_WEB
SKA_API void ska_run(ska_frame_fn frame, void* user_data) {
	if (!frame) return;
	while (frame(user_data)) {}
}
#endif

// ============================================================================
// Input State
// ============================================================================

SKA_API const uint8_t* ska_keyboard_get_state(int32_t* opt_out_num_keys) {
	if (opt_out_num_keys) {
		*opt_out_num_keys = ska_scancode_count;
	}
	return g_ska.input_state.keyboard;
}

SKA_API uint16_t ska_keyboard_get_modifiers(void) {
	return g_ska.input_state.key_modifiers;
}

SKA_API uint32_t ska_mouse_get_state(int32_t* opt_out_x, int32_t* opt_out_y) {
	if (opt_out_x) *opt_out_x = g_ska.input_state.mouse_x;
	if (opt_out_y) *opt_out_y = g_ska.input_state.mouse_y;
	return g_ska.input_state.mouse_buttons;
}

SKA_API uint32_t ska_mouse_get_global_state(int32_t* opt_out_x, int32_t* opt_out_y) {
	// Platform-specific implementation would query OS directly
	return ska_mouse_get_state(opt_out_x, opt_out_y);
}

SKA_API void ska_mouse_warp(ska_window_t* ref_window, int32_t x, int32_t y) {
	if (!ref_window) return;
	ska_platform_warp_mouse(ref_window, x, y);
}

SKA_API void ska_cursor_set(ska_system_cursor_ cursor) {
	ska_platform_set_cursor(cursor);
}

SKA_API void ska_cursor_show(bool show) {
	ska_platform_show_cursor(show);
	g_ska.input_state.cursor_visible = show;
}

SKA_API bool ska_mouse_set_relative_mode(bool enabled) {
	if (ska_platform_set_relative_mouse_mode(enabled)) {
		g_ska.input_state.relative_mouse_mode = enabled;
		return true;
	}
	return false;
}

SKA_API bool ska_mouse_get_relative_mode(void) {
	return g_ska.input_state.relative_mouse_mode;
}

// ============================================================================
// Vulkan Support
// ============================================================================

SKA_API const char** ska_vk_get_instance_extensions(uint32_t* out_count) {
	if (!out_count) return NULL;
	return ska_platform_vk_get_instance_extensions(out_count);
}

SKA_API bool ska_vk_create_surface(const ska_window_t* window, void* instance, void* out_surface) {
	if (!window || !instance || !out_surface) {
		ska_set_error("Invalid parameters for Vulkan surface creation");
		return false;
	}
	return ska_platform_vk_create_surface(window, (VkInstance)instance, (VkSurfaceKHR*)out_surface);
}

// ============================================================================
// Utilities
// ============================================================================

uint64_t ska_get_time_ns(void) {
#ifdef SKA_PLATFORM_WIN32
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	// Convert to nanoseconds: counter * 1e9 / frequency
	// To avoid overflow, we do: (counter / freq) * 1e9 + (counter % freq) * 1e9 / freq
	uint64_t whole_seconds = counter.QuadPart / g_qpc_frequency.QuadPart;
	uint64_t remainder = counter.QuadPart % g_qpc_frequency.QuadPart;
	return whole_seconds * 1000000000ULL + (remainder * 1000000000ULL) / g_qpc_frequency.QuadPart;
#elif defined(SKA_PLATFORM_MACOS)
	static mach_timebase_info_data_t timebase = {0};
	if (timebase.denom == 0) {
		mach_timebase_info(&timebase);
	}
	return (mach_absolute_time() * timebase.numer) / timebase.denom;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

SKA_API uint64_t ska_time_get_elapsed_ns(void) {
	return ska_get_time_ns() - g_ska.start_time;
}

SKA_API double ska_time_get_elapsed_s(void) {
	return (double)ska_time_get_elapsed_ns() / 1000000000.0;
}

SKA_API void ska_time_sleep(uint32_t ms) {
#ifdef SKA_PLATFORM_WIN32
	Sleep(ms);
#else
	usleep(ms * 1000);
#endif
}

// ============================================================================
// Clipboard Support
// ============================================================================

SKA_API char* ska_clipboard_get_text(void) {
	return ska_platform_clipboard_get_text();
}

SKA_API bool ska_clipboard_set_text(const char* text) {
	if (!text) {
		ska_set_error("ska_clipboard_set_text: text cannot be NULL");
		return false;
	}
	return ska_platform_clipboard_set_text(text);
}

// ============================================================================
// Logging
// ============================================================================

SKA_API void ska_log(ska_log_ level, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);

#ifdef SKA_PLATFORM_ANDROID
	int android_level;
	switch (level) {
		case ska_log_info:  android_level = ANDROID_LOG_INFO;  break;
		case ska_log_warn:  android_level = ANDROID_LOG_WARN;  break;
		case ska_log_error: android_level = ANDROID_LOG_ERROR; break;
		default:            android_level = ANDROID_LOG_INFO;  break;
	}
	__android_log_vprint(android_level, "sk_app", fmt, args);
#else
	FILE* output = (level == ska_log_error) ? stderr : stdout;
	const char* prefix;
	switch (level) {
		case ska_log_info:  prefix = "[INFO] ";  break;
		case ska_log_warn:  prefix = "[WARN] ";  break;
		case ska_log_error: prefix = "[ERROR] "; break;
		default:            prefix = "[LOG] ";   break;
	}
	fprintf(output, "%s", prefix);
	vfprintf(output, fmt, args);
	fprintf(output, "\n");
#endif

	va_end(args);
}

// ============================================================================
// File Dialog
// ============================================================================

// Global file dialog state
ska_file_dialog_state_t g_ska_file_dialog = {0};

// MIME type to extension mapping for common types
static const struct {
	const char* mime;
	const char* exts;
} ska_mime_to_exts[] = {
	{ "*/*",              "*" },
	{ "image/*",          "*.png *.jpg *.jpeg *.gif *.bmp *.webp *.tiff *.ico" },
	{ "image/png",        "*.png" },
	{ "image/jpeg",       "*.jpg *.jpeg" },
	{ "image/gif",        "*.gif" },
	{ "image/bmp",        "*.bmp" },
	{ "image/webp",       "*.webp" },
	{ "text/*",           "*.txt *.md *.c *.h *.cpp *.hpp *.py *.js *.json *.xml *.html *.css" },
	{ "text/plain",       "*.txt" },
	{ "text/html",        "*.html *.htm" },
	{ "text/css",         "*.css" },
	{ "text/javascript",  "*.js" },
	{ "text/markdown",    "*.md" },
	{ "text/xml",         "*.xml" },
	{ "application/json", "*.json" },
	{ "application/pdf",  "*.pdf" },
	{ "application/zip",  "*.zip" },
	{ "audio/*",          "*.mp3 *.wav *.ogg *.flac *.aac *.m4a" },
	{ "audio/mpeg",       "*.mp3" },
	{ "audio/wav",        "*.wav" },
	{ "audio/ogg",        "*.ogg" },
	{ "video/*",          "*.mp4 *.avi *.mkv *.mov *.webm *.wmv" },
	{ "video/mp4",        "*.mp4" },
	{ "video/webm",       "*.webm" },
	{ NULL, NULL }
};

const char* ska_filter_get_exts(const ska_file_filter_t* filter) {
	if (!filter) return "*";

	// If exts provided, use it directly
	if (filter->exts && filter->exts[0]) {
		return filter->exts;
	}

	// Try to translate MIME type to extensions
	if (filter->mime && filter->mime[0]) {
		for (int32_t i = 0; ska_mime_to_exts[i].mime != NULL; i++) {
			if (strcmp(filter->mime, ska_mime_to_exts[i].mime) == 0) {
				return ska_mime_to_exts[i].exts;
			}
		}
	}

	// Default: all files
	return "*";
}

const char* ska_filter_get_mime(const ska_file_filter_t* filter) {
	if (!filter) return "*/*";

	// If mime provided, use it directly
	if (filter->mime && filter->mime[0]) {
		return filter->mime;
	}

	// Default: all files (we could try to reverse-translate from exts, but that's complex)
	return "*/*";
}

ska_file_dialog_result_t* ska_file_dialog_result_alloc(ska_file_dialog_id_t id, const char* title) {
	// Check for leaked results and warn
	for (int32_t i = 0; i < g_ska_file_dialog.result_count; i++) {
		if (!g_ska_file_dialog.results[i].freed) {
			ska_log(ska_log_warn, "File dialog result '%s' (id=%u) was not freed before new dialog",
			        g_ska_file_dialog.results[i].title ? g_ska_file_dialog.results[i].title : "(null)",
			        g_ska_file_dialog.results[i].id);
			g_ska_file_dialog.leaked_count++;
		}
	}

	// Clear old results if we're at capacity
	if (g_ska_file_dialog.result_count >= SKA_MAX_FILE_DIALOGS) {
		// Free all old results
		for (int32_t i = 0; i < g_ska_file_dialog.result_count; i++) {
			ska_file_dialog_result_t* r = &g_ska_file_dialog.results[i];
			if (r->title) ska_free(r->title);
			if (r->paths) {
				for (int32_t j = 0; j < r->path_count; j++) {
					if (r->paths[j]) ska_free(r->paths[j]);
				}
				ska_free(r->paths);
			}
		}
		g_ska_file_dialog.result_count = 0;
	}

	ska_file_dialog_result_t* result = &g_ska_file_dialog.results[g_ska_file_dialog.result_count++];
	memset(result, 0, sizeof(*result));
	result->id = id;
	result->title = title ? ska_strdup(title) : NULL;
	result->paths = NULL;
	result->path_count = 0;
	result->cancelled = false;
	result->freed = false;

	return result;
}

void ska_file_dialog_result_add_path(ska_file_dialog_result_t* result, const char* path) {
	if (!result || !path) return;

	if (result->path_count >= SKA_MAX_DIALOG_PATHS) {
		ska_log(ska_log_warn, "File dialog path limit reached (%d)", SKA_MAX_DIALOG_PATHS);
		return;
	}

	// Allocate or grow paths array
	if (!result->paths) {
		result->paths = (char**)ska_calloc(SKA_MAX_DIALOG_PATHS, sizeof(char*));
		if (!result->paths) return;
	}

	result->paths[result->path_count++] = ska_strdup(path);
}

void ska_file_dialog_result_complete(ska_file_dialog_result_t* result, bool cancelled) {
	if (!result) return;

	result->cancelled = cancelled;
	if (cancelled) {
		result->path_count = 0;
	}

	// Post event
	ska_event_t event = {0};
	event.type = ska_event_file_dialog;
	event.timestamp = ska_time_get_elapsed_ms();
	event.file_dialog.id = result->id;
	event.file_dialog.title = result->title;
	event.file_dialog.cancelled = result->cancelled;
	event.file_dialog.count = result->path_count;
	event.file_dialog._internal = result;

	ska_post_event(&event);
}

SKA_API bool ska_file_dialog_available(ska_file_dialog_ type) {
	return ska_platform_file_dialog_available(type);
}

SKA_API ska_file_dialog_id_t ska_file_dialog_show(const ska_file_dialog_request_t* request) {
	if (!request) {
		ska_set_error("ska_file_dialog_show: request is NULL");
		return 0;
	}

	if (!g_ska.initialized) {
		ska_set_error("ska_file_dialog_show: sk_app not initialized");
		return 0;
	}

	if (!ska_platform_file_dialog_available(request->type)) {
		ska_set_error("ska_file_dialog_show: file dialog type %d not available on this platform", request->type);
		return 0;
	}

	ska_file_dialog_id_t id = ++g_ska_file_dialog.next_id;
	if (id == 0) id = ++g_ska_file_dialog.next_id; // Skip 0

	if (!ska_platform_file_dialog_show(id, request)) {
		return 0;
	}

	return id;
}

SKA_API const char* ska_file_dialog_get_path(const ska_event_file_dialog_t* result, int32_t index) {
	if (!result || !result->_internal) return NULL;

	ska_file_dialog_result_t* internal = (ska_file_dialog_result_t*)result->_internal;
	if (index < 0 || index >= internal->path_count) return NULL;

	return internal->paths[index];
}

SKA_API void ska_file_dialog_free_result(ska_event_file_dialog_t* ref_result) {
	if (!ref_result || !ref_result->_internal) return;

	ska_file_dialog_result_t* internal = (ska_file_dialog_result_t*)ref_result->_internal;
	if (internal->freed) return; // Already freed

	// Free paths
	if (internal->paths) {
		for (int32_t i = 0; i < internal->path_count; i++) {
			if (internal->paths[i]) ska_free(internal->paths[i]);
		}
		ska_free(internal->paths);
		internal->paths = NULL;
	}

	// Free title
	if (internal->title) {
		ska_free(internal->title);
		internal->title = NULL;
	}

	internal->freed = true;
	internal->path_count = 0;

	// Clear the event data
	ref_result->_internal = NULL;
	ref_result->title = NULL;
	ref_result->count = 0;
}

// ============================================================================
// Platform-Specific Exports
// ============================================================================

#ifdef SKA_PLATFORM_WIN32
SKA_API void* ska_win32_get_hinstance(void) {
	return g_ska.hinstance;
}
#endif

#ifdef SKA_PLATFORM_LINUX
SKA_API void* ska_linux_get_x11_display(void) {
	return g_ska.x_display;
}

SKA_API void* ska_linux_get_wayland_display(void) {
	// Not implemented yet
	return NULL;
}
#endif

