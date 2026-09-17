/*
 * NovexOS - fat32.c
 * FAT32 Driver Implementation (Minimal).
 * Supports reading files in Root Directory.
 */

#include "fat32.h"
#include "ata.h"
#include "heap.h"
#include "io.h"
#include "mbr.h"
#include "string.h"

/* External Terminal */
extern void terminal_writestring(const char *s);
extern void terminal_putchar(char c);

/* BPB for FAT32 */
struct bpb32 {
  uint8_t jmp[3];
  char oem[8];
  uint16_t bytes_per_sector;
  uint8_t sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t fats_count;
  uint16_t root_entries_count; /* 0 for FAT32 */
  uint16_t total_sectors_16;   /* 0 for FAT32 */
  uint8_t media_type;
  uint16_t sectors_per_fat_16; /* 0 for FAT32 */
  uint16_t sectors_per_track;
  uint16_t heads_count;
  uint32_t hidden_sectors;
  uint32_t total_sectors_32;

  /* FAT32 Extended */
  uint32_t sectors_per_fat_32;
  uint16_t ext_flags;
  uint16_t fs_version;
  uint32_t root_cluster;
  uint16_t fs_info;
  uint16_t backup_boot_sector;
  uint8_t reserved[12];
  uint8_t drive_number;
  uint8_t reserved1;
  uint8_t signature;
  uint32_t vol_id;
  char vol_label[11];
  char fs_type[8];
} __attribute__((packed));

/* FAT32 FSInfo Sector */
struct fat32_fsinfo {
  uint32_t lead_sig; /* 0x41615252 */
  uint8_t reserved1[480];
  uint32_t struc_sig;  /* 0x61417272 */
  uint32_t free_count; /* 0xFFFFFFFF = unknown */
  uint32_t next_free;  /* 0xFFFFFFFF = unknown */
  uint8_t reserved2[12];
  uint32_t trail_sig; /* 0xAA550000 */
} __attribute__((packed));

/* Directory Entry (Same as FAT16) */
struct fat_dir_entry {
  char name[8];
  char ext[3];
  uint8_t attr;
  uint8_t reserved;
  uint8_t ct_teenth;
  uint16_t ct_time;
  uint16_t ct_date;
  uint16_t last_access_date;
  uint16_t first_cluster_high;
  uint16_t last_mod_time;
  uint16_t last_mod_date;
  uint16_t first_cluster_low;
  uint32_t size;
} __attribute__((packed));

void fat32_format_partition(uint32_t lba_start, uint32_t sector_count,
                            void (*cb)(int, const char *));
int fat32_write_file(const char *filename, uint8_t *buffer, uint32_t len);

static uint32_t part_lba_start = 0;
static struct bpb32 fs_bpb;
static uint32_t fat_start_lba;
static uint32_t data_start_lba;

/* Helpers */
static void fat_name_to_str(struct fat_dir_entry *entry, char *buf) {
  int i, j = 0;
  for (i = 0; i < 8 && entry->name[i] != ' '; i++)
    buf[j++] = entry->name[i];
  if (entry->ext[0] != ' ') {
    buf[j++] = '.';
    for (i = 0; i < 3 && entry->ext[i] != ' '; i++)
      buf[j++] = entry->ext[i];
  }
  buf[j] = '\0';
}

static uint32_t next_free_cluster = 3;

void fat32_reset_allocator(void) { next_free_cluster = 3; }

static void fat32_to_name83(const char *filename, char *name83) {
  memset(name83, ' ', 11);
  const char *dot = strchr(filename, '.');
  int name_len = dot ? (int)(dot - filename) : (int)strlen(filename);
  if (name_len > 8)
    name_len = 8;
  for (int j = 0; j < name_len; j++) {
    char c = filename[j];
    if (c >= 'a' && c <= 'z')
      c -= 32;
    name83[j] = c;
  }
  if (dot) {
    int ext_len = (int)strlen(dot + 1);
    if (ext_len > 3)
      ext_len = 3;
    for (int j = 0; j < ext_len; j++) {
      char c = (dot + 1)[j];
      if (c >= 'a' && c <= 'z')
        c -= 32;
      name83[8 + j] = c;
    }
  }
}

