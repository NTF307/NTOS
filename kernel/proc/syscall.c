/*
 * syscall.c - system call table. Every user pointer is checked against the
 * caller's page tables (present + user + writable as needed) before use.
 */
#include "proc.h"
#include "lib.h"
#include "console.h"
#include "drivers.h"

/* Layout matches the push order in syscall.asm. */
typedef struct {
    uint64_t r15, r14, r13, r12, r10, r9, r8, rbp, rdi, rsi, rdx, rbx, rax;
    uint64_t rip, rflags, rsp;
} syscall_frame;

enum {
    SYS_EXIT, SYS_WRITE, SYS_READ, SYS_SLEEP, SYS_GETPID, SYS_YIELD,
    SYS_SPAWN, SYS_WAIT, SYS_SBRK, SYS_UPTIME, SYS_COUNT
};

extern void syscall_entry(void);

void syscall_init(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);                          /* SCE: enable SYSCALL */
    /* SYSCALL: CS=0x08 SS=0x10.  SYSRET (64-bit): CS=0x10+16|3=0x23, SS=0x10+8|3=0x1B. */
    wrmsr(MSR_STAR, (0x10ULL << 48) | (0x08ULL << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x700);                                       /* clear TF, IF, DF on entry */
}

/* Copy a NUL-terminated user string; returns its length or a negative error. */
static int64_t copy_str(address_space *as, uint64_t u, char *dst, size_t cap)
{
    for (size_t i = 0; i < cap; i++) {
        if ((i == 0 || ((u + i) & (PAGE_SIZE - 1)) == 0) && !vmm_user_ok(as, u + i, 1, false)) return -E_FAULT;
        dst[i] = *(const char *)(u + i);
        if (!dst[i]) return (int64_t)i;
    }
    return -E_INVAL;
}

static int64_t sys_write(process_t *p, uint64_t fd, uint64_t buf, uint64_t len)
{
    if (fd != 1 && fd != 2) return -E_BADF;
    if (len > 65536) len = 65536;
    if (!vmm_user_ok(p->as, buf, len, false)) return -E_FAULT;
    console_write((const char *)buf, len);
    serial_write((const char *)buf, len);
    return (int64_t)len;
}

static void echo(char c)
{
    console_putc(c);
    if (c == '\b') serial_write("\b \b", 3);
    else serial_putc(c);
}

/* fd 0: one edited line from the keyboard (Backspace edits, Ctrl+D on an empty line = EOF). */
static int64_t sys_read(process_t *p, uint64_t fd, uint64_t buf, uint64_t len)
{
    if (fd != 0) return -E_BADF;
    if (len == 0) return 0;
    if (!vmm_user_ok(p->as, buf, len, true)) return -E_FAULT;
    char line[256];
    size_t cap = MIN(len, sizeof line), n = 0;
    console_set_cursor(true);
    for (;;) {
        key_event ev;
        if (!keyboard_get_event(&ev, true)) { console_set_cursor(false); return -E_INTR; }
        if (!ev.pressed || !ev.ascii) continue;
        char c = ev.ascii;
        if (c == '\n') { echo('\n'); if (n < cap) line[n++] = '\n'; break; }
        if (c == '\b') { if (n) { n--; echo('\b'); } continue; }
        if (c == 4) { if (n == 0) break; continue; }               /* Ctrl+D */
        if (c >= 32 && c < 127 && n + 1 < cap) { line[n++] = c; echo(c); }
    }
    console_set_cursor(false);
    memcpy((void *)buf, line, n);
    return (int64_t)n;
}

static int64_t sys_spawn(process_t *p, uint64_t path_u, uint64_t argv_u)
{
    char path[64], strings[PROC_ARG_BYTES];
    const char *argv[PROC_ARGS_MAX];
    int64_t r = copy_str(p->as, path_u, path, sizeof path);
    if (r < 0) return r;
    int argc = 0;
    if (!argv_u) {
        argv[argc++] = path;
    } else {
        size_t used = 0;
        for (;; argc++) {
            if (!vmm_user_ok(p->as, argv_u + (uint64_t)argc * 8, 8, false)) return -E_FAULT;
            uint64_t s = ((const uint64_t *)argv_u)[argc];
            if (!s) break;
            if (argc == PROC_ARGS_MAX) return -E_INVAL;
            r = copy_str(p->as, s, strings + used, sizeof strings - used);
            if (r < 0) return r;
            argv[argc] = strings + used;
            used += (size_t)r + 1;
        }
    }
    return process_spawn(path, argc, argv, p->pid, false);
}

static int64_t sys_wait(process_t *p, uint64_t pid, uint64_t status_u)
{
    if (status_u && !vmm_user_ok(p->as, status_u, sizeof(int), true)) return -E_FAULT;
    int code = 0;
    int r = process_wait((uint32_t)pid, p->pid, &code);
    if (r > 0 && status_u) *(int *)status_u = code;
    return r;
}

static int64_t sys_sbrk(process_t *p, int64_t inc)
{
    uint64_t old = p->brk, new = old + (uint64_t)inc;
    if (inc == 0) return (int64_t)old;
    if (new < p->brk_start || new > p->brk_start + USER_HEAP_MAX) return -E_NOMEM;
    for (uint64_t va = ALIGN_UP(old, PAGE_SIZE); va < ALIGN_UP(new, PAGE_SIZE); va += PAGE_SIZE)
        if (!vmm_translate(p->as, va, NULL) && vmm_alloc(p->as, va, VMM_USER | VMM_WRITE)) return -E_NOMEM;
    p->brk = new;
    return (int64_t)old;
}

void syscall_dispatch(syscall_frame *f)
{
    process_t *p = process_current();
    uint64_t a0 = f->rdi, a1 = f->rsi, a2 = f->rdx;
    int64_t r;
    switch (f->rax) {
    case SYS_EXIT:   process_exit((int)a0);
    case SYS_WRITE:  r = sys_write(p, a0, a1, a2); break;
    case SYS_READ:   r = sys_read(p, a0, a1, a2); break;
    case SYS_SLEEP:  thread_sleep_ms(a0); r = 0; break;
    case SYS_GETPID: r = p->pid; break;
    case SYS_YIELD:  thread_yield(); r = 0; break;
    case SYS_SPAWN:  r = sys_spawn(p, a0, a1); break;
    case SYS_WAIT:   r = sys_wait(p, a0, a1); break;
    case SYS_SBRK:   r = sys_sbrk(p, (int64_t)a0); break;
    case SYS_UPTIME: r = (int64_t)(timer_ticks() * 1000 / timer_hz()); break;
    default:         r = -E_NOSYS; break;
    }
    f->rax = (uint64_t)r;
    process_check_kill();
}
