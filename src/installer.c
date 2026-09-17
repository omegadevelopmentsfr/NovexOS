/*
 * NovexOS - installer.c
 * Full installation wizard:
 *   - FAT32 / NTFS / ext4 formatting
 *   - Full-disk or dual-boot partitioning (shrinks existing partition)
 *   - GRUB-compatible boot hint sector
 *   - Stage2 loader + kernel copy from RAM
 */

#include "installer.h"
#include "ata.h"
#include "fat32.h"
#include "io.h"
#include "keyboard.h"
#include "lang.h"
#include "mbr.h"
#include "string.h"
#include "timer.h"

extern void terminal_writestring(const char *s);
extern int chosen_res_id;
extern void terminal_putchar(char c);
extern void terminal_set_color(uint8_t color);

static install_cb_t current_cb = 0;

void installer_log(const char *msg) {
  if (!msg)
    return;
  terminal_writestring(msg);
  serial_print(msg);
  if (current_cb) {
    current_cb(-1, msg); /* -1 means "just a log message, don't update bar" */
  }
}

void installer_log_hex(const char *prefix, uint32_t val) {
  installer_log(prefix);
  char buf[12];
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 0; i < 8; i++) {
    uint8_t nibble = (val >> (28 - i * 4)) & 0xF;
    buf[2 + i] = (nibble < 10) ? ('0' + nibble) : ('A' + nibble - 10);
  }
  buf[10] = '\0';
  installer_log(buf);
  installer_log("\n");
}

/* -----------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------- */

static void print_uint32(uint32_t v) {
  char buf[12];
  int i = 10;
  buf[11] = '\0';
  if (v == 0) {
    terminal_putchar('0');
    return;
  }
  while (v > 0 && i >= 0) {
    buf[i--] = (char)('0' + v % 10);
    v /= 10;
  }
  terminal_writestring(&buf[i + 1]);
}

static void print_mb(uint32_t sectors) {
  print_uint32(sectors / 2048);
  terminal_writestring(" MB");
}

/* Cached result of the last installer_probe() call */
static disk_info_t g_info;

static void installer_create_system_folder(void);

/* -----------------------------------------------------------------------
 * installer_probe
 * Detects ATA disk, reads MBR, fills disk_info_t.
 * --------------------------------------------------------------------- */
int installer_probe(disk_info_t *info) {
  memset(info, 0, sizeof(*info));

  /* Soft-reset ATA bus with generous timing to avoid intermittent failures */
  outb(0x3F6, 0x04);
  timer_sleep(20);
  outb(0x3F6, 0x00);
  timer_sleep(50);
  uint32_t t = 0;
  while ((inb(0x1F7) & 0x80) && t++ < 1000000)
    ;

  /* Retry up to 3 times if ata_identify fails */
  int identified = 0;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (ata_identify(0)) {
      identified = 1;
      break;
    }
    timer_sleep(50);
  }
  if (!identified)
    return 0;
  info->disk_found = 1;

  /* Read MBR (sector 0) */
  uint8_t buf[512];
  ata_read_sectors(0, 1, buf);
  struct mbr *m = (struct mbr *)buf;

  if (m->signature != 0xAA55) {
    /* Unformatted disk — ask IDENTIFY for sector count (words 60-61) */
    outb(0x1F6, 0xA0);
    outb(0x1F7, 0xEC);
    t = 0;
    while ((inb(0x1F7) & 0x88) != 0x08 && t++ < 500000)
      ;
    uint16_t id[256];
    for (int i = 0; i < 256; i++)
      id[i] = inw(0x1F0);
    info->total_sectors = ((uint32_t)id[61] << 16) | (uint32_t)id[60];
    if (info->total_sectors < 20480)
      info->total_sectors = 20480;
    g_info = *info;
    return 1;
  }

  info->has_valid_mbr = 1;

  /* Walk partitions: compute total disk size and detect existing OS */
  uint32_t max_end = 0;
  for (int i = 0; i < 4; i++) {
    uint8_t type = m->partitions[i].type;
    if (type == 0)
      continue;
    uint32_t end = m->partitions[i].lba_start + m->partitions[i].sector_count;
    if (end > max_end)
      max_end = end;

    /* Anything that is not our own FAT32 type (0x0B/0x0C) is "existing OS" */
    if (!info->existing_os && type != 0x0B && type != 0x0C) {
      info->existing_os = 1;
      info->existing_type = type;
      info->existing_lba = m->partitions[i].lba_start;
      info->existing_sectors = m->partitions[i].sector_count;
    }
  }
  info->total_sectors = (max_end < 20480) ? 20480 : max_end;
  g_info = *info;
  return 1;
}

const disk_info_t *installer_get_info(void) { return &g_info; }

/* -----------------------------------------------------------------------
 * installer_format_fat32
 * Delegates to the existing fat32_format_partition() in fat32.c.
 * --------------------------------------------------------------------- */
int installer_format_fat32(uint32_t lba_start, uint32_t sectors,
                           install_cb_t cb) {
  if (cb)
    cb(5, "Formatting FAT32 partition...");
  terminal_writestring("  Formatting FAT32 ... ");
  fat32_format_partition(lba_start, sectors, cb);
  terminal_writestring("[OK]\n");
  if (cb)
    cb(95, "FAT32 Format Complete");
  return 1;
}

