/*
 * NovexOS - python.c  (v2.0 — built-in modules)
 * Python 3 subset interpreter, bare-metal x86_64, zero libc.
 *
 * Built-in modules (no import required, always available):
 *   math    — sqrt, pow, sin, cos, abs, gcd, pi, e, tau
 *   random  — randint, randrange, seed
 *   time    — time, sleep, ticks
 *   os      — uname, name, sep
 *   sys     — version, platform, maxsize, exit
 *   turtle  — full graphics via VBE framebuffer (Bresenham + sin/cos LUT)
 *
 * Language features:
 *   Types    : int, str, bool, None
 *   Ops      : + - * // / % ** unary-minus
 *   Strings  : concatenation, repetition, escape sequences
 *   Compare  : == != < > <= >=
 *   Boolean  : and or not
 *   Assign   : = += -= *=
 *   Control  : if/elif/else, while, for..in..range(), break, continue, pass
 *   Builtins : print, input, len, str, int, bool, abs, chr, ord, max, min
 *   Import   : import X  (no-op — all modules always accessible)
 */

#include "python.h"
#include "fb.h"
#include "keyboard.h"
#include "ramfs.h"
#include "string.h"
#include "timer.h"
#include "types.h"
#include "vbe.h"

/* ============================================================
 *  External kernel I/O
 * ============================================================ */
extern void terminal_putchar(char c);
extern void terminal_writestring(const char *s);
extern void terminal_set_color(uint8_t col);
extern void terminal_backspace(void);

/* ============================================================
 *  Output helpers
 * ============================================================ */
static void py_putc(char c) { terminal_putchar(c); }
static void py_puts(const char *s) { terminal_writestring(s); }

static void py_puti(int32_t n) {
  if (n == 0) {
    py_putc('0');
    return;
  }
  char tmp[12];
  int ti = 0;
  bool neg = (n < 0);
  if (neg)
    n = -n;
  while (n > 0 && ti < 11) {
    tmp[ti++] = (char)('0' + n % 10);
    n /= 10;
  }
  if (neg)
    py_putc('-');
  for (int j = ti - 1; j >= 0; j--)
    py_putc(tmp[j]);
}

static void py_error(const char *msg) {
  terminal_set_color(0x04);
  py_puts("\n[PythonError] ");
  py_puts(msg);
  py_putc('\n');
  terminal_set_color(0x07);
}

/* ============================================================
 *  Value type
 * ============================================================ */
#define PY_INT 0
#define PY_STR 1
#define PY_BOOL 2
#define PY_NONE 3
#define PY_ERR 4

#define PY_SMAX 120

typedef struct {
  uint8_t t;
  int32_t i;
  char s[PY_SMAX];
} PyVal;

static PyVal mkint(int32_t n) {
  PyVal v;
  v.t = PY_INT;
  v.i = n;
  v.s[0] = 0;
  return v;
}
static PyVal mkbool(int b) {
  PyVal v;
  v.t = PY_BOOL;
  v.i = b ? 1 : 0;
  v.s[0] = 0;
  return v;
}
static PyVal mknone(void) {
  PyVal v;
  v.t = PY_NONE;
  v.i = 0;
  v.s[0] = 0;
  return v;
}
static PyVal mkerr(void) {
  PyVal v;
  v.t = PY_ERR;
  v.i = 0;
  v.s[0] = 0;
  return v;
}
static PyVal mkstr(const char *src) {
  PyVal v;
  v.t = PY_STR;
  v.i = 0;
  int i = 0;
  while (src[i] && i < PY_SMAX - 1) {
    v.s[i] = src[i];
    i++;
  }
  v.s[i] = 0;
  return v;
}

static int py_truthy(PyVal v) {
  if (v.t == PY_INT || v.t == PY_BOOL)
    return v.i != 0;
  if (v.t == PY_STR)
    return v.s[0] != 0;
  return 0;
}

static void val_to_buf(PyVal v, char *buf, int maxlen) {
  if (maxlen <= 0)
    return;
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
    if (neg && bi < maxlen - 1)
      buf[bi++] = '-';
    for (int j = ti - 1; j >= 0 && bi < maxlen - 1; j--)
      buf[bi++] = tmp[j];
    buf[bi] = 0;
  } else if (v.t == PY_BOOL) {
    const char *s = v.i ? "True" : "False";
    int i = 0;
    while (s[i] && i < maxlen - 1) {
      buf[i] = s[i];
      i++;
    }
    buf[i] = 0;
  } else if (v.t == PY_NONE) {
    const char *s = "None";
    int i = 0;
    while (s[i] && i < maxlen - 1) {
      buf[i] = s[i];
      i++;
    }
    buf[i] = 0;
  } else if (v.t == PY_STR) {
    int i = 0;
    while (v.s[i] && i < maxlen - 1) {
      buf[i] = v.s[i];
      i++;
    }
    buf[i] = 0;
  } else {
    buf[0] = '?';
    buf[1] = 0;
  }
}

static void py_val_print(PyVal v) {
  if (v.t == PY_INT)
    py_puti(v.i);
  else if (v.t == PY_BOOL)
    py_puts(v.i ? "True" : "False");
  else if (v.t == PY_NONE)
    py_puts("None");
  else if (v.t == PY_STR)
    py_puts(v.s);
}

/* ============================================================
 *  Variable store
 * ============================================================ */
#define PY_MAXVARS 32
static char pyn[PY_MAXVARS][32];
static PyVal pyv[PY_MAXVARS];
static int pync = 0;

static void py_vars_clear(void) { pync = 0; }

static PyVal py_var_get(const char *name) {
  for (int i = 0; i < pync; i++)
    if (strcmp(pyn[i], name) == 0)
      return pyv[i];
  char msg[56];
  const char *pfx = "name not defined: ";
  int k = 0;
  while (pfx[k] && k < 55) {
    msg[k] = pfx[k];
    k++;
  }
  int j = 0;
  while (name[j] && k < 55) {
    msg[k] = name[j];
    k++;
    j++;
  }
  msg[k] = 0;
  py_error(msg);
  return mkerr();
}

