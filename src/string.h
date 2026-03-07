/*
 * OmegaOS - string.h
 * Freestanding string utility functions (no libc).
 */

#ifndef STRING_H
#define STRING_H

#include "types.h"

size_t strlen(const char *str);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
void *memset(void *ptr, int value, size_t num);
void *memcpy(void *dest, const void *src, size_t num);
int memcmp(const void *s1, const void *s2, size_t n);
char *strchr(const char *s, int c);
void int_to_str(uint32_t n, char *buf);

#endif /* STRING_H */
