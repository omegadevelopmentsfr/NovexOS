/*
 * NovexOS - ramfs.c
 * Simple in-memory flat filesystem.
 * No directories — just a flat array of named files.
 */

#include "ramfs.h"
#include "string.h"

static struct ramfs_file files[RAMFS_MAX_FILES];

void ramfs_init(void) {
  for (int i = 0; i < RAMFS_MAX_FILES; i++) {
    files[i].used = false;
    files[i].size = 0;
    files[i].is_dir = false;
    memset(files[i].name, 0, RAMFS_MAX_NAME);
    memset(files[i].data, 0, RAMFS_MAX_FILESIZE);
  }
}

/* Find a file by name, return index or -1 */
static int ramfs_find(const char *name) {
  for (int i = 0; i < RAMFS_MAX_FILES; i++) {
    if (files[i].used && strcmp(files[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

/* Find a free slot, return index or -1 */
static int ramfs_find_free(void) {
  for (int i = 0; i < RAMFS_MAX_FILES; i++) {
    if (!files[i].used)
      return i;
  }
  return -1;
}

int ramfs_write(const char *name, const char *data, uint32_t size) {
  if (size > RAMFS_MAX_FILESIZE)
    return -1;

  int idx = ramfs_find(name);
  if (idx < 0) {
    idx = ramfs_find_free();
    if (idx < 0)
      return -1; /* No space */
  }

  strncpy(files[idx].name, name, RAMFS_MAX_NAME - 1);
  files[idx].name[RAMFS_MAX_NAME - 1] = '\0';
  memcpy(files[idx].data, data, size);
  files[idx].data[size] = '\0';
  files[idx].size = size;
  files[idx].used = true;
  files[idx].is_dir = false;

  return 0;
}

int ramfs_mkdir(const char *name) {
  int idx = ramfs_find(name);
  if (idx >= 0)
    return -1; /* Already exists */

  idx = ramfs_find_free();
  if (idx < 0)
    return -1;

  strncpy(files[idx].name, name, RAMFS_MAX_NAME - 1);
  files[idx].name[RAMFS_MAX_NAME - 1] = '\0';
  files[idx].size = 0;
  files[idx].used = true;
  files[idx].is_dir = true;
  return 0;
}

const char *ramfs_read(const char *name, uint32_t *out_size) {
  int idx = ramfs_find(name);
  if (idx < 0 || files[idx].is_dir)
    return NULL;

  if (out_size)
    *out_size = files[idx].size;
  return files[idx].data;
}

int ramfs_delete(const char *name) {
  int idx = ramfs_find(name);
  if (idx < 0)
    return -1;

  files[idx].used = false;
  files[idx].size = 0;
  files[idx].is_dir = false;
  memset(files[idx].name, 0, RAMFS_MAX_NAME);
  return 0;
}

void ramfs_list(void (*callback)(const char *name, uint32_t size,
                                 bool is_dir)) {
  for (int i = 0; i < RAMFS_MAX_FILES; i++) {
    if (files[i].used) {
      callback(files[i].name, files[i].size, files[i].is_dir);
    }
  }
}

int ramfs_file_count(void) {
  int count = 0;
  for (int i = 0; i < RAMFS_MAX_FILES; i++) {
    if (files[i].used)
      count++;
  }
  return count;
}
