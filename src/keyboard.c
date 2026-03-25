/*
 * NovexOS - keyboard.c
 * PS/2 Keyboard driver — Scancode Set 1 → ASCII (QWERTY/AZERTY).
 * Supports Ctrl key modifier for editor save (Ctrl+S).
 */

#include "keyboard.h"
#include "desktop.h"
#include "io.h"
#include "isr.h"
#include "shell.h"

/* ------- Scancode Set 1 → ASCII: QWERTY ------- */
static const char scancode_qwerty[128] = {
    0,   27,   '1',  '2', '3',  '4', '5', '6', '7', '8', '9', '0', '-',
    '=', '\b', '\t', 'q', 'w',  'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
    '[', ']',  '\n', 0,   'a',  's', 'd', 'f', 'g', 'h', 'j', 'k', 'l',
    ';', '\'', '`',  0,   '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',',
    '.', '/',  0,    '*', 0,    ' ', 0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,    0,   0,    0,   '7', '8', '9', '-', '4', '5', '6',
    '+', '1',  '2',  '3', '0',  '.', 0,   0,   0,   0,   0,   0,   0,
};

static const char scancode_qwerty_shift[128] = {
    0,   27,   '!',  '@', '#', '$', '%', '^', '&', '*', '(', ')', '_',
    '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    '{', '}',  '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L',
    ':', '"',  '~',  0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<',
    '>', '?',  0,    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,
};

/* ------- Scancode Set 1 → ASCII: AZERTY ------- */
static const char scancode_azerty[128] = {
    0,   27,   '&',  'e', '"', '\'', '(', '-', 'e', '_', 'c', 'a', ')',
    '=', '\b', '\t', 'a', 'z', 'e',  'r', 't', 'y', 'u', 'i', 'o', 'p',
    '^', '$',  '\n', 0,   'q', 's',  'd', 'f', 'g', 'h', 'j', 'k', 'l',
    'm', 'u',  '*',  0,   '<', 'w',  'x', 'c', 'v', 'b', 'n', ',', ';',
    ':', '!',  0,    '*', 0,   ' ',  0,   0,   0,   0,   0,   0,   0,
};

static const char scancode_azerty_shift[128] = {
    0,   27,   '1',  '2', '3', '4', '5', '6', '7', '8', '9', '0', '_',
    '+', '\b', '\t', 'A', 'Z', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    '^', '$',  '\n', 0,   'Q', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L',
    'M', '%',  '*',  0,   '>', 'W', 'X', 'C', 'V', 'B', 'N', '?', '.',
    '/', '!',  0,    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,
};

static const char *active_layout = scancode_qwerty;
static const char *active_layout_shift = scancode_qwerty_shift;
static int ctrl_held = 0;
static int shift_held = 0;

/* getchar state */
static volatile char getchar_buffer = 0;
static volatile bool getchar_waiting = false;

char keyboard_getchar(void) {
  getchar_waiting = true;
  while (getchar_buffer == 0) {
    /* Enable interrupts and wait for next interrupt */
    __asm__ volatile("sti; hlt");
  }
  char c = getchar_buffer;
  getchar_buffer = 0;
  getchar_waiting = false;
  return c;
}

void keyboard_set_layout(int layout) {
  if (layout == 1) {
    active_layout = scancode_azerty;
    active_layout_shift = scancode_azerty_shift;
  } else {
    active_layout = scancode_qwerty;
    active_layout_shift = scancode_qwerty_shift;
  }
}

int keyboard_get_layout(void) {
  return (active_layout == scancode_azerty) ? 1 : 0;
}

void keyboard_handler(void) {
  uint8_t scancode = inb(0x60);

  /* Track Ctrl (0x1D) */
  if (scancode == 0x1D) {
    ctrl_held = 1;
    return;
  }
  if (scancode == (0x1D | 0x80)) {
    ctrl_held = 0;
    return;
  }

  /* Track Shift (LShift: 0x2A, RShift: 0x36) */
  if (scancode == 0x2A || scancode == 0x36) {
    shift_held = 1;
    return;
  }
  if (scancode == (0x2A | 0x80) || scancode == (0x36 | 0x80)) {
    shift_held = 0;
    return;
  }

  /* Ignore key-release events */
  if (scancode & 0x80) {
    return;
  }

  /* ESC key */
  if (scancode == 0x01) {
    shell_input(27);
    return;
  }

  /* Arrow keys (Set 1 scancodes) */
  if (scancode == 0x48) {
    shell_input(KEY_UP);
    return;
  }
  if (scancode == 0x50) {
    shell_input(KEY_DOWN);
    return;
  }
  if (scancode == 0x4B) {
    shell_input(KEY_LEFT);
    return;
  }
  if (scancode == 0x4D) {
    shell_input(KEY_RIGHT);
    return;
  }

  /* Ctrl+key combinations */
  if (ctrl_held) {
    char c = scancode_qwerty[scancode];
    if (c == 's') {
      shell_input(19); /* Ctrl+S */
      return;
    }
    return;
  }

  char c = shift_held ? active_layout_shift[scancode] : active_layout[scancode];
  if (c != 0) {
    if (getchar_waiting) {
      getchar_buffer = c;
    } else if (desktop_is_active()) {
      desktop_handle_key(c);
    } else {
      shell_input(c);
    }
  }
}

/* Wrapper for ISR callback */
static void keyboard_callback(registers_t *regs) {
  (void)regs;
  keyboard_handler();
}

void keyboard_init(void) { register_interrupt_handler(33, keyboard_callback); }
