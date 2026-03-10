/*
 * NovexOS - editor.c
 * Simple text editor — nano-like interface.
 * Supports Ctrl+S to save to RAM filesystem.
 */

#include "io.h"
#include "ramfs.h"
#include "string.h"
#include "types.h"

/* ------- External terminal functions (kernel.c) ------- */
extern void terminal_putchar(char c);
extern void terminal_writestring(const char *s);
extern void terminal_set_color(uint8_t color);
extern void terminal_clear(void);
extern uint8_t terminal_get_color(void);

/* ------- Constants ------- */
#define EDITOR_MAX_LINES 100
#define EDITOR_MAX_COLS 79
#define EDITOR_VIEW_LINES                                                      \
  22 /* Lines visible in edit area (25 - 3 for bars)                           \
      */

/* ------- Editor state ------- */
static char lines[EDITOR_MAX_LINES][EDITOR_MAX_COLS + 1];
static int line_count;
static int cursor_row; /* Cursor position in buffer */
static int cursor_col;
static int scroll_offset; /* First visible line */
static int active;
static char filename[32];
static int modified;

/* ------- VGA direct write for status bars ------- */
static uint16_t *vga = (uint16_t *)0xB8000;

static void vga_write_at(int row, int col, const char *text, uint8_t color) {
  while (*text && col < 80) {
    vga[row * 80 + col] = (uint16_t)((uint16_t)*text | ((uint16_t)color << 8));
    text++;
    col++;
  }
}

static void vga_fill_row(int row, uint8_t color) {
  for (int col = 0; col < 80; col++) {
    vga[row * 80 + col] = (uint16_t)(' ' | ((uint16_t)color << 8));
  }
}

