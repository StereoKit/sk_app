/*
 * sk_app - macOS Cocoa platform backend
 */

#include "ska_internal.h"

#ifdef SKA_PLATFORM_MACOS

#import <Cocoa/Cocoa.h>
#import <Carbon/Carbon.h>  /* For key codes */
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

/* Forward declaration for file dialog check */
static void ska_macos_check_file_dialog(void);

/* Scancode translation table (Carbon key codes to ska_scancode_) */
static ska_scancode_ ska_macos_scancode_table[256];

static void ska_init_scancode_table(void) {
	/* Initialize all to unknown */
	for (int i = 0; i < 256; i++) {
		ska_macos_scancode_table[i] = ska_scancode_unknown;
	}

	/* Letters */
	ska_macos_scancode_table[kVK_ANSI_A] = ska_scancode_a;
	ska_macos_scancode_table[kVK_ANSI_B] = ska_scancode_b;
	ska_macos_scancode_table[kVK_ANSI_C] = ska_scancode_c;
	ska_macos_scancode_table[kVK_ANSI_D] = ska_scancode_d;
	ska_macos_scancode_table[kVK_ANSI_E] = ska_scancode_e;
	ska_macos_scancode_table[kVK_ANSI_F] = ska_scancode_f;
	ska_macos_scancode_table[kVK_ANSI_G] = ska_scancode_g;
	ska_macos_scancode_table[kVK_ANSI_H] = ska_scancode_h;
	ska_macos_scancode_table[kVK_ANSI_I] = ska_scancode_i;
	ska_macos_scancode_table[kVK_ANSI_J] = ska_scancode_j;
	ska_macos_scancode_table[kVK_ANSI_K] = ska_scancode_k;
	ska_macos_scancode_table[kVK_ANSI_L] = ska_scancode_l;
	ska_macos_scancode_table[kVK_ANSI_M] = ska_scancode_m;
	ska_macos_scancode_table[kVK_ANSI_N] = ska_scancode_n;
	ska_macos_scancode_table[kVK_ANSI_O] = ska_scancode_o;
	ska_macos_scancode_table[kVK_ANSI_P] = ska_scancode_p;
	ska_macos_scancode_table[kVK_ANSI_Q] = ska_scancode_q;
	ska_macos_scancode_table[kVK_ANSI_R] = ska_scancode_r;
	ska_macos_scancode_table[kVK_ANSI_S] = ska_scancode_s;
	ska_macos_scancode_table[kVK_ANSI_T] = ska_scancode_t;
	ska_macos_scancode_table[kVK_ANSI_U] = ska_scancode_u;
	ska_macos_scancode_table[kVK_ANSI_V] = ska_scancode_v;
	ska_macos_scancode_table[kVK_ANSI_W] = ska_scancode_w;
	ska_macos_scancode_table[kVK_ANSI_X] = ska_scancode_x;
	ska_macos_scancode_table[kVK_ANSI_Y] = ska_scancode_y;
	ska_macos_scancode_table[kVK_ANSI_Z] = ska_scancode_z;

	/* Numbers */
	ska_macos_scancode_table[kVK_ANSI_0] = ska_scancode_0;
	ska_macos_scancode_table[kVK_ANSI_1] = ska_scancode_1;
	ska_macos_scancode_table[kVK_ANSI_2] = ska_scancode_2;
	ska_macos_scancode_table[kVK_ANSI_3] = ska_scancode_3;
	ska_macos_scancode_table[kVK_ANSI_4] = ska_scancode_4;
	ska_macos_scancode_table[kVK_ANSI_5] = ska_scancode_5;
	ska_macos_scancode_table[kVK_ANSI_6] = ska_scancode_6;
	ska_macos_scancode_table[kVK_ANSI_7] = ska_scancode_7;
	ska_macos_scancode_table[kVK_ANSI_8] = ska_scancode_8;
	ska_macos_scancode_table[kVK_ANSI_9] = ska_scancode_9;

	/* Function keys */
	ska_macos_scancode_table[kVK_Return] = ska_scancode_return;
	ska_macos_scancode_table[kVK_Escape] = ska_scancode_escape;
	ska_macos_scancode_table[kVK_Delete] = ska_scancode_backspace;
	ska_macos_scancode_table[kVK_Tab   ] = ska_scancode_tab;
	ska_macos_scancode_table[kVK_Space ] = ska_scancode_space;

	/* Symbols */
	ska_macos_scancode_table[kVK_ANSI_Minus       ] = ska_scancode_minus;
	ska_macos_scancode_table[kVK_ANSI_Equal       ] = ska_scancode_equals;
	ska_macos_scancode_table[kVK_ANSI_LeftBracket ] = ska_scancode_leftbracket;
	ska_macos_scancode_table[kVK_ANSI_RightBracket] = ska_scancode_rightbracket;
	ska_macos_scancode_table[kVK_ANSI_Backslash   ] = ska_scancode_backslash;
	ska_macos_scancode_table[kVK_ANSI_Semicolon   ] = ska_scancode_semicolon;
	ska_macos_scancode_table[kVK_ANSI_Quote       ] = ska_scancode_apostrophe;
	ska_macos_scancode_table[kVK_ANSI_Grave       ] = ska_scancode_grave;
	ska_macos_scancode_table[kVK_ANSI_Comma       ] = ska_scancode_comma;
	ska_macos_scancode_table[kVK_ANSI_Period      ] = ska_scancode_period;
	ska_macos_scancode_table[kVK_ANSI_Slash       ] = ska_scancode_slash;

	ska_macos_scancode_table[kVK_CapsLock] = ska_scancode_capslock;

	/* F keys */
	ska_macos_scancode_table[kVK_F1] = ska_scancode_f1;
	ska_macos_scancode_table[kVK_F2] = ska_scancode_f2;
	ska_macos_scancode_table[kVK_F3] = ska_scancode_f3;
	ska_macos_scancode_table[kVK_F4] = ska_scancode_f4;
	ska_macos_scancode_table[kVK_F5] = ska_scancode_f5;
	ska_macos_scancode_table[kVK_F6] = ska_scancode_f6;
	ska_macos_scancode_table[kVK_F7] = ska_scancode_f7;
	ska_macos_scancode_table[kVK_F8] = ska_scancode_f8;
	ska_macos_scancode_table[kVK_F9] = ska_scancode_f9;
	ska_macos_scancode_table[kVK_F10] = ska_scancode_f10;
	ska_macos_scancode_table[kVK_F11] = ska_scancode_f11;
	ska_macos_scancode_table[kVK_F12] = ska_scancode_f12;

	/* Navigation */
	ska_macos_scancode_table[kVK_Home         ] = ska_scancode_home;
	ska_macos_scancode_table[kVK_PageUp       ] = ska_scancode_pageup;
	ska_macos_scancode_table[kVK_ForwardDelete] = ska_scancode_delete;
	ska_macos_scancode_table[kVK_End          ] = ska_scancode_end;
	ska_macos_scancode_table[kVK_PageDown     ] = ska_scancode_pagedown;
	ska_macos_scancode_table[kVK_RightArrow   ] = ska_scancode_right;
	ska_macos_scancode_table[kVK_LeftArrow    ] = ska_scancode_left;
	ska_macos_scancode_table[kVK_DownArrow    ] = ska_scancode_down;
	ska_macos_scancode_table[kVK_UpArrow      ] = ska_scancode_up;

	/* Modifiers */
	ska_macos_scancode_table[kVK_Control] = ska_scancode_lctrl;
	ska_macos_scancode_table[kVK_Shift] = ska_scancode_lshift;
	ska_macos_scancode_table[kVK_Option] = ska_scancode_lalt;
	ska_macos_scancode_table[kVK_Command] = ska_scancode_lgui;
	ska_macos_scancode_table[kVK_RightControl] = ska_scancode_rctrl;
	ska_macos_scancode_table[kVK_RightShift] = ska_scancode_rshift;
	ska_macos_scancode_table[kVK_RightOption] = ska_scancode_ralt;
	/* Note: macOS doesn't distinguish right Command in Carbon */
}

