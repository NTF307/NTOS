/* initrd.c - read-only ustar archive loaded by the boot loader (\NTOS\INITRD.TAR) */
#include "proc.h"
#include "lib.h"

static const uint8_t *base;
static uint64_t       total;

static uint64_t octal(const char *s, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (uint64_t)(s[i] - '0');
    return v;
}

typedef struct { const char *name; const uint8_t *data; uint64_t size; bool is_file; } entry;

/* Iterate over archive members; returns false at the end. */
static bool next_entry(uint64_t *off, entry *e)
{
    while (*off + 512 <= total) {
        const char *h = (const char *)base + *off;
        if (!h[0]) return false;                                  /* zero block: end of archive */
        if (memcmp(h + 257, "ustar", 5) != 0) return false;
        uint64_t size = octal(h + 124, 12);
        char type = h[156];
        e->name = h;
        e->data = base + *off + 512;
        e->size = size;
        e->is_file = type == '0' || type == 0;
        *off += 512 + ALIGN_UP(size, 512);
        if (e->data + size > base + total) return false;
        if (e->name[0] == '.' && e->name[1] == '/') e->name += 2;
        return true;
    }
    return false;
}

bool initrd_init(uint64_t phys, uint64_t size)
{
    if (!phys || size < 512) return false;
    base = phys_to_virt(phys);
    total = size;
    return true;
}

bool initrd_find(const char *name, const uint8_t **data, uint64_t *size)
{
    if (!base) return false;
    while (*name == '/') name++;
    uint64_t off = 0;
    entry e;
    while (next_entry(&off, &e)) {
        if (!e.is_file) continue;
        bool match = strncmp(e.name, name, 100) == 0 ||
                     (strncmp(e.name, "bin/", 4) == 0 && strncmp(e.name + 4, name, 96) == 0);
        if (match) { *data = e.data; *size = e.size; return true; }
    }
    return false;
}

void initrd_list(initrd_cb cb, void *ctx)
{
    if (!base) return;
    uint64_t off = 0;
    entry e;
    while (next_entry(&off, &e)) if (e.is_file) cb(e.name, e.size, ctx);
}
