/*
 * NovexOS - python.c  v3.0
 * ===================================================================
 * Python 3 subset interpreter — bare-metal x86_64, zero libc.
 *
 * MODULES  : math, random, string, sys, os, time, turtle
 * TYPES    : int, float, str, bool, None
 * FEATURES : import X, from X import Y, X.attr, X.method(),
 *            str.method(), float literals, **, //, augmented assign,
 *            if/elif/else, while, for..in..range(), break/continue/pass
 * BUILTINS : print, input, len, str, int, float, bool, abs, round,
 *            pow, min, max, type, isinstance, chr, ord, hex, bin, oct,
 *            range (in for), sorted (no-op on str), sum, divmod
 * ===================================================================
 */

/* Disable GCC's misleading-indentation warning — intentional compact style */
#pragma GCC diagnostic ignored "-Wmisleading-indentation"

#include "python.h"
#define double long double
#include "fb.h"

#include "desktop.h"
#include "fat32.h"
#include "font.h"
#include "gui.h"
#include "keyboard.h"
#include "ramfs.h"
#include "shell.h"
#include "string.h"
#include "timer.h"
#include "types.h"
#include "vbe.h"

/* ================================================================
 *  External kernel I/O
 * ================================================================ */
extern void terminal_putchar(char c);
extern void terminal_writestring(const char *s);
extern void terminal_set_color(uint8_t col);
extern void terminal_backspace(void);

/* ================================================================
 *  Output helpers
 * ================================================================ */
static void py_putc(char c) { terminal_putchar(c); }
static void py_puts(const char *s) { terminal_writestring(s); }

static void py_error(const char *msg) {
  terminal_set_color(0x04);
  py_puts("\n[PythonError] ");
  py_puts(msg);
  py_putc('\n');
  terminal_set_color(0x07);
}

/* ================================================================
 *  Freestanding math  (no libm)
 * ================================================================ */
#define MY_PI 3.14159265358979323846
#define MY_E 2.71828182845904523536
#define MY_TAU 6.28318530717958647692
#define MY_LN2 0.69314718055994530942
#define MY_LN10 2.30258509299404568402

static double my_fabs(double x) { return x < 0.0 ? -x : x; }
static double my_trunc(double x) { return (double)(int64_t)x; }
static double my_floor(double x) {
  double t = my_trunc(x);
  return (x < t) ? t - 1.0 : t;
}
static double my_ceil(double x) {
  double t = my_trunc(x);
  return (x > t) ? t + 1.0 : t;
}
static double my_copysign(double x, double y) {
  return y < 0.0 ? -my_fabs(x) : my_fabs(x);
}

static double my_sqrt(double x) {
  if (x <= 0.0)
    return 0.0;
  double r = x > 1.0 ? x * 0.5 : 1.0;
  for (int i = 0; i < 64; i++) {
    double n = 0.5 * (r + x / r);
    if (my_fabs(n - r) < 1e-15 * my_fabs(r))
      break;
    r = n;
  }
  return r;
}
static double my_exp(double x) {
  int64_t k = (int64_t)(x / MY_LN2);
  double r = x - (double)k * MY_LN2, term = 1.0, sum = 1.0;
  for (int n = 1; n <= 20; n++) {
    term *= r / (double)n;
    sum += term;
  }
  if (k >= 0) {
    for (int64_t i = 0; i < k && i < 1023; i++)
      sum *= 2.0;
  } else {
    for (int64_t i = 0; i > k && i > -1023; i--)
      sum *= 0.5;
  }
  return sum;
}
static double my_log(double x) {
  if (x <= 0.0)
    return -1e308;
  int e = 0;
  double m = x;
  while (m >= 1.0) {
    m *= 0.5;
    e++;
  }
  while (m < 0.5) {
    m *= 2.0;
    e--;
  }
  double y = (m - 1.0) / (m + 1.0), y2 = y * y, term = y, sum = y;
  for (int n = 1; n <= 25; n++) {
    term *= y2;
    sum += term / (double)(2 * n + 1);
  }
  return 2.0 * sum + (double)e * MY_LN2;
}
static double my_log2(double x) { return my_log(x) / MY_LN2; }
static double my_log10(double x) { return my_log(x) / MY_LN10; }
static double my_sin(double x) {
  x -= MY_TAU * my_floor(x / MY_TAU);
  if (x > MY_PI)
    x -= MY_TAU;
  double x2 = x * x, term = x, sum = x;
  for (int n = 1; n <= 12; n++) {
    term *= -x2 / ((double)(2 * n) * (double)(2 * n + 1));
    sum += term;
  }
  return sum;
}
static double my_cos(double x) {
  x -= MY_TAU * my_floor(x / MY_TAU);
  if (x > MY_PI)
    x -= MY_TAU;
  double x2 = x * x, term = 1.0, sum = 1.0;
  for (int n = 1; n <= 12; n++) {
    term *= -x2 / ((double)(2 * n - 1) * (double)(2 * n));
    sum += term;
  }
  return sum;
}
static double my_tan(double x) {
  double c = my_cos(x);
  return my_fabs(c) < 1e-15 ? 1e300 : my_sin(x) / c;
}
static double my_atan(double x) {
  bool neg = (x < 0.0), rec = false;
  if (neg)
    x = -x;
  if (x > 1.0) {
    x = 1.0 / x;
    rec = true;
  }
  double x2 = x * x, term = x, sum = x;
  for (int n = 1; n <= 30; n++) {
    term *= -x2;
    sum += term / (double)(2 * n + 1);
  }
  if (rec)
    sum = MY_PI * 0.5 - sum;
  return neg ? -sum : sum;
}
static double my_atan2(double y, double x) {
  if (x > 0.0)
    return my_atan(y / x);
  if (x < 0.0 && y >= 0.0)
    return my_atan(y / x) + MY_PI;
  if (x < 0.0 && y < 0.0)
    return my_atan(y / x) - MY_PI;
  if (x == 0.0 && y > 0.0)
    return MY_PI * 0.5;
  if (x == 0.0 && y < 0.0)
    return -MY_PI * 0.5;
  return 0.0;
}
static double my_asin(double x) {
  if (x < -1.0)
    x = -1.0;
  if (x > 1.0)
    x = 1.0;
  return my_atan2(x, my_sqrt(1.0 - x * x));
}
static double my_acos(double x) { return MY_PI * 0.5 - my_asin(x); }
static double my_hypot(double a, double b) { return my_sqrt(a * a + b * b); }
static double my_pow(double base, double e) {
  if (e == my_trunc(e) && my_fabs(e) < 1e9) {
    int64_t ei = (int64_t)e;
    bool neg = (ei < 0);
    if (neg)
      ei = -ei;
    double r = 1.0, b = base;
    while (ei > 0) {
      if (ei & 1)
        r *= b;
      b *= b;
      ei >>= 1;
    }
    return neg ? 1.0 / r : r;
  }
  if (base <= 0.0)
    return 0.0;
  return my_exp(e * my_log(base));
}
static int64_t my_factorial(int64_t n) {
  if (n < 0 || n > 20)
    return -1;
  int64_t r = 1;
  for (int64_t i = 2; i <= n; i++)
    r *= i;
  return r;
}
static int64_t my_gcd(int64_t a, int64_t b) {
  if (a < 0)
    a = -a;
  if (b < 0)
    b = -b;
  while (b) {
    int64_t t = b;
    b = a % b;
    a = t;
  }
  return a;
}
static int64_t my_lcm(int64_t a, int64_t b) {
  if (a == 0 || b == 0)
    return 0;
  return (a / my_gcd(a, b)) * b;
}

/* ================================================================
 *  double -> ASCII  (no sprintf)
 * ================================================================ */
static void dtoa(double v, char *buf, int maxlen) {
  if (maxlen < 4) {
    if (maxlen > 0)
      buf[0] = 0;
    return;
  }
  int pos = 0;
  if (v < 0.0) {
    buf[pos++] = '-';
    v = -v;
  }
  if (v >= 1e15) {
    int64_t iv = (int64_t)v;
    char tmp[22];
    int ti = 0;
    if (iv == 0)
      tmp[ti++] = '0';
    else
      while (iv > 0 && ti < 20) {
        tmp[ti++] = (char)('0' + (int)(iv % 10));
        iv /= 10;
      }
    for (int j = ti - 1; j >= 0 && pos < maxlen - 2; j--)
      buf[pos++] = tmp[j];
    buf[pos++] = '.';
    buf[pos++] = '0';
    buf[pos] = 0;
    return;
  }
  int64_t ip = (int64_t)v;
  double frac = v - (double)ip;
  if (ip == 0) {
    buf[pos++] = '0';
  } else {
    char tmp[20];
    int ti = 0;
    int64_t ii = ip;
    while (ii > 0 && ti < 19) {
      tmp[ti++] = (char)('0' + (int)(ii % 10));
      ii /= 10;
    }
    for (int j = ti - 1; j >= 0 && pos < maxlen - 1; j--)
      buf[pos++] = tmp[j];
  }
  buf[pos++] = '.';
  int ds = pos, lnz = ds - 1;
  for (int d = 0; d < 10 && pos < maxlen - 1; d++) {
    frac *= 10.0;
    int dig = (int)frac;
    if (dig > 9)
      dig = 9;
    if (dig < 0)
      dig = 0;
    buf[pos++] = (char)('0' + dig);
    if (dig != 0)
      lnz = pos - 1;
    frac -= (double)dig;
  }
  pos = (lnz < ds) ? ds + 1 : lnz + 1;
  buf[pos] = 0;
}

/* ================================================================
 *  Value types
 * ================================================================ */
#define PY_INT 0
#define PY_STR 1
#define PY_BOOL 2
#define PY_NONE 3
#define PY_ERR 4
#define PY_FLOAT 5
#define PY_MOD 6
#define PY_BUILTIN 7 /* bound function: mod+name */

#define MOD_MATH 1
#define MOD_RANDOM 2
#define MOD_STRING 3
#define MOD_SYS 4
#define MOD_OS 5
#define MOD_TIME 6
#define MOD_TURTLE 7

#define PY_SMAX 120

typedef struct {
  uint8_t t, mod, _p[2];
  int32_t i;
  double d;
  char s[PY_SMAX];
} PyVal;

static PyVal mkint(int32_t n) {
  PyVal v;
  v.t = PY_INT;
  v.mod = 0;
  v.i = n;
  v.d = 0;
  v.s[0] = 0;
  return v;
}
static PyVal mkfloat(double d) {
  PyVal v;
  v.t = PY_FLOAT;
  v.mod = 0;
  v.i = 0;
  v.d = d;
  v.s[0] = 0;
  return v;
}
static PyVal mkbool(int b) {
  PyVal v;
  v.t = PY_BOOL;
  v.mod = 0;
  v.i = b ? 1 : 0;
  v.d = 0;
  v.s[0] = 0;
  return v;
}
static PyVal mknone(void) {
  PyVal v;
  v.t = PY_NONE;
  v.mod = 0;
  v.i = 0;
  v.d = 0;
  v.s[0] = 0;
  return v;
}
static PyVal mkerr(void) {
  PyVal v;
  v.t = PY_ERR;
  v.mod = 0;
  v.i = 0;
  v.d = 0;
  v.s[0] = 0;
  return v;
}
static PyVal mkmod(uint8_t m) {
  PyVal v;
  v.t = PY_MOD;
  v.mod = m;
  v.i = 0;
  v.d = 0;
  v.s[0] = 0;
  return v;
}
static PyVal mkstr(const char *s) {
  PyVal v;
  v.t = PY_STR;
  v.mod = 0;
  v.i = 0;
  v.d = 0;
  int i = 0;
  while (s[i] && i < PY_SMAX - 1) {
    v.s[i] = s[i];
    i++;
  }
  v.s[i] = 0;
  return v;
}
static PyVal mkbuiltin(uint8_t m, const char *fn) {
  PyVal v;
  v.t = PY_BUILTIN;
  v.mod = m;
  v.i = 0;
  v.d = 0;
  int i = 0;
  while (fn[i] && i < PY_SMAX - 1) {
    v.s[i] = fn[i];
    i++;
  }
  v.s[i] = 0;
  return v;
}

static double val_num(PyVal v) {
  if (v.t == PY_FLOAT)
    return v.d;
  return (double)v.i;
}
static int py_truthy(PyVal v) {
  if (v.t == PY_INT || v.t == PY_BOOL)
    return v.i != 0;
  if (v.t == PY_FLOAT)
    return v.d != 0.0;
  if (v.t == PY_STR)
    return v.s[0] != 0;
  return 0;
}
static void val_to_buf(PyVal v, char *buf, int ml) {
  if (ml <= 0)
    return;
  if (v.t == PY_FLOAT) {
    dtoa(v.d, buf, ml);
    return;
  }
  if (v.t == PY_INT) {
    if (v.i == 0) {
      buf[0] = '0';
      buf[1] = 0;
      return;
    }
    char tmp[12];
    int ti = 0;
    bool neg = (v.i < 0);
    int32_t n = neg ? -v.i : v.i;
    while (n > 0 && ti < 11) {
      tmp[ti++] = (char)('0' + n % 10);
      n /= 10;
    }
    int bi = 0;
    if (neg && bi < ml - 1)
      buf[bi++] = '-';
    for (int j = ti - 1; j >= 0 && bi < ml - 1; j--)
      buf[bi++] = tmp[j];
    buf[bi] = 0;
  } else if (v.t == PY_BOOL) {
    const char *s = v.i ? "True" : "False";
    int i = 0;
    while (s[i] && i < ml - 1) {
      buf[i] = s[i];
      i++;
    }
    buf[i] = 0;
  } else if (v.t == PY_NONE) {
    const char *s = "None";
    int i = 0;
    while (s[i] && i < ml - 1) {
      buf[i] = s[i];
      i++;
    }
    buf[i] = 0;
  } else if (v.t == PY_STR) {
    int i = 0;
    while (v.s[i] && i < ml - 1) {
      buf[i] = v.s[i];
      i++;
    }
    buf[i] = 0;
  } else {
    buf[0] = '?';
    buf[1] = 0;
  }
}
static void py_print_val(PyVal v) {
  char b[PY_SMAX + 20];
  val_to_buf(v, b, (int)sizeof(b));
  py_puts(b);
}

/* ================================================================
 *  Variable store  (64 slots)
 * ================================================================ */
#define PY_MAXVARS 64
static char pyn[PY_MAXVARS][32];
static PyVal pyv[PY_MAXVARS];
static int pync = 0;

static void py_vars_clear(void) { pync = 0; }
static bool py_var_exists(const char *n) {
  for (int i = 0; i < pync; i++)
    if (strcmp(pyn[i], n) == 0)
      return true;
  return false;
}
static PyVal py_var_get(const char *n) {
  for (int i = 0; i < pync; i++)
    if (strcmp(pyn[i], n) == 0)
      return pyv[i];
  char msg[56] = "name not defined: ";
  int k = 18;
  for (int j = 0; n[j] && k < 55; j++)
    msg[k++] = n[j];
  msg[k] = 0;
  py_error(msg);
  return mkerr();
}
static void py_var_set(const char *n, PyVal val) {
  for (int i = 0; i < pync; i++) {
    if (strcmp(pyn[i], n) == 0) {
      pyv[i] = val;
      return;
    }
  }
  if (pync < PY_MAXVARS) {
    int i = 0;
    while (n[i] && i < 31) {
      pyn[pync][i] = n[i];
      i++;
    }
    pyn[pync][i] = 0;
    pyv[pync] = val;
    pync++;
  } else
    py_error("too many variables");
}

/* ================================================================
 *  RNG  (LCG)
 * ================================================================ */