static ska_window_t* ska_find_window_by_nswindow(NSWindow* nswindow) {
	for (uint32_t i = 0; i < SKA_MAX_WINDOWS; i++) {
		if (g_ska.windows[i] && g_ska.windows[i]->ns_window == nswindow) {
			return g_ska.windows[i];
		}
	}
	return NULL;
}

static uint16_t ska_macos_get_modifiers(NSEventModifierFlags flags) {
	uint16_t mods = 0;
	if (flags & NSEventModifierFlagShift) mods |= ska_keymod_shift;
	if (flags & NSEventModifierFlagControl) mods |= ska_keymod_ctrl;
	if (flags & NSEventModifierFlagOption) mods |= ska_keymod_alt;
	if (flags & NSEventModifierFlagCommand) mods |= ska_keymod_gui;
	return mods;
}

/* Window delegate for event handling */
@interface SKAWindowDelegate : NSObject <NSWindowDelegate>
@property (assign) ska_window_t* window;
@end

@implementation SKAWindowDelegate

- (void)windowDidResize:(NSNotification*)notification {
	if (!self.window) return;

	NSWindow* nswindow = (NSWindow*)self.window->ns_window;
	NSRect rect = [nswindow contentRectForFrameRect:[nswindow frame]];

	int32_t width = (int32_t)rect.size.width;
	int32_t height = (int32_t)rect.size.height;

	if (width != self.window->width || height != self.window->height) {
		ska_event_t event = {0};
		event.type = ska_event_window_resized;
		event.timestamp = (uint32_t)ska_time_get_elapsed_ms();
		event.window.window_id = self.window->id;
		event.window.data1 = width;
		event.window.data2 = height;
		self.window->width = width;
		self.window->height = height;

		/* Update drawable size for Retina displays */
		NSView* view = (NSView*)self.window->ns_view;
		NSRect backing_rect = [view convertRectToBacking:rect];
		self.window->drawable_width = (int32_t)backing_rect.size.width;
		self.window->drawable_height = (int32_t)backing_rect.size.height;

		ska_post_event(&event);
	}
}

- (void)windowDidMove:(NSNotification*)notification {
	if (!self.window) return;

	NSWindow* nswindow = (NSWindow*)self.window->ns_window;
	NSRect frame = [nswindow frame];

	/* macOS origin is bottom-left, convert to top-left */
	NSScreen* screen = [NSScreen mainScreen];
	int32_t x = (int32_t)frame.origin.x;
	int32_t y = (int32_t)(screen.frame.size.height - frame.origin.y - frame.size.height);

	if (x != self.window->x || y != self.window->y) {
		ska_event_t event = {0};
		event.type = ska_event_window_moved;
		event.timestamp = (uint32_t)ska_time_get_elapsed_ms();
		event.window.window_id = self.window->id;
		event.window.data1 = x;
		event.window.data2 = y;
		self.window->x = x;
		self.window->y = y;
		ska_post_event(&event);
	}
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
	if (!self.window) return;

	ska_event_t event = {0};
	event.type = ska_event_window_focus_gained;
	event.timestamp = (uint32_t)ska_time_get_elapsed_ms();
	event.window.window_id = self.window->id;
	self.window->has_focus = true;
	ska_post_event(&event);
}

