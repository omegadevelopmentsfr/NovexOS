/*
 * NovexOS - desktop.c
 * NovexDE — Graphical Desktop Environment.
 *
 * Rendering strategy — dirty-rect optimisation
 * --------------------------------------------
 * A full redraw (background + windows + taskbar) is issued only when the
 * scene actually changes:
 *   - First frame
 *   - Terminal window shown/hidden or dragged
 *   - Power menu opened/closed
 *   - Terminal buffer written to (keystroke / command output)
 *   - Text cursor blink state toggles (every 500 ms)
 *
 * On all other frames (typically mouse movement at 60 fps):
 *   1. Restore the 32×32 area that was saved before the previous cursor draw.
 *   2. Optionally update only the clock region if the second ticked.
 *   3. Save the new cursor area and draw the cursor.
 *   4. Swap buffers.
 *
 * This reduces per-frame work from ~6 MB of memory ops (full blit + redraw)
 * to ~6 KB (two 32×32 pixel save/restore) on idle mouse-move frames.
 *
 * CPU idle optimisation
 * ---------------------
 * The frame-rate throttle loop now executes HLT instead of spinning.
 * The CPU sleeps until the next PIT interrupt (~1 ms) then re-checks the
 * tick counter.  Reduces idle CPU usage from ~100% to near 0%.
 */

#include "desktop.h"
#include "assets.h"
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

/* =========================================================================
 * Desktop state
 * ========================================================================= */
static int de_active = 0;
static int show_power_menu = 0;

/* Tracks the editor state from the previous frame to detect close transitions.
 */
static int last_editor_active = 0;

/* Terminal window */
static gui_window_t term_window;

/* =========================================================================
 * Embedded terminal
 * ========================================================================= */
#define TERM_COLS 80
#define TERM_ROWS 25
#define TERM_BUF_SZ (TERM_COLS * TERM_ROWS)

static char term_buf[TERM_BUF_SZ];
static int term_cx = 0;
static int term_cy = 0;

/* Set whenever term_buf changes — triggers a full redraw next frame.
 * Written from interrupt context (keyboard IRQ), so declared volatile. */
static volatile int term_content_dirty = 1;

/* =========================================================================
 * Dirty-rendering state
 * ========================================================================= */

/* Cursor sprite save/restore — sized to the largest possible cursor (32x32) */
#define CURSOR_SAVE_MAX 32
static uint32_t cursor_save_buf[CURSOR_SAVE_MAX * CURSOR_SAVE_MAX];
static int32_t cursor_save_x = -1000;
static int32_t cursor_save_y = -1000;
static int cursor_saved = 0;

/* Scene-change tracking */
static int full_dirty = 1; /* force first-frame full redraw */
static int last_term_visible = -1;
static int32_t last_win_x = -1;
static int32_t last_win_y = -1;
static int last_power_menu = -1;
static int last_blink_state = -1;
static uint32_t last_clock_secs = (uint32_t)-1;

/* Pre-computed clock x position (set in desktop_init after fb_init) */
static int32_t clock_x_pos = 0;

/* =========================================================================
 * Internal terminal helpers
 * ========================================================================= */
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
  term_content_dirty = 1; /* mark scene dirty */
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

/* =========================================================================
 * Public helpers for the embedded terminal
 * ========================================================================= */

void desktop_terminal_clear(void) {
  term_clear();
  term_content_dirty = 1;
}

/* Move the software text cursor (used by the in-console editor). */
void desktop_set_cursor(int row, int col) {
  if (row >= 0 && row < TERM_ROWS)
    term_cy = row;
  if (col >= 0 && col < TERM_COLS)
    term_cx = col;
}

/* Called from desktop_run() every frame to detect when the editor closes.
 * Runs entirely in main-loop context — zero races with the render path. */
static void desktop_check_editor_closed(void) {
  extern int shell_is_editor_active(void);
  int editor_now = shell_is_editor_active();
  if (last_editor_active && !editor_now) {
    /* Editor just closed: clear the dirty edit screen and reset the shell. */
    term_clear();
    desktop_print_welcome();
    extern void shell_init(void);
    shell_init();
    full_dirty = 1;
  }
  last_editor_active = editor_now;
}

/* Print the NovexOS welcome banner into the desktop terminal buffer so the
 * user sees it as soon as they open the Console window. */