/* -----------------------------------------------------------------------
 * installer_format_ntfs
 * Writes a minimal NTFS VBR (Volume Boot Record) to the first sector of
 * the partition.  This is sufficient for GRUB to recognise the volume
 * type and for NovexOS to detect "NTFS" via the OEM-ID field.
 * Full NTFS read/write support is a future work item.
 * --------------------------------------------------------------------- */
/* NTFS Structures */
#pragma pack(push, 1)
typedef struct {
  uint32_t magic; /* "FILE" */
  uint16_t usa_offset;
  uint16_t usa_count;
  uint64_t lsn;
  uint16_t seq_num;
  uint16_t link_count;
  uint16_t attr_offset;
  uint16_t flags; /* 1 = in use, 2 = directory */
  uint32_t bytes_in_use;
  uint32_t bytes_alloc;
  uint64_t base_mft_rec;
  uint16_t next_attr_id;
  uint16_t reserved;
  uint32_t mft_record_id;
} ntfs_mft_record_t;

typedef struct {
  uint32_t type;
  uint32_t length;
  uint8_t non_resident;
  uint8_t name_length;
  uint16_t name_offset;
  uint16_t flags;
  uint16_t attr_id;
} ntfs_attr_header_t;

typedef struct {
  uint32_t bg_block_bitmap_lo;
  uint32_t bg_inode_bitmap_lo;
  uint32_t bg_inode_table_lo;
  uint16_t bg_free_blocks_count_lo;
  uint16_t bg_free_inodes_count_lo;
  uint16_t bg_used_dirs_count_lo;
  uint16_t bg_flags;
  uint32_t bg_exclude_bitmap_lo;
  uint16_t bg_block_bitmap_csum_lo;
  uint16_t bg_inode_bitmap_csum_lo;
  uint16_t bg_itable_unused_lo;
  uint16_t bg_checksum;
} ext4_group_desc_t;
#pragma pack(pop)

static void ntfs_init_mft_record(uint8_t *buf, uint32_t id) {
  memset(buf, 0, 1024);
  ntfs_mft_record_t *m = (ntfs_mft_record_t *)buf;
  memcpy(&m->magic, "FILE", 4);
  m->usa_offset = 48;
  m->usa_count = 3;
  m->seq_num = 1;
  m->link_count = 1;
  m->attr_offset = 56;
  m->flags = 1;
  m->bytes_in_use = 56; /* Just header for now */
  m->bytes_alloc = 1024;
  m->mft_record_id = id;
}

int installer_format_ntfs(uint32_t lba_start, uint32_t sectors,
                          install_cb_t cb) {
  if (cb)
    cb(10, "Writing NTFS VBR...");
  terminal_writestring("  Formatting NTFS  ... ");

  uint8_t vbr[512];
  memset(vbr, 0, 512);

  /* ---- x86 bootstrap stub: EB 52 90 ---- */
  vbr[0] = 0xEB;
  vbr[1] = 0x52;
  vbr[2] = 0x90;

  /* ---- OEM ID: "NTFS    " (8 bytes) ---- */
  memcpy(&vbr[3], "NTFS    ", 8);

  /* ---- BPB (BIOS Parameter Block) ---- */
  vbr[11] = 0x00;
  vbr[12] = 0x02; /* 512 bytes/sec */
  vbr[13] = 0x08; /* 8 sectors/cluster = 4KB */
  vbr[21] = 0xF8; /* Media: fixed */
  vbr[24] = 0x3F;
  vbr[25] = 0x00; /* 63 sectors/track */
  vbr[26] = 0xFF;
  vbr[27] = 0x00; /* 255 heads */

  vbr[28] = (uint8_t)(lba_start);
  vbr[29] = (uint8_t)(lba_start >> 8);
  vbr[30] = (uint8_t)(lba_start >> 16);
  vbr[31] = (uint8_t)(lba_start >> 24);

  /* Total sectors (64-bit) */
  uint64_t sec64 = (uint64_t)(sectors - 1);
  memcpy(&vbr[40], &sec64, 8);

  /* MFT starts at ~12.5% of volume per README */
  uint64_t mft_lcn = (sectors / 8) / 8;
  memcpy(&vbr[48], &mft_lcn, 8);

  /* MFT Mirror at midpoint */
  uint64_t mft_mirror = (sectors / 8) / 2;
  memcpy(&vbr[56], &mft_mirror, 8);

  vbr[64] = 0xF6; /* -10 = 1024 bytes/record */
  vbr[68] = 0x01; /* 1 cluster/index */

  /* Serial */
  uint64_t serial = 0x1234567890ABCDEF ^ lba_start;
  memcpy(&vbr[72], &serial, 8);

  vbr[510] = 0x55;
  vbr[511] = 0xAA;

  ata_write_sectors(lba_start, 1, vbr);

  /* Write basic MFT records */
  uint8_t mft_rec[1024];
  uint32_t mft_start_sector = (uint32_t)mft_lcn * 8;
  for (int i = 0; i < 16; i++) {
    ntfs_init_mft_record(mft_rec, i);
    ata_write_sectors(lba_start + mft_start_sector + (i * 2), 2, mft_rec);
    if (cb && (i % 4 == 0))
      cb(10 + (i * 5), "Initializing NTFS MFT...");
  }

  terminal_writestring("[OK]\n");
  return 1;
}

