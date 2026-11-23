/* shell.c - NTOS interactive kernel shell */
#include "lib.h"
#include "console.h"
#include "drivers.h"
#include "arch.h"
#include "mm.h"
#include "theme.h"
#include "version.h"

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
        kprintf("  %-10s", c->name);
        console_set_color(f, b);
        kprintf(" %s\n", c->help);
    }
    kprintf("Keys: Up/Down = history, Ctrl+C = cancel line, Ctrl+L = clear screen\n");
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

static const command commands[] = {
    { "help",     "show this list",                          cmd_help },
    { "clear",    "clear the screen",                        cmd_clear },
    { "echo",     "print the arguments",                     cmd_echo },
    { "info",     "system information",                      cmd_info },
    { "mem",      "memory map and allocator statistics",     cmd_mem },
    { "uptime",   "time since boot",                         cmd_uptime },
    { "heaptest", "exercise the kernel heap",                cmd_heaptest },
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
    kprintf("Type 'help' for a list of commands.\n\n");
    for (;;) {
        prompt();
        if (!read_line(line)) continue;
        int argc = split(line, argv);
        if (!argc) continue;
        const command *c = commands;
        while (c->name && strcmp(c->name, argv[0]) != 0) c++;
        if (c->name) c->fn(argc, argv);
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
