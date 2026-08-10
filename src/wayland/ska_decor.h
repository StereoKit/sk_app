//
// sk_app - libdecor call redirection
//
// Client-side window decorations, for compositors that will not draw a frame.
// GNOME's mutter is the case that matters: it has no xdg-decoration at all, so
// without this a window has no titlebar, resize grips, or way to drag it.
//
// Kept apart from ska_wl_dyn.h because libdecor.h pulls in wayland-client.h,
// which resolves to the shim and would re-enter it before its macros exist.
// Must therefore be included after the shim.

#ifndef SKA_LINUX_DECOR_H
#define SKA_LINUX_DECOR_H

#include <stdbool.h>
#include <libdecor.h>

#define SKA_WL_SYM(name)
#define SKA_WL_CURSOR_SYM(name)
#define SKA_XKB_SYM(name)
#define SKA_DECOR_SYM(name) extern __typeof__(name)* ska_dyn_##name;
#include "ska_wl_syms.h"
#undef SKA_WL_SYM
#undef SKA_WL_CURSOR_SYM
#undef SKA_XKB_SYM
#undef SKA_DECOR_SYM

// Loads libdecor. Returns false when it is absent or incomplete, which costs
// window decorations and nothing else.
bool ska_wl_decor_load(void);
void ska_wl_decor_unload(void);

#define libdecor_new                            ska_dyn_libdecor_new
#define libdecor_unref                          ska_dyn_libdecor_unref
#define libdecor_decorate                       ska_dyn_libdecor_decorate
#define libdecor_frame_unref                    ska_dyn_libdecor_frame_unref
#define libdecor_frame_set_title                ska_dyn_libdecor_frame_set_title
#define libdecor_frame_set_app_id               ska_dyn_libdecor_frame_set_app_id
#define libdecor_frame_map                      ska_dyn_libdecor_frame_map
#define libdecor_frame_commit                   ska_dyn_libdecor_frame_commit
#define libdecor_frame_set_maximized            ska_dyn_libdecor_frame_set_maximized
#define libdecor_frame_unset_maximized          ska_dyn_libdecor_frame_unset_maximized
#define libdecor_frame_set_fullscreen           ska_dyn_libdecor_frame_set_fullscreen
#define libdecor_frame_unset_fullscreen         ska_dyn_libdecor_frame_unset_fullscreen
#define libdecor_frame_set_minimized            ska_dyn_libdecor_frame_set_minimized
#define libdecor_frame_set_min_content_size     ska_dyn_libdecor_frame_set_min_content_size
#define libdecor_frame_set_max_content_size     ska_dyn_libdecor_frame_set_max_content_size
#define libdecor_state_new                      ska_dyn_libdecor_state_new
#define libdecor_state_free                     ska_dyn_libdecor_state_free
#define libdecor_configuration_get_content_size ska_dyn_libdecor_configuration_get_content_size
#define libdecor_configuration_get_window_state ska_dyn_libdecor_configuration_get_window_state

#endif // SKA_LINUX_DECOR_H
