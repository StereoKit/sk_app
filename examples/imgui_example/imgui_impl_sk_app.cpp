// Dear ImGui sk_app Backend
// Platform backend for sk_app (window/input handling)

#include "imgui_impl_sk_app.h"
#include <sk_app.h>
#include "imgui.h"
#include <cstring>
#include <cstdlib>

// Backend data stored in io.BackendPlatformUserData
struct ImGui_ImplSkApp_Data
{
	ska_window_t* window;
	double        time;

	// Mouse handling
	int32_t       mouse_buttons_down;

	ImGui_ImplSkApp_Data() { memset(this, 0, sizeof(*this)); }
};

static ImGui_ImplSkApp_Data* ImGui_ImplSkApp_GetBackendData()
{
	return ImGui::GetCurrentContext() ? (ImGui_ImplSkApp_Data*)ImGui::GetIO().BackendPlatformUserData : nullptr;
}

// Scancode to ImGuiKey mapping
static ImGuiKey ImGui_ImplSkApp_ScancodeToImGuiKey(ska_scancode_ scancode)
{
	switch (scancode)
	{
		case ska_scancode_tab:          return ImGuiKey_Tab;
		case ska_scancode_left:         return ImGuiKey_LeftArrow;
		case ska_scancode_right:        return ImGuiKey_RightArrow;
		case ska_scancode_up:           return ImGuiKey_UpArrow;
		case ska_scancode_down:         return ImGuiKey_DownArrow;
		case ska_scancode_pageup:       return ImGuiKey_PageUp;
		case ska_scancode_pagedown:     return ImGuiKey_PageDown;
		case ska_scancode_home:         return ImGuiKey_Home;
		case ska_scancode_end:          return ImGuiKey_End;
		case ska_scancode_insert:       return ImGuiKey_Insert;
		case ska_scancode_delete:       return ImGuiKey_Delete;
		case ska_scancode_backspace:    return ImGuiKey_Backspace;
		case ska_scancode_space:        return ImGuiKey_Space;
		case ska_scancode_return:       return ImGuiKey_Enter;
		case ska_scancode_escape:       return ImGuiKey_Escape;
		case ska_scancode_apostrophe:   return ImGuiKey_Apostrophe;
		case ska_scancode_comma:        return ImGuiKey_Comma;
		case ska_scancode_minus:        return ImGuiKey_Minus;
		case ska_scancode_period:       return ImGuiKey_Period;
		case ska_scancode_slash:        return ImGuiKey_Slash;
		case ska_scancode_semicolon:    return ImGuiKey_Semicolon;
		case ska_scancode_equals:       return ImGuiKey_Equal;
		case ska_scancode_leftbracket:  return ImGuiKey_LeftBracket;
		case ska_scancode_backslash:    return ImGuiKey_Backslash;
		case ska_scancode_rightbracket: return ImGuiKey_RightBracket;
		case ska_scancode_grave:        return ImGuiKey_GraveAccent;
		case ska_scancode_capslock:     return ImGuiKey_CapsLock;
		case ska_scancode_scrolllock:   return ImGuiKey_ScrollLock;
		case ska_scancode_printscreen:  return ImGuiKey_PrintScreen;
		case ska_scancode_pause:        return ImGuiKey_Pause;
		case ska_scancode_0:            return ImGuiKey_0;
		case ska_scancode_1:            return ImGuiKey_1;
		case ska_scancode_2:            return ImGuiKey_2;
		case ska_scancode_3:            return ImGuiKey_3;
		case ska_scancode_4:            return ImGuiKey_4;
		case ska_scancode_5:            return ImGuiKey_5;
		case ska_scancode_6:            return ImGuiKey_6;
		case ska_scancode_7:            return ImGuiKey_7;
		case ska_scancode_8:            return ImGuiKey_8;
		case ska_scancode_9:            return ImGuiKey_9;
		case ska_scancode_a:            return ImGuiKey_A;
		case ska_scancode_b:            return ImGuiKey_B;
		case ska_scancode_c:            return ImGuiKey_C;
		case ska_scancode_d:            return ImGuiKey_D;
		case ska_scancode_e:            return ImGuiKey_E;
		case ska_scancode_f:            return ImGuiKey_F;
		case ska_scancode_g:            return ImGuiKey_G;
		case ska_scancode_h:            return ImGuiKey_H;
		case ska_scancode_i:            return ImGuiKey_I;
		case ska_scancode_j:            return ImGuiKey_J;
		case ska_scancode_k:            return ImGuiKey_K;
		case ska_scancode_l:            return ImGuiKey_L;
		case ska_scancode_m:            return ImGuiKey_M;
		case ska_scancode_n:            return ImGuiKey_N;
		case ska_scancode_o:            return ImGuiKey_O;
		case ska_scancode_p:            return ImGuiKey_P;
		case ska_scancode_q:            return ImGuiKey_Q;
		case ska_scancode_r:            return ImGuiKey_R;
		case ska_scancode_s:            return ImGuiKey_S;
		case ska_scancode_t:            return ImGuiKey_T;
		case ska_scancode_u:            return ImGuiKey_U;
		case ska_scancode_v:            return ImGuiKey_V;
		case ska_scancode_w:            return ImGuiKey_W;
		case ska_scancode_x:            return ImGuiKey_X;
		case ska_scancode_y:            return ImGuiKey_Y;
		case ska_scancode_z:            return ImGuiKey_Z;
		case ska_scancode_f1:           return ImGuiKey_F1;
		case ska_scancode_f2:           return ImGuiKey_F2;
		case ska_scancode_f3:           return ImGuiKey_F3;
		case ska_scancode_f4:           return ImGuiKey_F4;
		case ska_scancode_f5:           return ImGuiKey_F5;
		case ska_scancode_f6:           return ImGuiKey_F6;
		case ska_scancode_f7:           return ImGuiKey_F7;
		case ska_scancode_f8:           return ImGuiKey_F8;
		case ska_scancode_f9:           return ImGuiKey_F9;
		case ska_scancode_f10:          return ImGuiKey_F10;
		case ska_scancode_f11:          return ImGuiKey_F11;
		case ska_scancode_f12:          return ImGuiKey_F12;
		case ska_scancode_lctrl:        return ImGuiKey_LeftCtrl;
		case ska_scancode_lshift:       return ImGuiKey_LeftShift;
		case ska_scancode_lalt:         return ImGuiKey_LeftAlt;
		case ska_scancode_lgui:         return ImGuiKey_LeftSuper;
		case ska_scancode_rctrl:        return ImGuiKey_RightCtrl;
		case ska_scancode_rshift:       return ImGuiKey_RightShift;
		case ska_scancode_ralt:         return ImGuiKey_RightAlt;
		case ska_scancode_rgui:         return ImGuiKey_RightSuper;
		default:                        return ImGuiKey_None;
	}
}

