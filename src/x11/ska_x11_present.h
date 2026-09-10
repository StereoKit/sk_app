//
// sk_app - the slice of libXpresent it uses
//
// Declared here rather than through <X11/extensions/Xpresent.h>: the library
// ships with every X server, the development header often doesn't. Kept in
// step with Xpresent.h, which ska_x11_dyn.c binds these names to at runtime.

#ifndef SKA_X11_PRESENT_H
#define SKA_X11_PRESENT_H

#include <X11/Xlib.h>
#include <stdint.h>

#define SKA_PRESENT_COMPLETE_NOTIFY_MASK (1 << 1)
#define SKA_PRESENT_COMPLETE_NOTIFY      1

typedef struct {
	int           type;
	unsigned long serial;
	Bool          send_event;
	Display*      display;
	int           extension;
	int           evtype;
	uint32_t      eid;
	Window        window;
	uint32_t      serial_number;
	uint64_t      ust;
	uint64_t      msc;
	uint8_t       kind;
	uint8_t       mode;
} XPresentCompleteNotifyEvent;

Bool XPresentQueryExtension(Display* dpy, int* major_opcode_return, int* event_base_return, int* error_base_return);
XID  XPresentSelectInput   (Display* dpy, Window window, unsigned event_mask);
void XPresentNotifyMSC     (Display* dpy, Window window, uint32_t serial, uint64_t target_msc, uint64_t divisor, uint64_t remainder);

#endif // SKA_X11_PRESENT_H