int installer_format_ext4(uint32_t lba_start, uint32_t sectors,
                          install_cb_t cb) {
  serial_print("[INSTALLER] format_ext4: enter lba_start=");
  serial_print_hex(lba_start);
  serial_print(" sectors=");
  serial_print_hex(sectors);
  serial_print("\r\n");
  if (cb)
    cb(10, "Writing ext4 superblock...");
  terminal_writestring("  Formatting ext4  ... ");

  uint8_t sb_buf[1024];
  memset(sb_buf, 0, 1024);

  uint32_t block_size = 4096;
  uint32_t blocks_per_group = 32768;
  uint32_t total_blocks = sectors / (block_size / 512);
  uint32_t num_groups =
      (total_blocks + blocks_per_group - 1) / blocks_per_group;
  uint32_t inode_size = 256;
  uint32_t inodes_per_group = 8192;

  /* Superblock at offset 1024 */
  uint32_t *sb32 = (uint32_t *)sb_buf;
  uint16_t *sb16 = (uint16_t *)sb_buf;

  sb32[0] = inodes_per_group * num_groups; /* s_inodes_count */
  sb32[1] = total_blocks;                  /* s_blocks_count_lo */
  sb32[2] = total_blocks / 20;             /* s_r_blocks_count_lo */
  sb32[3] = total_blocks - 100;            /* s_free_blocks_count_lo (approx) */
  sb32[4] = sb32[0] - 11;                  /* s_free_inodes_count */
  sb32[5] = 0;                             /* s_first_data_block */
  sb32[6] = 2;                             /* s_log_block_size (4096) */
  sb32[8] = blocks_per_group;
  sb32[10] = inodes_per_group;
  sb16[28] = 0xEF53;     /* s_magic */
  sb16[29] = 1;          /* s_state: clean */
  sb16[38] = 1;          /* s_rev_level */
  sb32[21] = 11;         /* s_first_ino */
  sb16[44] = inode_size; /* s_inode_size */

  /* Features: EXTENTS, 64BIT, FLEX_BG, etc. */
  sb32[24] = 0x3C; /* Compat */
  sb32[25] = 0xC2; /* Incompat */
  sb32[26] = 0x63; /* RO Compat */

  ata_write_sectors(lba_start + 2, 2, sb_buf);

  /* Write Group Descriptor Table (GDT) */
  uint8_t gdt_buf[512];
  memset(gdt_buf, 0, 512);
  ext4_group_desc_t *gdt = (ext4_group_desc_t *)gdt_buf;
  for (uint32_t i = 0; i < num_groups && i < 8; i++) {
    gdt[i].bg_block_bitmap_lo = (i * blocks_per_group) + 1;
    gdt[i].bg_inode_bitmap_lo = (i * blocks_per_group) + 2;
    gdt[i].bg_inode_table_lo = (i * blocks_per_group) + 3;
    gdt[i].bg_free_blocks_count_lo = blocks_per_group - 20;
    gdt[i].bg_free_inodes_count_lo = inodes_per_group;
    gdt[i].bg_flags = 0x0004; /* INODE_UNINIT */
  }
  ata_write_sectors(lba_start + 8, 1, gdt_buf);

  terminal_writestring("[OK]\n");
  serial_print("[INSTALLER] format_ext4: DONE\r\n");
  return 1;
}

/* -----------------------------------------------------------------------
 * installer_write_kernel
 * Scans RAM for the Multiboot magic, copies the kernel to the FAT32
 * partition already mounted by fat32_init(), and patches the Stage2
 * loader in sectors 1-8 with the correct LBA / size / entry point.
 * --------------------------------------------------------------------- */
