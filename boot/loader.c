/*
 * loader.c - NTOS UEFI boot loader (BOOTX64.EFI)
 *
 *  1. pick a graphics mode (GOP) and record the framebuffer
 *  2. load \NTOS\KERNEL.ELF from the boot volume and place its PT_LOAD segments
 *  3. build 4-level page tables: identity + HHDM (2 MiB pages) + kernel (4 KiB)
 *  4. ExitBootServices, convert the memory map, switch CR3, jump to the kernel
 */
#include "efi.h"
#include "../include/bootinfo.h"
#include "version.h"

#define PAGE_SIZE    4096ULL
#define KERNEL_PATH  u"\\NTOS\\KERNEL.ELF"
#define MAX_FB_W     1920
#define MAX_FB_H     1080
#define STACK_PAGES  16

static EFI_SYSTEM_TABLE  *ST;
static EFI_BOOT_SERVICES *BS;

/* The compiler may emit calls to these even in freestanding code. */
void *memset(void *d, int c, size_t n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }

/* ---------------------------------------------------------------- output */
static void print(const char *s)
{
    CHAR16 buf[128];
    size_t i = 0;
    while (*s) {
        if (i >= 124) { buf[i] = 0; ST->ConOut->OutputString(ST->ConOut, buf); i = 0; }
        if (*s == '\n') buf[i++] = u'\r';
        buf[i++] = (CHAR16)(uint8_t)*s++;
    }
    buf[i] = 0;
    ST->ConOut->OutputString(ST->ConOut, buf);
}

static void print_hex(uint64_t v)
{
    char b[19] = "0x";
    for (int i = 0; i < 16; i++) b[2 + i] = "0123456789ABCDEF"[(v >> (60 - 4 * i)) & 0xF];
    b[18] = 0;
    print(b);
}

static void print_dec(uint64_t v)
{
    char b[21]; int i = 20; b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    print(&b[i]);
}

static EFI_STATUS fatal(const char *msg, EFI_STATUS st)
{
    ST->ConOut->SetAttribute(ST->ConOut, 0x0C); /* light red */
    print("\n[NTOS] FATAL: "); print(msg); print(" (status "); print_hex(st); print(")\n");
    ST->ConOut->SetAttribute(ST->ConOut, 0x07);
    print("Press any key to return to the firmware...\n");
    UINTN idx;
    ST->ConIn->Reset(ST->ConIn, 0);
    BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &idx);
    return EFI_ERROR(st) ? st : EFI_LOAD_ERROR;
}

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

/* -------------------------------------------------------------- graphics */
static int usable_format(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info, uint32_t *fmt)
{
    switch (info->PixelFormat) {
    case PixelBlueGreenRedReserved8BitPerColor: *fmt = NTOS_FB_BGRX; return 1;
    case PixelRedGreenBlueReserved8BitPerColor: *fmt = NTOS_FB_RGBX; return 1;
    case PixelBitMask:
        if (info->PixelInformation.RedMask == 0xFF0000 && info->PixelInformation.GreenMask == 0xFF00 &&
            info->PixelInformation.BlueMask == 0xFF) { *fmt = NTOS_FB_BGRX; return 1; }
        if (info->PixelInformation.RedMask == 0xFF && info->PixelInformation.GreenMask == 0xFF00 &&
            info->PixelInformation.BlueMask == 0xFF0000) { *fmt = NTOS_FB_RGBX; return 1; }
        return 0;
    default: return 0;
    }
}

static EFI_STATUS setup_graphics(ntos_framebuffer *out)
{
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
    EFI_STATUS st = BS->LocateProtocol(&guid, NULL, (void **)&gop);
    if (EFI_ERROR(st) || !gop) return EFI_ERROR(st) ? st : EFI_NOT_FOUND;

    uint32_t best = gop->Mode->Mode, fmt;
    uint64_t best_area = 0;
    for (uint32_t m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info; UINTN sz;
        if (EFI_ERROR(gop->QueryMode(gop, m, &sz, &info))) continue;
        if (!usable_format(info, &fmt)) continue;
        if (info->HorizontalResolution > MAX_FB_W || info->VerticalResolution > MAX_FB_H) continue;
        uint64_t area = (uint64_t)info->HorizontalResolution * info->VerticalResolution;
        if (area > best_area) { best_area = area; best = m; }
    }
    if (best != gop->Mode->Mode) gop->SetMode(gop, best); /* on failure we keep the current mode */

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
    if (!usable_format(info, &fmt)) return EFI_UNSUPPORTED;
    out->base   = gop->Mode->FrameBufferBase;
    out->width  = info->HorizontalResolution;
    out->height = info->VerticalResolution;
    out->pitch  = info->PixelsPerScanLine * 4;
    out->bpp    = 32;
    out->format = fmt;
    return EFI_SUCCESS;
}