static void fat32_scan_free_cluster(void) {
  if (part_lba_start == 0 || fs_bpb.sectors_per_cluster == 0)
    return;

  uint32_t root_sector =
      data_start_lba + ((fs_bpb.root_cluster - 2) * fs_bpb.sectors_per_cluster);
  uint8_t sector_buf[512];
  struct fat_dir_entry *entry;
  uint32_t max_cluster = 3;
  uint32_t cluster_size = fs_bpb.sectors_per_cluster * 512;

  for (int s = 0; s < fs_bpb.sectors_per_cluster; s++) {
    ata_read_sectors(root_sector + s, 1, sector_buf);
    for (int i = 0; i < 16; i++) {
      entry = (struct fat_dir_entry *)(sector_buf + i * 32);
      if (entry->name[0] == 0x00)
        goto done;
      if ((uint8_t)entry->name[0] == 0xE5)
        continue;
      if (entry->attr & 0x0F)
        continue;

      uint32_t c = ((uint32_t)entry->first_cluster_high << 16) |
                   entry->first_cluster_low;
      if (c >= 2) {
        uint32_t num_clusters = (entry->size + cluster_size - 1) / cluster_size;
        if (num_clusters == 0)
          num_clusters = 1;
        if (c + num_clusters > max_cluster) {
          max_cluster = c + num_clusters;
        }
      }
    }
  }
done:
  next_free_cluster = max_cluster;
}

/* -----------------------------------------------------------------------
 * fat32_mount_lba: mount a FAT32 volume from a given LBA.
 * Updates all global variables (part_lba_start, fs_bpb,
 * fat_start_lba, data_start_lba). Returns 1 on success, 0 on failure.
 * Called by fat32_init() AND by fat32_format_partition().
 * ----------------------------------------------------------------------- */
static int fat32_mount_lba(uint32_t lba) {
  if (lba == 0)
    return 0;

  uint8_t buf[512];
  ata_read_sectors(lba, 1, buf);

  /* Verify the 0x55AA boot signature */
  if (buf[510] != 0x55 || buf[511] != 0xAA)
    return 0;

  /* Verify that this is indeed FAT32 */
  struct bpb32 *b = (struct bpb32 *)buf;
  if (b->bytes_per_sector == 0 || b->sectors_per_cluster == 0)
    return 0;
  if (b->sectors_per_fat_32 == 0)
    return 0;

  /* All good: copy the BPB and compute offsets */
  memcpy(&fs_bpb, b, sizeof(struct bpb32));
  part_lba_start = lba;
  fat_start_lba = lba + fs_bpb.reserved_sectors;
  data_start_lba =
      fat_start_lba + (fs_bpb.fats_count * fs_bpb.sectors_per_fat_32);
  fat32_scan_free_cluster();
  return 1;
}

void fat32_init(void) {
  /* When booting from USB/CD, the hard disk may not have
   * a valid FAT32 partition. We try anyway: if it fails,
   * part_lba_start stays 0 and FAT32 functions return -1. */
  part_lba_start = 0;

  uint32_t lba = mbr_find_partition();
  if (lba == 0) {
    terminal_writestring("FAT32: no partition found, using RAMFS\n");
    return;
  }
  if (!fat32_mount_lba(lba)) {
    terminal_writestring("FAT32: invalid BPB, using RAMFS\n");
    return;
  }
  terminal_writestring("FAT32: mounted OK\n");
}

static uint32_t fat32_find_dir_cluster(const char *path) {
  if (!path || *path == '\0' || strcmp(path, "/") == 0 ||
      strcmp(path, "disk:/") == 0 || strcmp(path, "disk:") == 0)
    return fs_bpb.root_cluster;

  if (strncmp(path, "disk:/", 6) == 0)
    path += 6;
  else if (strncmp(path, "disk:", 5) == 0)
    path += 5;
  while (*path == '/')
    path++;
  if (*path == '\0')
    return fs_bpb.root_cluster;

  /* Look up directory in root directory */
  char name83[11];
  fat32_to_name83(path, name83);

  uint32_t root_sector =
      data_start_lba + ((fs_bpb.root_cluster - 2) * fs_bpb.sectors_per_cluster);
  uint8_t sector_buf[512];

  for (int s = 0; s < fs_bpb.sectors_per_cluster; s++) {
    ata_read_sectors(root_sector + s, 1, sector_buf);
    for (int i = 0; i < 16; i++) {
      struct fat_dir_entry *entry = (struct fat_dir_entry *)(sector_buf + i * 32);
      if (entry->name[0] == 0x00)
        return 0;
      if ((uint8_t)entry->name[0] == 0xE5)
        continue;
      if ((entry->attr & 0x10) && memcmp(entry->name, name83, 11) == 0) {
        uint32_t c = ((uint32_t)entry->first_cluster_high << 16) | entry->first_cluster_low;
        return (c == 0) ? fs_bpb.root_cluster : c;
      }
    }
  }
  return 0;
}

