/*
 * NovexOS - desktop.c
 * NovexDE — Graphical Desktop Environment.
 *
 * Renders a desktop with:
 *   - Gradient wallpaper
 *   - Taskbar with NovexOS logo and system clock
 *   - Mouse cursor
 *   - Terminal window with shell integration
 *
 * Double-buffered at ~30 FPS using PIT ticks.
 */

#include "desktop.h"
#include "fb.h"
#include "font.h"
#include "gui.h"
#include "io.h"
#include "lang.h"
#include "mouse.h"
#include "string.h"
#include "timer.h"
#include "vbe.h"

/* External terminal functions for text-mode fallback */
extern void terminal_writestring(const char *s);
extern void terminal_set_color(uint8_t color);

/* --- Desktop state --- */
static int de_active = 0;
static int show_power_menu = 0;

/* Terminal window */
static gui_window_t term_window;

/* Embedded terminal: text buffer for the built-in terminal */
#define TERM_COLS 80
#define TERM_ROWS 28
#define TERM_BUF_SZ (TERM_COLS * TERM_ROWS)

static char term_buf[TERM_BUF_SZ];
static int term_cx = 0; /* cursor column */
static int term_cy = 0; /* cursor row */

/* Cursor for the mouse arrow sprite */
static const uint8_t cursor_bitmap[19] = {
    0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF, 0xFF, 0xFC,
    0xF8, 0xF0, 0xD8, 0x8C, 0x0C, 0x06, 0x06, 0x03, 0x03};

static const uint8_t cursor_mask[19] = {
    0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF, 0xFF, 0xFF, 0xFE,
    0xFC, 0xF8, 0xFC, 0xDE, 0x1E, 0x0F, 0x0F, 0x07, 0x07};

/* --- Internal terminal helpers --- */
static void term_clear(void) {
  memset(term_buf, ' ', TERM_BUF_SZ);
  term_cx = 0;
  term_cy = 0;
}

static void term_scroll(void) {
  memcpy(term_buf, term_buf + TERM_COLS, TERM_COLS * (TERM_ROWS - 1));
  memset(term_buf + TERM_COLS * (TERM_ROWS - 1), ' ', TERM_COLS);
  term_cy = TERM_ROWS - 1;
}

static void term_putchar(char c) {
  if (c == '\n') {
    term_cx = 0;
    term_cy++;
    if (term_cy >= TERM_ROWS)
      term_scroll();
  } else if (c == '\b') {
    if (term_cx > 0) {
      term_cx--;
      term_buf[term_cy * TERM_COLS + term_cx] = ' ';
    }
  } else {
    if (term_cx >= TERM_COLS) {
      term_cx = 0;
      term_cy++;
      if (term_cy >= TERM_ROWS)
        term_scroll();
    }
    term_buf[term_cy * TERM_COLS + term_cx] = c;
    term_cx++;
  }
}

void desktop_terminal_putchar(char c) { term_putchar(c); }

/* --- Rendering --- */

/* Render gradient wallpaper */
static void render_wallpaper(void) {
  uint32_t w = fb_get_width();
  uint32_t h = fb_get_height();
  uint32_t desktop_h = h - TASKBAR_HEIGHT;

  for (uint32_t y = 0; y < desktop_h; y++) {
    /* Interpolate between top and bottom colors */
    uint8_t r_top = 0x0E, g_top = 0x0E, b_top = 0x28; /* Deep dark blue */
    uint8_t r_bot = 0x16, g_bot = 0x34, b_bot = 0x60; /* Medium blue */

    uint8_t r = (uint8_t)(r_top + (r_bot - r_top) * y / desktop_h);
    uint8_t g = (uint8_t)(g_top + (g_bot - g_top) * y / desktop_h);
    uint8_t b = (uint8_t)(b_top + (b_bot - b_top) * y / desktop_h);

    uint32_t color = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    fb_hline(0, (int32_t)y, (int32_t)w, color);
  }
}