/* ------------------------------------------------------------ file load */
static EFI_STATUS load_file(EFI_HANDLE image, CHAR16 *path, void **data, UINTN *size)
{
    EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID, sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *lip;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *sfs;
    EFI_FILE_PROTOCOL *root, *file;
    EFI_STATUS st;

    if (EFI_ERROR(st = BS->HandleProtocol(image, &lip_guid, (void **)&lip))) return st;
    if (EFI_ERROR(st = BS->HandleProtocol(lip->DeviceHandle, &sfs_guid, (void **)&sfs))) return st;
    if (EFI_ERROR(st = sfs->OpenVolume(sfs, &root))) return st;
    if (EFI_ERROR(st = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0))) { root->Close(root); return st; }

    uint64_t len = 0;
    file->SetPosition(file, 0xFFFFFFFFFFFFFFFFULL);   /* seek to end */
    file->GetPosition(file, &len);
    file->SetPosition(file, 0);

    void *buf = NULL;
    if (EFI_ERROR(st = BS->AllocatePool(EfiLoaderData, len ? len : 1, &buf))) goto out;
    UINTN done = 0;
    while (done < len) {
        UINTN chunk = len - done;
        if (EFI_ERROR(st = file->Read(file, &chunk, (uint8_t *)buf + done))) break;
        if (chunk == 0) { st = EFI_LOAD_ERROR; break; }
        done += chunk;
    }
    if (EFI_ERROR(st)) { BS->FreePool(buf); goto out; }
    *data = buf; *size = len;
out:
    file->Close(file);
    root->Close(root);
    return st;
}

/* ------------------------------------------------------------------ ELF */
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

typedef struct { uint64_t entry, vbase, phys, size; } kernel_image;

static EFI_STATUS load_elf(const uint8_t *file, UINTN size, kernel_image *k)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
    if (size < sizeof(*eh) || eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
        eh->e_ident[3] != 'F' || eh->e_ident[4] != 2 || eh->e_ident[5] != 1 || eh->e_machine != 0x3E ||
        eh->e_type != 2 || eh->e_phentsize != sizeof(Elf64_Phdr) ||
        eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(Elf64_Phdr) > size)
        return EFI_UNSUPPORTED;

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(file + eh->e_phoff);
    uint64_t vmin = ~0ULL, vmax = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 1 || ph[i].p_memsz == 0) continue;
        if (ph[i].p_offset + ph[i].p_filesz > size || ph[i].p_filesz > ph[i].p_memsz) return EFI_UNSUPPORTED;
        if (ph[i].p_vaddr < vmin) vmin = ph[i].p_vaddr;
        if (ph[i].p_vaddr + ph[i].p_memsz > vmax) vmax = ph[i].p_vaddr + ph[i].p_memsz;
    }
    if (vmax == 0 || vmin < NTOS_KERNEL_VBASE) return EFI_UNSUPPORTED;
    vmin &= ~(PAGE_SIZE - 1);
    vmax = (vmax + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    UINTN pages = (vmax - vmin) / PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS phys;
    EFI_STATUS st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &phys);
    if (EFI_ERROR(st)) return st;
    memset((void *)phys, 0, pages * PAGE_SIZE);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != 1 || ph[i].p_memsz == 0) continue;
        memcpy((uint8_t *)phys + (ph[i].p_vaddr - vmin), file + ph[i].p_offset, ph[i].p_filesz);
    }
    k->entry = eh->e_entry; k->vbase = vmin; k->phys = phys; k->size = pages * PAGE_SIZE;
    return EFI_SUCCESS;
}

/* ---------------------------------------------------------- page tables */
#define PTE_P   0x001ULL
#define PTE_W   0x002ULL
#define PTE_PS  0x080ULL
#define PTE_ADDR 0x000FFFFFFFFFF000ULL

static uint64_t *pt_pool;
static UINTN pt_pool_pages, pt_pool_used;

static uint64_t *pt_alloc(void)
{
    if (pt_pool_used >= pt_pool_pages) return NULL;
    uint64_t *t = pt_pool + pt_pool_used++ * 512;
    memset(t, 0, PAGE_SIZE);
    return t;
}

static uint64_t *pt_next(uint64_t *table, UINTN idx)
{
    if (!(table[idx] & PTE_P)) {
        uint64_t *t = pt_alloc();
        if (!t) return NULL;
        table[idx] = (uint64_t)t | PTE_P | PTE_W;
    }
    return (uint64_t *)(table[idx] & PTE_ADDR);
}