static uint64_t rng_st = 0x853C49E6748FEA9BULL;
static uint64_t rng_next(void) {
  rng_st = rng_st * 6364136223846793005ULL + 1442695040888963407ULL;
  return rng_st;
}
static double rng_f(void) {
  return (double)(rng_next() >> 11) / (double)(1ULL << 53);
}

/* ================================================================
 *  Color helpers  (used by turtle)
 * ================================================================ */
static uint32_t color_from_name(const char *n) {
  if (strcmp(n, "red") == 0)
    return 0xFFFF0000;
  if (strcmp(n, "green") == 0)
    return 0xFF00C000;
  if (strcmp(n, "blue") == 0)
    return 0xFF0000FF;
  if (strcmp(n, "white") == 0)
    return 0xFFFFFFFF;
  if (strcmp(n, "black") == 0)
    return 0xFF000000;
  if (strcmp(n, "yellow") == 0)
    return 0xFFFFFF00;
  if (strcmp(n, "cyan") == 0)
    return 0xFF00FFFF;
  if (strcmp(n, "magenta") == 0)
    return 0xFFFF00FF;
  if (strcmp(n, "orange") == 0)
    return 0xFFFF8000;
  if (strcmp(n, "purple") == 0)
    return 0xFF800080;
  if (strcmp(n, "pink") == 0)
    return 0xFFFF69B4;
  if (strcmp(n, "brown") == 0)
    return 0xFF8B4513;
  if (strcmp(n, "gray") == 0 || strcmp(n, "grey") == 0)
    return 0xFF808080;
  if (strcmp(n, "silver") == 0)
    return 0xFFC0C0C0;
  if (strcmp(n, "gold") == 0)
    return 0xFFFFD700;
  if (strcmp(n, "lime") == 0)
    return 0xFF00FF00;
  if (strcmp(n, "navy") == 0)
    return 0xFF000080;
  if (strcmp(n, "teal") == 0)
    return 0xFF008080;
  if (strcmp(n, "olive") == 0)
    return 0xFF808000;
  if (strcmp(n, "maroon") == 0)
    return 0xFF800000;
  return 0xFFFFFFFF;
}
static uint32_t color_from_rgb(int r, int g, int b) {
  return 0xFF000000 | ((uint32_t)(r & 0xFF) << 16) |
         ((uint32_t)(g & 0xFF) << 8) | (uint32_t)(b & 0xFF);
}

/* ================================================================
 *  Turtle state
 * ================================================================ */
static double trt_x, trt_y; /* current pos (turtle coords) */
static double trt_heading;  /* degrees, 0=East, 90=North */
static bool trt_pen_down;
static uint32_t trt_pen_color;
static uint32_t trt_bg_color;
static bool trt_visible;
static int trt_pen_width;
static bool trt_init_done;
static volatile bool trt_active = false;

/* Turtle owns a canvas rather than the whole framebuffer.  Keeping the
 * pixels separately lets the desktop repaint it after a drag or an exposed
 * window, which is the basis for other graphical applications as well. */
#define TURTLE_CANVAS_W 640
#define TURTLE_CANVAS_H 400
static uint32_t trt_canvas[TURTLE_CANVAS_W * TURTLE_CANVAS_H];
static gui_window_t trt_window;
static bool trt_window_init_done;
static char trt_window_title[PY_SMAX] = "Python Turtle";

int python_is_turtle_active(void) { return trt_active; }

static void trt_request_present(void) {
  desktop_request_redraw();
  /* A Python script runs synchronously from the console.  Present right
   * away so a short script such as turtle.forward(50) still opens a window. */
  desktop_present_now();
}

/* Convert turtle -> canvas coords */
static int32_t trt_sx(double tx) {
  return TURTLE_CANVAS_W / 2 + (int32_t)tx;
}
static int32_t trt_sy(double ty) {
  return TURTLE_CANVAS_H / 2 - (int32_t)ty;
}

static void trt_put_pixel(int32_t x, int32_t y, uint32_t color) {
  if (x >= 0 && x < TURTLE_CANVAS_W && y >= 0 && y < TURTLE_CANVAS_H)
    trt_canvas[y * TURTLE_CANVAS_W + x] = color;
}

static void trt_clear_canvas(uint32_t color) {
  for (int i = 0; i < TURTLE_CANVAS_W * TURTLE_CANVAS_H; i++)
    trt_canvas[i] = color;
}

static void trt_draw_text(int32_t x, int32_t y, const char *text,
                          uint32_t color) {
  while (*text) {
    const uint8_t *glyph = font_get_glyph(*text++);
    for (int32_t row = 0; row < FONT_HEIGHT; row++)
      for (int32_t col = 0; col < FONT_WIDTH; col++)
        if (glyph[row] & (0x80 >> col))
          trt_put_pixel(x + col, y + row, color);
    x += FONT_WIDTH;
  }
}

/* The cursor is an overlay, not part of the drawing canvas.  Otherwise each
 * move leaves its old triangle behind and looks like a second turtle. */
static void trt_draw_screen_line(int32_t x0, int32_t y0, int32_t x1,
                                 int32_t y1, uint32_t col) {
  int32_t dx = x1 - x0, dy = y1 - y0;
  int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  int32_t sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
  int32_t err = adx - ady;
  while (1) {
    fb_put_pixel(x0, y0, col);
    if (x0 == x1 && y0 == y1) break;
    int32_t e2 = 2 * err;
    if (e2 > -ady) { err -= ady; x0 += sx; }
    if (e2 < adx)  { err += adx; y0 += sy; }
  }
}

static void trt_draw_cursor_on_screen(int32_t origin_x, int32_t origin_y);

void python_turtle_render(void) {
  if (!trt_active || !trt_window_init_done || !trt_window.visible)
    return;

  gui_window_render(&trt_window);
  int32_t dst_x = trt_window.bounds.x + 10;
  int32_t dst_y = trt_window.bounds.y + TITLEBAR_HEIGHT + 6;
  uint32_t *dst = fb_get_backbuffer();
  int32_t screen_w = (int32_t)fb_get_width();
  int32_t screen_h = (int32_t)fb_get_height();

  for (int32_t sy = 0; sy < TURTLE_CANVAS_H; sy++) {
    int32_t py = dst_y + sy;
    if (py < 0 || py >= screen_h) continue;
    for (int32_t sx = 0; sx < TURTLE_CANVAS_W; sx++) {
      int32_t px = dst_x + sx;
      if (px >= 0 && px < screen_w)
        dst[py * screen_w + px] = trt_canvas[sy * TURTLE_CANVAS_W + sx];
    }
  }
  trt_draw_cursor_on_screen(dst_x, dst_y);
}

int python_turtle_pointer_down(int32_t x, int32_t y) {
  if (!trt_active || !trt_window.visible)
    return 0;
  if (gui_window_hit_close(&trt_window, x, y)) {
    trt_window.visible = false;
    trt_active = false;
    keyboard_cancel_getchar();
    desktop_request_redraw();
    return 1;
  }
  if (gui_window_begin_drag(&trt_window, x, y))
    return 1;
  return gui_point_in_rect(x, y, &trt_window.bounds);
}

void python_turtle_pointer_move(int32_t x, int32_t y) {
  if (!trt_active || !trt_window.dragging)
    return;
  gui_window_update_drag(&trt_window, x, y, (int32_t)fb_get_width(),
                         (int32_t)fb_get_height() - TASKBAR_HEIGHT);
  desktop_request_redraw();
}

void python_turtle_pointer_up(void) {
  if (trt_window_init_done)
    gui_window_end_drag(&trt_window);
}

/* Bresenham line on the framebuffer */
static void trt_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                          uint32_t col) {
  int32_t dx = x1 - x0, dy = y1 - y0;
  int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  int32_t sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
  int32_t err = adx - ady;
  while (1) {
    trt_put_pixel(x0, y0, col);
    if (x0 == x1 && y0 == y1)
      break;
    int32_t e2 = 2 * err;
    if (e2 > -ady) {
      err -= ady;
      x0 += sx;
    }
    if (e2 < adx) {
      err += adx;
      y0 += sy;
    }
  }
}

/* Draw turtle cursor as a small filled triangle over the composed canvas. */
static void trt_draw_cursor_on_screen(int32_t origin_x, int32_t origin_y) {
  if (!trt_visible)
    return;
  int32_t cx = origin_x + trt_sx(trt_x), cy = origin_y + trt_sy(trt_y);
  double hr = trt_heading * (MY_PI / 180.0);
  double ax = cx + 12.0 * my_cos(hr), ay = cy - 12.0 * my_sin(hr);
  double bx = cx + 6.0 * my_cos(hr + 2.4), by = cy - 6.0 * my_sin(hr + 2.4);
  double dx2 = cx + 6.0 * my_cos(hr - 2.4), dy2 = cy - 6.0 * my_sin(hr - 2.4);
  trt_draw_screen_line((int32_t)ax, (int32_t)ay, (int32_t)bx, (int32_t)by,
                       0xFF00FF00);
  trt_draw_screen_line((int32_t)bx, (int32_t)by, (int32_t)dx2, (int32_t)dy2,
                       0xFF00FF00);
  trt_draw_screen_line((int32_t)dx2, (int32_t)dy2, (int32_t)ax, (int32_t)ay,
                       0xFF00FF00);
}

static void trt_ensure_init(void) {
  if (trt_init_done) {
    trt_active = true;
    trt_window.visible = true;
    trt_request_present();
    return;
  }
  trt_x = 0;
  trt_y = 0;
  trt_heading = 90.0;
  trt_pen_down = true;
  trt_pen_color = 0xFFFFFFFF;
  trt_bg_color = 0xFF000000;
  trt_visible = true;
  trt_pen_width = 1;
  trt_init_done = true;
  trt_active = true;
  if (!trt_window_init_done) {
    int32_t x = ((int32_t)fb_get_width() - (TURTLE_CANVAS_W + 20)) / 2;
    int32_t y = ((int32_t)fb_get_height() - (TURTLE_CANVAS_H + TITLEBAR_HEIGHT + 12)) / 2;
    if (x < 8) x = 8;
    if (y < 8) y = 8;
    gui_window_init(&trt_window, x, y, TURTLE_CANVAS_W + 20,
                    TURTLE_CANVAS_H + TITLEBAR_HEIGHT + 12,
                    trt_window_title);
    trt_window_init_done = true;
  }
  trt_window.visible = true;
  trt_clear_canvas(trt_bg_color);
  trt_request_present();
}

/* A `python file.py` invocation starts a fresh graphical program.  Do not
 * leak the old canvas or its window into the next script. */
static void trt_reset_for_next_program(void) {
  trt_init_done = false;
  trt_active = false;
  trt_visible = true;
  strcpy(trt_window_title, "Python Turtle");
  if (trt_window_init_done)
    trt_window.visible = false;
  trt_clear_canvas(0xFF000000);
  desktop_request_redraw();
}

/* Move forward and draw a line if pen is down */
static void trt_move(double dist) {
  double rad = trt_heading * (MY_PI / 180.0);
  double nx = trt_x + dist * my_cos(rad);
  double ny = trt_y + dist * my_sin(rad);
  if (trt_pen_down) {
    int32_t x0 = trt_sx(trt_x), y0 = trt_sy(trt_y);
    int32_t x1 = trt_sx(nx), y1 = trt_sy(ny);
    for (int w = 0; w < trt_pen_width; w++) {
      trt_draw_line(x0 + w, y0, x1 + w, y1, trt_pen_color);
      trt_draw_line(x0, y0 + w, x1, y1 + w, trt_pen_color);
    }
  }
  trt_x = nx;
  trt_y = ny;
  /* The cursor is composited at its new position by the window renderer. */
  trt_request_present();
}

/* ================================================================
 *  turtle module
 * ================================================================ */
