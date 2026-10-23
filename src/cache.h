/* Cache hierarchy for full-system mode (--cache): per core a 48 KiB 12-way
 * L1 data cache and a 32 KiB 8-way L1 instruction cache, and a 3 MiB 12-way
 * L2 shared by all cores. 64-byte lines, write-back, write-allocate, LRU.
 *
 * The caches hold the data: memory is only updated when a dirty line is
 * written back. The L1s are kept coherent with the MESI protocol on a
 * snooping bus (BusRd, BusRdX, BusUpgr, writebacks); a modified line is
 * supplied cache-to-cache by flushing it to the L2. The L2 is inclusive of
 * every L1 (evicting an L2 line back-invalidates the L1 copies) and tracks
 * only valid/dirty, shown as E (clean) and M (dirty). Instruction-cache
 * lines count as sharers, so stores invalidate stale code in any L1I. */
#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>
#include <stdio.h>

#include "mem.h"

#define CACHE_LINE 64
#define CACHE_UNIT_L2 0xFFF /* event unit number of the L2 / external agent */
#define CACHE_LOG_LEN 256

enum { MESI_I, MESI_S, MESI_E, MESI_M };
enum { CA_READ, CA_WRITE, CA_FETCH };
enum { CU_L1D, CU_L1I };                         /* L1 units of a core */
enum { BUS_RD, BUS_RDX, BUS_UPGR, BUS_WB, BUS_BACKINV, BUS_FLUSH }; /* log operations */

typedef struct {
    uint64_t tag;        /* line address (physical address >> 6) */
    uint32_t lru;        /* last use, per-cache clock */
    uint8_t state;       /* MESI_* */
    uint8_t data[CACHE_LINE];
} CacheLine;

typedef struct {
    uint64_t reads, writes, fetches, hits, misses;
    uint64_t evictions, writebacks;   /* lines replaced / dirty lines written to the next level */
    uint64_t snoops, snoop_hits;      /* bus requests seen from other caches / that found the line */
    uint64_t invalidations;           /* copies lost to other caches' writes or L2 back-invalidation */
    uint64_t interventions;           /* modified lines supplied to another cache */
    uint64_t upgrades;                /* S -> M through BusUpgr */
    uint64_t trans[4][4];             /* MESI transitions [from][to] */
} CacheStats;

typedef struct {
    char name[16];
    int sets, ways;
    uint32_t clock;
    CacheLine *lines;   /* sets * ways */
    CacheStats st;
} Cache;

/* One coherence event: the requester's transition plus up to four snooped copies.
 * Units are core * 2 + CU_* (up to 255 cores) or CACHE_UNIT_L2. */
typedef struct {
    uint64_t seq, line;
    uint16_t unit;
    uint8_t op, from, to;
    uint8_t npeers;     /* recorded in peers[] */
    uint16_t nsnooped;  /* all copies the request touched */
    uint16_t peers[4];  /* unit << 4 | from << 2 | to */
} CohEvent;

typedef struct CacheSys {
    Mem *mem;
    int ncores;
    Cache *l1d, *l1i, l2; /* ncores each */
    struct { uint64_t lo, hi; } ranges[4]; /* cacheable physical ranges (write-back RAM) */
    int nranges;
    struct {
        uint64_t bus_rd, bus_rdx, bus_upgr, bus_wb, c2c; /* c2c: transfers from a modified L1 */
        uint64_t mem_reads, mem_writes, uncached;
    } bus;
    CohEvent log[CACHE_LOG_LEN];
    uint64_t nlog;
} CacheSys;

CacheSys *cache_new(Mem *mem, int ncores);
void cache_free(CacheSys *s);
void cache_add_range(CacheSys *s, uint64_t lo, uint64_t hi);
int cache_cacheable(CacheSys *s, uint64_t pa);

/* A CPU access of n bytes (any alignment); buf may be NULL to only touch the lines. */
void cache_access(CacheSys *s, int core, int kind, uint64_t pa, void *buf, int n);
/* Coherent debugger/firmware accesses, done next to a direct memory access:
 * cache_peek copies newer cached bytes over buf (read from memory first),
 * cache_poke updates every cached copy of bytes just written to memory. */
void cache_peek(CacheSys *s, uint64_t pa, void *buf, uint64_t n);
void cache_poke(CacheSys *s, uint64_t pa, const void *buf, uint64_t n);
/* CLFLUSH (write back and invalidate everywhere), CLWB (write back, keep). */
void cache_flush_line(CacheSys *s, uint64_t pa, int invalidate);
/* WBINVD / INVD: write back and invalidate every cache. */
void cache_flush_all(CacheSys *s);

/* States of one line for cores first..first+3: L1D of core first+k in bits
 * 4k..4k+1, its L1I in bits 4k+2..4k+3, the L2 in bits 16-17 (MESI_* values). */
uint32_t cache_line_states(CacheSys *s, uint64_t pa, int first);
/* MESI state of one line in one unit (core * 2 + CU_*, CACHE_UNIT_L2). */
int cache_unit_state(CacheSys *s, int unit, uint64_t pa);
const char *cache_op_name(int op);
extern const char cache_state_char[4];
const char *cache_unit_name(CacheSys *s, int unit); /* core * 2 + CU_*, CACHE_UNIT_L2 */

/* Text reports: statistics, where one line lives, the last n coherence events. */
void cache_print_stats(CacheSys *s, FILE *f);
void cache_print_line(CacheSys *s, uint64_t pa, FILE *f);
void cache_print_log(CacheSys *s, int n, FILE *f);
/* One event as text ("cpu1 L1D BusRdX 0x1000c0: I->M; cpu0 L1D M->I"). */
void cache_format_event(CacheSys *s, const CohEvent *e, char *buf, size_t n);

#endif
