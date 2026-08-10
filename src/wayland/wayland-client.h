//
// sk_app - wayland-client.h shim
//
// wayland-scanner hardcodes this include into every generated header, and this
// directory precedes the system path, so the include lands here. That is what
// lets sk_app dlopen libwayland.
//
// The three includes below must stay in this order: real declarations first so
// __typeof__ sees genuine signatures, then the redirect macros, then the
// generated protocols whose inline wrappers must compile against them.
//
// The system wayland-client-protocol.h is deliberately unused; its
// wl_*_interface objects would have to be dlsym'd as data symbols.

#ifndef SKA_WAYLAND_CLIENT_SHIM_H
#define SKA_WAYLAND_CLIENT_SHIM_H

#include <wayland-client-core.h>
#include "ska_wl_dyn.h"
#include "wayland-protocols.h"

#endif // SKA_WAYLAND_CLIENT_SHIM_H
