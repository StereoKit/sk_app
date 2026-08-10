//
// sk_app - Linux backend-agnostic platform code
//
// The parts of the Linux platform layer that do not touch a display server at
// all, and so are shared verbatim by the X11 and Wayland backends rather than
// going through the ska_linux_vtable_t dispatch in ska_linux.c:
//
//   - Keysym to scancode translation, identical on X11 and xkbcommon
//   - File dialogs, which shell out to a zenity/kdialog subprocess
//   - The kvpstore, which is plain XDG config-directory file I/O
//   - The virtual keyboard stub (desktop Linux has none)

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "ska_internal.h"

#ifdef SKA_PLATFORM_LINUX

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ========== Keysym Translation ==========

// Keysym values are identical between X11 and xkbcommon, so both backends
// share this table and only the spelling of the constants differs.
#if defined(SKA_LINUX_WAYLAND)
	#include <xkbcommon/xkbcommon-keysyms.h>
	#define SKA_KEY(name) XKB_KEY_##name
#else
	#include <X11/keysym.h>
	#define SKA_KEY(name) XK_##name
#endif

ska_scancode_ ska_linux_keysym_to_scancode(uint32_t keysym) {
	// Letters (uppercase and lowercase)
	if (keysym >= SKA_KEY(a) && keysym <= SKA_KEY(z)) {
		return ska_scancode_a + (keysym - SKA_KEY(a));
	}
	if (keysym >= SKA_KEY(A) && keysym <= SKA_KEY(Z)) {
		return ska_scancode_a + (keysym - SKA_KEY(A));
	}

	// Numbers
	if (keysym >= SKA_KEY(1) && keysym <= SKA_KEY(9)) {
		return ska_scancode_1 + (keysym - SKA_KEY(1));
	}
	if (keysym == SKA_KEY(0)) return ska_scancode_0;

	// Function keys
	if (keysym >= SKA_KEY(F1) && keysym <= SKA_KEY(F12)) {
		return ska_scancode_f1 + (keysym - SKA_KEY(F1));
	}

	// Special keys
	switch (keysym) {
		case SKA_KEY(Return): return ska_scancode_return;
		case SKA_KEY(Escape): return ska_scancode_escape;
		case SKA_KEY(BackSpace): return ska_scancode_backspace;
		case SKA_KEY(Tab): return ska_scancode_tab;
		case SKA_KEY(space): return ska_scancode_space;
		case SKA_KEY(minus): return ska_scancode_minus;
		case SKA_KEY(equal): return ska_scancode_equals;
		case SKA_KEY(bracketleft): return ska_scancode_leftbracket;
		case SKA_KEY(bracketright): return ska_scancode_rightbracket;
		case SKA_KEY(backslash): return ska_scancode_backslash;
		case SKA_KEY(semicolon): return ska_scancode_semicolon;
		case SKA_KEY(apostrophe): return ska_scancode_apostrophe;
		case SKA_KEY(grave): return ska_scancode_grave;
		case SKA_KEY(comma): return ska_scancode_comma;
		case SKA_KEY(period): return ska_scancode_period;
		case SKA_KEY(slash): return ska_scancode_slash;
		case SKA_KEY(Caps_Lock): return ska_scancode_capslock;

		// Navigation
		case SKA_KEY(Print): return ska_scancode_printscreen;
		case SKA_KEY(Scroll_Lock): return ska_scancode_scrolllock;
		case SKA_KEY(Pause): return ska_scancode_pause;
		case SKA_KEY(Insert): return ska_scancode_insert;
		case SKA_KEY(Home): return ska_scancode_home;
		case SKA_KEY(Page_Up): return ska_scancode_pageup;
		case SKA_KEY(Delete): return ska_scancode_delete;
		case SKA_KEY(End): return ska_scancode_end;
		case SKA_KEY(Page_Down): return ska_scancode_pagedown;
		case SKA_KEY(Right): return ska_scancode_right;
		case SKA_KEY(Left): return ska_scancode_left;
		case SKA_KEY(Down): return ska_scancode_down;
		case SKA_KEY(Up): return ska_scancode_up;

		// Modifiers
		case SKA_KEY(Control_L): return ska_scancode_lctrl;
		case SKA_KEY(Shift_L): return ska_scancode_lshift;
		case SKA_KEY(Alt_L): return ska_scancode_lalt;
		case SKA_KEY(Super_L): return ska_scancode_lgui;
		case SKA_KEY(Control_R): return ska_scancode_rctrl;
		case SKA_KEY(Shift_R): return ska_scancode_rshift;
		case SKA_KEY(Alt_R): return ska_scancode_ralt;
		case SKA_KEY(Super_R): return ska_scancode_rgui;

		default: return ska_scancode_unknown;
	}
}

#undef SKA_KEY

// ========== Vulkan Loader ==========