/* Render taskbar */
static void render_taskbar(void) {
  uint32_t w = fb_get_width();
  uint32_t h = fb_get_height();
  int32_t ty = (int32_t)(h - TASKBAR_HEIGHT);

  /* Taskbar background */
  fb_fill_rect(0, ty, (int32_t)w, TASKBAR_HEIGHT, COLOR_TASKBAR);

  /* Top border line (accent) */
  fb_hline(0, ty, (int32_t)w, COLOR_ACCENT);

  /* NovexOS/Menu button on the left */
  gui_draw_button(6, ty + 6, 90, 28, get_string(STR_DESKTOP_BTN),
                  show_power_menu ? COLOR_WIN_TITLE : COLOR_TASKBAR,
                  COLOR_TEXT_WHITE);

  /* Terminal button */
  gui_draw_button(100, ty + 6, 90, 28, get_string(STR_CONSOLE_BTN),
                  term_window.visible ? COLOR_WIN_TITLE : COLOR_TASKBAR,
                  COLOR_TEXT_WHITE);

  /* Power Menu rendering (if active) */
  if (show_power_menu) {
    int32_t menu_w = 120;
    int32_t menu_h = 60;
    int32_t menu_x = 6;
    int32_t menu_y = ty - menu_h - 4;

    /* Background and border */
    fb_fill_rect(menu_x, menu_y, menu_w, menu_h, COLOR_WIN_BG);
    fb_draw_rect(menu_x, menu_y, menu_w, menu_h, COLOR_WIN_BORDER);

    /* Restart Option */
    fb_fill_rect(menu_x + 2, menu_y + 2, menu_w - 4, 26,
                 COLOR_WIN_BG); /* Hover state could go here */
    fb_draw_string(menu_x + 10, menu_y + 8, get_string(STR_RESTART),
                   COLOR_TEXT_WHITE, COLOR_WIN_BG);

    /* Shutdown Option */
    fb_fill_rect(menu_x + 2, menu_y + 30, menu_w - 4, 26, COLOR_WIN_BG);
    fb_draw_string(menu_x + 10, menu_y + 36, get_string(STR_SHUTDOWN),
                   COLOR_TEXT_WHITE, COLOR_WIN_BG);
  }

  /* System clock (uptime) on the right */
  char time_buf[32];
  uint32_t secs = timer_get_uptime_seconds();
  uint32_t mins = secs / 60;
  uint32_t hours = mins / 60;
  secs %= 60;
  mins %= 60;

  /* Format HH:MM:SS */
  char h_str[4], m_str[4], s_str[4];
  int_to_str(hours, h_str);
  int_to_str(mins, m_str);
  int_to_str(secs, s_str);

  int idx = 0;
  /* Hours */
  if (hours < 10)
    time_buf[idx++] = '0';
  for (int i = 0; h_str[i]; i++)
    time_buf[idx++] = h_str[i];
  time_buf[idx++] = ':';
  /* Minutes */
  if (mins < 10)
    time_buf[idx++] = '0';
  for (int i = 0; m_str[i]; i++)
    time_buf[idx++] = m_str[i];
  time_buf[idx++] = ':';
  /* Seconds */
  if (secs < 10)
    time_buf[idx++] = '0';
  for (int i = 0; s_str[i]; i++)
    time_buf[idx++] = s_str[i];
  time_buf[idx] = '\0';

  int32_t clock_x = (int32_t)(w - (uint32_t)(idx * FONT_WIDTH) - 12);
  fb_draw_string(clock_x, ty + 12, time_buf, COLOR_TASKBAR_TEXT, COLOR_TASKBAR);
}

/* Render the terminal window content */
static void render_terminal_content(void) {
  int32_t cx = term_window.bounds.x + 4;
  int32_t cy = term_window.bounds.y + TITLEBAR_HEIGHT + 4;

  /* Determine how many rows fit in the window */
  int32_t avail_h = term_window.bounds.h - TITLEBAR_HEIGHT - 8;
  int32_t max_rows = avail_h / FONT_HEIGHT;
  if (max_rows > TERM_ROWS)
    max_rows = TERM_ROWS;

  int32_t avail_w = term_window.bounds.w - 8;
  int32_t max_cols = avail_w / FONT_WIDTH;
  if (max_cols > TERM_COLS)
    max_cols = TERM_COLS;

  for (int32_t row = 0; row < max_rows; row++) {
    for (int32_t col = 0; col < max_cols; col++) {
      char ch = term_buf[row * TERM_COLS + col];
      uint32_t fg = COLOR_TEXT_GREEN;

      /* Color the prompt differently */
      fb_draw_char(cx + col * FONT_WIDTH, cy + row * FONT_HEIGHT, ch, fg,
                   COLOR_WIN_BG);
    }
  }

  /* Draw blinking cursor (solid block) */
  uint32_t ticks = timer_get_ticks();
  if ((ticks / 500) % 2 == 0) { /* Blink every ~0.5s at 1000Hz */
    int32_t cur_px = cx + term_cx * FONT_WIDTH;
    int32_t cur_py = cy + term_cy * FONT_HEIGHT;
    fb_fill_rect(cur_px, cur_py, FONT_WIDTH, FONT_HEIGHT, COLOR_TEXT_GREEN);
  }
}

/* Render mouse cursor */
static void render_cursor(int32_t mx, int32_t my) {
  for (int row = 0; row < 19; row++) {
    for (int col = 0; col < 8; col++) {
      if (cursor_mask[row] & (0x80 >> col)) {
        uint32_t color = (cursor_bitmap[row] & (0x80 >> col))
                             ? COLOR_CURSOR
                             : 0xFF000000; /* Black outline */
        fb_put_pixel(mx + col, my + row, color);
      }
    }
  }
}

/* --- Public API --- */

