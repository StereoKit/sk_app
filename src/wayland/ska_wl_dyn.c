//
// sk_app - Wayland runtime loading
//
// Defines the function pointers declared in ska_wl_dyn.h and resolves
// them with dlsym. This file must NOT reach the wayland-client.h shim: it needs
// the genuine names for __typeof__, not the redirected ones.

#define _POSIX_C_SOURCE 200809L
#include "ska_internal.h"

#if defined(SKA_PLATFORM_LINUX) && defined(SKA_LINUX_WAYLAND)

#include <wayland-client-core.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <dlfcn.h>

#define SKA_WL_SYM(name)        __typeof__(name)* ska_dyn_##name = NULL;
#define SKA_WL_CURSOR_SYM(name) __typeof__(name)* ska_dyn_##name = NULL;
#define SKA_XKB_SYM(name)       __typeof__(name)* ska_dyn_##name = NULL;
#define SKA_DECOR_SYM(name)     // libdecor is loaded by ska_decor.c
#include "ska_wl_syms.h"
#undef SKA_WL_SYM
#undef SKA_WL_CURSOR_SYM
#undef SKA_XKB_SYM
#undef SKA_DECOR_SYM

static void* g_wl_lib;
static void* g_wl_cursor_lib;
static void* g_xkb_lib;

// dlsym returns void*, which C forbids assigning to a function pointer, so the
// result goes through a void** alias of the pointer itself.
static bool ska_wl_dyn_sym(void* lib, const char* name, void* out_ptr) {
	void* sym = dlsym(lib, name);
	if (!sym) {
		ska_log(ska_log_warn, "Wayland: missing symbol %s", name);
		return false;
	}
	*(void**)out_ptr = sym;
	return true;
}

void ska_wl_dyn_unload(void) {
	if (g_xkb_lib)       dlclose(g_xkb_lib);
	if (g_wl_cursor_lib) dlclose(g_wl_cursor_lib);
	if (g_wl_lib)        dlclose(g_wl_lib);
	g_xkb_lib       = NULL;
	g_wl_cursor_lib = NULL;
	g_wl_lib        = NULL;
}

bool ska_wl_dyn_load(void) {
	if (g_wl_lib) return true;

	g_wl_lib        = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_LOCAL);
	g_wl_cursor_lib = dlopen("libwayland-cursor.so.0", RTLD_LAZY | RTLD_LOCAL);
	g_xkb_lib       = dlopen("libxkbcommon.so.0",      RTLD_LAZY | RTLD_LOCAL);
	if (!g_wl_lib || !g_wl_cursor_lib || !g_xkb_lib) {
		ska_wl_dyn_unload();
		return false;
	}

	bool ok = true;
	#define SKA_WL_SYM(name)        ok = ska_wl_dyn_sym(g_wl_lib,        #name, &ska_dyn_##name) && ok;
	#define SKA_WL_CURSOR_SYM(name) ok = ska_wl_dyn_sym(g_wl_cursor_lib, #name, &ska_dyn_##name) && ok;
	#define SKA_XKB_SYM(name)       ok = ska_wl_dyn_sym(g_xkb_lib,       #name, &ska_dyn_##name) && ok;
	#define SKA_DECOR_SYM(name)     // Optional, loaded separately below
	#include "ska_wl_syms.h"
	#undef SKA_WL_SYM
	#undef SKA_WL_CURSOR_SYM
	#undef SKA_XKB_SYM
	#undef SKA_DECOR_SYM

	if (!ok) {
		ska_wl_dyn_unload();
		return false;
	}

	return true;
}

#endif // SKA_PLATFORM_LINUX && SKA_LINUX_WAYLAND