static uint32_t fat32_resolve_dir(const char *path, const char **out_leaf) {
  if (part_lba_start == 0 || fs_bpb.sectors_per_cluster == 0)
    return 0;

  uint32_t current_cluster = fs_bpb.root_cluster;
  if (!path || *path == '\0') {
    if (out_leaf)
      *out_leaf = "";
    return current_cluster;
  }

  if (strncmp(path, "disk:/", 6) == 0)
    path += 6;
  else if (strncmp(path, "disk:", 5) == 0)
    path += 5;
  while (*path == '/')
    path++;

  if (*path == '\0') {
    if (out_leaf)
      *out_leaf = "";
    return current_cluster;
  }

  const char *slash = strchr(path, '/');
  if (slash) {
    char dir_part[32];
    int len = (int)(slash - path);
    if (len > 31)
      len = 31;
    memcpy(dir_part, path, len);
    dir_part[len] = '\0';

    uint32_t sub_cluster = fat32_find_dir_cluster(dir_part);
    if (sub_cluster != 0) {
      current_cluster = sub_cluster;
      path = slash + 1;
      while (*path == '/')
        path++;
    }
  }

  if (out_leaf)
    *out_leaf = path;
  return current_cluster;
}

static int fat32_is_protected(const char *name83) {
  if (memcmp(name83, "SYSTEM     ", 11) == 0)
    return 1;
  if (memcmp(name83, "KERNEL  SYS", 11) == 0)
    return 1;
  if (memcmp(name83, "SETTINGSDAT", 11) == 0)
    return 1;
  return 0;
}

int fat32_is_dir(const char *name) {
  uint32_t c = fat32_find_dir_cluster(name);
  return (c != 0 && c != fs_bpb.root_cluster) ? 1 : 0;
}

void fat32_ls(const char *path) {
  if (part_lba_start == 0 || fs_bpb.sectors_per_cluster == 0)
    return;

  uint32_t cluster = fat32_find_dir_cluster(path);
  if (cluster == 0) {
    terminal_writestring("Directory not found.\n");
    return;
  }

  uint32_t dir_sector =
      data_start_lba + ((cluster - 2) * fs_bpb.sectors_per_cluster);
  uint8_t buffer[512];
  struct fat_dir_entry *entry;
  char name_buf[13];

  for (int s = 0; s < fs_bpb.sectors_per_cluster; s++) {
    ata_read_sectors(dir_sector + s, 1, buffer);
    for (int i = 0; i < 16; i++) {
      entry = (struct fat_dir_entry *)(buffer + i * 32);
      if (entry->name[0] == 0x00)
        return;
      if ((uint8_t)entry->name[0] == 0xE5)
        continue;
      if ((entry->attr & 0x0F) == 0x0F)
        continue;

      fat_name_to_str(entry, name_buf);
      if (entry->attr & 0x10) {
        terminal_writestring("  [DIR] ");
      } else {
        terminal_writestring("        ");
      }
      terminal_writestring(name_buf);

      if (!(entry->attr & 0x10)) {
        size_t nlen = strlen(name_buf);
        while (nlen < 16) {
          terminal_putchar(' ');
          nlen++;
        }
        char sbuf[12];
        int_to_str(entry->size, sbuf);
        terminal_writestring(sbuf);
        terminal_writestring(" bytes");
      }
      terminal_writestring("\n");
    }
  }
}

