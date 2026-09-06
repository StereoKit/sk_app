// Dear ImGui sk_app Backend
// Platform backend for sk_app (window/input handling)
// C-compatible header for integration with both C and C++ code

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ska_window_t;
struct ska_event_t;

// Initialization and lifecycle
bool ImGui_ImplSkApp_Init         (struct ska_window_t* window);
void ImGui_ImplSkApp_Shutdown     (void);
void ImGui_ImplSkApp_NewFrame     (void);

// Event processing - call this for each event from ska_event_poll()
// Returns true if ImGui wants to capture this event
bool ImGui_ImplSkApp_ProcessEvent (const struct ska_event_t* event);

// Get the DPI scale factor for the window.
//
// The backend drives ImGui in pixels: io.DisplaySize is the drawable size and
// mouse input is scaled to match. The drawable is always the real pixel count,
// so scale the style and rasterize fonts by this factor, or the UI renders at
// the wrong size.
//
// Use this to scale font sizes when loading fonts:
//
//   float dpi_scale = ImGui_ImplSkApp_GetDpiScale();
//   ImFontConfig config;
//   config.SizePixels = 13.0f * dpi_scale;
//   io.Fonts->AddFontDefault(&config);
//   // Or when loading a custom font:
//   io.Fonts->AddFontFromFileTTF("font.ttf", 16.0f * dpi_scale);
//
// Note: Call this AFTER ImGui_ImplSkApp_Init() but BEFORE building the font atlas.
//
// To handle DPI changes at runtime (e.g., moving window between monitors):
// 1. Watch for ska_event_window_dpi_changed in your event loop
// 2. When received, rebuild fonts at the new scale. The renderer backend
//    re-uploads the atlas on its own.
float ImGui_ImplSkApp_GetDpiScale (void);

#ifdef __cplusplus
}
#endif
