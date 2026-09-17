/*
 * NovexOS - shell.c
 * Command-line shell with built-in commands.
 * Supports RAMFS and FAT32 (Disk) via 'cd' and 'ls'.
 */

#include "shell.h"
#include "ata.h"
#include "desktop.h"
#include "editor.h"
#include "fat32.h"
#include "io.h"
#include "keyboard.h"
#include "pmm.h"
#include "python.h"
#include "ramfs.h"
#include "string.h"
#include "timer.h"
#include "types.h"
#include "vbe.h"

/* ------- External terminal functions ------- */
extern void terminal_putchar(char c);
extern void terminal_backspace(void);
extern void terminal_writestring(const char *s);
extern void terminal_clear(void);
extern void terminal_set_color(uint8_t color);
extern uint8_t terminal_get_color(void);

/* ------- Shell state ------- */
static char cmd_buffer[SHELL_MAX_CMD];
static size_t cmd_len;

extern int system_booted_from_disk(void);

/* Current Working Directory - defaults to disk:/ if booted from disk */
static char cwd[64] = "ram:/";

/* ------- Helper: Check if we are on disk ------- */
static int is_disk(void) { return (strncmp(cwd, "disk:", 5) == 0); }
int shell_is_disk(void) { return is_disk(); }
const char *shell_get_cwd(void) { return cwd; }

/* ------- Print prompt ------- */
static void shell_prompt(void) {
  uint8_t old = terminal_get_color();
  terminal_set_color(0x0B);
  terminal_writestring("omega");
  terminal_set_color(0x0A);
  terminal_writestring("@");
  terminal_set_color(0x0E);
  terminal_writestring("os");
  terminal_set_color(0x07);
  terminal_writestring(":");
  terminal_set_color(0x09); /* Blue for path */
  terminal_writestring(cwd);
  terminal_set_color(0x07);
  terminal_writestring("$ ");
  terminal_set_color(old);
}

static const char *skip_spaces(const char *s) {
  while (*s == ' ')
    s++;
  return s;
}

/* ------- Command handlers ------- */

static void cmd_help(void) {
  terminal_writestring("Available commands:\n");
  terminal_writestring("  help          - Show help\n");
  terminal_writestring("  clear         - Clear screen\n");
  terminal_writestring("  echo <text>   - Print text\n");
  terminal_writestring("  uname / version - System info\n");
  terminal_writestring("  uptime        - System uptime\n");
  terminal_writestring("  ls            - List files (RAM or Disk)\n");
  terminal_writestring(
      "  cd <path>     - Change directory (ram:/ or disk:/)\n");
  terminal_writestring("  pwd           - Print working directory\n");
  terminal_writestring("  mkdir <name>  - Create directory (RAM or disk:/)\n");
  terminal_writestring("  cat <file>    - Display file content\n");
  terminal_writestring("  rm <file>     - Delete file or directory\n");
  terminal_writestring("  edit <file>   - Open text editor\n");
  terminal_writestring("  startde       - Launch NovexDE desktop\n");
  terminal_writestring("  python <file> - Run a .py script (RAM or disk:/)\n");
  terminal_writestring("  shutdown      - Power off\n");
  terminal_writestring("  reboot        - Reboot\n");
  terminal_writestring("  color         - Change terminal color\n");
  terminal_writestring("  free          - Show free space in ram\n");
  terminal_writestring("  pwd           - Show active directory\n");
}

static void cmd_uname(void) {
  terminal_writestring("NovexOS v0.8.0 [x86_64] - FAT32 Support Enabled\n");
}

static void cmd_version(void) {
  terminal_writestring("NovexOS version 0.8.0\n");
  terminal_writestring(
      "Features: 64-bit Long Mode, ATA LBA48 (PIO), FAT32, MBR\n");
}

static void cmd_uptime(void) {
  char buf[12];
  uint32_t secs = timer_get_uptime_seconds();
  int_to_str(secs, buf);
  terminal_writestring("Uptime: ");
  terminal_writestring(buf);
  terminal_writestring(" seconds\n");
}

static void cmd_echo(const char *args) {
  terminal_writestring(skip_spaces(args));
  terminal_putchar('\n');
}

static void cmd_color(const char *args) {
  const char *num_str = skip_spaces(args);
  if (*num_str == '\0')
    return;
  uint8_t color = 0;
  while (*num_str >= '0' && *num_str <= '9') {
    color = (uint8_t)(color * 10 + (*num_str - '0'));
    num_str++;
  }
  terminal_set_color(color & 0x0F);
  terminal_writestring("Color changed.\n");
}

extern int keyboard_get_layout(void);
static void cmd_keymap(void) {
  int layout = keyboard_get_layout();
  terminal_writestring("Layout: ");
  terminal_writestring(layout == 1 ? "AZERTY" : "QWERTY");
  terminal_putchar('\n');
}

/* ------- Filesystem commands ------- */

static void cmd_pwd(void) {
  terminal_writestring(cwd);
  terminal_putchar('\n');
}