static void py_var_set(const char *name, PyVal val) {
  for (int i = 0; i < pync; i++) {
    if (strcmp(pyn[i], name) == 0) {
      pyv[i] = val;
      return;
    }
  }
  if (pync < PY_MAXVARS) {
    int i = 0;
    while (name[i] && i < 31) {
      pyn[pync][i] = name[i];
      i++;
    }
    pyn[pync][i] = 0;
    pyv[pync] = val;
    pync++;
  } else {
    py_error("too many variables");
  }
}

/* ============================================================
 *  Source loader
 * ============================================================ */
#define PY_MAXLINES 256
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

/* ============================================================
 *  Sin/cos lookup table (0..90 deg, values * 1000)
 * ============================================================ */
static const int16_t sin_lut[91] = {
    0,   17,  35,  52,  70,  87,  105, 122, 139, 156, 174, 191,  208,
    225, 242, 259, 276, 292, 309, 326, 342, 358, 375, 391, 407,  423,
    438, 454, 469, 485, 500, 515, 530, 545, 559, 574, 588, 602,  616,
    629, 643, 656, 669, 682, 695, 707, 719, 731, 743, 755, 766,  777,
    788, 799, 809, 819, 829, 839, 848, 857, 866, 875, 883, 891,  899,
    906, 914, 921, 927, 934, 940, 946, 951, 956, 961, 966, 970,  974,
    978, 982, 985, 988, 990, 993, 995, 996, 998, 999, 999, 1000, 1000};

/* Returns sin(deg) * 1000  (integer, exact for 0..359) */
static int32_t py_sin(int32_t deg) {
  deg = deg % 360;
  if (deg < 0)
    deg += 360;
  if (deg <= 90)
    return (int32_t)sin_lut[deg];
  if (deg <= 180)
    return (int32_t)sin_lut[180 - deg];
  if (deg <= 270)
    return -(int32_t)sin_lut[deg - 180];
  return -(int32_t)sin_lut[360 - deg];
}
static int32_t py_cos(int32_t deg) { return py_sin(deg + 90); }

/* Integer square root (Newton's method) */
static int32_t py_isqrt(int32_t n) {
  if (n <= 0)
    return 0;
  int32_t x = n, y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + n / x) / 2;
  }
  return x;
}

/* Integer power */
static int32_t py_ipow(int32_t base, int32_t exp) {
  if (exp < 0)
    return 0;
  int32_t r = 1;
  while (exp-- > 0)
    r *= base;
  return r;
}

/* ============================================================
 *  LCG random number generator
 * ============================================================ */
static uint32_t rng_state = 0xDEAD1337UL;

static uint32_t py_rand(void) {
  rng_state = rng_state * 1664525UL + 1013904223UL;
  return rng_state;
}

/* ============================================================
 *  Color name resolver
 * ============================================================ */
static uint32_t parse_color_name(const char *name) {
  if (strcmp(name, "red") == 0)
    return 0xFFFF3030;
  if (strcmp(name, "green") == 0)
    return 0xFF30C030;
  if (strcmp(name, "blue") == 0)
    return 0xFF3060FF;
  if (strcmp(name, "white") == 0)
    return 0xFFFFFFFF;
  if (strcmp(name, "black") == 0)
    return 0xFF000000;
  if (strcmp(name, "yellow") == 0)
    return 0xFFFFFF00;
  if (strcmp(name, "orange") == 0)
    return 0xFFFF8800;
  if (strcmp(name, "purple") == 0)
    return 0xFF9900FF;
  if (strcmp(name, "cyan") == 0)
    return 0xFF00FFFF;
  if (strcmp(name, "magenta") == 0)
    return 0xFFFF00FF;
  if (strcmp(name, "gray") == 0)
    return 0xFF888888;
  if (strcmp(name, "grey") == 0)
    return 0xFF888888;
  if (strcmp(name, "pink") == 0)
    return 0xFFFF69B4;
  if (strcmp(name, "brown") == 0)
    return 0xFF8B4513;
  if (strcmp(name, "gold") == 0)
    return 0xFFFFD700;
  if (strcmp(name, "lime") == 0)
    return 0xFF00FF00;
  if (strcmp(name, "navy") == 0)
    return 0xFF000080;
  if (strcmp(name, "teal") == 0)
    return 0xFF008080;
  if (strcmp(name, "silver") == 0)
    return 0xFFC0C0C0;
  return 0xFFFFFFFF;
}

/* ============================================================
 *  Turtle state
 * ============================================================ */
static bool tu_active = false;
static int32_t tu_x = 0; /* screen coords */
static int32_t tu_y = 0;
static int32_t tu_cx = 0; /* center (home) */
static int32_t tu_cy = 0;
static int32_t tu_angle = 0; /* 0=East, 90=North (turtle convention) */
static bool tu_pen = true;
static uint32_t tu_color = 0xFFFFFFFF;
static uint32_t tu_bgcolor = 0xFF000000;
static int32_t tu_width = 1;

