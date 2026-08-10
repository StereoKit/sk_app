//
// sk_app - WebGPU surface creation
//
// One file for every platform: each build fills in the platform's
// WGPUSurfaceDescriptor chain and calls wgpuInstanceCreateSurface, which the
// application links via Dawn, wgpu-native, or Emscripten's emdawnwebgpu port.
// This file lives in its own translation unit so the static-library linker
// only pulls in the wgpuInstanceCreateSurface dependency when
// ska_wgpu_create_surface is actually used.

#include "ska_internal.h"

SKA_API bool ska_wgpu_create_surface(const ska_window_t* window, void* instance, void* out_surface) {
	if (!window || !instance || !out_surface) {
		ska_set_error("Invalid parameters for WebGPU surface creation");
		return false;
	}

	WGPUSurfaceDescriptor desc = {0};
	desc.label.data   = NULL;
	desc.label.length = WGPU_STRLEN;

#if defined(SKA_PLATFORM_WIN32)
	WGPUSurfaceSourceWindowsHWND source = {0};
	source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
	source.hinstance   = (void*)g_ska.hinstance;
	source.hwnd        = (void*)window->hwnd;
	desc.nextInChain   = &source.chain;

#elif defined(SKA_PLATFORM_LINUX)
	// Both descriptor types are declared up front so whichever branch runs has
	// storage that outlives the wgpuInstanceCreateSurface call below.
	#ifdef SKA_LINUX_X11
	WGPUSurfaceSourceXlibWindow     xlib_source = {0};
	#endif
	#ifdef SKA_LINUX_WAYLAND
	WGPUSurfaceSourceWaylandSurface wl_source   = {0};

	if (g_ska.backend == ska_linux_backend_wayland) {
		void* surface = ska_wl_get_native_handle(window);
		if (!surface) {
			ska_set_error("ska_wgpu_create_surface: window has no wl_surface");
			return false;
		}
		wl_source.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
		wl_source.display     = ska_wl_get_display();
		wl_source.surface     = surface;
		desc.nextInChain      = &wl_source.chain;
	} else
	#endif
	{
	#ifdef SKA_LINUX_X11
		if (!g_ska.x_display || !window->xwindow) {
			ska_set_error("ska_wgpu_create_surface: no X11 display/window available");
			return false;
		}
		xlib_source.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
		xlib_source.display     = (void*)g_ska.x_display;
		xlib_source.window      = (uint64_t)window->xwindow;
		desc.nextInChain        = &xlib_source.chain;
	#else
		ska_set_error("ska_wgpu_create_surface: no X11 backend in this build");
		return false;
	#endif
	}

#elif defined(SKA_PLATFORM_ANDROID)
	if (!window->native_window) {
		ska_set_error("ska_wgpu_create_surface: native window not available yet");
		return false;
	}
	WGPUSurfaceSourceAndroidNativeWindow source = {0};
	source.chain.sType = WGPUSType_SurfaceSourceAndroidNativeWindow;
	source.window      = (void*)window->native_window;
	desc.nextInChain   = &source.chain;

#elif defined(SKA_PLATFORM_WEB)
	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector source = {0};
	source.chain.sType     = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
	source.selector.data   = window->canvas_selector;
	source.selector.length = WGPU_STRLEN;
	desc.nextInChain       = &source.chain;

#elif defined(SKA_PLATFORM_MACOS)
	// Needs a CAMetalLayer attached to the window's content view, which the
	// macOS backend doesn't set up yet.
	ska_set_error("ska_wgpu_create_surface: not implemented on macOS yet");
	return false;

#else
	ska_set_error("ska_wgpu_create_surface: unsupported platform");
	return false;
#endif

#if defined(SKA_PLATFORM_WIN32) || defined(SKA_PLATFORM_LINUX) || defined(SKA_PLATFORM_ANDROID) || defined(SKA_PLATFORM_WEB)
	WGPUSurface surface = wgpuInstanceCreateSurface((WGPUInstance)instance, &desc);
	if (!surface) {
		ska_set_error("ska_wgpu_create_surface: wgpuInstanceCreateSurface failed");
		return false;
	}

	// Only after success: marked presenting, sk_app stops committing the
	// window's wl_surface, so marking a failed surface would freeze it.
	#if defined(SKA_PLATFORM_LINUX) && defined(SKA_LINUX_WAYLAND)
	if (g_ska.backend == ska_linux_backend_wayland) ska_wl_mark_presenting(window);
	#endif

	*(WGPUSurface*)out_surface = surface;
	return true;
#endif
}
