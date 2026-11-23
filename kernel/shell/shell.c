/* shell.c - NTOS interactive kernel shell */
#include "lib.h"
#include "console.h"
#include "drivers.h"
#include "arch.h"
#include "mm.h"
#include "theme.h"
#include "version.h"
#include "sched.h"
#include "proc.h"

#define LINE_MAX  256
#define HIST_MAX  16
#define ARGS_MAX  16

extern const ntos_boot_info *g_boot_info;

static char history[HIST_MAX][LINE_MAX];
static int  hist_count;

/* ------------------------------------------------------------ helpers */
static void out_char(char c) { console_putc(c); serial_putc(c); }
static void out_str(const char *s) { while (*s) out_char(*s++); }

static void erase_chars(size_t n)
{
    while (n--) { console_putc('\b'); serial_write("\b \b", 3); }
}

static void prompt(void)
{
    uint32_t f, b;
    console_get_color(&f, &b);
    console_set_color(THEME_PROMPT, b);
    kprintf("ntos");
    console_set_color(THEME_DIM, b);
    kprintf(":/");
    console_set_color(f, b);
    kprintf("> ");
}

static size_t read_line(char *buf)
{
    size_t len = 0;
    int hpos = hist_count;
    console_set_cursor(true);
    for (;;) {
        int k = keyboard_getchar();
        if (k == '\n') { out_char('\n'); break; }
        if (k == '\b') { if (len) { len--; erase_chars(1); } continue; }
        if (k == 3) { out_str("^C\n"); len = 0; break; }                  /* Ctrl+C */
        if (k == 12) {                                                     /* Ctrl+L */
            console_clear(); prompt();
            for (size_t i = 0; i < len; i++) out_char(buf[i]);
            continue;
        }
        if (k == KEY_UP || k == KEY_DOWN) {
            int np = hpos + (k == KEY_UP ? -1 : 1);
            if (np < 0 || np > hist_count) continue;
            hpos = np;
            erase_chars(len);
            if (hpos == hist_count) len = 0;
            else { strncpy(buf, history[hpos], LINE_MAX - 1); buf[LINE_MAX - 1] = 0; len = strlen(buf); }
            for (size_t i = 0; i < len; i++) out_char(buf[i]);
            continue;
        }
        if (k >= 32 && k < 127 && len < LINE_MAX - 1) { buf[len++] = (char)k; out_char((char)k); }
    }
    console_set_cursor(false);
    buf[len] = 0;
    if (len && (hist_count == 0 || strcmp(history[hist_count - 1], buf) != 0)) {
        if (hist_count == HIST_MAX) {               /* drop the oldest entry */
            memmove(history[0], history[1], (HIST_MAX - 1) * LINE_MAX);
            hist_count--;
        }
        strncpy(history[hist_count++], buf, LINE_MAX - 1);
    }
    return len;
}

static void cpu_brand(char *out, size_t cap)
{
    uint32_t a, b, c, d, regs[12];
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000004) {
        cpuid(0, 0, &a, &b, &c, &d);
        uint32_t v[3] = { b, d, c };
        memcpy(out, v, 12); out[12] = 0;
        return;
    }
    for (uint32_t i = 0; i < 3; i++) cpuid(0x80000002 + i, 0, &regs[i * 4], &regs[i * 4 + 1], &regs[i * 4 + 2], &regs[i * 4 + 3]);
    char *s = (char *)regs;
    s[47] = 0;
    while (*s == ' ') s++;
    strncpy(out, s, cap - 1);
    out[cap - 1] = 0;
}

static void print_uptime(void)
{
    uint64_t secs = timer_ticks() / timer_hz();
    kprintf("%lu:%02lu:%02lu", secs / 3600, (secs / 60) % 60, secs % 60);
}

/* ----------------------------------------------------------- commands */
typedef struct { const char *name, *help; void (*fn)(int argc, char **argv); } command;
static const command commands[];

