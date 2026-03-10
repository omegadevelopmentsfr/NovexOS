/*
 * NovexOS - types.h
 * Global type definitions and stdint replacement for x86_64 (64-bit).
 */

#ifndef TYPES_H
#define TYPES_H

/* Exact-width integer types */
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long int64_t;

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;

/* Pointer-sized integers (64-bit on x86_64) */
typedef long intptr_t;
typedef unsigned long uintptr_t;

/* Size type (64-bit) */
typedef unsigned long size_t;

/* Boolean type */
#ifndef __cplusplus
typedef _Bool bool;
#define true 1
#define false 0
#endif

#define NULL ((void *)0)

#endif /* TYPES_H */