int installer_write_kernel(uint32_t lba_start) {
  (void)lba_start; /* fat32_init() already knows the partition */

  terminal_writestring("  Scanning RAM for kernel magic ... ");

  extern char _kernel_start[];
  extern char _edata[];
  /* Use _edata (end of initialised data) instead of _sbss/_kernel_end:
   * BSS is zero-initialised at runtime, not stored on disk.  Using _sbss
   * would include the entire BSS in write_size, producing a multi-MB write
   * that makes the installer appear frozen via PIO. */
  uint32_t kernel_max_size = (uint32_t)(_edata - _kernel_start);

  /* Scan 128 KB from 0x100000 for Multiboot magic 0x1BADB002 */
  uint32_t *scan = (uint32_t *)0x100000;
  uint8_t *kernel_source = (uint8_t *)_kernel_start;
  for (uint32_t i = 0; i < 32768; i++) {
    if (scan[i] == 0x1BADB002) {
      kernel_source = (uint8_t *)&scan[i];
      uint32_t off = i;
      installer_log_hex("Kernel: Found signature at offset ", off);
      break;
    }
  }
  terminal_writestring("[OK]\n");

  /* Sanity-cap write_size: never write more than 4 MB.
   * Stage2 loader cannot load more anyway, and PIO writes of several MB
   * take minutes on QEMU, appearing as a freeze. */
  uint32_t write_size = kernel_max_size;
  if (write_size == 0 || write_size > 4 * 1024 * 1024) {
    terminal_set_color(0x0E);
    terminal_writestring("  [WARN] kernel size suspicious, capping at 4MB\n");
    terminal_set_color(0x07);
    write_size = 4 * 1024 * 1024;
  }

  installer_log_hex("Kernel: write_size (bytes) = ", write_size);
  installer_log_hex("Kernel: write_size (sectors) = ",
                    (write_size + 511) / 512);

  terminal_writestring("  Creating SYSTEM/ directory ... ");
  fat32_mkdir("SYSTEM");
  terminal_writestring("[OK]\n");

  terminal_writestring("  Writing SYSTEM/KERNEL.SYS ... ");
  fat32_write_file("SYSTEM/KERNEL.SYS", kernel_source, write_size);
  terminal_writestring("[OK]\n");

  /* ---- Patch Stage2 loader ---- */
  terminal_writestring("  Patching Stage2 loader ... ");

  extern char _binary_build_loader_bin_start[];
  extern char _binary_build_loader_bin_size[];
  uintptr_t loader_size = (uintptr_t)_binary_build_loader_bin_size;
  installer_log_hex("Installer: Loader size is ", (uint32_t)loader_size);

  uint8_t loader_buf[4096];

  /* Guard: if the Stage2 binary was not embedded, abort now instead of
   * writing a blank/zero Stage2 that will cause an infinite reboot loop. */
  if (loader_size == 0) {
    terminal_set_color(0x0C);
    terminal_writestring("[ERR] Stage2 loader binary not embedded!\n");
    terminal_set_color(0x07);
    return 0;
  }

  memset(loader_buf, 0, 4096);
  memcpy(loader_buf, _binary_build_loader_bin_start,
         loader_size > 4096 ? 4096 : loader_size);

  extern uint32_t fat32_cluster_to_lba(uint32_t cluster);

  uint32_t k_cluster = fat32_get_file_first_cluster("SYSTEM/KERNEL.SYS");
  if (k_cluster == 0) {
    k_cluster = fat32_get_file_first_cluster("KERNEL.SYS");
  }
  if (k_cluster == 0) {
    terminal_set_color(0x0C);
    terminal_writestring("[ERR] SYSTEM/KERNEL.SYS cluster not found in FAT!\n");
    terminal_set_color(0x07);
    return 0;
  }
  uint32_t k_lba = fat32_cluster_to_lba(k_cluster);
  installer_log_hex("Kernel: Target LBA calculated as ", k_lba);

  uint8_t verify_buf[512];
  ata_read_sectors(k_lba, 1, verify_buf);
  if (*(uint32_t *)verify_buf == 0x1BADB002) {
    installer_log("Kernel: Verify-after-Write ... [OK]\n");
  } else {
    installer_log("Kernel: Verify-after-Write ... [FAILED]\n");
    installer_log_hex("Kernel: Sector header read was: ",
                      *(uint32_t *)verify_buf);
  }

  installer_log_hex("Kernel: Writing KERNEL.SYS to FAT32 LBA ", k_lba);
  installer_log_hex("Kernel: Sector count: ", (write_size + 511) / 512);

  /* Patch fields must match loader.s .org layout:
   *   offset  8 = kernel_lba
   *   offset 12 = kernel_sectors
   *   offset 16 = kernel_entry  */
  *(uint32_t *)(loader_buf + 8) = k_lba;
  *(uint32_t *)(loader_buf + 12) = (write_size + 511) / 512;
  /* Entry point: _start is the real kernel entry (in .text section, after the
   * .multiboot section).  The Multiboot magic is at _kernel_start (0x100000)
   * but that is HEADER DATA — jumping there executes garbage bytes and triple-
   * faults.  _start is identity-mapped, so its virtual == physical address. */
  extern char _start[];
  *(uint32_t *)(loader_buf + 16) = (uint32_t)(uintptr_t)_start;

  installer_log_hex("Stage2 patch: kernel_lba written = ", k_lba);
  installer_log_hex("Stage2 patch: kernel_sectors written = ",
                    (write_size + 511) / 512);
  installer_log_hex("Stage2 patch: kernel_entry written = ",
                    (uint32_t)(uintptr_t)_start);

  ata_write_sectors(1, 8, loader_buf);
  terminal_writestring("[OK]\n");

  /* Verify-after-write for the Stage2 patch itself. Nothing previously
   * checked that what we WROTE is actually what's READABLE back from
   * disk at LBA 1 -- unlike KERNEL.SYS, which already has this check. */
  {
    uint8_t stage2_verify[512];
    ata_read_sectors(1, 1, stage2_verify);
    uint32_t rb_lba = *(uint32_t *)(stage2_verify + 8);
    uint32_t rb_sectors = *(uint32_t *)(stage2_verify + 12);
    uint32_t rb_entry = *(uint32_t *)(stage2_verify + 16);
    installer_log_hex("Stage2 verify: kernel_lba read back = ", rb_lba);
    installer_log_hex("Stage2 verify: kernel_sectors read back = ", rb_sectors);
    installer_log_hex("Stage2 verify: kernel_entry read back = ", rb_entry);
    if (rb_lba != k_lba) {
      terminal_set_color(0x0C);
      terminal_writestring("  [WARN] Stage2 kernel_lba readback MISMATCH!\n");
      terminal_set_color(0x07);
    }
  }

  /* ---- Quick verify ---- */
  terminal_writestring("  Verifying SYSTEM/KERNEL.SYS ... ");
  uint8_t verify[512];
  if (fat32_read_file("SYSTEM/KERNEL.SYS", verify, 512) > 0 ||
      fat32_read_file("KERNEL.SYS", verify, 512) > 0) {
    uint32_t magic = *(uint32_t *)verify;
    if (magic == 0x1BADB002) {
      terminal_writestring("[OK] magic match\n");
    } else {
      terminal_set_color(0x0C);
      terminal_writestring("[WARN] magic mismatch\n");
      terminal_set_color(0x07);
    }
  } else {
    terminal_set_color(0x0C);
    terminal_writestring("[ERR] read failed\n");
    terminal_set_color(0x07);
    return 0;
  }
  return 1;
}

