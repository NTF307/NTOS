/* ntos.h - NTOS user-space API: system calls and a tiny C library */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

/* System call numbers (must match kernel/proc/syscall.c) */
enum {
    SYS_EXIT, SYS_WRITE, SYS_READ, SYS_SLEEP, SYS_GETPID, SYS_YIELD,
    SYS_SPAWN, SYS_WAIT, SYS_SBRK, SYS_UPTIME,
};

/* Error codes (system calls return -E...) */
enum { ENOENT = 2, EINTR = 4, ENOEXEC = 8, EBADF = 9, ECHILD = 10, ENOMEM = 12,
       EFAULT = 14, EINVAL = 22, ENOSYS = 38 };

static inline long syscall3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

/* system calls */
__attribute__((noreturn)) void exit(int code);
long  write(int fd, const void *buf, size_t len);
long  read(int fd, void *buf, size_t len);       /* fd 0: one line from the keyboard */
void  sleep_ms(uint64_t ms);
int   getpid(void);
void  yield(void);
int   spawn(const char *path, const char *const *argv);   /* returns pid or -error */
int   wait(int pid, int *status);
void *sbrk(long increment);                      /* (void *)-1 on failure */
uint64_t uptime_ms(void);

/* string / memory */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *d, const char *s);
int    atoi(const char *s);

/* stdio */
int  vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  puts(const char *s);

/* heap */
void *malloc(size_t n);
void  free(void *p);