void desktop_print_welcome(void) {
  const char *banner[] = {"  _   _                      ____   _____ \n",
                          " | \\ | |                    / __ \\ / ____|\n",
                          " |  \\| | _____   _______  _| |  | | (___  \n",
                          " | . ` |/ _ \\ \\ / / _ \\ \\/ / |  | |\\___ \\ \n",
                          " | |\\  | (_) \\ V /  __/>  <| |__| |____) |\n",
                          " |_| \\_|\\___/ \\_/ \\___/_/\\_\\\\____/|_____/ \n",
                          0};
  int i;
  for (i = 0; banner[i]; i++) {
    const char *p = banner[i];
    while (*p)
      term_putchar(*p++);
  }
  const char *info = "\n NovexOS v0.7.2 - Bare Metal Monolithic Kernel\n\n";
  while (*info)
    term_putchar(*info++);
}

/* =========================================================================
 * Clock formatting helper
 * Returns the number of characters written into buf (always 8: HH:MM:SS).
 * ========================================================================= */
static int format_clock(char *buf) {
  uint32_t secs = timer_get_uptime_seconds();
  uint32_t mins = secs / 60;
  uint32_t hours = mins / 60;
  secs %= 60;
  mins %= 60;

  char hs[4], ms[4], ss[4];
  int_to_str(hours, hs);
  int_to_str(mins, ms);
  int_to_str(secs, ss);

  int idx = 0;
  if (hours < 10)
    buf[idx++] = '0';
  for (int i = 0; hs[i]; i++)
    buf[idx++] = hs[i];
  buf[idx++] = ':';
  if (mins < 10)
    buf[idx++] = '0';
  for (int i = 0; ms[i]; i++)
    buf[idx++] = ms[i];
  buf[idx++] = ':';
  if (secs < 10)
    buf[idx++] = '0';
  for (int i = 0; ss[i]; i++)
    buf[idx++] = ss[i];
  buf[idx] = '\0';
  return idx;
}

/* =========================================================================
 * Rendering helpers
 * ========================================================================= */

static void render_wallpaper(void) {
  const sprite_t *bg = assets_get_background();
  fb_draw_background(bg->pixels, bg->width, bg->height);
}

static void render_taskbar(void) {
  uint32_t w = fb_get_width();
  uint32_t h = fb_get_height();
  int32_t ty = (int32_t)(h - TASKBAR_HEIGHT);

  fb_fill_rect(0, ty, (int32_t)w, TASKBAR_HEIGHT, COLOR_TASKBAR);
  fb_hline(0, ty, (int32_t)w, COLOR_ACCENT);

  gui_draw_button(6, ty + 6, 90, 28, get_string(STR_DESKTOP_BTN),
                  show_power_menu ? COLOR_WIN_TITLE : COLOR_TASKBAR,
                  COLOR_TEXT_WHITE);
  gui_draw_button(100, ty + 6, 90, 28, get_string(STR_CONSOLE_BTN),
                  term_window.visible ? COLOR_WIN_TITLE : COLOR_TASKBAR,
                  COLOR_TEXT_WHITE);

  if (show_power_menu) {
    int32_t menu_w = 120, menu_h = 60;
    int32_t menu_x = 6, menu_y = ty - menu_h - 4;
    fb_fill_rect(menu_x, menu_y, menu_w, menu_h, COLOR_WIN_BG);
    fb_draw_rect(menu_x, menu_y, menu_w, menu_h, COLOR_WIN_BORDER);
    fb_fill_rect(menu_x + 2, menu_y + 2, menu_w - 4, 26, COLOR_WIN_BG);
    fb_draw_string(menu_x + 10, menu_y + 8, get_string(STR_RESTART),
                   COLOR_TEXT_WHITE, COLOR_WIN_BG);
    fb_fill_rect(menu_x + 2, menu_y + 30, menu_w - 4, 26, COLOR_WIN_BG);
    fb_draw_string(menu_x + 10, menu_y + 36, get_string(STR_SHUTDOWN),
                   COLOR_TEXT_WHITE, COLOR_WIN_BG);
  }

  /* Clock */
  char time_buf[32];
  int idx = format_clock(time_buf);
  fb_draw_string(clock_x_pos, ty + 12, time_buf, COLOR_TASKBAR_TEXT,
                 COLOR_TASKBAR);
  (void)idx;
}

/*
 * render_clock_only — partial update: erase + redraw only the clock area.
 * Called on non-dirty frames when the second ticks.
 */
static void render_clock_only(void) {
  uint32_t h = fb_get_height();
  int32_t ty = (int32_t)(h - TASKBAR_HEIGHT);
  /* Clock occupies at most 8 chars: "HH:MM:SS" */
  int32_t cw = 8 * FONT_WIDTH + 4;
  fb_fill_rect(clock_x_pos - 2, ty + 4, cw, TASKBAR_HEIGHT - 8, COLOR_TASKBAR);
  char time_buf[32];
  format_clock(time_buf);
  fb_draw_string(clock_x_pos, ty + 12, time_buf, COLOR_TASKBAR_TEXT,
                 COLOR_TASKBAR);
}

