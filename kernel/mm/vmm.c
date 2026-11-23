/*
 * vmm.c - 4-level page tables and per-process address spaces.
 *
 * The kernel half (PML4 slots 256-511: HHDM + kernel image) is created once
 * and every address space links to the same lower-level tables, so kernel
 * mappings look identical everywhere. The user half (slots 0-255) is private.
 * All page-table pages are reached through the HHDM.
 */
#include "mm.h"
#include "arch.h"
#include "lib.h"

#define PTE_P     0x001ULL
#define PTE_W     0x002ULL
#define PTE_U     0x004ULL
#define PTE_PS    0x080ULL
#define PTE_OWNED 0x200ULL                 /* software bit: frame belongs to this address space */
#define PTE_NX    (1ULL << 63)
#define PTE_ADDR  0x000FFFFFFFFFF000ULL

static uint64_t kernel_pml4;
static bool     nx_on, wp_on, smep_on;
static char     features[32];

static inline uint64_t *table(uint64_t phys) { return phys_to_virt(phys & PTE_ADDR); }

static uint64_t alloc_table(void)
{
    uint64_t p = pmm_alloc_page();
    if (p) memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

void vmm_init(void)
{
    uint32_t a, b, c, d;

    /* No-execute pages (EFER.NXE), if the CPU has them. */
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, 0, &a, &b, &c, &d);
        if (d & (1u << 20)) { wrmsr(MSR_EFER, rdmsr(MSR_EFER) | (1ULL << 11)); nx_on = true; }
    }
    /* Kernel honours read-only pages (CR0.WP); kernel may not execute user pages (CR4.SMEP). */
    write_cr0(read_cr0() | (1ULL << 16));
    wp_on = true;
    cpuid(0, 0, &a, &b, &c, &d);
    if (a >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        if (b & (1u << 7)) { write_cr4(read_cr4() | (1ULL << 20)); smep_on = true; }
    }
    ksnprintf(features, sizeof features, "%s%s%s", nx_on ? "NX " : "", wp_on ? "WP " : "", smep_on ? "SMEP" : "");

    /* New PML4: reuse the loader's kernel-half tables; give every empty kernel slot
     * a table now so later kernel mappings show up in all address spaces. */
    uint64_t *old = table(read_cr3());
    kernel_pml4 = alloc_table();
    if (!kernel_pml4) panic("vmm: out of memory");
    uint64_t *k = table(kernel_pml4);
    for (int i = 256; i < 512; i++) {
        if (old[i] & PTE_P) k[i] = old[i];
        else {
            uint64_t t = alloc_table();
            if (!t) panic("vmm: out of memory");
            k[i] = t | PTE_P | PTE_W;
        }
    }
    write_cr3(kernel_pml4);                 /* identity map (slot 0) is gone from here on */
}

uint64_t    vmm_kernel_pml4(void) { return kernel_pml4; }
const char *vmm_features(void)    { return features; }

address_space *vmm_create(void)
{
    address_space *as = kzalloc(sizeof *as);
    if (!as) return NULL;
    as->pml4 = alloc_table();
    if (!as->pml4) { kfree(as); return NULL; }
    uint64_t *n = table(as->pml4), *k = table(kernel_pml4);
    for (int i = 256; i < 512; i++) n[i] = k[i];
    return as;
}

void vmm_destroy(address_space *as)
{
    if (!as) return;
    if ((read_cr3() & PTE_ADDR) == as->pml4) write_cr3(kernel_pml4);
    uint64_t *pml4 = table(as->pml4);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_P)) continue;
        uint64_t *pdpt = table(pml4[i]);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_P)) continue;
            uint64_t *pd = table(pdpt[j]);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_P) || (pd[k] & PTE_PS)) continue;
                uint64_t *pt = table(pd[k]);
                for (int l = 0; l < 512; l++)
                    if ((pt[l] & PTE_P) && (pt[l] & PTE_OWNED)) pmm_free_page(pt[l] & PTE_ADDR);
                pmm_free_page(pd[k] & PTE_ADDR);
            }
            pmm_free_page(pdpt[j] & PTE_ADDR);
        }
        pmm_free_page(pml4[i] & PTE_ADDR);
    }
    pmm_free_page(as->pml4);
    kfree(as);
}

