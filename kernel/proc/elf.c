/* elf.c - load a static ELF64 executable into a user address space */
#include "proc.h"
#include "lib.h"

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

#define PT_LOAD 1
#define PF_X    1
#define PF_W    2

/* Returns 0, or -E_NOEXEC (bad file) / -E_NOMEM. *end = first page after the image (heap start). */
int elf_load(address_space *as, const uint8_t *data, uint64_t size, uint64_t *entry, uint64_t *end)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;
    if (size < sizeof *eh || memcmp(eh->e_ident, "\x7F" "ELF", 4) != 0 || eh->e_ident[4] != 2 ||
        eh->e_ident[5] != 1 || eh->e_machine != 0x3E || eh->e_type != 2 ||
        eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phoff > size ||
        (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > size - eh->e_phoff)
        return -E_NOEXEC;

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff);
    uint64_t top = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *p = &ph[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;
        if (p->p_filesz > p->p_memsz || p->p_offset > size || p->p_filesz > size - p->p_offset ||
            p->p_vaddr < 0x1000 || p->p_vaddr >= USER_TOP || p->p_memsz > USER_TOP - p->p_vaddr)
            return -E_NOEXEC;

        uint32_t flags = VMM_USER | ((p->p_flags & PF_W) ? VMM_WRITE : 0) | ((p->p_flags & PF_X) ? VMM_EXEC : 0);
        for (uint64_t va = ALIGN_DOWN(p->p_vaddr, PAGE_SIZE); va < p->p_vaddr + p->p_memsz; va += PAGE_SIZE) {
            uint64_t pte;
            uint64_t phys = vmm_translate(as, va, &pte);
            if (phys) {
                /* Page shared with the previous segment: keep it, widen permissions. */
                if (pte & 0x2) flags |= VMM_WRITE;
                if (!(pte & (1ULL << 63))) flags |= VMM_EXEC;
                if (vmm_map(as, va, phys & ~(PAGE_SIZE - 1), flags)) return -E_NOMEM;
            } else if (vmm_alloc(as, va, flags)) {
                return -E_NOMEM;
            }
        }
        if (p->p_filesz && vmm_write(as, p->p_vaddr, data + p->p_offset, p->p_filesz)) return -E_NOMEM;
        if (p->p_vaddr + p->p_memsz > top) top = p->p_vaddr + p->p_memsz;
    }
    if (!top || eh->e_entry < 0x1000 || eh->e_entry >= USER_TOP) return -E_NOEXEC;
    *entry = eh->e_entry;
    *end = ALIGN_UP(top, PAGE_SIZE);
    return 0;
}