static void render_terminal_content(void) {
  int32_t cx = term_window.bounds.x + 4;
  int32_t cy = term_window.bounds.y + TITLEBAR_HEIGHT + 4;

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
      fb_draw_char(cx + col * FONT_WIDTH, cy + row * FONT_HEIGHT, ch,
                   COLOR_TEXT_GREEN, COLOR_WIN_BG);
    }
  }

  /* Blinking text cursor */
  uint32_t ticks = timer_get_ticks();
  if ((ticks / 500) % 2 == 0) {
    int32_t cur_px = cx + term_cx * FONT_WIDTH;
    int32_t cur_py = cy + term_cy * FONT_HEIGHT;
    fb_fill_rect(cur_px, cur_py, FONT_WIDTH, FONT_HEIGHT, COLOR_TEXT_GREEN);
  }
}

static void render_cursor(int32_t mx, int32_t my) {
  const sprite_t *cur = assets_get_cursor(CURSOR_POINTER);
  fb_draw_sprite_alpha(mx - cur->hotspot_x, my - cur->hotspot_y, cur->pixels,
                       cur->width, cur->height, COLOR_CURSOR, /* white fill  */
                       0xFF000000u /* black outline */
  );
}

/* =========================================================================
 * Cursor sprite save / restore
 * Saves the 32×32 backbuffer region that the cursor will overwrite, and
 * restores it before the next cursor draw.  This lets partial-update frames
 * skip the 3 MB background blit entirely.
 * ========================================================================= */
static void cursor_save_area(int32_t mx, int32_t my) {
  const sprite_t *cur = assets_get_cursor(CURSOR_POINTER);
  uint32_t sw = (cur->width < CURSOR_SAVE_MAX) ? cur->width : CURSOR_SAVE_MAX;
  uint32_t sh = (cur->height < CURSOR_SAVE_MAX) ? cur->height : CURSOR_SAVE_MAX;
  int32_t sx = mx - cur->hotspot_x;
  int32_t sy = my - cur->hotspot_y;
  uint32_t *bb = fb_get_backbuffer();
  uint32_t bbw = fb_get_width();
  uint32_t bbh = fb_get_height();

  for (uint32_t row = 0; row < sh; row++) {
    int32_t py = sy + (int32_t)row;
    for (uint32_t col = 0; col < sw; col++) {
      int32_t px = sx + (int32_t)col;
      cursor_save_buf[row * CURSOR_SAVE_MAX + col] =
          (px >= 0 && px < (int32_t)bbw && py >= 0 && py < (int32_t)bbh)
              ? bb[py * bbw + px]
              : 0u;
    }
  }
  cursor_save_x = sx;
  cursor_save_y = sy;
  cursor_saved = 1;
}