// Dlopened by name rather than found with RTLD_DEFAULT, because an application
// using volk keeps vkGetInstanceProcAddr private instead of exporting it.
PFN_vkGetInstanceProcAddr ska_linux_vk_get_proc_addr(void) {
	static PFN_vkGetInstanceProcAddr cached = NULL;
	if (cached) return cached;

	void* module = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
	if (!module) module = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
	if (!module) {
		ska_set_error("Failed to load Vulkan .so");
		return NULL;
	}

	// dlsym returns void*, which C forbids converting to a function pointer
	union {
		void*                     obj;
		PFN_vkGetInstanceProcAddr func;
	} sym;
	sym.obj = dlsym(module, "vkGetInstanceProcAddr");
	if (!sym.obj) {
		ska_set_error("Failed to load vkGetInstanceProcAddr");
		dlclose(module);
		return NULL;
	}

	cached = sym.func;
	return cached;
}

// ========== Text Input Platform Functions ==========

void ska_platform_show_virtual_keyboard(bool visible, ska_text_input_type_ type) {
	// Desktop Linux has no virtual keyboard on either backend
	(void)visible;
	(void)type;
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
	char*                    buffer;   // Output streamed in across pumps
	size_t                   length;
	size_t                   capacity;
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
			static char filter_buf[8][256];
			int32_t     filter_max = (int32_t)(sizeof(filter_buf) / sizeof(filter_buf[0]));
			for (int32_t i = 0; i < request->filter_count && i < filter_max && argc < 28; i++) {
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

	g_linux_file_dialog.pid      = pid;
	g_linux_file_dialog.pipe_fd  = pipefd[0];
	g_linux_file_dialog.id       = id;
	g_linux_file_dialog.title    = request->title ? ska_strdup(request->title) : NULL;
	g_linux_file_dialog.buffer   = NULL;
	g_linux_file_dialog.length   = 0;
	g_linux_file_dialog.capacity = 0;
	g_linux_file_dialog.active   = true;

	return true;
}

// Drains whatever the dialog subprocess has written so far. Reading every pump
// rather than once at exit keeps a selection larger than the pipe capacity
// from deadlocking the child against a full pipe.
static void ska_linux_dialog_drain(void) {
	for (;;) {
		if (g_linux_file_dialog.length + 1 >= g_linux_file_dialog.capacity) {
			size_t grown = g_linux_file_dialog.capacity ? g_linux_file_dialog.capacity * 2 : 4096;
			char*  buf   = ska_realloc(g_linux_file_dialog.buffer, grown);
			if (!buf) return;
			g_linux_file_dialog.buffer   = buf;
			g_linux_file_dialog.capacity = grown;
		}
		ssize_t n = read(g_linux_file_dialog.pipe_fd,
			g_linux_file_dialog.buffer + g_linux_file_dialog.length,
			g_linux_file_dialog.capacity - g_linux_file_dialog.length - 1);
		if (n <= 0) return; // Drained (EAGAIN) or the child closed its end
		g_linux_file_dialog.length += (size_t)n;
	}
}

// Called from ska_platform_pump_events to check for dialog completion
void ska_linux_check_file_dialog(void) {
	if (!g_linux_file_dialog.active) return;

	ska_linux_dialog_drain();

	int status;
	pid_t result = waitpid(g_linux_file_dialog.pid, &status, WNOHANG);
	if (result == 0) return; // Still running

	// The child is gone; pick up anything written between the drain and exit
	ska_linux_dialog_drain();

	ska_file_dialog_result_t* dialog_result = ska_file_dialog_result_alloc(
		g_linux_file_dialog.id,
		g_linux_file_dialog.title
	);

	bool cancelled = true;
	if (result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && g_linux_file_dialog.buffer) {
		cancelled = false;
		g_linux_file_dialog.buffer[g_linux_file_dialog.length] = '\0';

		// Parse paths (separated by newlines)
		char* line = g_linux_file_dialog.buffer;
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
	if (g_linux_file_dialog.title)  ska_free(g_linux_file_dialog.title);
	if (g_linux_file_dialog.buffer) ska_free(g_linux_file_dialog.buffer);
	memset(&g_linux_file_dialog, 0, sizeof(g_linux_file_dialog));

	// Post the result event
	ska_file_dialog_result_complete(dialog_result, cancelled);
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

	// Create app dir. A truncated path would silently point somewhere else, so
	// it is an error rather than something to carry on with.
	char app_dir[PATH_MAX];
	int app_len = snprintf(app_dir, sizeof(app_dir), "%s/%s", config_dir, ska_kvpstore_get_app_name());
	if (app_len < 0 || (size_t)app_len >= sizeof(app_dir)) {
		ska_set_error("ska_kvpstore: config path is too long");
		return false;
	}
	if (stat(app_dir, &st) != 0) {
		if (mkdir(app_dir, 0755) != 0 && errno != EEXIST) {
			ska_set_error("ska_kvpstore: failed to create app directory");
			return false;
		}
	}

	int len = snprintf(buffer, buffer_size, "%s/%s", app_dir, key);
	if (len < 0 || (size_t)len >= buffer_size) {
		ska_set_error("ska_kvpstore: path for key '%s' is too long", key);
		return false;
	}
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
