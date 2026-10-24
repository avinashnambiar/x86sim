/* Memory management unit for full-system mode: linear -> physical
 * translation for every paging mode (32-bit with PSE, PAE, 4- and 5-level),
 * a software TLB tagged with PCIDs, access/dirty bit updates and the
 * protection checks (U/S, R/W with CR0.WP, NX, SMEP, SMAP, protection keys).
 *
 * The TLB caches one entry per 4 KiB linear page, also for large pages;
 * entries keep the combined permissions of the walk, and every access
 * re-checks them against the current CPL, CR0, CR4 and RFLAGS.AC. */
#include <stdio.h>
#include <string.h>

#include "cache.h"
#include "cpu_internal.h"

#define ADDR_MASK 0x000FFFFFFFFFF000ULL
#define PTE_P 1ULL
#define PTE_W 2ULL
#define PTE_U 4ULL
#define PTE_A 0x20ULL
#define PTE_D 0x40ULL
#define PTE_PS 0x80ULL
#define PTE_G 0x100ULL
#define PTE_NX (1ULL << 63)

enum { TLB_LARGE = 32, TLB_SS = 64 }; /* SS: CET shadow-stack page (R/W=0, D=1, upper levels writable) */

static uint16_t cur_pcid(CPU *c) { return (c->cr4 & CR4_PCIDE) ? (uint16_t)(c->cr3 & 0xFFF) : 0; }

/* ---- physical memory: unmapped addresses read as all ones, writes are dropped ----
 * With the cache hierarchy simulated, RAM accesses go through this CPU's L1. */

static void raw_read(CPU *c, uint64_t pa, void *buf, int n) {
    if (pa >= c->mmio_lo && pa < c->mmio_hi && c->on_mmio(c, pa, buf, n, 0)) return;
    if (c->cache) {
        cache_access(c->cache, c->index, CA_READ, pa, buf, n);
        return;
    }
    if (mem_read(c->mem, pa, buf, (size_t)n)) {
        uint8_t *b = buf;
        for (int i = 0; i < n; i++)
            if (mem_read(c->mem, pa + i, b + i, 1)) b[i] = 0xFF;
    }
}

static void raw_write(CPU *c, uint64_t pa, const void *buf, int n) {
    if (pa >= c->mmio_lo && pa < c->mmio_hi && c->on_mmio(c, pa, (void *)buf, n, 1)) return;
    if (c->icache) icache_written(c, pa, (size_t)n);
    if (c->cache) {
        cache_access(c->cache, c->index, CA_WRITE, pa, (void *)buf, n);
        return;
    }
    if (mem_write(c->mem, pa, buf, (size_t)n)) {
        const uint8_t *b = buf;
        for (int i = 0; i < n; i++) mem_write(c->mem, pa + i, b + i, 1);
    }
}

/* Instruction fetch: like phys_read, through the L1 instruction cache. */
/* Loads and stores, through transactional memory when any transaction is
 * live (a transaction touching device memory aborts). */
void phys_read(CPU *c, uint64_t pa, void *buf, int n) {
    pa &= c->a20_mask;
    if (smrr_blocked(c, pa)) {
        memset(buf, 0xFF, (size_t)n);
        return;
    }
    if (c->tx_live && *c->tx_live) {
        if (tx_active(c) && pa >= c->mmio_lo && pa < c->mmio_hi) {
            tx_abort(c, 0);
            c->tx_jump = 1;
            _longjmp(c->jb, 1);
        }
        raw_read(c, pa, buf, n);
        tx_read(c, pa, buf, n);
        return;
    }
    raw_read(c, pa, buf, n);
}

void phys_write(CPU *c, uint64_t pa, const void *buf, int n) {
    pa &= c->a20_mask;
    if (smrr_blocked(c, pa)) return;
    if (c->tx_live && *c->tx_live) {
        if (tx_active(c) && pa >= c->mmio_lo && pa < c->mmio_hi) {
            tx_abort(c, 0);
            c->tx_jump = 1;
            _longjmp(c->jb, 1);
        }
        if (tx_write(c, pa, buf, n)) return;
    }
    raw_write(c, pa, buf, n);
}

/* Page walks and their accessed/dirty updates are not transactional. */
static void pt_raw_read(CPU *c, uint64_t pa, void *buf, int n) { raw_read(c, pa & c->a20_mask, buf, n); }

void phys_fetch(CPU *c, uint64_t pa, void *buf, int n) {
    pa &= c->a20_mask;
    if (smrr_blocked(c, pa)) {
        memset(buf, 0xFF, (size_t)n);
        return;
    }
    if (c->cache && !(pa >= c->mmio_lo && pa < c->mmio_hi)) {
        cache_access(c->cache, c->index, CA_FETCH, pa, buf, n);
        return;
    }
    raw_read(c, pa, buf, n);
}