int fat32_mkdir(const char *dirname) {
  if (part_lba_start == 0 || fs_bpb.sectors_per_cluster == 0)
    return -1;

  if (!dirname || *dirname == '\0')
    return -1;

  /* Resolve the parent directory cluster (supports "SUBDIR/NAME" paths,
   * one level deep, same as fat32_write_file/fat32_delete). */
  const char *leaf = dirname;
  uint32_t parent_cluster = fat32_resolve_dir(dirname, &leaf);
  if (parent_cluster == 0)
    parent_cluster = fs_bpb.root_cluster;
  if (*leaf == '\0')
    return -1;

  char name83[11];
  fat32_to_name83(leaf, name83);

  uint32_t parent_sector =
      data_start_lba + ((parent_cluster - 2) * fs_bpb.sectors_per_cluster);
  uint8_t sector_buf[512];
  struct fat_dir_entry *entry;

  int first_free_sector = -1;
  int first_free_idx = -1;

  for (int s = 0; s < fs_bpb.sectors_per_cluster; s++) {
    ata_read_sectors(parent_sector + s, 1, sector_buf);
    for (int i = 0; i < 16; i++) {
      entry = (struct fat_dir_entry *)(sector_buf + i * 32);
      if (entry->name[0] != 0x00 && (uint8_t)entry->name[0] != 0xE5) {
        if (memcmp(entry->name, name83, 11) == 0) {
          return -1; /* Already exists */
        }
      } else if (first_free_sector < 0) {
        first_free_sector = s;
        first_free_idx = i;
      }
    }
  }

  if (first_free_sector < 0)
    return -1;

  uint32_t new_cluster = next_free_cluster++;

  /* Mark cluster in FAT table as EOF */
  {
    uint32_t this_fat_sec = fat_start_lba + (new_cluster * 4 / 512);
    ata_read_sectors(this_fat_sec, 1, sector_buf);
    ((uint32_t *)sector_buf)[new_cluster % 128] = 0x0FFFFFFF;
    ata_write_sectors(this_fat_sec, 1, sector_buf);

    if (fs_bpb.fats_count >= 2) {
      uint32_t fat2_sec = this_fat_sec + fs_bpb.sectors_per_fat_32;
      ata_write_sectors(fat2_sec, 1, sector_buf);
    }
  }

  /* Initialize directory cluster with . and .. entries */
  memset(sector_buf, 0, 512);
  struct fat_dir_entry *dot = (struct fat_dir_entry *)sector_buf;
  memset(dot->name, ' ', 11);
  dot->name[0] = '.';
  dot->attr = 0x10;
  dot->first_cluster_low = (uint16_t)(new_cluster & 0xFFFF);
  dot->first_cluster_high = (uint16_t)(new_cluster >> 16);
  dot->size = 0;

  struct fat_dir_entry *dotdot = (struct fat_dir_entry *)(sector_buf + 32);
  memset(dotdot->name, ' ', 11);
  dotdot->name[0] = '.';
  dotdot->name[1] = '.';
  dotdot->attr = 0x10;
  /* By FAT convention, ".." pointing at the root directory is stored as
   * cluster 0, even though the root's real cluster is fs_bpb.root_cluster. */
  if (parent_cluster == fs_bpb.root_cluster) {
    dotdot->first_cluster_low = 0;
    dotdot->first_cluster_high = 0;
  } else {
    dotdot->first_cluster_low = (uint16_t)(parent_cluster & 0xFFFF);
    dotdot->first_cluster_high = (uint16_t)(parent_cluster >> 16);
  }
  dotdot->size = 0;

  uint32_t new_dir_lba =
      data_start_lba + ((new_cluster - 2) * fs_bpb.sectors_per_cluster);
  ata_write_sectors(new_dir_lba, 1, sector_buf);

  if (fs_bpb.sectors_per_cluster > 1) {
    memset(sector_buf, 0, 512);
    for (uint32_t s = 1; s < fs_bpb.sectors_per_cluster; s++) {
      ata_write_sectors(new_dir_lba + s, 1, sector_buf);
    }
  }

  /* Write entry in parent directory */
  ata_read_sectors(parent_sector + first_free_sector, 1, sector_buf);
  entry = (struct fat_dir_entry *)(sector_buf + first_free_idx * 32);
  memset(entry, 0, 32);
  memcpy(entry->name, name83, 11);
  entry->attr = 0x10;
  entry->first_cluster_low = (uint16_t)(new_cluster & 0xFFFF);
  entry->first_cluster_high = (uint16_t)(new_cluster >> 16);
  entry->size = 0;
  ata_write_sectors(parent_sector + first_free_sector, 1, sector_buf);

  return 0;
}

