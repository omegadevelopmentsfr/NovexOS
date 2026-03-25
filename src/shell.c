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
#include "mbr.h"
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

/* Current Working Directory - defaults to ramfs root */
static char cwd[64] = "ram:/";

/* ------- Helper: Check if we are on disk ------- */
static int is_disk(void) { return (strncmp(cwd, "disk:", 5) == 0); }

/* ------- Print prompt ------- */
static void shell_prompt(void) {
  uint8_t old = terminal_get_color();
  terminal_set_color(0x0B);
  terminal_writestring("novex");
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
  terminal_writestring("  mkdir <name>  - Create RAM directory\n");
  terminal_writestring("  cat <file>    - Display file content\n");
  terminal_writestring("  rm <file>     - Delete file\n");
  terminal_writestring("  install       - Format disk (FAT32)\n");
  terminal_writestring("  edit <file>   - Open text editor\n");
  terminal_writestring("  startde       - Launch NovexDE desktop\n");
  terminal_writestring("  python <file> - Run a .py script from RAM\n");
  terminal_writestring("  shutdown      - Power off\n");
  terminal_writestring("  reboot        - Reboot\n");
}

static void cmd_uname(void) {
  terminal_writestring("NovexOS v0.7.2 [x86_64] - FAT32 Support Enabled\n");
}

