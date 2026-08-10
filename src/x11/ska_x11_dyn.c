//
// sk_app - X11 runtime loading
//
// Defines the function pointers declared in ska_x11_dyn.h and resolves
// them with dlsym. This file must NOT include ska_x11_dyn.h: it needs the
// genuine Xlib names for __typeof__, not the redirected ones.

#define _POSIX_C_SOURCE 200809L
#include "ska_internal.h"

#if defined(SKA_PLATFORM_LINUX) && defined(SKA_LINUX_X11)

#include <X11/Xresource.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/sync.h>
#include <dlfcn.h>

#define SKA_X11_SYM(name)     __typeof__(name)* ska_dyn_##name = NULL;
#define SKA_XRANDR_SYM(name)  __typeof__(name)* ska_dyn_##name = NULL;
#define SKA_XCURSOR_SYM(name) __typeof__(name)* ska_dyn_##name = NULL;
#define SKA_XI2_SYM(name)     __typeof__(name)* ska_dyn_##name = NULL;
#define SKA_XEXT_SYM(name)    __typeof__(name)* ska_dyn_##name = NULL;
#include "ska_x11_syms.h"
#undef SKA_X11_SYM
#undef SKA_XRANDR_SYM
#undef SKA_XCURSOR_SYM
#undef SKA_XI2_SYM
#undef SKA_XEXT_SYM

static void* g_x11_lib;
static void* g_xrandr_lib;
static void* g_xcursor_lib;
static void* g_xi2_lib;
static void* g_xext_lib;

// dlsym returns void*, which C forbids assigning to a function pointer, so the
// result goes through a void** alias of the pointer itself.
static bool ska_x11_dyn_sym(void* lib, const char* name, void* out_ptr) {
	void* sym = dlsym(lib, name);
	if (!sym) {
		ska_log(ska_log_warn, "X11: missing symbol %s", name);
		return false;
	}
	*(void**)out_ptr = sym;
	return true;
}

void ska_x11_dyn_unload(void) {
	if (g_xcursor_lib) dlclose(g_xcursor_lib);
	if (g_xrandr_lib)  dlclose(g_xrandr_lib);
	if (g_x11_lib)     dlclose(g_x11_lib);
	if (g_xi2_lib)     dlclose(g_xi2_lib);
	if (g_xext_lib)    dlclose(g_xext_lib);
	g_xext_lib    = NULL;
	g_xi2_lib     = NULL;
	g_xcursor_lib = NULL;
	g_xrandr_lib  = NULL;
	g_x11_lib     = NULL;
}

bool ska_x11_dyn_load(void) {
	if (g_x11_lib) return true;

	g_x11_lib     = dlopen("libX11.so.6",     RTLD_LAZY | RTLD_LOCAL);
	g_xrandr_lib  = dlopen("libXrandr.so.2",  RTLD_LAZY | RTLD_LOCAL);
	g_xcursor_lib = dlopen("libXcursor.so.1", RTLD_LAZY | RTLD_LOCAL);
	// These two are optional: libXi is relative mouse mode only, libXext is
	// the resize frame-sync handshake only
	g_xi2_lib     = dlopen("libXi.so.6",      RTLD_LAZY | RTLD_LOCAL);
	g_xext_lib    = dlopen("libXext.so.6",    RTLD_LAZY | RTLD_LOCAL);
	if (!g_x11_lib || !g_xrandr_lib || !g_xcursor_lib) {
		ska_x11_dyn_unload();
		return false;
	}

	bool ok      = true;
	bool xi2_ok  = true;
	bool xext_ok = true;
	#define SKA_X11_SYM(name)     ok = ska_x11_dyn_sym(g_x11_lib,     #name, &ska_dyn_##name) && ok;
	#define SKA_XRANDR_SYM(name)  ok = ska_x11_dyn_sym(g_xrandr_lib,  #name, &ska_dyn_##name) && ok;
	#define SKA_XCURSOR_SYM(name) ok = ska_x11_dyn_sym(g_xcursor_lib, #name, &ska_dyn_##name) && ok;
	// The optional libraries' symbols do not gate the backend
	#define SKA_XI2_SYM(name)     xi2_ok  = g_xi2_lib  && ska_x11_dyn_sym(g_xi2_lib,  #name, &ska_dyn_##name) && xi2_ok;
	#define SKA_XEXT_SYM(name)    xext_ok = g_xext_lib && ska_x11_dyn_sym(g_xext_lib, #name, &ska_dyn_##name) && xext_ok;
	#include "ska_x11_syms.h"
	#undef SKA_X11_SYM
	#undef SKA_XRANDR_SYM
	#undef SKA_XCURSOR_SYM
	#undef SKA_XI2_SYM
	#undef SKA_XEXT_SYM
	if (!xi2_ok  && g_xi2_lib)  { dlclose(g_xi2_lib);  g_xi2_lib  = NULL; }
	if (!xext_ok && g_xext_lib) { dlclose(g_xext_lib); g_xext_lib = NULL; }

	if (!ok) {
		ska_x11_dyn_unload();
		return false;
	}
	return true;
}

bool ska_x11_dyn_has_xi2(void) {
	return g_xi2_lib != NULL;
}

bool ska_x11_dyn_has_xext(void) {
	return g_xext_lib != NULL;
}

#endif // SKA_PLATFORM_LINUX && SKA_LINUX_X11
