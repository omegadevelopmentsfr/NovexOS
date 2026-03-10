/*
 * NovexOS - fat32.c
 * FAT32 Driver Implementation (Minimal).
 * Supports reading files in Root Directory.
 */

#include "fat32.h"
#include "ata.h"
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

void fat32_format_partition(uint32_t lba_start, uint32_t sector_count);
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

/* -----------------------------------------------------------------------
 * fat32_mount_lba : monte un volume FAT32 à partir d'un LBA donné.
 * Met à jour toutes les variables globales (part_lba_start, fs_bpb,
 * fat_start_lba, data_start_lba). Retourne 1 si succès, 0 sinon.
 * Appelé par fat32_init() ET par fat32_format_partition().
 * ----------------------------------------------------------------------- */
static int fat32_mount_lba(uint32_t lba) {
  if (lba == 0)
    return 0;

  uint8_t buf[512];
  ata_read_sectors(lba, 1, buf);

  /* Vérifier la signature 0x55AA */
  if (buf[510] != 0x55 || buf[511] != 0xAA)
    return 0;

  /* Vérifier que c'est bien du FAT32 */
  struct bpb32 *b = (struct bpb32 *)buf;
  if (b->bytes_per_sector == 0 || b->sectors_per_cluster == 0)
    return 0;
  if (b->sectors_per_fat_32 == 0)
    return 0;

  /* Tout est bon : copier le BPB et calculer les offsets */
  memcpy(&fs_bpb, b, sizeof(struct bpb32));
  part_lba_start = lba;
  fat_start_lba = lba + fs_bpb.reserved_sectors;
  data_start_lba =
      fat_start_lba + (fs_bpb.fats_count * fs_bpb.sectors_per_fat_32);
  return 1;
}

void fat32_init(void) {
  /* Pendant le boot depuis USB/CD, le disque dur peut ne pas avoir
   * de partition FAT32 valide. On essaie quand même : si ça échoue,
   * part_lba_start reste 0 et les fonctions FAT32 retournent -1. */
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

void fat32_ls(const char *path) {
  (void)path;
  if (part_lba_start == 0)
    return;

  /* Root directory starts at root_cluster */
  /* Read one cluster of root dir */
  uint32_t root_sector =
      data_start_lba + ((fs_bpb.root_cluster - 2) * fs_bpb.sectors_per_cluster);

  uint8_t buffer[512];
  struct fat_dir_entry *entry;
  char name_buf[13];

  /* Iterate sectors in the cluster */
  for (int s = 0; s < fs_bpb.sectors_per_cluster; s++) {
    ata_read_sectors(root_sector + s, 1, buffer);
    for (int i = 0; i < 16; i++) {
      entry = (struct fat_dir_entry *)(buffer + i * 32);
      if (entry->name[0] == 0x00)
        return;
      if ((uint8_t)entry->name[0] == 0xE5)
        continue;
      if (entry->attr & 0x0F)
        continue;

      fat_name_to_str(entry, name_buf);
      terminal_writestring("  ");
      terminal_writestring(name_buf);
      terminal_writestring("\n");
    }
  }
}

int fat32_read_file(const char *filename, uint8_t *buffer, uint32_t max_len) {
  if (part_lba_start == 0)
    return -1;

  uint32_t root_sector =
      data_start_lba + ((fs_bpb.root_cluster - 2) * fs_bpb.sectors_per_cluster);
  uint8_t sector_buf[512];
  struct fat_dir_entry *entry;
  char name83[11];

  /* Convert target filename to 8.3 space-padded */
  memset(name83, ' ', 11);
  const char *dot = strchr(filename, '.');
  int name_len = dot ? (int)(dot - filename) : (int)strlen(filename);
  if (name_len > 8)
    name_len = 8;
  memcpy(name83, filename, name_len);
  if (dot) {
    int ext_len = (int)strlen(dot + 1);
    if (ext_len > 3)
      ext_len = 3;
    memcpy(name83 + 8, dot + 1, ext_len);
  }

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
        uint32_t sectors_to_read = (size + 511) / 512;
        if (sectors_to_read > 0) {
          ata_read_sectors(lba, sectors_to_read, buffer);
        }
        return (int)size;
      }
    }
  }
  return -1;
}

static uint32_t next_free_cluster = 3;

void fat32_reset_allocator(void) { next_free_cluster = 3; }

