//
// sk_app - File I/O utilities

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "ska_internal.h"

#ifdef SKA_PLATFORM_WIN32
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <direct.h>
#include <shlobj.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

#ifdef SKA_PLATFORM_MACOS
#include <mach-o/dyld.h>
#endif

#if defined(SKA_PLATFORM_LINUX) || defined(SKA_PLATFORM_MACOS)
#include <pwd.h>
#endif

#include <errno.h>
#include <limits.h>
// Not the platform PATH_MAX: MinGW's is 260, and MSVC has none
#define SKA_PATH_MAX 4096

// ============================================================================
// File I/O Implementation
// ============================================================================

SKA_API bool ska_file_read(const char* filename, void** out_data, size_t* out_size) {
	if (!filename) {
		ska_set_error("ska_file_read: NULL filename");
		return false;
	}

	if (!out_data) {
		ska_set_error("ska_file_read: NULL out_data");
		return false;
	}

#ifdef SKA_PLATFORM_ANDROID
	// content:// URIs from file pickers must go through ContentResolver
	if (strncmp(filename, "content://", 10) == 0)
		return ska_android_content_read(filename, out_data, out_size);
#endif

#ifdef SKA_PLATFORM_WIN32
	wchar_t* wfilename = ska_utf8_to_wide(filename);
	if (!wfilename) {
		ska_set_error("ska_file_read: Failed to convert filename to UTF-16");
		return false;
	}
	FILE* file = _wfopen(wfilename, L"rb");
	ska_free(wfilename);
#else
	FILE* file = fopen(filename, "rb");
#endif
	if (!file) {
		ska_set_error("ska_file_read: Failed to open '%s'", filename);
		return false;
	}

	// Get file size
	fseek(file, 0, SEEK_END);
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);

	if (file_size < 0) {
		ska_set_error("ska_file_read: Failed to get file size for '%s'", filename);
		fclose(file);
		return false;
	}

	// Allocate buffer
	void* data = ska_malloc((size_t)file_size);
	if (!data) {
		ska_set_error("ska_file_read: Failed to allocate %ld bytes", file_size);
		fclose(file);
		return false;
	}

	// Read file
	size_t bytes_read = fread(data, 1, (size_t)file_size, file);
	fclose(file);

	if (bytes_read != (size_t)file_size) {
		ska_set_error("ska_file_read: Read %zu bytes, expected %ld", bytes_read, file_size);
		ska_free(data);
		return false;
	}

	*out_data = data;
	if (out_size) {
		*out_size = (size_t)file_size;
	}

	return true;
}

SKA_API bool ska_file_read_text(const char* filename, char** out_text) {
	if (!filename) {
		ska_set_error("ska_file_read_text: NULL filename");
		return false;
	}

	if (!out_text) {
		ska_set_error("ska_file_read_text: NULL out_text");
		return false;
	}

	size_t file_size = 0;
	void* data = NULL;
	if (!ska_file_read(filename, &data, &file_size)) {
		return false;
	}

	// Allocate +1 for null terminator
	char* text = (char*)ska_realloc(data, file_size + 1);
	if (!text) {
		ska_set_error("ska_file_read_text: Failed to allocate null terminator");
		ska_free(data);
		return false;
	}

	text[file_size] = '\0';
	*out_text = text;
	return true;
}

