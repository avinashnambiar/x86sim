/* Cache hierarchy with MESI coherence (see cache.h).
 *
 * Bus transactions, issued by an L1 that misses or must gain ownership:
 *   BusRd   read miss. A modified copy elsewhere is flushed to the L2
 *           (intervention) and every copy drops to S. The requester gets E
 *           when no other cache holds the line, S otherwise (always S in an
 *           L1I).
 *   BusRdX  write miss (read for ownership). Other copies are flushed if
 *           modified and invalidated; the requester gets M.
 *   BusUpgr write hit on S. Other copies are invalidated; S -> M without
 *           a data transfer. A write hit on E becomes M silently.
 *   BusWB   eviction of an M line from an L1 into the L2.
 * The L2 fills from memory on a miss and writes dirty victims back after
 * back-invalidating the L1 copies (inclusion). */
#include "cache.h"

#include <stdlib.h>
#include <string.h>

const char cache_state_char[4] = {'I', 'S', 'E', 'M'};

const char *cache_op_name(int op) {
    static const char *const n[] = {"BusRd", "BusRdX", "BusUpgr", "BusWB", "BackInv", "Flush"};
    return op >= 0 && op < 6 ? n[op] : "?";
}

static void cache_init(Cache *c, const char *name, int size, int ways) {
    snprintf(c->name, sizeof c->name, "%s", name);
    c->ways = ways;
    c->sets = size / (ways * CACHE_LINE);
    c->lines = calloc((size_t)c->sets * (size_t)ways, sizeof *c->lines);
}

CacheSys *cache_new(Mem *mem, int ncores) {
    CacheSys *s = calloc(1, sizeof *s);
    s->mem = mem;
    s->ncores = ncores < 1 ? 1 : ncores;
    s->l1d = calloc((size_t)s->ncores, sizeof *s->l1d);
    s->l1i = calloc((size_t)s->ncores, sizeof *s->l1i);
    for (int i = 0; i < s->ncores; i++) {
        char name[16];
        snprintf(name, sizeof name, "cpu%d L1D", i);
        cache_init(&s->l1d[i], name, 48 * 1024, 12); /* 64 sets */
        snprintf(name, sizeof name, "cpu%d L1I", i);
        cache_init(&s->l1i[i], name, 32 * 1024, 8);  /* 64 sets */
    }
    cache_init(&s->l2, "L2", 3 * 1024 * 1024, 12); /* 4096 sets */
    return s;
}

void cache_free(CacheSys *s) {
    if (!s) return;
    for (int i = 0; i < s->ncores; i++) {
        free(s->l1d[i].lines);
        free(s->l1i[i].lines);
    }
    free(s->l1d);
    free(s->l1i);
    free(s->l2.lines);
    free(s);
}

void cache_add_range(CacheSys *s, uint64_t lo, uint64_t hi) {
    if (s->nranges < 4 && lo < hi) {
        s->ranges[s->nranges].lo = lo;
        s->ranges[s->nranges].hi = hi;
        s->nranges++;
    }
}

int cache_cacheable(CacheSys *s, uint64_t pa) {
    for (int i = 0; i < s->nranges; i++)
        if (pa >= s->ranges[i].lo && pa < s->ranges[i].hi) return 1;
    return 0;
}

/* ---- lines ---- */

static int nunits(CacheSys *s) { return s->ncores * 2; }
static Cache *unit_cache(CacheSys *s, int u) { return (u & 1) ? &s->l1i[u >> 1] : &s->l1d[u >> 1]; }

static CacheLine *find(Cache *c, uint64_t line) {
    CacheLine *set = c->lines + (line & (uint64_t)(c->sets - 1)) * (uint64_t)c->ways;
    for (int w = 0; w < c->ways; w++)
        if (set[w].state != MESI_I && set[w].tag == line) return &set[w];
    return NULL;
}

static CacheLine *victim(Cache *c, uint64_t line) {
    CacheLine *set = c->lines + (line & (uint64_t)(c->sets - 1)) * (uint64_t)c->ways, *v = set;
    for (int w = 0; w < c->ways; w++) {
        if (set[w].state == MESI_I) return &set[w];
        if (set[w].lru < v->lru) v = &set[w];
    }
    return v;
}

static void set_state(Cache *c, CacheLine *l, int st) {
    if (l->state != st) c->st.trans[l->state][st]++;
    l->state = (uint8_t)st;
}