// Update key modifiers from ska_keymod_
static void ImGui_ImplSkApp_UpdateKeyModifiers(uint16_t ska_modifiers)
{
	ImGuiIO& io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Ctrl,  (ska_modifiers & ska_keymod_ctrl)  != 0);
	io.AddKeyEvent(ImGuiMod_Shift, (ska_modifiers & ska_keymod_shift) != 0);
	io.AddKeyEvent(ImGuiMod_Alt,   (ska_modifiers & ska_keymod_alt)   != 0);
	io.AddKeyEvent(ImGuiMod_Super, (ska_modifiers & ska_keymod_gui)   != 0);
}

bool ImGui_ImplSkApp_Init(ska_window_t* window)
{
	ImGuiIO& io = ImGui::GetIO();
	IM_ASSERT(io.BackendPlatformUserData == nullptr && "Already initialized a platform backend!");

	// Setup backend capabilities flags
	ImGui_ImplSkApp_Data* bd = new ImGui_ImplSkApp_Data();
	io.BackendPlatformUserData = (void*)bd;
	io.BackendPlatformName = "imgui_impl_sk_app";
	io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;  // We can honor GetMouseCursor() values (optional)

	bd->window = window;
	bd->time = 0.0;

	// Setup clipboard callbacks
	io.SetClipboardTextFn = [](void*, const char* text) {
		ska_clipboard_set_text(text);
	};

	io.GetClipboardTextFn = [](void*) -> const char* {
		static char* clipboard_buffer = nullptr;

		// Free previous buffer
		if (clipboard_buffer) {
			free(clipboard_buffer);
		}

		// Get clipboard text (returns malloc'd string)
		clipboard_buffer = ska_clipboard_get_text();
		return clipboard_buffer ? clipboard_buffer : "";
	};

	// NOTE: IME and mouse cursor shape are NOT YET IMPLEMENTED
	// See report at the end of this file for missing features

	return true;
}