int fat32_write_file(const char *filename, uint8_t *buffer, uint32_t len) {
  if (part_lba_start == 0)
    return -1;

  uint32_t root_sector =
      data_start_lba + ((fs_bpb.root_cluster - 2) * fs_bpb.sectors_per_cluster);
  uint8_t sector_buf[512];
  struct fat_dir_entry *entry;

  /* Find free entry or existing same name */
  int found_sector = -1;

  for (int s = 0; s < fs_bpb.sectors_per_cluster; s++) {
    ata_read_sectors(root_sector + s, 1, sector_buf);
    for (int i = 0; i < 16; i++) {
      entry = (struct fat_dir_entry *)(sector_buf + i * 32);
      if (entry->name[0] == 0x00 || (uint8_t)entry->name[0] == 0xE5) {
        found_sector = s;
        goto found;
      }
    }
  }
  return -1;

found:
  /* Prepare entry */
  memset(entry, 0, 32);
  memset(entry->name, ' ', 11);
  const char *dot = strchr(filename, '.');
  int name_len = dot ? (int)(dot - filename) : (int)strlen(filename);
  if (name_len > 8)
    name_len = 8;
  memcpy(entry->name, filename, name_len);
  if (dot) {
    int ext_len = (int)strlen(dot + 1);
    if (ext_len > 3)
      ext_len = 3;
    memcpy(entry->name + 8, dot + 1, ext_len);
  }

  /* For simplicity, allocate cluster 3+ for first few files */
  /* This is a hack! But works for early setup */
  entry->first_cluster_low = (uint16_t)(next_free_cluster & 0xFFFF);
  entry->first_cluster_high = (uint16_t)(next_free_cluster >> 16);
  entry->size = len;

  /* Write directory sector */
  ata_write_sectors(root_sector + found_sector, 1, sector_buf);

  /* Write data */
  uint32_t lba =
      data_start_lba + ((next_free_cluster - 2) * fs_bpb.sectors_per_cluster);
  uint32_t sectors_to_write = (len + 511) / 512;
  if (sectors_to_write > 0) {
    ata_write_sectors(lba, (uint8_t)sectors_to_write, buffer);
  }

  /* Update FAT (Mark EOC) */
  uint32_t fat_sector = fat_start_lba + (next_free_cluster * 4 / 512);
  ata_read_sectors(fat_sector, 1, sector_buf);
  ((uint32_t *)sector_buf)[next_free_cluster % 128] = 0x0FFFFFFF;
  ata_write_sectors(fat_sector, 1, sector_buf);

  /* Guard critique : si sectors_per_cluster est 0 (fat32_init pas encore
   * appelé après un format), la division ligne suivante crasherait en #DE */
  if (fs_bpb.sectors_per_cluster == 0)
    return -1;

  next_free_cluster += (sectors_to_write + fs_bpb.sectors_per_cluster - 1) /
                       fs_bpb.sectors_per_cluster;

  return 0;
}

int fat32_create_file(const char *filename) {
  return fat32_write_file(filename, (uint8_t *)"", 0);
}
int fat32_delete_file(const char *filename) {
  (void)filename;
  return -1;
}
void fat32_format(void) {
  uint32_t lba = mbr_find_partition();
  if (lba == 0) {
    terminal_writestring("No partition found for formatting.\n");
    return;
  }
  fat32_format_partition(lba, 20480); /* Assuming 10MB disk for now */
}

void fat32_format_partition(uint32_t lba_start, uint32_t sector_count) {
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
  bpb->sectors_per_fat_32 = (sector_count / 8 / 512) + 1;
  bpb->root_cluster = 2;
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

  /* Clear FATs */
  memset(buffer, 0, 512);
  for (uint32_t i = 0; i < bpb->sectors_per_fat_32; i++) {
    ata_write_sectors(local_fat_lba + i, 1, buffer);
    /* FAT2 */
    ata_write_sectors(local_fat_lba + bpb->sectors_per_fat_32 + i, 1, buffer);
  }

  /* Initialize FAT: Cluster 0, 1 (Reserved), Cluster 2 (Root) */
  ata_read_sectors(local_fat_lba, 1, buffer);
  uint32_t *fat = (uint32_t *)buffer;
  fat[0] = 0x0FFFFFF8;
  fat[1] = 0x0FFFFFFF;
  fat[2] = 0x0FFFFFFF;
  ata_write_sectors(local_fat_lba, 1, buffer);

  /* Clear Data Area (Root Cluster) */
  memset(buffer, 0, 512);
  uint32_t root_lba =
      local_data_lba + ((bpb->root_cluster - 2) * bpb->sectors_per_cluster);
  for (uint32_t i = 0; i < bpb->sectors_per_cluster; i++) {
    ata_write_sectors(root_lba + i, 1, buffer);
  }

  /* Reset cluster allocator state */
  extern void fat32_reset_allocator(void);
  fat32_reset_allocator();

  terminal_writestring("FAT32: Formatting complete.\n");

  /* Monter le volume fraîchement formaté pour initialiser les globales.
   * CRITIQUE : sans ça, sectors_per_cluster = 0 → division par zéro
   * dans fat32_write_file lors de l'installation. */
  fat32_mount_lba(lba_start);
}
uint32_t fat32_cluster_to_lba(uint32_t cluster) {
  if (part_lba_start == 0)
    return 0;
  return data_start_lba + ((cluster - 2) * fs_bpb.sectors_per_cluster);
}

uint32_t fat32_get_file_first_cluster(const char *filename) {
  if (part_lba_start == 0)
    return 0;

  uint32_t root_sector =
      data_start_lba + ((fs_bpb.root_cluster - 2) * fs_bpb.sectors_per_cluster);
  uint8_t sector_buf[512];
  struct fat_dir_entry *entry;
  char name83[11];

  memset(name83, ' ', 11);
  const char *dot = strchr(filename, '.');
  int name_len = dot ? (int)(dot - filename) : (int)strlen(filename);
  if (name_len > 8)
    name_len = 8;
  memcpy(name83, filename, name_len);
  if (dot) {
    int ext_len = (int)strlen(dot + 1);
    if (ext_len > 3)
      ext_len = 3;
    memcpy(name83 + 8, dot + 1, ext_len);
  }

  for (int s = 0; s < fs_bpb.sectors_per_cluster; s++) {
    ata_read_sectors(root_sector + s, 1, sector_buf);
    for (int i = 0; i < 16; i++) {
      entry = (struct fat_dir_entry *)(sector_buf + i * 32);
      if (entry->name[0] == 0)
        return 0;
      if (memcmp(entry->name, name83, 11) == 0) {
        return ((uint32_t)entry->first_cluster_high << 16) |
               entry->first_cluster_low;
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