- (void)windowDidResignKey:(NSNotification*)notification {
	if (!self.window) return;

	ska_event_t event = {0};
	event.type = ska_event_window_focus_lost;
	event.timestamp = (uint32_t)ska_time_get_elapsed_ms();
	event.window.window_id = self.window->id;
	self.window->has_focus = false;
	ska_post_event(&event);
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
	if (!self.window) return YES;

	ska_event_t event = {0};
	event.type = ska_event_window_close;
	event.timestamp = (uint32_t)ska_time_get_elapsed_ms();
	event.window.window_id = self.window->id;
	self.window->should_close = true;
	ska_post_event(&event);

	return NO;  /* We'll close it manually */
}

- (void)windowDidMiniaturize:(NSNotification*)notification {
	if (!self.window) return;

	ska_event_t event = {0};
	event.type = ska_event_window_minimized;
	event.timestamp = (uint32_t)ska_time_get_elapsed_ms();
	event.window.window_id = self.window->id;
	ska_post_event(&event);
}

- (void)windowDidDeminiaturize:(NSNotification*)notification {
	if (!self.window) return;

	ska_event_t event = {0};
	event.type = ska_event_window_restored;
	event.timestamp = (uint32_t)ska_time_get_elapsed_ms();
	event.window.window_id = self.window->id;
	ska_post_event(&event);
}

@end

/* Application delegate for app lifecycle */
@interface SKAAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation SKAAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
	g_ska.app_activated = true;
	[NSApp stop:nil];  /* Exit the initial run loop */
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
	return NO;  /* Don't auto-quit */
}

@end

