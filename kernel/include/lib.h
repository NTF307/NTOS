/* lib.h - freestanding C library subset + kernel printf/panic */
#pragma once
#include "types.h"

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strncpy(char *dst, const char *src, size_t n);

#define PRINTF_FMT(f, a) __attribute__((format(printf, f, a)))

int  kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int  ksnprintf(char *buf, size_t cap, const char *fmt, ...) PRINTF_FMT(3, 4);
void kprintf(const char *fmt, ...) PRINTF_FMT(1, 2);
NORETURN void panic(const char *fmt, ...) PRINTF_FMT(1, 2);