/* Build the full ram:/disk-relative path for `name` inside the current
 * directory. cwd is either "<proto>:/" (root) or "<proto>:/NAME"
 * (one-level subfolder) - matches the one-level nesting ramfs/fat32.c
 * support. `out` must be at least 64 bytes. */
void shell_build_path(char *out, const char *name) {
  const char *prefix = is_disk() ? "disk:/" : "ram:/";
  if (strcmp(cwd, prefix) == 0) {
    strcpy(out, name);
  } else {
    size_t plen = strlen(prefix);
    const char *rel = cwd + plen;
    size_t rl = strlen(rel);
    memcpy(out, rel, rl);
    out[rl] = '/';
    strcpy(out + rl + 1, name);
  }
}

static void cmd_cd(const char *args) {
  const char *path = skip_spaces(args);
  if (*path == '\0') {
    terminal_writestring("Usage: cd <path>\n");
    return;
  }

  /* Simple switching for now */
  if (strcmp(path, "ram:") == 0 || strcmp(path, "ram:/") == 0) {
    if (system_booted_from_disk()) {
      terminal_writestring("Notice: System is booted from disk. RAM is read-only.\n");
    }
    strcpy(cwd, "ram:/");
  } else if (strcmp(path, "disk:") == 0 || strcmp(path, "disk:/") == 0) {
    strcpy(cwd, "disk:/");
  } else if (strcmp(path, "/") == 0) {
    if (system_booted_from_disk())
      strcpy(cwd, "disk:/");
    else
      strcpy(cwd, "ram:/");
  } else if (strcmp(path, "..") == 0) {
    if (is_disk()) {
      strcpy(cwd, "disk:/"); /* Only one level of nesting supported */
    } else {
      strcpy(cwd, "ram:/");
    }
  } else {
    /* Enter a subdirectory (one level deep, relative to the current root) */
    int found_dir = is_disk() ? fat32_is_dir(path) : ramfs_is_dir(path);
    if (found_dir) {
      strcpy(cwd, is_disk() ? "disk:/" : "ram:/");
      strcpy(cwd + strlen(cwd), path);
    } else {
      terminal_writestring("Directory not found: ");
      terminal_writestring(path);
      terminal_putchar('\n');
    }
  }
}

static void cmd_mkdir(const char *args) {
  const char *name = skip_spaces(args);
  if (*name == '\0') {
    terminal_writestring("Usage: mkdir <name>\n");
    return;
  }

  char path[64];
  shell_build_path(path, name);

  if (is_disk()) {
    if (fat32_mkdir(path) == 0) {
      terminal_writestring("Directory created on disk.\n");
    } else {
      terminal_writestring(
          "Could not create directory (already exists, disk full, or invalid name).\n");
    }
  } else {
    if (system_booted_from_disk()) {
      terminal_writestring("Error: RAM is read-only after installation. Use disk:/\n");
      return;
    }
    if (ramfs_mkdir(path) == 0) {
      terminal_writestring("Directory created.\n");
    } else {
      terminal_writestring("Could not create directory.\n");
    }
  }
}

static void ls_ram_cb(const char *name, uint32_t size, bool is_dir) {
  char buf[12];
  terminal_writestring("  ");
  if (is_dir) {
    terminal_writestring("[DIR] ");
  } else {
    terminal_writestring("      ");
  }
  terminal_writestring(name);
  size_t len = strlen(name);
  while (len < 20) {
    terminal_putchar(' ');
    len++;
  }
  if (!is_dir) {
    int_to_str(size, buf);
    terminal_writestring(buf);
    terminal_writestring(" bytes\n");
  } else {
    terminal_writestring("\n");
  }
}

static void cmd_ls(void) {
  if (is_disk()) {
    terminal_writestring("Listing Disk (FAT32) - ");
    terminal_writestring(cwd);
    terminal_writestring(":\n");
    fat32_ls(cwd);
  } else {
    terminal_writestring("Listing RAM Filesystem:\n");
    ramfs_list(ls_ram_cb);
  }
}

static void cmd_cat(const char *args) {
  const char *name = skip_spaces(args);
  if (*name == '\0') {
    terminal_writestring("Usage: cat <file>\n");
    return;
  }

  char path[64];
  shell_build_path(path, name);

  uint8_t buf[1024]; /* Small buffer on stack */
  uint32_t size = 0;

  if (is_disk()) {
    int res = fat32_read_file(path, buf, 1023);
    if (res < 0) {
      terminal_writestring("File not found on disk.\n");
      return;
    }
    size = res;
  } else {
    const char *data = ramfs_read(path, &size);
    if (!data) {
      terminal_writestring("File not found in RAM.\n");
      return;
    }
    if (size > 1023)
      size = 1023; /* Cap for display */
    memcpy(buf, data, size);
  }

  for (uint32_t i = 0; i < size; i++) {
    terminal_putchar((char)buf[i]);
  }
  terminal_putchar('\n');
}

