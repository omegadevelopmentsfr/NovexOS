/*
 * NovexOS - kernel.c
 * Kernel entry point. VGA text-mode console driver.
 * Initializes all subsystems: GDT, IDT, ISR, PIT, PMM, heap, ramfs.
 *
 * VGA text buffer at 0xB8000, 80×25, 16 colors.
 */

#include "ata.h"
#include "desktop.h"
#include "fat32.h"
#include "gdt.h"
#include "heap.h"
#include "idt.h"
#include "io.h"
#include "isr.h"
#include "keyboard.h"
#include "lang.h"
#include "mouse.h"
#include "pmm.h"
#include "ramfs.h"
#include "shell.h"
#include "string.h"
#include "timer.h"
#include "types.h"
#include "vfs.h"

int chosen_res_id = 3;

/* ------- VGA constants ------- */
#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_MEMORY 0xB8000

/* VGA colors */
enum vga_color {
  VGA_COLOR_BLACK = 0,
  VGA_COLOR_BLUE = 1,
  VGA_COLOR_GREEN = 2,
  VGA_COLOR_CYAN = 3,
  VGA_COLOR_RED = 4,
  VGA_COLOR_MAGENTA = 5,
  VGA_COLOR_BROWN = 6,
  VGA_COLOR_LIGHT_GREY = 7,
  VGA_COLOR_DARK_GREY = 8,
  VGA_COLOR_LIGHT_BLUE = 9,
  VGA_COLOR_LIGHT_GREEN = 10,
  VGA_COLOR_LIGHT_CYAN = 11,
  VGA_COLOR_LIGHT_RED = 12,
  VGA_COLOR_LIGHT_MAGENTA = 13,
  VGA_COLOR_LIGHT_BROWN = 14,
  VGA_COLOR_WHITE = 15,
};

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
  return (uint8_t)(fg | (bg << 4));
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
  return (uint16_t)uc | ((uint16_t)color << 8);
}

/* ------- Terminal state ------- */
static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t *terminal_buffer;

/* ------- Hardware cursor ------- */
static void terminal_update_cursor(void) {
  uint16_t pos = (uint16_t)(terminal_row * VGA_WIDTH + terminal_column);
  outb(0x3D4, 14);
  outb(0x3D5, (uint8_t)(pos >> 8));
  outb(0x3D4, 15);
  outb(0x3D5, (uint8_t)(pos & 0xFF));
}

/* ------- Initialize terminal ------- */
static void terminal_initialize(void) {
  terminal_row = 0;
  terminal_column = 0;
  terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
  terminal_buffer = (uint16_t *)(uintptr_t)VGA_MEMORY;

  for (size_t y = 0; y < VGA_HEIGHT; y++) {
    for (size_t x = 0; x < VGA_WIDTH; x++) {
      terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
    }
  }
  terminal_update_cursor();
}

/* ------- Scroll screen up one line ------- */
static void terminal_scroll(void) {
  for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
    for (size_t x = 0; x < VGA_WIDTH; x++) {
      terminal_buffer[y * VGA_WIDTH + x] =
          terminal_buffer[(y + 1) * VGA_WIDTH + x];
    }
  }
  /* Clear last line */
  for (size_t x = 0; x < VGA_WIDTH; x++) {
    terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] =
        vga_entry(' ', terminal_color);
  }
}