SKA_API bool ska_file_write(const char* filename, const void* data, size_t size) {
	if (!filename) {
		ska_set_error("ska_file_write: NULL filename");
		return false;
	}

	if (!data && size > 0) {
		ska_set_error("ska_file_write: NULL data with non-zero size");
		return false;
	}

#ifdef SKA_PLATFORM_ANDROID
	// content:// URIs from file pickers must go through ContentResolver
	if (strncmp(filename, "content://", 10) == 0)
		return ska_android_content_write(filename, data, size);
#endif

#ifdef SKA_PLATFORM_WIN32
	wchar_t* wfilename = ska_utf8_to_wide(filename);
	if (!wfilename) {
		ska_set_error("ska_file_write: Failed to convert filename to UTF-16");
		return false;
	}
	FILE* file = _wfopen(wfilename, L"wb");
	ska_free(wfilename);
#else
	FILE* file = fopen(filename, "wb");
#endif
	if (!file) {
		ska_set_error("ska_file_write: Failed to open '%s' for writing", filename);
		return false;
	}

	if (size > 0) {
		size_t bytes_written = fwrite(data, 1, size, file);
		fclose(file);

		if (bytes_written != size) {
			ska_set_error("ska_file_write: Wrote %zu bytes, expected %zu", bytes_written, size);
			return false;
		}
	} else {
		fclose(file);
	}

	return true;
}

SKA_API bool ska_file_write_text(const char* filename, const char* text) {
	if (!text) {
		ska_set_error("ska_file_write_text: NULL text");
		return false;
	}

	return ska_file_write(filename, text, strlen(text));
}

SKA_API void ska_file_free_data(void* data) {
	ska_free(data);
}

SKA_API bool ska_file_exists(const char* filename) {
	if (!filename) {
		return false;
	}

#ifdef SKA_PLATFORM_ANDROID
	// content:// URIs can't be checked via access(), but if we have one, it
	// came from a picker or API and should be treated as existing.
	if (strncmp(filename, "content://", 10) == 0) return true;
#endif

#ifdef SKA_PLATFORM_WIN32
	// Windows: use _waccess for Unicode support
	wchar_t* wfilename = ska_utf8_to_wide(filename);
	if (!wfilename) {
		return false;
	}
	int result = _waccess(wfilename, F_OK);
	ska_free(wfilename);
	return result == 0;
#else
	// POSIX: use access
	return access(filename, F_OK) == 0;
#endif
}

SKA_API size_t ska_file_size(const char* filename) {
	if (!filename) {
		return 0;
	}

#ifdef SKA_PLATFORM_WIN32
	// Windows: use _wstat for Unicode support
	wchar_t* wfilename = ska_utf8_to_wide(filename);
	if (!wfilename) {
		return 0;
	}
	struct _stat st;
	int result = _wstat(wfilename, &st);
	ska_free(wfilename);
	if (result != 0) {
		return 0;
	}
	return (size_t)st.st_size;
#else
	// POSIX: use stat
	struct stat st;
	if (stat(filename, &st) != 0) {
		return 0;
	}
	return (size_t)st.st_size;
#endif
}

// ============================================================================
// Directory Iteration
// ============================================================================

