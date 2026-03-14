/*
 * NovexOS - string.c
 * Freestanding string/memory functions — optimized for x86-64.
 *
 * memcpy / memset use 64-bit word operations for bulk data, giving up to
 * 8x throughput improvement over byte-by-byte loops on modern CPUs.
 * memset32 fills uint32_t arrays (framebuffer, pixel rows) with minimal
 * overhead by packing two 32-bit values into every 64-bit store.
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
    if (s1[i] != s2[i])
      return (unsigned char)s1[i] - (unsigned char)s2[i];
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
  for (i = 0; i < n && src[i] != '\0'; i++)
    dest[i] = src[i];
  for (; i < n; i++)
    dest[i] = '\0';
  return dest;
}

/*
 * memset — 64-bit bulk fill.
 * Replicates the byte value into a uint64_t and writes 8 bytes at a time.
 * Handles any size and any alignment (x86-64 tolerates unaligned stores).
 */
void *memset(void *ptr, int value, size_t num) {
  uint8_t  v8  = (uint8_t)value;
  uint64_t v64 = (uint64_t)v8 * 0x0101010101010101ULL;

  uint64_t *p64   = (uint64_t *)ptr;
  size_t    words = num >> 3;          /* num / 8 */
  for (size_t i = 0; i < words; i++)
    p64[i] = v64;

  uint8_t *p8   = (uint8_t *)(p64 + words);
  size_t   tail = num & 7;
  for (size_t i = 0; i < tail; i++)
    p8[i] = v8;

  return ptr;
}

/*
 * memcpy — 64-bit bulk copy.
 * Copies 8 bytes per iteration; handles tail bytes individually.
 * Both src and dst are assumed to be at least 4-byte aligned (framebuffer
 * data), so 64-bit reads are always safe on x86-64.
 */
void *memcpy(void *dest, const void *src, size_t num) {
  uint64_t       *d64   = (uint64_t *)dest;
  const uint64_t *s64   = (const uint64_t *)src;
  size_t          words = num >> 3;
  for (size_t i = 0; i < words; i++)
    d64[i] = s64[i];

  uint8_t       *d8   = (uint8_t *)(d64 + words);
  const uint8_t *s8   = (const uint8_t *)(s64 + words);
  size_t         tail = num & 7;
  for (size_t i = 0; i < tail; i++)
    d8[i] = s8[i];

  return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
  const uint8_t *p1 = (const uint8_t *)s1;
  const uint8_t *p2 = (const uint8_t *)s2;
  for (size_t i = 0; i < n; i++) {
    if (p1[i] != p2[i])
      return p1[i] - p2[i];
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

/*
 * memset32 — fill a uint32_t array with a 32-bit value.
 * Packs two pixels into each 64-bit store, halving the number of write
 * instructions vs a plain 32-bit loop.  Used by fb_fill_rect / fb_hline.
 */
void memset32(uint32_t *dst, uint32_t val, size_t count) {
  uint64_t v64   = ((uint64_t)val << 32) | val;
  uint64_t *d64  = (uint64_t *)dst;
  size_t    pairs = count >> 1;         /* count / 2 */
  for (size_t i = 0; i < pairs; i++)
    d64[i] = v64;
  if (count & 1)
    dst[count - 1] = val;
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
  while (i > 0)
    buf[j++] = tmp[--i];
  buf[j] = '\0';
}
