/*
 * OmegaOS - keyboard.h
 * Keyboard driver interface.
 */

#ifndef KEYBOARD_H
#define KEYBOARD_H

void keyboard_init(void);
void keyboard_handler(void);
int keyboard_get_layout(void);
void keyboard_set_layout(int layout);
int keyboard_is_layout_selected(void);
char keyboard_getchar(void);

#endif /* KEYBOARD_H */
