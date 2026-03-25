/*
 * NovexOS - keyboard.h
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

#define KEY_UP ((char)0x80)
#define KEY_DOWN ((char)0x81)
#define KEY_LEFT ((char)0x82)
#define KEY_RIGHT ((char)0x83)

#endif /* KEYBOARD_H */
