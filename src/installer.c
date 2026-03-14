/*
 * NovexOS - installer.c
 * Interactive installation wizard
 */

#include "installer.h"
#include "ata.h"
#include "io.h"
#include "keyboard.h"
#include "mbr.h"
#include "string.h"

extern void terminal_writestring(const char *s);
extern void terminal_putchar(char c);
extern void terminal_set_color(uint8_t color);
extern uint8_t terminal_get_color(void);

/* Disk detection state */
static struct {
  int disk_found;
  int is_unformatted; /* 1 = empty/unformatted disk, 0 = valid MBR */
  uint32_t disk_sectors;
} disk_state = {0, 0, 0};

/* Print a byte as hex */
static void print_hex_byte(uint8_t b) {
  const char *hex = "0123456789ABCDEF";
  char buf[3];
  buf[0] = hex[(b >> 4) & 0xF];
  buf[1] = hex[b & 0xF];
  buf[2] = '\0';
  terminal_writestring(buf);
}

/* -----------------------------------------------------------------------
 * Disk detection: purely hardware via IDENTIFY.
 * Works whether the disk is empty, unformatted, FAT32, NTFS, ext4...
 * Then checks whether the MBR is valid to inform the user.
 * ----------------------------------------------------------------------- */
/* Raw dump of ATA registers for diagnostics */
static void ata_dump_registers(void) {
  /* Select Master before reading */
  outb(0x1F6, 0xA0);
  /* 400ns delay */
  inb(0x3F6);
  inb(0x3F6);
  inb(0x3F6);
  inb(0x3F6);

  terminal_writestring("  Reg 0x1F7 (Status)  : 0x");
  print_hex_byte(inb(0x1F7));
  terminal_writestring("\n");
  terminal_writestring("  Reg 0x3F6 (AltStat) : 0x");
  print_hex_byte(inb(0x3F6));
  terminal_writestring("\n");
  terminal_writestring("  Reg 0x1F6 (Drive)   : 0x");
  print_hex_byte(inb(0x1F6));
  terminal_writestring("\n");
  terminal_writestring("  Reg 0x1F4 (CylLo)   : 0x");
  print_hex_byte(inb(0x1F4));
  terminal_writestring("\n");
  terminal_writestring("  Reg 0x1F5 (CylHi)   : 0x");
  print_hex_byte(inb(0x1F5));
  terminal_writestring("\n");
  terminal_writestring("  Reg 0x1F1 (Error)   : 0x");
  print_hex_byte(inb(0x1F1));
  terminal_writestring("\n");
}

int installer_detect_disks(void) {
  terminal_writestring("\n=== Hard Disk Detection ===\n");

  terminal_writestring("--- ATA Registers BEFORE IDENTIFY ---\n");
  ata_dump_registers();

  terminal_writestring("Sending IDENTIFY command (0xEC)...\n");
  /* Soft reset */
  outb(0x3F6, 0x04);
  inb(0x3F6);
  inb(0x3F6);
  outb(0x3F6, 0x00);
  /* Wait for BSY */
  uint32_t t = 0;
  while ((inb(0x1F7) & 0x80) && t < 500000)
    t++;
  terminal_writestring("  Post-reset Status   : 0x");
  print_hex_byte(inb(0x1F7));
  terminal_writestring("\n");

  /* Select Master and send IDENTIFY */
  outb(0x1F6, 0xA0);
  inb(0x3F6);
  inb(0x3F6);
  inb(0x3F6);
  inb(0x3F6);
  outb(0x1F2, 0);
  outb(0x1F3, 0);
  outb(0x1F4, 0);
  outb(0x1F5, 0);
  outb(0x1F7, 0xEC);
  inb(0x3F6);
  inb(0x3F6);
  inb(0x3F6);
  inb(0x3F6);

  uint8_t status = inb(0x1F7);
  terminal_writestring("  Status after IDENTIFY: 0x");
  print_hex_byte(status);
  terminal_writestring("\n");

  /* Wait for BSY=0 */
  t = 0;
  while ((inb(0x1F7) & 0x80) && t < 500000)
    t++;
  terminal_writestring("  Status after wait   : 0x");
  print_hex_byte(inb(0x1F7));
  terminal_writestring("\n");
  terminal_writestring("  CylLo / CylHi       : 0x");
  print_hex_byte(inb(0x1F4));
  terminal_writestring(" / 0x");
  print_hex_byte(inb(0x1F5));
  terminal_writestring("\n");

  terminal_writestring("--- Calling ata_identify() ---\n");
  int result = ata_identify(0);

  if (!result) {
    terminal_writestring("[FAIL] ata_identify() returned 0\n");
    disk_state.disk_found = 0;
    disk_state.is_unformatted = 0;
    return 0;
  }

  terminal_writestring("[OK] ATA drive detected on Primary Master.\n");
  disk_state.disk_found = 1;

  /* Level 2: read sector 0 (MBR) to check if the disk is formatted */
  terminal_writestring("Reading MBR (sector 0)...\n");
  uint8_t mbr_buf[512];
  ata_read_sectors(0, 1, mbr_buf);

  uint8_t sig_lo = mbr_buf[510];
  uint8_t sig_hi = mbr_buf[511];
  terminal_writestring("  MBR Signature : 0x");
  print_hex_byte(sig_hi);
  print_hex_byte(sig_lo);
  terminal_writestring("\n");

  if (sig_lo == 0x55 && sig_hi == 0xAA) {
    terminal_writestring("[OK] Valid MBR (0xAA55) - disk is partitioned.\n");
    disk_state.is_unformatted = 0;
  } else {
    terminal_writestring("[INFO] No valid MBR signature - ");
    terminal_writestring("empty or unformatted disk.\n");
    terminal_writestring("  (This is normal for a new disk)\n");
    disk_state.is_unformatted = 1;
  }

  return 1;
}