/* Bresenham line */
static void turtle_bline(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                         uint32_t col) {
  int32_t dx = x1 - x0;
  if (dx < 0)
    dx = -dx;
  int32_t dy = y1 - y0;
  if (dy < 0)
    dy = -dy;
  int32_t sx = (x0 < x1) ? 1 : -1;
  int32_t sy = (y0 < y1) ? 1 : -1;
  int32_t err = dx - dy;
  for (;;) {
    fb_put_pixel(x0, y0, col);
    if (x0 == x1 && y0 == y1)
      break;
    int32_t e2 = 2 * err;
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

/* Thick line via parallel offset lines */
static void turtle_thick_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  turtle_bline(x0, y0, x1, y1, tu_color);
  for (int32_t w = 1; w < tu_width; w++) {
    turtle_bline(x0 + w, y0, x1 + w, y1, tu_color);
    turtle_bline(x0, y0 + w, x1, y1 + w, tu_color);
    turtle_bline(x0 - w, y0, x1 - w, y1, tu_color);
    turtle_bline(x0, y0 - w, x1, y1 - w, tu_color);
  }
}

/* Filled circle for dot() */
static void fill_circle(int32_t cx, int32_t cy, int32_t r, uint32_t col) {
  for (int32_t dy = -r; dy <= r; dy++) {
    int32_t r2 = r * r - dy * dy;
    int32_t dx = 0;
    while ((dx + 1) * (dx + 1) <= r2)
      dx++;
    fb_hline(cx - dx, cy + dy, 2 * dx + 1, col);
  }
}

/* Initialize turtle (auto-switch to VBE if needed) */
static void turtle_ensure_init(void) {
  if (tu_active)
    return;
  if (!vbe_is_available()) {
    vbe_init(NULL);
    fb_init(vbe_get_framebuffer(), vbe_get_width(), vbe_get_height(),
            vbe_get_pitch());
  }
  uint32_t w = fb_get_width();
  uint32_t h = fb_get_height();
  tu_cx = (int32_t)(w / 2);
  tu_cy = (int32_t)(h / 2);
  tu_x = tu_cx;
  tu_y = tu_cy;
  tu_angle = 0;
  tu_pen = true;
  tu_color = 0xFFFFFFFF;
  tu_bgcolor = 0xFF000000;
  tu_width = 1;
  fb_clear(tu_bgcolor);
  fb_swap();
  tu_active = true;
}

/* Move forward (negative = backward) */
static void turtle_do_forward(int32_t dist) {
  int32_t nx = tu_x + py_cos(tu_angle) * dist / 1000;
  int32_t ny = tu_y - py_sin(tu_angle) * dist / 1000; /* flip Y */
  if (tu_pen)
    turtle_thick_line(tu_x, tu_y, nx, ny);
  tu_x = nx;
  tu_y = ny;
}

/* Draw circle arc */
static void turtle_do_circle(int32_t radius, int32_t extent) {
  int32_t steps = 36;
  /* arc length per step = 2*pi*r * (extent/360) / steps */
  int32_t step_len = (int32_t)((int64_t)6283 * radius * extent /
                               ((int64_t)360 * 1000 * steps));
  int32_t step_ang = extent / steps;
  for (int32_t i = 0; i < steps; i++) {
    turtle_do_forward(step_len);
    tu_angle += step_ang;
    while (tu_angle >= 360)
      tu_angle -= 360;
    while (tu_angle < 0)
      tu_angle += 360;
  }
}

/* ============================================================
 *  Execution flags (also used by sys.exit)
 * ============================================================ */
static bool exec_error;
static bool exec_break;
static bool exec_continue;
static bool exec_repl;

/* ============================================================
 *  Forward declarations
 * ============================================================ */
static PyVal parse_expr(void);
static PyVal dispatch_module(const char *mod, const char *meth, PyVal *a,
                             int n);
static PyVal get_module_attr(const char *mod, const char *attr);
static PyVal call_builtin(const char *fname);

/* ============================================================
 *  Lexer
 * ============================================================ */
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

/* ============================================================
 *  Recursive-descent parser
 * ============================================================ */
static PyVal parse_or(void);
static PyVal parse_and(void);
static PyVal parse_not_expr(void);
static PyVal parse_cmp(void);
static PyVal parse_add(void);
static PyVal parse_mul(void);
static PyVal parse_unary(void);
static PyVal parse_primary(void);

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
    PyVal v = parse_not_expr();
    return mkbool(!py_truthy(v));
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
        v = mkbool(v.i == r.i);
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
        v = mkbool(v.i != r.i);
    } else if (strncmp(lp, "<=", 2) == 0) {
      lp += 2;
      PyVal r = parse_add();
      v = mkbool(v.i <= r.i);
    } else if (strncmp(lp, ">=", 2) == 0) {
      lp += 2;
      PyVal r = parse_add();
      v = mkbool(v.i >= r.i);
    } else if (*lp == '<' && lp[1] != '<' && lp[1] != '=') {
      lp++;
      PyVal r = parse_add();
      v = mkbool(v.i < r.i);
    } else if (*lp == '>' && lp[1] != '>' && lp[1] != '=') {
      lp++;
      PyVal r = parse_add();
      v = mkbool(v.i > r.i);
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
      } else {
        v = mkint(v.i + r.i);
      }
    } else if (*lp == '-' && lp[1] != '=') {
      lp++;
      PyVal r = parse_mul();
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
    if (lp[0] == '*' && lp[1] == '*') {
      lp += 2;
      PyVal r = parse_unary();
      v = mkint(py_ipow(v.i, r.i));
    } else if (lp[0] == '*' && lp[1] != '=') {
      lp++;
      PyVal r = parse_unary();
      if (v.t == PY_STR && r.t == PY_INT) {
        char out[PY_SMAX];
        int oi = 0;
        int len = (int)strlen(v.s);
        for (int k = 0; k < r.i && oi + len < PY_SMAX - 1; k++)
          for (int j = 0; j < len && oi < PY_SMAX - 1; j++)
            out[oi++] = v.s[j];
        out[oi] = 0;
        v = mkstr(out);
      } else if (v.t == PY_INT && r.t == PY_STR) {
        char out[PY_SMAX];
        int oi = 0;
        int len = (int)strlen(r.s);
        for (int k = 0; k < v.i && oi + len < PY_SMAX - 1; k++)
          for (int j = 0; j < len && oi < PY_SMAX - 1; j++)
            out[oi++] = r.s[j];
        out[oi] = 0;
        v = mkstr(out);
      } else {
        v = mkint(v.i * r.i);
      }
    } else if (lp[0] == '/' && lp[1] == '/') {
      lp += 2;
      PyVal r = parse_unary();
      if (r.i == 0) {
        py_error("ZeroDivisionError");
        return mkerr();
      }
      int32_t q = v.i / r.i;
      if ((v.i ^ r.i) < 0 && q * r.i != v.i)
        q--;
      v = mkint(q);
    } else if (lp[0] == '/' && lp[1] != '/') {
      lp++;
      PyVal r = parse_unary();
      if (r.i == 0) {
        py_error("ZeroDivisionError");
        return mkerr();
      }
      v = mkint(v.i / r.i);
    } else if (lp[0] == '%' && lp[1] != '=') {
      lp++;
      PyVal r = parse_unary();
      if (r.i == 0) {
        py_error("ZeroDivisionError");
        return mkerr();
      }
      v = mkint(v.i % r.i);
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
    return mkint(-v.i);
  }
  if (*lp == '+') {
    lp++;
    return parse_unary();
  }
  return parse_primary();
}

/* ============================================================
 *  Primary: literals, names, calls, module.method
 * ============================================================ */
