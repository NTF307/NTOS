/* kmain.c - NTOS kernel initialisation */
#include "arch.h"
#include "console.h"
#include "drivers.h"
#include "lib.h"
#include "mm.h"
#include "sched.h"
#include "proc.h"
#include "theme.h"
#include "version.h"

const ntos_boot_info *g_boot_info;
void shell_run(void);

static void status(uint32_t color, const char *tag, const char *msg)
{
    uint32_t f, b;
    console_get_color(&f, &b);
    kprintf("[");
    console_set_color(color, b);
    kprintf("%s", tag);
    console_set_color(f, b);
    kprintf("] %s\n", msg);
}

static void ok(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    status(THEME_OK, " OK ", buf);
}

static void warn(const char *msg) { status(THEME_WARN, "WARN", msg); }

/* Moves the on-screen pointer whenever the mouse reports motion. */
static void pointer_main(void *arg)
{
    (void)arg;
    mouse_event ev;
    for (;;) {
        if (!mouse_get_event(&ev, true)) { thread_sleep_ms(1000); continue; }
        while (mouse_get_event(&ev, false)) ;          /* only the latest position matters */
        console_pointer_set(ev.x, ev.y, true);
    }
}

static void banner(void)
{
    uint32_t f, b;
    console_get_color(&f, &b);
    console_set_color(THEME_ACCENT, b);
    kprintf("\n");
    kprintf("   _   _ _____ ___  ____\n");
    kprintf("  | \\ | |_   _/ _ \\/ ___|\n");
    kprintf("  |  \\| | | || | | \\___ \\\n");
    kprintf("  | |\\  | | || |_| |___) |\n");
    kprintf("  |_| \\_| |_| \\___/|____/\n\n");
    console_set_color(THEME_DIM, b);
    kprintf("  version %s  build %d  (%s)\n\n", NTOS_VERSION, NTOS_BUILD, NTOS_BUILD_DATE);
    console_set_color(f, b);
}

NORETURN void kmain(const ntos_boot_info *bi)
{
    serial_init();
    if (!bi || bi->magic != NTOS_BOOT_MAGIC) {
        serial_write("NTOS: invalid boot info, halting\n", 33);
        cpu_halt_forever();
    }
    g_boot_info = bi;
    g_hhdm = bi->hhdm_base;

    console_init(&bi->fb, bi->hhdm_base);
    banner();
    ok("Framebuffer %ux%u console (%u x %u)", bi->fb.width, bi->fb.height, console_cols(), console_rows());
    if (serial_present()) ok("Serial log on COM1");
    else status(THEME_WARN, "WARN", "No serial port; logging to screen only");

    gdt_init();
    ok("GDT and TSS loaded");
    idt_init();
    ok("IDT: 256 vectors, PIC remapped to 32-47");

    pmm_init(bi);
    ok("Physical memory: %lu MiB usable, %lu MiB free", pmm_total_bytes() >> 20, pmm_free_bytes() >> 20);
    if (console_enable_backbuffer()) ok("Console back buffer enabled");
    else status(THEME_WARN, "WARN", "Console back buffer unavailable");
    heap_init();
    ok("Kernel heap ready");
    vmm_init();
    ok("Virtual memory: kernel page tables, identity map removed [%s]", vmm_features());
    syscall_init();
    ok("System calls: SYSCALL/SYSRET, ring 3 user mode");

    pit_init(1000);
    sched_init();
    ok("Scheduler: preemptive round-robin, %u ms quantum, timer at %u Hz", SCHED_QUANTUM_MS, timer_hz());

    uint32_t sw, sh;
    console_screen_size(&sw, &sh);
    bool have_ps2 = ps2_init();
    bool have_kbd = have_ps2 && keyboard_init();
    bool have_mouse = have_ps2 && mouse_init(sw, sh);
    if (have_ps2) ps2_enable_irqs();
    cpu_sti();

    if (have_kbd) ok("PS/2 keyboard ready");
    else warn("No PS/2 controller: keyboard unavailable");
    if (have_mouse) {
        mouse_state ms;
        mouse_get_state(&ms);
        if (thread_create("pointer", pointer_main, NULL))
            ok("PS/2 mouse ready (%s)", ms.has_wheel ? "with scroll wheel" : "3 buttons");
        else warn("Cannot start pointer thread");
    } else {
        warn("No PS/2 mouse found");
    }

    if (initrd_init(bi->initrd_phys, bi->initrd_size)) ok("Initrd: %lu KiB of user programs", bi->initrd_size >> 10);
    else warn("No initrd: user programs unavailable");

    kprintf("\n");
    serial_write("NTOS shell ready\n", 17);
    shell_run();
    cpu_halt_forever();
}
