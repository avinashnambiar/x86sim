#include "mem.h"

#include <stdlib.h>
#include <string.h>

#define PAGE 4096ULL
#define PMAPPED 0x80
#define TLB_SIZE 256

typedef struct {
    uint64_t pn;
    uint8_t *data;   /* NULL until first touched */
    uint8_t flags;   /* PMAPPED | MEM_R/W/X */
    uint8_t used;
} PageEnt;

typedef struct {
    uint64_t start, end;
    char name[32];
} NamedRange;

struct Mem {
    PageEnt *tab;
    size_t cap, count;
    uint64_t fault_addr;
    uint64_t tlb_pn[TLB_SIZE];
    PageEnt *tlb_ent[TLB_SIZE];
    NamedRange *names;
    int nnames, capnames;
};

static size_t hash(uint64_t pn, size_t cap) {
    return (size_t)((pn * 0x9E3779B97F4A7C15ULL) >> 24) & (cap - 1);
}

Mem *mem_new(void) {
    Mem *m = calloc(1, sizeof *m);
    m->cap = 1024;
    m->tab = calloc(m->cap, sizeof *m->tab);
    return m;
}

void mem_free(Mem *m) {
    if (!m) return;
    for (size_t i = 0; i < m->cap; i++) free(m->tab[i].data);
    free(m->tab);
    free(m->names);
    free(m);
}

static PageEnt *find(Mem *m, uint64_t pn) {
    size_t t = pn & (TLB_SIZE - 1);
    if (m->tlb_ent[t] && m->tlb_pn[t] == pn) return m->tlb_ent[t];
    size_t h = hash(pn, m->cap);
    while (m->tab[h].used) {
        if (m->tab[h].pn == pn) {
            m->tlb_pn[t] = pn;
            m->tlb_ent[t] = &m->tab[h];
            return &m->tab[h];
        }
        h = (h + 1) & (m->cap - 1);
    }
    return NULL;
}

static void grow(Mem *m) {
    size_t oc = m->cap;
    PageEnt *ot = m->tab;
    m->cap = oc * 2;
    m->tab = calloc(m->cap, sizeof *m->tab);
    for (size_t i = 0; i < oc; i++) {
        if (!ot[i].used) continue;
        size_t h = hash(ot[i].pn, m->cap);
        while (m->tab[h].used) h = (h + 1) & (m->cap - 1);
        m->tab[h] = ot[i];
    }
    free(ot);
    memset(m->tlb_ent, 0, sizeof m->tlb_ent);
}

static PageEnt *insert(Mem *m, uint64_t pn) {
    PageEnt *e = find(m, pn);
    if (e) return e;
    if ((m->count + 1) * 2 > m->cap) grow(m);
    size_t h = hash(pn, m->cap);
    while (m->tab[h].used) h = (h + 1) & (m->cap - 1);
    e = &m->tab[h];
    e->used = 1;
    e->pn = pn;
    e->data = NULL;
    e->flags = 0;
    m->count++;
    return e;
}

static void add_name(Mem *m, uint64_t start, uint64_t end, const char *name) {
    if (!name || !*name) return;
    if (m->nnames == m->capnames) {
        m->capnames = m->capnames ? m->capnames * 2 : 16;
        m->names = realloc(m->names, m->capnames * sizeof *m->names);
    }
    NamedRange *r = &m->names[m->nnames++];
    r->start = start;
    r->end = end;
    strncpy(r->name, name, sizeof r->name - 1);
    r->name[sizeof r->name - 1] = 0;
}

void mem_map(Mem *m, uint64_t addr, uint64_t len, int prot, const char *name) {
    if (!len) return;
    uint64_t first = addr / PAGE, last = (addr + len - 1) / PAGE;
    for (uint64_t pn = first; pn <= last; pn++) {
        PageEnt *e = insert(m, pn);
        if (e->data) memset(e->data, 0, PAGE);
        e->flags = PMAPPED | (prot & 7);
    }
    add_name(m, first * PAGE, (last + 1) * PAGE, name);
}

void mem_unmap(Mem *m, uint64_t addr, uint64_t len) {
    if (!len) return;
    uint64_t first = addr / PAGE, last = (addr + len - 1) / PAGE;
    for (uint64_t pn = first; pn <= last; pn++) {
        PageEnt *e = find(m, pn);
        if (!e) continue;
        free(e->data);
        e->data = NULL;
        e->flags = 0;
    }
}

void mem_protect(Mem *m, uint64_t addr, uint64_t len, int prot) {
    if (!len) return;
    uint64_t first = addr / PAGE, last = (addr + len - 1) / PAGE;
    for (uint64_t pn = first; pn <= last; pn++) {
        PageEnt *e = find(m, pn);
        if (e && (e->flags & PMAPPED)) e->flags = PMAPPED | (prot & 7);
    }
}