int fat32_delete(const char *name) {
  if (part_lba_start == 0 || fs_bpb.sectors_per_cluster == 0)
    return -1;

  if (!name || *name == '\0')
    return -1;

  if (strncmp(name, "disk:/", 6) == 0)
    name += 6;
  else if (strncmp(name, "disk:", 5) == 0)
    name += 5;
  while (*name == '/')
    name++;

  const char *leaf = name;
  uint32_t dir_cluster = fat32_resolve_dir(name, &leaf);
  if (dir_cluster == 0)
    dir_cluster = fs_bpb.root_cluster;

  char name83[11];
  fat32_to_name83(leaf, name83);

  /* Check protection: DO NOT allow deleting the SYSTEM directory or its
   * critical files, wherever they are referenced from (root, or a copy
   * inside SYSTEM/ itself). */
  if (fat32_is_protected(name83)) {
    return -2; /* Protected */
  }

  uint32_t dir_sector =
      data_start_lba + ((dir_cluster - 2) * fs_bpb.sectors_per_cluster);
  uint8_t sector_buf[512];
  struct fat_dir_entry *entry;

  for (int s = 0; s < fs_bpb.sectors_per_cluster; s++) {
    ata_read_sectors(dir_sector + s, 1, sector_buf);
    for (int i = 0; i < 16; i++) {
      entry = (struct fat_dir_entry *)(sector_buf + i * 32);
      if (entry->name[0] == 0x00)
        return -1;
      if ((uint8_t)entry->name[0] == 0xE5)
        continue;
      if (memcmp(entry->name, name83, 11) == 0) {
        entry->name[0] = (char)0xE5;
        ata_write_sectors(dir_sector + s, 1, sector_buf);
        return 0;
      }
    }
  }
  return -1;
}

int fat32_delete_file(const char *filename) {
  return fat32_delete(filename);
}

int fat32_read_file(const char *filename, uint8_t *buffer, uint32_t max_len) {
  if (part_lba_start == 0 || fs_bpb.sectors_per_cluster == 0)
    return -1;

  const char *leaf = filename;
  uint32_t dir_cluster = fat32_resolve_dir(filename, &leaf);
  if (dir_cluster == 0)
    dir_cluster = fs_bpb.root_cluster;

  uint32_t root_sector =
      data_start_lba + ((dir_cluster - 2) * fs_bpb.sectors_per_cluster);
  uint8_t sector_buf[512];
  struct fat_dir_entry *entry;
  char name83[11];
  fat32_to_name83(leaf, name83);

  for (int s = 0; s < fs_bpb.sectors_per_cluster; s++) {
    ata_read_sectors(root_sector + s, 1, sector_buf);
    for (int i = 0; i < 16; i++) {
      entry = (struct fat_dir_entry *)(sector_buf + i * 32);
      if (entry->name[0] == 0)
        return -1;
      if (memcmp(entry->name, name83, 11) == 0) {
        uint32_t cluster = ((uint32_t)entry->first_cluster_high << 16) |
                           entry->first_cluster_low;
        uint32_t size = entry->size;
        if (size > max_len)
          size = max_len;

        uint32_t lba =
            data_start_lba + ((cluster - 2) * fs_bpb.sectors_per_cluster);
        uint32_t full_sectors = size / 512;
        uint32_t rem_bytes = size % 512;

        if (full_sectors > 0) {
          ata_read_sectors(lba, full_sectors, buffer);
        }
        if (rem_bytes > 0) {
          uint8_t tail_sec[512];
          ata_read_sectors(lba + full_sectors, 1, tail_sec);
          memcpy(buffer + (full_sectors * 512), tail_sec, rem_bytes);
        }
        return (int)size;
      }
    }
  }
  return -1;
}