static PyVal turtle_call(const char *fn, PyVal *a, int n) {
  trt_ensure_init();
  (void)n;

  /* movement */
  if (strcmp(fn, "forward") == 0 || strcmp(fn, "fd") == 0) {
    if (n >= 1)
      trt_move(val_num(a[0]));
    return mknone();
  }
  if (strcmp(fn, "backward") == 0 || strcmp(fn, "bk") == 0 ||
      strcmp(fn, "back") == 0) {
    if (n >= 1)
      trt_move(-val_num(a[0]));
    return mknone();
  }
  if (strcmp(fn, "right") == 0 || strcmp(fn, "rt") == 0) {
    if (n >= 1)
      trt_heading -= val_num(a[0]);
    return mknone();
  }
  if (strcmp(fn, "left") == 0 || strcmp(fn, "lt") == 0) {
    if (n >= 1)
      trt_heading += val_num(a[0]);
    return mknone();
  }
  if (strcmp(fn, "goto") == 0 || strcmp(fn, "setpos") == 0 ||
      strcmp(fn, "setposition") == 0) {
    if (n >= 2) {
      int32_t ox = trt_sx(trt_x), oy = trt_sy(trt_y);
      trt_x = val_num(a[0]);
      trt_y = val_num(a[1]);
      if (trt_pen_down) {
        trt_draw_line(ox, oy, trt_sx(trt_x), trt_sy(trt_y), trt_pen_color);
      }
      trt_request_present();
    }
    return mknone();
  }
  if (strcmp(fn, "home") == 0) {
    trt_x = 0;
    trt_y = 0;
    trt_heading = 90.0;
    return mknone();
  }
  if (strcmp(fn, "setx") == 0) {
    if (n >= 1)
      trt_x = val_num(a[0]);
    return mknone();
  }
  if (strcmp(fn, "sety") == 0) {
    if (n >= 1)
      trt_y = val_num(a[0]);
    return mknone();
  }

  /* pen */
  if (strcmp(fn, "penup") == 0 || strcmp(fn, "pu") == 0 ||
      strcmp(fn, "up") == 0) {
    trt_pen_down = false;
    return mknone();
  }
  if (strcmp(fn, "pendown") == 0 || strcmp(fn, "pd") == 0 ||
      strcmp(fn, "down") == 0) {
    trt_pen_down = true;
    return mknone();
  }
  if (strcmp(fn, "isdown") == 0)
    return mkbool(trt_pen_down);
  if (strcmp(fn, "pencolor") == 0 || strcmp(fn, "color") == 0) {
    if (n == 1 && a[0].t == PY_STR)
      trt_pen_color = color_from_name(a[0].s);
    else if (n == 3)
      trt_pen_color = color_from_rgb((int)val_num(a[0]), (int)val_num(a[1]),
                                     (int)val_num(a[2]));
    return mknone();
  }
  if (strcmp(fn, "fillcolor") == 0) {
    return mknone(); /* stub */
  }
  if (strcmp(fn, "pensize") == 0 || strcmp(fn, "width") == 0) {
    if (n >= 1) {
      trt_pen_width = (int)val_num(a[0]);
      if (trt_pen_width < 1)
        trt_pen_width = 1;
      if (trt_pen_width > 8)
        trt_pen_width = 8;
    }
    return mknone();
  }
  if (strcmp(fn, "bgcolor") == 0) {
    if (n == 1 && a[0].t == PY_STR)
      trt_bg_color = color_from_name(a[0].s);
    else if (n == 3)
      trt_bg_color = color_from_rgb((int)val_num(a[0]), (int)val_num(a[1]),
                                    (int)val_num(a[2]));
    trt_clear_canvas(trt_bg_color);
    trt_request_present();
    return mknone();
  }

  /* visibility */
  if (strcmp(fn, "hideturtle") == 0 || strcmp(fn, "ht") == 0) {
    trt_visible = false;
    return mknone();
  }
  if (strcmp(fn, "showturtle") == 0 || strcmp(fn, "st") == 0) {
    trt_visible = true;
    return mknone();
  }
  if (strcmp(fn, "isvisible") == 0)
    return mkbool(trt_visible);

  /* queries */
  if (strcmp(fn, "xcor") == 0 || strcmp(fn, "pos_x") == 0)
    return mkfloat(trt_x);
  if (strcmp(fn, "ycor") == 0 || strcmp(fn, "pos_y") == 0)
    return mkfloat(trt_y);
  if (strcmp(fn, "heading") == 0) {
    double h = trt_heading;
    while (h < 0)
      h += 360.0;
    while (h >= 360)
      h -= 360.0;
    return mkfloat(h);
  }
  if (strcmp(fn, "distance") == 0) {
    if (n >= 2) {
      double dx = val_num(a[0]) - trt_x, dy = val_num(a[1]) - trt_y;
      return mkfloat(my_sqrt(dx * dx + dy * dy));
    }
    return mkfloat(0.0);
  }

  /* setheading / seth */
  if (strcmp(fn, "setheading") == 0 || strcmp(fn, "seth") == 0) {
    if (n >= 1)
      trt_heading = val_num(a[0]);
    return mknone();
  }

  /* circle */
  if (strcmp(fn, "circle") == 0) {
    double r = n >= 1 ? val_num(a[0]) : 50.0;
    int steps = n >= 2 ? (int)val_num(a[1]) : 36;
    if (steps < 4)
      steps = 4;
    if (steps > 360)
      steps = 360;
    double ext = n >= 3 ? val_num(a[2]) : 360.0;
    double step_deg = ext / (double)steps;
    double step_len = (2.0 * MY_PI * my_fabs(r) / 360.0) * step_deg;
    double turn_deg = step_deg * (r < 0.0 ? -1.0 : 1.0);
    for (int i = 0; i < steps; i++) {
      trt_move(step_len);
      trt_heading += (r < 0.0 ? -turn_deg : turn_deg);
    }
    return mknone();
  }

  /* dot */
  if (strcmp(fn, "dot") == 0) {
    int sz = n >= 1 ? (int)val_num(a[0]) : 5;
    uint32_t col =
        n >= 2 && a[1].t == PY_STR ? color_from_name(a[1].s) : trt_pen_color;
    {
      int32_t cx = trt_sx(trt_x), cy = trt_sy(trt_y);
      int r = sz / 2;
      for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
          if (dx * dx + dy * dy <= r * r)
          trt_put_pixel(cx + dx, cy + dy, col);
      trt_request_present();
    }
    return mknone();
  }

  /* write */
  if (strcmp(fn, "write") == 0) {
    if (n >= 1) {
      char buf[PY_SMAX];
      val_to_buf(a[0], buf, PY_SMAX);
      trt_draw_text(trt_sx(trt_x), trt_sy(trt_y), buf, trt_pen_color);
      trt_request_present();
    }
    return mknone();
  }

  /* clear / reset */
  if (strcmp(fn, "clear") == 0) {
    trt_clear_canvas(trt_bg_color);
    trt_request_present();
    return mknone();
  }
  if (strcmp(fn, "reset") == 0) {
    trt_init_done = false;
    trt_ensure_init();
    return mknone();
  }

  /* screen / window helpers (stubs) */
  if (strcmp(fn, "speed") == 0)
    return mknone();
  if (strcmp(fn, "title") == 0) {
    if (n >= 1) {
      char buf[PY_SMAX];
      val_to_buf(a[0], buf, PY_SMAX);
      strcpy(trt_window_title, buf);
      trt_request_present();
    }
    return mknone();
  }
  if (strcmp(fn, "screensize") == 0 || strcmp(fn, "setup") == 0)
    return mknone();
  if (strcmp(fn, "tracer") == 0)
    return mknone();
  if (strcmp(fn, "update") == 0) {
    trt_request_present();
    return mknone();
  }

  /* done / mainloop / bye / exit: wait for any keypress and end turtle mode */
  if (strcmp(fn, "done") == 0 || strcmp(fn, "mainloop") == 0) {
    py_puts("[turtle] Press any key to continue...\n");
    keyboard_getchar();
    trt_active = false;
    desktop_request_redraw();
    return mknone();
  }
  if (strcmp(fn, "bye") == 0 || strcmp(fn, "exit") == 0) {
    trt_active = false;
    desktop_request_redraw();
    return mknone();
  }
  /* stamp: draw a small square */
  if (strcmp(fn, "stamp") == 0) {
    {
      int32_t cx = trt_sx(trt_x), cy = trt_sy(trt_y);
      for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++)
          trt_put_pixel(cx + dx, cy + dy, trt_pen_color);
      trt_request_present();
    }
    return mknone();
  }

  py_error("turtle: unknown function");
  return mkerr();
}

static PyVal turtle_getattr(const char *attr) {
  const char *fns[] = {
      "forward",    "fd",          "backward",  "bk",         "back",
      "right",      "rt",          "left",      "lt",         "goto",
      "setpos",     "setposition", "home",      "setx",       "sety",
      "penup",      "pu",          "up",        "pendown",    "pd",
      "down",       "isdown",      "pencolor",  "color",      "fillcolor",
      "pensize",    "width",       "bgcolor",   "hideturtle", "ht",
      "showturtle", "st",          "isvisible", "xcor",       "ycor",
      "heading",    "setheading",  "seth",      "distance",   "circle",
      "dot",        "write",       "clear",     "reset",      "speed",
      "title",      "screensize",  "setup",     "tracer",     "update",
      "done",       "mainloop",    "stamp",     NULL};
  for (int i = 0; fns[i]; i++)
    if (strcmp(attr, fns[i]) == 0)
      return mkbuiltin(MOD_TURTLE, attr);
  py_error("turtle: unknown attribute");
  return mkerr();
}

/* ================================================================
 *  math module
 * ================================================================ */
static PyVal math_call(const char *fn, PyVal *a, int n) {
  double x = n > 0 ? val_num(a[0]) : 0.0, y = n > 1 ? val_num(a[1]) : 0.0;
  if (strcmp(fn, "sqrt") == 0)
    return mkfloat(my_sqrt(x));
  if (strcmp(fn, "floor") == 0)
    return mkint((int32_t)my_floor(x));
  if (strcmp(fn, "ceil") == 0)
    return mkint((int32_t)my_ceil(x));
  if (strcmp(fn, "trunc") == 0)
    return mkint((int32_t)my_trunc(x));
  if (strcmp(fn, "fabs") == 0)
    return mkfloat(my_fabs(x));
  if (strcmp(fn, "exp") == 0)
    return mkfloat(my_exp(x));
  if (strcmp(fn, "log") == 0)
    return (n == 2) ? mkfloat(my_log(x) / my_log(y)) : mkfloat(my_log(x));
  if (strcmp(fn, "log2") == 0)
    return mkfloat(my_log2(x));
  if (strcmp(fn, "log10") == 0)
    return mkfloat(my_log10(x));
  if (strcmp(fn, "pow") == 0)
    return mkfloat(my_pow(x, y));
  if (strcmp(fn, "sin") == 0)
    return mkfloat(my_sin(x));
  if (strcmp(fn, "cos") == 0)
    return mkfloat(my_cos(x));
  if (strcmp(fn, "tan") == 0)
    return mkfloat(my_tan(x));
  if (strcmp(fn, "asin") == 0)
    return mkfloat(my_asin(x));
  if (strcmp(fn, "acos") == 0)
    return mkfloat(my_acos(x));
  if (strcmp(fn, "atan") == 0)
    return mkfloat(my_atan(x));
  if (strcmp(fn, "atan2") == 0)
    return mkfloat(my_atan2(x, y));
  if (strcmp(fn, "hypot") == 0)
    return mkfloat(my_hypot(x, y));
  if (strcmp(fn, "degrees") == 0)
    return mkfloat(x * (180.0 / MY_PI));
  if (strcmp(fn, "radians") == 0)
    return mkfloat(x * (MY_PI / 180.0));
  if (strcmp(fn, "copysign") == 0)
    return mkfloat(my_copysign(x, y));
  if (strcmp(fn, "factorial") == 0) {
    int64_t r = my_factorial((int64_t)x);
    if (r < 0) {
      py_error("factorial overflow/negative");
      return mkerr();
    }
    return mkint((int32_t)r);
  }
  if (strcmp(fn, "gcd") == 0)
    return mkint((int32_t)my_gcd((int64_t)x, (int64_t)y));
  if (strcmp(fn, "lcm") == 0)
    return mkint((int32_t)my_lcm((int64_t)x, (int64_t)y));
  if (strcmp(fn, "isqrt") == 0)
    return mkint((int32_t)my_sqrt(x));
  if (strcmp(fn, "isinf") == 0)
    return mkbool(x > 1e300 || x < -1e300);
  if (strcmp(fn, "isnan") == 0)
    return mkbool(0);
  if (strcmp(fn, "isfinite") == 0)
    return mkbool(!(x > 1e300 || x < -1e300));
  if (strcmp(fn, "comb") == 0) {
    int64_t nn = (int64_t)x, kk = (int64_t)y;
    if (kk < 0 || kk > nn)
      return mkint(0);
    if (kk == 0 || kk == nn)
      return mkint(1);
    if (kk > nn - kk)
      kk = nn - kk;
    int64_t r = 1;
    for (int64_t i = 0; i < kk; i++) {
      r = r * (nn - i) / (i + 1);
    }
    return mkint((int32_t)r);
  }
  if (strcmp(fn, "perm") == 0) {
    int64_t nn = (int64_t)x, kk = (n >= 2) ? (int64_t)y : nn;
    int64_t r = 1;
    for (int64_t i = nn - kk + 1; i <= nn; i++)
      r *= i;
    return mkint((int32_t)r);
  }
  if (strcmp(fn, "dist") == 0) { /* 2D */
    if (n >= 2) {
      double dx2 = x - y, dy2 = val_num(a[2]) - val_num(a[3]);
      return mkfloat(my_sqrt(dx2 * dx2 + dy2 * dy2));
    }
    return mkfloat(0.0);
  }
  if (strcmp(fn, "fsum") == 0)
    return mkfloat(x);
  if (strcmp(fn, "prod") == 0)
    return mkfloat(x);
  if (strcmp(fn, "remainder") == 0) {
    if (y == 0.0) {
      py_error("remainder by zero");
      return mkerr();
    }
    double q = my_trunc(x / y);
    return mkfloat(x - q * y);
  }
  if (strcmp(fn, "modf") == 0) {
    double ip = my_trunc(x);
    return mkfloat(x - ip); /* returns frac; full tuple not supported */
  }
  if (strcmp(fn, "frexp") == 0)
    return mkfloat(x);
  if (strcmp(fn, "ldexp") == 0)
    return mkfloat(x * my_pow(2.0, y));
  if (strcmp(fn, "sinh") == 0)
    return mkfloat((my_exp(x) - my_exp(-x)) * 0.5);
  if (strcmp(fn, "cosh") == 0)
    return mkfloat((my_exp(x) + my_exp(-x)) * 0.5);
  if (strcmp(fn, "tanh") == 0) {
    double ep = my_exp(x), em = my_exp(-x);
    return mkfloat((ep - em) / (ep + em));
  }
  if (strcmp(fn, "asinh") == 0)
    return mkfloat(my_log(x + my_sqrt(x * x + 1.0)));
  if (strcmp(fn, "acosh") == 0)
    return mkfloat(my_log(x + my_sqrt(x * x - 1.0)));
  if (strcmp(fn, "atanh") == 0)
    return mkfloat(0.5 * my_log((1.0 + x) / (1.0 - x)));
  if (strcmp(fn, "erf") == 0) { /* Horner approx */
    double t = 1.0 / (1.0 + 0.3275911 * my_fabs(x));
    double y2 =
        1.0 -
        t *
            (0.254829592 +
             t * (-0.284496736 +
                  t * (1.421413741 + t * (-1.453152027 + t * 1.061405429)))) *
            my_exp(-x * x);
    return mkfloat(x < 0 ? -y2 : y2);
  }
  if (strcmp(fn, "erfc") == 0) {
    double t = 1.0 / (1.0 + 0.3275911 * my_fabs(x));
    double y2 =
        t *
        (0.254829592 +
         t * (-0.284496736 +
              t * (1.421413741 + t * (-1.453152027 + t * 1.061405429)))) *
        my_exp(-x * x);
    return mkfloat(x < 0 ? 2.0 - y2 : y2);
  }
  if (strcmp(fn, "gamma") == 0) { /* Stirling approx */
    if (x <= 0.5)
      return mkfloat(MY_PI /
                     (my_sin(MY_PI * x) *
                      my_exp(my_log(1.0 - x) + 0.5 * my_log(2.0 * MY_PI) -
                             (1.0 - x) * my_log(1.0 / my_exp(1.0)))));
    double z = x - 1.0;
    return mkfloat(my_sqrt(2.0 * MY_PI / z) * my_pow(z / MY_E, z) *
                   (1.0 + 1.0 / (12.0 * z)));
  }
  if (strcmp(fn, "lgamma") == 0)
    return mkfloat(my_log(my_fabs(val_num(math_call("gamma", &a[0], 1)))));
  py_error("math: unknown function");
  return mkerr();
}
static PyVal math_getattr(const char *attr) {
  if (strcmp(attr, "pi") == 0)
    return mkfloat(MY_PI);
  if (strcmp(attr, "e") == 0)
    return mkfloat(MY_E);
  if (strcmp(attr, "tau") == 0)
    return mkfloat(MY_TAU);
  if (strcmp(attr, "inf") == 0)
    return mkfloat(1e308 * 10.0);
  if (strcmp(attr, "nan") == 0)
    return mkfloat(0.0);
  if (strcmp(attr, "sqrt") == 0 || strcmp(attr, "floor") == 0 ||
      strcmp(attr, "ceil") == 0 || strcmp(attr, "trunc") == 0 ||
      strcmp(attr, "fabs") == 0 || strcmp(attr, "exp") == 0 ||
      strcmp(attr, "log") == 0 || strcmp(attr, "log2") == 0 ||
      strcmp(attr, "log10") == 0 || strcmp(attr, "pow") == 0 ||
      strcmp(attr, "sin") == 0 || strcmp(attr, "cos") == 0 ||
      strcmp(attr, "tan") == 0 || strcmp(attr, "asin") == 0 ||
      strcmp(attr, "acos") == 0 || strcmp(attr, "atan") == 0 ||
      strcmp(attr, "atan2") == 0 || strcmp(attr, "hypot") == 0 ||
      strcmp(attr, "degrees") == 0 || strcmp(attr, "radians") == 0 ||
      strcmp(attr, "factorial") == 0 || strcmp(attr, "gcd") == 0 ||
      strcmp(attr, "lcm") == 0 || strcmp(attr, "isqrt") == 0 ||
      strcmp(attr, "copysign") == 0 || strcmp(attr, "isinf") == 0 ||
      strcmp(attr, "isnan") == 0 || strcmp(attr, "isfinite") == 0 ||
      strcmp(attr, "comb") == 0 || strcmp(attr, "perm") == 0 ||
      strcmp(attr, "dist") == 0 || strcmp(attr, "remainder") == 0 ||
      strcmp(attr, "sinh") == 0 || strcmp(attr, "cosh") == 0 ||
      strcmp(attr, "tanh") == 0 || strcmp(attr, "asinh") == 0 ||
      strcmp(attr, "acosh") == 0 || strcmp(attr, "atanh") == 0 ||
      strcmp(attr, "erf") == 0 || strcmp(attr, "erfc") == 0 ||
      strcmp(attr, "gamma") == 0 || strcmp(attr, "lgamma") == 0 ||
      strcmp(attr, "modf") == 0 || strcmp(attr, "fsum") == 0)
    return mkbuiltin(MOD_MATH, attr);
  py_error("math: unknown attribute");
  return mkerr();
}

