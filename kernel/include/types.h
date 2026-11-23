#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#define NORETURN    __attribute__((noreturn))
#define PACKED      __attribute__((packed))
#define ALIGNED(n)  __attribute__((aligned(n)))
#define UNUSED      __attribute__((unused))

#define PAGE_SIZE         4096ULL
#define ARRAY_SIZE(a)     (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)    (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a)  ((x) & ~((uint64_t)(a) - 1))
#define MIN(a, b)         ((a) < (b) ? (a) : (b))
#define MAX(a, b)         ((a) > (b) ? (a) : (b))