/* Walk to the PTE for virt; create missing tables if asked. */
static uint64_t *walk(address_space *as, uint64_t virt, bool create)
{
    uint64_t *t = table(as->pml4);
    for (int shift = 39; shift > 12; shift -= 9) {
        uint64_t *e = &t[(virt >> shift) & 511];
        if (!(*e & PTE_P)) {
            if (!create) return NULL;
            uint64_t n = alloc_table();
            if (!n) return NULL;
            *e = n | PTE_P | PTE_W | PTE_U;         /* permissions are enforced at the leaf */
        } else if (*e & PTE_PS) {
            return NULL;                            /* huge page: not used in the user half */
        }
        t = table(*e);
    }
    return &t[(virt >> 12) & 511];
}

static uint64_t leaf_flags(uint32_t flags)
{
    uint64_t f = PTE_P;
    if (flags & VMM_WRITE) f |= PTE_W;
    if (flags & VMM_USER)  f |= PTE_U;
    if (!(flags & VMM_EXEC) && nx_on) f |= PTE_NX;
    return f;
}

int vmm_map(address_space *as, uint64_t virt, uint64_t phys, uint32_t flags)
{
    if (virt >= USER_TOP || (virt & (PAGE_SIZE - 1))) return -1;
    uint64_t *pte = walk(as, virt, true);
    if (!pte) return -1;
    uint64_t owned = *pte & PTE_OWNED;
    *pte = (phys & PTE_ADDR) | leaf_flags(flags) | owned;
    if ((read_cr3() & PTE_ADDR) == as->pml4) invlpg(virt);
    return 0;
}

int vmm_alloc(address_space *as, uint64_t virt, uint32_t flags)
{
    uint64_t phys = alloc_table();               /* a zeroed page */
    if (!phys) return -1;
    if (vmm_map(as, virt, phys, flags)) { pmm_free_page(phys); return -1; }
    *walk(as, virt, false) |= PTE_OWNED;
    return 0;
}

uint64_t vmm_translate(address_space *as, uint64_t virt, uint64_t *pte_out)
{
    uint64_t *pte = walk(as, virt & ~(PAGE_SIZE - 1), false);
    if (!pte || !(*pte & PTE_P)) return 0;
    if (pte_out) *pte_out = *pte;
    return (*pte & PTE_ADDR) | (virt & (PAGE_SIZE - 1));
}

bool vmm_user_ok(address_space *as, uint64_t virt, uint64_t len, bool write)
{
    if (len == 0) return true;
    if (virt >= USER_TOP || len > USER_TOP - virt) return false;
    for (uint64_t p = ALIGN_DOWN(virt, PAGE_SIZE); p < virt + len; p += PAGE_SIZE) {
        uint64_t pte;
        if (!vmm_translate(as, p, &pte)) return false;
        if (!(pte & PTE_U) || (write && !(pte & PTE_W))) return false;
    }
    return true;
}

int vmm_write(address_space *as, uint64_t virt, const void *src, uint64_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint64_t phys = vmm_translate(as, virt, NULL);
        if (!phys) return -1;
        uint64_t chunk = MIN(len, PAGE_SIZE - (virt & (PAGE_SIZE - 1)));
        memcpy(phys_to_virt(phys), s, chunk);
        virt += chunk; s += chunk; len -= chunk;
    }
    return 0;
}

uint64_t vmm_user_pages(address_space *as)
{
    uint64_t n = 0;
    uint64_t *pml4 = table(as->pml4);
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_P)) continue;
        uint64_t *pdpt = table(pml4[i]);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_P)) continue;
            uint64_t *pd = table(pdpt[j]);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_P) || (pd[k] & PTE_PS)) continue;
                uint64_t *pt = table(pd[k]);
                for (int l = 0; l < 512; l++) if (pt[l] & PTE_P) n++;
            }
        }
    }
    return n;
}