int fat32_write_file(const char *filename, uint8_t *buffer, uint32_t len) {
  serial_print("[FAT32] write_file: enter, len=");
  serial_print_hex(len);
  serial_print("\r\n");

  if (part_lba_start == 0 || fs_bpb.sectors_per_cluster == 0) {
    serial_print("[FAT32] write_file: part_lba_start == 0, abort\r\n");
    return -1;
  }

  const char *leaf = filename;
  uint32_t dir_cluster = fat32_resolve_dir(filename, &leaf);
  if (dir_cluster == 0)
    dir_cluster = fs_bpb.root_cluster;

  uint32_t root_sector =
      data_start_lba + ((dir_cluster - 2) * fs_bpb.sectors_per_cluster);
  uint8_t sector_buf[512];
  struct fat_dir_entry *entry = NULL;

  char name83[11];
  fat32_to_name83(leaf, name83);

  /* Find existing entry or first free entry */
  int target_sector = -1;
  int target_idx = -1;
  int first_free_sector = -1;
  int first_free_idx = -1;

  for (int s = 0; s < fs_bpb.sectors_per_cluster; s++) {
    ata_read_sectors(root_sector + s, 1, sector_buf);
    for (int i = 0; i < 16; i++) {
      entry = (struct fat_dir_entry *)(sector_buf + i * 32);
      if (entry->name[0] != 0x00 && (uint8_t)entry->name[0] != 0xE5) {
        if (memcmp(entry->name, name83, 11) == 0) {
          target_sector = s;
          target_idx = i;
          goto found_slot;
        }
      } else if (first_free_sector < 0) {
        first_free_sector = s;
        first_free_idx = i;
      }
    }
  }

  if (first_free_sector >= 0) {
    target_sector = first_free_sector;
    target_idx = first_free_idx;
  } else {
    serial_print("[FAT32] write_file: NO free dir entry found, abort\r\n");
    return -1;
  }

found_slot:
  ata_read_sectors(root_sector + target_sector, 1, sector_buf);
  entry = (struct fat_dir_entry *)(sector_buf + target_idx * 32);

  /* Prepare entry */
  memset(entry, 0, 32);
  memcpy(entry->name, name83, 11);

  uint32_t current_cluster = next_free_cluster;
  entry->first_cluster_low = (uint16_t)(current_cluster & 0xFFFF);
  entry->first_cluster_high = (uint16_t)(current_cluster >> 16);
  entry->size = len;

  /* Write directory sector */
  ata_write_sectors(root_sector + target_sector, 1, sector_buf);

  uint32_t sectors_to_write = (len + 511) / 512;
  if (sectors_to_write == 0)
    return 0;

  uint32_t clusters_to_write =
      (sectors_to_write + fs_bpb.sectors_per_cluster - 1) /
      fs_bpb.sectors_per_cluster;

  serial_print("[FAT32] write_file: FAT update start, clusters_to_write=");
  serial_print_hex(clusters_to_write);
  serial_print(" start_cluster=");
  serial_print_hex(current_cluster);
  serial_print("\r\n");

  /* Update FAT to link all allocated clusters */
  {
    uint32_t i = 0;
    while (i < clusters_to_write) {
      uint32_t this_cluster = current_cluster + i;
      uint32_t this_fat_sec = fat_start_lba + (this_cluster * 4 / 512);

      ata_read_sectors(this_fat_sec, 1, sector_buf);

      while (i < clusters_to_write) {
        uint32_t c = current_cluster + i;
        if (fat_start_lba + (c * 4 / 512) != this_fat_sec)
          break;
        uint32_t next_val = (i == clusters_to_write - 1) ? 0x0FFFFFFF : (c + 1);
        ((uint32_t *)sector_buf)[c % 128] = next_val;
        i++;
      }

      ata_write_sectors(this_fat_sec, 1, sector_buf);

      if (fs_bpb.fats_count >= 2) {
        uint32_t fat2_sec = this_fat_sec + fs_bpb.sectors_per_fat_32;
        ata_write_sectors(fat2_sec, 1, sector_buf);
      }
    }
  }

  serial_print("[FAT32] write_file: FAT update done, data write start, "
               "sectors_to_write=");
  serial_print_hex(sectors_to_write);
  serial_print("\r\n");

  uint32_t remaining_sectors = sectors_to_write;
  uint32_t current_lba =
      data_start_lba + ((current_cluster - 2) * fs_bpb.sectors_per_cluster);
  uint8_t *data_ptr = buffer;

  while (remaining_sectors > 0) {
    uint32_t chunk = remaining_sectors > 255 ? 255 : remaining_sectors;
    ata_write_sectors(current_lba, (uint8_t)chunk, data_ptr);
    current_lba += chunk;
    data_ptr += chunk * 512;
    remaining_sectors -= chunk;
  }

  next_free_cluster += clusters_to_write;

  serial_print("[FAT32] write_file: DONE, returning 0\r\n");
  return 0;
}

int fat32_create_file(const char *filename) {
  return fat32_write_file(filename, (uint8_t *)"", 0);
}
void fat32_format(void) {
  uint32_t lba = mbr_find_partition();
  if (lba == 0) {
    terminal_writestring("No partition found for formatting.\n");
    return;
  }
  fat32_format_partition(lba, 20480, NULL); /* Assuming 10MB disk for now */
}