static int map(uint64_t *pml4, uint64_t virt, uint64_t phys, int huge)
{
    uint64_t *pdpt = pt_next(pml4, (virt >> 39) & 511);  if (!pdpt) return 0;
    uint64_t *pd   = pt_next(pdpt, (virt >> 30) & 511);  if (!pd) return 0;
    if (huge) { pd[(virt >> 21) & 511] = phys | PTE_P | PTE_W | PTE_PS; return 1; }
    uint64_t *pt   = pt_next(pd, (virt >> 21) & 511);    if (!pt) return 0;
    pt[(virt >> 12) & 511] = phys | PTE_P | PTE_W;
    return 1;
}

/* Returns the highest physical address of RAM/firmware memory. MMIO windows are skipped:
 * firmware often reports huge 64-bit PCI apertures; the caller always maps the low 4 GiB. */
static EFI_STATUS scan_memory(uint64_t *max_phys)
{
    UINTN size = 0, key, dsize; uint32_t dver;
    EFI_STATUS st = BS->GetMemoryMap(&size, NULL, &key, &dsize, &dver);
    if (st != EFI_BUFFER_TOO_SMALL) return st;
    size += 8 * dsize;
    void *buf;
    if (EFI_ERROR(st = BS->AllocatePool(EfiLoaderData, size, &buf))) return st;
    if (EFI_ERROR(st = BS->GetMemoryMap(&size, buf, &key, &dsize, &dver))) { BS->FreePool(buf); return st; }
    uint64_t max = 0;
    for (UINTN off = 0; off < size; off += dsize) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)buf + off);
        if (d->Type == EfiMemoryMappedIO || d->Type == EfiMemoryMappedIOPortSpace ||
            d->Type == EfiReservedMemoryType || d->Type == EfiUnusableMemory) continue;
        uint64_t end = d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
        if (end > max) max = end;
    }
    BS->FreePool(buf);
    *max_phys = max;
    return EFI_SUCCESS;
}

static uint32_t convert_type(uint32_t t)
{
    switch (t) {
    case EfiConventionalMemory: case EfiBootServicesCode: case EfiBootServicesData: return NTOS_MEM_USABLE;
    case EfiLoaderCode: case EfiLoaderData:   return NTOS_MEM_BOOTLOADER;
    case EfiACPIReclaimMemory:                return NTOS_MEM_ACPI_RECLAIM;
    case EfiACPIMemoryNVS:                    return NTOS_MEM_ACPI_NVS;
    case EfiUnusableMemory:                   return NTOS_MEM_BAD;
    default:                                  return NTOS_MEM_RESERVED;
    }
}