static CohEvent *log_event(CacheSys *s, int op, int unit, uint64_t line, int from) {
    CohEvent *e = &s->log[s->nlog % CACHE_LOG_LEN];
    e->seq = s->nlog++;
    e->op = (uint8_t)op;
    e->unit = (uint16_t)unit;
    e->line = line;
    e->from = e->to = (uint8_t)from;
    e->npeers = 0;
    e->nsnooped = 0;
    return e;
}

static void log_peer(CohEvent *e, int unit, int from, int to) {
    e->nsnooped++;
    if (e->npeers < 4) e->peers[e->npeers++] = (uint16_t)(unit << 4 | from << 2 | to);
}

/* ---- L2 ---- */

static void mem_fill(CacheSys *s, uint64_t line, uint8_t *data) {
    size_t got = mem_peek(s->mem, line << 6, data, CACHE_LINE);
    if (got < CACHE_LINE) memset(data + got, 0xFF, CACHE_LINE - got);
    s->bus.mem_reads++;
}

static void mem_writeback(CacheSys *s, uint64_t line, const uint8_t *data) {
    mem_poke(s->mem, line << 6, data, CACHE_LINE);
    s->bus.mem_writes++;
}

/* Evict an L2 line: pull modified data out of the L1s, invalidate them,
 * write the line to memory if dirty. */
static void l2_evict(CacheSys *s, CacheLine *v) {
    Cache *c = &s->l2;
    CohEvent *e = log_event(s, BUS_BACKINV, CACHE_UNIT_L2, v->tag, v->state);
    c->st.evictions++;
    for (int u = 0; u < nunits(s); u++) {
        Cache *x = unit_cache(s, u);
        CacheLine *l = find(x, v->tag);
        if (!l) continue;
        if (l->state == MESI_M) {
            memcpy(v->data, l->data, CACHE_LINE);
            set_state(c, v, MESI_M);
            x->st.writebacks++;
        }
        log_peer(e, u, l->state, MESI_I);
        set_state(x, l, MESI_I);
        x->st.invalidations++;
    }
    if (v->state == MESI_M) {
        mem_writeback(s, v->tag, v->data);
        c->st.writebacks++;
    }
    set_state(c, v, MESI_I);
    e->to = MESI_I;
}

static CacheLine *l2_get(CacheSys *s, uint64_t line) {
    Cache *c = &s->l2;
    CacheLine *l = find(c, line);
    if (l) {
        c->st.hits++;
    } else {
        c->st.misses++;
        l = victim(c, line);
        if (l->state != MESI_I) l2_evict(s, l);
        mem_fill(s, line, l->data);
        l->tag = line;
        set_state(c, l, MESI_E);
    }
    c->st.reads++;
    l->lru = ++c->clock;
    return l;
}

/* An L1's modified data goes to the (inclusive) L2 copy. */
static void l1_to_l2(CacheSys *s, CacheLine *l) {
    CacheLine *l2 = find(&s->l2, l->tag);
    if (!l2) return; /* cannot happen: the L2 includes every L1 line */
    memcpy(l2->data, l->data, CACHE_LINE);
    set_state(&s->l2, l2, MESI_M);
    s->l2.st.writes++;
    s->l2.st.hits++; /* inclusion: a writeback always finds its line */
}

/* ---- L1 ---- */

static void l1_evict(CacheSys *s, Cache *x, int unit, CacheLine *l) {
    x->st.evictions++;
    if (l->state == MESI_M) {
        CohEvent *e = log_event(s, BUS_WB, unit, l->tag, MESI_M);
        e->to = MESI_I;
        l1_to_l2(s, l);
        x->st.writebacks++;
        s->bus.bus_wb++;
    }
    set_state(x, l, MESI_I);
}

/* Snoop every other L1 for a BusRd (write = 0) or BusRdX (write = 1).
 * Returns 1 when some other cache held the line. */
static int snoop(CacheSys *s, int unit, uint64_t line, int write, CohEvent *e) {
    int shared = 0;
    for (int u = 0; u < nunits(s); u++) {
        if (u == unit) continue;
        Cache *y = unit_cache(s, u);
        y->st.snoops++;
        CacheLine *l = find(y, line);
        if (!l) continue;
        y->st.snoop_hits++;
        int from = l->state, to = write ? MESI_I : MESI_S;
        if (from == MESI_M) {
            l1_to_l2(s, l);
            y->st.interventions++;
            s->bus.c2c++;
        }
        if (write) y->st.invalidations++;
        log_peer(e, u, from, to);
        set_state(y, l, to);
        shared = 1;
    }
    return shared;
}

static void event(CacheSys *s, int unit, int ev) {
    if (s->on_event) s->on_event(s->ev_ctx, unit >> 1, ev);
}