static void cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("Available commands:\n");
    for (const command *c = commands; c->name; c++) {
        uint32_t f, b;
        console_get_color(&f, &b);
        console_set_color(THEME_ACCENT, b);
        kprintf("  %-11s", c->name);
        console_set_color(f, b);
        kprintf(" %s\n", c->help);
    }
    kprintf("Keys: Up/Down = history, Ctrl+C = cancel line, Ctrl+L = clear screen, Ctrl+Alt+Del = reboot\n");
}

static void cmd_clear(int argc, char **argv) { (void)argc; (void)argv; console_clear(); }

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    kprintf("\n");
}

static void cmd_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    char brand[49];
    cpu_brand(brand, sizeof brand);
    const ntos_framebuffer *fb = &g_boot_info->fb;
    kprintf("NTOS %s (build %d, %s)\n", NTOS_VERSION, NTOS_BUILD, NTOS_BUILD_DATE);
    kprintf("  CPU:         %s\n", brand);
    kprintf("  Memory:      %lu MiB usable, %lu MiB free\n", pmm_total_bytes() >> 20, pmm_free_bytes() >> 20);
    kprintf("  Display:     %ux%u, %u columns x %u rows\n", fb->width, fb->height, console_cols(), console_rows());
    kprintf("  Kernel:      %lu KiB at physical 0x%lx\n", g_boot_info->kernel_size >> 10, g_boot_info->kernel_phys);
    kprintf("  ACPI RSDP:   0x%lx\n", g_boot_info->rsdp);
    kprintf("  Serial log:  %s\n", serial_present() ? "COM1" : "not present");
    kprintf("  Uptime:      "); print_uptime(); kprintf("\n");
}

static void cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const char *const names[] = { "?", "usable", "reserved", "ACPI reclaim", "ACPI NVS", "bootloader", "bad" };
    kprintf("Physical memory map (%lu regions):\n", g_boot_info->mem_count);
    for (uint64_t i = 0; i < g_boot_info->mem_count; i++) {
        const ntos_mem_region *r = &g_boot_info->mem[i];
        uint32_t t = r->type < ARRAY_SIZE(names) ? r->type : 0;
        uint64_t kib = r->length >> 10;
        kprintf("  %016lx - %016lx  %8lu %s  %s\n", r->base, r->base + r->length - 1,
                kib >= 10240 ? kib >> 10 : kib, kib >= 10240 ? "MiB" : "KiB", names[t]);
    }
    size_t total, used, blocks;
    heap_stats(&total, &used, &blocks);
    kprintf("PMM:  %lu MiB usable, %lu MiB free\n", pmm_total_bytes() >> 20, pmm_free_bytes() >> 20);
    kprintf("Heap: %lu KiB total, %lu KiB used, %lu blocks\n", total >> 10, used >> 10, blocks);
}

static void cmd_uptime(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("Up "); print_uptime(); kprintf(" (%lu ticks at %u Hz)\n", timer_ticks(), timer_hz());
}

static void cmd_heaptest(int argc, char **argv)
{
    (void)argc; (void)argv;
    void *p[64];
    size_t before_total, before_used, blocks;
    heap_stats(&before_total, &before_used, &blocks);
    for (int i = 0; i < 64; i++) {
        p[i] = kmalloc(16 + (size_t)i * 97);
        if (!p[i]) { kprintf("kmalloc failed at %d\n", i); return; }
        memset(p[i], i, 16 + (size_t)i * 97);
    }
    for (int i = 0; i < 64; i += 2) kfree(p[i]);
    for (int i = 1; i < 64; i += 2) {
        uint8_t *b = p[i];
        for (size_t j = 0; j < 16 + (size_t)i * 97; j++)
            if (b[j] != (uint8_t)i) { kprintf("heap corruption in block %d\n", i); return; }
        kfree(p[i]);
    }
    size_t t, u;
    heap_stats(&t, &u, &blocks);
    kprintf("heaptest: 64 allocations verified and freed; used %lu -> %lu bytes, %lu block(s)\n",
            before_used, u, blocks);
}

