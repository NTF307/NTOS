/*
 * bootinfo.h - contract between the NTOS UEFI loader and the kernel.
 *
 * The loader fills an ntos_boot_info, maps it in the higher-half direct map
 * (HHDM) and passes its virtual address to the kernel entry point in RDI.
 *
 * Virtual memory layout set up by the loader:
 *   0x0000000000000000 .. limit      identity map of physical memory (temporary)
 *   0xFFFF800000000000 .. +limit     HHDM: all physical memory, 2 MiB pages
 *   0xFFFFFFFF80000000 ..            kernel image (linked here), 4 KiB pages
 */
#pragma once
#include <stdint.h>

#define NTOS_BOOT_MAGIC       0x544F4F42534F544EULL /* "NTOSBOOT" little-endian */
#define NTOS_KERNEL_VBASE     0xFFFFFFFF80000000ULL
#define NTOS_HHDM_BASE        0xFFFF800000000000ULL
#define NTOS_MAX_MEM_REGIONS  512

enum ntos_mem_type {
    NTOS_MEM_USABLE = 1,       /* free RAM */
    NTOS_MEM_RESERVED,         /* firmware / MMIO / unknown */
    NTOS_MEM_ACPI_RECLAIM,     /* ACPI tables, reusable after parsing */
    NTOS_MEM_ACPI_NVS,         /* ACPI non-volatile storage */
    NTOS_MEM_BOOTLOADER,       /* loader data: kernel image, page tables, boot info */
    NTOS_MEM_BAD,              /* defective RAM */
};

enum ntos_fb_format {
    NTOS_FB_BGRX = 0,          /* bytes B,G,R,X  -> 0x00RRGGBB as uint32 */
    NTOS_FB_RGBX = 1,          /* bytes R,G,B,X  -> 0x00BBGGRR as uint32 */
};

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;             /* enum ntos_mem_type */
    uint32_t reserved;
} ntos_mem_region;

typedef struct {
    uint64_t base;             /* physical address */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;            /* bytes per scanline */
    uint32_t bpp;              /* always 32 */
    uint32_t format;           /* enum ntos_fb_format */
    uint32_t reserved;
} ntos_framebuffer;

typedef struct {
    uint64_t         magic;
    ntos_framebuffer fb;
    uint64_t         kernel_phys;   /* physical base of the kernel image */
    uint64_t         kernel_size;   /* bytes, page aligned */
    uint64_t         hhdm_base;     /* virtual = physical + hhdm_base */
    uint64_t         rsdp;          /* physical address of ACPI RSDP, 0 if none */
    uint64_t         initrd_phys;   /* \NTOS\INITRD.TAR (ustar archive), 0 if absent */
    uint64_t         initrd_size;   /* bytes */
    uint64_t         mem_count;
    ntos_mem_region  mem[NTOS_MAX_MEM_REGIONS];
} ntos_boot_info;
