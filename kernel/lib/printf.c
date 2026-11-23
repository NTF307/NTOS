/* printf.c - kernel formatted output: %d %i %u %x %X %p %s %c %%, flags '-' '0', width, l/ll/z */
#include "lib.h"
#include "console.h"
#include "arch.h"
#include "theme.h"

typedef struct { char *buf; size_t cap, pos; } sink;

static void emit(sink *s, char c)
{
    if (s->pos + 1 < s->cap) s->buf[s->pos] = c;
    s->pos++;
}

static void emit_padded(sink *s, const char *str, size_t len, int width, bool left, char padc)
{
    int pad = width > (int)len ? width - (int)len : 0;
    if (!left) while (pad-- > 0) emit(s, padc);
    for (size_t i = 0; i < len; i++) emit(s, str[i]);
    if (left) while (pad-- > 0) emit(s, ' ');
}

int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    sink s = { buf, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&s, *fmt); continue; }
        if (!*++fmt) break;

        bool left = false, zero = false;
        for (;; fmt++) {
            if (*fmt == '-') left = true;
            else if (*fmt == '0') zero = true;
            else break;
        }
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int lng = 0;
        while (*fmt == 'l') { lng++; fmt++; }
        if (*fmt == 'z') { lng = 2; fmt++; }

        char tmp[24];
        int n = 0;
        switch (*fmt) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (v < 0) {
                if (zero && !left) { emit(&s, '-'); if (width) width--; }
                else tmp[n++] = '-';
            }
            char out[24];
            for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
            emit_padded(&s, out, n, width, left, zero && !left ? '0' : ' ');
            break;
        }
        case 'u': case 'x': case 'X': case 'p': {
            uint64_t u;
            unsigned base = *fmt == 'u' ? 10 : 16;
            const char *digits = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            if (*fmt == 'p') { u = (uint64_t)va_arg(ap, void *); emit(&s, '0'); emit(&s, 'x'); width = 16; zero = true; left = false; }
            else u = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            do { tmp[n++] = digits[u % base]; u /= base; } while (u);
            char out[24];
            for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
            emit_padded(&s, out, n, width, left, zero && !left ? '0' : ' ');
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            emit_padded(&s, str, strlen(str), width, left, ' ');
            break;
        }
        case 'c': { char c = (char)va_arg(ap, int); emit_padded(&s, &c, 1, width, left, ' '); break; }
        case '%': emit(&s, '%'); break;
        default:  emit(&s, '%'); emit(&s, *fmt); break;
        }
    }
    if (cap) buf[s.pos < cap ? s.pos : cap - 1] = 0;
    return (int)s.pos;
}

int ksnprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

void kprintf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t len = n < (int)sizeof buf ? (size_t)n : sizeof buf - 1;
    console_write(buf, len);
    serial_write(buf, len);
}

void panic(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    cpu_cli();
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    console_set_color(THEME_PANIC_FG, THEME_PANIC_BG);
    kprintf("\n\n  *** NTOS KERNEL PANIC ***\n  %s\n\n  System halted.\n", buf);
    cpu_halt_forever();
}
