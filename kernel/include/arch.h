/* arch.h - x86_64 CPU primitives, descriptor tables, interrupts, timer */
#pragma once
#include "types.h"

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

/* ---- port I/O ---------------------------------------------------------- */
static inline void outb(uint16_t p, uint8_t v)  { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p)  { uint8_t v;  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

/* ---- CPU control ------------------------------------------------------- */
static inline void cpu_cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void cpu_sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void cpu_hlt(void) { __asm__ volatile("hlt" ::: "memory"); }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- interrupts -------------------------------------------------------- */
/* Layout matches the push order in isr.asm. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
} interrupt_frame;

typedef void (*irq_handler_t)(interrupt_frame *frame);

void gdt_init(void);
void idt_init(void);
void irq_install(int irq, irq_handler_t handler);
void pic_unmask(int irq);
void pic_mask(int irq);

void     pit_init(uint32_t hz);
uint64_t timer_ticks(void);
uint32_t timer_hz(void);
void     timer_sleep_ms(uint64_t ms);

NORETURN void cpu_reboot(void);
NORETURN void cpu_halt_forever(void);
