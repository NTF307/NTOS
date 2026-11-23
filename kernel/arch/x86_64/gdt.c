/* gdt.c - Global Descriptor Table and Task State Segment */
#include "arch.h"
#include "lib.h"

typedef struct PACKED {
    uint32_t reserved0;
    uint64_t rsp[3];           /* stacks used when entering ring 0 from ring 1-3 */
    uint64_t reserved1;
    uint64_t ist[7];           /* interrupt stack table */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss_t;

static tss_t    tss;
static uint64_t gdt[7];
static uint8_t  df_stack[16384] ALIGNED(16);      /* IST1: double fault */
static uint8_t  ring0_stack[16384] ALIGNED(16);   /* RSP0 until the first user thread runs */

static struct PACKED { uint16_t limit; uint64_t base; } gdtr;

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL;   /* 0x08 kernel code, 64-bit */
    gdt[2] = 0x00CF92000000FFFFULL;   /* 0x10 kernel data */
    gdt[3] = 0x00CFF2000000FFFFULL;   /* 0x18 user data (DPL 3) */
    gdt[4] = 0x00AFFA000000FFFFULL;   /* 0x20 user code (DPL 3) */

    memset(&tss, 0, sizeof tss);
    tss.ist[0] = (uint64_t)(df_stack + sizeof df_stack);
    tss.rsp[0] = (uint64_t)(ring0_stack + sizeof ring0_stack);
    tss.iomap_base = sizeof tss;

    uint64_t base = (uint64_t)&tss, limit = sizeof tss - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    gdtr.limit = sizeof gdt - 1;
    gdtr.base = (uint64_t)gdt;

    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"                    /* reload CS */
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "xor %%eax, %%eax\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        :: "m"(gdtr) : "rax", "memory");
    __asm__ volatile("ltr %w0" :: "r"((uint16_t)GDT_TSS));
}

extern uint64_t syscall_kernel_rsp;     /* syscall.asm: stack the syscall entry switches to */

void gdt_set_kernel_stack(uint64_t top)
{
    tss.rsp[0] = top;
    syscall_kernel_rsp = top;
}
