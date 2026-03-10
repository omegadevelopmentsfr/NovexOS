/*
 * NovexOS - fat32.h
 * FAT32 Support.
 */

#ifndef FAT32_H
#define FAT32_H

#include "types.h"

void fat32_init(void);
void fat32_ls(const char *path);
int fat32_read_file(const char *filename, uint8_t *buffer, uint32_t max_len);
int fat32_write_file(const char *filename, uint8_t *buffer, uint32_t len);
uint32_t fat32_get_file_first_cluster(const char *filename);
int fat32_create_file(const char *filename);
int fat32_delete_file(const char *filename);
void fat32_format(void);
void fat32_print_hex(uint32_t val);

/* Get partition start LBA (for VFS filesystem detection) */
uint32_t fat32_get_part_lba_start(void);

/* Format a partition with FAT32 */
void fat32_format_partition(uint32_t lba_start, uint32_t sector_count);

#endif /* FAT32_H */