/* ------- Public: write one character ------- */
void terminal_putchar(char c) {
  if (desktop_is_active()) {
    desktop_terminal_putchar(c);
    return;
  }

  if (c == '\n') {
    terminal_column = 0;
    if (++terminal_row == VGA_HEIGHT) {
      terminal_row = VGA_HEIGHT - 1;
      terminal_scroll();
    }
  } else if (c == '\b') {
    if (terminal_column > 0) {
      terminal_column--;
      terminal_buffer[terminal_row * VGA_WIDTH + terminal_column] =
          vga_entry(' ', terminal_color);
    }
  } else if (c == '\t') {
    size_t next = (terminal_column + 4) & ~((size_t)3);
    if (next >= VGA_WIDTH)
      next = VGA_WIDTH - 1;
    while (terminal_column < next) {
      terminal_buffer[terminal_row * VGA_WIDTH + terminal_column] =
          vga_entry(' ', terminal_color);
      terminal_column++;
    }
  } else {
    terminal_buffer[terminal_row * VGA_WIDTH + terminal_column] =
        vga_entry((unsigned char)c, terminal_color);
    if (++terminal_column == VGA_WIDTH) {
      terminal_column = 0;
      if (++terminal_row == VGA_HEIGHT) {
        terminal_row = VGA_HEIGHT - 1;
        terminal_scroll();
      }
    }
  }
  terminal_update_cursor();
}

/* ------- Public: backspace ------- */
void terminal_backspace(void) {
  if (desktop_is_active()) {
    desktop_terminal_putchar('\b');
    return;
  }
  if (terminal_column > 0) {
    terminal_column--;
    terminal_buffer[terminal_row * VGA_WIDTH + terminal_column] =
        vga_entry(' ', terminal_color);
    terminal_update_cursor();
  }
}

/* ------- Public: write a string ------- */
void terminal_writestring(const char *data) {
  size_t len = strlen(data);
  for (size_t i = 0; i < len; i++) {
    terminal_putchar(data[i]);
  }
}

/* ------- Public: clear screen ------- */
void terminal_clear(void) {
  terminal_row = 0;
  terminal_column = 0;

  for (size_t y = 0; y < VGA_HEIGHT; y++) {
    for (size_t x = 0; x < VGA_WIDTH; x++) {
      terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
    }
  }
  terminal_update_cursor();
}

/* ------- Public: set/get text color ------- */
void terminal_set_color(uint8_t color) { terminal_color = color; }
uint8_t terminal_get_color(void) { return terminal_color; }

/* ------- Interactive Boot Sequence ------- */
static void show_banner(void) {
  terminal_set_color(vga_entry_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK));
  terminal_writestring("  _   _                      ____   _____ \n");
  terminal_writestring(" | \\ | |                    / __ \\ / ____|\n");
  terminal_writestring(" |  \\| | _____   _______  _| |  | | (___  \n");
  terminal_writestring(" | . ` |/ _ \\ \\ / / _ \\ \\/ / |  | |\\___ \\ \n");
  terminal_writestring(" | |\\  | (_) \\ V /  __/>  <| |__| |____) |\n");
  terminal_writestring(" |_| \\_|\\___/ \\_/ \\___/_/\\_\\\\____/|_____/\n");
}

static void interactive_boot_sequence(void) {
  terminal_clear();
  terminal_set_color(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK));
  terminal_writestring("=== NovexOS First Boot ===\n\n");

  /* 1. Language Selection */
  terminal_writestring("Select Language / Choisissez la langue:\n");
  terminal_writestring("[1] English\n");
  terminal_writestring("[2] Francais\n");
  terminal_writestring("\n> ");

  char c;
  while (1) {
    c = keyboard_getchar();
    if (c == '1' || c == '&') {
      lang_set(LANG_EN);
      break;
    }
    if (c == '2' || c == 'e') {
      lang_set(LANG_FR);
      break;
    }
  }

  /* 2. Keyboard Layout */
  terminal_clear();
  terminal_writestring(get_string(STR_SELECT_KB));
  while (1) {
    c = keyboard_getchar();
    if (c == '1' || c == '&') {
      keyboard_set_layout(0);
      break;
    }
    if (c == '2' || c == 'e') {
      keyboard_set_layout(1);
      break;
    }
  }

  /* 3. Screen Resolution */
  terminal_clear();
  terminal_writestring(get_string(STR_SELECT_RES));
  while (1) {
    c = keyboard_getchar();
    if (c == '1' || c == '&') {
      chosen_res_id = 1;
      break;
    }
    if (c == '2' || c == 'e') {
      chosen_res_id = 2;
      break;
    }
    if (c == '3' || c == '"') {
      chosen_res_id = 3;
      break;
    }
  }

  terminal_clear();
  show_banner();

  terminal_set_color(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
  terminal_writestring("\n NovexOS v0.7 - Bare Metal Monolithic Kernel\n");

  terminal_set_color(vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK));
  terminal_writestring(get_string(STR_BOOT_COMPLETE));

  terminal_set_color(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
  shell_init();

  /* Automatically launch the desktop environment */
  shell_input('s');
  shell_input('t');
  shell_input('a');
  shell_input('r');
  shell_input('t');
  shell_input('d');
  shell_input('e');
  shell_input('\n');
}