static CacheLine *l1_miss(CacheSys *s, int unit, uint64_t line, int write) {
    Cache *x = unit_cache(s, unit);
    uint64_t c2c = s->bus.c2c, l2miss = s->l2.st.misses;
    event(s, unit, (unit & 1) ? CE_L1I_MISS : CE_L1D_MISS);
    event(s, unit, CE_L2_REF);
    CohEvent *e = log_event(s, write ? BUS_RDX : BUS_RD, unit, line, MESI_I);
    if (write) s->bus.bus_rdx++;
    else s->bus.bus_rd++;
    int shared = snoop(s, unit, line, write, e);
    CacheLine *l2 = l2_get(s, line);
    if (s->bus.c2c != c2c) event(s, unit, CE_HITM);       /* another core had it modified */
    if (s->l2.st.misses != l2miss) event(s, unit, CE_L2_MISS);
    CacheLine *v = victim(x, line);
    if (v->state != MESI_I) l1_evict(s, x, unit, v);
    memcpy(v->data, l2->data, CACHE_LINE);
    v->tag = line;
    int st = write ? MESI_M : ((unit & 1) || shared) ? MESI_S : MESI_E;
    set_state(x, v, st);
    e->to = (uint8_t)st;
    return v;
}

static void upgrade(CacheSys *s, int unit, CacheLine *l) {
    Cache *x = unit_cache(s, unit);
    CohEvent *e = log_event(s, BUS_UPGR, unit, l->tag, MESI_S);
    s->bus.bus_upgr++;
    x->st.upgrades++;
    for (int u = 0; u < nunits(s); u++) {
        if (u == unit) continue;
        Cache *y = unit_cache(s, u);
        y->st.snoops++;
        CacheLine *o = find(y, l->tag);
        if (!o) continue;
        y->st.snoop_hits++;
        y->st.invalidations++;
        log_peer(e, u, o->state, MESI_I);
        set_state(y, o, MESI_I);
    }
    set_state(x, l, MESI_M);
    e->to = MESI_M;
}

static void access_line(CacheSys *s, int core, int kind, uint64_t pa, uint8_t *buf, int n) {
    if (!cache_cacheable(s, pa)) {
        s->bus.uncached++;
        if (!buf) return;
        if (kind == CA_WRITE) {
            if (mem_write(s->mem, pa, buf, (size_t)n))
                for (int i = 0; i < n; i++) mem_write(s->mem, pa + i, buf + i, 1);
        } else if (mem_read(s->mem, pa, buf, (size_t)n)) {
            for (int i = 0; i < n; i++)
                if (mem_read(s->mem, pa + i, buf + i, 1)) buf[i] = 0xFF;
        }
        return;
    }
    uint64_t line = pa >> 6;
    int unit = core * 2 + (kind == CA_FETCH ? CU_L1I : CU_L1D);
    Cache *x = unit_cache(s, unit);
    if (kind == CA_WRITE) x->st.writes++;
    else if (kind == CA_FETCH) x->st.fetches++;
    else x->st.reads++;
    CacheLine *l = find(x, line);
    if (l) {
        x->st.hits++;
        if (kind == CA_WRITE) {
            if (l->state == MESI_S) upgrade(s, unit, l);
            else if (l->state == MESI_E) set_state(x, l, MESI_M);
        }
    } else {
        x->st.misses++;
        l = l1_miss(s, unit, line, kind == CA_WRITE);
    }
    l->lru = ++x->clock;
    if (!buf) return;
    uint64_t off = pa & (CACHE_LINE - 1);
    if (kind == CA_WRITE) memcpy(l->data + off, buf, (size_t)n);
    else memcpy(buf, l->data + off, (size_t)n);
}

void cache_access(CacheSys *s, int core, int kind, uint64_t pa, void *buf, int n) {
    uint8_t *b = buf;
    while (n > 0) {
        int k = CACHE_LINE - (int)(pa & (CACHE_LINE - 1));
        if (k > n) k = n;
        access_line(s, core, kind, pa, b, k);
        pa += (uint64_t)k;
        if (b) b += k;
        n -= k;
    }
}

/* ---- coherent side accesses ---- */

/* The newest copy of a line: a modified L1 line, else the L2 (which
 * includes every clean L1 copy), else NULL (memory is current). */
static CacheLine *newest(CacheSys *s, uint64_t line) {
    for (int c = 0; c < s->ncores; c++) {
        CacheLine *l = find(&s->l1d[c], line);
        if (l && l->state == MESI_M) return l;
    }
    return find(&s->l2, line);
}