static void cmd_version(void) {
  terminal_writestring("NovexOS version 0.7.2\n");
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

static void cmd_cd(const char *args) {
  const char *path = skip_spaces(args);
  if (*path == '\0') {
    terminal_writestring("Usage: cd <path>\n");
    return;
  }

  /* Simple switching for now */
  if (strcmp(path, "ram:") == 0 || strcmp(path, "ram:/") == 0) {
    strcpy(cwd, "ram:/");
  } else if (strcmp(path, "disk:") == 0 || strcmp(path, "disk:/") == 0) {
    strcpy(cwd, "disk:/");
  } else if (strcmp(path, "/") == 0) {
    /* Default to ram */
    strcpy(cwd, "ram:/");
  } else {
    terminal_writestring("Unknown path. Try 'ram:/' or 'disk:/'\n");
  }
}

static void cmd_mkdir(const char *args) {
  const char *name = skip_spaces(args);
  if (*name == '\0') {
    terminal_writestring("Usage: mkdir <name>\n");
    return;
  }

  if (is_disk()) {
    terminal_writestring("MKDIR on disk not implemented.\n");
  } else {
    if (ramfs_mkdir(name) == 0) {
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
    terminal_writestring("Listing Disk (FAT32):\n");
    fat32_ls("/");
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

  uint8_t buf[1024]; /* Small buffer on stack */
  uint32_t size = 0;

  if (is_disk()) {
    int res = fat32_read_file(name, buf, 1023);
    if (res < 0) {
      terminal_writestring("File not found on disk.\n");
      return;
    }
    size = res;
  } else {
    const char *data = ramfs_read(name, &size);
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

  if (is_disk()) {
    fat32_delete_file(name); /* Stub */
    terminal_writestring("Disk write not fully implemented yet.\n");
  } else {
    if (ramfs_delete(name) == 0) {
      terminal_writestring("Deleted.\n");
    } else {
      terminal_writestring("File not found.\n");
    }
  }
}

static void cmd_install(void) {
  terminal_set_color(0x0E); /* Yellow */
  terminal_writestring("--- NovexOS Installer ---\n");
  terminal_set_color(0x07);

  /* 1. Hardware disk detection via IDENTIFY (works even if
   *    the disk is empty/unformatted/all zeroes).
   *    Do NOT use an all-zero check: disk.img is created with
   *    dd if=/dev/zero and will always be zero before installation. */
  terminal_writestring("Detecting disk...\n");
  if (!ata_identify(0)) {
    terminal_set_color(0x0C);
    terminal_writestring("ERROR: No hard disk detected on Primary Master!\n");
    terminal_set_color(0x07);
    terminal_writestring("Make sure QEMU has a disk on IDE bus=0,unit=0.\n");
    return;
  }
  terminal_writestring("Hard disk detected.\n");

  /* Read the MBR for partition info (disk may be empty) */
  uint8_t mbr_buf[512];
  ata_read_sectors(0, 1, mbr_buf);

  /* 2. Detect existing OS / Dual-boot logic */
  struct mbr *m = (struct mbr *)mbr_buf;

  bool dual_boot = false;
  if (m->signature == 0xAA55) {
    for (int i = 0; i < 4; i++) {
      if (m->partitions[i].type != 0 && m->partitions[i].type != 0x0B) {
        dual_boot = true;
        break;
      }
    }
  }

  if (dual_boot) {
    terminal_set_color(0x0C); /* Red */
    terminal_writestring("WARNING: Other OS detected. Dual-boot enabled.\n");
    terminal_set_color(0x07);
    terminal_writestring("Installation will preserve existing partitions.\n");
  } else {
    terminal_writestring("No other OS detected. Clean install.\n");
  }

  terminal_writestring("Proceed with installation? (y/n): ");
  char c = keyboard_getchar();
  terminal_putchar(c);
  terminal_putchar('\n');

  if (c != 'y' && c != 'Y') {
    terminal_writestring("Installation aborted.\n");
    return;
  }

  /* 2. Partitioning */
  terminal_writestring("Partitioning disk...\n");
  struct mbr new_mbr;
  mbr_create_partition_table(&new_mbr, 20480);
  terminal_writestring("  [1/9] MBR created\n");
  mbr_write(&new_mbr);
  terminal_writestring("  [2/9] MBR written to disk\n");

  /* 3. Formatting */
  terminal_writestring("Formatting NovexOS partition (FAT32)...\n");
  fat32_format_partition(2048, 20480 - 2048);
  terminal_writestring("  [3/9] FAT32 format done\n");

  /* 3a. Re-mount FS */
  fat32_init();
  terminal_writestring("  [4/9] FAT32 re-mounted\n");

  /* 3b. Copy Files (Real Kernel) */
  terminal_writestring("  [5/9] Scanning RAM for kernel magic...\n");

  extern char _kernel_start[];
  extern char _kernel_end[];
  uint32_t kernel_max_size = (uint32_t)(_kernel_end - _kernel_start);

  /* Scan memory from 0x100000 (1MB) up to 0x120000 (128KB scan) for magic */
  uint32_t *magic_search = (uint32_t *)0x100000;
  uint32_t *found_magic_addr = NULL;

  for (uint32_t i = 0; i < 32768; i++) {
    if (magic_search[i] == 0x1BADB002) {
      found_magic_addr = &magic_search[i];
      break;
    }
  }

  uint8_t *kernel_source;
  if (found_magic_addr) {
    terminal_writestring("  Multiboot magic found at: ");
    fat32_print_hex((uint32_t)(uintptr_t)found_magic_addr);
    terminal_putchar('\n');
    kernel_source = (uint8_t *)found_magic_addr;
  } else {
    terminal_set_color(0x0E);
    terminal_writestring("  WARNING: Magic NOT found, using _kernel_start\n");
    terminal_set_color(0x07);
    kernel_source = (uint8_t *)_kernel_start;
  }

  terminal_writestring("  Kernel size: ");
  char kbuf[12];
  int_to_str(kernel_max_size, kbuf);
  terminal_writestring(kbuf);
  terminal_writestring(" bytes\n");

  terminal_writestring("  [6/9] Writing KERNEL.SYS...\n");
  /* Cap at 255 sectors (127KB) maximum for ata_write_sectors (uint8_t) */
  uint32_t write_size = kernel_max_size;
  if (write_size > 255 * 512)
    write_size = 255 * 512;
  fat32_write_file("KERNEL.SYS", kernel_source, write_size);
  terminal_writestring("  [7/9] KERNEL.SYS written\n");

  /* Get KERNEL.SYS location on disk to patch Stage 2 */
  uint32_t k_lba = fat32_get_file_first_cluster("KERNEL.SYS");
  terminal_writestring("  [8/9] Stage 2 loader patch...\n");

  /* 4. Install Stage 2 Loader */
  terminal_writestring("Installing Stage 2 loader... \n");
  extern char _binary_build_loader_bin_start[];
  extern char _binary_build_loader_bin_end[];
  uint32_t loader_size =
      (uint32_t)(_binary_build_loader_bin_end - _binary_build_loader_bin_start);

  /* Write Stage 2 to sectors 1-8 */
  uint8_t loader_temp[4096];
  memset(loader_temp, 0, 4096);
  memcpy(loader_temp, _binary_build_loader_bin_start,
         loader_size > 4096 ? 4096 : loader_size);

  /* Get Kernel Entry Point */
  extern void _start(void);
  uint32_t k_entry = (uint32_t)(uintptr_t)_start;

  /* Calculate kernel LBA accurately. */
  extern uint32_t fat32_cluster_to_lba(uint32_t cluster);
  uint32_t boot_lba = fat32_cluster_to_lba(k_lba);

  /* Stage 2 metadata starting at offset 496: LBA, Sectors, Entry */
  *(uint32_t *)(loader_temp + 496) = boot_lba;
  *(uint32_t *)(loader_temp + 500) = (write_size + 511) / 512;
  *(uint32_t *)(loader_temp + 504) = k_entry;

  ata_write_sectors(1, 8, loader_temp);
  terminal_writestring("  [9/9] Stage 2 written to sectors 1-8\n");

  /* Verify KERNEL.SYS */
  terminal_writestring("Verification: ");
  uint8_t head[512];
  if (fat32_read_file("KERNEL.SYS", head, 512) > 0) {
    uint32_t magic = *(uint32_t *)head;
    fat32_print_hex(magic);
    if (magic == 0x1BADB002) {
      terminal_writestring(" (DISK OK)\n");
    } else {
      terminal_set_color(0x0C);
      terminal_writestring(" (DISK FAIL: MAGIC MISMATCH)\n");
      terminal_set_color(0x07);
    }
  } else {
    terminal_writestring("(READ ERROR)\n");
  }

  terminal_writestring("Stage 2 installed successfully. \n");

  terminal_set_color(0x0A); /* Green */
  terminal_writestring("Installation SUCCESSFUL!\n");
  terminal_set_color(0x07);

  terminal_writestring("PLEASE REMOVE INSTALLATION MEDIA (ISO) NOW.\n");
  terminal_writestring("Press any key to reboot from the disk...\n");
  keyboard_getchar();

  terminal_writestring("\nRebooting...\n");
  timer_sleep(10);
  reboot();
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
  const char *filename = skip_spaces(args);
  editor_open(*filename ? filename : "untitled.txt");
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
  else if (strcmp(c, "install") == 0)
    cmd_install();
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
    python_run_file(c + 7);
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
    if (!editor_is_active())
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