/* ================================================================
 *  random module
 * ================================================================ */
static PyVal random_call(const char *fn, PyVal *a, int n) {
  if (strcmp(fn, "seed") == 0) {
    if (n > 0)
      rng_st = (uint64_t)(int64_t)val_num(a[0]) ^ 0xDEADBEEFULL;
    return mknone();
  }
  if (strcmp(fn, "random") == 0)
    return mkfloat(rng_f());
  if (strcmp(fn, "randint") == 0) {
    if (n < 2) {
      py_error("randint needs 2 args");
      return mkerr();
    }
    int32_t lo = a[0].i, hi = a[1].i;
    if (lo > hi) {
      py_error("randint: lo>hi");
      return mkerr();
    }
    return mkint(lo + (int32_t)(rng_next() % (uint64_t)(hi - lo + 1)));
  }
  if (strcmp(fn, "randrange") == 0) {
    int32_t start = 0, stop, step = 1;
    if (n == 1) {
      stop = a[0].i;
    } else {
      start = a[0].i;
      stop = a[1].i;
    }
    if (n >= 3)
      step = a[2].i;
    if (step == 0) {
      py_error("randrange: step=0");
      return mkerr();
    }
    int32_t cnt = (stop - start + step - 1) / step;
    return mkint(start + step * (int32_t)(rng_next() % (uint64_t)cnt));
  }
  if (strcmp(fn, "uniform") == 0) {
    if (n < 2) {
      py_error("uniform needs 2 args");
      return mkerr();
    }
    double lo = val_num(a[0]), hi = val_num(a[1]);
    return mkfloat(lo + rng_f() * (hi - lo));
  }
  if (strcmp(fn, "choice") == 0) {
    if (n < 1 || a[0].t != PY_STR) {
      py_error("choice: need a string");
      return mkerr();
    }
    int len = (int)strlen(a[0].s);
    if (len == 0) {
      py_error("choice: empty");
      return mkerr();
    }
    char s[2] = {a[0].s[(int)(rng_next() % (uint64_t)len)], 0};
    return mkstr(s);
  }
  if (strcmp(fn, "shuffle") == 0)
    return mknone(); /* no list support */
  if (strcmp(fn, "sample") == 0)
    return mknone();
  if (strcmp(fn, "gauss") == 0 || strcmp(fn, "normalvariate") == 0) {
    /* Box-Muller */
    double u1 = rng_f() + 1e-15, u2 = rng_f();
    double z = my_sqrt(-2.0 * my_log(u1)) * my_cos(2.0 * MY_PI * u2);
    double mu = n > 0 ? val_num(a[0]) : 0.0,
           sigma = n > 1 ? val_num(a[1]) : 1.0;
    return mkfloat(mu + sigma * z);
  }
  if (strcmp(fn, "triangular") == 0) {
    double lo2 = n > 0 ? val_num(a[0]) : 0.0, hi2 = n > 1 ? val_num(a[1]) : 1.0,
           m = n > 2 ? val_num(a[2]) : (lo2 + hi2) * 0.5;
    double u = rng_f(), fc = (m - lo2) / (hi2 - lo2);
    return mkfloat(u < fc ? lo2 + my_sqrt(u * (hi2 - lo2) * (m - lo2))
                          : hi2 - my_sqrt((1.0 - u) * (hi2 - lo2) * (hi2 - m)));
  }
  if (strcmp(fn, "expovariate") == 0) {
    double lam = n > 0 ? val_num(a[0]) : 1.0;
    return mkfloat(-my_log(rng_f() + 1e-15) / lam);
  }
  if (strcmp(fn, "getrandbits") == 0) {
    if (n < 1)
      return mkint(0);
    int bits = (int)val_num(a[0]);
    if (bits < 1)
      bits = 1;
    if (bits > 31)
      bits = 31;
    return mkint((int32_t)(rng_next() >> ((uint64_t)(64 - bits))));
  }
  py_error("random: unknown function");
  return mkerr();
}
static PyVal random_getattr(const char *attr) {
  if (strcmp(attr, "random") == 0 || strcmp(attr, "randint") == 0 ||
      strcmp(attr, "randrange") == 0 || strcmp(attr, "uniform") == 0 ||
      strcmp(attr, "seed") == 0 || strcmp(attr, "choice") == 0 ||
      strcmp(attr, "gauss") == 0 || strcmp(attr, "normalvariate") == 0 ||
      strcmp(attr, "triangular") == 0 || strcmp(attr, "expovariate") == 0 ||
      strcmp(attr, "getrandbits") == 0)
    return mkbuiltin(MOD_RANDOM, attr);
  py_error("random: unknown attribute");
  return mkerr();
}

/* ================================================================
 *  string module
 * ================================================================ */
#define S_DIGITS "0123456789"
#define S_LOWER "abcdefghijklmnopqrstuvwxyz"
#define S_UPPER "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define S_LETTERS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define S_PUNCT "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
#define S_WHITE " \t\n\r\x0b\x0c"
#define S_PRINTABLE                                                            \
  "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!\"#$%&'()*+" \
  ",-./:;<=>?@[\\]^_`{|}~ "
#define S_HEX "0123456789abcdefABCDEF"
#define S_OCT "01234567"

static PyVal string_call(const char *fn, PyVal *a, int n) {
  if (strcmp(fn, "capwords") == 0) {
    if (n < 1 || a[0].t != PY_STR)
      return mkstr("");
    char out[PY_SMAX];
    int oi = 0;
    bool cap = true;
    for (int i = 0; a[0].s[i] && oi < PY_SMAX - 1; i++) {
      char c = a[0].s[i];
      if (c == ' ' || c == '\t') {
        out[oi++] = c;
        cap = true;
      } else if (cap && c >= 'a' && c <= 'z') {
        out[oi++] = (char)(c - 32);
        cap = false;
      } else {
        out[oi++] = c;
        cap = false;
      }
    }
    out[oi] = 0;
    return mkstr(out);
  }
  py_error("string: unknown function");
  return mkerr();
}
static PyVal string_getattr(const char *attr) {
  if (strcmp(attr, "digits") == 0)
    return mkstr(S_DIGITS);
  if (strcmp(attr, "ascii_lowercase") == 0)
    return mkstr(S_LOWER);
  if (strcmp(attr, "ascii_uppercase") == 0)
    return mkstr(S_UPPER);
  if (strcmp(attr, "ascii_letters") == 0)
    return mkstr(S_LETTERS);
  if (strcmp(attr, "punctuation") == 0)
    return mkstr(S_PUNCT);
  if (strcmp(attr, "whitespace") == 0)
    return mkstr(S_WHITE);
  if (strcmp(attr, "printable") == 0)
    return mkstr(S_PRINTABLE);
  if (strcmp(attr, "hexdigits") == 0)
    return mkstr(S_HEX);
  if (strcmp(attr, "octdigits") == 0)
    return mkstr(S_OCT);
  if (strcmp(attr, "capwords") == 0)
    return mkbuiltin(MOD_STRING, attr);
  py_error("string: unknown attribute");
  return mkerr();
}

/* ================================================================
 *  sys module
 * ================================================================ */
static bool sys_exit_called = false;
static PyVal sys_call(const char *fn, PyVal *a, int n) {
  (void)a;
  (void)n;
  if (strcmp(fn, "exit") == 0) {
    sys_exit_called = true;
    return mknone();
  }
  py_error("sys: unknown function");
  return mkerr();
}
static PyVal sys_getattr(const char *attr) {
  if (strcmp(attr, "version") == 0)
    return mkstr("NovexPy 3.0 (NovexOS x86_64)");
  if (strcmp(attr, "platform") == 0)
    return mkstr("novexos");
  if (strcmp(attr, "maxsize") == 0)
    return mkint(2147483647);
  if (strcmp(attr, "float_info.max") == 0)
    return mkfloat(1.7976931348623157e+308);
  if (strcmp(attr, "float_info.min") == 0)
    return mkfloat(2.2250738585072014e-308);
  if (strcmp(attr, "float_info.epsilon") == 0)
    return mkfloat(2.220446049250313e-16);
  if (strcmp(attr, "exit") == 0)
    return mkbuiltin(MOD_SYS, "exit");
  py_error("sys: unknown attribute");
  return mkerr();
}

/* ================================================================
 *  os module
 * ================================================================ */
static char os_ls_buf[512];
static int os_ls_idx;
static void os_ls_cb(const char *n, uint32_t sz, bool is_dir) {
  (void)sz;
  (void)is_dir;
  if (os_ls_idx > 0 && os_ls_idx < 508) {
    os_ls_buf[os_ls_idx++] = ',';
    os_ls_buf[os_ls_idx++] = ' ';
  }
  for (int j = 0; n[j] && os_ls_idx < 509; j++)
    os_ls_buf[os_ls_idx++] = n[j];
  os_ls_buf[os_ls_idx] = 0;
}
static PyVal os_call(const char *fn, PyVal *a, int n) {
  (void)a;
  (void)n;
  if (strcmp(fn, "getcwd") == 0)
    return mkstr("ram:/");
  if (strcmp(fn, "listdir") == 0) {
    os_ls_buf[0] = 0;
    os_ls_idx = 0;
    ramfs_list(os_ls_cb);
    return mkstr(os_ls_buf);
  }
  if (strcmp(fn, "getenv") == 0)
    return mknone();
  if (strcmp(fn, "system") == 0)
    return mkint(-1);
  if (strcmp(fn, "getpid") == 0)
    return mkint(1);
  if (strcmp(fn, "getlogin") == 0)
    return mkstr("omega");
  py_error("os: unknown function");
  return mkerr();
}
static PyVal os_getattr(const char *attr) {
  if (strcmp(attr, "getcwd") == 0 || strcmp(attr, "listdir") == 0 ||
      strcmp(attr, "getenv") == 0 || strcmp(attr, "getpid") == 0 ||
      strcmp(attr, "getlogin") == 0)
    return mkbuiltin(MOD_OS, attr);
  if (strcmp(attr, "sep") == 0)
    return mkstr("/");
  if (strcmp(attr, "linesep") == 0)
    return mkstr("\n");
  if (strcmp(attr, "curdir") == 0)
    return mkstr(".");
  if (strcmp(attr, "pardir") == 0)
    return mkstr("..");
  if (strcmp(attr, "name") == 0)
    return mkstr("novexos");
  if (strcmp(attr, "devnull") == 0)
    return mkstr("/dev/null");
  py_error("os: unknown attribute");
  return mkerr();
}

/* ================================================================
 *  time module
 * ================================================================ */
static PyVal time_call(const char *fn, PyVal *a, int n) {
  if (strcmp(fn, "time") == 0 || strcmp(fn, "monotonic") == 0 ||
      strcmp(fn, "perf_counter") == 0)
    return mkfloat((double)timer_get_uptime_seconds());
  if (strcmp(fn, "sleep") == 0) {
    if (n > 0) {
      uint32_t ms = (uint32_t)(val_num(a[0]) * 1000.0);
      timer_sleep(ms);
    }
    return mknone();
  }
  if (strcmp(fn, "time_ns") == 0)
    return mkint((int32_t)(timer_get_uptime_seconds() * 1000000000LL));
  if (strcmp(fn, "clock") == 0)
    return mkfloat((double)timer_get_ticks() / 1000.0);
  py_error("time: unknown function");
  return mkerr();
}
static PyVal time_getattr(const char *attr) {
  if (strcmp(attr, "time") == 0 || strcmp(attr, "sleep") == 0 ||
      strcmp(attr, "monotonic") == 0 || strcmp(attr, "perf_counter") == 0 ||
      strcmp(attr, "time_ns") == 0 || strcmp(attr, "clock") == 0)
    return mkbuiltin(MOD_TIME, attr);
  py_error("time: unknown attribute");
  return mkerr();
}

/* ================================================================
 *  Module dispatch
 * ================================================================ */

static PyVal mod_call(uint8_t mid, const char *fn, PyVal *a, int n) {
  switch (mid) {
  case MOD_MATH:
    return math_call(fn, a, n);
  case MOD_RANDOM:
    return random_call(fn, a, n);
  case MOD_STRING:
    return string_call(fn, a, n);
  case MOD_SYS:
    return sys_call(fn, a, n);
  case MOD_OS:
    return os_call(fn, a, n);
  case MOD_TIME:
    return time_call(fn, a, n);
  case MOD_TURTLE:
    return turtle_call(fn, a, n);
  default:
    py_error("unknown module");
    return mkerr();
  }
}
static PyVal mod_getattr(uint8_t mid, const char *attr) {
  switch (mid) {
  case MOD_MATH:
    return math_getattr(attr);
  case MOD_RANDOM:
    return random_getattr(attr);
  case MOD_STRING:
    return string_getattr(attr);
  case MOD_SYS:
    return sys_getattr(attr);
  case MOD_OS:
    return os_getattr(attr);
  case MOD_TIME:
    return time_getattr(attr);
  case MOD_TURTLE:
    return turtle_getattr(attr);
  default:
    py_error("unknown module");
    return mkerr();
  }
}
static uint8_t modname_to_id(const char *n) {
  if (strcmp(n, "math") == 0)
    return MOD_MATH;
  if (strcmp(n, "random") == 0)
    return MOD_RANDOM;
  if (strcmp(n, "string") == 0)
    return MOD_STRING;
  if (strcmp(n, "sys") == 0)
    return MOD_SYS;
  if (strcmp(n, "os") == 0)
    return MOD_OS;
  if (strcmp(n, "time") == 0)
    return MOD_TIME;
  if (strcmp(n, "turtle") == 0)
    return MOD_TURTLE;
  return 0;
}

/* ================================================================
 *  str.method()  — called as  var.method(args)
 * ================================================================ */
