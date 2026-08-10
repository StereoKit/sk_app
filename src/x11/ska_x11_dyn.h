//
// sk_app - X11 call redirection
//
// sk_app dlopens libX11 rather than linking it, so one binary runs on X11,
// Wayland, and headless systems. This header rewrites each Xlib call in the
// backend to the matching function pointer from ska_x11_dyn.c.
//
// Must be included after the real X11 headers: the pointer types are taken
// from the genuine declarations with __typeof__, so a signature here cannot
// disagree with the one Xlib publishes.

#ifndef SKA_X11_DYN_H
#define SKA_X11_DYN_H

#ifdef SKA_LINUX_X11

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/XKBlib.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/XInput2.h>

#define SKA_X11_SYM(name)     extern __typeof__(name)* ska_dyn_##name;
#define SKA_XRANDR_SYM(name)  extern __typeof__(name)* ska_dyn_##name;
#define SKA_XCURSOR_SYM(name) extern __typeof__(name)* ska_dyn_##name;
#define SKA_XI2_SYM(name)     extern __typeof__(name)* ska_dyn_##name;
#include "ska_x11_syms.h"
#undef SKA_X11_SYM
#undef SKA_XRANDR_SYM
#undef SKA_XCURSOR_SYM
#undef SKA_XI2_SYM

// Loads libX11, libXrandr, and libXcursor. Returns false when any is missing,
// which lets backend selection fall through to another backend. libXi is
// optional and only costs relative mouse mode when it is absent.
bool ska_x11_dyn_load(void);
bool ska_x11_dyn_has_xi2(void);
void ska_x11_dyn_unload(void);

#define XQueryExtension              ska_dyn_XQueryExtension
#define XGetEventData                ska_dyn_XGetEventData
#define XFreeEventData               ska_dyn_XFreeEventData
#define XGrabPointer                 ska_dyn_XGrabPointer
#define XUngrabPointer               ska_dyn_XUngrabPointer
#define XIQueryVersion               ska_dyn_XIQueryVersion
#define XISelectEvents               ska_dyn_XISelectEvents
#define XAllocClassHint              ska_dyn_XAllocClassHint
#define XAllocSizeHints              ska_dyn_XAllocSizeHints
#define XChangeProperty              ska_dyn_XChangeProperty
#define XCheckIfEvent                ska_dyn_XCheckIfEvent
#define XCheckTypedWindowEvent       ska_dyn_XCheckTypedWindowEvent
#define XCloseDisplay                ska_dyn_XCloseDisplay
#define XCloseIM                     ska_dyn_XCloseIM
#define XConvertSelection            ska_dyn_XConvertSelection
#define XCreateBitmapFromData        ska_dyn_XCreateBitmapFromData
#define XCreateColormap              ska_dyn_XCreateColormap
#define XFreeColormap                ska_dyn_XFreeColormap
#define XCreateFontCursor            ska_dyn_XCreateFontCursor
#define XCreateIC                    ska_dyn_XCreateIC
#define XCreatePixmapCursor          ska_dyn_XCreatePixmapCursor
#define XCreateWindow                ska_dyn_XCreateWindow
#define XcursorLibraryLoadCursor     ska_dyn_XcursorLibraryLoadCursor
#define XDefineCursor                ska_dyn_XDefineCursor
#define XDeleteProperty              ska_dyn_XDeleteProperty
#define XDestroyIC                   ska_dyn_XDestroyIC
#define XDestroyWindow               ska_dyn_XDestroyWindow
#define XFilterEvent                 ska_dyn_XFilterEvent
#define XFlush                       ska_dyn_XFlush
#define XFree                        ska_dyn_XFree
#define XFreePixmap                  ska_dyn_XFreePixmap
#define XGetSelectionOwner           ska_dyn_XGetSelectionOwner
#define XGetWindowAttributes         ska_dyn_XGetWindowAttributes
#define XGetWindowProperty           ska_dyn_XGetWindowProperty
#define XIconifyWindow               ska_dyn_XIconifyWindow
#define XInternAtom                  ska_dyn_XInternAtom
#define XkbSetDetectableAutoRepeat   ska_dyn_XkbSetDetectableAutoRepeat
#define XLookupKeysym                ska_dyn_XLookupKeysym
#define XMapWindow                   ska_dyn_XMapWindow
#define XMoveWindow                  ska_dyn_XMoveWindow
#define XNextEvent                   ska_dyn_XNextEvent
#define XOpenDisplay                 ska_dyn_XOpenDisplay
#define XOpenIM                      ska_dyn_XOpenIM
#define XPending                     ska_dyn_XPending
#define XRaiseWindow                 ska_dyn_XRaiseWindow
#define XResizeWindow                ska_dyn_XResizeWindow
#define XResourceManagerString       ska_dyn_XResourceManagerString
#define XrmDestroyDatabase           ska_dyn_XrmDestroyDatabase
#define XrmGetResource               ska_dyn_XrmGetResource
#define XrmGetStringDatabase         ska_dyn_XrmGetStringDatabase
#define XrmInitialize                ska_dyn_XrmInitialize
#define XRRConfigCurrentRate         ska_dyn_XRRConfigCurrentRate
#define XRRFreeScreenConfigInfo      ska_dyn_XRRFreeScreenConfigInfo
#define XRRGetScreenInfo             ska_dyn_XRRGetScreenInfo
#define XSelectInput                 ska_dyn_XSelectInput
#define XSendEvent                   ska_dyn_XSendEvent
#define XSetClassHint                ska_dyn_XSetClassHint
#define XSetICFocus                  ska_dyn_XSetICFocus
#define XSetIconName                 ska_dyn_XSetIconName
#define XSetInputFocus               ska_dyn_XSetInputFocus
#define XSetLocaleModifiers          ska_dyn_XSetLocaleModifiers
#define XSetSelectionOwner           ska_dyn_XSetSelectionOwner
#define XSetWMNormalHints            ska_dyn_XSetWMNormalHints
#define XSetWMProtocols              ska_dyn_XSetWMProtocols
#define XStoreName                   ska_dyn_XStoreName
#define XSync                        ska_dyn_XSync
#define XTranslateCoordinates        ska_dyn_XTranslateCoordinates
#define XUnmapWindow                 ska_dyn_XUnmapWindow
#define XUnsetICFocus                ska_dyn_XUnsetICFocus
#define Xutf8LookupString            ska_dyn_Xutf8LookupString

#endif // SKA_LINUX_X11
#endif // SKA_X11_DYN_H