/* ----------------------------------------------------------------- main */
EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    EFI_STATUS st;
    ST = systab;
    BS = systab->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, NULL);

    ntos_framebuffer fb;
    if (EFI_ERROR(st = setup_graphics(&fb))) return fatal("no usable graphics mode (GOP)", st);

    ST->ConOut->ClearScreen(ST->ConOut);
    print("NTOS Boot Loader " NTOS_VERSION " (build " NTOS_BUILD_STR ")\n");
    print("Framebuffer: "); print_dec(fb.width); print("x"); print_dec(fb.height);
    print(" at "); print_hex(fb.base); print("\n");

    /* ---- kernel */
    void *kfile; UINTN ksize;
    if (EFI_ERROR(st = load_file(image, KERNEL_PATH, &kfile, &ksize)))
        return fatal("cannot read \\NTOS\\KERNEL.ELF", st);
    kernel_image k;
    if (EFI_ERROR(st = load_elf(kfile, ksize, &k))) return fatal("KERNEL.ELF is not a valid NTOS kernel", st);
    BS->FreePool(kfile);
    print("Kernel: "); print_dec(k.size / 1024); print(" KiB at phys "); print_hex(k.phys);
    print(", entry "); print_hex(k.entry); print("\n");

    /* ---- boot info + stack */
    EFI_PHYSICAL_ADDRESS bi_phys, stack_phys;
    UINTN bi_pages = (sizeof(ntos_boot_info) + PAGE_SIZE - 1) / PAGE_SIZE;
    if (EFI_ERROR(st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, bi_pages, &bi_phys)))
        return fatal("out of memory (boot info)", st);
    if (EFI_ERROR(st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, STACK_PAGES, &stack_phys)))
        return fatal("out of memory (stack)", st);
    ntos_boot_info *bi = (ntos_boot_info *)bi_phys;
    memset(bi, 0, bi_pages * PAGE_SIZE);
    bi->magic = NTOS_BOOT_MAGIC;
    bi->fb = fb;
    bi->kernel_phys = k.phys;
    bi->kernel_size = k.size;
    bi->hhdm_base = NTOS_HHDM_BASE;

    EFI_GUID acpi2 = EFI_ACPI_20_TABLE_GUID, acpi1 = EFI_ACPI_10_TABLE_GUID;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
        if (guid_eq(&t->VendorGuid, &acpi2)) { bi->rsdp = (uint64_t)t->VendorTable; break; }
        if (guid_eq(&t->VendorGuid, &acpi1)) bi->rsdp = (uint64_t)t->VendorTable;
    }

    /* ---- page tables */
    uint64_t max_phys;
    if (EFI_ERROR(st = scan_memory(&max_phys))) return fatal("cannot read memory map", st);
    uint64_t fb_end = fb.base + (uint64_t)fb.pitch * fb.height;
    if (fb_end > max_phys) max_phys = fb_end;
    if (max_phys < 0x100000000ULL) max_phys = 0x100000000ULL;          /* always cover 4 GiB (LAPIC, IOAPIC) */
    uint64_t limit = (max_phys + 0x3FFFFFFFULL) & ~0x3FFFFFFFULL;      /* round up to 1 GiB */
    if (limit > 0x8000000000ULL) limit = 0x8000000000ULL;             /* one PML4 slot: 512 GiB */

    UINTN gibs = limit >> 30;
    UINTN kpages = k.size / PAGE_SIZE;
    pt_pool_pages = 1 + 2 * (1 + gibs) + 2 + (kpages + 511) / 512 + 1 + 8;
    EFI_PHYSICAL_ADDRESS pool_phys;
    if (EFI_ERROR(st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, pt_pool_pages, &pool_phys)))
        return fatal("out of memory (page tables)", st);
    pt_pool = (uint64_t *)pool_phys;
    pt_pool_used = 0;

    uint64_t *pml4 = pt_alloc();
    for (uint64_t a = 0; a < limit; a += 0x200000ULL) {
        if (!map(pml4, a, a, 1) || !map(pml4, NTOS_HHDM_BASE + a, a, 1))
            return fatal("page table pool exhausted", EFI_OUT_OF_RESOURCES);
    }
    for (UINTN i = 0; i < kpages; i++) {
        if (!map(pml4, k.vbase + i * PAGE_SIZE, k.phys + i * PAGE_SIZE, 0))
            return fatal("page table pool exhausted", EFI_OUT_OF_RESOURCES);
    }

    print("Memory: mapped "); print_dec(limit >> 30); print(" GiB, starting kernel...\n");

    /* ---- exit boot services (no console output allowed after this) */
    UINTN map_size = 0, map_key, dsize; uint32_t dver;
    EFI_MEMORY_DESCRIPTOR *mmap = NULL;
    BS->GetMemoryMap(&map_size, NULL, &map_key, &dsize, &dver);
    UINTN map_cap = map_size + 32 * dsize;
    if (EFI_ERROR(st = BS->AllocatePool(EfiLoaderData, map_cap, (void **)&mmap)))
        return fatal("out of memory (memory map)", st);

    for (int attempt = 0;; attempt++) {
        map_size = map_cap;
        st = BS->GetMemoryMap(&map_size, mmap, &map_key, &dsize, &dver);
        if (st == EFI_BUFFER_TOO_SMALL && attempt < 4) {
            BS->FreePool(mmap);
            map_cap = map_size + 32 * dsize;
            if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, map_cap, (void **)&mmap)))
                return fatal("out of memory (memory map)", st);
            continue;
        }
        if (EFI_ERROR(st)) return fatal("GetMemoryMap failed", st);
        st = BS->ExitBootServices(image, map_key);
        if (!EFI_ERROR(st)) break;
        if (attempt >= 4) return fatal("ExitBootServices failed", st);
    }
    __asm__ volatile("cli");

    /* ---- convert memory map (merge adjacent regions of the same type) */
    uint64_t n = 0;
    for (UINTN off = 0; off < map_size; off += dsize) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)mmap + off);
        uint64_t base = d->PhysicalStart, len = d->NumberOfPages * PAGE_SIZE;
        uint32_t type = convert_type(d->Type);
        if (len == 0) continue;
        if (n > 0 && bi->mem[n - 1].type == type && bi->mem[n - 1].base + bi->mem[n - 1].length == base) {
            bi->mem[n - 1].length += len;
            continue;
        }
        if (n >= NTOS_MAX_MEM_REGIONS) break;
        bi->mem[n].base = base; bi->mem[n].length = len; bi->mem[n].type = type;
        n++;
    }
    bi->mem_count = n;

    /* ---- switch to our page tables and enter the kernel */
    uint64_t stack_top = NTOS_HHDM_BASE + stack_phys + STACK_PAGES * PAGE_SIZE;
    uint64_t bi_virt = NTOS_HHDM_BASE + bi_phys;
    __asm__ volatile(
        "mov %0, %%cr3\n\t"
        "mov %1, %%rsp\n\t"
        "xor %%rbp, %%rbp\n\t"
        "jmp *%2\n\t"
        :: "a"((uint64_t)pml4), "d"(stack_top), "c"(k.entry), "D"(bi_virt)
        : "memory");
    __builtin_unreachable();
}
