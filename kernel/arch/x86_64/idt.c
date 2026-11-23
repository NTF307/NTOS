/* idt.c - Interrupt Descriptor Table, exception handling, 8259 PIC, local APIC setup */
#include "arch.h"
#include "lib.h"
#include "mm.h"
#include "console.h"
#include "theme.h"
#include "sched.h"

typedef struct PACKED {
    uint16_t off_lo, selector;
    uint8_t  ist, attr;
    uint16_t off_mid;
    uint32_t off_hi, zero;
} idt_entry;

static idt_entry     idt[256];
static irq_handler_t irq_handlers[16];
static volatile uint32_t *lapic;   /* NULL if the local APIC is disabled */
extern const uint64_t isr_stub_table[256];

static struct PACKED { uint16_t limit; uint64_t base; } idtr;

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20
#define IRQ_BASE  32

static const char *const exception_names[32] = {
    "Divide Error", "Debug", "Non-Maskable Interrupt", "Breakpoint", "Overflow",
    "Bound Range Exceeded", "Invalid Opcode", "Device Not Available", "Double Fault",
    "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
    "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
    "x87 Floating-Point Error", "Alignment Check", "Machine Check", "SIMD Floating-Point",
    "Virtualization", "Control Protection", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Hypervisor Injection", "VMM Communication", "Security", "Reserved",
};

static void set_gate(int v, uint64_t handler, uint8_t ist, uint8_t attr)
{
    idt[v].off_lo   = handler & 0xFFFF;
    idt[v].selector = GDT_KERNEL_CODE;
    idt[v].ist      = ist;
    idt[v].attr     = attr;
    idt[v].off_mid  = (handler >> 16) & 0xFFFF;
    idt[v].off_hi   = handler >> 32;
    idt[v].zero     = 0;
}

static void pic_remap(void)
{
    outb(PIC1_CMD, 0x11);  io_wait();     /* ICW1: init, expect ICW4 */
    outb(PIC2_CMD, 0x11);  io_wait();
    outb(PIC1_DATA, IRQ_BASE);     io_wait();   /* ICW2: vector offsets */
    outb(PIC2_DATA, IRQ_BASE + 8); io_wait();
    outb(PIC1_DATA, 4);    io_wait();     /* ICW3: slave on IRQ2 */
    outb(PIC2_DATA, 2);    io_wait();
    outb(PIC1_DATA, 1);    io_wait();     /* ICW4: 8086 mode */
    outb(PIC2_DATA, 1);    io_wait();
    outb(PIC1_DATA, 0xFB);                /* mask everything except the cascade */
    outb(PIC2_DATA, 0xFF);
}

/*
 * UEFI firmware usually leaves the local APIC enabled. Put it in "virtual wire"
 * mode so 8259 interrupts reach the CPU through LINT0, and silence its timer.
 */
static void lapic_setup(void)
{
    uint64_t base = rdmsr(0x1B);
    if (!(base & (1ULL << 11))) return;                 /* globally disabled: legacy INTR pin */
    lapic = (volatile uint32_t *)phys_to_virt(base & 0xFFFFFF000ULL);
    lapic[0x80 / 4]  = 0;                                /* TPR: accept all */
    lapic[0xF0 / 4]  = 0x100 | 0xFF;                     /* SVR: enable, spurious vector 0xFF */
    lapic[0x320 / 4] = 0x10000;                          /* LVT timer: masked */
    lapic[0x350 / 4] = 0x700;                            /* LINT0: ExtINT (8259) */
    lapic[0x360 / 4] = 0x400;                            /* LINT1: NMI */
}

void pic_unmask(int irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

void pic_mask(int irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) | (1 << (irq & 7)));
}

void irq_install(int irq, irq_handler_t handler)
{
    if (irq >= 0 && irq < 16) irq_handlers[irq] = handler;
}

void idt_init(void)
{
    for (int v = 0; v < 256; v++)
        set_gate(v, isr_stub_table[v], v == 8 ? 1 : 0, 0x8E);   /* present, ring 0, interrupt gate */
    idtr.limit = sizeof idt - 1;
    idtr.base = (uint64_t)idt;
    __asm__ volatile("lidt %0" :: "m"(idtr));
    pic_remap();
    lapic_setup();
}

static NORETURN void exception_panic(interrupt_frame *f)
{
    cpu_cli();
    g_panicking = true;
    console_set_color(THEME_PANIC_FG, THEME_PANIC_BG);
    console_clear();
    kprintf("\n  *** NTOS KERNEL PANIC ***\n\n");
    kprintf("  Exception %lu: %s   error code 0x%lx\n\n", f->vector, exception_names[f->vector], f->error);
    if (f->vector == 14) {
        kprintf("  Faulting address (CR2): 0x%016lx  [%s, %s, %s]\n\n", read_cr2(),
                (f->error & 1) ? "protection" : "not-present",
                (f->error & 2) ? "write" : "read",
                (f->error & 4) ? "user" : "kernel");
    }
    kprintf("  RIP=%016lx  CS=%04lx  RFLAGS=%016lx\n", f->rip, f->cs, f->rflags);
    kprintf("  RSP=%016lx  SS=%04lx  CR3=%016lx\n\n", f->rsp, f->ss, read_cr3());
    kprintf("  RAX=%016lx  RBX=%016lx  RCX=%016lx\n", f->rax, f->rbx, f->rcx);
    kprintf("  RDX=%016lx  RSI=%016lx  RDI=%016lx\n", f->rdx, f->rsi, f->rdi);
    kprintf("  RBP=%016lx  R8 =%016lx  R9 =%016lx\n", f->rbp, f->r8, f->r9);
    kprintf("  R10=%016lx  R11=%016lx  R12=%016lx\n", f->r10, f->r11, f->r12);
    kprintf("  R13=%016lx  R14=%016lx  R15=%016lx\n\n", f->r13, f->r14, f->r15);
    kprintf("  System halted. Restart the machine to continue.\n");
    cpu_halt_forever();
}

void isr_dispatch(interrupt_frame *f)
{
    uint64_t v = f->vector;

    if (v < 32) exception_panic(f);

    if (v >= IRQ_BASE && v < IRQ_BASE + 16) {
        int irq = (int)(v - IRQ_BASE);
        /* Spurious IRQ 7/15: the PIC's in-service bit is clear. */
        if (irq == 7 || irq == 15) {
            uint16_t cmd = irq == 7 ? PIC1_CMD : PIC2_CMD;
            outb(cmd, 0x0B);
            if (!(inb(cmd) & 0x80)) {
                if (irq == 15) outb(PIC1_CMD, PIC_EOI);
                return;
            }
        }
        if (irq_handlers[irq]) irq_handlers[irq](f);
        if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
        sched_irq_exit();           /* may switch threads; we resume here later */
        return;
    }

    /* Anything else came through the local APIC (0xFF = spurious, no EOI). */
    if (v != 0xFF && lapic) lapic[0xB0 / 4] = 0;
}
