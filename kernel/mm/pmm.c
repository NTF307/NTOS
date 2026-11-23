/* pmm.c - bitmap physical page allocator (1 bit per 4 KiB page, 1 = used) */
#include "mm.h"
#include "lib.h"
#include "arch.h"

uint64_t g_hhdm;

static uint8_t *bitmap;
static uint64_t bitmap_pages;     /* number of pages tracked */
static uint64_t total_pages, free_pages, hint;

static inline void set_used(uint64_t p) { bitmap[p / 8] |= (uint8_t)(1 << (p % 8)); }
static inline void set_free(uint64_t p) { bitmap[p / 8] &= (uint8_t)~(1 << (p % 8)); }
static inline bool is_used(uint64_t p)  { return bitmap[p / 8] & (1 << (p % 8)); }

void pmm_init(const ntos_boot_info *bi)
{
    uint64_t top = 0;
    for (uint64_t i = 0; i < bi->mem_count; i++) {
        const ntos_mem_region *r = &bi->mem[i];
        if (r->type == NTOS_MEM_USABLE && r->base + r->length > top) top = r->base + r->length;
    }
    bitmap_pages = top / PAGE_SIZE;
    uint64_t bitmap_bytes = ALIGN_UP((bitmap_pages + 7) / 8, PAGE_SIZE);

    /* Place the bitmap in the first usable region above 1 MiB that fits it. */
    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < bi->mem_count; i++) {
        const ntos_mem_region *r = &bi->mem[i];
        uint64_t base = ALIGN_UP(MAX(r->base, 0x100000ULL), PAGE_SIZE);
        if (r->type != NTOS_MEM_USABLE || base >= r->base + r->length) continue;
        if (r->base + r->length - base >= bitmap_bytes) { bitmap_phys = base; break; }
    }
    if (!bitmap_phys) panic("pmm: no room for a %lu byte page bitmap", bitmap_bytes);

    bitmap = phys_to_virt(bitmap_phys);
    memset(bitmap, 0xFF, bitmap_bytes);

    for (uint64_t i = 0; i < bi->mem_count; i++) {
        const ntos_mem_region *r = &bi->mem[i];
        if (r->type != NTOS_MEM_USABLE) continue;
        uint64_t start = ALIGN_UP(r->base, PAGE_SIZE) / PAGE_SIZE;
        uint64_t end = ALIGN_DOWN(r->base + r->length, PAGE_SIZE) / PAGE_SIZE;
        for (uint64_t p = start; p < end; p++) { set_free(p); free_pages++; total_pages++; }
    }

    /* Keep the first 1 MiB (real-mode area, future SMP trampoline) and the bitmap itself. */
    for (uint64_t p = 0; p < 256 && p < bitmap_pages; p++)
        if (!is_used(p)) { set_used(p); free_pages--; }
    for (uint64_t p = bitmap_phys / PAGE_SIZE; p < (bitmap_phys + bitmap_bytes) / PAGE_SIZE; p++)
        if (!is_used(p)) { set_used(p); free_pages--; }
    hint = 256;
}

static uint64_t alloc_page_locked(void)
{
    for (uint64_t n = 0; n < bitmap_pages; n++) {
        uint64_t p = (hint + n) % bitmap_pages;
        if (bitmap[p / 8] == 0xFF) { n += 7 - (p % 8); continue; }   /* skip full bytes */
        if (!is_used(p)) {
            set_used(p);
            free_pages--;
            hint = p + 1;
            return p * PAGE_SIZE;
        }
    }
    return 0;
}

static uint64_t alloc_pages_locked(size_t count)
{
    if (count == 1) return alloc_page_locked();
    uint64_t run = 0;
    for (uint64_t p = 256; p < bitmap_pages; p++) {
        if (is_used(p)) { run = 0; continue; }
        if (++run == count) {
            uint64_t first = p + 1 - count;
            for (uint64_t q = first; q <= p; q++) set_used(q);
            free_pages -= count;
            return first * PAGE_SIZE;
        }
    }
    return 0;
}

static void free_page_locked(uint64_t phys)
{
    uint64_t p = phys / PAGE_SIZE;
    if (p >= bitmap_pages || !is_used(p)) panic("pmm: bad free of 0x%lx", phys);
    set_free(p);
    free_pages++;
    if (p < hint) hint = p;
}

/* Public entry points are interrupt-safe (single CPU: masking IRQs is the lock). */
uint64_t pmm_alloc_page(void)
{
    uint64_t f = irq_save();
    uint64_t p = alloc_page_locked();
    irq_restore(f);
    return p;
}

uint64_t pmm_alloc_pages(size_t count)
{
    if (count == 0) return 0;
    uint64_t f = irq_save();
    uint64_t p = alloc_pages_locked(count);
    irq_restore(f);
    return p;
}

void pmm_free_page(uint64_t phys)
{
    uint64_t f = irq_save();
    free_page_locked(phys);
    irq_restore(f);
}

void pmm_free_pages(uint64_t phys, size_t count)
{
    uint64_t f = irq_save();
    for (size_t i = 0; i < count; i++) free_page_locked(phys + i * PAGE_SIZE);
    irq_restore(f);
}

uint64_t pmm_total_bytes(void) { return total_pages * PAGE_SIZE; }
uint64_t pmm_free_bytes(void)  { return free_pages * PAGE_SIZE; }
