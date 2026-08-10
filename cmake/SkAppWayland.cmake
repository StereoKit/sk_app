# Wayland support detection.
#
# sk_app dlopens libwayland, libwayland-cursor, and libxkbcommon at runtime, so
# nothing here adds a link dependency. The protocol sources are generated ahead
# of time by tools/gen_wayland_protocols.sh and committed under
# src/wayland, so neither wayland-scanner nor the wayland-protocols
# package is needed to build, and every build compiles identical protocol code
# regardless of what the build machine happens to have installed.
#
# That leaves wayland-client, xkbcommon, and libdecor headers as build
# requirements. libdecor is needed only for its type declarations; the library
# itself stays optional at runtime, and a missing one costs window decorations.
# Both are small, stable, and version-insensitive for the entities sk_app uses
# (wl_proxy, wl_display, wl_interface, and the xkb handles). When either is
# absent, SKA_LINUX_WAYLAND_FOUND is left false and an X11-only sk_app builds.

function(ska_wayland_detect)
	find_package(PkgConfig QUIET)
	if(NOT PkgConfig_FOUND)
		set(SKA_LINUX_WAYLAND_FOUND FALSE PARENT_SCOPE)
		return()
	endif()

	pkg_check_modules(WAYLAND_CLIENT QUIET wayland-client)
	pkg_check_modules(XKBCOMMON      QUIET xkbcommon)
	pkg_check_modules(LIBDECOR       QUIET libdecor-0)
	if(NOT WAYLAND_CLIENT_FOUND OR NOT XKBCOMMON_FOUND OR NOT LIBDECOR_FOUND)
		set(SKA_LINUX_WAYLAND_FOUND FALSE PARENT_SCOPE)
		return()
	endif()

	set(SKA_LINUX_WAYLAND_FOUND TRUE PARENT_SCOPE)
	set(WAYLAND_INCLUDE_DIRS "${WAYLAND_CLIENT_INCLUDE_DIRS};${XKBCOMMON_INCLUDE_DIRS};${LIBDECOR_INCLUDE_DIRS}" PARENT_SCOPE)
endfunction()
