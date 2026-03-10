/*
 * NovexOS - ramfs.h
 * Simple in-memory flat filesystem.
 */

#ifndef RAMFS_H
#define RAMFS_H

#include "types.h"

#define RAMFS_MAX_FILES 32
#define RAMFS_MAX_NAME 32
#define RAMFS_MAX_FILESIZE 4096

/* File entry */
struct ramfs_file {
  char name[RAMFS_MAX_NAME];
  char data[RAMFS_MAX_FILESIZE];
  uint32_t size;
  bool used;
  bool is_dir;
};

/* Initialize the filesystem */
void ramfs_init(void);

/* Create or overwrite a file. Returns 0 on success, -1 on failure. */
int ramfs_write(const char *name, const char *data, uint32_t size);

/* Create a directory. Returns 0 on success, -1 on failure. */
int ramfs_mkdir(const char *name);

/* Read a file. Returns pointer to data and sets *out_size, or NULL if not
 * found. */
const char *ramfs_read(const char *name, uint32_t *out_size);

/* Delete a file. Returns 0 on success, -1 if not found. */
int ramfs_delete(const char *name);

/* List all files. Calls callback(name, size, is_dir) for each file. */
void ramfs_list(void (*callback)(const char *name, uint32_t size, bool is_dir));

/* Get number of files */
int ramfs_file_count(void);

#endif /* RAMFS_H */
