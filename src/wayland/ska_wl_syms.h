//
// sk_app - Wayland symbols resolved at runtime
//
// Expanded several times with different definitions of SKA_WL_SYM,
// SKA_WL_CURSOR_SYM, and SKA_XKB_SYM, so each symbol is written once and the
// pointer, its type, and the dlsym call cannot drift apart.
// Deliberately has no include guard.

// libwayland-client. The wl_proxy_* entries are what wayland-scanner's
// generated inline wrappers call; the rest is what the backend calls directly.
SKA_WL_SYM(wl_proxy_marshal_flags)
SKA_WL_SYM(wl_proxy_add_listener)
SKA_WL_SYM(wl_proxy_get_version)
SKA_WL_SYM(wl_proxy_get_user_data)
SKA_WL_SYM(wl_proxy_set_user_data)
SKA_WL_SYM(wl_proxy_destroy)
SKA_WL_SYM(wl_display_connect)
SKA_WL_SYM(wl_display_disconnect)
SKA_WL_SYM(wl_display_roundtrip)
SKA_WL_SYM(wl_display_dispatch)
SKA_WL_SYM(wl_display_dispatch_pending)
SKA_WL_SYM(wl_display_flush)
SKA_WL_SYM(wl_display_get_fd)
SKA_WL_SYM(wl_display_get_error)
SKA_WL_SYM(wl_display_prepare_read)
SKA_WL_SYM(wl_display_read_events)
SKA_WL_SYM(wl_display_cancel_read)

// libwayland-cursor, for themed cursor images
SKA_WL_CURSOR_SYM(wl_cursor_theme_load)
SKA_WL_CURSOR_SYM(wl_cursor_theme_destroy)
SKA_WL_CURSOR_SYM(wl_cursor_theme_get_cursor)
SKA_WL_CURSOR_SYM(wl_cursor_image_get_buffer)

// libdecor, for client-side decorations when the compositor will not draw a
// frame. Optional at runtime: a missing library only means no window frame,
// so these load through SKA_DECOR_SYM, which is allowed to fail.
SKA_DECOR_SYM(libdecor_new)
SKA_DECOR_SYM(libdecor_unref)
SKA_DECOR_SYM(libdecor_decorate)
SKA_DECOR_SYM(libdecor_frame_unref)
SKA_DECOR_SYM(libdecor_frame_set_title)
SKA_DECOR_SYM(libdecor_frame_set_app_id)
SKA_DECOR_SYM(libdecor_frame_map)
SKA_DECOR_SYM(libdecor_frame_commit)
SKA_DECOR_SYM(libdecor_frame_set_maximized)
SKA_DECOR_SYM(libdecor_frame_unset_maximized)
SKA_DECOR_SYM(libdecor_frame_set_fullscreen)
SKA_DECOR_SYM(libdecor_frame_unset_fullscreen)
SKA_DECOR_SYM(libdecor_frame_set_minimized)
SKA_DECOR_SYM(libdecor_frame_set_min_content_size)
SKA_DECOR_SYM(libdecor_frame_set_max_content_size)
SKA_DECOR_SYM(libdecor_state_new)
SKA_DECOR_SYM(libdecor_state_free)
SKA_DECOR_SYM(libdecor_configuration_get_content_size)
SKA_DECOR_SYM(libdecor_configuration_get_window_state)

// libxkbcommon, for the compositor-supplied keymap
SKA_XKB_SYM(xkb_context_new)
SKA_XKB_SYM(xkb_context_unref)
SKA_XKB_SYM(xkb_keymap_new_from_string)
SKA_XKB_SYM(xkb_keymap_unref)
SKA_XKB_SYM(xkb_state_new)
SKA_XKB_SYM(xkb_state_unref)
SKA_XKB_SYM(xkb_state_update_mask)
SKA_XKB_SYM(xkb_keymap_key_get_syms_by_level)
SKA_XKB_SYM(xkb_state_key_get_syms)
SKA_XKB_SYM(xkb_state_key_get_utf8)
SKA_XKB_SYM(xkb_state_mod_name_is_active)
SKA_XKB_SYM(xkb_keymap_key_repeats)
SKA_XKB_SYM(xkb_compose_table_new_from_locale)
SKA_XKB_SYM(xkb_compose_table_unref)
SKA_XKB_SYM(xkb_compose_state_new)
SKA_XKB_SYM(xkb_compose_state_unref)
SKA_XKB_SYM(xkb_compose_state_feed)
SKA_XKB_SYM(xkb_compose_state_reset)
SKA_XKB_SYM(xkb_compose_state_get_status)
SKA_XKB_SYM(xkb_compose_state_get_utf8)