static uint64_t pt_read(CPU *c, uint64_t pa, int size) {
    uint64_t v = 0;
    pt_raw_read(c, pa, &v, size);
    return v;
}

static void pt_set_bits(CPU *c, uint64_t pa, int size, uint64_t old, uint64_t bits) {
    if ((old & bits) == bits) return;
    uint64_t v = old | bits;
    raw_write(c, pa & c->a20_mask, &v, size);
}

/* ---- TLB maintenance ---- */

void cpu_tlb_flush(CPU *c, int keep_global) {
    for (int i = 0; i < TLB_SIZE; i++)
        if (!(keep_global && (c->tlb[i].flags & TLB_G))) c->tlb[i].tag = 0;
}

void tlb_flush_pcid(CPU *c, uint16_t pcid, int keep_global) {
    for (int i = 0; i < TLB_SIZE; i++) {
        TlbEnt *e = &c->tlb[i];
        if (e->tag && e->pcid == pcid && !(keep_global && (e->flags & TLB_G))) e->tag = 0;
    }
}

/* INVLPG: the page in every PCID, plus anything cached from a large page
 * (whose other 4 KiB pieces live in other slots). */
void tlb_invlpg(CPU *c, uint64_t lin) {
    TlbEnt *e = &c->tlb[(lin >> 12) & (TLB_SIZE - 1)];
    if (e->tag == (lin >> 12) + 1) e->tag = 0;
    for (int i = 0; i < TLB_SIZE; i++)
        if (c->tlb[i].flags & TLB_LARGE) c->tlb[i].tag = 0;
}

void tlb_invlpg_pcid(CPU *c, uint64_t lin, uint16_t pcid) {
    TlbEnt *e = &c->tlb[(lin >> 12) & (TLB_SIZE - 1)];
    if (e->tag == (lin >> 12) + 1 && e->pcid == pcid && !(e->flags & TLB_G)) e->tag = 0;
    for (int i = 0; i < TLB_SIZE; i++)
        if ((c->tlb[i].flags & TLB_LARGE) && c->tlb[i].pcid == pcid) c->tlb[i].tag = 0;
}

/* ---- protection ---- */

static int is_user_access(CPU *c, int acc) { return (acc & ACC_USER) || (c->cpl == 3 && !(acc & ACC_SUPER)); }

/* 0 when allowed, otherwise the #PF error code (protection violation). */
static uint32_t check_perm(CPU *c, int flags, int pkey, int acc) {
    int w = (acc & ACC_W) != 0, user = is_user_access(c, acc);
    uint32_t ec = 1 | (w ? 2 : 0) | (user ? 4 : 0);
    if (acc & ACC_X) {
        if ((c->efer & EFER_NXE) || (c->cr4 & CR4_SMEP)) ec |= 16;
        if (flags & TLB_NX) return ec;
        if (user) return (flags & TLB_U) ? 0 : ec;
        if ((flags & TLB_U) && (c->cr4 & CR4_SMEP)) return ec;
        return 0;
    }
    if (acc & ACC_SHSTK) {
        ec |= 64;
        if (!(flags & TLB_SS)) return ec;
        return user == !!(flags & TLB_U) ? 0 : ec; /* user/supervisor shadow stacks are separate */
    }
    if (user) {
        if (!(flags & TLB_U)) return ec;
        if (w && !(flags & TLB_W)) return ec;
    } else {
        if ((flags & TLB_U) && (c->cr4 & CR4_SMAP) && ((acc & ACC_SUPER) || !(c->rflags & FL_AC))) return ec;
        if (w && !(flags & TLB_W) && (c->cr0 & CR0_WP)) return ec;
    }
    if ((flags & TLB_U) && (c->cr4 & CR4_PKE) && (c->efer & EFER_LMA)) {
        int ad = (c->pkru >> (2 * pkey)) & 1, wd = (c->pkru >> (2 * pkey + 1)) & 1;
        if (ad || (w && wd && (user || (c->cr0 & CR0_WP)))) return ec | 32;
    }
    return 0;
}

/* Walk the paging structures. Returns 0 and fills *e, or WALK_FAIL plus the
 * #PF error code (bit 0 clear: not present; the code itself may be 0).
 * Accessed/dirty bits are set unless the walk is a side-effect-free probe. */