static void cmd_crash(int argc, char **argv)
{
    const char *kind = argc > 1 ? argv[1] : "ud";
    kprintf("Triggering exception '%s'...\n", kind);
    if (strcmp(kind, "pf") == 0) { volatile uint64_t *bad = (uint64_t *)0xFFFF900000000000ULL; *bad = 1; }
    else if (strcmp(kind, "div") == 0) { volatile int z = 0; volatile int r = 1 / z; (void)r; }
    else __asm__ volatile("ud2");
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("Rebooting...\n");
    timer_sleep_ms(300);
    cpu_reboot();
}

static void cmd_halt(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("System halted. It is now safe to turn off the machine.\n");
    cpu_halt_forever();
}

/* ------------------------------------------------- threads and input */
static bool parse_uint(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (!s || !*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return true;
}

static void error(const char *msg)
{
    uint32_t f, b;
    console_get_color(&f, &b);
    console_set_color(THEME_ERROR, b);
    kprintf("%s\n", msg);
    console_set_color(f, b);
}

static void cmd_ps(int argc, char **argv)
{
    (void)argc; (void)argv;
    static thread_info list[64];
    size_t n = thread_list(list, ARRAY_SIZE(list));
    uint32_t hz = timer_hz();
    kprintf("  TID  MODE    STATE       CPU ms   SWITCHES  STACK  NAME\n");
    for (size_t i = 0; i < n; i++)
        kprintf("  %3u  %-6s  %-9s %8lu %10lu  %3luK   %s\n", list[i].tid, list[i].user ? "user" : "kernel",
                thread_state_name(list[i].state), list[i].cpu_ticks * 1000 / hz, list[i].switches,
                list[i].stack_size >> 10, list[i].name);
    kprintf("%lu thread(s), %lu context switches since boot\n", n, sched_context_switches());
}

static void cmd_kill(int argc, char **argv)
{
    uint64_t tid;
    if (argc < 2 || !parse_uint(argv[1], &tid)) { error("usage: kill <tid>"); return; }
    int r = thread_kill((uint32_t)tid);
    if (r == 0) kprintf("killed thread %lu\n", tid);
    else if (r == -2) error("that thread cannot be killed");
    else error("no such thread");
}

/* CPU-bound worker: proves preemption keeps the shell responsive. */
static void spinner_main(void *arg)
{
    (void)arg;
    volatile uint64_t n = 0;
    for (;;) n++;
}

static void cmd_spawn(int argc, char **argv)
{
    uint64_t count = 1;
    if (argc > 1 && (!parse_uint(argv[1], &count) || count == 0 || count > 32)) { error("usage: spawn [1-32]"); return; }
    for (uint64_t i = 0; i < count; i++) {
        thread_t *t = thread_create("spinner", spinner_main, NULL);
        if (!t) { error("thread_create failed"); return; }
        kprintf("started spinner, tid %u\n", t->tid);
    }
    kprintf("These loop forever; use 'ps' to watch and 'kill <tid>' to stop them.\n");
}

static void cmd_sleep(int argc, char **argv)
{
    uint64_t ms;
    if (argc < 2 || !parse_uint(argv[1], &ms)) { error("usage: sleep <ms>"); return; }
    uint64_t t0 = timer_ticks();
    thread_sleep_ms(ms);
    kprintf("slept %lu ms\n", (timer_ticks() - t0) * 1000 / timer_hz());
}

#define TT_THREADS 8
#define TT_ITERS   20000
static mutex_t           tt_lock;
static volatile uint64_t tt_counter;
static volatile uint32_t tt_done;

static void tt_worker(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    for (int i = 0; i < TT_ITERS; i++) {
        mutex_lock(&tt_lock);
        uint64_t v = tt_counter;                       /* read-modify-write: racy without the lock */
        if ((i & 255) == (int)id) thread_yield();      /* widen the window on purpose */
        tt_counter = v + 1;
        mutex_unlock(&tt_lock);
    }
    if (id & 1) thread_sleep_ms(20);
    uint64_t f = irq_save();
    tt_done++;
    irq_restore(f);
}

static void cmd_threadtest(int argc, char **argv)
{
    (void)argc; (void)argv;
    tt_counter = 0;
    tt_done = 0;
    uint64_t sw0 = sched_context_switches(), t0 = timer_ticks();
    for (uintptr_t i = 0; i < TT_THREADS; i++)
        if (!thread_create("tt-worker", tt_worker, (void *)i)) { error("thread_create failed"); return; }
    while (tt_done < TT_THREADS) thread_sleep_ms(5);
    uint64_t ms = (timer_ticks() - t0) * 1000 / timer_hz();
    bool pass = tt_counter == (uint64_t)TT_THREADS * TT_ITERS;

    uint64_t s0 = timer_ticks();
    thread_sleep_ms(100);
    uint64_t slept = (timer_ticks() - s0) * 1000 / timer_hz();
    pass = pass && slept >= 100 && slept <= 120;

    kprintf("threadtest: %u threads x %u locked increments = %lu (expected %u)\n",
            TT_THREADS, TT_ITERS, tt_counter, TT_THREADS * TT_ITERS);
    kprintf("            %lu context switches in %lu ms, sleep(100) took %lu ms\n",
            sched_context_switches() - sw0, ms, slept);
    uint32_t f, b;
    console_get_color(&f, &b);
    console_set_color(pass ? THEME_OK : THEME_ERROR, b);
    kprintf("threadtest: %s\n", pass ? "PASS" : "FAIL");
    console_set_color(f, b);
}

static void print_mouse(const mouse_state *s)
{
    kprintf("x=%4d y=%4d  buttons [%c%c%c]  packets %lu", s->x, s->y,
            (s->buttons & MOUSE_LEFT) ? 'L' : '-', (s->buttons & MOUSE_MIDDLE) ? 'M' : '-',
            (s->buttons & MOUSE_RIGHT) ? 'R' : '-', s->packets);
}

static void cmd_mouse(int argc, char **argv)
{
    mouse_state s;
    mouse_get_state(&s);
    if (!s.present) { error("no PS/2 mouse"); return; }
    bool live = argc > 1 && strcmp(argv[1], "live") == 0;
    kprintf("PS/2 mouse%s\n", s.has_wheel ? " with scroll wheel" : "");
    if (!live) { print_mouse(&s); kprintf("\n"); return; }

    kprintf("Live view - press any key to stop.\n");
    int key;
    for (;;) {
        mouse_get_state(&s);
        kprintf("\r");
        print_mouse(&s);
        kprintf("   ");
        if (keyboard_poll(&key)) break;
        thread_sleep_ms(50);
    }
    kprintf("\n");
}

static void cmd_keys(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("Keyboard event viewer - press Esc to stop.\n");
    key_event ev;
    for (;;) {
        keyboard_get_event(&ev, true);
        uint8_t m = ev.mods;
        kprintf("  %-7s %-10s scancode %04x  mods [%s%s%s%s] %s%s%s",
                ev.pressed ? "press" : "release", keyboard_key_name(ev.key), ev.scancode,
                (m & KMOD_SHIFT) ? "S" : "-", (m & KMOD_CTRL) ? "C" : "-", (m & KMOD_ALT) ? "A" : "-",
                (m & KMOD_GUI) ? "W" : "-", (m & KMOD_CAPS) ? "Caps " : "", (m & KMOD_NUM) ? "Num " : "",
                (m & KMOD_SCROLL) ? "Scroll" : "");
        if (ev.ascii >= 32 && ev.ascii < 127) kprintf("  '%c'", ev.ascii);
        else if (ev.ascii) kprintf("  0x%02x", (unsigned char)ev.ascii);
        kprintf("\n");
        if (ev.pressed && ev.key == KEY_ESC) break;
    }
}

static void ls_entry(const char *name, uint64_t size, void *ctx)
{
    (*(int *)ctx)++;
    kprintf("  %-20s %7lu bytes\n", name, size);
}

static void cmd_ls(int argc, char **argv)
{
    (void)argc; (void)argv;
    int n = 0;
    initrd_list(ls_entry, &n);
    if (!n) error("initrd is empty or missing");
    else kprintf("%d file(s). Run a program by typing its name; add '&' to run it in the background.\n", n);
}

static const char *spawn_error(int r)
{
    switch (-r) {
    case E_NOENT:  return "no such program";
    case E_NOEXEC: return "not a valid executable";
    case E_NOMEM:  return "out of memory";
    case E_INVAL:  return "too many or too long arguments";
    default:       return "cannot start program";
    }
}

/* Start a user program; wait for it unless the last argument is '&'. */
static void run_program(int argc, char **argv)
{
    bool background = argc > 1 && strcmp(argv[argc - 1], "&") == 0;
    if (background) argc--;
    int pid = process_spawn(argv[0], argc, (const char *const *)argv, 0, background);
    if (pid < 0) {
        uint32_t f, b;
        console_get_color(&f, &b);
        console_set_color(THEME_ERROR, b);
        kprintf("%s: %s\n", argv[0], spawn_error(pid));
        console_set_color(f, b);
        return;
    }
    if (background) { kprintf("[%d] %s running in the background\n", pid, argv[0]); return; }
    process_set_foreground((uint32_t)pid);
    int code = 0;
    process_wait((uint32_t)pid, 0, &code);
    process_set_foreground(0);
    if (code == -9) kprintf("^C [killed]\n");
    else if (code != 0 && code != -1) kprintf("[exit code %d]\n", code);
}

static void cmd_run(int argc, char **argv)
{
    if (argc < 2) { error("usage: run <program> [args...] [&]"); return; }
    run_program(argc - 1, argv + 1);
}

static const command commands[] = {
    { "help",     "show this list",                          cmd_help },
    { "clear",    "clear the screen",                        cmd_clear },
    { "echo",     "print the arguments",                     cmd_echo },
    { "info",     "system information",                      cmd_info },
    { "mem",      "memory map and allocator statistics",     cmd_mem },
    { "uptime",   "time since boot",                         cmd_uptime },
    { "heaptest", "exercise the kernel heap",                cmd_heaptest },
    { "ls",       "list user programs in the initrd",        cmd_ls },
    { "run",      "run a program: run <prog> [args] [&]",    cmd_run },
    { "ps",       "list threads (user and kernel)",          cmd_ps },
    { "kill",     "stop a thread or program: kill <tid>",    cmd_kill },
    { "spawn",    "start CPU-bound test threads: spawn [n]", cmd_spawn },
    { "sleep",    "sleep for <ms> milliseconds",             cmd_sleep },
    { "threadtest", "scheduler + mutex self-test",           cmd_threadtest },
    { "mouse",    "mouse state: mouse [live]",               cmd_mouse },
    { "keys",     "show raw keyboard events (Esc quits)",    cmd_keys },
    { "crash",    "trigger an exception: crash [ud|pf|div]", cmd_crash },
    { "reboot",   "restart the machine",                     cmd_reboot },
    { "halt",     "stop the CPU",                            cmd_halt },
    { NULL, NULL, NULL },
};

/* --------------------------------------------------------------- main */
static int split(char *line, char **argv)
{
    int argc = 0;
    while (*line && argc < ARGS_MAX) {
        while (*line == ' ') *line++ = 0;
        if (!*line) break;
        argv[argc++] = line;
        while (*line && *line != ' ') line++;
    }
    return argc;
}

void shell_run(void)
{
    char line[LINE_MAX];
    char *argv[ARGS_MAX];
    keyboard_set_ctrl_c_hook(process_interrupt_foreground);
    kprintf("Type 'help' for a list of commands, 'ls' for programs.\n\n");
    for (;;) {
        prompt();
        if (!read_line(line)) continue;
        int argc = split(line, argv);
        if (!argc) continue;
        const command *c = commands;
        while (c->name && strcmp(c->name, argv[0]) != 0) c++;
        const uint8_t *img;
        uint64_t img_size;
        if (c->name) c->fn(argc, argv);
        else if (initrd_find(argv[0], &img, &img_size)) run_program(argc, argv);
        else {
            uint32_t f, b;
            console_get_color(&f, &b);
            console_set_color(THEME_ERROR, b);
            kprintf("unknown command: %s", argv[0]);
            console_set_color(f, b);
            kprintf(" (try 'help')\n");
        }
    }
}