SKA_API bool ska_dir_iterate(const char* path, void* opt_context, ska_dir_iterate_fn callback) {
	if (!path) {
		ska_set_error("ska_dir_iterate: NULL path");
		return false;
	}
	if (!callback) {
		ska_set_error("ska_dir_iterate: NULL callback");
		return false;
	}

#ifdef SKA_PLATFORM_WIN32
	// Windows: use FindFirstFileW/FindNextFileW for Unicode support
	// Convert path to wide string and append \*
	wchar_t* wpath = ska_utf8_to_wide(path);
	if (!wpath) {
		ska_set_error("ska_dir_iterate: Failed to convert path to UTF-16");
		return false;
	}

	size_t wpath_len = wcslen(wpath);
	wchar_t* wsearch_path = (wchar_t*)ska_malloc((wpath_len + 3) * sizeof(wchar_t));
	if (!wsearch_path) {
		ska_free(wpath);
		ska_set_error("ska_dir_iterate: Failed to allocate memory");
		return false;
	}

	// Build search pattern: path\*
	wcscpy(wsearch_path, wpath);
	ska_free(wpath);

	if (wpath_len > 0 && wsearch_path[wpath_len - 1] != L'\\' && wsearch_path[wpath_len - 1] != L'/') {
		wsearch_path[wpath_len    ] = L'\\';
		wsearch_path[wpath_len + 1] = L'*';
		wsearch_path[wpath_len + 2] = L'\0';
	} else {
		wsearch_path[wpath_len    ] = L'*';
		wsearch_path[wpath_len + 1] = L'\0';
	}

	WIN32_FIND_DATAW find_data;
	HANDLE find_handle = FindFirstFileW(wsearch_path, &find_data);
	ska_free(wsearch_path);

	if (find_handle == INVALID_HANDLE_VALUE) {
		DWORD err = GetLastError();
		if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
			ska_set_error("ska_dir_iterate: Directory not found '%s'", path);
		} else {
			ska_set_error("ska_dir_iterate: Failed to open directory '%s' (error %lu)", path, err);
		}
		return false;
	}

	do {
		// Skip "." and ".."
		if (find_data.cFileName[0] == L'.') {
			if (find_data.cFileName[1] == L'\0') continue;
			if (find_data.cFileName[1] == L'.' && find_data.cFileName[2] == L'\0') continue;
		}

		// Convert filename to UTF-8
		char* utf8_name = ska_wide_to_utf8(find_data.cFileName);
		if (!utf8_name) continue;

		ska_dir_entry_t dir_entry;
		dir_entry.name   = utf8_name;
		dir_entry.is_dir = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		dir_entry.size   = dir_entry.is_dir ? 0 : ((size_t)find_data.nFileSizeHigh << 32) | find_data.nFileSizeLow;

		bool should_continue = callback(opt_context, &dir_entry);
		ska_free(utf8_name);

		if (!should_continue) {
			FindClose(find_handle);
			return true; // Callback requested stop, not an error
		}
	} while (FindNextFileW(find_handle, &find_data));

	FindClose(find_handle);
	return true;

#else
	// POSIX: use opendir/readdir (Linux, macOS, Android)
	DIR* dir = opendir(path);
	if (!dir) {
		ska_set_error("ska_dir_iterate: Failed to open directory '%s'", path);
		return false;
	}

	// Pre-allocate buffer for full paths (reused across iterations)
	size_t path_len = strlen(path);
	size_t buffer_size = path_len + 1 + 256 + 1;  // path + '/' + NAME_MAX + null
	char*  full_path = (char*)ska_malloc(buffer_size);
	if (!full_path) {
		closedir(dir);
		ska_set_error("ska_dir_iterate: Failed to allocate memory");
		return false;
	}
	memcpy(full_path, path, path_len);
	full_path[path_len] = '/';

	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		// Skip "." and ".."
		if (entry->d_name[0] == '.') {
			if (entry->d_name[1] == '\0') continue;
			if (entry->d_name[1] == '.' && entry->d_name[2] == '\0') continue;
		}

		ska_dir_entry_t dir_entry;
		dir_entry.name   = entry->d_name;
		dir_entry.is_dir = false;
		dir_entry.size   = 0;

		// d_type is a dirent field that gives the entry type (file/dir) without
		// needing stat(). Not all filesystems support it (returns DT_UNKNOWN).
		bool need_stat = true;
#if defined(_DIRENT_HAVE_D_TYPE) || defined(SKA_PLATFORM_ANDROID)
		if (entry->d_type != DT_UNKNOWN) {
			dir_entry.is_dir = (entry->d_type == DT_DIR);
			need_stat        = !dir_entry.is_dir; // Still need stat for file size
		}
#endif

		if (need_stat) {
			// Build full path for stat
			size_t name_len = strlen(entry->d_name);

			// Reallocate if name is longer than expected
			if (path_len + 1 + name_len + 1 > buffer_size) {
				buffer_size = path_len + 1 + name_len + 1;
				char* new_buffer = (char*)ska_realloc(full_path, buffer_size);
				if (!new_buffer) continue;
				full_path = new_buffer;
			}
			memcpy(full_path + path_len + 1, entry->d_name, name_len + 1);

			struct stat st;
			if (stat(full_path, &st) == 0) {
				dir_entry.is_dir = S_ISDIR(st.st_mode);
				if (!dir_entry.is_dir) {
					dir_entry.size = (size_t)st.st_size;
				}
			}
		}

		if (!callback(opt_context, &dir_entry)) {
			ska_free(full_path);
			closedir(dir);
			return true;  // Callback requested stop, not an error
		}
	}

	ska_free(full_path);
	closedir(dir);
	return true;