static PyVal str_method(PyVal sv, const char *fn, PyVal *a, int n) {
  const char *s = sv.s;
  int slen = (int)strlen(s);
  char out[PY_SMAX];
  int oi;

  /* upper / lower */
  if (strcmp(fn, "upper") == 0) {
    for (oi = 0; s[oi] && oi < PY_SMAX - 1; oi++)
      out[oi] = (s[oi] >= 'a' && s[oi] <= 'z') ? (char)(s[oi] - 32) : s[oi];
    out[oi] = 0;
    return mkstr(out);
  }
  if (strcmp(fn, "lower") == 0) {
    for (oi = 0; s[oi] && oi < PY_SMAX - 1; oi++)
      out[oi] = (s[oi] >= 'A' && s[oi] <= 'Z') ? (char)(s[oi] + 32) : s[oi];
    out[oi] = 0;
    return mkstr(out);
  }
  if (strcmp(fn, "capitalize") == 0) {
    for (oi = 0; s[oi] && oi < PY_SMAX - 1; oi++) {
      if (oi == 0)
        out[oi] = (s[oi] >= 'a' && s[oi] <= 'z') ? (char)(s[oi] - 32) : s[oi];
      else
        out[oi] = (s[oi] >= 'A' && s[oi] <= 'Z') ? (char)(s[oi] + 32) : s[oi];
    }
    out[oi] = 0;
    return mkstr(out);
  }
  if (strcmp(fn, "swapcase") == 0) {
    for (oi = 0; s[oi] && oi < PY_SMAX - 1; oi++) {
      if (s[oi] >= 'a' && s[oi] <= 'z')
        out[oi] = (char)(s[oi] - 32);
      else if (s[oi] >= 'A' && s[oi] <= 'Z')
        out[oi] = (char)(s[oi] + 32);
      else
        out[oi] = s[oi];
    }
    out[oi] = 0;
    return mkstr(out);
  }

  /* strip */
  if (strcmp(fn, "strip") == 0) {
    int l = 0, r = slen - 1;
    while (l <= r &&
           (s[l] == ' ' || s[l] == '\t' || s[l] == '\n' || s[l] == '\r'))
      l++;
    while (r >= l &&
           (s[r] == ' ' || s[r] == '\t' || s[r] == '\n' || s[r] == '\r'))
      r--;
    int len = r - l + 1;
    if (len < 0)
      len = 0;
    for (oi = 0; oi < len && oi < PY_SMAX - 1; oi++)
      out[oi] = s[l + oi];
    out[oi] = 0;
    return mkstr(out);
  }
  if (strcmp(fn, "lstrip") == 0) {
    int l = 0;
    while (l < slen && (s[l] == ' ' || s[l] == '\t'))
      l++;
    for (oi = 0; s[l + oi] && oi < PY_SMAX - 1; oi++)
      out[oi] = s[l + oi];
    out[oi] = 0;
    return mkstr(out);
  }
  if (strcmp(fn, "rstrip") == 0) {
    int r = slen - 1;
    while (r >= 0 && (s[r] == ' ' || s[r] == '\t' || s[r] == '\n'))
      r--;
    for (oi = 0; oi <= r && oi < PY_SMAX - 1; oi++)
      out[oi] = s[oi];
    out[oi] = 0;
    return mkstr(out);
  }

  /* find / index / rfind */
  if (strcmp(fn, "find") == 0 || strcmp(fn, "index") == 0) {
    if (n < 1 || a[0].t != PY_STR)
      return mkint(-1);
    const char *needle = a[0].s;
    int nl = (int)strlen(needle);
    for (int i = 0; i <= slen - nl; i++) {
      bool ok = true;
      for (int j = 0; j < nl; j++)
        if (s[i + j] != needle[j]) {
          ok = false;
          break;
        }
      if (ok)
        return mkint(i);
    }
    if (strcmp(fn, "index") == 0) {
      py_error("substring not found");
      return mkerr();
    }
    return mkint(-1);
  }
  if (strcmp(fn, "rfind") == 0) {
    if (n < 1 || a[0].t != PY_STR)
      return mkint(-1);
    const char *needle = a[0].s;
    int nl = (int)strlen(needle), last = -1;
    for (int i = 0; i <= slen - nl; i++) {
      bool ok = true;
      for (int j = 0; j < nl; j++)
        if (s[i + j] != needle[j]) {
          ok = false;
          break;
        }
      if (ok)
        last = i;
    }
    return mkint(last);
  }

  /* count */
  if (strcmp(fn, "count") == 0) {
    if (n < 1)
      return mkint(0);
    const char *needle = a[0].t == PY_STR ? a[0].s : NULL;
    if (!needle)
      return mkint(0);
    int nl = (int)strlen(needle), cnt = 0;
    for (int i = 0; i <= slen - nl; i++) {
      bool ok = true;
      for (int j = 0; j < nl; j++)
        if (s[i + j] != needle[j]) {
          ok = false;
          break;
        }
      if (ok) {
        cnt++;
        i += nl - 1;
      }
    }
    return mkint(cnt);
  }

  /* replace */
  if (strcmp(fn, "replace") == 0) {
    if (n < 2 || a[0].t != PY_STR || a[1].t != PY_STR)
      return sv;
    const char *old2 = a[0].s, *neu = a[1].s;
    int ol = (int)strlen(old2), nl2 = (int)strlen(neu);
    oi = 0;
    int maxr = (n >= 3) ? a[2].i : 9999, cnt = 0;
    for (int i = 0; s[i] && oi < PY_SMAX - 1;) {
      bool match = false;
      if (cnt < maxr && ol > 0) {
        match = true;
        for (int j = 0; j < ol; j++)
          if (s[i + j] != old2[j]) {
            match = false;
            break;
          }
      }
      if (match) {
        for (int j = 0; j < nl2 && oi < PY_SMAX - 1; j++)
          out[oi++] = neu[j];
        i += ol;
        cnt++;
      } else
        out[oi++] = s[i++];
    }
    out[oi] = 0;
    return mkstr(out);
  }

  /* startswith / endswith */
  if (strcmp(fn, "startswith") == 0) {
    if (n < 1 || a[0].t != PY_STR)
      return mkbool(0);
    const char *p = a[0].s;
    int pl = (int)strlen(p);
    if (pl > slen)
      return mkbool(0);
    for (int i = 0; i < pl; i++)
      if (s[i] != p[i])
        return mkbool(0);
    return mkbool(1);
  }
  if (strcmp(fn, "endswith") == 0) {
    if (n < 1 || a[0].t != PY_STR)
      return mkbool(0);
    const char *p = a[0].s;
    int pl = (int)strlen(p);
    if (pl > slen)
      return mkbool(0);
    for (int i = 0; i < pl; i++)
      if (s[slen - pl + i] != p[i])
        return mkbool(0);
    return mkbool(1);
  }

  /* predicates */
  if (strcmp(fn, "isdigit") == 0) {
    if (!s[0])
      return mkbool(0);
    for (int i = 0; s[i]; i++)
      if (s[i] < '0' || s[i] > '9')
        return mkbool(0);
    return mkbool(1);
  }
  if (strcmp(fn, "isalpha") == 0) {
    if (!s[0])
      return mkbool(0);
    for (int i = 0; s[i]; i++)
      if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z')))
        return mkbool(0);
    return mkbool(1);
  }
  if (strcmp(fn, "isalnum") == 0) {
    if (!s[0])
      return mkbool(0);
    for (int i = 0; s[i]; i++)
      if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
            (s[i] >= '0' && s[i] <= '9')))
        return mkbool(0);
    return mkbool(1);
  }
  if (strcmp(fn, "isspace") == 0) {
    if (!s[0])
      return mkbool(0);
    for (int i = 0; s[i]; i++)
      if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
        return mkbool(0);
    return mkbool(1);
  }
  if (strcmp(fn, "isupper") == 0) {
    bool has_upper = false;
    for (int i = 0; s[i]; i++) {
      if (s[i] >= 'A' && s[i] <= 'Z')
        has_upper = true;
      else if (s[i] >= 'a' && s[i] <= 'z')
        return mkbool(0);
    }
    return mkbool(has_upper);
  }
  if (strcmp(fn, "islower") == 0) {
    bool has_lower = false;
    for (int i = 0; s[i]; i++) {
      if (s[i] >= 'a' && s[i] <= 'z')
        has_lower = true;
      else if (s[i] >= 'A' && s[i] <= 'Z')
        return mkbool(0);
    }
    return mkbool(has_lower);
  }
  if (strcmp(fn, "isprintable") == 0) {
    for (int i = 0; s[i]; i++)
      if (s[i] < ' ' || s[i] > 126)
        return mkbool(0);
    return mkbool(1);
  }
  if (strcmp(fn, "isnumeric") == 0 || strcmp(fn, "isdecimal") == 0)
    return str_method(sv, "isdigit", a, n);
  if (strcmp(fn, "isidentifier") == 0) {
    if (!s[0])
      return mkbool(0);
    if (!(s[0] >= 'a' && s[0] <= 'z') && !(s[0] >= 'A' && s[0] <= 'Z') &&
        s[0] != '_')
      return mkbool(0);
    for (int i = 1; s[i]; i++)
      if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
            (s[i] >= '0' && s[i] <= '9') || s[i] == '_'))
        return mkbool(0);
    return mkbool(1);
  }
  if (strcmp(fn, "isascii") == 0) {
    for (int i = 0; s[i]; i++)
      if ((unsigned char)s[i] > 127)
        return mkbool(0);
    return mkbool(1);
  }

  /* len (as method) */
  if (strcmp(fn, "__len__") == 0)
    return mkint(slen);

  /* padding */
  if (strcmp(fn, "center") == 0) {
    int width = n > 0 ? (int)val_num(a[0]) : slen;
    char fill = n > 1 && a[1].t == PY_STR ? a[1].s[0] : ' ';
    int pad = width - slen;
    if (pad < 0)
      return sv;
    int lp2 = pad / 2, rp = pad - lp2;
    oi = 0;
    for (int i = 0; i < lp2 && oi < PY_SMAX - 1; i++)
      out[oi++] = fill;
    for (int i = 0; s[i] && oi < PY_SMAX - 1; i++)
      out[oi++] = s[i];
    for (int i = 0; i < rp && oi < PY_SMAX - 1; i++)
      out[oi++] = fill;
    out[oi] = 0;
    return mkstr(out);
  }
  if (strcmp(fn, "ljust") == 0) {
    int width = n > 0 ? (int)val_num(a[0]) : slen;
    char fill = n > 1 && a[1].t == PY_STR ? a[1].s[0] : ' ';
    oi = 0;
    for (int i = 0; s[i] && oi < PY_SMAX - 1; i++)
      out[oi++] = s[i];
    while (oi < width && oi < PY_SMAX - 1)
      out[oi++] = fill;
    out[oi] = 0;
    return mkstr(out);
  }
  if (strcmp(fn, "rjust") == 0) {
    int width = n > 0 ? (int)val_num(a[0]) : slen;
    char fill = n > 1 && a[1].t == PY_STR ? a[1].s[0] : ' ';
    int pad = width - slen;
    if (pad < 0)
      return sv;
    oi = 0;
    for (int i = 0; i < pad && oi < PY_SMAX - 1; i++)
      out[oi++] = fill;
    for (int i = 0; s[i] && oi < PY_SMAX - 1; i++)
      out[oi++] = s[i];
    out[oi] = 0;
    return mkstr(out);
  }
  if (strcmp(fn, "zfill") == 0) {
    int width = n > 0 ? (int)val_num(a[0]) : slen;
    int pad = width - slen;
    if (pad < 0)
      return sv;
    oi = 0;
    bool neg = (s[0] == '-');
    if (neg)
      out[oi++] = '-';
    for (int i = 0; i < pad && oi < PY_SMAX - 1; i++)
      out[oi++] = '0';
    for (int i = neg ? 1 : 0; s[i] && oi < PY_SMAX - 1; i++)
      out[oi++] = s[i];
    out[oi] = 0;
    return mkstr(out);
  }

  /* encode (stub) */
  if (strcmp(fn, "encode") == 0)
    return sv;

  /* split — returns comma-separated first tokens (limited) */
  if (strcmp(fn, "split") == 0) {
    char sep = ' ';
    if (n > 0 && a[0].t == PY_STR && a[0].s[0])
      sep = a[0].s[0];
    /* Return first token only (no list support) */
    oi = 0;
    for (int i = 0; s[i] && s[i] != sep && oi < PY_SMAX - 1; i++)
      out[oi++] = s[i];
    out[oi] = 0;
    return mkstr(out);
  }
  if (strcmp(fn, "join") == 0) {
    if (n < 1 || a[0].t != PY_STR)
      return mkstr(s);
    /* Join chars of the argument string with self as separator */
    oi = 0;
    for (int i = 0; a[0].s[i]; i++) {
      if (i > 0) {
        for (int j = 0; s[j] && oi < PY_SMAX - 1; j++)
          out[oi++] = s[j];
      }
      if (oi < PY_SMAX - 1)
        out[oi++] = a[0].s[i];
    }
    out[oi] = 0;
    return mkstr(out);
  }

  /* format (very basic: {0} substitution) */
  if (strcmp(fn, "format") == 0) {
    oi = 0;
    for (int i = 0; s[i] && oi < PY_SMAX - 1;) {
      if (s[i] == '{' && s[i + 1] >= '0' && s[i + 1] <= '9' &&
          s[i + 2] == '}') {
        int idx = s[i + 1] - '0';
        i += 3;
        if (idx < n) {
          char tb[PY_SMAX];
          val_to_buf(a[idx], tb, PY_SMAX);
          for (int j = 0; tb[j] && oi < PY_SMAX - 1; j++)
            out[oi++] = tb[j];
        }
      } else
        out[oi++] = s[i++];
    }
    out[oi] = 0;
    return mkstr(out);
  }
  if (strcmp(fn, "format_map") == 0)
    return sv;

  /* expandtabs */
  if (strcmp(fn, "expandtabs") == 0) {
    int tabsize = n > 0 ? (int)val_num(a[0]) : 8;
    oi = 0;
    for (int i = 0; s[i] && oi < PY_SMAX - 1; i++) {
      if (s[i] == '\t') {
        int sp = tabsize - (oi % tabsize);
        while (sp-- > 0 && oi < PY_SMAX - 1)
          out[oi++] = ' ';
      } else
        out[oi++] = s[i];
    }
    out[oi] = 0;
    return mkstr(out);
  }

  /* partition */
  if (strcmp(fn, "partition") == 0) {
    if (n < 1 || a[0].t != PY_STR)
      return sv;
    const char *sep2 = a[0].s;
    int sl = (int)strlen(sep2);
    for (int i = 0; i <= slen - sl; i++) {
      bool ok = true;
      for (int j = 0; j < sl; j++)
        if (s[i + j] != sep2[j]) {
          ok = false;
          break;
        }
      if (ok) {
        for (oi = 0; oi < i && oi < PY_SMAX - 1; oi++)
          out[oi] = s[oi];
        out[oi] = 0;
        return mkstr(out);
      }
    }
    return sv;
  }

  /* slice: s[start:end] not directly callable but __getitem__ */
  if (strcmp(fn, "__getitem__") == 0) {
    if (n >= 1) {
      int idx = a[0].i;
      if (idx < 0)
        idx += slen;
      if (idx < 0 || idx >= slen)
        return mkstr("");
      char c2[2] = {s[idx], 0};
      return mkstr(c2);
    }
    return sv;
  }

  char msg[56] = "str has no method: ";
  int k = 19;
  for (int j = 0; fn[j] && k < 55; j++)
    msg[k++] = fn[j];
  msg[k] = 0;
  py_error(msg);
  return mkerr();
}

/* ================================================================
 *  Source loader
 * ================================================================ */
#define PY_MAXLINES 128
#define PY_LLEN 200
typedef struct {
  int indent;
  char t[PY_LLEN];
} Ln;
static Ln pylines[PY_MAXLINES];
static int pynl = 0;

static void py_load(const char *src) {
  pynl = 0;
  const char *p = src;
  while (*p && pynl < PY_MAXLINES) {
    int sp = 0;
    while (*p == ' ') {
      sp++;
      p++;
    }
    while (*p == '\t') {
      sp += 4;
      p++;
    }
    const char *ts = p;
    while (*p && *p != '\n' && *p != '\r')
      p++;
    int tlen = (int)(p - ts);
    if (*p == '\r')
      p++;
    if (*p == '\n')
      p++;
    while (tlen > 0 && (ts[tlen - 1] == ' ' || ts[tlen - 1] == '\r'))
      tlen--;
    if (tlen == 0 || ts[0] == '#')
      continue;
    pylines[pynl].indent = sp;
    int j;
    for (j = 0; j < tlen && j < PY_LLEN - 1; j++)
      pylines[pynl].t[j] = ts[j];
    pylines[pynl].t[j] = 0;
    pynl++;
  }
}

/* ================================================================
 *  Lexer helpers
 * ================================================================ */
static const char *lp;
static void lskip(void) {
  while (*lp == ' ' || *lp == '\t')
    lp++;
}
static bool lis_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool lis_digit(char c) { return c >= '0' && c <= '9'; }
static bool lis_alnum(char c) { return lis_alpha(c) || lis_digit(c); }

/* ================================================================
 *  Execution state
 * ================================================================ */
