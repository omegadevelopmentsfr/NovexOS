/*
 * NovexOS - installer_app.c
 * Graphical installer wizard for NovexDE.
 *
 * Multi-step wizard:
 *   Step 0 — Welcome + disk info
 *   Step 1 — Install type (full / dual-boot)
 *   Step 2 — Filesystem (FAT32 / ext4 / NTFS)
 *   Step 3 — Dual-boot partition config (if dual-boot)
 *   Step 4 — Confirmation
 *   Step 5 — Installing (progress)
 *   Step 6 — Done (reboot)
 */

#include "installer_app.h"
#include "fb.h"
#include "font.h"
#include "gui.h"
#include "installer.h"
#include "io.h"
#include "string.h"

/* =========================================================================
 * State
 * ========================================================================= */
static gui_window_t inst_win;
static int inst_step = 0;        /* Current wizard step (0-6) */
static int inst_type = 0;        /* 0 = full disk, 1 = dual-boot */
static int inst_fs = 0;          /* 0 = FAT32, 1 = ext4, 2 = NTFS */
static uint32_t inst_shrink = 0; /* Sectors to keep for existing OS */
static int inst_result = 0;      /* 1 = success, 0 = failure */
static disk_info_t inst_disk;    /* Cached disk probe result */
static int inst_probed = 0;      /* 1 = disk has been probed */

/* Status messages shown during/after install */
#define MSG_MAX 20
#define MSG_LEN 64
static char inst_msgs[MSG_MAX][MSG_LEN];
static int inst_msg_count = 0;
static int inst_percent = 0; /* 0-100 visual progress bar */

/* =========================================================================
 * Theme colours (consistent with NovexDE)
 * ========================================================================= */
#define C_BG 0xFF1E1E2E         /* Window body */
#define C_CARD 0xFF282840       /* Card/panel background */
#define C_ACCENT 0xFF6C63FF     /* Purple accent */
#define C_ACCENT_DIM 0xFF4B44CC /* Darker accent */
#define C_GREEN 0xFF50FA7B      /* Success green */
#define C_RED 0xFFFF5555        /* Error red */
#define C_ORANGE 0xFFFFB86C     /* Warning orange */
#define C_WHITE 0xFFFFFFFF
#define C_GRAY 0xFF888888
#define C_DARK 0xFF16162A /* Dark card */

/* =========================================================================
 * Helpers
 * ========================================================================= */
static void inst_add_msg(const char *s) {
  if (!s)
    return;

  /* If we've reached the limit, shift all messages up by 1 */
  if (inst_msg_count >= MSG_MAX) {
    for (int i = 0; i < MSG_MAX - 1; i++) {
      memcpy(inst_msgs[i], inst_msgs[i + 1], MSG_LEN);
    }
    inst_msg_count = MSG_MAX - 1;
  }

  /* Copy message, ensuring it's null-terminated and truncated if too long */
  int i = 0;
  while (s[i] && i < MSG_LEN - 1) {
    inst_msgs[inst_msg_count][i] = s[i];
    i++;
  }
  inst_msgs[inst_msg_count][i] = '\0';
  inst_msg_count++;
}