/* -----------------------------------------------------------------------
 * installer_write_grub_entry
 * Writes a 512-byte hint block to sector 9 so that GRUB (or any
 * bootloader that respects the "NXGE" tag) can chainload NovexOS.
 * Sector layout: 0=MBR, 1-8=Stage2, 9=GRUB hint.
 * --------------------------------------------------------------------- */
void installer_write_grub_entry(uint32_t novex_lba, uint32_t novex_sectors) {
  uint8_t sec[512];
  memset(sec, 0, 512);

  /* Tag "NXGE" */
  sec[0] = 'N';
  sec[1] = 'X';
  sec[2] = 'G';
  sec[3] = 'E';

  /* NovexOS partition LBA (4 bytes LE) */
  sec[4] = (uint8_t)(novex_lba);
  sec[5] = (uint8_t)(novex_lba >> 8);
  sec[6] = (uint8_t)(novex_lba >> 16);
  sec[7] = (uint8_t)(novex_lba >> 24);

  /* NovexOS partition sector count (4 bytes LE) */
  sec[8] = (uint8_t)(novex_sectors);
  sec[9] = (uint8_t)(novex_sectors >> 8);
  sec[10] = (uint8_t)(novex_sectors >> 16);
  sec[11] = (uint8_t)(novex_sectors >> 24);

  /* Human-readable label */
  const char *label = "NovexOS";
  for (int i = 0; label[i] && i < 16; i++)
    sec[16 + i] = (uint8_t)label[i];

  /* Minimal GRUB-style menu entry (ASCII, null-terminated) at offset 32 */
  const char *grub = "menuentry \"NovexOS\" {\n"
                     "  set root=(hd0,msdos2)\n"
                     "  chainloader +1\n"
                     "}\n";
  for (int i = 0; grub[i] && i < 200; i++)
    sec[32 + i] = (uint8_t)grub[i];

  ata_write_sectors(9, 1, sec);
}

/* -----------------------------------------------------------------------
 * installer_verify_format
 * Reads the partition back to verify that the format was successful.
 * --------------------------------------------------------------------- */
static int installer_verify_format(uint32_t lba_start, install_fs_t fs,
                                   install_cb_t cb) {
  if (cb)
    cb(98, "Verifying filesystem signature...");
  uint8_t buf[512];
  ata_read_sectors(lba_start, 1, buf);

  if (fs == INSTALL_FS_FAT32) {
    if (buf[510] != 0x55 || buf[511] != 0xAA ||
        memcmp(&buf[82], "FAT32   ", 8) != 0) {
      if (cb)
        cb(100, "Error: FAT32 verification failed");
      return 0;
    }
  } else if (fs == INSTALL_FS_NTFS) {
    if (buf[510] != 0x55 || buf[511] != 0xAA ||
        memcmp(&buf[3], "NTFS    ", 8) != 0) {
      if (cb)
        cb(100, "Error: NTFS VBR verification failed");
      return 0;
    }
    /* Verify MFT record 0: mft_lcn is at offset 48 in NTFS VBR */
    uint64_t mft_lcn = *(uint64_t *)&buf[48];
    uint32_t mft_start = (uint32_t)mft_lcn * 8;
    ata_read_sectors(lba_start + mft_start, 1, buf);
    if (memcmp(buf, "FILE", 4) != 0) {
      if (cb)
        cb(100, "Error: NTFS MFT verification failed");
      return 0;
    }
  } else if (fs == INSTALL_FS_EXT4) {
    /* Ext4 superblock is at byte offset 1024 (lba_start + 2) */
    ata_read_sectors(lba_start + 2, 1, buf);
    if (buf[56] != 0x53 || buf[57] != 0xEF) {
      if (cb)
        cb(100, "Error: ext4 verification failed");
      return 0;
    }
  }
  return 1;
}

/* -----------------------------------------------------------------------
 * installer_full_install
 * Creates a fresh MBR, formats the entire disk with the chosen FS,
 * mounts FAT32 for kernel copy, writes kernel and GRUB hint.
 * --------------------------------------------------------------------- */