static void cursor_restore_area(void) {
  if (!cursor_saved)
    return;
  const sprite_t *cur = assets_get_cursor(CURSOR_POINTER);
  uint32_t sw = (cur->width < CURSOR_SAVE_MAX) ? cur->width : CURSOR_SAVE_MAX;
  uint32_t sh = (cur->height < CURSOR_SAVE_MAX) ? cur->height : CURSOR_SAVE_MAX;
  uint32_t *bb = fb_get_backbuffer();
  uint32_t bbw = fb_get_width();
  uint32_t bbh = fb_get_height();

  for (uint32_t row = 0; row < sh; row++) {
    int32_t py = cursor_save_y + (int32_t)row;
    if (py < 0 || py >= (int32_t)bbh)
      continue;
    for (uint32_t col = 0; col < sw; col++) {
      int32_t px = cursor_save_x + (int32_t)col;
      if (px >= 0 && px < (int32_t)bbw)
        bb[py * bbw + px] = cursor_save_buf[row * CURSOR_SAVE_MAX + col];
    }
  }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

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

  fb_init(vbe_get_framebuffer(), w, h, vbe_get_pitch());
  mouse_set_bounds((int32_t)w, (int32_t)h);

  /* Pre-compute clock x position (8 chars "HH:MM:SS") */
  clock_x_pos = (int32_t)(fb_get_width()) - 8 * FONT_WIDTH - 12;

  int32_t win_w = 660, win_h = 480;
  int32_t win_x = ((int32_t)w - win_w) / 2;
  int32_t win_y = ((int32_t)(h - TASKBAR_HEIGHT) - win_h) / 2;
  gui_window_init(&term_window, win_x, win_y, win_w, win_h,
                  get_string(STR_CONSOLE_BTN));
  term_window.visible = 0;

  term_clear();
  desktop_print_welcome();
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
    uint32_t now = timer_get_ticks();

    if (loop_count == 0) {
      serial_print("[DE] First loop iteration. Ticks: ");
      serial_print_hex(now);
      serial_print("\r\n");
      loop_count = 1;
    }

    /* Throttle to ~60 FPS.
     * HLT suspends the CPU until the next interrupt (~1 ms PIT tick),
     * eliminating the busy-spin that consumed 100% CPU. */
    if (now - last_tick < 16) {
      __asm__ volatile("hlt");
      continue;
    }

    if (loop_count == 1) {
      serial_print("[DE] Passed tick throttle. Rendering first frame...\r\n");
      loop_count = 2;
    }
    last_tick = now;

    /* --- Poll for editor-closed transition -------------------------------- */
    desktop_check_editor_closed();

    /* --- Mouse state ---------------------------------------------------- */
    int32_t mx = mouse_get_x();
    int32_t my = mouse_get_y();
    uint8_t buttons = mouse_get_buttons();
    uint32_t h = fb_get_height();

    /* --- Mouse interaction (modifies scene state for next render) -------- */
    if (buttons & 0x01) {           /* left button pressed */
      if (!(last_buttons & 0x01)) { /* just clicked */
        int32_t ty = (int32_t)(h - TASKBAR_HEIGHT);
        if (show_power_menu) {
          int32_t menu_w = 120, menu_h = 60;
          int32_t menu_x = 6, menu_y = ty - menu_h - 4;
          if (mx >= menu_x && mx <= menu_x + menu_w && my >= menu_y &&
              my <= menu_y + menu_h) {
            if (my < menu_y + 30) {
              reboot();
            } else {
              outw(0x604, 0x2000);
              outw(0xB004, 0x2000);
              __asm__ volatile("cli; hlt");
            }
          } else if (!(my >= ty && mx >= 6 && mx <= 96)) {
            show_power_menu = 0;
          }
        }

        if (my >= ty) {
          if (mx >= 6 && mx <= 96 && my >= ty + 6 && my <= ty + 34)
            show_power_menu = !show_power_menu;
          else if (mx >= 100 && mx <= 190 && my >= ty + 6 && my <= ty + 34)
            term_window.visible = !term_window.visible;
        } else if (term_window.visible && !show_power_menu) {
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
      if (term_window.dragging && term_window.visible) {
        term_window.bounds.x = mx - term_window.drag_offset_x;
        term_window.bounds.y = my - term_window.drag_offset_y;
      }
    } else {
      term_window.dragging = 0;
    }
    last_buttons = buttons;

    /* --- Dirty check ---------------------------------------------------- */
    uint32_t ticks = timer_get_ticks();
    int blink_state = (int)((ticks / 500) % 2);

    int need_full =
        full_dirty || (term_window.visible != last_term_visible) ||
        (term_window.visible && term_window.bounds.x != last_win_x) ||
        (term_window.visible && term_window.bounds.y != last_win_y) ||
        (show_power_menu != last_power_menu) ||
        (term_window.visible && term_content_dirty) ||
        (term_window.visible && blink_state != last_blink_state);

    /* --- Render --------------------------------------------------------- */
    if (need_full) {
      /* Full scene redraw */
      render_wallpaper();
      if (term_window.visible) {
        gui_window_render(&term_window);
        render_terminal_content();
      }
      render_taskbar();

      /* Update tracking state */
      last_term_visible = term_window.visible;
      last_win_x = term_window.bounds.x;
      last_win_y = term_window.bounds.y;
      last_power_menu = show_power_menu;
      last_blink_state = blink_state;
      last_clock_secs = timer_get_uptime_seconds();
      term_content_dirty = 0;
      full_dirty = 0;
      cursor_saved = 0; /* saved area is stale after full redraw */
    } else {
      /* Partial update: restore previous cursor area */
      cursor_restore_area();

      /* Update clock region if the second changed */
      uint32_t cur_secs = timer_get_uptime_seconds();
      if (cur_secs != last_clock_secs) {
        render_clock_only();
        last_clock_secs = cur_secs;
      }
    }

    /* Always: save the area under the new cursor position, draw cursor */
    cursor_save_area(mx, my);
    render_cursor(mx, my);

    /* Swap back-buffer to screen */
    fb_swap();
  }
}

void desktop_handle_key(char c) {
  if (!de_active)
    return;
  if (term_window.visible) {
    extern void shell_input(char c);
    shell_input(c);
  }
}

int desktop_is_active(void) { return de_active; }