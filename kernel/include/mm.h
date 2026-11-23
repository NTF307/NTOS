/* mm.h - physical page allocator and kernel heap */
#pragma once
#include "types.h"
#include "../../include/bootinfo.h"

extern uint64_t g_hhdm;
static inline void *phys_to_virt(uint64_t p) { return (void *)(p + g_hhdm); }

void     pmm_init(const ntos_boot_info *bi);
uint64_t pmm_alloc_page(void);               /* returns physical address, 0 on failure */
uint64_t pmm_alloc_pages(size_t count);      /* physically contiguous */
void     pmm_free_page(uint64_t phys);
void     pmm_free_pages(uint64_t phys, size_t count);
uint64_t pmm_total_bytes(void);              /* usable RAM managed by the allocator */
uint64_t pmm_free_bytes(void);

void   heap_init(void);
void  *kmalloc(size_t size);
void  *kzalloc(size_t size);
void   kfree(void *ptr);
void   heap_stats(size_t *total, size_t *used, size_t *blocks);