int installer_full_install(install_fs_t fs, install_cb_t cb) {
  current_cb = cb;
  disk_info_t info;
  if (cb)
    cb(0, "Initializing full installation...");

  terminal_writestring("\n[1/5] Detecting disk ...\n");
  if (cb)
    cb(2, "Detecting disk...");
  if (!installer_probe(&info) || !info.disk_found) {
    terminal_set_color(0x0C);
    terminal_writestring("  [ERR] No ATA disk detected.\n");
    terminal_set_color(0x07);
    return 0;
  }
  terminal_writestring("  Disk: ");
  print_mb(info.total_sectors);
  terminal_putchar('\n');

  terminal_writestring("[2/5] Creating MBR partition table ...\n");
  if (cb)
    cb(4, "Creating MBR partition table...");
  struct mbr new_mbr;
  mbr_create_partition_table(&new_mbr, info.total_sectors);
  /* Set partition type byte to match chosen FS */
  new_mbr.partitions[0].type = (fs == INSTALL_FS_FAT32)  ? 0x0B
                               : (fs == INSTALL_FS_NTFS) ? 0x07
                                                         : 0x83;
  mbr_write(&new_mbr);
  terminal_writestring("  [OK] MBR written\n");

  uint32_t part_lba = 2048;
  uint32_t part_secs = info.total_sectors - 2048;

  terminal_writestring("[3/5] Formatting partition ...\n");
  int fmt_ok = 0;
  if (fs == INSTALL_FS_FAT32)
    fmt_ok = installer_format_fat32(part_lba, part_secs, cb);
  else if (fs == INSTALL_FS_NTFS)
    fmt_ok = installer_format_ntfs(part_lba, part_secs, cb);
  else
    fmt_ok = installer_format_ext4(part_lba, part_secs, cb);
  if (!fmt_ok)
    return 0;

  if (!installer_verify_format(part_lba, fs, cb)) {
    terminal_set_color(0x0C);
    terminal_writestring("[ERR] Verification of formatting failed!\n");
    terminal_set_color(0x07);
    return 0;
  }

  /* The kernel is always installed to a FAT32 area.
     For NTFS/ext4 we format a small FAT32 boot area at the start of the
     partition so the Stage2 loader can locate KERNEL.SYS.
     CRITICAL: For ext4, we MUST NOT overwrite the Superblock at sector 2.
     For NTFS, we MUST NOT overwrite the VBR at sector 0.
     Modified FAT32 overlay: only write FATs and Root Dir if they don't
     conflict with the signatures.
  */
  terminal_writestring("[4/5] Mounting boot area & writing kernel ...\n");
  if (cb)
    cb(96, "Writing kernel to disk...");
  serial_print(
      "[INSTALLER] full_install: entering boot-area overlay block, fs=");
  serial_print_hex((uint32_t)fs);
  serial_print("\r\n");
  if (fs != INSTALL_FS_FAT32) {
    terminal_writestring("  (Boot area: FAT32 overlay for Stage2)\n");
    /* We still call installer_format_fat32 but internally it should
     * probably be aware of the "overlay" mode. For now, we rely on
     * the fact that FAT32 VBR is similar to NTFS VBR at 0, and for
     * ext4, the VBR is at 0 while ext4 SB is at 2. */
    serial_print("[INSTALLER] full_install: calling installer_format_fat32 "
                 "(overlay) ...\r\n");
    installer_format_fat32(part_lba, part_secs < 20480 ? part_secs : 20480,
                           NULL);
    serial_print("[INSTALLER] full_install: installer_format_fat32 (overlay) "
                 "returned\r\n");

    /* RE-WRITE signatures if they were overwritten by FAT32 VBR */
    if (fs == INSTALL_FS_NTFS) {
      /* NTFS VBR and FAT32 VBR both live at sector 0.
       * Re-writing NTFS VBR here would break FAT32 bootability.
       * For now, we assume FAT32 is the priority for Stage2. */
    } else if (fs == INSTALL_FS_EXT4) {
      /* Re-write ext4 superblock to sector 2 and 3 because FAT32
       * doesn't use those sectors for critical boot info (usually). */
      serial_print(
          "[INSTALLER] full_install: re-writing ext4 superblock ...\r\n");
      installer_format_ext4(part_lba, part_secs, NULL);
      serial_print(
          "[INSTALLER] full_install: ext4 superblock re-write returned\r\n");
    }
  }
  serial_print(
      "[INSTALLER] full_install: about to call installer_write_kernel\r\n");
  /* For FAT32: the format at step 3 already mounted the volume.
   * No remount needed — the global variables are already valid. */
  if (!installer_write_kernel(part_lba))
    return 0;

  terminal_writestring("[5/5] Writing GRUB hint ...\n");
  if (cb)
    cb(99, "Writing GRUB hint sector...");
  installer_write_grub_entry(part_lba, part_secs);
  terminal_writestring("  [OK]\n");

  terminal_set_color(0x0A);
  if (cb)
    cb(100, "Installation complete");
  terminal_writestring("\n[DONE] Full installation complete!\n");
  terminal_set_color(0x07);
  terminal_writestring("Remove the installation media and reboot.\n");

  /* Save user preferences to the freshly installed partition */
  installer_save_settings();

  /* Create the protected SYSTEM/ directory with system file copies */
  installer_create_system_folder();

  return 1;
}

/* -----------------------------------------------------------------------
 * installer_dualboot_install
 * Shrinks the existing OS partition by updating only its sector_count
 * in the MBR (no data is written to that partition — the OS data
 * survives if shrink_to_sectors <= original size).
 * A new NovexOS partition is created in the freed space.
 * --------------------------------------------------------------------- */
