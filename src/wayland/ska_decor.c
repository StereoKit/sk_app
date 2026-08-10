//
// sk_app - libdecor runtime loading
//
// Defines the function pointers declared in ska_decor.h and resolves them
// with dlsym. This file must NOT include ska_decor.h: it needs the
// genuine libdecor names for __typeof__, not the redirected ones.

#define _POSIX_C_SOURCE 200809L
#include "ska_internal.h"

#if defined(SKA_PLATFORM_LINUX) && defined(SKA_LINUX_WAYLAND)

// The shim comes first so libdecor.h's own wayland-client.h include lands on a
// header that is already complete.
#include "wayland-client.h"
#include <libdecor.h>
#include <dlfcn.h>

#define SKA_WL_SYM(name)
#define SKA_WL_CURSOR_SYM(name)
#define SKA_XKB_SYM(name)
#define SKA_DECOR_SYM(name) __typeof__(name)* ska_dyn_##name = NULL;
#include "ska_wl_syms.h"
#undef SKA_WL_SYM
#undef SKA_WL_CURSOR_SYM
#undef SKA_XKB_SYM
#undef SKA_DECOR_SYM

static void* g_decor_lib;

void ska_wl_decor_unload(void) {
	if (g_decor_lib) dlclose(g_decor_lib);
	g_decor_lib = NULL;
}

bool ska_wl_decor_load(void) {
	if (g_decor_lib) return true;

	g_decor_lib = dlopen("libdecor-0.so.0", RTLD_LAZY | RTLD_LOCAL);
	if (!g_decor_lib) return false;

	// All or nothing: a partially bound table would fault on the first call
	// that happened to be missing.
	bool ok = true;
	#define SKA_WL_SYM(name)
	#define SKA_WL_CURSOR_SYM(name)
	#define SKA_XKB_SYM(name)
	#define SKA_DECOR_SYM(name) \
		*(void**)(&ska_dyn_##name) = dlsym(g_decor_lib, #name); \
		if (!ska_dyn_##name) ok = false;
	#include "ska_wl_syms.h"
	#undef SKA_WL_SYM
	#undef SKA_WL_CURSOR_SYM
	#undef SKA_XKB_SYM
	#undef SKA_DECOR_SYM

	if (!ok) {
		ska_wl_decor_unload();
		return false;
	}
	return true;
}

#endif // SKA_PLATFORM_LINUX && SKA_LINUX_WAYLAND