void cache_peek(CacheSys *s, uint64_t pa, void *buf, uint64_t n) {
    uint8_t *b = buf;
    while (n) {
        uint64_t off = pa & (CACHE_LINE - 1), k = CACHE_LINE - off;
        if (k > n) k = n;
        CacheLine *l = cache_cacheable(s, pa) ? newest(s, pa >> 6) : NULL;
        if (l) memcpy(b, l->data + off, k);
        pa += k, b += k, n -= k;
    }
}

void cache_poke(CacheSys *s, uint64_t pa, const void *buf, uint64_t n) {
    const uint8_t *b = buf;
    while (n) {
        uint64_t off = pa & (CACHE_LINE - 1), k = CACHE_LINE - off;
        if (k > n) k = n;
        if (cache_cacheable(s, pa)) {
            for (int u = 0; u < nunits(s); u++) {
                CacheLine *l = find(unit_cache(s, u), pa >> 6);
                if (l) memcpy(l->data + off, b, k);
            }
            CacheLine *l = find(&s->l2, pa >> 6);
            if (l) memcpy(l->data + off, b, k);
        }
        pa += k, b += k, n -= k;
    }
}

void cache_flush_line(CacheSys *s, uint64_t pa, int invalidate) {
    if (!cache_cacheable(s, pa)) return;
    uint64_t line = pa >> 6;
    CacheLine *l2 = find(&s->l2, line);
    if (!l2) return; /* not cached anywhere (inclusion) */
    CohEvent *e = log_event(s, BUS_FLUSH, CACHE_UNIT_L2, line, l2->state);
    for (int u = 0; u < nunits(s); u++) {
        Cache *x = unit_cache(s, u);
        CacheLine *l = find(x, line);
        if (!l) continue;
        int to = invalidate ? MESI_I : l->state == MESI_M ? MESI_E : l->state;
        if (l->state == MESI_M) {
            l1_to_l2(s, l);
            x->st.writebacks++;
        }
        log_peer(e, u, l->state, to);
        set_state(x, l, to);
    }
    if (l2->state == MESI_M) {
        mem_writeback(s, line, l2->data);
        s->l2.st.writebacks++;
        set_state(&s->l2, l2, MESI_E);
    }
    if (invalidate) set_state(&s->l2, l2, MESI_I);
    e->to = l2->state;
}

void cache_flush_all(CacheSys *s) {
    for (int u = 0; u < nunits(s); u++) {
        Cache *x = unit_cache(s, u);
        for (int i = 0; i < x->sets * x->ways; i++) {
            CacheLine *l = &x->lines[i];
            if (l->state == MESI_M) {
                l1_to_l2(s, l);
                x->st.writebacks++;
            }
            if (l->state != MESI_I) set_state(x, l, MESI_I);
        }
    }
    Cache *c = &s->l2;
    for (int i = 0; i < c->sets * c->ways; i++) {
        CacheLine *l = &c->lines[i];
        if (l->state == MESI_M) {
            mem_writeback(s, l->tag, l->data);
            c->st.writebacks++;
        }
        if (l->state != MESI_I) set_state(c, l, MESI_I);
    }
    log_event(s, BUS_FLUSH, CACHE_UNIT_L2, 0, MESI_I);
}

Cache *cache_unit(CacheSys *s, int unit) {
    if (unit == CACHE_UNIT_L2) return &s->l2;
    return unit >= 0 && unit < nunits(s) ? unit_cache(s, unit) : NULL;
}

int cache_unit_state(CacheSys *s, int unit, uint64_t pa) {
    if (unit != CACHE_UNIT_L2 && (unit < 0 || unit >= nunits(s))) return MESI_I;
    CacheLine *l = find(unit == CACHE_UNIT_L2 ? &s->l2 : unit_cache(s, unit), pa >> 6);
    return l ? l->state : MESI_I;
}

uint32_t cache_line_states(CacheSys *s, uint64_t pa, int first) {
    uint64_t line = pa >> 6;
    uint32_t v = 0;
    for (int k = 0; k < 4 && first + k < s->ncores; k++) {
        int c = first + k;
        CacheLine *d = find(&s->l1d[c], line), *i = find(&s->l1i[c], line);
        v |= (uint32_t)(d ? d->state : 0) << (4 * k);
        v |= (uint32_t)(i ? i->state : 0) << (4 * k + 2);
    }
    CacheLine *l2 = find(&s->l2, line);
    v |= (uint32_t)(l2 ? l2->state : 0) << 16;
    return v;
}

/* ---- reports ---- */

const char *cache_unit_name(CacheSys *s, int unit) {
    if (unit == CACHE_UNIT_L2) return "L2";
    return unit_cache(s, unit)->name;
}

