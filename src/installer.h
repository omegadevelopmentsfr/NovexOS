/*
 * NovexOS - installer.h
 * Installation wizard: disk format (FAT32 / NTFS / ext4) + dual-boot
 * partitioning. Called from the NovexPy `novex` module.
 */

#ifndef INSTALLER_H
#define INSTALLER_H

#include "types.h"

/* -----------------------------------------------------------------------
 * Filesystem selection
 * --------------------------------------------------------------------- */
typedef enum {
  INSTALL_FS_FAT32 = 0,
  INSTALL_FS_EXT4 = 1,
  INSTALL_FS_NTFS = 2,
} install_fs_t;

/* -----------------------------------------------------------------------
 * Disk information (filled by installer_probe)
 * --------------------------------------------------------------------- */
typedef struct {
  int disk_found;            /* 1 if an ATA disk was detected           */
  uint32_t total_sectors;    /* total sectors on the disk               */
  int has_valid_mbr;         /* 1 if MBR signature 0xAA55 is present    */
  int existing_os;           /* 1 if a non-NovexOS partition exists     */
  uint8_t existing_type;     /* partition type byte of existing OS      */
  uint32_t existing_lba;     /* LBA start of the first existing OS part */
  uint32_t existing_sectors; /* sector count of that partition          */
} disk_info_t;

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

/* Probe disk and fill *info; returns 1 on success, 0 if no disk found */
int installer_probe(disk_info_t *info);

/* Get pointer to the last probed disk_info (no new probe) */
const disk_info_t *installer_get_info(void);

/* Progress callback: percent (0-100), msg */
typedef void (*install_cb_t)(int percent, const char *msg);

/* Full-disk install: write MBR, format entire disk with chosen FS,
   copy kernel from RAM, write Stage2 + GRUB hint.
   Returns 1 on success, 0 on failure.                                   */
int installer_full_install(install_fs_t fs, install_cb_t cb);

/* Dual-boot install: shrink the existing OS partition to
   shrink_to_sectors, create a new NovexOS partition in freed space,
   format with chosen FS, copy kernel.
   Returns 1 on success, 0 on failure.                                   */
int installer_dualboot_install(install_fs_t fs, uint32_t shrink_to_sectors,
                               install_cb_t cb);

/* Low-level format helpers */
int installer_format_fat32(uint32_t lba_start, uint32_t sectors,
                           install_cb_t cb);
int installer_format_ntfs(uint32_t lba_start, uint32_t sectors,
                          install_cb_t cb);
int installer_format_ext4(uint32_t lba_start, uint32_t sectors,
                          install_cb_t cb);

/* Write GRUB-compatible hint sector (sector 9) so GRUB can chainload   */
void installer_write_grub_entry(uint32_t novex_lba, uint32_t novex_sectors);

/* Copy the running kernel from RAM to the FAT32 partition; also patches
   the Stage2 loader (sectors 1-8) with the correct LBA / entry point.  */
int installer_write_kernel(uint32_t lba_start);

/* Logging / Diagnostics */
void installer_log(const char *msg);
void installer_log_hex(const char *prefix, uint32_t val);

/* Save language, keyboard, and resolution to SETTINGS.DAT */
void installer_save_settings(void);

#endif /* INSTALLER_H */