/* libc.c - NTOS user library: system call wrappers, strings, printf, malloc */
#include "ntos.h"

/* ------------------------------------------------------------ syscalls */
void exit(int code)
{
    syscall3(SYS_EXIT, code, 0, 0);
    for (;;) ;
}
long  write(int fd, const void *buf, size_t len) { return syscall3(SYS_WRITE, fd, (long)buf, (long)len); }
long  read(int fd, void *buf, size_t len)        { return syscall3(SYS_READ, fd, (long)buf, (long)len); }
void  sleep_ms(uint64_t ms)                      { syscall3(SYS_SLEEP, (long)ms, 0, 0); }
int   getpid(void)                               { return (int)syscall3(SYS_GETPID, 0, 0, 0); }
void  yield(void)                                { syscall3(SYS_YIELD, 0, 0, 0); }
int   spawn(const char *path, const char *const *argv) { return (int)syscall3(SYS_SPAWN, (long)path, (long)argv, 0); }
int   wait(int pid, int *status)                 { return (int)syscall3(SYS_WAIT, pid, (long)status, 0); }
uint64_t uptime_ms(void)                         { return (uint64_t)syscall3(SYS_UPTIME, 0, 0, 0); }

void *sbrk(long inc)
{
    long r = syscall3(SYS_SBRK, inc, 0, 0);
    return r < 0 ? (void *)-1 : (void *)r;
}

/* -------------------------------------------------------------- string */
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *a = d; const uint8_t *b = s;
    if (a < b) while (n--) *a++ = *b++;
    else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
int memcmp(const void *x, const void *y, size_t n)
{
    const uint8_t *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (uint8_t)*a - (uint8_t)*b; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++) != 0) {} return r; }
int atoi(const char *s)
{
    int sign = 1, v = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v * sign;
}

/* --------------------------------------------------------------- stdio */
typedef struct { char *buf; size_t cap, pos; } sink;

static void emit(sink *s, char c) { if (s->pos + 1 < s->cap) s->buf[s->pos] = c; s->pos++; }

static void emit_str(sink *s, const char *str, size_t len, int width, bool left, char pad)
{
    int n = width > (int)len ? width - (int)len : 0;
    if (!left) while (n--) emit(s, pad);
    while (len--) emit(s, *str++);
    if (left) while (n-- > 0) emit(s, ' ');
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    sink s = { buf, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&s, *fmt); continue; }
        fmt++;
        bool left = false, zero = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else break;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 1; fmt++; }
        char tmp[24];
        int n = 0;
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            uint64_t u = v < 0 ? (uint64_t)-v : (uint64_t)v;
            do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (v < 0) tmp[n++] = '-';
            goto reverse;
        }
        case 'u': case 'x': case 'p': {
            uint64_t u;
            unsigned base = *fmt == 'u' ? 10 : 16;
            if (*fmt == 'p') { u = (uint64_t)va_arg(ap, void *); emit(&s, '0'); emit(&s, 'x'); }
            else u = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            do { tmp[n++] = "0123456789abcdef"[u % base]; u /= base; } while (u);
        reverse:
            for (int i = 0; i < n / 2; i++) { char t = tmp[i]; tmp[i] = tmp[n - 1 - i]; tmp[n - 1 - i] = t; }
            emit_str(&s, tmp, (size_t)n, width, left, zero && !left ? '0' : ' ');
            break;
        }
        case 's': { const char *str = va_arg(ap, const char *); if (!str) str = "(null)";
                    emit_str(&s, str, strlen(str), width, left, ' '); break; }
        case 'c': { char c = (char)va_arg(ap, int); emit_str(&s, &c, 1, width, left, ' '); break; }
        case '%': emit(&s, '%'); break;
        default:  emit(&s, '%'); if (*fmt) emit(&s, *fmt); else fmt--; break;
        }
    }
    if (cap) buf[s.pos < cap ? s.pos : cap - 1] = 0;
    return (int)s.pos;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    write(1, buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
    return n;
}

int puts(const char *s)
{
    write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

/* ---------------------------------------------------------------- heap */
/* First-fit free list over sbrk; blocks are 16-byte aligned. */
typedef struct block { size_t size; struct block *next; size_t free, pad; } block;
static block *heap_head;

void *malloc(size_t n)
{
    if (!n) return NULL;
    n = (n + 15) & ~(size_t)15;
    block *last = NULL;
    for (block *b = heap_head; b; last = b, b = b->next) {
        if (!b->free || b->size < n) continue;
        if (b->size >= n + sizeof(block) + 32) {          /* split */
            block *rest = (block *)((uint8_t *)(b + 1) + n);
            rest->size = b->size - n - sizeof(block);
            rest->next = b->next;
            rest->free = 1;
            b->next = rest;
            b->size = n;
        }
        b->free = 0;
        return b + 1;
    }
    block *b = sbrk((long)(sizeof(block) + n));
    if (b == (void *)-1) return NULL;
    b->size = n; b->next = NULL; b->free = 0;
    if (last) last->next = b; else heap_head = b;
    return b + 1;
}

void free(void *p)
{
    if (!p) return;
    block *b = (block *)p - 1;
    b->free = 1;
    for (block *c = heap_head; c && c->next;) {           /* merge adjacent free blocks */
        if (c->free && c->next->free && (uint8_t *)(c + 1) + c->size == (uint8_t *)c->next) {
            c->size += sizeof(block) + c->next->size;
            c->next = c->next->next;
        } else c = c->next;
    }
}