void cache_format_event(CacheSys *s, const CohEvent *e, char *buf, size_t n) {
    int k = snprintf(buf, n, "#%llu %-8s %-7s 0x%llx: %c->%c", (unsigned long long)e->seq,
                     cache_unit_name(s, e->unit), cache_op_name(e->op), (unsigned long long)(e->line << 6),
                     cache_state_char[e->from & 3], cache_state_char[e->to & 3]);
    for (int i = 0; i < e->npeers && k > 0 && (size_t)k < n; i++) {
        int p = e->peers[i];
        k += snprintf(buf + k, n - (size_t)k, "%s %s %c->%c", i ? "," : ";", cache_unit_name(s, p >> 4),
                      cache_state_char[(p >> 2) & 3], cache_state_char[p & 3]);
    }
    if (e->nsnooped > e->npeers && k > 0 && (size_t)k < n)
        snprintf(buf + k, n - (size_t)k, " (+%d more)", e->nsnooped - e->npeers);
}

static void print_cache(Cache *c, FILE *f) {
    CacheStats *t = &c->st;
    uint64_t acc = t->reads + t->writes + t->fetches;
    int count[4] = {0};
    for (int i = 0; i < c->sets * c->ways; i++) count[c->lines[i].state]++;
    fprintf(f, "  %-9s %7.2f%% hit  %10llu acc %9llu miss %8llu evict %8llu wb | M %5d E %5d S %5d I %5d\n",
            c->name, acc ? 100.0 * (double)t->hits / (double)acc : 0.0, (unsigned long long)acc,
            (unsigned long long)t->misses, (unsigned long long)t->evictions, (unsigned long long)t->writebacks,
            count[MESI_M], count[MESI_E], count[MESI_S], count[MESI_I]);
    if (t->snoops)
        fprintf(f, "  %-9s snoops %llu (hit %llu)  invalidated %llu  interventions %llu  upgrades %llu\n", "",
                (unsigned long long)t->snoops, (unsigned long long)t->snoop_hits, (unsigned long long)t->invalidations,
                (unsigned long long)t->interventions, (unsigned long long)t->upgrades);
}

void cache_print_stats(CacheSys *s, FILE *f) {
    fprintf(f, "caches: L1D 48K 12-way x%d, L1I 32K 8-way x%d, L2 3M 12-way shared, 64B lines, MESI\n",
            s->ncores, s->ncores);
    for (int i = 0; i < s->ncores; i++) {
        print_cache(&s->l1d[i], f);
        print_cache(&s->l1i[i], f);
    }
    print_cache(&s->l2, f);
    fprintf(f, "  bus: BusRd %llu  BusRdX %llu  BusUpgr %llu  BusWB %llu  cache-to-cache %llu\n",
            (unsigned long long)s->bus.bus_rd, (unsigned long long)s->bus.bus_rdx,
            (unsigned long long)s->bus.bus_upgr, (unsigned long long)s->bus.bus_wb, (unsigned long long)s->bus.c2c);
    fprintf(f, "  memory: %llu line reads, %llu line writes, %llu uncached accesses\n",
            (unsigned long long)s->bus.mem_reads, (unsigned long long)s->bus.mem_writes,
            (unsigned long long)s->bus.uncached);
}

void cache_print_line(CacheSys *s, uint64_t pa, FILE *f) {
    uint64_t line = pa >> 6;
    fprintf(f, "line 0x%llx (set L1 %llu, L2 %llu)%s\n", (unsigned long long)(line << 6),
            (unsigned long long)(line & 63), (unsigned long long)(line & 4095),
            cache_cacheable(s, pa) ? "" : " uncacheable");
    for (int u = 0; u < nunits(s); u++) {
        CacheLine *l = find(unit_cache(s, u), line);
        fprintf(f, "  %-9s %c\n", cache_unit_name(s, u), cache_state_char[l ? l->state : 0]);
    }
    CacheLine *l2 = find(&s->l2, line);
    fprintf(f, "  %-9s %s\n", "L2", !l2 ? "I" : l2->state == MESI_M ? "valid, dirty" : "valid, clean");
}

void cache_print_log(CacheSys *s, int n, FILE *f) {
    uint64_t first = s->nlog > (uint64_t)n ? s->nlog - (uint64_t)n : 0;
    if (s->nlog - first > CACHE_LOG_LEN) first = s->nlog - CACHE_LOG_LEN;
    for (uint64_t i = first; i < s->nlog; i++) {
        char buf[256];
        cache_format_event(s, &s->log[i % CACHE_LOG_LEN], buf, sizeof buf);
        fprintf(f, "  %s\n", buf);
    }
}