/* ------- Integer to string (local) ------- */
static void editor_int_to_str(int n, char *buf) {
  if (n == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  char tmp[12];
  int i = 0;
  while (n > 0) {
    tmp[i++] = '0' + (n % 10);
    n /= 10;
  }
  int j = 0;
  while (i > 0) {
    buf[j++] = tmp[--i];
  }
  buf[j] = '\0';
}

/* ------- Redraw the editor ------- */
static void editor_redraw(void) {
  /* Title bar (row 0) */
  vga_fill_row(0, 0x70); /* Black on light grey */
  vga_write_at(0, 1, "OmegaEdit - ", 0x70);
  vga_write_at(0, 13, filename, 0x70);
  if (modified) {
    int pos = 13 + (int)strlen(filename);
    vga_write_at(0, pos, " [modified]", 0x74);
  }

  /* Edit area (rows 1-22) */
  for (int i = 0; i < EDITOR_VIEW_LINES; i++) {
    int line_idx = scroll_offset + i;
    int vga_row = i + 1;

    /* Clear the row */
    for (int col = 0; col < 80; col++) {
      vga[vga_row * 80 + col] = (uint16_t)(' ' | (0x0F << 8));
    }

    if (line_idx < line_count) {
      vga_write_at(vga_row, 0, lines[line_idx], 0x0F);
    }
  }

  /* Status bar (row 23) */
  vga_fill_row(23, 0x70);
  char buf[12];
  vga_write_at(23, 1, "Ln ", 0x70);
  editor_int_to_str(cursor_row + 1, buf);
  vga_write_at(23, 4, buf, 0x70);
  int pos = 4 + (int)strlen(buf);
  vga_write_at(23, pos, " Col ", 0x70);
  pos += 5;
  editor_int_to_str(cursor_col + 1, buf);
  vga_write_at(23, pos, buf, 0x70);
  pos += (int)strlen(buf) + 2;
  vga_write_at(23, pos, "Lines:", 0x70);
  pos += 6;
  editor_int_to_str(line_count, buf);
  vga_write_at(23, pos, buf, 0x70);

  /* Shortcut bar (row 24) */
  vga_fill_row(24, 0x30); /* Cyan background */
  vga_write_at(24, 1, "ESC", 0x3E);
  vga_write_at(24, 4, " Exit", 0x30);
  vga_write_at(24, 12, "Ctrl+S", 0x3E);
  vga_write_at(24, 18, " Save", 0x30);

  /* Position hardware cursor */
  int screen_row = cursor_row - scroll_offset + 1;
  uint16_t cursor_pos = (uint16_t)(screen_row * 80 + cursor_col);
  outb(0x3D4, 14);
  outb(0x3D5, (uint8_t)(cursor_pos >> 8));
  outb(0x3D4, 15);
  outb(0x3D5, (uint8_t)(cursor_pos & 0xFF));
}

/* ------- Save to ramfs ------- */
static void editor_save(void) {
  /* Flatten lines into a single buffer */
  char buf[RAMFS_MAX_FILESIZE];
  uint32_t pos = 0;

  for (int i = 0; i < line_count; i++) {
    size_t len = strlen(lines[i]);
    if (pos + len + 1 >= RAMFS_MAX_FILESIZE)
      break;
    memcpy(&buf[pos], lines[i], len);
    pos += (uint32_t)len;
    buf[pos++] = '\n';
  }

  if (ramfs_write(filename, buf, pos) == 0) {
    modified = 0;
    /* Flash status bar green briefly */
    vga_fill_row(23, 0x20);
    vga_write_at(23, 1, "Saved!", 0x2F);
  } else {
    vga_fill_row(23, 0x40);
    vga_write_at(23, 1, "Error: could not save!", 0x4F);
  }
}

/* ------- Load from ramfs ------- */
static void editor_load(void) {
  uint32_t size;
  const char *data = ramfs_read(filename, &size);
  if (!data)
    return;

  line_count = 0;
  int col = 0;
  for (uint32_t i = 0; i < size && line_count < EDITOR_MAX_LINES; i++) {
    if (data[i] == '\n') {
      lines[line_count][col] = '\0';
      line_count++;
      col = 0;
    } else if (col < EDITOR_MAX_COLS) {
      lines[line_count][col++] = data[i];
    }
  }
  /* Handle last line without trailing newline */
  if (col > 0 && line_count < EDITOR_MAX_LINES) {
    lines[line_count][col] = '\0';
    line_count++;
  }
}

/* ------- Open editor ------- */
void editor_open(const char *fname) {
  active = 1;
  modified = 0;
  cursor_row = 0;
  cursor_col = 0;
  scroll_offset = 0;

  strncpy(filename, fname, 31);
  filename[31] = '\0';

  /* Clear all lines */
  for (int i = 0; i < EDITOR_MAX_LINES; i++) {
    memset(lines[i], 0, EDITOR_MAX_COLS + 1);
  }
  line_count = 1;

  /* Try to load existing file */
  editor_load();

  editor_redraw();
}

int editor_is_active(void) { return active; }

/* ------- Scroll to keep cursor visible ------- */
static void editor_ensure_visible(void) {
  if (cursor_row < scroll_offset) {
    scroll_offset = cursor_row;
  }
  if (cursor_row >= scroll_offset + EDITOR_VIEW_LINES) {
    scroll_offset = cursor_row - EDITOR_VIEW_LINES + 1;
  }
}

/* ------- Handle input ------- */
void editor_input(char c) {
  if (!active)
    return;

  /* ESC — exit editor */
  if (c == 27) {
    active = 0;
    terminal_clear();
    extern void shell_init(void);
    shell_init();
    return;
  }

  /* Ctrl+S — save (ASCII 0x13 = DC3) */
  if (c == 19) {
    editor_save();
    editor_redraw();
    return;
  }

  /* Enter — split line */
  if (c == '\n') {
    if (line_count >= EDITOR_MAX_LINES)
      return;

    /* Shift lines down */
    for (int i = line_count; i > cursor_row + 1; i--) {
      memcpy(lines[i], lines[i - 1], EDITOR_MAX_COLS + 1);
    }

    /* Split current line at cursor */
    int cur_len = (int)strlen(lines[cursor_row]);
    if (cursor_col < cur_len) {
      strcpy(lines[cursor_row + 1], &lines[cursor_row][cursor_col]);
      lines[cursor_row][cursor_col] = '\0';
    } else {
      lines[cursor_row + 1][0] = '\0';
    }

    line_count++;
    cursor_row++;
    cursor_col = 0;
    modified = 1;
    editor_ensure_visible();
    editor_redraw();
    return;
  }

  /* Backspace */
  if (c == '\b') {
    if (cursor_col > 0) {
      /* Delete char in current line */
      int len = (int)strlen(lines[cursor_row]);
      for (int i = cursor_col - 1; i < len; i++) {
        lines[cursor_row][i] = lines[cursor_row][i + 1];
      }
      cursor_col--;
      modified = 1;
    } else if (cursor_row > 0) {
      /* Merge with previous line */
      int prev_len = (int)strlen(lines[cursor_row - 1]);
      int cur_len = (int)strlen(lines[cursor_row]);

      if (prev_len + cur_len <= EDITOR_MAX_COLS) {
        strcpy(&lines[cursor_row - 1][prev_len], lines[cursor_row]);

        /* Shift lines up */
        for (int i = cursor_row; i < line_count - 1; i++) {
          memcpy(lines[i], lines[i + 1], EDITOR_MAX_COLS + 1);
        }
        memset(lines[line_count - 1], 0, EDITOR_MAX_COLS + 1);
        line_count--;
        cursor_row--;
        cursor_col = prev_len;
        modified = 1;
      }
    }
    editor_ensure_visible();
    editor_redraw();
    return;
  }

  /* Tab — insert 4 spaces */
  if (c == '\t') {
    for (int i = 0; i < 4; i++) {
      int len = (int)strlen(lines[cursor_row]);
      if (len < EDITOR_MAX_COLS) {
        /* Shift right */
        for (int j = len + 1; j > cursor_col; j--) {
          lines[cursor_row][j] = lines[cursor_row][j - 1];
        }
        lines[cursor_row][cursor_col] = ' ';
        cursor_col++;
      }
    }
    modified = 1;
    editor_redraw();
    return;
  }

  /* Regular character */
  int len = (int)strlen(lines[cursor_row]);
  if (len < EDITOR_MAX_COLS) {
    /* Shift right */
    for (int i = len + 1; i > cursor_col; i--) {
      lines[cursor_row][i] = lines[cursor_row][i - 1];
    }
    lines[cursor_row][cursor_col] = c;
    cursor_col++;
    modified = 1;
  }
  editor_redraw();
}