#define WALK_FAIL 0x80000000u
static uint32_t walk(CPU *c, uint64_t lin, int acc, TlbEnt *e) {
    int w = (acc & ACC_W) != 0, user = is_user_access(c, acc), probe = (acc & ACC_NOFAULT) != 0;
    uint32_t np = WALK_FAIL | (w ? 2 : 0) | (user ? 4 : 0);
    if ((acc & ACC_X) && ((c->efer & EFER_NXE) || (c->cr4 & CR4_SMEP))) np |= 16;
    if (acc & ACC_SHSTK) np |= 64;
    uint64_t nx_mask = (c->efer & EFER_NXE) ? PTE_NX : 0;
    int flags = TLB_W | TLB_U, large = 0, pkey = 0;
    uint64_t leaf, leaf_pa, page;
    int leaf_size;

    if (!(c->cr4 & CR4_PAE)) {
        uint64_t pde_pa = (c->cr3 & 0xFFFFF000) + ((lin >> 22) & 0x3FF) * 4;
        uint64_t pde = pt_read(c, pde_pa, 4);
        if (!(pde & PTE_P)) return np;
        if (!(pde & PTE_W)) flags &= ~TLB_W;
        if (!(pde & PTE_U)) flags &= ~TLB_U;
        if ((pde & PTE_PS) && (c->cr4 & CR4_PSE)) {
            page = (pde & 0xFFC00000) | ((pde >> 13) & 0xFF) << 32 | (lin & 0x3FF000);
            leaf = pde;
            leaf_pa = pde_pa;
            large = 1;
        } else {
            if (!probe) pt_set_bits(c, pde_pa, 4, pde, PTE_A);
            leaf_pa = (pde & 0xFFFFF000) + ((lin >> 12) & 0x3FF) * 4;
            leaf = pt_read(c, leaf_pa, 4);
            if (!(leaf & PTE_P)) return np;
            if (!(leaf & PTE_W)) flags &= ~TLB_W;
            if (!(leaf & PTE_U)) flags &= ~TLB_U;
            page = leaf & 0xFFFFF000;
        }
        leaf_size = 4;
    } else {
        uint64_t table;
        int level;
        if (c->efer & EFER_LMA) {
            level = (c->cr4 & CR4_LA57) ? 5 : 4;
            table = c->cr3 & ADDR_MASK;
        } else {
            uint64_t pdpte = pt_read(c, (c->cr3 & 0xFFFFFFE0) + ((lin >> 30) & 3) * 8, 8);
            if (!(pdpte & PTE_P)) return np;
            level = 2;
            table = pdpte & ADDR_MASK;
        }
        for (;; level--) {
            int shift = 12 + 9 * (level - 1);
            uint64_t pa = table + ((lin >> shift) & 511) * 8;
            uint64_t ent = pt_read(c, pa, 8);
            if (!(ent & PTE_P)) return np;
            int leaf_level = level == 1 || (level <= 3 && (ent & PTE_PS));
            if (leaf_level && (flags & TLB_W) && !(ent & PTE_W) && (ent & PTE_D)) flags |= TLB_SS;
            if (!(ent & PTE_W)) flags &= ~TLB_W;
            if (!(ent & PTE_U)) flags &= ~TLB_U;
            if (ent & nx_mask) flags |= TLB_NX;
            if (leaf_level) {
                uint64_t off_mask = (1ULL << shift) - 1;
                page = (ent & ADDR_MASK & ~off_mask) | (lin & off_mask & ~0xFFFULL);
                large = level > 1;
                leaf = ent;
                leaf_pa = pa;
                pkey = (int)((ent >> 59) & 15);
                break;
            }
            if (ent & PTE_PS) return np | 1 | 8; /* PS in a PML4/PML5 entry is reserved */
            if (!probe) pt_set_bits(c, pa, 8, ent, PTE_A);
            table = ent & ADDR_MASK;
        }
        leaf_size = 8;
    }

    if (large) flags |= TLB_LARGE;
    if ((leaf & PTE_G) && (c->cr4 & CR4_PGE)) flags |= TLB_G;
    uint32_t ec = check_perm(c, flags, pkey, acc);
    if (ec) return WALK_FAIL | ec;
    if (!probe) {
        pt_set_bits(c, leaf_pa, leaf_size, leaf, PTE_A | (w ? PTE_D : 0));
        if (w || (leaf & PTE_D)) flags |= TLB_D;
    }
    e->phys = page;
    e->flags = (uint8_t)flags;
    e->pkey = (uint8_t)pkey;
    return 0;
}

