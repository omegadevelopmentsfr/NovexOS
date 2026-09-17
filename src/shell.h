/*
 * NovexOS - shell.h
 * Shell public API.
 */

#ifndef SHELL_H
#define SHELL_H

#define SHELL_MAX_CMD 256

void shell_init(void);
void shell_input(char c);
int shell_is_editor_active(void);
int shell_is_disk(void);
const char *shell_get_cwd(void);

/* Build the full ram:/disk-relative path for `name` inside the current
 * working directory (handles the one level of subfolder nesting that
 * ramfs/fat32 support). `out` must be at least 64 bytes. */
void shell_build_path(char *out, const char *name);

#endif /* SHELL_H */