static void cmd_rm(const char *args) {
  const char *name = skip_spaces(args);
  if (*name == '\0') {
    terminal_writestring("Usage: rm <file>\n");
    return;
  }

  char path[64];
  shell_build_path(path, name);

  if (is_disk()) {
    int r = fat32_delete(path);
    if (r == 0) {
      terminal_writestring("Deleted from disk.\n");
    } else if (r == -2) {
      terminal_writestring("Error: '");
      terminal_writestring(name);
      terminal_writestring(
          "' is a protected system directory/file and cannot be deleted!\n");
    } else {
      terminal_writestring("File not found on disk.\n");
    }
  } else {
    if (system_booted_from_disk()) {
      terminal_writestring("Error: RAM is read-only after installation.\n");
      return;
    }
    if (ramfs_delete(path) == 0) {
      terminal_writestring("Deleted.\n");
    } else {
      terminal_writestring("File not found.\n");
    }
  }
}

static void cmd_free(void) {
  char buf[12];
  terminal_writestring("RAM Free: ");
  int_to_str(pmm_free_pages() * 4, buf);
  terminal_writestring(buf);
  terminal_writestring(" KB\n");
}

/* ------- System commands ------- */
static void cmd_reboot(void) {
  /* 64-bit IDT limit is 0, causes triple fault */
  uint8_t zero_idt[10] = {0};
  __asm__ volatile("lidt (%0)" : : "r"(zero_idt));
  __asm__ volatile("int $0x03");
}

static void cmd_shutdown(void) {
  outw(0x604, 0x2000);
  outw(0xB004, 0x2000);
  outb(0x64, 0xFE);
  __asm__ volatile("cli");
  for (;;) {
    __asm__ volatile("hlt");
  }
}

static void cmd_edit(const char *args) {
  const char *raw = skip_spaces(args);
  const char *name = *raw ? raw : "untitled.txt";
  char path[64];
  shell_build_path(path, name);
  editor_open(path);
}

/* ------- Execute ------- */
static void shell_execute(const char *cmd) {
  const char *c = skip_spaces(cmd);
  if (*c == '\0')
    return;

  if (strcmp(c, "help") == 0)
    cmd_help();
  else if (strcmp(c, "clear") == 0)
    terminal_clear();
  else if (strcmp(c, "uname") == 0)
    cmd_uname();
  else if (strcmp(c, "version") == 0)
    cmd_version();
  else if (strcmp(c, "uptime") == 0)
    cmd_uptime();
  else if (strcmp(c, "reboot") == 0)
    cmd_reboot();
  else if (strcmp(c, "shutdown") == 0)
    cmd_shutdown();
  else if (strcmp(c, "keymap") == 0)
    cmd_keymap();
  else if (strcmp(c, "ls") == 0)
    cmd_ls();
  else if (strncmp(c, "cd ", 3) == 0)
    cmd_cd(c + 3);
  else if (strncmp(c, "mkdir ", 6) == 0)
    cmd_mkdir(c + 6);
  else if (strcmp(c, "pwd") == 0)
    cmd_pwd();
  else if (strcmp(c, "free") == 0)
    cmd_free();
  else if (strncmp(c, "echo ", 5) == 0)
    cmd_echo(c + 5);
  else if (strncmp(c, "color ", 6) == 0)
    cmd_color(c + 6);
  else if (strncmp(c, "cat ", 4) == 0)
    cmd_cat(c + 4);
  else if (strncmp(c, "rm ", 3) == 0)
    cmd_rm(c + 3);
  else if (strncmp(c, "edit ", 5) == 0)
    cmd_edit(c + 5);
  else if (strcmp(c, "edit") == 0)
    cmd_edit("");
  else if (strcmp(c, "startde") == 0) {
    vbe_init(NULL); /* Switch from VGA text to VBE Graphic */
    desktop_init();
  } else if (strncmp(c, "python ", 7) == 0) {
    char path[64];
    shell_build_path(path, skip_spaces(c + 7));
    python_run_file(path);
  } else {
    terminal_writestring("Unknown command: ");
    terminal_writestring(c);
    terminal_writestring("\n");
  }
}

/* ------- API ------- */
void shell_init(void) {
  cmd_len = 0;
  memset(cmd_buffer, 0, SHELL_MAX_CMD);
  if (system_booted_from_disk()) {
    strcpy(cwd, "disk:/");
  } else {
    strcpy(cwd, "ram:/");
  }
  shell_prompt();
}
int shell_is_editor_active(void) { return editor_is_active(); }
void shell_input(char c) {
  if (editor_is_active()) {
    editor_input(c);
    return;
  }
  if (c == '\n') {
    terminal_putchar('\n');
    cmd_buffer[cmd_len] = '\0';
    shell_execute(cmd_buffer);
    cmd_len = 0;
    memset(cmd_buffer, 0, SHELL_MAX_CMD);
    shell_prompt();
  } else if (c == '\b') {
    if (cmd_len > 0) {
      cmd_len--;
      cmd_buffer[cmd_len] = '\0';
      terminal_backspace();
    }
  } else {
    if (cmd_len < SHELL_MAX_CMD - 1) {
      if (c >= ' ' && c <= '~') {
        cmd_buffer[cmd_len++] = c;
        terminal_putchar(c);
      }
    }
  }
}