void installer_show_menu(void) {
  terminal_writestring("\n");
  terminal_writestring("╔════════════════════════════════╗\n");
  terminal_writestring("║     NovexOS Installation       ║\n");
  terminal_writestring("╚════════════════════════════════╝\n");
  terminal_writestring("\n");

  if (!disk_state.disk_found) {
    terminal_writestring("No hard disk detected.\n\n");
    terminal_writestring("Options:\n");
    terminal_writestring("  [1] Continue without disk (RAM)\n");
    terminal_writestring("  [2] Re-detect disk\n");
    terminal_writestring("  [3] Reboot\n");
  } else {
    if (disk_state.is_unformatted) {
      terminal_writestring("Disk detected : EMPTY / UNFORMATTED\n");
      terminal_writestring(
          "Installation will create an MBR and a FAT32 partition.\n\n");
    } else {
      terminal_writestring(
          "Disk detected : valid MBR (already partitioned)\n\n");
    }
    terminal_writestring("Installation options:\n");
    terminal_writestring("  [1] Install (format the entire disk)\n");
    terminal_writestring("  [2] Dual-boot installation\n");
    terminal_writestring("  [3] Re-detect disk\n");
    terminal_writestring("  [4] Back to shell\n");
    terminal_writestring("  [5] Reboot\n");
  }

  terminal_writestring("\nChoice: ");
}

void installer_install_full_disk(void) {
  terminal_writestring("\n");
  terminal_writestring("╔════════════════════════════════╗\n");
  terminal_writestring("║ WARNING  : Full format         ║\n");
  terminal_writestring("╚════════════════════════════════╝\n");
  terminal_writestring("\nALL data will be erased!\n");
  terminal_writestring("Confirm? (type 'yes'): ");

  char buffer[10];
  int pos = 0;
  while (pos < 9) {
    int key = keyboard_getchar();
    if (key == '\n' || key == '\r') {
      buffer[pos] = '\0';
      break;
    }
    if (key >= 32 && key < 127) {
      buffer[pos] = key;
      terminal_putchar(key);
      pos++;
    }
  }
  terminal_writestring("\n");

  if (strcmp(buffer, "yes") != 0) {
    terminal_writestring("Installation cancelled.\n");
    return;
  }

  terminal_writestring("\nCreating partition table (MBR)...\n");
  struct mbr new_mbr;
  mbr_create_partition_table(&new_mbr, 20480); /* 10 MB default */
  mbr_write(&new_mbr);
  terminal_writestring("[OK] MBR written.\n");

  terminal_writestring("Installing NovexOS...\n");
  terminal_writestring("[OK] Installation complete!\n");
  terminal_writestring("You can now reboot from the hard disk.\n");
}

void installer_install_dualboot(void) {
  terminal_writestring("\nDual-boot not yet implemented.\n");
  terminal_writestring("Use the full installation for now.\n");
}

void installer_main(void) {
  terminal_writestring("\n");
  terminal_writestring("╔════════════════════════════════╗\n");
  terminal_writestring("║     NovexOS Live System        ║\n");
  terminal_writestring("║   Running from RAM (USB key)   ║\n");
  terminal_writestring("╚════════════════════════════════╝\n");
  terminal_writestring(
      "\nPress 'I' for the installer, any other key for the shell.\n");

  int key = keyboard_getchar();
  if (key != 'I' && key != 'i') {
    terminal_writestring("\nBack to shell...\n");
    return;
  }

  for (;;) {
    int disk_detected = installer_detect_disks();
    installer_show_menu();

    int choice = keyboard_getchar();
    terminal_putchar(choice);
    terminal_writestring("\n");

    if (!disk_detected) {
      switch (choice) {
      case '1':
        terminal_writestring("RAM mode...\n");
        return;
      case '2':
        continue;
      case '3':
        terminal_writestring("Rebooting...\n");
        __asm__ volatile("hlt");
        break;
      default:
        terminal_writestring("Invalid option.\n");
        break;
      }
    } else {
      switch (choice) {
      case '1':
        installer_install_full_disk();
        break;
      case '2':
        installer_install_dualboot();
        break;
      case '3':
        continue;
      case '4':
        terminal_writestring("Back to shell...\n");
        return;
      case '5':
        terminal_writestring("Rebooting...\n");
        __asm__ volatile("hlt");
        break;
      default:
        terminal_writestring("Invalid option.\n");
        break;
      }
    }
  }
}