void ImGui_ImplSkApp_Shutdown()
{
	ImGui_ImplSkApp_Data* bd = ImGui_ImplSkApp_GetBackendData();
	IM_ASSERT(bd != nullptr && "No platform backend to shutdown!");

	ImGuiIO& io = ImGui::GetIO();
	io.BackendPlatformName = nullptr;
	io.BackendPlatformUserData = nullptr;
	io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;

	delete bd;
}

// Screen coordinates to the pixels io.DisplaySize is expressed in. This is the
// drawable ratio rather than the DPI scale, which keeps the math exact even
// where the two differ by rounding.
static float ImGui_ImplSkApp_InputScale(ska_window_t* window)
{
	int32_t w, h, dw, dh;
	ska_window_get_content_size (window, &w,  &h);
	ska_window_get_drawable_size(window, &dw, &dh);
	return w > 0 ? (float)dw / (float)w : 1.0f;
}

float ImGui_ImplSkApp_GetDpiScale()
{
	ImGui_ImplSkApp_Data* bd = ImGui_ImplSkApp_GetBackendData();
	if (!bd || !bd->window)
		return 1.0f;
	return ska_window_get_dpi_scale(bd->window);
}

void ImGui_ImplSkApp_NewFrame()
{
	ImGui_ImplSkApp_Data* bd = ImGui_ImplSkApp_GetBackendData();
	IM_ASSERT(bd != nullptr && "Context or backend not initialized!");

	ImGuiIO& io = ImGui::GetIO();

	// ImGui is driven entirely in pixels here: the display size is the drawable
	// size, the style is scaled by the DPI factor, and fonts are rasterized at
	// that factor. Mouse input arrives in screen coordinates and is scaled to
	// match, so DisplayFramebufferScale stays at one.
	int32_t display_w, display_h;
	ska_window_get_drawable_size(bd->window, &display_w, &display_h);
	io.DisplaySize             = ImVec2((float)display_w, (float)display_h);
	io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

	// Setup time step
	double current_time = ska_time_get_elapsed_s();
	io.DeltaTime = bd->time > 0.0 ? (float)(current_time - bd->time) : (1.0f / 60.0f);
	bd->time = current_time;

	// Mouse state is in screen coordinates; ImGui is in pixels
	float   scale = ImGui_ImplSkApp_InputScale(bd->window);
	int32_t mouse_x, mouse_y;
	ska_mouse_get_state(&mouse_x, &mouse_y);
	io.AddMousePosEvent((float)mouse_x * scale, (float)mouse_y * scale);

	// Update mouse cursor shape
	if (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange)
		return;

	ImGuiMouseCursor imgui_cursor = ImGui::GetMouseCursor();
	if (imgui_cursor == ImGuiMouseCursor_None || io.MouseDrawCursor)
	{
		// Hide OS cursor
		ska_cursor_show(false);
	}
	else
	{
		// Show OS cursor and set shape
		ska_cursor_show(true);

		ska_system_cursor_ ska_cursor = ska_system_cursor_arrow;
		switch (imgui_cursor)
		{
			case ImGuiMouseCursor_Arrow:       ska_cursor = ska_system_cursor_arrow; break;
			case ImGuiMouseCursor_TextInput:   ska_cursor = ska_system_cursor_ibeam; break;
			case ImGuiMouseCursor_ResizeAll:   ska_cursor = ska_system_cursor_sizeall; break;
			case ImGuiMouseCursor_ResizeNS:    ska_cursor = ska_system_cursor_sizens; break;
			case ImGuiMouseCursor_ResizeEW:    ska_cursor = ska_system_cursor_sizewe; break;
			case ImGuiMouseCursor_ResizeNESW:  ska_cursor = ska_system_cursor_sizenesw; break;
			case ImGuiMouseCursor_ResizeNWSE:  ska_cursor = ska_system_cursor_sizenwse; break;
			case ImGuiMouseCursor_Hand:        ska_cursor = ska_system_cursor_hand; break;
			case ImGuiMouseCursor_NotAllowed:  ska_cursor = ska_system_cursor_no; break;
			default: break;
		}
		ska_cursor_set(ska_cursor);
	}
}

