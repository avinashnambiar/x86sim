/* Sparse, paged guest memory covering the full 64-bit address space. */
#ifndef MEM_H
#define MEM_H

#include <stddef.h>
#include <stdint.h>

enum { MEM_R = 1, MEM_W = 2, MEM_X = 4 };

typedef struct {
    uint64_t start, end;   /* [start, end) */
    int prot;
    char name[32];
} MemRegion;

typedef struct Mem Mem;

Mem *mem_new(void);
void mem_free(Mem *m);

/* Map zero-filled pages (lazily allocated). Remapping replaces contents. */
void mem_map(Mem *m, uint64_t addr, uint64_t len, int prot, const char *name);
void mem_unmap(Mem *m, uint64_t addr, uint64_t len);
void mem_protect(Mem *m, uint64_t addr, uint64_t len, int prot);
int mem_prot_at(Mem *m, uint64_t addr); /* -1 when unmapped */

/* Guest accesses, checked against page protection. 0 on success, -1 on fault. */
int mem_read(Mem *m, uint64_t addr, void *buf, size_t n);
int mem_write(Mem *m, uint64_t addr, const void *buf, size_t n);
size_t mem_fetch(Mem *m, uint64_t addr, void *buf, size_t n); /* executable bytes available */
uint64_t mem_fault_addr(const Mem *m);

/* Debugger/loader accesses: ignore protection, return bytes transferred. */
size_t mem_peek(Mem *m, uint64_t addr, void *buf, size_t n);
size_t mem_poke(Mem *m, uint64_t addr, const void *buf, size_t n);

/* Current mapping as merged regions; caller frees *out. */
int mem_regions(Mem *m, MemRegion **out);

#endif