static bool exec_error, exec_break, exec_continue, exec_repl;
/* Scripts run synchronously from the keyboard IRQ, so an unbounded loop
 * cannot be allowed to monopolise the CPU with interrupts masked. */
#define PY_EXEC_MAX_STEPS 20000u
static uint32_t exec_steps;

/* ================================================================
 *  Forward declarations
 * ================================================================ */
static PyVal parse_expr(void);
static PyVal parse_primary(void);
static int parse_arglist(PyVal *args, int max);
static int exec_block(int pc, int parent_ind);

/* ================================================================
 *  Arglist parser
 * ================================================================ */
static int parse_arglist(PyVal *args, int max) {
  int n = 0;
  lskip();
  while (*lp && *lp != ')' && n < max) {
    if (lis_alpha(*lp)) {
      const char *sv = lp;
      char _kw[1];
      int ki = 0;
      (void)_kw;
      while (lis_alnum(*lp) && ki < 15)
        lp++;
      lskip();
      if (*lp == '=') {
        lp++;
        lskip();
        parse_expr();
        lskip();
        if (*lp == ',') {
          lp++;
          lskip();
        }
        continue;
      }
      lp = sv;
    }
    args[n++] = parse_expr();
    lskip();
    if (*lp == ',') {
      lp++;
      lskip();
    }
  }
  if (*lp && *lp != ')') {
    while (*lp && *lp != ')') {
      parse_expr();
      lskip();
      if (*lp == ',') {
        lp++;
        lskip();
      }
    }
    py_error("too many arguments");
  }
  if (*lp == ')')
    lp++;
  return n;
}

/* ================================================================
 *  Expression parser (precedence climbing)
 * ================================================================ */
static PyVal parse_or(void);
static PyVal parse_and(void);
static PyVal parse_not_expr(void);
static PyVal parse_cmp(void);
static PyVal parse_add(void);
static PyVal parse_mul(void);
static PyVal parse_unary(void);

static PyVal parse_expr(void) { return parse_or(); }
static PyVal parse_or(void) {
  PyVal v = parse_and();
  while (1) {
    lskip();
    if (strncmp(lp, "or", 2) == 0 && !lis_alnum(lp[2])) {
      lp += 2;
      PyVal r = parse_and();
      v = mkbool(py_truthy(v) || py_truthy(r));
    } else
      break;
  }
  return v;
}
static PyVal parse_and(void) {
  PyVal v = parse_not_expr();
  while (1) {
    lskip();
    if (strncmp(lp, "and", 3) == 0 && !lis_alnum(lp[3])) {
      lp += 3;
      PyVal r = parse_not_expr();
      v = mkbool(py_truthy(v) && py_truthy(r));
    } else
      break;
  }
  return v;
}
static PyVal parse_not_expr(void) {
  lskip();
  if (strncmp(lp, "not", 3) == 0 && !lis_alnum(lp[3])) {
    lp += 3;
    return mkbool(!py_truthy(parse_not_expr()));
  }
  return parse_cmp();
}
static PyVal parse_cmp(void) {
  PyVal v = parse_add();
  while (1) {
    lskip();
    if (v.t == PY_ERR)
      break;
    if (strncmp(lp, "==", 2) == 0) {
      lp += 2;
      PyVal r = parse_add();
      if (v.t == PY_STR && r.t == PY_STR)
        v = mkbool(strcmp(v.s, r.s) == 0);
      else if (v.t == PY_NONE && r.t == PY_NONE)
        v = mkbool(1);
      else if (v.t == PY_NONE || r.t == PY_NONE)
        v = mkbool(0);
      else if (v.t == PY_STR || r.t == PY_STR)
        v = mkbool(0);
      else
        v = mkbool(val_num(v) == val_num(r));
    } else if (strncmp(lp, "!=", 2) == 0) {
      lp += 2;
      PyVal r = parse_add();
      if (v.t == PY_STR && r.t == PY_STR)
        v = mkbool(strcmp(v.s, r.s) != 0);
      else if (v.t == PY_NONE && r.t == PY_NONE)
        v = mkbool(0);
      else if (v.t == PY_NONE || r.t == PY_NONE)
        v = mkbool(1);
      else if (v.t == PY_STR || r.t == PY_STR)
        v = mkbool(1);
      else
        v = mkbool(val_num(v) != val_num(r));
    } else if (strncmp(lp, "<=", 2) == 0) {
      lp += 2;
      PyVal r = parse_add();
      v = mkbool(val_num(v) <= val_num(r));
    } else if (strncmp(lp, ">=", 2) == 0) {
      lp += 2;
      PyVal r = parse_add();
      v = mkbool(val_num(v) >= val_num(r));
    } else if (*lp == '<' && lp[1] != '<' && lp[1] != '=') {
      lp++;
      PyVal r = parse_add();
      v = mkbool(val_num(v) < val_num(r));
    } else if (*lp == '>' && lp[1] != '>' && lp[1] != '=') {
      lp++;
      PyVal r = parse_add();
      v = mkbool(val_num(v) > val_num(r));
    } else if (strncmp(lp, "in ", 3) == 0 || strncmp(lp, "in\t", 3) == 0) {
      lp += 3;
      lskip();
      PyVal r = parse_add();
      if (v.t == PY_STR && r.t == PY_STR) {
        int nl = (int)strlen(v.s), rl = (int)strlen(r.s);
        bool found = false;
        for (int i = 0; i <= rl - nl; i++) {
          bool ok = true;
          for (int j = 0; j < nl; j++)
            if (r.s[i + j] != v.s[j]) {
              ok = false;
              break;
            }
          if (ok) {
            found = true;
            break;
          }
        }
        v = mkbool(found);
      } else
        v = mkbool(0);
    } else if (strncmp(lp, "not in", 6) == 0 && !lis_alnum(lp[6])) {
      lp += 6;
      lskip();
      PyVal r = parse_add();
      if (v.t == PY_STR && r.t == PY_STR) {
        int nl = (int)strlen(v.s), rl = (int)strlen(r.s);
        bool found = false;
        for (int i = 0; i <= rl - nl; i++) {
          bool ok = true;
          for (int j = 0; j < nl; j++)
            if (r.s[i + j] != v.s[j]) {
              ok = false;
              break;
            }
          if (ok) {
            found = true;
            break;
          }
        }
        v = mkbool(!found);
      } else
        v = mkbool(1);
    } else
      break;
  }
  return v;
}
static PyVal parse_add(void) {
  PyVal v = parse_mul();
  while (1) {
    lskip();
    if (v.t == PY_ERR)
      break;
    if (*lp == '+' && lp[1] != '=') {
      lp++;
      PyVal r = parse_mul();
      if (v.t == PY_STR || r.t == PY_STR) {
        char lb[PY_SMAX], rb[PY_SMAX], out[PY_SMAX];
        val_to_buf(v, lb, PY_SMAX);
        val_to_buf(r, rb, PY_SMAX);
        int oi = 0, k;
        for (k = 0; lb[k] && oi < PY_SMAX - 1; k++)
          out[oi++] = lb[k];
        for (k = 0; rb[k] && oi < PY_SMAX - 1; k++)
          out[oi++] = rb[k];
        out[oi] = 0;
        v = mkstr(out);
      } else if (v.t == PY_FLOAT || r.t == PY_FLOAT)
        v = mkfloat(val_num(v) + val_num(r));
      else
        v = mkint(v.i + r.i);
    } else if (*lp == '-' && lp[1] != '=') {
      lp++;
      PyVal r = parse_mul();
      if (v.t == PY_FLOAT || r.t == PY_FLOAT)
        v = mkfloat(val_num(v) - val_num(r));
      else
        v = mkint(v.i - r.i);
    } else
      break;
  }
  return v;
}
static PyVal parse_mul(void) {
  PyVal v = parse_unary();
  while (1) {
    lskip();
    if (v.t == PY_ERR)
      break;
    if (*lp == '*' && lp[1] == '*') {
      lp += 2;
      PyVal r = parse_unary();
      v = mkfloat(my_pow(val_num(v), val_num(r)));
    } else if (*lp == '*' && lp[1] != '=' && lp[1] != '*') {
      lp++;
      PyVal r = parse_unary();
      if (v.t == PY_STR && r.t == PY_INT) {
        char out[PY_SMAX];
        out[0] = 0;
        int oi = 0, len = (int)strlen(v.s);
        for (int k = 0; k < r.i && oi + len < PY_SMAX - 1; k++)
          for (int j = 0; j < len && oi < PY_SMAX - 1; j++)
            out[oi++] = v.s[j];
        out[oi] = 0;
        v = mkstr(out);
      } else if (v.t == PY_FLOAT || r.t == PY_FLOAT)
        v = mkfloat(val_num(v) * val_num(r));
      else
        v = mkint(v.i * r.i);
    } else if (lp[0] == '/' && lp[1] == '/') {
      lp += 2;
      PyVal r = parse_unary();
      if (val_num(r) == 0.0) {
        py_error("ZeroDivisionError");
        return mkerr();
      }
      if (v.t == PY_FLOAT || r.t == PY_FLOAT)
        v = mkfloat(my_floor(val_num(v) / val_num(r)));
      else {
        int32_t q = v.i / r.i;
        if ((v.i ^ r.i) < 0 && q * r.i != v.i)
          q--;
        v = mkint(q);
      }
    } else if (lp[0] == '/' && lp[1] != '/') {
      lp++;
      PyVal r = parse_unary();
      if (val_num(r) == 0.0) {
        py_error("ZeroDivisionError");
        return mkerr();
      }
      v = mkfloat(val_num(v) / val_num(r));
    } else if (lp[0] == '%' && lp[1] != '=') {
      lp++;
      PyVal r = parse_unary();
      if (v.t == PY_FLOAT || r.t == PY_FLOAT) {
        double a = val_num(v), b = val_num(r);
        v = mkfloat(a - my_floor(a / b) * b);
      } else {
        if (r.i == 0) {
          py_error("ZeroDivisionError");
          return mkerr();
        }
        v = mkint(v.i % r.i);
      }
    } else if (*lp == '&' && lp[1] != '=') {
      lp++;
      PyVal r = parse_unary();
      v = mkint(v.i & r.i);
    } else if (*lp == '|' && lp[1] != '=') {
      lp++;
      PyVal r = parse_unary();
      v = mkint(v.i | r.i);
    } else if (*lp == '^' && lp[1] != '=') {
      lp++;
      PyVal r = parse_unary();
      v = mkint(v.i ^ r.i);
    } else if (lp[0] == '<' && lp[1] == '<') {
      lp += 2;
      PyVal r = parse_unary();
      v = mkint(v.i << r.i);
    } else if (lp[0] == '>' && lp[1] == '>') {
      lp += 2;
      PyVal r = parse_unary();
      v = mkint(v.i >> r.i);
    } else
      break;
  }
  return v;
}
static PyVal parse_unary(void) {
  lskip();
  if (*lp == '-') {
    lp++;
    PyVal v = parse_unary();
    return (v.t == PY_FLOAT) ? mkfloat(-v.d) : mkint(-v.i);
  }
  if (*lp == '+') {
    lp++;
    return parse_unary();
  }
  if (*lp == '~') {
    lp++;
    PyVal v = parse_unary();
    return mkint(~v.i);
  }
  return parse_primary();
}

/* ================================================================
 *  Primary
 * ================================================================ */