#endif
}

// ============================================================================
// Common Paths
// ============================================================================

#ifndef SKA_PLATFORM_ANDROID
static bool ska_path_copy(const char* src, char* out_buffer, size_t buffer_size) {
	size_t len = strlen(src);
	if (len >= buffer_size) {
		ska_set_error("ska_path_get: Buffer too small");
		return false;
	}
	memcpy(out_buffer, src, len + 1);
	return true;
}
#endif

#ifdef SKA_PLATFORM_WIN32
#define SKA_PATH_SEP "\\"

static bool ska_path_from_wide(const wchar_t* wpath, char* out_buffer, size_t buffer_size) {
	char* utf8 = ska_wide_to_utf8(wpath);
	if (!utf8) {
		ska_set_error("ska_path_get: Failed to convert path to UTF-8");
		return false;
	}
	bool result = ska_path_copy(utf8, out_buffer, buffer_size);
	ska_free(utf8);
	return result;
}
#else
#define SKA_PATH_SEP "/"
#endif

static bool ska_path_cwd_get(char* out_buffer, size_t buffer_size) {
#ifdef SKA_PLATFORM_WIN32
	wchar_t* wcwd = _wgetcwd(NULL, 0);
	if (!wcwd) {
		ska_set_error("ska_path_get: _wgetcwd failed");
		return false;
	}
	bool result = ska_path_from_wide(wcwd, out_buffer, buffer_size);
	free(wcwd); // _wgetcwd allocates with malloc, not ska_malloc
	return result;
#else
	if (!getcwd(out_buffer, buffer_size)) {
		ska_set_error("ska_path_get: getcwd failed");
		return false;
	}
	return true;
#endif
}

static bool ska_exe_file_get(char* out_buffer, size_t buffer_size) {
#if defined(SKA_PLATFORM_WIN32)
	// Long path aware installs can pass MAX_PATH. A full buffer means truncation.
	const DWORD capacity = 32768;
	wchar_t*    wpath    = (wchar_t*)ska_malloc(capacity * sizeof(wchar_t));
	DWORD       len      = GetModuleFileNameW(NULL, wpath, capacity);
	bool        result   = len > 0 && len < capacity;
	if (!result) ska_set_error("ska_path_get: GetModuleFileNameW failed");
	else         result = ska_path_from_wide(wpath, out_buffer, buffer_size);
	ska_free(wpath);
	if (!result) return false;
#elif defined(SKA_PLATFORM_LINUX)
	// A result that fills the buffer may have been truncated
	ssize_t len = readlink("/proc/self/exe", out_buffer, buffer_size - 1);
	if (len < 0 || (size_t)len >= buffer_size - 1) {
		ska_set_error("ska_path_get: readlink failed");
		return false;
	}
	out_buffer[len] = '\0';
#elif defined(SKA_PLATFORM_MACOS)
	// May be relative to the launch folder, which breaks once the cwd changes
	char     raw [SKA_PATH_MAX];
	char     real[SKA_PATH_MAX];
	uint32_t size = sizeof(raw);
	if (_NSGetExecutablePath(raw, &size) != 0 || !realpath(raw, real)) {
		ska_set_error("ska_path_get: _NSGetExecutablePath failed");
		return false;
	}
	if (!ska_path_copy(real, out_buffer, buffer_size)) return false;
#else
	ska_set_error("ska_path_get: No executable folder on this platform");
	return false;
#endif
	return true;
}

static bool ska_path_exe_get(char* out_buffer, size_t buffer_size) {
	if (!ska_exe_file_get(out_buffer, buffer_size)) return false;

	char* last_sep = strrchr(out_buffer, SKA_PATH_SEP[0]);
	if (last_sep) *last_sep = '\0';
	return true;
}

