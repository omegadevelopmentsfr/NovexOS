/*
 * NovexOS - editor.h
 * Text editor public API.
 */

#ifndef EDITOR_H
#define EDITOR_H

void editor_open(const char *filename);
void editor_input(char c);
int editor_is_active(void);

#endif /* EDITOR_H */
