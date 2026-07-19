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

#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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
// Working Directory
// ============================================================================

SKA_API bool ska_get_cwd(char* ref_buffer, size_t buffer_size) {
	if (!ref_buffer || buffer_size == 0) {
		ska_set_error("ska_get_cwd: Invalid buffer");
		return false;
	}

	ref_buffer[0] = '\0';

#ifdef SKA_PLATFORM_WIN32
	// Use _wgetcwd for Unicode support, then convert to UTF-8
	wchar_t* wcwd = _wgetcwd(NULL, 0);
	if (wcwd == NULL) {
		ska_set_error("ska_get_cwd: _wgetcwd failed");
		return false;
	}
	char* utf8_cwd = ska_wide_to_utf8(wcwd);
	free(wcwd);  // _wgetcwd uses malloc internally
	if (!utf8_cwd) {
		ska_set_error("ska_get_cwd: Failed to convert path to UTF-8");
		return false;
	}
	size_t len = strlen(utf8_cwd);
	if (len >= buffer_size) {
		ska_free(utf8_cwd);
		ska_set_error("ska_get_cwd: Buffer too small");
		return false;
	}
	memcpy(ref_buffer, utf8_cwd, len + 1);
	ska_free(utf8_cwd);
#else
	if (getcwd(ref_buffer, buffer_size) == NULL) {
		ska_set_error("ska_get_cwd: getcwd failed");
		return false;
	}
#endif

	return true;
}

// Helper: Get directory portion of a path (modifies buffer in-place)
static void ska_path_get_directory(char* path) {
	if (!path || path[0] == '\0') return;

	// Find last separator
	char* last_sep = NULL;
	for (char* p = path; *p; p++) {
		if (*p == '/' || *p == '\\') {
			last_sep = p;
		}
	}

	if (last_sep) {
		*last_sep = '\0';
	} else {
		// No separator found, use current directory
		path[0] = '.';
		path[1] = '\0';
	}
}

SKA_API bool ska_get_exe_path(char* ref_buffer, size_t buffer_size) {
	if (!ref_buffer || buffer_size == 0) {
		ska_set_error("ska_get_exe_path: Invalid buffer");
		return false;
	}

	ref_buffer[0] = '\0';

#if defined(SKA_PLATFORM_WIN32)
	// Use GetModuleFileNameW for Unicode support, then convert to UTF-8
	wchar_t wpath[MAX_PATH];
	DWORD len = GetModuleFileNameW(NULL, wpath, MAX_PATH);
	if (len == 0 || len >= MAX_PATH) {
		ska_set_error("ska_get_exe_path: GetModuleFileNameW failed");
		return false;
	}
	char* utf8_path = ska_wide_to_utf8(wpath);
	if (!utf8_path) {
		ska_set_error("ska_get_exe_path: Failed to convert path to UTF-8");
		return false;
	}
	size_t utf8_len = strlen(utf8_path);
	if (utf8_len >= buffer_size) {
		ska_free(utf8_path);
		ska_set_error("ska_get_exe_path: Buffer too small");
		return false;
	}
	memcpy(ref_buffer, utf8_path, utf8_len + 1);
	ska_free(utf8_path);
	return true;

#elif defined(SKA_PLATFORM_LINUX)
	ssize_t len = readlink("/proc/self/exe", ref_buffer, buffer_size - 1);
	if (len < 0 || (size_t)len >= buffer_size) {
		ska_set_error("ska_get_exe_path: readlink failed");
		return false;
	}
	ref_buffer[len] = '\0';
	return true;

#elif defined(SKA_PLATFORM_MACOS)
	uint32_t size = (uint32_t)buffer_size;
	if (_NSGetExecutablePath(ref_buffer, &size) != 0) {
		ska_set_error("ska_get_exe_path: _NSGetExecutablePath failed");
		return false;
	}
	return true;

#elif defined(SKA_PLATFORM_ANDROID)
	ska_set_error("ska_get_exe_path: Not supported on Android");
	return false;

#elif defined(SKA_PLATFORM_WEB)
	ska_set_error("ska_get_exe_path: Not supported on the web");
	return false;

#else
	ska_set_error("ska_get_exe_path: Not supported on this platform");
	return false;
#endif
}

SKA_API bool ska_set_cwd(const char* opt_path) {
#ifdef SKA_PLATFORM_ANDROID
	ska_set_error("ska_set_cwd: Not supported on Android");
	return false;
#else

	char path_buffer[PATH_MAX];

	if (opt_path == NULL) {
		// Get executable directory
		if (!ska_get_exe_path(path_buffer, sizeof(path_buffer))) {
			ska_set_error("ska_set_cwd: ska_get_exe_path failed");
			return false;
		}
		ska_path_get_directory(path_buffer);
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