bool ska_exe_name_get(char* out_buffer, size_t buffer_size) {
#if defined(SKA_PLATFORM_ANDROID) || defined(SKA_PLATFORM_WEB)
	return false;
#else
	char path[SKA_PATH_MAX];
	if (!ska_exe_file_get(path, sizeof(path))) return false;

	char* last_sep = strrchr(path, SKA_PATH_SEP[0]);
	char* name     = last_sep ? last_sep + 1 : path;
#ifdef SKA_PLATFORM_WIN32
	char* ext = strrchr(name, '.');
	if (ext) *ext = '\0';
#endif
	return ska_path_copy(name, out_buffer, buffer_size);
#endif
}

// Android resolves everything below through its Context instead
#ifndef SKA_PLATFORM_ANDROID

static bool ska_path_join(const char* a, const char* b, char* out_buffer, size_t buffer_size) {
	int32_t len = snprintf(out_buffer, buffer_size, "%s" SKA_PATH_SEP "%s", a, b);
	if (len < 0 || (size_t)len >= buffer_size) {
		ska_set_error("ska_path_get: Buffer too small");
		return false;
	}
	return true;
}

// Strips trailing separators, keeping a root like "/" or "C:\".
static void ska_path_trim(char* ref_path) {
	size_t len  = strlen(ref_path);
	size_t root = len >= 3 && ref_path[1] == ':' ? 3 : 1;
	while (len > root && (ref_path[len - 1] == '/' || ref_path[len - 1] == '\\'))
		ref_path[--len] = '\0';
}

