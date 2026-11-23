/* heap.c - kernel heap: first-fit free list with splitting and coalescing */
#include "mm.h"
#include "lib.h"

#define HEAP_MAGIC      0x4E544F53u          /* "NTOS" */
#define HEAP_INITIAL    (4ULL * 1024 * 1024)
#define HEAP_GROW_MIN   (1ULL * 1024 * 1024)
#define MIN_SPLIT       32

typedef struct block {
    size_t        size;      /* payload bytes */
    struct block *next;
    uint32_t      free;
    uint32_t      magic;
    uint64_t      pad;       /* header = 32 bytes keeps payloads 16-byte aligned */
} block_t;

static block_t *head;
static size_t   heap_total;

static bool add_region(size_t bytes)
{
    bytes = ALIGN_UP(bytes, PAGE_SIZE);
    uint64_t phys = pmm_alloc_pages(bytes / PAGE_SIZE);
    if (!phys) return false;
    block_t *b = phys_to_virt(phys);
    b->size = bytes - sizeof(block_t);
    b->next = NULL;
    b->free = 1;
    b->magic = HEAP_MAGIC;
    if (!head) head = b;
    else { block_t *t = head; while (t->next) t = t->next; t->next = b; }
    heap_total += bytes;
    return true;
}

void heap_init(void)
{
    if (!add_region(HEAP_INITIAL)) panic("heap: cannot allocate initial region");
}

void *kmalloc(size_t size)
{
    if (size == 0) return NULL;
    size = ALIGN_UP(size, 16);
    for (int attempt = 0; attempt < 2; attempt++) {
        for (block_t *b = head; b; b = b->next) {
            if (!b->free || b->size < size) continue;
            if (b->size >= size + sizeof(block_t) + MIN_SPLIT) {
                block_t *rest = (block_t *)((uint8_t *)(b + 1) + size);
                rest->size = b->size - size - sizeof(block_t);
                rest->next = b->next;
                rest->free = 1;
                rest->magic = HEAP_MAGIC;
                b->next = rest;
                b->size = size;
            }
            b->free = 0;
            return b + 1;
        }
        if (!add_region(MAX(size + sizeof(block_t), HEAP_GROW_MIN))) return NULL;
    }
    return NULL;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    if (!ptr) return;
    block_t *b = (block_t *)ptr - 1;
    if (b->magic != HEAP_MAGIC || b->free) panic("heap: invalid kfree(%p)", ptr);
    b->free = 1;
    /* Merge physically adjacent free neighbours. */
    for (block_t *c = head; c && c->next;) {
        if (c->free && c->next->free && (uint8_t *)(c + 1) + c->size == (uint8_t *)c->next) {
            c->size += sizeof(block_t) + c->next->size;
            c->next = c->next->next;
        } else {
            c = c->next;
        }
    }
}

void heap_stats(size_t *total, size_t *used, size_t *blocks)
{
    size_t u = 0, n = 0;
    for (block_t *b = head; b; b = b->next) { n++; if (!b->free) u += b->size + sizeof(block_t); }
    *total = heap_total; *used = u; *blocks = n;
}
