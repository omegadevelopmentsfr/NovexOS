/*
 * OmegaOS - string.c
 * Freestanding implementations of basic string/memory functions.
 */

#include "string.h"

size_t strlen(const char *str) {
  size_t len = 0;
  while (str[len])
    len++;
  return len;
}

int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s1[i] != s2[i]) {
      return (unsigned char)s1[i] - (unsigned char)s2[i];
    }
    if (s1[i] == '\0')
      break;
  }
  return 0;
}

char *strcpy(char *dest, const char *src) {
  char *d = dest;
  while ((*d++ = *src++))
    ;
  return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
  size_t i;
  for (i = 0; i < n && src[i] != '\0'; i++) {
    dest[i] = src[i];
  }
  for (; i < n; i++) {
    dest[i] = '\0';
  }
  return dest;
}

void *memset(void *ptr, int value, size_t num) {
  uint8_t *p = (uint8_t *)ptr;
  for (size_t i = 0; i < num; i++) {
    p[i] = (uint8_t)value;
  }
  return ptr;
}

void *memcpy(void *dest, const void *src, size_t num) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;
  for (size_t i = 0; i < num; i++) {
    d[i] = s[i];
  }
  return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const uint8_t *p1 = (const uint8_t *)s1;
  const uint8_t *p2 = (const uint8_t *)s2;
  for (size_t i = 0; i < n; i++) {
    if (p1[i] != p2[i]) {
      return p1[i] - p2[i];
    }
  }
  return 0;
}

char *strchr(const char *s, int c) {
  while (*s) {
    if (*s == (char)c)
      return (char *)s;
    s++;
  }
  if (c == '\0')
    return (char *)s;
  return NULL;
}

void int_to_str(uint32_t n, char *buf) {
  if (n == 0) {
    buf[0] = '0';
    buf[1] = '\0';
    return;
  }
  char tmp[12];
  int i = 0;
  while (n > 0) {
    tmp[i++] = '0' + (n % 10);
    n /= 10;
  }
  int j = 0;
  while (i > 0) {
    buf[j++] = tmp[--i];
  }
  buf[j] = '\0';
}
