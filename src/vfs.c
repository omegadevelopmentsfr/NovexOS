/*
 * NovexOS - vfs.c
 * Virtual Filesystem Layer Implementation
 * Manages auto-detection and access to FAT32, NTFS, ext4, RAMFS
 */

#include "vfs.h"
#include "ata.h"
#include "fat32.h"
#include "ramfs.h"
#include "string.h"

extern void terminal_writestring(const char *s);

/* Global filesystem state */
static fs_type_t detected_fs = FS_NONE;

/* Stub implementations for NTFS and ext4 (basic detection only) */

/* NTFS Basic Detection */
static int ntfs_detect(void) {
  /* DISABLED: Don't try to read ATA disk on USB multiboot systems */
  return 0;

  /* Original code (disabled for USB multiboot):
  uint32_t part_lba_start = fat32_get_part_lba_start();

  if (part_lba_start == 0)
    return 0;

  uint8_t buffer[512];
  ata_read_sectors(part_lba_start, 1, buffer);

  if (buffer[0] == 0xEB && buffer[1] == 0x52 && buffer[2] == 0x90) {
    if (buffer[3] == 'N' && buffer[4] == 'T' &&
        buffer[5] == 'F' && buffer[6] == 'S') {
      return 1;
    }
  }

  return 0;
  */
}

static void ntfs_init(void) {
  terminal_writestring(
      "NTFS: Detected (read-only support not yet implemented)\n");
  terminal_writestring("NTFS: Falling back to RAMFS\n");
}

static int ntfs_read_file(const char *filename, uint8_t *buffer,
                          uint32_t max_len) {
  (void)filename;
  (void)buffer;
  (void)max_len;
  return -1; /* Not implemented */
}

static void ntfs_list_dir(const char *path) {
  (void)path;
  terminal_writestring("NTFS: Directory listing not implemented\n");
}

/* ext4 Basic Detection */
static int ext4_detect(void) {
  /* DISABLED: Don't try to read ATA disk on USB multiboot systems */
  return 0;

  /* Original code (disabled for USB multiboot):
  uint32_t part_lba_start = fat32_get_part_lba_start();

  if (part_lba_start == 0)
    return 0;

  uint8_t buffer[512];
  ata_read_sectors(part_lba_start + 2, 1, buffer);

  uint16_t *sig = (uint16_t *)(buffer + 0x38);
  if (*sig == 0xEF53) {
    return 1;
  }

  return 0;
  */
}

static void ext4_init(void) {
  terminal_writestring(
      "ext4: Detected (read-only support not yet implemented)\n");
  terminal_writestring("ext4: Falling back to RAMFS\n");
}

static int ext4_read_file(const char *filename, uint8_t *buffer,
                          uint32_t max_len) {
  (void)filename;
  (void)buffer;
  (void)max_len;
  return -1; /* Not implemented */
}

static void ext4_list_dir(const char *path) {
  (void)path;
  terminal_writestring("ext4: Directory listing not implemented\n");
}

void vfs_init(void) {
  terminal_writestring("=== Filesystem Detection ===\n");

  /* Try FAT32 first (most common) */
  terminal_writestring("Initializing FAT32...\n");
  fat32_init();

  /* Check if FAT32 mounted successfully */
  uint32_t part_lba_start = fat32_get_part_lba_start();
  if (part_lba_start != 0) {
    detected_fs = FS_FAT32;
    terminal_writestring("✓ FAT32 mounted\n");
    return;
  }

  /* Try NTFS */
  if (ntfs_detect()) {
    detected_fs = FS_NTFS;
    ntfs_init();
    return;
  }

  /* Try ext4 */
  if (ext4_detect()) {
    detected_fs = FS_EXT4;
    ext4_init();
    return;
  }

  /* Fallback to RAMFS */
  terminal_writestring("No disk filesystem detected. Using RAMFS.\n");
  detected_fs = FS_RAMFS;
  ramfs_init();
  terminal_writestring("✓ RAMFS initialized\n");
}

int vfs_read_file(const char *filename, uint8_t *buffer, uint32_t max_len) {
  /* Try to read from detected filesystem */
  switch (detected_fs) {
  case FS_FAT32:
    return fat32_read_file(filename, buffer, max_len);
  case FS_NTFS:
    return ntfs_read_file(filename, buffer, max_len);
  case FS_EXT4:
    return ext4_read_file(filename, buffer, max_len);
  case FS_RAMFS:
    /* RAMFS doesn't implement file reading yet */
    return -1;
  default:
    return -1;
  }
}

void vfs_list_dir(const char *path) {
  switch (detected_fs) {
  case FS_FAT32:
    fat32_ls(path);
    break;
  case FS_NTFS:
    ntfs_list_dir(path);
    break;
  case FS_EXT4:
    ext4_list_dir(path);
    break;
  case FS_RAMFS:
    terminal_writestring("RAMFS: Directory listing not implemented\n");
    break;
  default:
    break;
  }
}

fs_type_t vfs_get_fs_type(void) { return detected_fs; }