/* ------- Kernel entry point ------- */
void kernel_main(struct multiboot_info *mbi) {
  /* Safety check: Write kernel start marker to VGA */
  volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
  vga[0] = 0x2F4B; /* 'K' in green */

  /* 1. VGA console */
  terminal_initialize();

  /* Print initial boot message */
  terminal_writestring("=== NovexOS Kernel Start ===\n");
  terminal_writestring("Mode: 64-bit Long Mode\n");
  terminal_writestring("Memory: ");
  char buf[20];
  int_to_str(mbi->mem_upper, buf);
  terminal_writestring(buf);
  terminal_writestring(" KB\n");

  /* 2. GDT — our own segment descriptors */
  terminal_writestring("Initializing GDT...\n");
  gdt_init();
  terminal_writestring("GDT OK\n");

  /* 3. IDT + PIC + CPU exception ISRs */
  terminal_writestring("Initializing IDT...\n");
  idt_init();
  terminal_writestring("IDT OK\n");

  terminal_writestring("Initializing ISRs...\n");
  isr_init();
  terminal_writestring("ISRs OK\n");

  /* 4. PIT timer at 1000 Hz for 60fps rendering */
  terminal_writestring("Initializing Timer (1000Hz)...\n");
  timer_init(1000);
  terminal_writestring(get_string(STR_TIMER_OK));

  /* 5. Physical memory manager */
  terminal_writestring("Initializing PMM...\n");
  pmm_init(mbi);
  terminal_writestring("PMM OK\n");

  /* 6. Heap allocator */
  terminal_writestring("Initializing Heap...\n");
  heap_init();
  terminal_writestring("Heap OK\n");

  /* 7. RAM filesystem */
  terminal_writestring("Initializing RAMFS...\n");
  ramfs_init();
  terminal_writestring("RAMFS OK\n");

  /* 8. ATA Disk Driver */
  terminal_writestring("Initializing ATA...\n");
  ata_init();
  terminal_writestring("ATA OK\n");

  /* 9. Filesystem Detection (FAT32, NTFS, ext4, with RAMFS fallback) */
  terminal_writestring("=== Filesystem Detection ===\n");
  vfs_init();
  terminal_writestring("Filesystem OK\n");

  /* 9b. Keyboard Init */
  terminal_writestring("Initializing Keyboard...\n");
  keyboard_init();
  terminal_writestring(get_string(STR_KEYBOARD_OK));

  /* 9c. Mouse Init */
  terminal_writestring("Initializing Mouse...\n");
  mouse_init();
  terminal_writestring(get_string(STR_MOUSE_OK));

  /* 10. Enable interrupts */
  terminal_writestring("Enabling interrupts...\n");
  __asm__ volatile("sti");
  terminal_writestring("Interrupts enabled!\n");

  /* 11. Boot Complete - Start Interactive Boot Flow */
  interactive_boot_sequence();

  /* Halt loop - shell runs via keyboard interrupt handler */
  for (;;) {
    if (desktop_is_active()) {
      desktop_run();
      /* Once desktop exits, re-clear the screen to text mode */
      terminal_clear();
      interactive_boot_sequence();
    }
    __asm__ volatile("hlt");
  }
}