_Noreturn static void raise_pf(CPU *c, uint64_t lin, uint32_t ec) {
    char msg[96];
    c->cr2 = lin;
    c->exc_addr = lin;
    snprintf(msg, sizeof msg, "#PF page fault %s 0x%llx", (ec & 16) ? "fetching" : (ec & 2) ? "writing" : "reading",
             (unsigned long long)lin);
    raise_exc(c, EXC_PF, ec, msg);
}

/* Translate; raises #PF unless ACC_NOFAULT (then returns ~0 on failure). */
uint64_t mmu_translate(CPU *c, uint64_t lin, int acc) {
    if (!(c->cr0 & CR0_PG)) return lin & 0xFFFFFFFFULL;
    TlbEnt *e = &c->tlb[(lin >> 12) & (TLB_SIZE - 1)];
    uint64_t tag = (lin >> 12) + 1;
    uint16_t pcid = cur_pcid(c);
    if (e->tag == tag && (e->pcid == pcid || (e->flags & TLB_G)) && (!(acc & ACC_W) || (e->flags & TLB_D)) &&
        !check_perm(c, e->flags, e->pkey, acc))
        return e->phys | (lin & 0xFFF);
    TlbEnt ne;
    uint32_t ec = walk(c, lin, acc, &ne);
    if (ec) {
        if (acc & ACC_NOFAULT) return ~0ULL;
        if (e->tag == tag) e->tag = 0;
        raise_pf(c, lin, ec & ~WALK_FAIL);
    }
    if (acc & ACC_NOFAULT) return ne.phys | (lin & 0xFFF);
    ne.tag = tag;
    ne.pcid = pcid;
    *e = ne;
    return ne.phys | (lin & 0xFFF);
}

int cpu_virt_to_phys(CPU *c, uint64_t lin, uint64_t *phys) {
    if (!c->sys) {
        *phys = lin;
        return 0;
    }
    int cpl = c->cpl;
    c->cpl = 0;
    uint64_t pa = mmu_translate(c, lin, ACC_NOFAULT | ACC_SUPER);
    c->cpl = cpl;
    if (pa == ~0ULL) return -1;
    *phys = pa & c->a20_mask;
    return 0;
}

/* ---- linear accesses; page-crossing accesses check both pages first ---- */

void mmu_read(CPU *c, uint64_t lin, void *buf, int n, int acc) {
    uint64_t off = lin & 0xFFF;
    if (off + (uint64_t)n <= 4096) {
        phys_read(c, mmu_translate(c, lin, acc), buf, n);
        return;
    }
    int n1 = (int)(4096 - off);
    uint64_t lin2 = (lin + (uint64_t)n1) & (c->bits == 64 ? ~0ULL : 0xFFFFFFFFULL);
    uint64_t p1 = mmu_translate(c, lin, acc), p2 = mmu_translate(c, lin2, acc);
    phys_read(c, p1, buf, n1);
    phys_read(c, p2, (uint8_t *)buf + n1, n - n1);
}

void mmu_write(CPU *c, uint64_t lin, const void *buf, int n, int acc) {
    acc |= ACC_W;
    uint64_t off = lin & 0xFFF;
    if (off + (uint64_t)n <= 4096) {
        phys_write(c, mmu_translate(c, lin, acc), buf, n);
        return;
    }
    int n1 = (int)(4096 - off);
    uint64_t lin2 = (lin + (uint64_t)n1) & (c->bits == 64 ? ~0ULL : 0xFFFFFFFFULL);
    uint64_t p1 = mmu_translate(c, lin, acc), p2 = mmu_translate(c, lin2, acc);
    phys_write(c, p1, buf, n1);
    phys_write(c, p2, (const uint8_t *)buf + n1, n - n1);
}

/* Instruction fetch: faults only for the first byte; the following page
 * is read only if it is accessible (the decoder reports truncation). */
size_t mmu_fetch(CPU *c, uint64_t lin, uint8_t *buf, size_t n) {
    uint64_t off = lin & 0xFFF;
    size_t n1 = 4096 - off < n ? 4096 - off : n;
    phys_fetch(c, mmu_translate(c, lin, ACC_X), buf, (int)n1);
    if (n1 == n) return n;
    uint64_t lin2 = (lin + n1) & (c->bits == 64 ? ~0ULL : 0xFFFFFFFFULL);
    uint64_t p2 = mmu_translate(c, lin2, ACC_X | ACC_NOFAULT);
    if (p2 == ~0ULL) return n1;
    phys_fetch(c, p2, buf + n1, (int)(n - n1));
    return n;
}

_Noreturn void mmu_fetch_fault(CPU *c, uint64_t lin) {
    mmu_translate(c, lin, ACC_X);
    raise_exc(c, EXC_GP, 0, "instruction fetch failed");
}