static PyVal parse_primary(void) {
  lskip();

  /* Numeric literal */
  if (lis_digit(*lp)) {
    int64_t n = 0;
    while (lis_digit(*lp)) {
      n = n * 10 + (*lp - '0');
      lp++;
    }
    if (*lp == '.') {
      lp++;
      double frac = 0.0, sc = 0.1;
      while (lis_digit(*lp)) {
        frac += (*lp - '0') * sc;
        sc *= 0.1;
        lp++;
      }
      return mkfloat((double)n + frac);
    }
    return mkint((int32_t)n);
  }

  /* String literal */
  if (*lp == '"' || *lp == '\'') {
    char q = *lp++;
    /* Triple-quote support */
    bool triple = (lp[0] == q && lp[1] == q);
    if (triple)
      lp += 2;
    char buf[PY_SMAX];
    int bi = 0;
    while (*lp && bi < PY_SMAX - 1) {
      if (triple) {
        if (lp[0] == q && lp[1] == q && lp[2] == q) {
          lp += 3;
          break;
        }
      } else {
        if (*lp == q)
          break;
      }
      if (*lp == '\\' && lp[1]) {
        lp++;
        switch (*lp) {
        case 'n':
          buf[bi++] = '\n';
          break;
        case 't':
          buf[bi++] = '\t';
          break;
        case 'r':
          buf[bi++] = '\r';
          break;
        case '\\':
          buf[bi++] = '\\';
          break;
        case '"':
          buf[bi++] = '"';
          break;
        case '\'':
          buf[bi++] = '\'';
          break;
        case '0':
          buf[bi++] = 0;
          break;
        default:
          buf[bi++] = *lp;
          break;
        }
        lp++;
      } else
        buf[bi++] = *lp++;
    }
    if (!triple && *lp == q)
      lp++;
    buf[bi] = 0;
    return mkstr(buf);
  }

  /* Keywords */
  if (strncmp(lp, "True", 4) == 0 && !lis_alnum(lp[4])) {
    lp += 4;
    return mkbool(1);
  }
  if (strncmp(lp, "False", 5) == 0 && !lis_alnum(lp[5])) {
    lp += 5;
    return mkbool(0);
  }
  if (strncmp(lp, "None", 4) == 0 && !lis_alnum(lp[4])) {
    lp += 4;
    return mknone();
  }

  /* Name */
  if (lis_alpha(*lp)) {
    char name[32];
    int ni = 0;
    while (lis_alnum(*lp) && ni < 31)
      name[ni++] = *lp++;
    name[ni] = 0;
    lskip();

    /* Method/attribute chain on a known variable (str.method or mod.attr) */
    if (*lp == '.') {
      PyVal var = (py_var_exists(name)) ? py_var_get(name) : mkerr();
      /* For modules: name.attr or name.func() */
      if (var.t == PY_MOD) {
        lp++;
        char attr[32];
        int ai = 0;
        while (lis_alnum(*lp) && ai < 31)
          attr[ai++] = *lp++;
        attr[ai] = 0;
        lskip();
        if (*lp == '(') {
          lp++;
          PyVal args[8];
          int nargs = parse_arglist(args, 8);
          return mod_call(var.mod, attr, args, nargs);
        }
        return mod_getattr(var.mod, attr);
      }
      /* For strings: name.method() */
      if (var.t == PY_STR) {
        lp++;
        char method[32];
        int mi = 0;
        while (lis_alnum(*lp) && mi < 31)
          method[mi++] = *lp++;
        method[mi] = 0;
        lskip();
        if (*lp == '(') {
          lp++;
          PyVal args[4];
          int na = parse_arglist(args, 4);
          return str_method(var, method, args, na);
        }
        if (strcmp(method, "__len__") == 0 || strcmp(method, "length") == 0)
          return mkint((int32_t)strlen(var.s));
        return mkerr();
      }
    }

    /* Function call */
    if (*lp == '(') {
      lp++;
      PyVal args[8];
      int na = parse_arglist(args, 8);

      /* --- Core builtins --- */
      if (strcmp(name, "print") == 0) {
        for (int i = 0; i < na; i++) {
          if (i > 0)
            py_putc(' ');
          py_print_val(args[i]);
        }
        py_putc('\n');
        return mknone();
      }
      if (strcmp(name, "len") == 0)
        return (na == 1 && args[0].t == PY_STR)
                   ? mkint((int32_t)strlen(args[0].s))
                   : mkint(0);
      if (strcmp(name, "str") == 0) {
        char b[PY_SMAX];
        if (na >= 1)
          val_to_buf(args[0], b, PY_SMAX);
        else
          b[0] = 0;
        return mkstr(b);
      }
      if (strcmp(name, "int") == 0) {
        if (na >= 1) {
          if (args[0].t == PY_INT || args[0].t == PY_BOOL)
            return mkint(args[0].i);
          if (args[0].t == PY_FLOAT)
            return mkint((int32_t)args[0].d);
          if (args[0].t == PY_STR) {
            const char *p = args[0].s;
            while (*p == ' ')
              p++;
            bool neg = (*p == '-');
            if (*p == '-' || *p == '+')
              p++;
            int32_t v = 0;
            while (*p >= '0' && *p <= '9')
              v = v * 10 + (*p++ - '0');
            return mkint(neg ? -v : v);
          }
        }
        return mkint(0);
      }
      if (strcmp(name, "float") == 0) {
        if (na >= 1) {
          if (args[0].t == PY_FLOAT)
            return args[0];
          if (args[0].t == PY_INT || args[0].t == PY_BOOL)
            return mkfloat((double)args[0].i);
          if (args[0].t == PY_STR) {
            const char *p = args[0].s;
            while (*p == ' ')
              p++;
            bool neg = (*p == '-');
            if (*p == '-' || *p == '+')
              p++;
            double v = 0;
            while (*p >= '0' && *p <= '9')
              v = v * 10 + (*p++ - '0');
            if (*p == '.') {
              p++;
              double sc = 0.1;
              while (*p >= '0' && *p <= '9') {
                v += (*p++ - '0') * sc;
                sc *= 0.1;
              }
            }
            return mkfloat(neg ? -v : v);
          }
        }
        return mkfloat(0.0);
      }
      if (strcmp(name, "bool") == 0)
        return (na >= 1) ? mkbool(py_truthy(args[0])) : mkbool(0);
      if (strcmp(name, "abs") == 0) {
        if (na == 1) {
          if (args[0].t == PY_FLOAT)
            return mkfloat(my_fabs(args[0].d));
          return mkint(args[0].i < 0 ? -args[0].i : args[0].i);
        }
        return mkint(0);
      }
      if (strcmp(name, "round") == 0) {
        if (na >= 1) {
          double d = val_num(args[0]);
          int nd = (na >= 2) ? args[1].i : 0;
          double sc = my_pow(10.0, (double)nd);
          double r = my_floor(d * sc + 0.5) / sc;
          return (nd == 0) ? mkint((int32_t)r) : mkfloat(r);
        }
        return mkint(0);
      }
      if (strcmp(name, "pow") == 0)
        return (na >= 2) ? mkfloat(my_pow(val_num(args[0]), val_num(args[1])))
                         : mkint(1);
      if (strcmp(name, "min") == 0) {
        if (na >= 1) {
          double m = val_num(args[0]);
          for (int i = 1; i < na; i++) {
            double x = val_num(args[i]);
            if (x < m)
              m = x;
          }
          return (args[0].t == PY_FLOAT) ? mkfloat(m) : mkint((int32_t)m);
        }
        return mkint(0);
      }
      if (strcmp(name, "max") == 0) {
        if (na >= 1) {
          double m = val_num(args[0]);
          for (int i = 1; i < na; i++) {
            double x = val_num(args[i]);
            if (x > m)
              m = x;
          }
          return (args[0].t == PY_FLOAT) ? mkfloat(m) : mkint((int32_t)m);
        }
        return mkint(0);
      }
      if (strcmp(name, "sum") == 0) {
        double s = 0;
        for (int i = 0; i < na; i++)
          s += val_num(args[i]);
        return mkfloat(s);
      }
      if (strcmp(name, "chr") == 0) {
        if (na == 1 && args[0].t == PY_INT) {
          char s2[2] = {(char)(args[0].i & 0x7F), 0};
          return mkstr(s2);
        }
        return mkstr("");
      }
      if (strcmp(name, "ord") == 0)
        return (na == 1 && args[0].t == PY_STR)
                   ? mkint((int32_t)(unsigned char)args[0].s[0])
                   : mkint(0);
      if (strcmp(name, "hex") == 0) {
        if (na == 1 && args[0].t == PY_INT) {
          char buf[24];
          buf[0] = '0';
          buf[1] = 'x';
          int bi = 2;
          uint32_t n2 = (uint32_t)args[0].i;
          char tmp[8];
          int ti = 0;
          if (n2 == 0)
            tmp[ti++] = '0';
          while (n2 > 0 && ti < 8) {
            int d = (int)(n2 & 0xF);
            tmp[ti++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
            n2 >>= 4;
          }
          for (int j = ti - 1; j >= 0 && bi < 23; j--)
            buf[bi++] = tmp[j];
          buf[bi] = 0;
          return mkstr(buf);
        }
        return mkstr("0x0");
      }
      if (strcmp(name, "bin") == 0) {
        if (na == 1 && args[0].t == PY_INT) {
          char buf[40];
          buf[0] = '0';
          buf[1] = 'b';
          int bi = 2;
          uint32_t n2 = (uint32_t)args[0].i;
          char tmp[32];
          int ti = 0;
          if (n2 == 0)
            tmp[ti++] = '0';
          while (n2 > 0 && ti < 32) {
            tmp[ti++] = (char)('0' + (n2 & 1));
            n2 >>= 1;
          }
          for (int j = ti - 1; j >= 0 && bi < 38; j--)
            buf[bi++] = tmp[j];
          buf[bi] = 0;
          return mkstr(buf);
        }
        return mkstr("0b0");
      }
      if (strcmp(name, "oct") == 0) {
        if (na == 1 && args[0].t == PY_INT) {
          char buf[32];
          buf[0] = '0';
          buf[1] = 'o';
          int bi = 2;
          uint32_t n2 = (uint32_t)args[0].i;
          char tmp[16];
          int ti = 0;
          if (n2 == 0)
            tmp[ti++] = '0';
          while (n2 > 0 && ti < 15) {
            tmp[ti++] = (char)('0' + (n2 & 7));
            n2 >>= 3;
          }
          for (int j = ti - 1; j >= 0 && bi < 30; j--)
            buf[bi++] = tmp[j];
          buf[bi] = 0;
          return mkstr(buf);
        }
        return mkstr("0o0");
      }
      if (strcmp(name, "type") == 0) {
        if (na == 1)
          switch (args[0].t) {
          case PY_INT:
            return mkstr("<class 'int'>");
          case PY_FLOAT:
            return mkstr("<class 'float'>");
          case PY_STR:
            return mkstr("<class 'str'>");
          case PY_BOOL:
            return mkstr("<class 'bool'>");
          case PY_NONE:
            return mkstr("<class 'NoneType'>");
          case PY_MOD:
            return mkstr("<class 'module'>");
          default:
            return mkstr("<class 'object'>");
          }
        return mkstr("<class 'object'>");
      }
      if (strcmp(name, "isinstance") == 0) {
        if (na == 2 && args[1].t == PY_STR) {
          const char *tp = args[1].s;
          if (strcmp(tp, "int") == 0)
            return mkbool(args[0].t == PY_INT || args[0].t == PY_BOOL);
          if (strcmp(tp, "float") == 0)
            return mkbool(args[0].t == PY_FLOAT);
          if (strcmp(tp, "str") == 0)
            return mkbool(args[0].t == PY_STR);
          if (strcmp(tp, "bool") == 0)
            return mkbool(args[0].t == PY_BOOL);
        }
        return mkbool(0);
      }
      if (strcmp(name, "divmod") == 0) {
        if (na == 2) {
          int32_t b = args[1].i;
          if (b == 0) {
            py_error("ZeroDivisionError: integer division or modulo by zero");
            return mkerr();
          }
          int32_t a = args[0].i;
          int32_t q = a / b;
          int32_t r = a % b;
          if ((a ^ b) < 0 && r != 0) {
            q--;
            r += b;
          }
          char buf[40];
          char qb[20], rb[20];
          val_to_buf(mkint(q), qb, 20);
          val_to_buf(mkint(r), rb, 20);
          int bi = 0;
          buf[bi++] = '(';
          for (int k = 0; qb[k]; k++)
            buf[bi++] = qb[k];
          buf[bi++] = ',';
          buf[bi++] = ' ';
          for (int k = 0; rb[k]; k++)
            buf[bi++] = rb[k];
          buf[bi++] = ')';
          buf[bi] = 0;
          return mkstr(buf);
        }
        return mkint(0);
      }
      if (strcmp(name, "input") == 0) {
        if (na > 0)
          py_print_val(args[0]);
        char buf[80];
        int bi = 0;
        while (bi < 79) {
          char c = keyboard_getchar();
          if (c == '\n' || c == '\r') {
            terminal_putchar('\n');
            break;
          }
          if (c == '\b') {
            if (bi > 0) {
              bi--;
              terminal_backspace();
            }
            continue;
          }
          if (c >= ' ' && c <= '~') {
            buf[bi++] = c;
            terminal_putchar(c);
          }
        }
        buf[bi] = 0;
        return mkstr(buf);
      }
      if (strcmp(name, "range") == 0) {
        py_error("range() only valid in for..in..range()");
        return mkerr();
      }
      if (strcmp(name, "sorted") == 0) {
        if (na >= 1)
          return args[0];
        return mknone();
      }
      if (strcmp(name, "reversed") == 0) {
        if (na >= 1 && args[0].t == PY_STR) {
          char out[PY_SMAX];
          int l = (int)strlen(args[0].s), oi = 0;
          for (int i = l - 1; i >= 0 && oi < PY_SMAX - 1; i--)
            out[oi++] = args[0].s[i];
          out[oi] = 0;
          return mkstr(out);
        }
        return mknone();
      }
      /* Collections are not part of this small interpreter yet.  Returning
       * a plausible but wrong value is worse than a clear Python error. */
      if (strcmp(name, "enumerate") == 0 || strcmp(name, "map") == 0 ||
          strcmp(name, "filter") == 0 || strcmp(name, "zip") == 0) {
        py_error("collection helper not supported");
        return mkerr();
      }
      if (strcmp(name, "id") == 0)
        return mkint(42);
      if (strcmp(name, "callable") == 0) {
        if (na >= 1)
          return mkbool(args[0].t == PY_MOD || args[0].t == PY_BUILTIN);
        return mkbool(0);
      }
      if (strcmp(name, "hash") == 0) {
        if (na >= 1) {
          uint32_t h = 5381;
          char hash_buf[PY_SMAX];
          val_to_buf(args[0], hash_buf, PY_SMAX);
          for (char *p2 = hash_buf; *p2; p2++)
            h = ((h << 5) + h) + (unsigned char)*p2;
          return mkint((int32_t)h);
        }
        return mkint(0);
      }
      if (strcmp(name, "repr") == 0) {
        if (na >= 1) {
          char buf[PY_SMAX + 4];
          if (args[0].t == PY_STR) {
            buf[0] = '\'';
            int i = 1;
            for (int j = 0; args[0].s[j] && i < PY_SMAX + 2; j++)
              buf[i++] = args[0].s[j];
            buf[i++] = '\'';
            buf[i] = 0;
          } else
            val_to_buf(args[0], buf, (int)sizeof(buf));
          return mkstr(buf);
        }
        return mkstr("None");
      }

      /* Check PY_BUILTIN variable */
      if (py_var_exists(name)) {
        PyVal var = py_var_get(name);
        if (var.t == PY_BUILTIN)
          return mod_call(var.mod, var.s, args, na);
      }
      {
        char msg[56] = "not defined: ";
        int k = 13;
        for (int j = 0; name[j] && k < 55; j++)
          msg[k++] = name[j];
        msg[k] = 0;
        py_error(msg);
      }
      return mkerr();
    }

    /* Module attribute or str method after variable lookup */
    if (*lp == '.') {
      if (py_var_exists(name)) {
        PyVal var = py_var_get(name);
        if (var.t == PY_MOD) {
          lp++;
          char attr[32];
          int ai = 0;
          while (lis_alnum(*lp) && ai < 31)
            attr[ai++] = *lp++;
          attr[ai] = 0;
          lskip();
          if (*lp == '(') {
            lp++;
            PyVal args[8];
            int na2 = parse_arglist(args, 8);
            return mod_call(var.mod, attr, args, na2);
          }
          return mod_getattr(var.mod, attr);
        }
        if (var.t == PY_STR) {
          lp++;
          char method[32];
          int mi = 0;
          while (lis_alnum(*lp) && mi < 31)
            method[mi++] = *lp++;
          method[mi] = 0;
          lskip();
          if (*lp == '(') {
            lp++;
            PyVal args[4];
            int na2 = parse_arglist(args, 4);
            return str_method(var, method, args, na2);
          }
          return mkerr();
        }
      }
    }

    return py_var_get(name);
  }

  /* Parenthesised expression */
  if (*lp == '(') {
    lp++;
    PyVal v = parse_expr();
    lskip();
    if (*lp == ')')
      lp++;
    return v;
  }

  py_error("syntax error in expression");
  return mkerr();
}

/* ================================================================
 *  Statement helpers
 * ================================================================ */
static int skip_block(int pc, int bi) {
  while (pc < pynl && pylines[pc].indent > bi)
    pc++;
  return pc;
}
static int skip_if_chain(int pc, int ind) {
  while (pc < pynl && pylines[pc].indent == ind) {
    const char *t = pylines[pc].t;
    if (strncmp(t, "elif ", 5) == 0 || strcmp(t, "else:") == 0 ||
        strncmp(t, "else:", 5) == 0)
      pc = skip_block(pc + 1, ind);
    else
      break;
  }
  return pc;
}
static void strip_colon(const char *src, char *dst, int ml) {
  int len = (int)strlen(src);
  while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\r'))
    len--;
  if (len > 0 && src[len - 1] == ':')
    len--;
  while (len > 0 && src[len - 1] == ' ')
    len--;
  if (len >= ml)
    len = ml - 1;
  int i;
  for (i = 0; i < len; i++)
    dst[i] = src[i];
  dst[i] = 0;
}

/* ================================================================
 *  Compound statement executors
 * ================================================================ */
static int exec_if(int pc) {
  int ind = pylines[pc].indent;
  char cond[PY_LLEN];
  strip_colon(pylines[pc].t + 3, cond, PY_LLEN);
  lp = cond;
  PyVal cv = parse_expr();
  if (cv.t == PY_ERR) {
    exec_error = true;
    return skip_if_chain(skip_block(pc + 1, ind), ind);
  }
  if (py_truthy(cv))
    return skip_if_chain(exec_block(pc + 1, ind), ind);
  int next = skip_block(pc + 1, ind);
  while (next < pynl && pylines[next].indent == ind) {
    const char *nt = pylines[next].t;
    if (strncmp(nt, "elif ", 5) == 0) {
      char ec[PY_LLEN];
      strip_colon(nt + 5, ec, PY_LLEN);
      lp = ec;
      PyVal ev = parse_expr();
      if (ev.t == PY_ERR) {
        exec_error = true;
        return skip_if_chain(skip_block(next + 1, ind), ind);
      }
      if (py_truthy(ev))
        return skip_if_chain(exec_block(next + 1, ind), ind);
      next = skip_block(next + 1, ind);
    } else if (strcmp(nt, "else:") == 0 || strncmp(nt, "else:", 5) == 0) {
      return exec_block(next + 1, ind);
    } else
      break;
  }
  return next;
}
static int exec_while(int pc) {
  int ind = pylines[pc].indent;
  char cond[PY_LLEN];
  strip_colon(pylines[pc].t + 6, cond, PY_LLEN);
  int body_end = skip_block(pc + 1, ind);
  while (!exec_error && !sys_exit_called) {
    lp = cond;
    PyVal cv = parse_expr();
    if (cv.t == PY_ERR) {
      exec_error = true;
      break;
    }
    if (!py_truthy(cv))
      break;
    exec_block(pc + 1, ind);
    if (exec_break) {
      exec_break = false;
      break;
    }
    if (exec_continue)
      exec_continue = false;
  }
  return body_end;
}
static int exec_for(int pc) {
  int ind = pylines[pc].indent;
  const char *t = pylines[pc].t + 4;
  char var[32];
  int vi = 0;
  while (lis_alnum(*t) && vi < 31)
    var[vi++] = *t++;
  var[vi] = 0;
  while (*t == ' ')
    t++;
  if (strncmp(t, "in range(", 9) != 0 && strncmp(t, "in  range(", 10) != 0) {
    py_error("for: only 'for var in range(...)' supported");
    return skip_block(pc + 1, ind);
  }
  while (*t && *t != '(')
    t++;
  if (*t == '(')
    t++;
  lp = t;
  PyVal a1 = parse_expr();
  lskip();
  int32_t rs = 0, re = 0, rstep = 1;
  if (*lp == ',') {
    lp++;
    PyVal a2 = parse_expr();
    lskip();
    if (*lp == ',') {
      lp++;
      PyVal a3 = parse_expr();
      rs = a1.i;
      re = a2.i;
      rstep = a3.i;
    } else {
      rs = a1.i;
      re = a2.i;
    }
  } else
    re = a1.i;
  if (rstep == 0) {
    py_error("range(): step=0");
    return skip_block(pc + 1, ind);
  }
  int body_end = skip_block(pc + 1, ind);
  for (int32_t i = rs; (rstep > 0) ? (i < re) : (i > re); i += rstep) {
    if (exec_error || sys_exit_called)
      break;
    py_var_set(var, mkint(i));
    exec_block(pc + 1, ind);
    if (exec_break) {
      exec_break = false;
      break;
    }
    if (exec_continue)
      exec_continue = false;
  }
  return body_end;
}

/* ================================================================
 *  Assignment detection
 * ================================================================ */
static bool is_assignment(const char *t, char *vn, int *op, const char **es) {
  if (!lis_alpha(*t))
    return false;
  int vi = 0;
  const char *p = t;
  while (lis_alnum(*p) && vi < 31)
    vn[vi++] = *p++;
  vn[vi] = 0;
  while (*p == ' ')
    p++;
  if (*p == '=' && p[1] != '=') {
    *op = 0;
    *es = p + 1;
    while (**es == ' ')
      (*es)++;
    return true;
  }
  if (*p == '+' && p[1] == '=') {
    *op = 1;
    *es = p + 2;
    while (**es == ' ')
      (*es)++;
    return true;
  }
  if (*p == '-' && p[1] == '=') {
    *op = 2;
    *es = p + 2;
    while (**es == ' ')
      (*es)++;
    return true;
  }
  if (*p == '*' && p[1] == '=' && p[2] != '*') {
    *op = 3;
    *es = p + 2;
    while (**es == ' ')
      (*es)++;
    return true;
  }
  if (*p == '/' && p[1] == '=') {
    *op = 4;
    *es = p + 2;
    while (**es == ' ')
      (*es)++;
    return true;
  }
  if (*p == '%' && p[1] == '=') {
    *op = 5;
    *es = p + 2;
    while (**es == ' ')
      (*es)++;
    return true;
  }
  return false;
}

/* ================================================================
 *  Single-line executor
 * ================================================================ */
static int exec_line(int pc) {
  if (exec_error || exec_break || exec_continue || sys_exit_called)
    return pc + 1;
  if (++exec_steps > PY_EXEC_MAX_STEPS) {
    py_error("execution limit exceeded (possible infinite loop)");
    exec_error = true;
    return pynl;
  }
  const char *t = pylines[pc].t;
  int ind = pylines[pc].indent;

  if (strcmp(t, "pass") == 0)
    return pc + 1;
  if (strcmp(t, "break") == 0) {
    exec_break = true;
    return pc + 1;
  }
  if (strcmp(t, "continue") == 0) {
    exec_continue = true;
    return pc + 1;
  }
  if (strncmp(t, "if ", 3) == 0)
    return exec_if(pc);
  if (strncmp(t, "while ", 6) == 0)
    return exec_while(pc);
  if (strncmp(t, "for ", 4) == 0)
    return exec_for(pc);
  if (strncmp(t, "elif ", 5) == 0 || strcmp(t, "else:") == 0 ||
      strncmp(t, "else:", 5) == 0)
    return skip_block(pc + 1, ind);

  /* import X */
  if (strncmp(t, "import ", 7) == 0) {
    const char *mn = t + 7;
    while (*mn == ' ')
      mn++;
    /* Handle 'import X as Y' */
    char modname[32], alias[32];
    int mi = 0, ai = 0;
    while (*mn && *mn != ' ' && mi < 31)
      modname[mi++] = *mn++;
    modname[mi] = 0;
    while (*mn == ' ')
      mn++;
    if (strncmp(mn, "as ", 3) == 0) {
      mn += 3;
      while (*mn == ' ')
        mn++;
      while (*mn && ai < 31)
        alias[ai++] = *mn++;
      alias[ai] = 0;
    } else {
      for (int i = 0; modname[i] && i < 31; i++)
        alias[i] = modname[i];
      alias[mi] = 0;
    }
    uint8_t mid2 = modname_to_id(modname);
    if (mid2 == 0) {
      char msg[56] = "ModuleNotFoundError: '";
      int k = 22;
      for (int j = 0; modname[j] && k < 54; j++)
        msg[k++] = modname[j];
      msg[k++] = '\'';
      msg[k] = 0;
      py_error(msg);
    } else {
      py_var_set(alias, mkmod(mid2));
    }
    return pc + 1;
  }

  /* from X import Y [as Z] */
  if (strncmp(t, "from ", 5) == 0) {
    const char *p = t + 5;
    while (*p == ' ')
      p++;
    char mn[32];
    int mi = 0;
    while (lis_alnum(*p) && mi < 31)
      mn[mi++] = *p++;
    mn[mi] = 0;
    while (*p == ' ')
      p++;
    if (strncmp(p, "import ", 7) != 0) {
      py_error("invalid from-import");
      return pc + 1;
    }
    p += 7;
    uint8_t mid2 = modname_to_id(mn);
    if (mid2 == 0) {
      py_error("ModuleNotFoundError in from-import");
      return pc + 1;
    }
    if (*p == '*') {
      /* from X import * — import all known attrs */
      return pc + 1; /* stub: no star import */
    }
    while (*p) {
      while (*p == ' ')
        p++;
      char attr[32];
      int ai = 0;
      while (lis_alnum(*p) && ai < 31)
        attr[ai++] = *p++;
      attr[ai] = 0;
      if (!*attr)
        break;
      char alias2[32];
      int li = 0;
      while (*p == ' ')
        p++;
      if (strncmp(p, "as ", 3) == 0) {
        p += 3;
        while (*p == ' ')
          p++;
        while (lis_alnum(*p) && li < 31)
          alias2[li++] = *p++;
        alias2[li] = 0;
      } else {
        for (int i = 0; attr[i] && i < 31; i++)
          alias2[i] = attr[i];
        alias2[ai] = 0;
      }
      PyVal val = mod_getattr(mid2, attr);
      if (val.t == PY_ERR) {
        exec_error = true;
        return pc + 1;
      }
      py_var_set(alias2, val);
      while (*p == ' ')
        p++;
      if (*p == ',')
        p++;
    }
    return pc + 1;
  }

  /* print(...) fast path */
  if (strncmp(t, "print(", 6) == 0 || strcmp(t, "print()") == 0) {
    lp = t + 6;
    lskip();
    if (*lp == ')') {
      py_putc('\n');
      return pc + 1;
    }
    bool first = true;
    while (*lp && *lp != ')') {
      if (lis_alpha(*lp)) {
        const char *sv = lp;
        char _kw2[1];
        int ki = 0;
        (void)_kw2;
        while (lis_alnum(*lp) && ki < 1)
          ki++;
        lskip();
        if (*lp == '=') {
          lp++;
          lskip();
          parse_expr();
          lskip();
          if (*lp == ',') {
            lp++;
            lskip();
          }
          continue;
        }
        lp = sv;
      }
      if (!first)
        py_putc(' ');
      first = false;
      PyVal v = parse_expr();
      if (v.t == PY_ERR) {
        exec_error = true;
        break;
      }
      py_print_val(v);
      lskip();
      if (*lp == ',') {
        lp++;
        lskip();
      }
    }
    if (!exec_error)
      py_putc('\n');
    return pc + 1;
  }

  /* Assignment */
  {
    char vn[32];
    int op;
    const char *es;
    if (is_assignment(t, vn, &op, &es)) {
      lp = es;
      PyVal val = parse_expr();
      if (val.t == PY_ERR) {
        exec_error = true;
        return pc + 1;
      }
      if (op == 0) {
        py_var_set(vn, val);
      } else {
        PyVal old = py_var_get(vn);
        if (old.t == PY_ERR) {
          exec_error = true;
          return pc + 1;
        }
        switch (op) {
        case 1:
          if (old.t == PY_STR || val.t == PY_STR) {
            char lb[PY_SMAX], rb[PY_SMAX], out[PY_SMAX];
            val_to_buf(old, lb, PY_SMAX);
            val_to_buf(val, rb, PY_SMAX);
            int oi = 0, k;
            for (k = 0; lb[k] && oi < PY_SMAX - 1; k++)
              out[oi++] = lb[k];
            for (k = 0; rb[k] && oi < PY_SMAX - 1; k++)
              out[oi++] = rb[k];
            out[oi] = 0;
            py_var_set(vn, mkstr(out));
          } else if (old.t == PY_FLOAT || val.t == PY_FLOAT)
            py_var_set(vn, mkfloat(val_num(old) + val_num(val)));
          else
            py_var_set(vn, mkint(old.i + val.i));
          break;
        case 2:
          py_var_set(vn, (old.t == PY_FLOAT || val.t == PY_FLOAT)
                             ? mkfloat(val_num(old) - val_num(val))
                             : mkint(old.i - val.i));
          break;
        case 3:
          py_var_set(vn, (old.t == PY_FLOAT || val.t == PY_FLOAT)
                             ? mkfloat(val_num(old) * val_num(val))
                             : mkint(old.i * val.i));
          break;
        case 4:
          if (val_num(val) == 0.0) {
            py_error("ZeroDivisionError");
            exec_error = true;
          } else
            py_var_set(vn, mkfloat(val_num(old) / val_num(val)));
          break;
        case 5:
          if (val.i == 0) {
            py_error("ZeroDivisionError");
            exec_error = true;
          } else
            py_var_set(vn, (old.t == PY_FLOAT || val.t == PY_FLOAT)
                               ? mkfloat(val_num(old) -
                                         my_floor(val_num(old) / val_num(val)) *
                                             val_num(val))
                               : mkint(old.i % val.i));
          break;
        }
      }
      return pc + 1;
    }
  }

  /* Expression statement */
  lp = t;
  PyVal res = parse_expr();
  if (res.t == PY_ERR) {
    exec_error = true;
    return pc + 1;
  }
  if (exec_repl && res.t != PY_NONE) {
    py_print_val(res);
    py_putc('\n');
  }
  return pc + 1;
}

/* ================================================================
 *  Block executor
 * ================================================================ */
static int exec_block(int pc, int parent_ind) {
  while (pc < pynl && pylines[pc].indent > parent_ind) {
    if (exec_error || exec_break || exec_continue || sys_exit_called)
      break;
    pc = exec_line(pc);
  }
  return pc;
}

/* ================================================================
 *  Run source
 * ================================================================ */
static void py_run_src(const char *src, bool repl) {
  exec_repl = repl;
  exec_error = false;
  exec_break = false;
  exec_continue = false;
  sys_exit_called = false;
  exec_steps = 0;
  py_load(src);
  if (pynl == 0)
    return;
  exec_block(0, -1);
}

/* ================================================================
 *  Public API
 * ================================================================ */
void python_run_file(const char *filename) {
  uint32_t size = 0;
  const char *data = NULL;
  static uint8_t disk_buf[8192];

  if (shell_is_disk()) {
    int res = fat32_read_file(filename, disk_buf, sizeof(disk_buf) - 1);
    if (res >= 0) {
      disk_buf[res] = '\0';
      data = (const char *)disk_buf;
      size = (uint32_t)res;
    }
  } else {
    data = ramfs_read(filename, &size);
  }

  if (!data) {
    terminal_set_color(0x04);
    py_puts("python: file not found: ");
    py_puts(filename);
    py_putc('\n');
    terminal_set_color(0x07);
    return;
  }
  (void)size;
  terminal_set_color(0x0B);
  py_puts("Running ");
  py_puts(filename);
  py_puts("...\n");
  terminal_set_color(0x07);
  trt_reset_for_next_program();
  py_vars_clear();
  py_run_src(data, false);
  if (!exec_error && !sys_exit_called) {
    terminal_set_color(0x0A);
    py_puts("[Script finished]\n");
    terminal_set_color(0x07);
  }
}

void python_repl(void) {
  terminal_set_color(0x0B);
  py_puts(" _   _             ___        \n");
  py_puts("| \\ | | _____  __ | _ \\  _  _ \n");
  py_puts("|  \\| |/ _ \\ \\/ / |  _/ | || |\n");
  py_puts("| |\\  | (_) >  <  |_|    \\_, |\n");
  py_puts("|_| \\_|\\___/_/\\_\\        |__/ \n");
  terminal_set_color(0x0E);
  py_puts("NovexPy 3.0  |  Python 3 subset\n");
  terminal_set_color(0x07);
  py_puts("Modules: math  random  string  sys  os  time  turtle\n");
  py_puts("Type code. Empty line executes a block. 'exit' to quit.\n\n");

  py_vars_clear();
  static char src[4096];
  static char line[200];
  int src_len = 0, in_block = 0;

  while (1) {
    terminal_set_color(0x0E);
    py_puts(in_block ? "... " : ">>> ");
    terminal_set_color(0x07);
    int li = 0;
    while (li < 199) {
      char c = keyboard_getchar();
      if (c == '\n' || c == '\r') {
        terminal_putchar('\n');
        break;
      }
      if (c == '\b') {
        if (li > 0) {
          li--;
          terminal_backspace();
        }
        continue;
      }
      if (c >= ' ' && c <= '~') {
        line[li++] = c;
        terminal_putchar(c);
      }
    }
    line[li] = 0;
    while (li > 0 && line[li - 1] == ' ') {
      li--;
      line[li] = 0;
    }
    if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0 ||
        strcmp(line, "exit()") == 0 || strcmp(line, "quit()") == 0)
      break;
    bool empty = (li == 0), ends_colon = (li > 0 && line[li - 1] == ':');
    if (empty) {
      if (src_len > 0) {
        src[src_len] = 0;
        py_run_src(src, true);
        src_len = 0;
        in_block = 0;
        if (sys_exit_called)
          break;
      }
    } else {
      if (src_len + li + 2 < 4096) {
        int i;
        for (i = 0; i < li; i++)
          src[src_len++] = line[i];
        src[src_len++] = '\n';
        src[src_len] = 0;
      }
      if (ends_colon)
        in_block++;
      else if (!in_block) {
        py_run_src(src, true);
        src_len = 0;
        if (sys_exit_called)
          break;
      }
    }
  }
  terminal_set_color(0x0B);
  py_puts("Python REPL closed.\n");
  terminal_set_color(0x07);
}
