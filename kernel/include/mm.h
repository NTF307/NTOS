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

/* ---- vmm.c: page tables and address spaces ---------------------------- */
#define USER_TOP          0x0000800000000000ULL   /* user half: [0, USER_TOP) */
#define VMM_WRITE         0x01
#define VMM_USER          0x02
#define VMM_EXEC          0x04

typedef struct address_space { uint64_t pml4; } address_space;   /* physical address of PML4 */

void           vmm_init(void);                 /* own kernel page tables; drops the identity map */
uint64_t       vmm_kernel_pml4(void);
const char    *vmm_features(void);             /* "NX WP SMEP" etc. */
address_space *vmm_create(void);               /* empty user half, shared kernel half */
void           vmm_destroy(address_space *as); /* frees user pages it owns and all user page tables */
int            vmm_map(address_space *as, uint64_t virt, uint64_t phys, uint32_t flags);
int            vmm_alloc(address_space *as, uint64_t virt, uint32_t flags);   /* new zeroed, owned frame */
uint64_t       vmm_translate(address_space *as, uint64_t virt, uint64_t *pte_out);
bool           vmm_user_ok(address_space *as, uint64_t virt, uint64_t len, bool write);
int            vmm_write(address_space *as, uint64_t virt, const void *src, uint64_t len); /* via HHDM */
uint64_t       vmm_user_pages(address_space *as);

void   heap_init(void);
void  *kmalloc(size_t size);
void  *kzalloc(size_t size);
void   kfree(void *ptr);
void   heap_stats(size_t *total, size_t *used, size_t *blocks);