static bool ska_dir_make_one(const char* path) {
#ifdef SKA_PLATFORM_WIN32
	wchar_t* wpath = ska_utf8_to_wide(path);
	if (!wpath) return false;
	int32_t result = _wmkdir(wpath);
	ska_free(wpath);
	return result == 0 || errno == EEXIST;
#else
	return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

// Creates a folder and any missing parents. Parents like "C:" or "/home" fail
// harmlessly, so only the final folder's result counts.
static bool ska_dir_make(char* ref_path) {
	for (char* p = ref_path + 1; *p; p++) {
		if (*p != '/' && *p != '\\') continue;
		char sep = *p;
		*p = '\0';
		ska_dir_make_one(ref_path);
		*p = sep;
	}
	return ska_dir_make_one(ref_path);
}

#ifdef SKA_PLATFORM_WIN32
static bool ska_path_shell(int32_t csidl, char* out_buffer, size_t buffer_size) {
	wchar_t wpath[MAX_PATH];
	if (FAILED(SHGetFolderPathW(NULL, csidl | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, wpath))) {
		ska_set_error("ska_path_get: SHGetFolderPathW failed");
		return false;
	}
	return ska_path_from_wide(wpath, out_buffer, buffer_size);
}
#endif

#if defined(SKA_PLATFORM_LINUX) || defined(SKA_PLATFORM_MACOS)
// Sets an error when there is no home folder.
static const char* ska_home_dir(void) {
	const char* home = getenv("HOME");
	if (!home || home[0] == '\0') {
		struct passwd* pw = getpwuid(getuid());
		home = pw ? pw->pw_dir : NULL;
	}
	if (!home) ska_set_error("ska_path_get: Unable to find the home folder");
	return home;
}

static bool ska_path_home_join(const char* rel, char* out_buffer, size_t buffer_size) {
	const char* home = ska_home_dir();
	return home && ska_path_join(home, rel, out_buffer, buffer_size);
}
#endif

#ifdef SKA_PLATFORM_LINUX
// The XDG spec says relative values are invalid and should be ignored.
static bool ska_path_xdg(const char* env, const char* home_rel, char* out_buffer, size_t buffer_size) {
	const char* xdg = getenv(env);
	if (xdg && xdg[0] == '/') return ska_path_copy(xdg, out_buffer, buffer_size);
	return ska_path_home_join(home_rel, out_buffer, buffer_size);
}

// Finds a KEY="value" line in user-dirs.dirs text, terminating the value in place.
static const char* ska_path_user_dirs_find(char* ref_text, const char* key) {
	size_t key_len = strlen(key);
	for (char* line = ref_text; line; ) {
		char* next = strchr(line, '\n');
		if (next) *next++ = '\0';
		if (strncmp(line, key, key_len) == 0 && strncmp(line + key_len, "=\"", 2) == 0) {
			char* value = line + key_len + 2;
			char* end   = strchr(value, '"');
			if (end) *end = '\0';
			return value;
		}
		line = next;
	}
	return NULL;
}

// XDG user folders are set in user-dirs.dirs, as "$HOME/..." or absolute paths.
static bool ska_path_xdg_user(const char* key, const char* home_rel, char* out_buffer, size_t buffer_size) {
	char       dirs_path[SKA_PATH_MAX];
	char*      text     = NULL;
	const char suffix[] = "/user-dirs.dirs";
	if (ska_path_xdg("XDG_CONFIG_HOME", ".config", dirs_path, sizeof(dirs_path) - sizeof(suffix))) {
		strcat(dirs_path, suffix);
		if (ska_file_exists(dirs_path)) ska_file_read_text(dirs_path, &text);
	}

	const char* value = text ? ska_path_user_dirs_find(text, key) : NULL;
	bool        result;
	// A bare "$HOME" means the folder is disabled and resolves to home
	if      (value && strcmp (value, "$HOME")     == 0) result = ska_path_home_join("",        out_buffer, buffer_size);
	else if (value && strncmp(value, "$HOME/", 6) == 0) result = ska_path_home_join(value + 6, out_buffer, buffer_size);
	else if (value && value[0] == '/')                  result = ska_path_copy     (value,     out_buffer, buffer_size);
	else                                                result = ska_path_home_join(home_rel,  out_buffer, buffer_size);
	ska_file_free_data(text);
	return result;
}
#endif

// Shared per-user folders, which belong to the user rather than the app.
static bool ska_path_user_get(ska_path_ path, char* out_buffer, size_t buffer_size) {
#if defined(SKA_PLATFORM_WIN32)
	if (!ska_path_shell(path == ska_path_home ? CSIDL_PROFILE : CSIDL_PERSONAL, out_buffer, buffer_size)) return false;
#elif defined(SKA_PLATFORM_LINUX) || defined(SKA_PLATFORM_MACOS)
	if (path == ska_path_home) {
		const char* home = ska_home_dir();
		if (!home || !ska_path_copy(home, out_buffer, buffer_size)) return false;
	} else {
#ifdef SKA_PLATFORM_LINUX
		if (!ska_path_xdg_user("XDG_DOCUMENTS_DIR", "Documents", out_buffer, buffer_size)) return false;
#else
		if (!ska_path_home_join("Documents", out_buffer, buffer_size)) return false;
#endif
	}
#else
	ska_set_error("ska_path_get: No user folders on this platform");
	return false;
#endif
	ska_path_trim(out_buffer);
	return true;
}

// Per-user folder that the per-app folders live in.
static bool ska_path_app_root(ska_path_ path, char* out_buffer, size_t buffer_size) {
#if defined(SKA_PLATFORM_WIN32)
	return ska_path_shell(path == ska_path_cache ? CSIDL_LOCAL_APPDATA : CSIDL_APPDATA, out_buffer, buffer_size);
#elif defined(SKA_PLATFORM_LINUX)
	switch (path) {
	case ska_path_data:   return ska_path_xdg("XDG_DATA_HOME",   ".local/share", out_buffer, buffer_size);
	case ska_path_config: return ska_path_xdg("XDG_CONFIG_HOME", ".config",      out_buffer, buffer_size);
	default:              return ska_path_xdg("XDG_CACHE_HOME",  ".cache",       out_buffer, buffer_size);
	}
#elif defined(SKA_PLATFORM_MACOS)
	const char* rel = path == ska_path_cache ? "Library/Caches" : "Library/Application Support";
	return ska_path_home_join(rel, out_buffer, buffer_size);
#elif defined(SKA_PLATFORM_WEB)
	// MEMFS is wiped on reload, which only a cache can tolerate
	if (path != ska_path_cache) {
		ska_set_error("ska_path_get: No persistent file system on the web");
		return false;
	}
	return ska_path_copy("/tmp", out_buffer, buffer_size);
#else
	ska_set_error("ska_path_get: Not supported on this platform");
	return false;
#endif
}

static bool ska_path_app_get(ska_path_ path, char* out_buffer, size_t buffer_size) {
	char root[SKA_PATH_MAX];
	if (!ska_path_app_root(path, root, sizeof(root))) return false;
	ska_path_trim(root);

	if (!ska_path_join(root, ska_app_name(), out_buffer, buffer_size)) return false;
	if (!ska_dir_make(out_buffer)) {
		ska_set_error("ska_path_get: Failed to create '%s'", out_buffer);
		return false;
	}
	return true;
}

static bool ska_path_temp_get(char* out_buffer, size_t buffer_size) {
#ifdef SKA_PLATFORM_WIN32
	wchar_t wpath[MAX_PATH + 1];
	DWORD   len = GetTempPathW(MAX_PATH + 1, wpath);
	if (len == 0 || len > MAX_PATH) {
		ska_set_error("ska_path_get: GetTempPathW failed");
		return false;
	}
	if (!ska_path_from_wide(wpath, out_buffer, buffer_size)) return false;
#else
	const char* tmp = getenv("TMPDIR");
	if (!ska_path_copy(tmp && tmp[0] == '/' ? tmp : "/tmp", out_buffer, buffer_size)) return false;
#endif
	ska_path_trim(out_buffer);
	return true;
}

#endif // !SKA_PLATFORM_ANDROID

SKA_API bool ska_path_get(ska_path_ path, char* out_buffer, size_t buffer_size) {
	if (buffer_size == 0) {
		ska_set_error("ska_path_get: Empty buffer");
		return false;
	}
	bool per_app = path == ska_path_data || path == ska_path_config || path == ska_path_cache;
	if (per_app && !g_ska.initialized) {
		ska_set_error("ska_path_get: Per-app folders require ska_init, which sets the app_id they are named by");
		out_buffer[0] = '\0';
		return false;
	}

	bool result = false;
	switch (path) {
	case ska_path_cwd:    result = ska_path_cwd_get(out_buffer, buffer_size); break;
	case ska_path_exe:    result = ska_path_exe_get(out_buffer, buffer_size); break;
#ifdef SKA_PLATFORM_ANDROID
	case ska_path_data:
	case ska_path_config:
	case ska_path_cache:
	case ska_path_temp:   result = ska_android_path_get(path, out_buffer, buffer_size); break;
	case ska_path_home:
	case ska_path_documents: ska_set_error("ska_path_get: No user folders on Android"); break;
#else
	case ska_path_data:
	case ska_path_config:
	case ska_path_cache:  result = ska_path_app_get (path, out_buffer, buffer_size); break;
	case ska_path_temp:   result = ska_path_temp_get(      out_buffer, buffer_size); break;
	case ska_path_home:
	case ska_path_documents: result = ska_path_user_get(path, out_buffer, buffer_size); break;
#endif
	default: ska_set_error("ska_path_get: Unknown path %d", (int32_t)path); break;
	}

	if (!result) out_buffer[0] = '\0';
	return result;
}

SKA_API bool ska_set_cwd(const char* opt_path) {
#ifdef SKA_PLATFORM_ANDROID
	ska_set_error("ska_set_cwd: Not supported on Android");
	return false;
#else

	char path_buffer[SKA_PATH_MAX];

	if (opt_path == NULL) {
		if (!ska_path_get(ska_path_exe, path_buffer, sizeof(path_buffer))) return false;
		opt_path = path_buffer;
	}

#ifdef SKA_PLATFORM_WIN32
	// Use _wchdir for Unicode support
	wchar_t* wpath = ska_utf8_to_wide(opt_path);
	if (!wpath) {
		ska_set_error("ska_set_cwd: Failed to convert path to UTF-16");
		return false;
	}
	int result = _wchdir(wpath);
	ska_free(wpath);
	if (result != 0) {
		ska_set_error("ska_set_cwd: _wchdir failed for '%s'", opt_path);
		return false;
	}
#else
	if (chdir(opt_path) != 0) {
		ska_set_error("ska_set_cwd: chdir failed for '%s'", opt_path);
		return false;
	}
#endif

	return true;

#endif // !SKA_PLATFORM_ANDROID
}

// ============================================================================
// Asset I/O Implementation (non-Android)
// ============================================================================

#ifndef SKA_PLATFORM_ANDROID

SKA_API bool ska_asset_read(const char* asset_name, void** out_data, size_t* out_size) {
	if (!asset_name) {
		ska_set_error("ska_asset_read: NULL asset_name");
		return false;
	}

	if (!out_data) {
		ska_set_error("ska_asset_read: NULL out_data");
		return false;
	}

	// Build path: try "Assets/" first, then "assets/"
	size_t name_len = strlen(asset_name);
	char* path = (char*)ska_malloc(8 + name_len + 1);  // "Assets/" or "assets/" + name + null
	if (!path) {
		ska_set_error("ska_asset_read: Failed to allocate path buffer");
		return false;
	}

	// Try "Assets/" first
	snprintf(path, 8 + name_len + 1, "Assets/%s", asset_name);
	if (ska_file_exists(path)) {
		bool result = ska_file_read(path, out_data, out_size);
		ska_free(path);
		return result;
	}

#ifndef SKA_PLATFORM_WIN32
	// Try "assets/" as fallback on case-sensitive filesystems. Windows is
	// case-insensitive, so the "Assets/" check above already covered this.
	snprintf(path, 8 + name_len + 1, "assets/%s", asset_name);
	if (ska_file_exists(path)) {
		bool result = ska_file_read(path, out_data, out_size);
		ska_free(path);
		return result;
	}
#endif

	ska_free(path);
	return false;
}

SKA_API size_t ska_asset_size(const char* asset_name) {
	if (!asset_name) return 0;

	size_t name_len = strlen(asset_name);
	char*  path     = (char*)ska_malloc(8 + name_len + 1);
	if (!path) return 0;

	snprintf(path, 8 + name_len + 1, "Assets/%s", asset_name);
	if (ska_file_exists(path)) {
		size_t result = ska_file_size(path);
		ska_free(path);
		return result;
	}

#ifndef SKA_PLATFORM_WIN32
	snprintf(path, 8 + name_len + 1, "assets/%s", asset_name);
	if (ska_file_exists(path)) {
		size_t result = ska_file_size(path);
		ska_free(path);
		return result;
	}
#endif

	ska_free(path);
	return 0;
}

SKA_API bool ska_asset_read_text(const char* asset_name, char** out_text) {
	if (!asset_name) {
		ska_set_error("ska_asset_read_text: NULL asset_name");
		return false;
	}

	if (!out_text) {
		ska_set_error("ska_asset_read_text: NULL out_text");
		return false;
	}

	size_t file_size = 0;
	void* data = NULL;
	if (!ska_asset_read(asset_name, &data, &file_size)) {
		return false;
	}

	// Allocate +1 for null terminator
	char* text = (char*)ska_realloc(data, file_size + 1);
	if (!text) {
		ska_set_error("ska_asset_read_text: Failed to allocate null terminator");
		ska_free(data);
		return false;
	}

	text[file_size] = '\0';
	*out_text = text;
	return true;
}

#endif // !SKA_PLATFORM_ANDROID