bool ska_platform_init(void) {
	@autoreleasepool {
		/* Get or create NSApplication */
		g_ska.ns_app = [NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

		/* Create app delegate */
		g_ska.ns_delegate = [[SKAAppDelegate alloc] init];
		[NSApp setDelegate:(SKAAppDelegate*)g_ska.ns_delegate];

		/* Activate the app */
		[NSApp finishLaunching];
		[NSApp activateIgnoringOtherApps:YES];

		/* Initialize scancode table */
		ska_init_scancode_table();

		return true;
	}
}

void ska_platform_shutdown(void) {
	@autoreleasepool {
		if (g_ska.ns_delegate) {
			[NSApp setDelegate:nil];
			g_ska.ns_delegate = nil;
		}
	}
}

bool ska_platform_window_create(
	ska_window_t* window,
	const char* title,
	int32_t x, int32_t y,
	int32_t w, int32_t h,
	uint32_t flags
) {
	@autoreleasepool {
		/* Determine window style */
		NSWindowStyleMask style = NSWindowStyleMaskTitled;

		if (flags & ska_window_borderless) {
			style = NSWindowStyleMaskBorderless;
		} else {
			style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
			if (flags & ska_window_resizable) {
				style |= NSWindowStyleMaskResizable;
			}
		}

		/* Convert coordinates (macOS uses bottom-left origin) */
		NSScreen* screen = [NSScreen mainScreen];
		NSRect content_rect;

		if (x == -1 || y == -1) {
			/* Center window */
			content_rect = NSMakeRect(0, 0, w, h);
		} else {
			/* Convert from top-left to bottom-left origin */
			CGFloat cocoa_y = screen.frame.size.height - y - h;
			content_rect = NSMakeRect(x, cocoa_y, w, h);
		}

		/* Create window */
		NSWindow* nswindow = [[NSWindow alloc]
			initWithContentRect:content_rect
			styleMask:style
			backing:NSBackingStoreBuffered
			defer:NO];

		if (!nswindow) {
			ska_set_error("Failed to create NSWindow");
			return false;
		}

		window->ns_window = nswindow;

		/* Set title */
		NSString* ns_title = [NSString stringWithUTF8String:title];
		[nswindow setTitle:ns_title];
		window->title = ska_strdup(title);

		/* Create and set delegate */
		SKAWindowDelegate* delegate = [[SKAWindowDelegate alloc] init];
		delegate.window = window;
		[nswindow setDelegate:delegate];
		window->ns_view = [nswindow contentView];

		/* Set window properties */
		[nswindow setAcceptsMouseMovedEvents:YES];
		[nswindow setReleasedWhenClosed:NO];

		if (x == -1 || y == -1) {
			[nswindow center];
		}

		/* Get actual position and size */
		NSRect frame = [nswindow frame];
		NSRect client = [nswindow contentRectForFrameRect:frame];

		window->width = (int32_t)client.size.width;
		window->height = (int32_t)client.size.height;
		window->x = (int32_t)frame.origin.x;
		window->y = (int32_t)(screen.frame.size.height - frame.origin.y - frame.size.height);

		/* Get drawable size (for Retina displays) */
		NSView* view = (NSView*)window->ns_view;
		NSRect backing_rect = [view convertRectToBacking:client];
		window->drawable_width = (int32_t)backing_rect.size.width;
		window->drawable_height = (int32_t)backing_rect.size.height;
		window->dpi_scale = ska_platform_get_dpi_scale(window);

		/* Apply initial state */
		if (flags & ska_window_maximized) {
			[nswindow zoom:nil];
		}

		if (!(flags & ska_window_hidden)) {
			[nswindow makeKeyAndOrderFront:nil];
			window->is_visible = true;
		}

		return true;
	}
}

void ska_platform_window_destroy(ska_window_t* window) {
	@autoreleasepool {
		if (window->ns_window) {
			NSWindow* nswindow = (NSWindow*)window->ns_window;
			[nswindow setDelegate:nil];
			[nswindow close];
			window->ns_window = nil;
			window->ns_view = nil;
		}
	}
}

void ska_platform_window_set_title(ska_window_t* window, const char* title) {
	@autoreleasepool {
		NSWindow* nswindow = (NSWindow*)window->ns_window;
		NSString* ns_title = [NSString stringWithUTF8String:title];
		[nswindow setTitle:ns_title];

		if (window->title) {
			ska_free(window->title);
		}
		window->title = ska_strdup(title);
	}
}

void ska_platform_get_frame_extents(const ska_window_t* window, int32_t* out_left, int32_t* out_right, int32_t* out_top, int32_t* out_bottom) {
	@autoreleasepool {
		int32_t left = 0, right = 0, top = 0, bottom = 0;

		if (window && window->ns_window) {
			NSWindow* nswindow = (NSWindow*)window->ns_window;
			NSRect content_rect = NSMakeRect(0, 0, 100, 100);
			NSRect frame_rect = [nswindow frameRectForContentRect:content_rect];

			left   = (int32_t)(-frame_rect.origin.x);
			right  = (int32_t)(frame_rect.size.width - 100 - left);
			bottom = (int32_t)(-frame_rect.origin.y);
			top    = (int32_t)(frame_rect.size.height - 100 - bottom);
		}

		if (out_left)   *out_left   = left;
		if (out_right)  *out_right  = right;
		if (out_top)    *out_top    = top;
		if (out_bottom) *out_bottom = bottom;
	}
}

void ska_platform_window_set_frame_position(ska_window_t* window, int32_t x, int32_t y) {
	@autoreleasepool {
		NSWindow* nswindow = (NSWindow*)window->ns_window;
		NSScreen* screen = [NSScreen mainScreen];
		NSRect frame = [nswindow frame];

		/* Convert from top-left to bottom-left origin (y is frame top) */
		CGFloat cocoa_y = screen.frame.size.height - y - frame.size.height;
		[nswindow setFrameOrigin:NSMakePoint(x, cocoa_y)];

		/* Update cached content position */
		int32_t left, top;
		ska_platform_get_frame_extents(window, &left, NULL, &top, NULL);
		window->x = x + left;
		window->y = y + top;
	}
}

void ska_platform_window_set_frame_size(ska_window_t* window, int32_t w, int32_t h) {
	@autoreleasepool {
		NSWindow* nswindow = (NSWindow*)window->ns_window;
		NSRect frame = [nswindow frame];
		NSRect content_rect = NSMakeRect(frame.origin.x, frame.origin.y, w, h);
		NSRect new_frame = [nswindow frameRectForContentRect:content_rect];
		[nswindow setFrame:new_frame display:YES];
	}
}

void ska_platform_window_show(ska_window_t* window) {
	@autoreleasepool {
		NSWindow* nswindow = (NSWindow*)window->ns_window;
		[nswindow makeKeyAndOrderFront:nil];
		window->is_visible = true;
	}
}

void ska_platform_window_hide(ska_window_t* window) {
	@autoreleasepool {
		NSWindow* nswindow = (NSWindow*)window->ns_window;
		[nswindow orderOut:nil];
		window->is_visible = false;
	}
}

void ska_platform_window_maximize(ska_window_t* window) {
	@autoreleasepool {
		NSWindow* nswindow = (NSWindow*)window->ns_window;
		if (![nswindow isZoomed]) {
			[nswindow zoom:nil];
		}
	}
}

void ska_platform_window_minimize(ska_window_t* window) {
	@autoreleasepool {
		NSWindow* nswindow = (NSWindow*)window->ns_window;
		[nswindow miniaturize:nil];
	}
}

void ska_platform_window_restore(ska_window_t* window) {
	@autoreleasepool {
		NSWindow* nswindow = (NSWindow*)window->ns_window;
		if ([nswindow isMiniaturized]) {
			[nswindow deminiaturize:nil];
		} else if ([nswindow isZoomed]) {
			[nswindow zoom:nil];
		}
	}
}

void ska_platform_window_raise(ska_window_t* window) {
	@autoreleasepool {
		NSWindow* nswindow = (NSWindow*)window->ns_window;
		[nswindow makeKeyAndOrderFront:nil];
	}
}

void ska_platform_window_get_drawable_size(ska_window_t* window, int32_t* opt_out_width, int32_t* opt_out_height) {
	/* Already computed during resize — Retina displays have
	   drawable size != window size */
	if (opt_out_width)  *opt_out_width  = window->drawable_width;
	if (opt_out_height) *opt_out_height = window->drawable_height;
}

float ska_platform_get_dpi_scale(const ska_window_t* window) {
	@autoreleasepool {
		/* macOS handles scaling transparently via backingScaleFactor.
		 * The UI scale is built into the system and applications don't
		 * need to manually scale fonts - Core Text does this automatically.
		 * We return 1.0 here because macOS apps should use backingScaleFactor
		 * only for rendering pixel-perfect content, not for UI scaling. */
		(void)window;
		return 1.0f;
	}
}

float ska_platform_get_refresh_rate(const ska_window_t* window) {
	@autoreleasepool {
		NSScreen* screen = nil;

		if (window && window->ns_window) {
			NSWindow* nswindow = (NSWindow*)window->ns_window;
			screen = [nswindow screen];
		}

		if (!screen) {
			screen = [NSScreen mainScreen];
		}

		if (!screen) {
			return 0.0f;
		}

		/* Get the CGDirectDisplayID from the screen */
		NSDictionary* description = [screen deviceDescription];
		NSNumber* screenNumber = [description objectForKey:@"NSScreenNumber"];
		if (!screenNumber) {
			return 0.0f;
		}

		CGDirectDisplayID displayID = [screenNumber unsignedIntValue];
		CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
		if (!mode) {
			return 0.0f;
		}

		double rate = CGDisplayModeGetRefreshRate(mode);
		CGDisplayModeRelease(mode);

		/* Some displays (especially built-in) may report 0 for refresh rate.
		 * In that case, fall back to a reasonable default. */
		if (rate <= 0.0) {
			return 60.0f;
		}

		return (float)rate;
	}
}

void ska_platform_warp_mouse(ska_window_t* window, int32_t x, int32_t y) {
	@autoreleasepool {
		NSWindow* nswindow = (NSWindow*)window->ns_window;
		NSView* view = (NSView*)window->ns_view;

		NSPoint point = NSMakePoint(x, window->height - y);  /* Flip Y */
		point = [view convertPoint:point toView:nil];
		point = [nswindow convertPointToScreen:point];

		/* Convert to global screen coordinates */
		CGPoint cgpoint = CGPointMake(point.x, [[NSScreen mainScreen] frame].size.height - point.y);
		CGWarpMouseCursorPosition(cgpoint);
	}
}

void ska_platform_set_cursor(ska_system_cursor_ cursor) {
	NSCursor* ns_cursor = nil;

	switch (cursor) {
		case ska_system_cursor_arrow:      ns_cursor = [NSCursor arrowCursor]; break;
		case ska_system_cursor_ibeam:      ns_cursor = [NSCursor IBeamCursor]; break;
		case ska_system_cursor_crosshair:  ns_cursor = [NSCursor crosshairCursor]; break;
		case ska_system_cursor_hand:       ns_cursor = [NSCursor pointingHandCursor]; break;
		case ska_system_cursor_sizewe:     ns_cursor = [NSCursor resizeLeftRightCursor]; break;
		case ska_system_cursor_sizens:     ns_cursor = [NSCursor resizeUpDownCursor]; break;
		case ska_system_cursor_no:         ns_cursor = [NSCursor operationNotAllowedCursor]; break;
		case ska_system_cursor_wait:
		case ska_system_cursor_waitarrow:
		case ska_system_cursor_sizenwse:
		case ska_system_cursor_sizenesw:
		case ska_system_cursor_sizeall:
			ns_cursor = [NSCursor arrowCursor];
			break;
		default:
			ns_cursor = [NSCursor arrowCursor];
			break;
	}

	[ns_cursor set];
}

void ska_platform_show_cursor(bool show) {
	if (show) {
		[NSCursor unhide];
		CGAssociateMouseAndMouseCursorPosition(true);
	} else {
		[NSCursor hide];
	}
}

bool ska_platform_set_relative_mouse_mode(bool enabled) {
	CGAssociateMouseAndMouseCursorPosition(enabled ? false : true);
	if (enabled) {
		[NSCursor hide];
	} else {
		[NSCursor unhide];
	}
	return true;
}

void ska_platform_pump_events(void) {
	@autoreleasepool {
		while (true) {
			NSEvent* nsevent = [NSApp nextEventMatchingMask:NSEventMaskAny
									untilDate:[NSDate distantPast]
									inMode:NSDefaultRunLoopMode
									dequeue:YES];

			if (!nsevent) {
				break;
			}

			ska_window_t* window = NULL;
			if (nsevent.window) {
				window = ska_find_window_by_nswindow(nsevent.window);
			}

			ska_event_t event = {0};
			event.timestamp = (uint32_t)ska_time_get_elapsed_ms();

			switch (nsevent.type) {
				case NSEventTypeKeyDown:
				case NSEventTypeKeyUp: {
					if (window) {
						bool pressed = (nsevent.type == NSEventTypeKeyDown);
						unsigned short keycode = nsevent.keyCode;

						event.type = pressed ? ska_event_key_down : ska_event_key_up;
						event.keyboard.window_id = window->id;
						event.keyboard.pressed = pressed;
						event.keyboard.repeat = nsevent.isARepeat;
						event.keyboard.scancode = ska_macos_scancode_table[keycode];
						event.keyboard.modifiers = ska_macos_get_modifiers(nsevent.modifierFlags);

						/* Update input state */
						if (event.keyboard.scancode != ska_scancode_unknown) {
							g_ska.input_state.keyboard[event.keyboard.scancode] = pressed ? 1 : 0;
						}
						g_ska.input_state.key_modifiers = event.keyboard.modifiers;

						ska_post_event(&event);

						/* Handle text input */
						if (pressed && nsevent.characters.length > 0) {
							const char* utf8 = [nsevent.characters UTF8String];
							if (utf8 && strlen(utf8) > 0) {
								event.type = ska_event_text_input;
								event.text.window_id = window->id;
								strncpy(event.text.text, utf8, sizeof(event.text.text) - 1);
								ska_post_event(&event);
							}
						}
					}
					break;
				}

				case NSEventTypeMouseMoved:
				case NSEventTypeLeftMouseDragged:
				case NSEventTypeRightMouseDragged:
				case NSEventTypeOtherMouseDragged: {
					if (window) {
						NSPoint location = nsevent.locationInWindow;
						int32_t x = (int32_t)location.x;
						int32_t y = (int32_t)(window->height - location.y);  /* Flip Y */

						event.type = ska_event_mouse_motion;
						event.mouse_motion.window_id = window->id;
						event.mouse_motion.x = x;
						event.mouse_motion.y = y;
						event.mouse_motion.xrel = x - g_ska.input_state.mouse_x;
						event.mouse_motion.yrel = y - g_ska.input_state.mouse_y;

						g_ska.input_state.mouse_x = x;
						g_ska.input_state.mouse_y = y;
						g_ska.input_state.mouse_xrel = event.mouse_motion.xrel;
						g_ska.input_state.mouse_yrel = event.mouse_motion.yrel;

						ska_post_event(&event);
					}
					break;
				}

				case NSEventTypeLeftMouseDown:
				case NSEventTypeRightMouseDown:
				case NSEventTypeOtherMouseDown:
				case NSEventTypeLeftMouseUp:
				case NSEventTypeRightMouseUp:
				case NSEventTypeOtherMouseUp: {
					if (window) {
						bool pressed = (nsevent.type == NSEventTypeLeftMouseDown ||
									   nsevent.type == NSEventTypeRightMouseDown ||
									   nsevent.type == NSEventTypeOtherMouseDown);

						ska_mouse_button_ button;
						if (nsevent.type == NSEventTypeLeftMouseDown || nsevent.type == NSEventTypeLeftMouseUp) {
							button = ska_mouse_button_left;
						} else if (nsevent.type == NSEventTypeRightMouseDown || nsevent.type == NSEventTypeRightMouseUp) {
							button = ska_mouse_button_right;
						} else {
							/* OtherMouse: buttonNumber 2=middle, 3=X1, 4=X2 */
							switch (nsevent.buttonNumber) {
								case 2: button = ska_mouse_button_middle; break;
								case 3: button = ska_mouse_button_x1;     break;
								case 4: button = ska_mouse_button_x2;     break;
								default: break; /* Unknown button, abandon event */
							}
							if (nsevent.buttonNumber < 2 || nsevent.buttonNumber > 4) {
								break;
							}
						}

						NSPoint location = nsevent.locationInWindow;
						int32_t x = (int32_t)location.x;
						int32_t y = (int32_t)(window->height - location.y);

						event.type = pressed ? ska_event_mouse_button_down : ska_event_mouse_button_up;
						event.mouse_button.window_id = window->id;
						event.mouse_button.button = button;
						event.mouse_button.pressed = pressed;
						event.mouse_button.clicks = (uint8_t)nsevent.clickCount;
						event.mouse_button.x = x;
						event.mouse_button.y = y;

						/* Update button state */
						uint32_t button_mask = (1 << (button - 1));
						if (pressed) {
							g_ska.input_state.mouse_buttons |= button_mask;
						} else {
							g_ska.input_state.mouse_buttons &= ~button_mask;
						}

						ska_post_event(&event);
					}
					break;
				}

				case NSEventTypeScrollWheel: {
					if (window) {
						CGFloat deltaX = nsevent.scrollingDeltaX;
						CGFloat deltaY = nsevent.scrollingDeltaY;

						event.type = ska_event_mouse_wheel;
						event.mouse_wheel.window_id = window->id;
						event.mouse_wheel.x = (int32_t)deltaX;
						event.mouse_wheel.y = (int32_t)deltaY;
						event.mouse_wheel.precise_x = (float)deltaX;
						event.mouse_wheel.precise_y = (float)deltaY;

						ska_post_event(&event);
					}
					break;
				}

				default:
					break;
			}

			[NSApp sendEvent:nsevent];
		}

		/* Check for file dialog completion */
		ska_macos_check_file_dialog();
	}
}

/////////////////////////////////////////
// macOS specific subset of Vulkan header
/////////////////////////////////////////

#import <QuartzCore/CAMetalLayer.h>

typedef VkFlags VkMetalSurfaceCreateFlagsEXT;
typedef struct VkMetalSurfaceCreateInfoEXT {
	VkStructureType                 sType;
	const void*                     pNext;
	VkMetalSurfaceCreateFlagsEXT    flags;
	const CAMetalLayer*             pLayer;
} VkMetalSurfaceCreateInfoEXT;

typedef VkResult (VKAPI_PTR *PFN_vkCreateMetalSurfaceEXT)(VkInstance instance, const VkMetalSurfaceCreateInfoEXT* pCreateInfo, const /*VkAllocationCallbacks*/ void* pAllocator, VkSurfaceKHR* pSurface);

/////////////////////////////////////////

const char** ska_platform_vk_get_instance_extensions(uint32_t* out_count) {
	static const char* extensions[] = {
		"VK_KHR_surface",
		"VK_EXT_metal_surface"
	};
	*out_count = 2;
	return extensions;
}

bool ska_platform_vk_create_surface(const ska_window_t* window, VkInstance instance, VkSurfaceKHR* out_surface) {
	@autoreleasepool {
		void* module = dlopen("@executable_path/../Frameworks/libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
		if (!module)
			module = dlopen("libvulkan.dylib", RTLD_NOW | RTLD_LOCAL);
		if (!module)
			module = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
		if (!module && getenv("DYLD_FALLBACK_LIBRARY_PATH") == NULL)
			module = dlopen("/usr/local/lib/libvulkan.dylib", RTLD_NOW | RTLD_LOCAL);
		if (!module)
			module = dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
		if (!module)
			module = dlopen("vulkan.framework/vulkan", RTLD_NOW | RTLD_LOCAL);
		if (!module)
			module = dlopen("MoltenVK.framework/MoltenVK", RTLD_NOW | RTLD_LOCAL);
		if (!module) {
			ska_set_error("Failed to load Vulkan dylib");
			return false;
		}

		PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(module, "vkGetInstanceProcAddr");
		if (!vkGetInstanceProcAddr) {
			ska_set_error("Failed to load vkGetInstanceProcAddr");
			return false;
		}

		PFN_vkCreateMetalSurfaceEXT vkCreateMetalSurfaceEXT = (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT");
		if (!vkCreateMetalSurfaceEXT) {
			ska_set_error("Failed to load vkCreateMetalSurfaceEXT");
			return false;
		}

		NSView* nsview = (__bridge NSView*)window->ns_view;
		if (!nsview) {
			ska_set_error("Window view not available");
			return false;
		}

		/* Create a CAMetalLayer and set it as the view's layer */
		CAMetalLayer* metal_layer = [CAMetalLayer layer];
		[nsview setWantsLayer:YES];
		[nsview setLayer:metal_layer];

		VkMetalSurfaceCreateInfoEXT create_info = {0};
		create_info.sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
		create_info.pLayer = metal_layer;

		VkResult result = vkCreateMetalSurfaceEXT(instance, &create_info, NULL, out_surface);
		if (result != VK_SUCCESS) {
			ska_set_error("Failed to create Vulkan Metal surface: %d", result);
			return false;
		}

		return true;
	}
}

/* ========== Text Input Platform Functions ========== */

void ska_platform_show_virtual_keyboard(bool visible, ska_text_input_type_ type) {
	/* macOS - desktop platform, no virtual keyboard */
	(void)visible;
	(void)type;
}

/* ========== Clipboard Platform Functions ========== */

char* ska_platform_clipboard_get_text(void) {
	@autoreleasepool {
		NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
		NSString* text = [pasteboard stringForType:NSPasteboardTypeString];

		if (!text) {
			return NULL;
		}

		const char* utf8_text = [text UTF8String];
		if (!utf8_text) {
			return NULL;
		}

		size_t len = strlen(utf8_text);
		char* result = (char*)ska_malloc(len + 1);
		if (result) {
			memcpy(result, utf8_text, len + 1);
		}
		return result;
	}
}

bool ska_platform_clipboard_set_text(const char* text) {
	if (!text) {
		ska_set_error("ska_platform_clipboard_set_text: text cannot be NULL");
		return false;
	}

	@autoreleasepool {
		NSPasteboard* pasteboard = [NSPasteboard generalPasteboard];
		NSString* nstext = [NSString stringWithUTF8String:text];

		if (!nstext) {
			ska_set_error("ska_platform_clipboard_set_text: UTF-8 conversion failed");
			return false;
		}

		[pasteboard clearContents];
		BOOL success = [pasteboard setString:nstext forType:NSPasteboardTypeString];

		if (!success) {
			ska_set_error("ska_platform_clipboard_set_text: setString failed");
			return false;
		}

		return true;
	}
}

/* ========== File Dialog ========== */

/* Pending file dialog state */
typedef struct {
	ska_file_dialog_id_t     id;
	char*                    title;
	bool                     active;
	bool                     completed;
	bool                     cancelled;
	char**                   paths;
	int32_t                  path_count;
} ska_macos_file_dialog_t;

static ska_macos_file_dialog_t g_macos_file_dialog = {0};

bool ska_platform_file_dialog_available(ska_file_dialog_ type) {
	(void)type;
	return true; /* Always available on macOS */
}

bool ska_platform_file_dialog_show(ska_file_dialog_id_t id, const ska_file_dialog_request_t* request) {
	if (g_macos_file_dialog.active) {
		ska_set_error("File dialog already active");
		return false;
	}

	@autoreleasepool {
		g_macos_file_dialog.id = id;
		g_macos_file_dialog.title = request->title ? ska_strdup(request->title) : NULL;
		g_macos_file_dialog.active = true;
		g_macos_file_dialog.completed = false;
		g_macos_file_dialog.cancelled = false;
		g_macos_file_dialog.paths = NULL;
		g_macos_file_dialog.path_count = 0;

		if (request->type == ska_file_dialog_save) {
			NSSavePanel* panel = [NSSavePanel savePanel];

			if (request->title) {
				[panel setTitle:[NSString stringWithUTF8String:request->title]];
			}
			if (request->default_name) {
				[panel setNameFieldStringValue:[NSString stringWithUTF8String:request->default_name]];
			}

			/* Add file type filters */
			if (request->filters && request->filter_count > 0) {
				NSMutableArray<UTType*>* types = [NSMutableArray array];
				for (int32_t i = 0; i < request->filter_count; i++) {
					/* Get extensions (space-separated) */
					const char* exts = ska_filter_get_exts(&request->filters[i]);
					char* extsCopy = ska_strdup(exts);
					char* token = strtok(extsCopy, " ");
					while (token) {
						/* Remove "*."; keep the extension */
						const char* ext = token;
						if (ext[0] == '*' && ext[1] == '.') {
							ext += 2;
						} else if (ext[0] == '.') {
							ext += 1;
						}
						if (strlen(ext) > 0 && strcmp(ext, "*") != 0) {
							UTType* type = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:ext]];
							if (type) {
								[types addObject:type];
							}
						}
						token = strtok(NULL, " ");
					}
					ska_free(extsCopy);
				}
				if ([types count] > 0) {
					[panel setAllowedContentTypes:types];
				}
			}

			[panel beginWithCompletionHandler:^(NSModalResponse result) {
				if (result == NSModalResponseOK) {
					NSURL* url = [panel URL];
					g_macos_file_dialog.paths = (char**)ska_malloc(sizeof(char*));
					g_macos_file_dialog.paths[0] = ska_strdup([[url path] UTF8String]);
					g_macos_file_dialog.path_count = 1;
					g_macos_file_dialog.cancelled = false;
				} else {
					g_macos_file_dialog.cancelled = true;
				}
				g_macos_file_dialog.completed = true;
			}];
		} else {
			/* Open file or folder */
			NSOpenPanel* panel = [NSOpenPanel openPanel];

			if (request->title) {
				[panel setTitle:[NSString stringWithUTF8String:request->title]];
			}

			if (request->type == ska_file_dialog_open_folder) {
				[panel setCanChooseDirectories:YES];
				[panel setCanChooseFiles:NO];
			} else {
				[panel setCanChooseDirectories:NO];
				[panel setCanChooseFiles:YES];
				[panel setAllowsMultipleSelection:request->allow_multiple];

				/* Add file type filters */
				if (request->filters && request->filter_count > 0) {
					NSMutableArray<UTType*>* types = [NSMutableArray array];
					for (int32_t i = 0; i < request->filter_count; i++) {
						/* Get extensions (space-separated) */
						const char* exts = ska_filter_get_exts(&request->filters[i]);
						char* extsCopy = ska_strdup(exts);
						char* token = strtok(extsCopy, " ");
						while (token) {
							const char* ext = token;
							if (ext[0] == '*' && ext[1] == '.') {
								ext += 2;
							} else if (ext[0] == '.') {
								ext += 1;
							}
							if (strlen(ext) > 0 && strcmp(ext, "*") != 0) {
								UTType* type = [UTType typeWithFilenameExtension:[NSString stringWithUTF8String:ext]];
								if (type) {
									[types addObject:type];
								}
							}
							token = strtok(NULL, " ");
						}
						ska_free(extsCopy);
					}
					if ([types count] > 0) {
						[panel setAllowedContentTypes:types];
					}
				}
			}

			[panel beginWithCompletionHandler:^(NSModalResponse result) {
				if (result == NSModalResponseOK) {
					NSArray<NSURL*>* urls = [panel URLs];
					g_macos_file_dialog.path_count = (int32_t)[urls count];
					g_macos_file_dialog.paths = (char**)ska_malloc(g_macos_file_dialog.path_count * sizeof(char*));
					for (int32_t i = 0; i < g_macos_file_dialog.path_count; i++) {
						g_macos_file_dialog.paths[i] = ska_strdup([[[urls objectAtIndex:i] path] UTF8String]);
					}
					g_macos_file_dialog.cancelled = false;
				} else {
					g_macos_file_dialog.cancelled = true;
				}
				g_macos_file_dialog.completed = true;
			}];
		}

		return true;
	}
}

/* Called from ska_platform_pump_events to check for dialog completion */
static void ska_macos_check_file_dialog(void) {
	if (!g_macos_file_dialog.active || !g_macos_file_dialog.completed) {
		return;
	}

	ska_file_dialog_result_t* result = ska_file_dialog_result_alloc(
		g_macos_file_dialog.id,
		g_macos_file_dialog.title
	);

	if (!g_macos_file_dialog.cancelled && g_macos_file_dialog.paths) {
		for (int32_t i = 0; i < g_macos_file_dialog.path_count; i++) {
			ska_file_dialog_result_add_path(result, g_macos_file_dialog.paths[i]);
			ska_free(g_macos_file_dialog.paths[i]);
		}
		ska_free(g_macos_file_dialog.paths);
		g_macos_file_dialog.paths = NULL;
	}

	/* Cleanup */
	if (g_macos_file_dialog.title) {
		ska_free(g_macos_file_dialog.title);
		g_macos_file_dialog.title = NULL;
	}
	g_macos_file_dialog.active = false;

	/* Post result */
	ska_file_dialog_result_complete(result, g_macos_file_dialog.cancelled);
}

/* ============================================================================
   KVP Store (macOS: NSUserDefaults with NSData)
   ============================================================================ */

SKA_API bool ska_kvpstore_save(const char* key, const void* data, size_t size) {
	if (!ska_kvpstore_validate_key(key)) return false;
	if (!data && size > 0) {
		ska_set_error("ska_kvpstore_save: NULL data with non-zero size");
		return false;
	}

	@autoreleasepool {
		/* Build namespaced key: <app_name>.<key> */
		NSString* full_key = [NSString stringWithFormat:@"%s.%s",
			ska_kvpstore_get_app_name(), key];

		/* Wrap data in NSData */
		NSData* ns_data = [NSData dataWithBytes:data length:size];

		/* Store in NSUserDefaults */
		[[NSUserDefaults standardUserDefaults] setObject:ns_data forKey:full_key];
		[[NSUserDefaults standardUserDefaults] synchronize];
	}

	return true;
}

SKA_API bool ska_kvpstore_load(const char* key, void* opt_buffer, size_t buffer_size, size_t* opt_out_size) {
	if (!ska_kvpstore_validate_key(key)) return false;

	@autoreleasepool {
		NSString* full_key = [NSString stringWithFormat:@"%s.%s",
			ska_kvpstore_get_app_name(), key];

		NSData* ns_data = [[NSUserDefaults standardUserDefaults] dataForKey:full_key];

		if (!ns_data) {
			return false;
		}

		size_t data_size = [ns_data length];

		if (opt_out_size) {
			*opt_out_size = data_size;
		}

		/* Copy to buffer if provided */
		if (opt_buffer && buffer_size > 0) {
			size_t copy_size = (buffer_size < data_size) ? buffer_size : data_size;
			memcpy(opt_buffer, [ns_data bytes], copy_size);
		}
	}

	return true;
}

SKA_API bool ska_kvpstore_delete(const char* key) {
	if (!ska_kvpstore_validate_key(key)) return false;

	@autoreleasepool {
		NSString* full_key = [NSString stringWithFormat:@"%s.%s",
			ska_kvpstore_get_app_name(), key];

		[[NSUserDefaults standardUserDefaults] removeObjectForKey:full_key];
		[[NSUserDefaults standardUserDefaults] synchronize];
	}

	return true;
}

#endif /* SKA_PLATFORM_MACOS */
