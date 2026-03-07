/*
 * OmegaOS - vfs.h
 * Virtual Filesystem Layer - Abstraction for multiple filesystems.
 * Supports FAT32, NTFS (basic), ext4 (basic), and RAMFS fallback.
 */

#ifndef VFS_H
#define VFS_H

#include "types.h"

/* Filesystem types */
typedef enum {
  FS_NONE = 0,
  FS_FAT32 = 1,
  FS_NTFS = 2,
  FS_EXT4 = 3,
  FS_RAMFS = 4,
} fs_type_t;

/* VFS Operations Structure */
typedef struct {
  const char *name;
  int (*detect)(void);           /* Returns 1 if filesystem detected */
  void (*init)(void);            /* Initialize filesystem */
  int (*read_file)(const char *filename, uint8_t *buffer, uint32_t max_len);
  void (*list_dir)(const char *path);
} vfs_operations_t;

/* Initialize VFS and detect available filesystems */
void vfs_init(void);

/* Read file from any detected filesystem */
int vfs_read_file(const char *filename, uint8_t *buffer, uint32_t max_len);

/* List directory */
void vfs_list_dir(const char *path);

/* Get detected filesystem type */
fs_type_t vfs_get_fs_type(void);

#endif /* VFS_H */
