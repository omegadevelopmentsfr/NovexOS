/*
 * NovexOS - mouse.c
 * PS/2 Mouse driver — IRQ12.
 * Handles 3-byte PS/2 mouse packets, tracks position and buttons.
 */

#include "mouse.h"
#include "io.h"
#include "isr.h"

/* Mouse state */
static volatile int32_t mouse_x = 0;
static volatile int32_t mouse_y = 0;
static volatile uint8_t mouse_buttons = 0;
static int32_t bound_x = 1024;
static int32_t bound_y = 768;

/* Packet assembly */
static uint8_t mouse_cycle = 0;
static int8_t mouse_bytes[3];

/* Wait for PS/2 controller to be ready for input */
static void mouse_wait_write(void) {
  int timeout = 100000;
  while (timeout--) {
    if (!(inb(0x64) & 0x02))
      return;
  }
}

/* Wait for PS/2 controller to have data */
static void mouse_wait_read(void) {
  int timeout = 100000;
  while (timeout--) {
    if (inb(0x64) & 0x01)
      return;
  }
}

/* Send a command to the mouse (via PS/2 controller aux port) */
static void mouse_write(uint8_t data) {
  mouse_wait_write();
  outb(0x64, 0xD4); /* Tell controller: next byte goes to aux device */
  mouse_wait_write();
  outb(0x60, data);
}

/* Read a byte from PS/2 data port */
static uint8_t mouse_read(void) {
  mouse_wait_read();
  return inb(0x60);
}

/* IRQ12 handler: decode 3-byte PS/2 mouse packets */
static void mouse_handler(registers_t *regs) {
  (void)regs;

  uint8_t status = inb(0x64);
  /* Bit 0 = output buffer full, bit 5 = mouse data */
  if (!(status & 0x20))
    return;

  int8_t data = (int8_t)inb(0x60);

  switch (mouse_cycle) {
  case 0:
    mouse_bytes[0] = data;
    /* Validate: bit 3 must always be set in byte 0 */
    if (data & 0x08)
      mouse_cycle = 1;
    break;
  case 1:
    mouse_bytes[1] = data;
    mouse_cycle = 2;
    break;
  case 2:
    mouse_bytes[2] = data;
    mouse_cycle = 0;

    /* Decode packet */
    mouse_buttons = (uint8_t)(mouse_bytes[0] & 0x07);

    /* X movement (sign-extend if needed) */
    int32_t dx = mouse_bytes[1];
    if (mouse_bytes[0] & 0x10)
      dx |= (int32_t)0xFFFFFF00; /* Sign extend */

    /* Y movement (inverted: PS/2 Y is up-positive, screen is down-positive) */
    int32_t dy = mouse_bytes[2];
    if (mouse_bytes[0] & 0x20)
      dy |= (int32_t)0xFFFFFF00;

    /* Normalize mouse speed: x2 is usually comfortable at this resolution */
    dx *= 2;
    dy *= 2;

    mouse_x += dx;
    mouse_y -= dy; /* Invert Y */

    /* Clamp to screen bounds */
    if (mouse_x < 0)
      mouse_x = 0;
    if (mouse_y < 0)
      mouse_y = 0;
    if (mouse_x >= bound_x)
      mouse_x = bound_x - 1;
    if (mouse_y >= bound_y)
      mouse_y = bound_y - 1;
    break;
  }
}

void mouse_set_bounds(int32_t max_x, int32_t max_y) {
  bound_x = max_x;
  bound_y = max_y;
  /* Also reset cursor to center */
  mouse_x = max_x / 2;
  mouse_y = max_y / 2;
}

void mouse_init(void) {
  uint8_t status_byte;

  /* Enable the auxiliary mouse device */
  mouse_wait_write();
  outb(0x64, 0xA8); /* Enable aux port */

  /* Enable IRQ12 in the PS/2 controller */
  mouse_wait_write();
  outb(0x64, 0x20); /* Read command byte */
  mouse_wait_read();
  status_byte = inb(0x60);
  status_byte |= 0x02;  /* Enable IRQ12 */
  status_byte &= ~0x20; /* Enable mouse clock */
  mouse_wait_write();
  outb(0x64, 0x60); /* Write command byte */
  mouse_wait_write();
  outb(0x60, status_byte);

  /* Tell the mouse to use default settings */
  mouse_write(0xF6);
  mouse_read(); /* ACK */

  /* Set sample rate to 200 Hz for smoother movement */
  mouse_write(0xF3);
  mouse_read(); /* ACK */
  mouse_write(200);
  mouse_read(); /* ACK */

  /* Set resolution to maximum (3 = 8 counts/mm) */
  mouse_write(0xE8);
  mouse_read(); /* ACK */
  mouse_write(3);
  mouse_read(); /* ACK */

  /* Enable data reporting */
  mouse_write(0xF4);
  mouse_read(); /* ACK */

  /* Set initial position to center */
  mouse_x = bound_x / 2;
  mouse_y = bound_y / 2;

  /* Register IRQ12 handler (ISR 44 = IRQ12) */
  register_interrupt_handler(44, mouse_handler);
}

int32_t mouse_get_x(void) { return mouse_x; }
int32_t mouse_get_y(void) { return mouse_y; }
uint8_t mouse_get_buttons(void) { return mouse_buttons; }