void desktop_init(void) {
  if (!vbe_is_available()) {
    terminal_set_color(0x0C);
    terminal_writestring("ERROR: VBE framebuffer not available.\n");
    terminal_writestring("Make sure GRUB is configured for graphics mode.\n");
    terminal_set_color(0x07);
    return;
  }

  uint32_t w = vbe_get_width();
  uint32_t h = vbe_get_height();

  /* Initialize framebuffer drawing system */
  fb_init(vbe_get_framebuffer(), w, h, vbe_get_pitch());

  /* Set mouse bounds */
  mouse_set_bounds((int32_t)w, (int32_t)h);

  /* Initialize terminal window — centered */
  int32_t win_w = 660;
  int32_t win_h = 480;
  int32_t win_x = ((int32_t)w - win_w) / 2;
  int32_t win_y = ((int32_t)(h - TASKBAR_HEIGHT) - win_h) / 2;
  gui_window_init(&term_window, win_x, win_y, win_w, win_h,
                  get_string(STR_CONSOLE_BTN));
  term_window.visible = 0; /* Hidden by default per user request */

  /* Clear terminal buffer */
  term_clear();

  de_active = 1;
}

void desktop_run(void) {
  if (!de_active)
    return;

  serial_print("[DE] Entering desktop_run()...\r\n");

  uint32_t last_tick = 0;
  static uint8_t last_buttons = 0;
  uint8_t loop_count = 0;

  while (de_active) {
    /* Throttle to ~60 FPS (every 16 ticks at 1000Hz) */
    uint32_t now = timer_get_ticks();

    if (loop_count == 0) {
      serial_print("[DE] First loop iteration. Ticks: ");
      serial_print_hex(now);
      serial_print("\r\n");
      loop_count = 1;
    }

    if (now - last_tick < 16) {
      /* Do NOT hlt if interrupts are active and we are in main loop */
      continue;
    }

    if (loop_count == 1) {
      serial_print("[DE] Passed tick throttle. Rendering first frame...\r\n");
      loop_count = 2;
    }
    last_tick = now;

    /* --- Render frame --- */

    /* 1. Wallpaper */
    render_wallpaper();

    /* 2. Terminal window */
    if (term_window.visible) {
      gui_window_render(&term_window);
      render_terminal_content();
    }

    /* 3. Taskbar (on top of everything except cursor) */
    render_taskbar();

    /* 4. Handle mouse interaction */
    int32_t mx = mouse_get_x();
    int32_t my = mouse_get_y();
    uint8_t buttons = mouse_get_buttons();
    uint32_t h = fb_get_height();

    /* Window dragging and clicking */
    if (buttons & 0x01) {           /* Left button pressed */
      if (!(last_buttons & 0x01)) { /* Just clicked */
        int32_t ty = (int32_t)(h - TASKBAR_HEIGHT);
        if (show_power_menu) {
          int32_t menu_w = 120;
          int32_t menu_h = 60;
          int32_t menu_x = 6;
          int32_t menu_y = ty - menu_h - 4;

          if (mx >= menu_x && mx <= menu_x + menu_w && my >= menu_y &&
              my <= menu_y + menu_h) {
            /* Clicked inside power menu! */
            if (my < menu_y + 30) {
              /* Restart */
              reboot();
            } else {
              /* Shutdown (QEMU ACPI) */
              outw(0x604, 0x2000);
              outw(0xB004, 0x2000); /* Alternate Bochs port */
              __asm__ volatile("cli; hlt");
            }
          } else if (!(my >= ty && mx >= 6 && mx <= 96)) {
            /* Clicked outside menu AND outside the NovexOS button -> close menu
             */
            show_power_menu = 0;
          }
        }

        if (my >= ty) {
          /* Taskbar clicked */
          if (mx >= 6 && mx <= 96 && my >= ty + 6 && my <= ty + 34) {
            show_power_menu = !show_power_menu;
          } else if (mx >= 100 && mx <= 190 && my >= ty + 6 && my <= ty + 34) {
            term_window.visible = !term_window.visible;
          }
        } else if (term_window.visible && !show_power_menu) {
          /* Check titlebar click */
          gui_rect_t titlebar = {term_window.bounds.x, term_window.bounds.y,
                                 term_window.bounds.w, TITLEBAR_HEIGHT};
          if (gui_point_in_rect(mx, my, &titlebar)) {
            int32_t close_x = term_window.bounds.x + term_window.bounds.w - 22;
            int32_t close_y = term_window.bounds.y + 6;
            gui_rect_t close_rect = {close_x, close_y, 16, 16};
            if (gui_point_in_rect(mx, my, &close_rect)) {
              term_window.visible = 0;
            } else {
              term_window.dragging = 1;
              term_window.drag_offset_x = mx - term_window.bounds.x;
              term_window.drag_offset_y = my - term_window.bounds.y;
            }
          }
        }
      }

      /* Drag update */
      if (term_window.dragging && term_window.visible) {
        term_window.bounds.x = mx - term_window.drag_offset_x;
        term_window.bounds.y = my - term_window.drag_offset_y;
      }
    } else {
      term_window.dragging = 0;
    }

    last_buttons = buttons;

    /* 5. Mouse cursor (always on top) */
    render_cursor(mx, my);

    /* 6. Swap buffers */
    fb_swap();
  }
}

void desktop_handle_key(char c) {
  if (!de_active)
    return;

  /* If the window is focused (visible for now), route directly to real shell!
   */
  if (term_window.visible) {
    extern void shell_input(char c);
    shell_input(c);
  }
}

int desktop_is_active(void) { return de_active; }