bool ImGui_ImplSkApp_ProcessEvent(const ska_event_t* event)
{
	ImGuiIO& io = ImGui::GetIO();
	ImGui_ImplSkApp_Data* bd = ImGui_ImplSkApp_GetBackendData();

	switch (event->type)
	{
		case ska_event_mouse_motion:
		{
			// Screen coordinates in, pixels out, matching io.DisplaySize
			float scale = bd ? ImGui_ImplSkApp_InputScale(bd->window) : 1.0f;
			io.AddMousePosEvent((float)event->mouse_motion.x * scale,
			                    (float)event->mouse_motion.y * scale);
			return true;
		}

		case ska_event_mouse_wheel:
		{
			float wheel_x = (float)event->mouse_wheel.x;
			float wheel_y = (float)event->mouse_wheel.y;
			io.AddMouseWheelEvent(wheel_x, wheel_y);
			return true;
		}

		case ska_event_mouse_button_down:
		case ska_event_mouse_button_up:
		{
			int32_t mouse_button = -1;
			if (event->mouse_button.button == ska_mouse_button_left)   mouse_button = 0;
			if (event->mouse_button.button == ska_mouse_button_right)  mouse_button = 1;
			if (event->mouse_button.button == ska_mouse_button_middle) mouse_button = 2;
			if (event->mouse_button.button == ska_mouse_button_x1)     mouse_button = 3;
			if (event->mouse_button.button == ska_mouse_button_x2)     mouse_button = 4;

			if (mouse_button == -1)
				break;

			io.AddMouseButtonEvent(mouse_button, (event->type == ska_event_mouse_button_down));

			if (event->type == ska_event_mouse_button_down)
				bd->mouse_buttons_down |= (1 << mouse_button);
			else
				bd->mouse_buttons_down &= ~(1 << mouse_button);

			return true;
		}

		case ska_event_text_input:
		{
			io.AddInputCharactersUTF8(event->text.text);
			return true;
		}

		case ska_event_key_down:
		case ska_event_key_up:
		{
			ImGui_ImplSkApp_UpdateKeyModifiers(event->keyboard.modifiers);
			ImGuiKey key = ImGui_ImplSkApp_ScancodeToImGuiKey(event->keyboard.scancode);
			io.AddKeyEvent(key, (event->type == ska_event_key_down));
			// Note: We don't have access to the actual scancode value for SetKeyEventNativeData
			return true;
		}

		case ska_event_window_focus_gained:
		{
			io.AddFocusEvent(true);
			return true;
		}

		case ska_event_window_focus_lost:
		{
			io.AddFocusEvent(false);
			return true;
		}

		default:
			break;
	}

	return false;
}

/*
 * MISSING FEATURES REPORT
 * =======================
 *
 * The following features for the backend are not yet implemented
 * due to missing functionality in sk_app:

 * 1. IME (INPUT METHOD EDITOR) SUPPORT
 *    - Need: ska_ime_set_position(ska_window_t*, int32_t x, int32_t y, int32_t w, int32_t h)
 *    - This allows the system IME (for Asian languages, etc.) to position correctly
 *
 * 2. MOUSE SOURCE DISCRIMINATION
 *    - Need: Way to distinguish mouse vs touchscreen input
 *    - Could add a `source` field to ska_mouse_motion/button events
 *    - Values: ska_mouse_source_mouse, ska_mouse_source_touch
 *
 * 3. GLOBAL MOUSE POSITION
 *    - Already have: ska_mouse_get_global_state()
 *    - But need: ska_mouse_set_global_position(int32_t x, int32_t y)
 *    - This is used when io.WantSetMousePos is true (rare, but used in some ImGui features)
 *
 * 4. MOUSE CAPTURE
 *    - Need: ska_mouse_capture(bool enable)
 *    - Allows tracking mouse even when it leaves the window (during drag operations)
 *
 * PRIORITY RANKING:
 * -----------------
 * MEDIUM PRIORITY (improves UX):
 *   - IME support (#1)
 *   - Mouse capture (#4)
 *
 * LOW PRIORITY (nice to have):
 *   - Mouse source discrimination (#2)
 *   - Global mouse position (#3)
 */
