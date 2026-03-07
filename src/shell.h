/*
 * OmegaOS - shell.h
 * Shell public API.
 */

#ifndef SHELL_H
#define SHELL_H

#define SHELL_MAX_CMD 256

void shell_init(void);
void shell_input(char c);
int shell_is_editor_active(void);

#endif /* SHELL_H */