void fat32_format_partition(uint32_t lba_start, uint32_t sector_count,
                            void (*cb)(int, const char *)) {
  uint8_t buffer[512];
  memset(buffer, 0, 512);

  struct bpb32 *bpb = (struct bpb32 *)buffer;
  memcpy(bpb->jmp, "\xEB\x58\x90", 3);
  memcpy(bpb->oem, "OMEGA OS", 8);
  bpb->bytes_per_sector = 512;
  bpb->sectors_per_cluster = 8;
  bpb->reserved_sectors = 32;
  bpb->fats_count = 2;
  bpb->media_type = 0xF8;
  bpb->total_sectors_32 = sector_count;
  /* Microsoft FAT32 spec formula (fatgen103.pdf):
   * TmpVal1 = DskSize - ResvdSecCnt
   * TmpVal2 = (256 * SecPerClus + NumFATs) / 2   [FAT32]
   * FATSz   = ceil(TmpVal1 / TmpVal2)            */
  {
    uint32_t tmp1 = sector_count - (uint32_t)bpb->reserved_sectors;
    uint32_t tmp2 =
        (256 * (uint32_t)bpb->sectors_per_cluster + (uint32_t)bpb->fats_count) /
        2;
    bpb->sectors_per_fat_32 = (tmp1 + tmp2 - 1) / tmp2;
  }
  bpb->root_cluster = 2;
  bpb->fs_info = 1;
  bpb->backup_boot_sector = 6;
  bpb->signature = 0x29;
  memcpy(bpb->vol_label, "OMEGA DISK ", 11);
  memcpy(bpb->fs_type, "FAT32   ", 8);
  buffer[510] = 0x55;
  buffer[511] = 0xAA;

  /* Local offsets for formatting */
  uint32_t local_fat_lba = lba_start + bpb->reserved_sectors;
  uint32_t local_data_lba =
      local_fat_lba + (bpb->fats_count * bpb->sectors_per_fat_32);

  /* Write Boot Sector */
  ata_write_sectors(lba_start, 1, buffer);

  /* Write Backup Boot Sector (sector 6) */
  ata_write_sectors(lba_start + 6, 1, buffer);

  /* Write FSInfo Sector (sector 1 and 7) */
  memset(buffer, 0, 512);
  struct fat32_fsinfo *fsi = (struct fat32_fsinfo *)buffer;
  fsi->lead_sig = 0x41615252;
  fsi->struc_sig = 0x61417272;
  fsi->free_count = 0xFFFFFFFF; /* Unknown - simple formatter */
  fsi->next_free = 0xFFFFFFFF;  /* Unknown */
  fsi->trail_sig = 0xAA550000;
  ata_write_sectors(lba_start + 1, 1, buffer);
  ata_write_sectors(lba_start + 7, 1, buffer);

  /* Batch zeroing: allocate 32KB zero buffer on heap */
  void *zero_ptr = kmalloc(32768);
  if (!zero_ptr) {
    terminal_writestring("FAT32 Error: Could not allocate format buffer\n");
    return;
  }
  memset(zero_ptr, 0, 32768);

  /* Clear FATs (2 copies) */
  uint32_t fat_sectors = bpb->sectors_per_fat_32 * 2;
  uint32_t current_fat_lba = local_fat_lba;
  uint32_t remaining = fat_sectors;

  while (remaining > 0) {
    uint32_t chunk = (remaining > 64) ? 64 : remaining;
    ata_write_sectors(current_fat_lba, (uint8_t)chunk, zero_ptr);
    current_fat_lba += chunk;
    remaining -= chunk;

    if (cb) {
      int pct = 5 + (int)(((fat_sectors - remaining) * 90) / fat_sectors);
      cb(pct, "Zeroing FAT tables...");
    }
  }

  /* Initialize FAT: Cluster 0, 1 (Reserved), Cluster 2 (Root) */
  uint8_t first_fat_sector[512];
  memset(first_fat_sector, 0, 512);
  uint32_t *fat = (uint32_t *)first_fat_sector;
  fat[0] = 0x0FFFFFF8;
  fat[1] = 0x0FFFFFFF;
  fat[2] = 0x0FFFFFFF;
  /* Write both FAT copies so loaders that verify copy 2 don't fail */
  ata_write_sectors(local_fat_lba, 1, first_fat_sector);
  if (bpb->fats_count >= 2)
    ata_write_sectors(local_fat_lba + bpb->sectors_per_fat_32, 1,
                      first_fat_sector);

  /* Clear Data Area (Root Cluster: sectors_per_cluster) */
  uint32_t root_lba =
      local_data_lba + ((bpb->root_cluster - 2) * bpb->sectors_per_cluster);
  uint32_t root_remaining = bpb->sectors_per_cluster;
  uint32_t current_root_lba = root_lba;

  while (root_remaining > 0) {
    uint32_t chunk = (root_remaining > 64) ? 64 : root_remaining;
    ata_write_sectors(current_root_lba, (uint8_t)chunk, zero_ptr);
    current_root_lba += chunk;
    root_remaining -= chunk;
  }

  serial_print("[FAT32] format_partition: calling kfree(zero_ptr)\r\n");
  kfree(zero_ptr);
  serial_print("[FAT32] format_partition: kfree returned\r\n");

  /* Reset cluster allocator state */
  extern void fat32_reset_allocator(void);
  fat32_reset_allocator();
  serial_print("[FAT32] format_partition: reset_allocator done\r\n");

  terminal_writestring("FAT32: Formatting complete.\n");

  /* Mount the freshly formatted volume to initialize global variables.
   * CRITICAL: without this, sectors_per_cluster = 0 -> division by zero
   * in fat32_write_file during installation. */
  serial_print("[FAT32] format_partition: calling mount_lba\r\n");
  fat32_mount_lba(lba_start);
  serial_print("[FAT32] format_partition: mount_lba returned, EXIT\r\n");
}
uint32_t fat32_cluster_to_lba(uint32_t cluster) {
  if (part_lba_start == 0)
    return 0;
  return data_start_lba + ((cluster - 2) * fs_bpb.sectors_per_cluster);
}