/* Simple uint32 → str */
static void u32_to_str(uint32_t v, char *buf) {
  char tmp[12];
  int i = 0;
  if (v == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  while (v) {
    tmp[i++] = (char)('0' + v % 10);
    v /= 10;
  }
  int j = 0;
  while (i > 0)
    buf[j++] = tmp[--i];
  buf[j] = '\0';
}

/* Draw a button and return 1 if (mx,my) is inside it */
static int draw_btn(int32_t x, int32_t y, int32_t w, int32_t h,
                    const char *label, uint32_t bg, uint32_t fg, int32_t mx,
                    int32_t my) {
  gui_draw_button(x, y, w, h, label, bg, fg);
  return (mx >= x && mx < x + w && my >= y && my < y + h);
}

/* Draw a selectable option (radio-like) */
static void draw_option(int32_t x, int32_t y, int32_t w, const char *label,
                        const char *desc, int selected) {
  uint32_t bg = selected ? C_ACCENT : C_CARD;
  uint32_t fg = C_WHITE;
  fb_fill_rect(x, y, w, 44, bg);
  fb_draw_rect(x, y, w, 44, selected ? C_ACCENT : 0xFF3A3A5C);

  /* Radio circle */
  int32_t cx2 = x + 16;
  int32_t cy2 = y + 14;
  fb_fill_rect(cx2, cy2, 12, 12, selected ? C_WHITE : C_GRAY);
  if (!selected)
    fb_fill_rect(cx2 + 2, cy2 + 2, 8, 8, bg);

  fb_draw_string(x + 36, y + 8, label, fg, bg);
  if (desc && desc[0])
    fb_draw_string(x + 36, y + 26, desc, selected ? 0xFFDDDDFF : C_GRAY, bg);
}

/* =========================================================================
 * Render each step
 * ========================================================================= */
static int32_t win_cx(void) { return inst_win.bounds.x; }
static int32_t win_cy(void) { return inst_win.bounds.y + TITLEBAR_HEIGHT; }
static int32_t win_cw(void) { return inst_win.bounds.w; }
static int32_t win_ch(void) { return inst_win.bounds.h - TITLEBAR_HEIGHT; }

/* Horizontal separator line */
static void draw_sep(int32_t y) {
  fb_hline(win_cx() + 20, y, win_cw() - 40, 0xFF3A3A5C);
}

/* Step indicator dots */
static void draw_steps(int32_t y) {
  int total = (inst_type == 1) ? 7 : 6;
  int32_t dot_total = total * 12 + (total - 1) * 8;
  int32_t sx = win_cx() + (win_cw() - dot_total) / 2;
  for (int i = 0; i < total; i++) {
    uint32_t col = (i < inst_step)    ? C_GREEN
                   : (i == inst_step) ? C_ACCENT
                                      : C_GRAY;
    fb_fill_rect(sx + i * 20, y, 12, 12, col);
  }
}

static void render_step0(int32_t mx, int32_t my) {
  int32_t x = win_cx();
  int32_t y = win_cy();
  int32_t w = win_cw();
  (void)mx;
  (void)my;

  /* Title */
  fb_draw_string(x + (w - 22 * FONT_WIDTH) / 2, y + 20,
                 "Install NovexOS on Disk", C_ACCENT, C_BG);

  draw_sep(y + 44);

  /* Disk info card */
  fb_fill_rect(x + 30, y + 56, w - 60, 80, C_CARD);
  fb_draw_rect(x + 30, y + 56, w - 60, 80, 0xFF3A3A5C);

  if (inst_probed && inst_disk.disk_found) {
    char mb_str[12];
    u32_to_str(inst_disk.total_sectors / 2048, mb_str);
    fb_draw_string(x + 46, y + 66, "Disk detected", C_GREEN, C_CARD);

    int32_t tx = fb_draw_string(x + 46, y + 86, "Size: ", C_GRAY, C_CARD);
    tx = fb_draw_string(tx, y + 86, mb_str, C_WHITE, C_CARD);
    fb_draw_string(tx, y + 86, " MB", C_GRAY, C_CARD);

    if (inst_disk.existing_os) {
      fb_draw_string(x + 46, y + 106, "Existing OS partition detected",
                     C_ORANGE, C_CARD);
    } else {
      fb_draw_string(x + 46, y + 106, "No existing OS found", C_GRAY, C_CARD);
    }
  } else {
    fb_draw_string(x + 46, y + 80, "No disk detected!", C_RED, C_CARD);
  }

  /* Warning */
  fb_draw_string(x + 30, y + 150, "This will write to your disk.", C_ORANGE,
                 C_BG);

  draw_steps(y + 180);
}

static void render_step1(int32_t mx, int32_t my) {
  int32_t x = win_cx();
  int32_t y = win_cy();
  int32_t w = win_cw();
  (void)mx;
  (void)my;

  fb_draw_string(x + (w - 19 * FONT_WIDTH) / 2, y + 20, "Installation Type",
                 C_ACCENT, C_BG);
  draw_sep(y + 44);

  draw_option(x + 30, y + 60, w - 60, "Full Disk",
              "Erase disk and install NovexOS", inst_type == 0);

  if (inst_disk.existing_os) {
    draw_option(x + 30, y + 114, w - 60, "Dual Boot",
                "Keep existing OS, install alongside", inst_type == 1);
  } else {
    /* Grayed-out dual-boot option */
    fb_fill_rect(x + 30, y + 114, w - 60, 44, C_DARK);
    fb_draw_rect(x + 30, y + 114, w - 60, 44, 0xFF2A2A3C);
    fb_draw_string(x + 66, y + 122, "Dual Boot", C_GRAY, C_DARK);
    fb_draw_string(x + 66, y + 140, "No existing OS detected", 0xFF555555,
                   C_DARK);
  }

  draw_steps(y + 180);
}

static void render_step2(int32_t mx, int32_t my) {
  int32_t x = win_cx();
  int32_t y = win_cy();
  int32_t w = win_cw();
  (void)mx;
  (void)my;

  fb_draw_string(x + (w - 17 * FONT_WIDTH) / 2, y + 20, "Choose Filesystem",
                 C_ACCENT, C_BG);
  draw_sep(y + 44);

  draw_option(x + 30, y + 60, w - 60, "FAT32", "Best compatibility",
              inst_fs == 0);
  draw_option(x + 30, y + 114, w - 60, "ext4", "Linux native filesystem",
              inst_fs == 1);

  /* NTFS is disabled: the current boot-area overlay overwrites the NTFS
   * VBR at sector 0, so the resulting partition isn't valid NTFS. Needs
   * a real separate boot partition to fix properly - not done yet. */
  fb_fill_rect(x + 30, y + 168, w - 60, 44, C_DARK);
  fb_draw_rect(x + 30, y + 168, w - 60, 44, 0xFF2A2A3C);
  fb_draw_string(x + 66, y + 176, "NTFS", C_GRAY, C_DARK);
  fb_draw_string(x + 66, y + 194, "Not supported yet", 0xFF555555, C_DARK);

  draw_steps(y + 228);
}

static void render_step3(int32_t mx, int32_t my) {
  int32_t x = win_cx();
  int32_t y = win_cy();
  int32_t w = win_cw();
  (void)mx;
  (void)my;

  fb_draw_string(x + (w - 22 * FONT_WIDTH) / 2, y + 20,
                 "Dual-Boot Partitioning", C_ACCENT, C_BG);
  draw_sep(y + 44);

  /* Show existing OS size and slider */
  char curr_str[12], total_str[12];
  uint32_t existing_mb = inst_shrink / 2048;
  uint32_t total_mb = inst_disk.total_sectors / 2048;
  uint32_t novex_mb = total_mb - existing_mb;
  u32_to_str(existing_mb, curr_str);
  u32_to_str(novex_mb, total_str);

  fb_draw_string(x + 30, y + 60, "Existing OS:", C_GRAY, C_BG);
  int32_t tx =
      fb_draw_string(x + 30 + 15 * FONT_WIDTH, y + 60, curr_str, C_WHITE, C_BG);
  fb_draw_string(tx, y + 60, " MB", C_GRAY, C_BG);

  fb_draw_string(x + 30, y + 82, "NovexOS:    ", C_GRAY, C_BG);
  tx = fb_draw_string(x + 30 + 15 * FONT_WIDTH, y + 82, total_str, C_GREEN,
                      C_BG);
  fb_draw_string(tx, y + 82, " MB", C_GRAY, C_BG);

  /* Visual partition bar */
  int32_t bar_x = x + 30;
  int32_t bar_y = y + 110;
  int32_t bar_w = w - 60;
  int32_t bar_h = 30;
  fb_fill_rect(bar_x, bar_y, bar_w, bar_h, C_CARD);
  fb_draw_rect(bar_x, bar_y, bar_w, bar_h, 0xFF3A3A5C);

  /* Existing OS portion */
  int32_t existing_w = 0;
  if (inst_disk.total_sectors > 0)
    existing_w = (int32_t)((uint64_t)inst_shrink * (uint64_t)bar_w /
                           (uint64_t)inst_disk.total_sectors);
  if (existing_w > bar_w - 20)
    existing_w = bar_w - 20;
  fb_fill_rect(bar_x + 1, bar_y + 1, existing_w, bar_h - 2, C_ORANGE);
  /* NovexOS portion */
  fb_fill_rect(bar_x + 1 + existing_w, bar_y + 1, bar_w - 2 - existing_w,
               bar_h - 2, C_ACCENT);

  if (existing_w > 50)
    fb_draw_string(bar_x + 6, bar_y + 8, "Existing", C_WHITE, C_ORANGE);
  if (bar_w - existing_w > 60)
    fb_draw_string(bar_x + existing_w + 6, bar_y + 8, "NovexOS", C_WHITE,
                   C_ACCENT);

  /* +/- buttons to adjust partition size */
  fb_draw_string(x + 30, y + 156, "Adjust existing OS size:", C_GRAY, C_BG);

  draw_btn(x + w / 2 - 100, y + 178, 40, 30, "-10", C_CARD, C_WHITE, -1, -1);
  draw_btn(x + w / 2 - 50, y + 178, 40, 30, "-1", C_CARD, C_WHITE, -1, -1);
  draw_btn(x + w / 2 + 10, y + 178, 40, 30, "+1", C_CARD, C_WHITE, -1, -1);
  draw_btn(x + w / 2 + 60, y + 178, 40, 30, "+10", C_CARD, C_WHITE, -1, -1);
  fb_draw_string(x + w / 2 - 6, y + 185, "MB", C_GRAY, C_BG);

  draw_steps(y + 228);
}

static void render_step4(int32_t mx, int32_t my) {
  int32_t x = win_cx();
  int32_t y = win_cy();
  int32_t w = win_cw();
  (void)mx;
  (void)my;

  fb_draw_string(x + (w - 20 * FONT_WIDTH) / 2, y + 20, "Confirm Installation",
                 C_ACCENT, C_BG);
  draw_sep(y + 44);

  /* Summary card */
  fb_fill_rect(x + 30, y + 56, w - 60, 110, C_CARD);
  fb_draw_rect(x + 30, y + 56, w - 60, 110, 0xFF3A3A5C);

  const char *type_str = inst_type == 0 ? "Full Disk" : "Dual Boot";
  const char *fs_str = inst_fs == 0 ? "FAT32" : inst_fs == 1 ? "ext4" : "NTFS";

  fb_draw_string(x + 46, y + 66, "Type:       ", C_GRAY, C_CARD);
  fb_draw_string(x + 46 + 12 * FONT_WIDTH, y + 66, type_str, C_WHITE, C_CARD);

  fb_draw_string(x + 46, y + 86, "Filesystem: ", C_GRAY, C_CARD);
  fb_draw_string(x + 46 + 12 * FONT_WIDTH, y + 86, fs_str, C_WHITE, C_CARD);

  if (inst_type == 1) {
    char mb_str[12];
    u32_to_str(inst_shrink / 2048, mb_str);
    fb_draw_string(x + 46, y + 106, "Existing OS:", C_GRAY, C_CARD);
    int32_t tx2 = fb_draw_string(x + 46 + 12 * FONT_WIDTH, y + 106, mb_str,
                                 C_WHITE, C_CARD);
    fb_draw_string(tx2, y + 106, " MB", C_GRAY, C_CARD);

    char novex_str[12];
    u32_to_str((inst_disk.total_sectors - inst_shrink) / 2048, novex_str);
    fb_draw_string(x + 46, y + 126, "NovexOS:    ", C_GRAY, C_CARD);
    int32_t tx3 = fb_draw_string(x + 46 + 12 * FONT_WIDTH, y + 126, novex_str,
                                 C_WHITE, C_CARD);
    fb_draw_string(tx3, y + 126, " MB", C_GRAY, C_CARD);
  } else {
    char mb_str[12];
    u32_to_str(inst_disk.total_sectors / 2048, mb_str);
    fb_draw_string(x + 46, y + 106, "Disk size:  ", C_GRAY, C_CARD);
    int32_t tx2 = fb_draw_string(x + 46 + 12 * FONT_WIDTH, y + 106, mb_str,
                                 C_WHITE, C_CARD);
    fb_draw_string(tx2, y + 106, " MB", C_GRAY, C_CARD);
  }

  /* Warning */
  fb_draw_string(x + 30, y + 180, "DATA WILL BE ERASED. Continue?", C_RED,
                 C_BG);

  draw_steps(y + 210);
}

static void render_step5(int32_t mx, int32_t my) {
  int32_t x = win_cx();
  int32_t y = win_cy();
  int32_t w = win_cw();
  (void)mx;
  (void)my;

  fb_draw_string(x + (w - 14 * FONT_WIDTH) / 2, y + 20, "Installing ...",
                 C_ACCENT, C_BG);

  /* Messages list */
  int start_y = y + 45;
  for (int i = 0; i < inst_msg_count && i < MSG_MAX; i++) {
    uint32_t col = C_GRAY;
    if (i == inst_msg_count - 1)
      col = C_GREEN;
    fb_draw_string(x + 30, start_y + i * 14, inst_msgs[i], col, C_BG);
  }

  /* Progress bar */
  int32_t pbar_w = w - 60;
  int32_t pbar_x = x + 30;
  int32_t pbar_y = y + 230;
  int32_t pbar_h = 24;

  fb_fill_rect(pbar_x, pbar_y, pbar_w, pbar_h, C_CARD);
  fb_draw_rect(pbar_x, pbar_y, pbar_w, pbar_h, 0xFF3A3A5C);

  int32_t fill_w = (int32_t)((uint32_t)pbar_w * (uint32_t)inst_percent / 100);
  if (fill_w > 0) {
    if (fill_w > pbar_w - 2)
      fill_w = pbar_w - 2;
    fb_fill_rect(pbar_x + 1, pbar_y + 1, fill_w, pbar_h - 2, C_GREEN);
  }
}

static void render_step6(int32_t mx, int32_t my) {
  int32_t x = win_cx();
  int32_t y = win_cy();
  int32_t w = win_cw();
  (void)mx;
  (void)my;

  if (inst_result) {
    fb_draw_string(x + (w - 22 * FONT_WIDTH) / 2, y + 40,
                   "Installation Complete!", C_GREEN, C_BG);
    fb_draw_string(x + (w - 30 * FONT_WIDTH) / 2, y + 70,
                   "NovexOS has been installed.", C_WHITE, C_BG);
    fb_draw_string(x + (w - 36 * FONT_WIDTH) / 2, y + 92,
                   "Reboot to start from disk.", C_GRAY, C_BG);
  } else {
    fb_draw_string(x + (w - 22 * FONT_WIDTH) / 2, y + 40,
                   "Installation Failed!", C_RED, C_BG);

    for (int i = 0; i < inst_msg_count && i < MSG_MAX; i++) {
      fb_draw_string(x + 30, y + 80 + i * 20, inst_msgs[i], C_GRAY, C_BG);
    }
  }
}

/* =========================================================================
 * Navigation buttons (rendered at bottom of window)
 * ========================================================================= */

/* Returns button id clicked: -1=none, 0=back/cancel, 1=next/install/reboot */
static int render_nav_buttons(int32_t mx, int32_t my, int clicking) {
  int32_t x = win_cx();
  int32_t y = win_cy() + win_ch() - 50;
  int32_t w = win_cw();
  int hit = -1;

  if (inst_step == 0) {
    /* Start button only (if disk found) */
    if (inst_probed && inst_disk.disk_found) {
      int inside = draw_btn(x + w / 2 - 60, y, 120, 36, "Start", C_ACCENT,
                            C_WHITE, mx, my);
      if (inside && clicking)
        hit = 1;
    }
  } else if (inst_step == 5) {
    /* No buttons during install */
  } else if (inst_step == 6) {
    int inside = draw_btn(x + w / 2 - 60, y, 120, 36, "Reboot", C_GREEN,
                          C_WHITE, mx, my);
    if (inside && clicking)
      hit = 1;
  } else {
    /* Back + Next (or Install on step 4) */
    int back_in = draw_btn(x + 30, y, 100, 36, "Back", C_CARD, C_WHITE, mx, my);
    if (back_in && clicking)
      hit = 0;

    const char *next_label = (inst_step == 4) ? "Install" : "Next";
    uint32_t next_color = (inst_step == 4) ? C_GREEN : C_ACCENT;
    int next_in = draw_btn(x + w - 130, y, 100, 36, next_label, next_color,
                           C_WHITE, mx, my);
    if (next_in && clicking)
      hit = 1;
  }

  return hit;
}

/* =========================================================================
 * Perform the actual installation
 * ========================================================================= */

static void installer_progress_cb(int pct, const char *msg) {
  serial_print("[CB] enter\n");
  if (msg && msg[0]) {
    inst_add_msg(msg);
  }
  if (pct >= 0 && pct <= 100) {
    inst_percent = pct;
  }
  /* Force update the window interior immediately without waiting for desktop
   * loop */
  gui_window_render(&inst_win);
  render_step5(-1, -1);

  /* Swap buffers directly to push our update to the screen */
  fb_swap();
  serial_print("[CB] exit\n");
}

static void do_install(void) {
  inst_msg_count = 0;
  inst_percent = 0;
  inst_add_msg("Probing disk ...");

  install_fs_t fs = (inst_fs == 0)   ? INSTALL_FS_FAT32
                    : (inst_fs == 1) ? INSTALL_FS_EXT4
                                     : INSTALL_FS_NTFS;

  if (inst_type == 0) {
    inst_result = installer_full_install(fs, installer_progress_cb);
  } else {
    inst_result =
        installer_dualboot_install(fs, inst_shrink, installer_progress_cb);
  }
  serial_print("[INSTALLER_APP] do_install: install call returned\n");

  if (inst_result)
    inst_add_msg("Done!");
  else
    inst_add_msg("Error during installation.");

  serial_print("[INSTALLER_APP] do_install: msgs added, setting inst_step=6\n");
  inst_step = 6;

  /* Force-paint the final screen immediately (including Reboot button) */
  installer_app_render();
  fb_swap();
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void installer_app_init(void) {
  uint32_t sw = fb_get_width();
  uint32_t sh = fb_get_height();
  int32_t ww = 500, wh = 340;
  int32_t wx = ((int32_t)sw - ww) / 2;
  int32_t wy = ((int32_t)(sh - TASKBAR_HEIGHT) - wh) / 2;
  gui_window_init(&inst_win, wx, wy, ww, wh, "NovexOS Installer");
  inst_win.visible = 0;
  inst_step = 0;
  inst_probed = 0;
}

void installer_app_render(void) {
  if (!inst_win.visible)
    return;

  /* Probe disk on first show */
  if (!inst_probed) {
    inst_probed = installer_probe(&inst_disk);
    if (inst_disk.existing_os)
      inst_shrink = inst_disk.existing_sectors;
    else
      inst_shrink = 0;
  }

  gui_window_render(&inst_win);

  /* Render step content */
  switch (inst_step) {
  case 0:
    render_step0(-1, -1);
    break;
  case 1:
    render_step1(-1, -1);
    break;
  case 2:
    render_step2(-1, -1);
    break;
  case 3:
    render_step3(-1, -1);
    break;
  case 4:
    render_step4(-1, -1);
    break;
  case 5:
    render_step5(-1, -1);
    break;
  case 6:
    render_step6(-1, -1);
    break;
  }

  /* Nav buttons (render only, no click test) */
  render_nav_buttons(-1, -1, 0);
}

int installer_app_click(int32_t mx, int32_t my) {
  if (!inst_win.visible)
    return 0;

  int32_t x = inst_win.bounds.x;
  int32_t y = inst_win.bounds.y;
  int32_t w = inst_win.bounds.w;
  int32_t h = inst_win.bounds.h;

  /* Outside window? */
  if (mx < x || mx >= x + w || my < y || my >= y + h)
    return 0;

  /* Close button */
  int32_t close_x = x + w - 22;
  int32_t close_y = y + 6;
  gui_rect_t close_rect = {close_x, close_y, 16, 16};
  if (gui_point_in_rect(mx, my, &close_rect)) {
    inst_win.visible = 0;
    inst_step = 0;
    inst_probed = 0;
    return 1;
  }

  /* Titlebar drag is handled by desktop.c */
  gui_rect_t titlebar = {x, y, w, TITLEBAR_HEIGHT};
  if (gui_point_in_rect(mx, my, &titlebar))
    return 0; /* let desktop handle drag */

  /* Nav buttons */
  int nav = render_nav_buttons(mx, my, 1);
  if (nav == 0) {
    /* Back */
    if (inst_step > 0) {
      inst_step--;
      /* Skip dual-boot config step if full disk */
      if (inst_step == 3 && inst_type == 0)
        inst_step = 2;
    }
    return 1;
  }
  if (nav == 1) {
    /* Next / Start / Install / Reboot */
    if (inst_step == 6) {
      /* Reboot */
      reboot();
      while (1)
        __asm__ volatile("hlt");
      return 1;
    }
    if (inst_step == 4) {
      /* Install! */
      inst_step = 5;
      do_install();
      serial_print("[INSTALLER_APP] click handler: do_install() returned\n");
      return 1;
    }
    inst_step++;
    /* Skip dual-boot config step if full disk */
    if (inst_step == 3 && inst_type == 0)
      inst_step = 4;
    return 1;
  }

  /* Step-specific clicks */
  int32_t cy = inst_win.bounds.y + TITLEBAR_HEIGHT;

  if (inst_step == 1) {
    /* Full disk option */
    if (my >= cy + 60 && my < cy + 104)
      inst_type = 0;
    /* Dual boot option (only if OS exists) */
    if (inst_disk.existing_os && my >= cy + 114 && my < cy + 158)
      inst_type = 1;
    return 1;
  }

  if (inst_step == 2) {
    if (my >= cy + 60 && my < cy + 104)
      inst_fs = 0;
    else if (my >= cy + 114 && my < cy + 158)
      inst_fs = 1;
    /* NTFS option (cy+168..212) is disabled - not clickable. */
    return 1;
  }

  if (inst_step == 3) {
    /* +/- MB buttons */
    int32_t bx = x + w / 2;
    int32_t by = cy + 178;
    uint32_t min_sectors = 2048 * 10; /* 10 MB minimum */
    uint32_t max_sectors =
        inst_disk.total_sectors - 2048 * 20; /* 20 MB min for NovexOS */

    if (my >= by && my < by + 30) {
      if (mx >= bx - 100 && mx < bx - 60) {
        /* -10 MB */
        if (inst_shrink > min_sectors + 2048 * 10)
          inst_shrink -= 2048 * 10;
        else
          inst_shrink = min_sectors;
      } else if (mx >= bx - 50 && mx < bx - 10) {
        /* -1 MB */
        if (inst_shrink > min_sectors + 2048)
          inst_shrink -= 2048;
        else
          inst_shrink = min_sectors;
      } else if (mx >= bx + 10 && mx < bx + 50) {
        /* +1 MB */
        if (inst_shrink + 2048 <= max_sectors)
          inst_shrink += 2048;
        else
          inst_shrink = max_sectors;
      } else if (mx >= bx + 60 && mx < bx + 100) {
        /* +10 MB */
        if (inst_shrink + 2048 * 10 <= max_sectors)
          inst_shrink += 2048 * 10;
        else
          inst_shrink = max_sectors;
      }
    }
    return 1;
  }

  return 1; /* Consumed but no specific action */
}

int installer_app_key(char c) {
  (void)c;
  return 0; /* No keyboard input needed */
}

gui_window_t *installer_app_get_window(void) { return &inst_win; }