int mem_prot_at(Mem *m, uint64_t addr) {
    PageEnt *e = find(m, addr / PAGE);
    if (!e || !(e->flags & PMAPPED)) return -1;
    return e->flags & 7;
}

static uint8_t *page_ptr(Mem *m, uint64_t addr, int need) {
    PageEnt *e = find(m, addr / PAGE);
    if (!e || !(e->flags & PMAPPED) || (e->flags & need) != need) return NULL;
    if (!e->data) e->data = calloc(1, PAGE);
    return e->data;
}

int mem_read(Mem *m, uint64_t addr, void *buf, size_t n) {
    uint8_t *out = buf;
    while (n) {
        size_t off = addr & (PAGE - 1), c = PAGE - off;
        if (c > n) c = n;
        uint8_t *p = page_ptr(m, addr, MEM_R);
        if (!p) {
            m->fault_addr = addr;
            return -1;
        }
        memcpy(out, p + off, c);
        out += c;
        addr += c;
        n -= c;
    }
    return 0;
}

int mem_write(Mem *m, uint64_t addr, const void *buf, size_t n) {
    /* Check every page first so a faulting write leaves memory untouched. */
    uint64_t a = addr;
    size_t left = n;
    while (left) {
        size_t off = a & (PAGE - 1), c = PAGE - off;
        if (c > left) c = left;
        if (!page_ptr(m, a, MEM_W)) {
            m->fault_addr = a;
            return -1;
        }
        a += c;
        left -= c;
    }
    const uint8_t *in = buf;
    while (n) {
        size_t off = addr & (PAGE - 1), c = PAGE - off;
        if (c > n) c = n;
        memcpy(page_ptr(m, addr, MEM_W) + off, in, c);
        in += c;
        addr += c;
        n -= c;
    }
    return 0;
}

static size_t copy_out(Mem *m, uint64_t addr, void *buf, size_t n, int need) {
    uint8_t *out = buf;
    size_t done = 0;
    while (done < n) {
        size_t off = addr & (PAGE - 1), c = PAGE - off;
        if (c > n - done) c = n - done;
        uint8_t *p = page_ptr(m, addr, need);
        if (!p) {
            m->fault_addr = addr;
            break;
        }
        memcpy(out + done, p + off, c);
        done += c;
        addr += c;
    }
    return done;
}

size_t mem_fetch(Mem *m, uint64_t addr, void *buf, size_t n) {
    return copy_out(m, addr, buf, n, MEM_X);
}

size_t mem_peek(Mem *m, uint64_t addr, void *buf, size_t n) {
    return copy_out(m, addr, buf, n, 0);
}

size_t mem_poke(Mem *m, uint64_t addr, const void *buf, size_t n) {
    const uint8_t *in = buf;
    size_t done = 0;
    while (done < n) {
        size_t off = addr & (PAGE - 1), c = PAGE - off;
        if (c > n - done) c = n - done;
        uint8_t *p = page_ptr(m, addr, 0);
        if (!p) break;
        memcpy(p + off, in + done, c);
        done += c;
        addr += c;
    }
    return done;
}

uint64_t mem_fault_addr(const Mem *m) { return m->fault_addr; }

typedef struct {
    uint64_t pn;
    int prot;
} PageInfo;

static int cmp_page(const void *a, const void *b) {
    uint64_t x = ((const PageInfo *)a)->pn, y = ((const PageInfo *)b)->pn;
    return x < y ? -1 : x > y;
}

int mem_regions(Mem *m, MemRegion **out) {
    PageInfo *pages = malloc((m->count + 1) * sizeof *pages);
    size_t np = 0;
    for (size_t i = 0; i < m->cap; i++)
        if (m->tab[i].used && (m->tab[i].flags & PMAPPED))
            pages[np++] = (PageInfo){m->tab[i].pn, m->tab[i].flags & 7};
    qsort(pages, np, sizeof *pages, cmp_page);

    MemRegion *regs = NULL;
    int nr = 0, cap = 0;
    for (size_t i = 0; i < np; i++) {
        if (nr && regs[nr - 1].end == pages[i].pn * PAGE && regs[nr - 1].prot == pages[i].prot) {
            regs[nr - 1].end += PAGE;
            continue;
        }
        if (nr == cap) {
            cap = cap ? cap * 2 : 16;
            regs = realloc(regs, cap * sizeof *regs);
        }
        MemRegion *r = &regs[nr++];
        r->start = pages[i].pn * PAGE;
        r->end = r->start + PAGE;
        r->prot = pages[i].prot;
        r->name[0] = 0;
    }
    free(pages);
    for (int i = 0; i < nr; i++)
        for (int j = m->nnames - 1; j >= 0; j--)
            if (regs[i].start >= m->names[j].start && regs[i].start < m->names[j].end) {
                memcpy(regs[i].name, m->names[j].name, sizeof regs[i].name);
                break;
            }
    *out = regs;
    return nr;
}
