// First-fit allocator on sbrk with an address-ordered circular free list, as in K&R section 8.7:
// free merges a block with its free neighbours. Blocks are a header plus whole 16-byte units.
#include <errno.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct header {
    alignas(max_align_t) struct header* next; // next free block (free blocks only)
    size_t units;                             // size of the block, header included
} header;

_Static_assert(sizeof(header) == alignof(max_align_t), "a header is one unit");

enum { kMinGrowUnits = 256 }; // grow the heap by at least 4 KiB at a time

static header base;          // empty list head, size 0
static header* freep = NULL; // where the last search ended

// Extends the heap by `units` units at a unit-aligned address (the program may move the break
// itself).
static header* more(size_t units) {
    char* cur = sbrk(0);
    uintptr_t pad = -(uintptr_t)cur & (sizeof(header) - 1);
    if (cur == (char*)-1 || sbrk((intptr_t)(pad + units * sizeof(header))) == (void*)-1) {
        return NULL;
    }
    return (header*)(cur + pad);
}

static header* grow(size_t units) {
    if (units > INTPTR_MAX / sizeof(header) - 1) {
        errno = ENOMEM;
        return NULL;
    }
    size_t n = units < kMinGrowUnits ? kMinGrowUnits : units;
    header* block = more(n);
    if (!block && n != units) {
        n = units; // not enough room for the minimum, try exactly what is needed
        block = more(n);
    }
    if (!block) {
        return NULL;
    }
    block->units = n;
    free(block + 1);
    return freep;
}

void* malloc(size_t size) {
    if (size > SIZE_MAX - 2 * sizeof(header)) {
        errno = ENOMEM;
        return NULL;
    }
    size_t units = (size + sizeof(header) - 1) / sizeof(header) + 1;
    if (!freep) {
        base.next = freep = &base;
    }
    header* prev = freep;
    for (header* p = prev->next;; prev = p, p = p->next) {
        if (p->units >= units) {
            if (p->units == units) {
                prev->next = p->next;
            } else {
                p->units -= units; // hand out the tail
                p += p->units;
                p->units = units;
            }
            freep = prev;
            return p + 1;
        }
        if (p == freep && !(p = grow(units))) {
            return NULL;
        }
    }
}

void free(void* ptr) {
    if (!ptr) {
        return;
    }
    header* block = (header*)ptr - 1;
    header* p = freep;
    while (!(block > p && block < p->next)) {
        if (p >= p->next && (block > p || block < p->next)) {
            break; // at the end of the list, before its first or after its last block
        }
        p = p->next;
    }
    if (block + block->units == p->next) {
        block->units += p->next->units;
        block->next = p->next->next;
    } else {
        block->next = p->next;
    }
    if (p + p->units == block) {
        p->units += block->units;
        p->next = block->next;
    } else {
        p->next = block;
    }
    freep = p;
}

void* calloc(size_t count, size_t size) {
    if (size != 0 && count > SIZE_MAX / size) {
        errno = ENOMEM;
        return NULL;
    }
    void* p = malloc(count * size);
    return p ? memset(p, 0, count * size) : NULL;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }
    size_t have = (((header*)ptr - 1)->units - 1) * sizeof(header);
    if (size <= have) {
        return ptr;
    }
    void* p = malloc(size);
    if (p) {
        memcpy(p, ptr, have);
        free(ptr);
    }
    return p;
}
