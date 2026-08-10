//
// sk_app - Wayland call redirection
//
// Rewrites each libwayland, libwayland-cursor, and libxkbcommon call to a
// function pointer resolved by ska_wl_dyn.c. Included from the
// wayland-client.h shim, which fixes the include order this depends on: the
// real declarations must already be visible so __typeof__ picks up genuine
// signatures, and the macros must be live before any generated inline wrapper
// is parsed.

#ifndef SKA_LINUX_WL_DYN_H
#define SKA_LINUX_WL_DYN_H

#include <stdbool.h>
#include <wayland-client-core.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#define SKA_WL_SYM(name)        extern __typeof__(name)* ska_dyn_##name;
#define SKA_WL_CURSOR_SYM(name) extern __typeof__(name)* ska_dyn_##name;
#define SKA_XKB_SYM(name)       extern __typeof__(name)* ska_dyn_##name;
#define SKA_DECOR_SYM(name)     // libdecor lives in ska_decor.h, see there
#include "ska_wl_syms.h"
#undef SKA_WL_SYM
#undef SKA_WL_CURSOR_SYM
#undef SKA_XKB_SYM
#undef SKA_DECOR_SYM

// Loads libwayland-client, libwayland-cursor, and libxkbcommon. Returns false
// when any is missing, letting backend selection fall through to X11.
bool ska_wl_dyn_load(void);
void ska_wl_dyn_unload(void);

#define wl_proxy_marshal_flags            ska_dyn_wl_proxy_marshal_flags
#define wl_proxy_add_listener             ska_dyn_wl_proxy_add_listener
#define wl_proxy_get_version              ska_dyn_wl_proxy_get_version
#define wl_proxy_get_user_data            ska_dyn_wl_proxy_get_user_data
#define wl_proxy_set_user_data            ska_dyn_wl_proxy_set_user_data
#define wl_proxy_destroy                  ska_dyn_wl_proxy_destroy
#define wl_display_connect                ska_dyn_wl_display_connect
#define wl_display_disconnect             ska_dyn_wl_display_disconnect
#define wl_display_roundtrip              ska_dyn_wl_display_roundtrip
#define wl_display_dispatch               ska_dyn_wl_display_dispatch
#define wl_display_dispatch_pending       ska_dyn_wl_display_dispatch_pending
#define wl_display_flush                  ska_dyn_wl_display_flush
#define wl_display_get_fd                 ska_dyn_wl_display_get_fd
#define wl_display_get_error              ska_dyn_wl_display_get_error
#define wl_display_prepare_read           ska_dyn_wl_display_prepare_read
#define wl_display_read_events            ska_dyn_wl_display_read_events
#define wl_display_cancel_read            ska_dyn_wl_display_cancel_read

#define wl_cursor_theme_load              ska_dyn_wl_cursor_theme_load
#define wl_cursor_theme_destroy           ska_dyn_wl_cursor_theme_destroy
#define wl_cursor_theme_get_cursor        ska_dyn_wl_cursor_theme_get_cursor
#define wl_cursor_image_get_buffer        ska_dyn_wl_cursor_image_get_buffer

#define xkb_context_new                   ska_dyn_xkb_context_new
#define xkb_context_unref                 ska_dyn_xkb_context_unref
#define xkb_keymap_new_from_string        ska_dyn_xkb_keymap_new_from_string
#define xkb_keymap_unref                  ska_dyn_xkb_keymap_unref
#define xkb_state_new                     ska_dyn_xkb_state_new
#define xkb_state_unref                   ska_dyn_xkb_state_unref
#define xkb_state_update_mask             ska_dyn_xkb_state_update_mask
#define xkb_keymap_key_get_syms_by_level  ska_dyn_xkb_keymap_key_get_syms_by_level
#define xkb_state_key_get_syms            ska_dyn_xkb_state_key_get_syms
#define xkb_state_key_get_utf8            ska_dyn_xkb_state_key_get_utf8
#define xkb_state_mod_name_is_active      ska_dyn_xkb_state_mod_name_is_active
#define xkb_keymap_key_repeats            ska_dyn_xkb_keymap_key_repeats
#define xkb_compose_table_new_from_locale ska_dyn_xkb_compose_table_new_from_locale
#define xkb_compose_table_unref           ska_dyn_xkb_compose_table_unref
#define xkb_compose_state_new             ska_dyn_xkb_compose_state_new
#define xkb_compose_state_unref           ska_dyn_xkb_compose_state_unref
#define xkb_compose_state_feed            ska_dyn_xkb_compose_state_feed
#define xkb_compose_state_reset           ska_dyn_xkb_compose_state_reset
#define xkb_compose_state_get_status      ska_dyn_xkb_compose_state_get_status
#define xkb_compose_state_get_utf8        ska_dyn_xkb_compose_state_get_utf8

#endif // SKA_LINUX_WL_DYN_H
