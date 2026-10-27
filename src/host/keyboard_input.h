#pragma once
#include <stdint.h>
enum { INPUT_MOUSE_LEFT = 512, INPUT_MOUSE_RIGHT, INPUT_MOUSE_MIDDLE, INPUT_CODE_COUNT };

void keyboard_input_init(void);
void keyboard_input_key(unsigned scancode, int down);
void keyboard_input_mouse_button(unsigned button, int down);
void keyboard_input_release_mouse_buttons(void);
void keyboard_input_clear(void);
int keyboard_input_mouse_mode(void);
