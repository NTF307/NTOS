/* pit.c - 8254 Programmable Interval Timer (IRQ 0), reboot and halt helpers */
#include "arch.h"

#define PIT_FREQ 1193182U

static volatile uint64_t ticks;
static uint32_t hz = 100;

static void pit_irq(interrupt_frame *f) { (void)f; ticks++; }

void pit_init(uint32_t freq)
{
    hz = freq;
    uint32_t div = PIT_FREQ / freq;
    outb(0x43, 0x36);                 /* channel 0, lo/hi byte, mode 3 (square wave) */
    outb(0x40, div & 0xFF);
    outb(0x40, (div >> 8) & 0xFF);
    irq_install(0, pit_irq);
    pic_unmask(0);
}

uint64_t timer_ticks(void) { return ticks; }
uint32_t timer_hz(void)    { return hz; }

void timer_sleep_ms(uint64_t ms)
{
    uint64_t wait = (ms * hz + 999) / 1000;
    uint64_t target = ticks + (wait ? wait : 1);
    while (ticks < target) cpu_hlt();
}

void cpu_reboot(void)
{
    cpu_cli();
    outb(0xCF9, 0x02); io_wait(); outb(0xCF9, 0x06);       /* PCI reset control register */
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) ;
    outb(0x64, 0xFE);                                       /* 8042 CPU reset line */
    struct PACKED { uint16_t limit; uint64_t base; } null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int3" :: "m"(null_idt));    /* triple fault */
    for (;;) cpu_hlt();
}

void cpu_halt_forever(void)
{
    for (;;) { cpu_cli(); cpu_hlt(); }
}