static PyVal parse_primary(void) {
  lskip();

  /* Integer literal */
  if (lis_digit(*lp)) {
    int32_t n = 0;
    while (lis_digit(*lp)) {
      n = n * 10 + (*lp - '0');
      lp++;
    }
    return mkint(n);
  }

  /* String literal */
  if (*lp == '"' || *lp == '\'') {
    char q = *lp++;
    char buf[PY_SMAX];
    int bi = 0;
    while (*lp && *lp != q && bi < PY_SMAX - 1) {
      if (*lp == '\\' && lp[1]) {
        lp++;
        if (*lp == 'n')
          buf[bi++] = '\n';
        else if (*lp == 't')
          buf[bi++] = '\t';
        else if (*lp == '\\')
          buf[bi++] = '\\';
        else if (*lp == '"')
          buf[bi++] = '"';
        else if (*lp == '\'')
          buf[bi++] = '\'';
        else
          buf[bi++] = *lp;
        lp++;
      } else {
        buf[bi++] = *lp++;
      }
    }
    if (*lp == q)
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

  /* Name: variable, function, or module.method */
  if (lis_alpha(*lp)) {
    char name[32];
    int ni = 0;
    while (lis_alnum(*lp) && ni < 31)
      name[ni++] = *lp++;
    name[ni] = 0;
    lskip();

    /* module.attr  or  module.method(...) */
    if (*lp == '.') {
      lp++;
      char attr[32];
      int ai = 0;
      lskip();
      while (lis_alnum(*lp) && ai < 31)
        attr[ai++] = *lp++;
      attr[ai] = 0;
      lskip();
      if (*lp == '(') {
        lp++;
        PyVal margs[8];
        int nma = 0;
        lskip();
        while (*lp && *lp != ')' && nma < 8) {
          margs[nma++] = parse_expr();
          lskip();
          if (*lp == ',') {
            lp++;
            lskip();
          }
        }
        if (*lp == ')')
          lp++;
        return dispatch_module(name, attr, margs, nma);
      }
      return get_module_attr(name, attr);
    }

    /* Function call */
    if (*lp == '(') {
      lp++;
      return call_builtin(name);
    }

    /* Variable */
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

/* ============================================================
 *  Built-in functions
 * ============================================================ */
static PyVal call_builtin(const char *fname) {
  lskip();
  PyVal args[8];
  int nargs = 0;
  while (*lp && *lp != ')' && nargs < 8) {
    args[nargs++] = parse_expr();
    lskip();
    if (*lp == ',') {
      lp++;
      lskip();
    }
  }
  if (*lp == ')')
    lp++;

  if (strcmp(fname, "len") == 0) {
    if (nargs == 1 && args[0].t == PY_STR)
      return mkint((int32_t)strlen(args[0].s));
    return mkint(0);
  }
  if (strcmp(fname, "str") == 0) {
    if (nargs == 1) {
      char b[PY_SMAX];
      val_to_buf(args[0], b, PY_SMAX);
      return mkstr(b);
    }
    return mkstr("");
  }
  if (strcmp(fname, "int") == 0) {
    if (nargs == 1) {
      if (args[0].t == PY_INT || args[0].t == PY_BOOL)
        return mkint(args[0].i);
      if (args[0].t == PY_STR) {
        const char *p = args[0].s;
        while (*p == ' ')
          p++;
        bool neg = (*p == '-');
        if (*p == '-' || *p == '+')
          p++;
        int32_t n = 0;
        while (*p >= '0' && *p <= '9')
          n = n * 10 + (*p++ - '0');
        return mkint(neg ? -n : n);
      }
    }
    return mkint(0);
  }
  if (strcmp(fname, "bool") == 0) {
    return nargs == 1 ? mkbool(py_truthy(args[0])) : mkbool(0);
  }
  if (strcmp(fname, "abs") == 0) {
    if (nargs == 1 && (args[0].t == PY_INT || args[0].t == PY_BOOL))
      return mkint(args[0].i < 0 ? -args[0].i : args[0].i);
    return mkint(0);
  }
  if (strcmp(fname, "chr") == 0) {
    if (nargs == 1 && args[0].t == PY_INT) {
      char s[2] = {(char)(args[0].i & 0x7F), 0};
      return mkstr(s);
    }
    return mkstr("");
  }
  if (strcmp(fname, "ord") == 0) {
    if (nargs == 1 && args[0].t == PY_STR)
      return mkint((int32_t)(unsigned char)args[0].s[0]);
    return mkint(0);
  }
  if (strcmp(fname, "max") == 0) {
    if (nargs >= 1) {
      int32_t m = args[0].i;
      for (int i = 1; i < nargs; i++)
        if (args[i].i > m)
          m = args[i].i;
      return mkint(m);
    }
    return mkint(0);
  }
  if (strcmp(fname, "min") == 0) {
    if (nargs >= 1) {
      int32_t m = args[0].i;
      for (int i = 1; i < nargs; i++)
        if (args[i].i < m)
          m = args[i].i;
      return mkint(m);
    }
    return mkint(0);
  }
  if (strcmp(fname, "print") == 0) {
    for (int i = 0; i < nargs; i++) {
      if (i > 0)
        py_putc(' ');
      py_val_print(args[i]);
    }
    py_putc('\n');
    return mknone();
  }
  if (strcmp(fname, "input") == 0) {
    if (nargs > 0)
      py_val_print(args[0]);
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
  if (strcmp(fname, "range") == 0) {
    py_error("range() only valid in for..in..range()");
    return mkerr();
  }

  char msg[56] = "unknown function: ";
  int k = 18;
  const char *f = fname;
  while (*f && k < 55) {
    msg[k++] = *f++;
  }
  msg[k] = 0;
  py_error(msg);
  return mkerr();
}

/* ============================================================
 *  Module: math
 * ============================================================ */
static PyVal mod_math(const char *meth, PyVal *a, int n) {
  if (strcmp(meth, "sqrt") == 0 && n == 1)
    return mkint(py_isqrt(a[0].i));
  if (strcmp(meth, "pow") == 0 && n == 2)
    return mkint(py_ipow(a[0].i, a[1].i));
  if (strcmp(meth, "abs") == 0 && n == 1)
    return mkint(a[0].i < 0 ? -a[0].i : a[0].i);
  if (strcmp(meth, "sin") == 0 && n == 1)
    return mkint(py_sin(a[0].i));
  if (strcmp(meth, "cos") == 0 && n == 1)
    return mkint(py_cos(a[0].i));
  if (strcmp(meth, "floor") == 0 && n == 1)
    return mkint(a[0].i);
  if (strcmp(meth, "ceil") == 0 && n == 1)
    return mkint(a[0].i);
  if (strcmp(meth, "gcd") == 0 && n == 2) {
    int32_t x = a[0].i < 0 ? -a[0].i : a[0].i;
    int32_t y = a[1].i < 0 ? -a[1].i : a[1].i;
    while (y) {
      int32_t t = y;
      y = x % y;
      x = t;
    }
    return mkint(x);
  }
  py_error("math: unknown function");
  return mkerr();
}

/* ============================================================
 *  Module: random
 * ============================================================ */
static PyVal mod_random(const char *meth, PyVal *a, int n) {
  if (strcmp(meth, "seed") == 0 && n == 1) {
    rng_state = (uint32_t)a[0].i ^ 0xBEEF0000UL;
    return mknone();
  }
  if (strcmp(meth, "randint") == 0 && n == 2) {
    int32_t lo = a[0].i, hi = a[1].i;
    if (hi < lo) {
      int32_t t = lo;
      lo = hi;
      hi = t;
    }
    int32_t range = hi - lo + 1;
    return mkint(lo + (int32_t)((py_rand() >> 4) % (uint32_t)range));
  }
  if (strcmp(meth, "randrange") == 0) {
    int32_t lo = 0, hi = 1, step = 1;
    if (n == 1) {
      hi = a[0].i;
    } else if (n == 2) {
      lo = a[0].i;
      hi = a[1].i;
    } else if (n >= 3) {
      lo = a[0].i;
      hi = a[1].i;
      step = a[2].i;
    }
    if (step == 0) {
      py_error("randrange() step cannot be zero");
      return mkerr();
    }
    int32_t count = (hi - lo + step - 1) / step;
    if (count <= 0)
      return mkint(lo);
    return mkint(lo + step * (int32_t)((py_rand() >> 4) % (uint32_t)count));
  }
  py_error("random: unknown function");
  return mkerr();
}

/* ============================================================
 *  Module: time
 * ============================================================ */
static PyVal mod_time(const char *meth, PyVal *a, int n) {
  if (strcmp(meth, "time") == 0)
    return mkint((int32_t)timer_get_uptime_seconds());
  if (strcmp(meth, "ticks") == 0)
    return mkint((int32_t)timer_get_ticks());
  if (strcmp(meth, "sleep") == 0 && n == 1) {
    timer_sleep((uint32_t)a[0].i * 1000u); /* seconds → ticks @ 1000 Hz */
    return mknone();
  }
  py_error("time: unknown function");
  return mkerr();
}

/* ============================================================
 *  Module: os
 * ============================================================ */
static PyVal mod_os(const char *meth, PyVal *a, int n) {
  (void)a;
  (void)n;
  if (strcmp(meth, "uname") == 0)
    return mkstr("NovexOS v0.7.2 x86_64");
  if (strcmp(meth, "getenv") == 0)
    return mkstr("");
  py_error("os: unknown function");
  return mkerr();
}

/* ============================================================
 *  Module: sys
 * ============================================================ */
static PyVal mod_sys(const char *meth, PyVal *a, int n) {
  (void)a;
  (void)n;
  if (strcmp(meth, "exit") == 0) {
    exec_error = true;
    return mknone();
  }
  py_error("sys: unknown function");
  return mkerr();
}

/* ============================================================
 *  Module: turtle
 * ============================================================ */
static PyVal mod_turtle(const char *meth, PyVal *a, int n) {
  turtle_ensure_init();

  /* --- Movement --- */
  if ((strcmp(meth, "forward") == 0 || strcmp(meth, "fd") == 0) && n == 1) {
    turtle_do_forward(a[0].i);
    fb_swap();
    return mknone();
  }
  if ((strcmp(meth, "backward") == 0 || strcmp(meth, "bk") == 0 ||
       strcmp(meth, "back") == 0) &&
      n == 1) {
    turtle_do_forward(-a[0].i);
    fb_swap();
    return mknone();
  }

  /* --- Rotation --- */
  if ((strcmp(meth, "right") == 0 || strcmp(meth, "rt") == 0) && n == 1) {
    tu_angle -= a[0].i;
    while (tu_angle < 0)
      tu_angle += 360;
    while (tu_angle >= 360)
      tu_angle -= 360;
    return mknone();
  }
  if ((strcmp(meth, "left") == 0 || strcmp(meth, "lt") == 0) && n == 1) {
    tu_angle += a[0].i;
    while (tu_angle < 0)
      tu_angle += 360;
    while (tu_angle >= 360)
      tu_angle -= 360;
    return mknone();
  }
  if ((strcmp(meth, "setheading") == 0 || strcmp(meth, "seth") == 0) &&
      n == 1) {
    tu_angle = a[0].i % 360;
    if (tu_angle < 0)
      tu_angle += 360;
    return mknone();
  }

  /* --- Positioning --- */
  if ((strcmp(meth, "goto") == 0 || strcmp(meth, "setpos") == 0 ||
       strcmp(meth, "setposition") == 0) &&
      n == 2) {
    int32_t nx = tu_cx + a[0].i, ny = tu_cy - a[1].i;
    if (tu_pen)
      turtle_thick_line(tu_x, tu_y, nx, ny);
    tu_x = nx;
    tu_y = ny;
    fb_swap();
    return mknone();
  }
  if (strcmp(meth, "setx") == 0 && n == 1) {
    int32_t nx = tu_cx + a[0].i;
    if (tu_pen)
      turtle_thick_line(tu_x, tu_y, nx, tu_y);
    tu_x = nx;
    fb_swap();
    return mknone();
  }
  if (strcmp(meth, "sety") == 0 && n == 1) {
    int32_t ny = tu_cy - a[0].i;
    if (tu_pen)
      turtle_thick_line(tu_x, tu_y, tu_x, ny);
    tu_y = ny;
    fb_swap();
    return mknone();
  }
  if (strcmp(meth, "home") == 0) {
    if (tu_pen)
      turtle_thick_line(tu_x, tu_y, tu_cx, tu_cy);
    tu_x = tu_cx;
    tu_y = tu_cy;
    tu_angle = 0;
    fb_swap();
    return mknone();
  }

  /* --- Pen --- */
  if (strcmp(meth, "penup") == 0 || strcmp(meth, "pu") == 0 ||
      strcmp(meth, "up") == 0) {
    tu_pen = false;
    return mknone();
  }
  if (strcmp(meth, "pendown") == 0 || strcmp(meth, "pd") == 0 ||
      strcmp(meth, "down") == 0) {
    tu_pen = true;
    return mknone();
  }
  if (strcmp(meth, "isdown") == 0)
    return mkbool(tu_pen ? 1 : 0);
  if ((strcmp(meth, "pensize") == 0 || strcmp(meth, "width") == 0) && n == 1) {
    tu_width = a[0].i;
    if (tu_width < 1)
      tu_width = 1;
    if (tu_width > 16)
      tu_width = 16;
    return mknone();
  }

  /* --- Color --- */
  if (strcmp(meth, "pencolor") == 0 || strcmp(meth, "color") == 0) {
    if (n == 1 && a[0].t == PY_STR) {
      tu_color = parse_color_name(a[0].s);
      return mknone();
    }
    if (n == 3) {
      uint32_t r = (uint32_t)a[0].i & 0xFF;
      uint32_t g = (uint32_t)a[1].i & 0xFF;
      uint32_t b = (uint32_t)a[2].i & 0xFF;
      tu_color = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
    return mknone();
  }
  if (strcmp(meth, "bgcolor") == 0) {
    if (n == 1 && a[0].t == PY_STR)
      tu_bgcolor = parse_color_name(a[0].s);
    else if (n == 3) {
      uint32_t r = (uint32_t)a[0].i & 0xFF;
      uint32_t g = (uint32_t)a[1].i & 0xFF;
      uint32_t b = (uint32_t)a[2].i & 0xFF;
      tu_bgcolor = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
    fb_clear(tu_bgcolor);
    fb_swap();
    return mknone();
  }

  /* --- Drawing primitives --- */
  if (strcmp(meth, "dot") == 0) {
    int32_t sz = (n >= 1) ? a[0].i / 2 : 5;
    fill_circle(tu_x, tu_y, sz, tu_color);
    fb_swap();
    return mknone();
  }
  if (strcmp(meth, "circle") == 0 && n >= 1) {
    int32_t r = a[0].i;
    int32_t extent = (n >= 2) ? a[1].i : 360;
    turtle_do_circle(r, extent);
    fb_swap();
    return mknone();
  }
  if (strcmp(meth, "write") == 0 && n >= 1) {
    char buf[PY_SMAX];
    val_to_buf(a[0], buf, PY_SMAX);
    fb_draw_string(tu_x, tu_y, buf, tu_color, 0x00000000);
    fb_swap();
    return mknone();
  }
  if (strcmp(meth, "stamp") == 0) {
    /* Draw a small triangle (arrow) pointing in heading direction */
    int32_t sz = 10;
    int32_t fx = tu_x + py_cos(tu_angle) * sz / 1000;
    int32_t fy = tu_y - py_sin(tu_angle) * sz / 1000;
    int32_t lx = tu_x + py_cos(tu_angle + 135) * sz / 1000;
    int32_t ly = tu_y - py_sin(tu_angle + 135) * sz / 1000;
    int32_t rx = tu_x + py_cos(tu_angle - 135) * sz / 1000;
    int32_t ry = tu_y - py_sin(tu_angle - 135) * sz / 1000;
    turtle_bline(fx, fy, lx, ly, tu_color);
    turtle_bline(fx, fy, rx, ry, tu_color);
    turtle_bline(lx, ly, rx, ry, tu_color);
    fb_swap();
    return mknone();
  }

  /* --- Screen control --- */
  if (strcmp(meth, "clear") == 0 || strcmp(meth, "reset") == 0) {
    fb_clear(tu_bgcolor);
    if (strcmp(meth, "reset") == 0) {
      tu_x = tu_cx;
      tu_y = tu_cy;
      tu_angle = 0;
      tu_pen = true;
    }
    fb_swap();
    return mknone();
  }
  if (strcmp(meth, "update") == 0 || strcmp(meth, "refresh") == 0) {
    fb_swap();
    return mknone();
  }

  /* --- Queries --- */
  if (strcmp(meth, "xcor") == 0 || strcmp(meth, "getx") == 0)
    return mkint(tu_x - tu_cx);
  if (strcmp(meth, "ycor") == 0 || strcmp(meth, "gety") == 0)
    return mkint(tu_cy - tu_y);
  if (strcmp(meth, "heading") == 0)
    return mkint(tu_angle);
  if (strcmp(meth, "distance") == 0 && n == 2) {
    int32_t dx = a[0].i - (tu_x - tu_cx);
    int32_t dy = a[1].i - (tu_cy - tu_y);
    return mkint(py_isqrt(dx * dx + dy * dy));
  }

  /* --- No-ops for Python compatibility --- */
  if (strcmp(meth, "speed") == 0 || strcmp(meth, "hideturtle") == 0 ||
      strcmp(meth, "showturtle") == 0 || strcmp(meth, "ht") == 0 ||
      strcmp(meth, "st") == 0 || strcmp(meth, "tracer") == 0 ||
      strcmp(meth, "title") == 0 || strcmp(meth, "setup") == 0 ||
      strcmp(meth, "fillcolor") == 0 || strcmp(meth, "begin_fill") == 0 ||
      strcmp(meth, "end_fill") == 0)
    return mknone();

  /* --- done() / mainloop(): show "press any key", wait, then exit --- */
  if (strcmp(meth, "done") == 0 || strcmp(meth, "mainloop") == 0 ||
      strcmp(meth, "exitonclick") == 0) {
    uint32_t sw = fb_get_width(), sh = fb_get_height();
    fb_draw_string((int32_t)(sw / 2 - 110), (int32_t)(sh - 22),
                   "[ Press any key to close ]", 0xFF888888, 0x00000000);
    fb_swap();
    keyboard_getchar();
    tu_active = false; /* reset for next turtle session */
    return mknone();
  }

  py_error("turtle: unknown function");
  return mkerr();
}

/* ============================================================
 *  Module attribute access  (module.CONSTANT)
 * ============================================================ */
static PyVal get_module_attr(const char *mod, const char *attr) {
  if (strcmp(mod, "math") == 0) {
    if (strcmp(attr, "pi") == 0)
      return mkint(3141); /* pi  * 1000 */
    if (strcmp(attr, "e") == 0)
      return mkint(2718); /* e   * 1000 */
    if (strcmp(attr, "tau") == 0)
      return mkint(6283); /* 2pi * 1000 */
  }
  if (strcmp(mod, "sys") == 0) {
    if (strcmp(attr, "version") == 0)
      return mkstr("NovexPy 2.0 (Python 3 subset)");
    if (strcmp(attr, "platform") == 0)
      return mkstr("novexos");
    if (strcmp(attr, "maxsize") == 0)
      return mkint(2147483647);
  }
  if (strcmp(mod, "os") == 0) {
    if (strcmp(attr, "name") == 0)
      return mkstr("novex");
    if (strcmp(attr, "sep") == 0)
      return mkstr("/");
  }
  if (strcmp(mod, "turtle") == 0) {
    turtle_ensure_init();
    if (strcmp(attr, "x") == 0)
      return mkint(tu_x - tu_cx);
    if (strcmp(attr, "y") == 0)
      return mkint(tu_cy - tu_y);
  }
  char msg[64];
  const char *pfx = "unknown attribute: ";
  int k = 0;
  while (pfx[k] && k < 63) {
    msg[k] = pfx[k];
    k++;
  }
  int j = 0;
  while (mod[j] && k < 63) {
    msg[k] = mod[j];
    k++;
    j++;
  }
  if (k < 63) {
    msg[k] = '.';
    k++;
  }
  j = 0;
  while (attr[j] && k < 63) {
    msg[k] = attr[j];
    k++;
    j++;
  }
  msg[k] = 0;
  py_error(msg);
  return mkerr();
}

/* ============================================================
 *  Module dispatcher
 * ============================================================ */
static PyVal dispatch_module(const char *mod, const char *meth, PyVal *a,
                             int n) {
  if (strcmp(mod, "math") == 0)
    return mod_math(meth, a, n);
  if (strcmp(mod, "random") == 0)
    return mod_random(meth, a, n);
  if (strcmp(mod, "time") == 0)
    return mod_time(meth, a, n);
  if (strcmp(mod, "os") == 0)
    return mod_os(meth, a, n);
  if (strcmp(mod, "sys") == 0)
    return mod_sys(meth, a, n);
  if (strcmp(mod, "turtle") == 0)
    return mod_turtle(meth, a, n);
  char msg[48] = "unknown module: ";
  int k = 16;
  const char *m = mod;
  while (*m && k < 47) {
    msg[k++] = *m++;
  }
  msg[k] = 0;
  py_error(msg);
  return mkerr();
}

/* ============================================================
 *  Statement helpers
 * ============================================================ */
static void strip_colon(const char *src, char *dst, int maxlen) {
  int len = (int)strlen(src);
  while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\r'))
    len--;
  if (len > 0 && src[len - 1] == ':')
    len--;
  while (len > 0 && src[len - 1] == ' ')
    len--;
  if (len >= maxlen)
    len = maxlen - 1;
  int i;
  for (i = 0; i < len; i++)
    dst[i] = src[i];
  dst[i] = 0;
}

static int skip_block(int pc, int bind) {
  while (pc < pynl && pylines[pc].indent > bind)
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

/* Forward declaration for mutual recursion */
static int exec_block(int pc, int parent_ind);

static int exec_print(int pc) {
  const char *t = pylines[pc].t + 6;
  lp = t;
  lskip();
  if (*lp == ')') {
    py_putc('\n');
    return pc + 1;
  }
  bool first = true;
  while (*lp && *lp != ')') {
    if (strncmp(lp, "sep=", 4) == 0 || strncmp(lp, "end=", 4) == 0) {
      lp += 4;
      parse_primary();
      lskip();
      if (*lp == ',') {
        lp++;
        lskip();
      }
      continue;
    }
    if (!first)
      py_putc(' ');
    first = false;
    PyVal v = parse_expr();
    if (v.t == PY_ERR) {
      exec_error = true;
      break;
    }
    py_val_print(v);
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
  if (py_truthy(cv)) {
    int next = exec_block(pc + 1, ind);
    return skip_if_chain(next, ind);
  }
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
      if (py_truthy(ev)) {
        int after = exec_block(next + 1, ind);
        return skip_if_chain(after, ind);
      }
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
  while (!exec_error) {
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
    if (exec_continue) {
      exec_continue = false;
    }
  }
  return body_end;
}

static int exec_for(int pc) {
  int ind = pylines[pc].indent;
  const char *t = pylines[pc].t + 4;
  char var[32];
  int vi = 0;
  while (lis_alnum(*t) && vi < 31) {
    var[vi++] = *t++;
  }
  var[vi] = 0;
  while (*t == ' ')
    t++;
  if (strncmp(t, "in range(", 9) != 0 && strncmp(t, "in  range(", 10) != 0) {
    py_error("for: only 'for var in range(...)' is supported");
    return skip_block(pc + 1, ind);
  }
  while (*t && *t != '(') {
    t++;
  }
  if (*t == '(')
    t++;
  lp = t;
  PyVal a1 = parse_expr();
  lskip();
  int32_t r_start = 0, r_end = 0, r_step = 1;
  if (*lp == ',') {
    lp++;
    PyVal a2 = parse_expr();
    lskip();
    if (*lp == ',') {
      lp++;
      PyVal a3 = parse_expr();
      r_start = a1.i;
      r_end = a2.i;
      r_step = a3.i;
    } else {
      r_start = a1.i;
      r_end = a2.i;
    }
  } else {
    r_end = a1.i;
  }
  if (r_step == 0) {
    py_error("range() step cannot be zero");
    return skip_block(pc + 1, ind);
  }
  int body_end = skip_block(pc + 1, ind);
  for (int32_t i = r_start; (r_step > 0) ? (i < r_end) : (i > r_end);
       i += r_step) {
    if (exec_error)
      break;
    py_var_set(var, mkint(i));
    exec_block(pc + 1, ind);
    if (exec_break) {
      exec_break = false;
      break;
    }
    if (exec_continue) {
      exec_continue = false;
    }
  }
  return body_end;
}

static bool is_assignment(const char *t, char *varname, int *op,
                          const char **es) {
  if (!lis_alpha(*t))
    return false;
  int vi = 0;
  const char *p = t;
  while (lis_alnum(*p) && vi < 31) {
    varname[vi++] = *p++;
  }
  varname[vi] = 0;
  while (*p == ' ')
    p++;
  if (*p == '=' && p[1] != '=') {
    *op = 0;
    *es = p + 1;
    while (**es == ' ')
      (*es)++;
    return true;
  } else if (*p == '+' && p[1] == '=') {
    *op = 1;
    *es = p + 2;
    while (**es == ' ')
      (*es)++;
    return true;
  } else if (*p == '-' && p[1] == '=') {
    *op = 2;
    *es = p + 2;
    while (**es == ' ')
      (*es)++;
    return true;
  } else if (*p == '*' && p[1] == '=') {
    *op = 3;
    *es = p + 2;
    while (**es == ' ')
      (*es)++;
    return true;
  }
  return false;
}

static int exec_line(int pc) {
  if (exec_error || exec_break || exec_continue)
    return pc + 1;
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

  /* import / from — always no-op */
  if (strncmp(t, "import ", 7) == 0 || strncmp(t, "from ", 5) == 0)
    return pc + 1;

  if (strncmp(t, "if ", 3) == 0)
    return exec_if(pc);
  if (strncmp(t, "while ", 6) == 0)
    return exec_while(pc);
  if (strncmp(t, "for ", 4) == 0)
    return exec_for(pc);

  if (strncmp(t, "print(", 6) == 0 || strcmp(t, "print()") == 0)
    return exec_print(pc);

  if (strncmp(t, "elif ", 5) == 0 || strcmp(t, "else:") == 0 ||
      strncmp(t, "else:", 5) == 0)
    return skip_block(pc + 1, ind);

  /* Assignment */
  {
    char varname[32];
    int op;
    const char *es;
    if (is_assignment(t, varname, &op, &es)) {
      lp = es;
      PyVal val = parse_expr();
      if (val.t == PY_ERR) {
        exec_error = true;
        return pc + 1;
      }
      if (op == 0) {
        py_var_set(varname, val);
      } else {
        PyVal old = py_var_get(varname);
        if (old.t == PY_ERR) {
          exec_error = true;
          return pc + 1;
        }
        if (op == 1) {
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
            py_var_set(varname, mkstr(out));
          } else {
            py_var_set(varname, mkint(old.i + val.i));
          }
        } else if (op == 2) {
          py_var_set(varname, mkint(old.i - val.i));
        } else if (op == 3) {
          py_var_set(varname, mkint(old.i * val.i));
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
    py_val_print(res);
    py_putc('\n');
  }
  return pc + 1;
}

static int exec_block(int pc, int parent_ind) {
  while (pc < pynl && pylines[pc].indent > parent_ind) {
    if (exec_error || exec_break || exec_continue)
      break;
    pc = exec_line(pc);
  }
  return pc;
}

/* ============================================================
 *  Source runner
 * ============================================================ */
static void py_run_src(const char *src, bool repl) {
  exec_repl = repl;
  exec_error = false;
  exec_break = false;
  exec_continue = false;
  py_load(src);
  if (pynl == 0)
    return;
  exec_block(0, -1);
}

/* ============================================================
 *  Public API — run a RAMFS script
 * ============================================================ */
void python_run_file(const char *filename) {
  uint32_t size = 0;
  const char *data = ramfs_read(filename, &size);
  if (!data) {
    terminal_set_color(0x04);
    py_puts("python: file not found in RAM: ");
    py_puts(filename);
    py_putc('\n');
    terminal_set_color(0x07);
    return;
  }
  terminal_set_color(0x0B);
  py_puts("Running ");
  py_puts(filename);
  py_puts("...\n");
  terminal_set_color(0x07);
  py_vars_clear();
  tu_active = false; /* fresh turtle state for each script */
  py_run_src(data, false);
  if (!exec_error) {
    terminal_set_color(0x0A);
    py_puts("[Script finished]\n");
    terminal_set_color(0x07);
  }
}

/* ============================================================
 *  Public API — interactive REPL
 * ============================================================ */
void python_repl(void) {
  terminal_set_color(0x0B);
  py_puts(" _   _                      ____        \n");
  py_puts("| \\ | | _____   _______  __| __ )_   _ \n");
  py_puts("|  \\| |/ _ \\ \\ / / _ \\ \\/ /  _ \\| | | |\n");
  py_puts("| |\\  | (_) \\ V /  __/>  <| |_) | |_| |\n");
  py_puts("|_| \\_|\\___/ \\_/ \\___/_/\\_\\____/ \\__, |\n");
  py_puts("                                  |___/ \n");
  terminal_set_color(0x07);
  py_puts("NovexPy 2.0  |  Python 3 subset on NovexOS\n");
  py_puts("Modules: math  random  time  os  sys  turtle\n");
  py_puts("'import X' is optional — all modules always available.\n");
  py_puts("Empty line runs a multi-line block. 'exit' to quit.\n\n");

  py_vars_clear();
  tu_active = false;

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

    bool empty = (li == 0);
    bool ends_colon = (li > 0 && line[li - 1] == ':');

    if (empty) {
      if (src_len > 0) {
        src[src_len] = 0;
        exec_error = false;
        exec_break = false;
        exec_continue = false;
        py_run_src(src, true);
        src_len = 0;
        in_block = 0;
      }
    } else {
      if (src_len + li + 2 < 4096) {
        int i;
        for (i = 0; i < li; i++)
          src[src_len++] = line[i];
        src[src_len++] = '\n';
        src[src_len] = 0;
      }
      if (ends_colon) {
        in_block++;
      } else if (!in_block) {
        exec_error = false;
        exec_break = false;
        exec_continue = false;
        py_run_src(src, true);
        src_len = 0;
      }
    }
  }

  terminal_set_color(0x0B);
  py_puts("Python REPL closed.\n");
  terminal_set_color(0x07);
}