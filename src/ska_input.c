//
// sk_app - Input state implementation

#include "ska_internal.h"

void ska_input_state_init(ska_input_state_t* state) {
	memset(state, 0, sizeof(*state));
	state->cursor_visible = true;
	ska_text_queue_init(&state->text_queue);
	state->text_input_type = ska_text_input_type_text;
}

void ska_input_state_reset(ska_input_state_t* state) {
	memset(state->keyboard, 0, sizeof(state->keyboard));
	state->key_modifiers = 0;
	state->mouse_buttons = 0;
	state->mouse_xrel    = 0;
	state->mouse_yrel    = 0;
	state->mouse_delta_x = 0;
	state->mouse_delta_y = 0;
}

// Motion is reported both ways because the two are consumed differently: events
// carry a single delta, while a polling app wants everything since it last read.
void ska_input_add_relative(int32_t xrel, int32_t yrel) {
	g_ska.input_state.mouse_xrel    = xrel;
	g_ska.input_state.mouse_yrel    = yrel;
	g_ska.input_state.mouse_delta_x += xrel;
	g_ska.input_state.mouse_delta_y += yrel;
}

uint16_t ska_input_state_derive_modifiers(const ska_input_state_t* state) {
	uint16_t mods = 0;
	if (state->keyboard[ska_scancode_lshift]) mods |= ska_keymod_lshift;
	if (state->keyboard[ska_scancode_rshift]) mods |= ska_keymod_rshift;
	if (state->keyboard[ska_scancode_lctrl])  mods |= ska_keymod_lctrl;
	if (state->keyboard[ska_scancode_rctrl])  mods |= ska_keymod_rctrl;
	if (state->keyboard[ska_scancode_lalt])   mods |= ska_keymod_lalt;
	if (state->keyboard[ska_scancode_ralt])   mods |= ska_keymod_ralt;
	if (state->keyboard[ska_scancode_lgui])   mods |= ska_keymod_lgui;
	if (state->keyboard[ska_scancode_rgui])   mods |= ska_keymod_rgui;
	return mods;
}
