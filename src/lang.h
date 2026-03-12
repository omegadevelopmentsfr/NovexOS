/*
 * NovexOS - lang.h
 * Simple Language Manager
 */

#ifndef LANG_H
#define LANG_H

#include "types.h"

typedef enum { LANG_EN = 1, LANG_FR = 2 } lang_id_t;

typedef enum {
  STR_BOOT_COMPLETE,
  STR_SELECT_KB,
  STR_SELECT_RES,
  STR_DESKTOP_BTN,
  STR_CONSOLE_BTN,
  STR_RESTART,
  STR_SHUTDOWN,
  STR_TIMER_OK,
  STR_KEYBOARD_OK,
  STR_MOUSE_OK,
  STR_MAX
} string_id_t;

void lang_set(lang_id_t lang);
lang_id_t lang_get(void);
const char *get_string(string_id_t id);

#endif