int installer_dualboot_install(install_fs_t fs, uint32_t shrink_to_sectors,
                               install_cb_t cb) {
  current_cb = cb;
  if (cb)
    cb(0, "Initializing dual-boot installation...");
  disk_info_t info;

  terminal_writestring("\n[1/6] Detecting disk ...\n");
  if (cb)
    cb(2, "Detecting disk...");
  if (!installer_probe(&info) || !info.disk_found) {
    terminal_set_color(0x0C);
    terminal_writestring("  [ERR] No ATA disk detected.\n");
    terminal_set_color(0x07);
    return 0;
  }

  if (!info.existing_os) {
    terminal_writestring("  No existing OS found — running full install.\n");
    return installer_full_install(fs, cb);
  }

  terminal_writestring("  Existing OS partition: LBA ");
  print_uint32(info.existing_lba);
  terminal_writestring(", size ");
  print_mb(info.existing_sectors);
  terminal_putchar('\n');

  if (shrink_to_sectors == 0 || shrink_to_sectors >= info.existing_sectors) {
    terminal_set_color(0x0C);
    terminal_writestring(
        "  [ERR] shrink_to_sectors must be > 0 and < current size.\n");
    terminal_set_color(0x07);
    return 0;
  }

  uint32_t freed = info.existing_sectors - shrink_to_sectors;
  terminal_writestring("  Freeing ");
  print_mb(freed);
  terminal_writestring(" for NovexOS.\n");

  /* ---- Read MBR, patch existing partition, add new one ---- */
  terminal_writestring("[2/6] Updating MBR ...\n");
  if (cb)
    cb(5, "Shrinking existing partition in MBR...");
  uint8_t mbr_buf[512];
  ata_read_sectors(0, 1, mbr_buf);
  struct mbr *m = (struct mbr *)mbr_buf;

  int found_slot = -1;
  int free_slot = -1;

  for (int i = 0; i < 4; i++) {
    if (m->partitions[i].lba_start == info.existing_lba && found_slot < 0)
      found_slot = i;
    else if (m->partitions[i].type == 0 && free_slot < 0)
      free_slot = i;
  }

  if (found_slot < 0) {
    terminal_set_color(0x0C);
    terminal_writestring("  [ERR] Existing partition not found in MBR.\n");
    terminal_set_color(0x07);
    return 0;
  }
  if (free_slot < 0) {
    terminal_set_color(0x0C);
    terminal_writestring(
        "  [ERR] MBR has no free slot (max 4 primary partitions).\n");
    terminal_set_color(0x07);
    return 0;
  }

  /* Shrink existing partition */
  m->partitions[found_slot].sector_count = shrink_to_sectors;
  m->partitions[found_slot].status = 0x00; /* no longer bootable */

  /* NovexOS partition: align start to next 2048-sector boundary */
  uint32_t novex_lba = info.existing_lba + shrink_to_sectors;
  novex_lba = (novex_lba + 2047) & ~(uint32_t)2047;
  uint32_t novex_secs =
      info.total_sectors > novex_lba ? info.total_sectors - novex_lba : 0;
  if (novex_secs < 4096) {
    terminal_set_color(0x0C);
    terminal_writestring("  [ERR] Not enough space after shrink (< 2 MB).\n");
    terminal_set_color(0x07);
    return 0;
  }

  m->partitions[free_slot].status = 0x80; /* bootable */
  m->partitions[free_slot].type = (fs == INSTALL_FS_FAT32)  ? 0x0B
                                  : (fs == INSTALL_FS_NTFS) ? 0x07
                                                            : 0x83;
  m->partitions[free_slot].lba_start = novex_lba;
  m->partitions[free_slot].sector_count = novex_secs;
  /* CHS: fill with LBA-invalid sentinel — LBA mode only */
  m->partitions[free_slot].chs_start[0] = 0xFE;
  m->partitions[free_slot].chs_start[1] = 0xFF;
  m->partitions[free_slot].chs_start[2] = 0xFF;
  m->partitions[free_slot].chs_end[0] = 0xFE;
  m->partitions[free_slot].chs_end[1] = 0xFF;
  m->partitions[free_slot].chs_end[2] = 0xFF;

  ata_write_sectors(0, 1, mbr_buf);
  terminal_writestring("  [OK] MBR updated\n");

  terminal_writestring("[3/6] Formatting NovexOS partition (");
  terminal_writestring(fs == INSTALL_FS_FAT32  ? "FAT32"
                       : fs == INSTALL_FS_NTFS ? "NTFS"
                                               : "ext4");
  terminal_writestring(") ...\n");
  int fmt_ok = 0;
  if (fs == INSTALL_FS_FAT32)
    fmt_ok = installer_format_fat32(novex_lba, novex_secs, cb);
  else if (fs == INSTALL_FS_NTFS)
    fmt_ok = installer_format_ntfs(novex_lba, novex_secs, cb);
  else
    fmt_ok = installer_format_ext4(novex_lba, novex_secs, cb);
  if (!fmt_ok)
    return 0;

  if (!installer_verify_format(novex_lba, fs, cb)) {
    terminal_set_color(0x0C);
    terminal_writestring("[ERR] Verification of formatting failed!\n");
    terminal_set_color(0x07);
    return 0;
  }

  terminal_writestring("[4/6] Mounting boot area ...\n");
  if (cb)
    cb(92, "Mounting boot area...");
  if (fs != INSTALL_FS_FAT32) {
    uint32_t boot_secs = novex_secs < 20480 ? novex_secs : 20480;
    installer_format_fat32(novex_lba, boot_secs, NULL);
    /* Do NOT call fat32_init() — same reason as in installer_full_install:
     * mbr_find_partition() won't find NTFS (0x07) or ext4 (0x83) type,
     * which would reset part_lba_start to 0 and break the kernel write. */
  }
  /* FAT32 volume already mounted by the format step above (or step 3). */

  terminal_writestring("[5/6] Writing kernel ...\n");
  if (cb)
    cb(96, "Writing kernel to disk...");
  if (!installer_write_kernel(novex_lba))
    return 0;

  terminal_writestring("[6/6] Writing GRUB dual-boot entry ...\n");
  if (cb)
    cb(99, "Writing GRUB hint sector...");
  installer_write_grub_entry(novex_lba, novex_secs);
  terminal_writestring("  [OK]\n");

  terminal_set_color(0x0A);
  if (cb)
    cb(100, "Installation complete");
  terminal_writestring("\n[DONE] Dual-boot installation complete!\n");
  terminal_set_color(0x07);
  terminal_writestring(
      "Existing OS preserved. GRUB entry written to sector 9.\n");
  terminal_writestring("Remove installation media and reboot.\n");

  /* Save user preferences to the freshly installed partition */
  installer_save_settings();

  /* Create the protected SYSTEM/ directory with system file copies */
  installer_create_system_folder();

  return 1;
}