uint32_t fat32_get_file_first_cluster(const char *filename) {
  if (part_lba_start == 0 || fs_bpb.sectors_per_cluster == 0)
    return 0;

  const char *leaf = filename;
  uint32_t dir_cluster = fat32_resolve_dir(filename, &leaf);
  if (dir_cluster == 0)
    dir_cluster = fs_bpb.root_cluster;

  char name83[11];
  fat32_to_name83(leaf, name83);

  uint32_t dir_sector =
      data_start_lba + ((dir_cluster - 2) * fs_bpb.sectors_per_cluster);
  uint8_t sector_buf[512];
  struct fat_dir_entry *entry;

  for (int s = 0; s < fs_bpb.sectors_per_cluster; s++) {
    ata_read_sectors(dir_sector + s, 1, sector_buf);
    for (int i = 0; i < 16; i++) {
      entry = (struct fat_dir_entry *)(sector_buf + i * 32);
      if (entry->name[0] == 0)
        break;
      if ((uint8_t)entry->name[0] == 0xE5)
        continue;
      if (memcmp(entry->name, name83, 11) == 0) {
        return ((uint32_t)entry->first_cluster_high << 16) |
               entry->first_cluster_low;
      }
    }
  }

  /* Fallback: if searching root and not found, check SYSTEM folder */
  if (dir_cluster == fs_bpb.root_cluster) {
    uint32_t sys_cluster = fat32_find_dir_cluster("SYSTEM");
    if (sys_cluster != 0) {
      uint32_t sys_sector =
          data_start_lba + ((sys_cluster - 2) * fs_bpb.sectors_per_cluster);
      for (int s = 0; s < fs_bpb.sectors_per_cluster; s++) {
        ata_read_sectors(sys_sector + s, 1, sector_buf);
        for (int i = 0; i < 16; i++) {
          entry = (struct fat_dir_entry *)(sector_buf + i * 32);
          if (entry->name[0] == 0)
            break;
          if ((uint8_t)entry->name[0] == 0xE5)
            continue;
          if (memcmp(entry->name, name83, 11) == 0) {
            return ((uint32_t)entry->first_cluster_high << 16) |
                   entry->first_cluster_low;
          }
        }
      }
    }
  }

  return 0;
}
void fat32_print_hex(uint32_t val) {
  char hex[] = "0123456789ABCDEF";
  char buf[11];
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 7; i >= 0; i--) {
    buf[i + 2] = hex[val & 0xF];
    val >>= 4;
  }
  buf[10] = '\0';
  terminal_writestring(buf);
}

/* Getter for VFS filesystem detection */
uint32_t fat32_get_part_lba_start(void) { return part_lba_start; }