/* -----------------------------------------------------------------------
 * installer_save_settings
 * Writes a SETTINGS.DAT file to the FAT32 partition with current
 * language, keyboard layout, and resolution preferences.
 * Format: "LANG=x\nKBD=x\nRES=x\n" (simple ASCII key=value)
 * --------------------------------------------------------------------- */
void installer_save_settings(void) {
  char buf[64];
  int pos = 0;

  /* LANG= */
  buf[pos++] = 'L'; buf[pos++] = 'A'; buf[pos++] = 'N'; buf[pos++] = 'G';
  buf[pos++] = '=';
  buf[pos++] = '0' + (char)lang_get();
  buf[pos++] = '\n';

  /* KBD= */
  buf[pos++] = 'K'; buf[pos++] = 'B'; buf[pos++] = 'D';
  buf[pos++] = '=';
  buf[pos++] = '0' + (char)keyboard_get_layout();
  buf[pos++] = '\n';

  /* RES= */
  buf[pos++] = 'R'; buf[pos++] = 'E'; buf[pos++] = 'S';
  buf[pos++] = '=';
  buf[pos++] = '0' + (char)chosen_res_id;
  buf[pos++] = '\n';

  buf[pos] = '\0';

  fat32_mkdir("SYSTEM");
  fat32_write_file("SYSTEM/SETTINGS.DAT", (uint8_t *)buf, (uint32_t)pos);
  fat32_write_file("SETTINGS.DAT", (uint8_t *)buf, (uint32_t)pos);
  installer_log("Settings saved to SYSTEM/SETTINGS.DAT\n");
}

/* Append a NUL-terminated string to buf at pos, no libc needed. */
static int installer_append_str(char *buf, int pos, const char *s) {
  while (*s)
    buf[pos++] = *s++;
  return pos;
}

/* -----------------------------------------------------------------------
 * installer_create_system_folder
 * Creates the protected SYSTEM/ directory at the root of the freshly
 * installed FAT32 (or FAT32 boot-overlay) partition, and writes copies
 * of the critical system files inside it:
 *   SYSTEM/KERNEL.SYS   - small reference file (NOT a full binary copy:
 *                         duplicating the multi-MB kernel here doubled
 *                         install time via slow PIO writes and made the
 *                         installer look frozen after "complete" was
 *                         already shown - see CLAUDE.md/bug notes).
 *                         The real, bootable kernel stays at the
 *                         partition root, which is where Stage2 looks
 *                         for it.
 *   SYSTEM/SETTINGS.DAT - copy of the current install settings
 *   SYSTEM/SYSINFO.DAT  - build/version metadata
 * --------------------------------------------------------------------- */
static void installer_create_system_folder(void) {
  installer_log("  Creating SYSTEM directory ...\n");

  int mk = fat32_mkdir("SYSTEM");
  if (mk == 0) {
    installer_log("  [OK] SYSTEM directory created\n");
  } else if (mk == -1) {
    installer_log("  [OK] SYSTEM directory already present\n");
  } else {
    installer_log("  [WARN] Could not create SYSTEM directory\n");
  }

  /* SYSTEM/KERNEL.SYS already contains the real kernel binary written by
   * installer_write_kernel(). */

  /* Copy the SETTINGS.DAT we just wrote to the root into SYSTEM/ too. */
  uint8_t settings_buf[512];
  int slen = fat32_read_file("SETTINGS.DAT", settings_buf, sizeof(settings_buf));
  if (slen > 0) {
    fat32_write_file("SYSTEM/SETTINGS.DAT", settings_buf, (uint32_t)slen);
  }

  /* Write build/version metadata to SYSTEM/SYSINFO.DAT */
  char info[160];
  int pos = 0;
  pos = installer_append_str(info, pos, "NAME=NovexOS\n");
  pos = installer_append_str(info, pos, "VERSION=0.8.0\n");
  pos = installer_append_str(info, pos, "ARCH=x86_64\n");
  pos = installer_append_str(info, pos, "BUILD_DATE=" __DATE__ "\n");
  pos = installer_append_str(info, pos, "BUILD_TIME=" __TIME__ "\n");
  fat32_write_file("SYSTEM/SYSINFO.DAT", (uint8_t *)info, (uint32_t)pos);

  installer_log("  [OK] System files copied to SYSTEM/\n");
}