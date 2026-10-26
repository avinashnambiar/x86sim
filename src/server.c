/* A tiny single-threaded HTTP server exposing the machine as a JSON API and
 * serving the web UI. Binds to 127.0.0.1 only. */
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cache.h"
#include "machine.h"
#include "pc.h"

#define RUN_CHUNK 3000000ULL
#define MAX_UPLOAD (1ULL << 30) /* request bodies: OS images uploaded to the library */

static char os_dir[4096]; /* the OS image library: os/ next to ui/ */

/* ---- JSON helpers ---- */

static void json_str(Buf *b, const char *s) {
    buf_append(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': buf_append(b, "\\\"", 2); break;
        case '\\': buf_append(b, "\\\\", 2); break;
        case '\n': buf_append(b, "\\n", 2); break;
        case '\r': buf_append(b, "\\r", 2); break;
        case '\t': buf_append(b, "\\t", 2); break;
        default:
            if (*p < 0x20 || *p >= 0x7F) buf_printf(b, "\\u%04x", *p);
            else buf_append(b, p, 1);
        }
    }
    buf_append(b, "\"", 1);
}

static void json_strn(Buf *b, const char *s, size_t n) {
    char *t = malloc(n + 1);
    memcpy(t, s, n);
    t[n] = 0;
    /* NUL bytes in guest output would truncate; replace them */
    for (size_t i = 0; i < n; i++)
        if (!t[i]) t[i] = ' ';
    json_str(b, t);
    free(t);
}

static void json_hex(Buf *b, uint64_t v) { buf_printf(b, "\"0x%llx\"", (unsigned long long)v); }

/* ---- query parsing ---- */

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

static int query_get(const char *query, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t vl = len - kl - 1;
            if (vl >= n) vl = n - 1;
            memcpy(out, p + kl + 1, vl);
            out[vl] = 0;
            url_decode(out);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

static uint64_t query_u64(const char *q, const char *key, uint64_t def) {
    char v[64];
    if (!query_get(q, key, v, sizeof v)) return def;
    return strtoull(v, NULL, 0);
}

/* ---- API responses ---- */

static const char *state_name(int s) {
    switch (s) {
    case MS_READY: return "ready";
    case MS_WAIT_INPUT: return "wait_input";
    case MS_EXITED: return "exited";
    case MS_FAULT: return "fault";
    case MS_HALTED: return "halted";
    }
    return "?";
}

static void api_state(Machine *m, Buf *b) {
    CPU *c = machine_cpu(m);
    static const char *names[16] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
                                    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    static const int order[16] = {RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP, R8, R9, R10, R11, R12, R13, R14, R15};
    char sym[160];
    buf_printf(b, "{\"state\":\"%s\",\"stop\":\"%s\",\"status\":", state_name(m->state), stop_reason_name(m->last_stop));
    json_str(b, m->status);
    buf_printf(b, ",\"bits\":%d,\"os\":\"%s\",\"icount\":%llu,\"exit_code\":%d,\"rip\":", m->bits,
               m->os == OS_LINUX ? "linux" : m->os == OS_DOS ? "dos" : m->os == OS_PC ? "pc" : "none",
               (unsigned long long)c->icount, m->exit_code);
    json_hex(b, c->rip);
    if (c->sys) {
        const char *mode = c->smm ? "smm" : !(c->cr0 & CR0_PE) ? "real" : (c->rflags & FL_VM) ? "vm86"
                         : c->bits == 64 ? "long" : (c->efer & EFER_LMA) ? "compat" : "protected";
        buf_printf(b, ",\"sys\":{\"mode\":\"%s\",\"cpl\":%d,\"paging\":%s,\"time_ns\":%llu", mode, c->cpl,
                   (c->cr0 & CR0_PG) ? "true" : "false", (unsigned long long)pc_time_ns(m));
        buf_printf(b, ",\"vmx\":\"%s\",\"vm_exits\":%llu", c->vmx.guest ? "guest" : c->vmx.on ? "root" : "off",
                   (unsigned long long)c->vmx.exits);
        const char *cn[] = {"cr0", "cr2", "cr3", "cr4", "efer"};
        uint64_t cv[] = {c->cr0, c->cr2, c->cr3, c->cr4, c->efer};
        for (int i = 0; i < 5; i++) {
            buf_printf(b, ",\"%s\":", cn[i]);
            json_hex(b, cv[i]);
        }
        buf_printf(b, "}");
    }
    machine_symname(m, c->rip, sym, sizeof sym);
    buf_printf(b, ",\"where\":");
    json_str(b, sym);
    buf_printf(b, ",\"regs\":[");
    for (int i = 0; i < 16; i++) {
        buf_printf(b, "%s[\"%s\",", i ? "," : "", names[i]);
        json_hex(b, c->r[order[i]]);
        buf_printf(b, "]");
    }
    buf_printf(b, "],\"rflags\":");
    json_hex(b, c->rflags);
    buf_printf(b, ",\"segs\":[");
    static const char *sn[6] = {"es", "cs", "ss", "ds", "fs", "gs"};
    for (int i = 0; i < 6; i++) {
        buf_printf(b, "%s[\"%s\",%u,", i ? "," : "", sn[i], c->sreg[i]);
        json_hex(b, c->sbase[i]);
        buf_printf(b, "]");
    }
    buf_printf(b, "],\"xmm\":[");
    for (int i = 0; i < 16; i++)
        buf_printf(b, "%s\"%016llx%016llx\"", i ? "," : "", (unsigned long long)c->zmm[i].q[1],
                   (unsigned long long)c->zmm[i].q[0]);
    buf_printf(b, "],\"mxcsr\":%u,\"out_len\":%zu,\"input_pending\":%zu,\"breakpoints\":[", c->mxcsr, m->out.len,
               m->in.len - m->in_pos);
    for (int i = 0; i < m->nbp; i++) {
        if (i) buf_append(b, ",", 1);
        json_hex(b, m->bp[i]);
    }
    buf_printf(b, "],\"program\":[");
    for (int i = 0; i < m->nfiles; i++) {
        if (i) buf_append(b, ",", 1);
        json_str(b, m->files[i]);
    }
    buf_printf(b, "],\"boot\":{");
    const char *bn[] = {"kernel", "initrd", "append", "hda", "fda"};
    const char *bv[] = {m->pc_kernel, m->pc_initrd, m->pc_append, m->pc_hda, m->pc_fda};
    for (int i = 0, n = 0; i < 5; i++)
        if (bv[i]) {
            const char *slash = i == 2 ? NULL : strrchr(bv[i], '/');
            buf_printf(b, "%s\"%s\":", n++ ? "," : "", bn[i]);
            json_str(b, slash ? slash + 1 : bv[i]);
        }
    buf_printf(b, "},\"syscalls\":%llu,\"view_cpu\":%d,\"cache\":%s,\"cpus\":[", (unsigned long long)m->syslog_count,
               m->view_cpu, m->cache ? "true" : "false");
    for (int i = 0; i < m->ncpu; i++) {
        CPU *k = m->cpus[i];
        buf_printf(b, "%s{\"state\":\"%s\",\"icount\":%llu,\"rip\":", i ? "," : "",
                   k->wait_sipi ? "wait-sipi" : k->halted ? "halted" : "running", (unsigned long long)k->icount);
        json_hex(b, k->rip);
        buf_printf(b, "}");
    }
    buf_printf(b, "]}");
}

static void disasm_line(Machine *m, Buf *b, uint64_t addr, int *len_out) {
    Insn in;
    char text[200], sym[160];
    int len = machine_disasm(m, addr, &in, text, sizeof text);
    buf_printf(b, "{\"addr\":");
    json_hex(b, addr);
    buf_printf(b, ",\"bytes\":\"");
    for (int i = 0; i < len; i++) buf_printf(b, "%s%02x", i ? " " : "", in.bytes[i]);
    buf_printf(b, "\",\"text\":");
    json_str(b, text);
    const Symbol *s = machine_find_symbol(m, addr);
    if (s && s->addr == addr) {
        buf_printf(b, ",\"label\":");
        json_str(b, s->name);
    }
    if (in.id == I_JCC || in.id == I_JMP || in.id == I_CALL || in.id == I_LOOP || in.id == I_LOOPE ||
        in.id == I_LOOPNE || in.id == I_JRCXZ) {
        if (in.opnd[0] == O_Jb || in.opnd[0] == O_Jz) {
            buf_printf(b, ",\"target\":");
            json_hex(b, in.target);
        }
    }
    (void)sym;
    buf_printf(b, ",\"bp\":%s}", machine_has_bp(m, addr) ? "true" : "false");
    *len_out = len;
}

static void api_disasm(Machine *m, const char *q, Buf *b) {
    int count = (int)query_u64(q, "count", 40);
    if (count > 400) count = 400;
    char v[128];
    uint64_t start;
    if (query_get(q, "addr", v, sizeof v)) {
        if (!machine_parse_addr(m, v, &start)) start = machine_cpu(m)->rip;
    } else {
        /* Around RIP: start at the enclosing function when it is close, so
         * instruction boundaries are right; otherwise start at RIP. */
        uint64_t rip = machine_cpu(m)->rip;
        const Symbol *s = machine_find_symbol(m, rip);
        start = (s && rip - s->addr < 4096) ? s->addr : rip;
        int before = 0;
        uint64_t a = start;
        uint64_t hist[512];
        while (a < rip && before < 512) {
            Insn in;
            hist[before++] = a;
            a += machine_disasm(m, a, &in, NULL, 0);
        }
        if (a != rip) start = rip; /* not on an instruction boundary */
        else if (before > count / 3) start = hist[before - count / 3];
    }
    buf_printf(b, "{\"lines\":[");
    uint64_t a = start;
    for (int i = 0; i < count; i++) {
        int len;
        if (i) buf_append(b, ",", 1);
        disasm_line(m, b, a, &len);
        a += len;
    }
    buf_printf(b, "]}");
}

static void api_mem(Machine *m, const char *q, Buf *b) {
    char v[128];
    uint64_t addr = 0;
    if (query_get(q, "addr", v, sizeof v)) machine_parse_addr(m, v, &addr);
    uint64_t len = query_u64(q, "len", 256);
    if (len > 4096) len = 4096;
    uint64_t lin = m->bits == 16 && !query_u64(q, "linear", 0) ? machine_cpu(m)->sbase[DS] + (addr & 0xFFFF) : addr;
    buf_printf(b, "{\"addr\":");
    json_hex(b, addr);
    buf_printf(b, ",\"linear\":");
    json_hex(b, lin);
    buf_printf(b, ",\"bytes\":[");
    for (uint64_t i = 0; i < len; i++) {
        uint8_t x;
        int ok = machine_peek(m, lin + i, &x, 1) == 1;
        buf_printf(b, "%s%d", i ? "," : "", ok ? x : -1);
    }
    buf_printf(b, "]}");
}

static void api_stack(Machine *m, const char *q, Buf *b) {
    int count = (int)query_u64(q, "count", 24);
    if (count > 256) count = 256;
    int ws = m->bits == 64 ? 8 : m->bits == 32 ? 4 : 2;
    uint64_t sp = machine_cpu(m)->r[RSP] & (m->bits == 64 ? ~0ULL : m->bits == 32 ? 0xFFFFFFFFULL : 0xFFFF);
    uint64_t lin = machine_cpu(m)->sbase[SS] + sp;
    buf_printf(b, "{\"width\":%d,\"entries\":[", ws);
    for (int i = 0; i < count; i++) {
        uint64_t v = 0;
        char sym[160] = "";
        int ok = machine_peek(m, lin + (uint64_t)i * ws, &v, ws) == (size_t)ws;
        if (ok) machine_symname(m, v, sym, sizeof sym);
        buf_printf(b, "%s{\"addr\":", i ? "," : "");
        json_hex(b, sp + (uint64_t)i * ws);
        buf_printf(b, ",\"value\":");
        if (ok) json_hex(b, v);
        else buf_printf(b, "null");
        buf_printf(b, ",\"sym\":");
        json_str(b, sym);
        uint64_t rbp = machine_cpu(m)->r[RBP] & (m->bits == 64 ? ~0ULL : 0xFFFFFFFFULL);
        buf_printf(b, ",\"rbp\":%s}", sp + (uint64_t)i * ws == rbp ? "true" : "false");
    }
    buf_printf(b, "]}");
}

static void api_output(Machine *m, const char *q, Buf *b) {
    uint64_t from = query_u64(q, "from", 0);
    if (from > m->out.len) from = 0;
    buf_printf(b, "{\"len\":%zu,\"text\":", m->out.len);
    json_strn(b, m->out.data ? m->out.data + from : "", m->out.len - from);
    buf_printf(b, "}");
}

static void api_syscalls(Machine *m, Buf *b) {
    buf_printf(b, "{\"lines\":[");
    uint64_t start = m->syslog_count > SYSLOG_LEN ? m->syslog_count - SYSLOG_LEN : 0;
    for (uint64_t i = start; i < m->syslog_count; i++) {
        if (i > start) buf_append(b, ",", 1);
        json_str(b, m->syslog[i % SYSLOG_LEN] ? m->syslog[i % SYSLOG_LEN] : "");
    }
    buf_printf(b, "]}");
}

static void api_trace(Machine *m, const char *q, Buf *b) {
    uint64_t n = query_u64(q, "count", 64);
    if (n > TRACE_LEN) n = TRACE_LEN;
    if (n > m->trace_count) n = m->trace_count;
    buf_printf(b, "{\"lines\":[");
    for (uint64_t i = m->trace_count - n; i < m->trace_count; i++) {
        Insn in;
        char text[200], sym[160];
        uint64_t a = m->trace[i % TRACE_LEN];
        machine_disasm(m, a, &in, text, sizeof text);
        machine_symname(m, a, sym, sizeof sym);
        buf_printf(b, "%s{\"addr\":", i > m->trace_count - n ? "," : "");
        json_hex(b, a);
        buf_printf(b, ",\"text\":");
        json_str(b, text);
        buf_printf(b, ",\"sym\":");
        json_str(b, sym);
        buf_printf(b, "}");
    }
    buf_printf(b, "]}");
}

static void api_regions(Machine *m, Buf *b) {
    MemRegion *regs;
    int n = mem_regions(m->mem, &regs);
    buf_printf(b, "{\"regions\":[");
    for (int i = 0; i < n; i++) {
        buf_printf(b, "%s{\"start\":", i ? "," : "");
        json_hex(b, regs[i].start);
        buf_printf(b, ",\"end\":");
        json_hex(b, regs[i].end);
        buf_printf(b, ",\"prot\":\"%c%c%c\",\"name\":", regs[i].prot & MEM_R ? 'r' : '-',
                   regs[i].prot & MEM_W ? 'w' : '-', regs[i].prot & MEM_X ? 'x' : '-');
        json_str(b, regs[i].name);
        buf_printf(b, "}");
    }
    free(regs);
    buf_printf(b, "]}");
}

static void api_symbols(Machine *m, Buf *b) {
    buf_printf(b, "{\"symbols\":[");
    for (int i = 0; i < m->nsyms; i++) {
        buf_printf(b, "%s{\"name\":", i ? "," : "");
        json_str(b, m->syms[i].name);
        buf_printf(b, ",\"addr\":");
        json_hex(b, m->syms[i].addr);
        buf_printf(b, ",\"size\":%llu}", (unsigned long long)m->syms[i].size);
    }
    buf_printf(b, "]}");
}

static void json_cache_stats(Buf *b, Cache *c) {
    CacheStats *t = &c->st;
    int count[4] = {0};
    for (int i = 0; i < c->sets * c->ways; i++) count[c->lines[i].state]++;
    buf_printf(b, "{\"name\":\"%s\",\"sets\":%d,\"ways\":%d,\"reads\":%llu,\"writes\":%llu,\"fetches\":%llu,"
               "\"hits\":%llu,\"misses\":%llu,\"evictions\":%llu,\"writebacks\":%llu,\"snoops\":%llu,"
               "\"snoop_hits\":%llu,\"invalidations\":%llu,\"interventions\":%llu,\"upgrades\":%llu,"
               "\"lines\":{\"M\":%d,\"E\":%d,\"S\":%d,\"I\":%d},\"trans\":[",
               c->name, c->sets, c->ways, (unsigned long long)t->reads, (unsigned long long)t->writes,
               (unsigned long long)t->fetches, (unsigned long long)t->hits, (unsigned long long)t->misses,
               (unsigned long long)t->evictions, (unsigned long long)t->writebacks, (unsigned long long)t->snoops,
               (unsigned long long)t->snoop_hits, (unsigned long long)t->invalidations,
               (unsigned long long)t->interventions, (unsigned long long)t->upgrades, count[MESI_M], count[MESI_E],
               count[MESI_S], count[MESI_I]);
    for (int i = 0; i < 4; i++)
        buf_printf(b, "%s[%llu,%llu,%llu,%llu]", i ? "," : "", (unsigned long long)t->trans[i][0],
                   (unsigned long long)t->trans[i][1], (unsigned long long)t->trans[i][2],
                   (unsigned long long)t->trans[i][3]);
    buf_printf(b, "]}");
}

/* The contents of one cache (unit= core * 2 + kind, 4095 = L2): sets set= to
 * set + n - 1, each way as 0 (invalid) or [state, line address, LRU age];
 * occ: one character per set of the whole cache ('a' + valid ways, upper
 * case when one is dirty); sel=set,way adds that line's bytes next to the
 * bytes in memory (they differ where the copy is dirty). */
static void api_error(Buf *b, const char *msg);

static void api_cachelines(Machine *m, const char *q, Buf *b) {
    CacheSys *s = m->cache;
    if (!s) {
        buf_printf(b, "{\"enabled\":false}");
        return;
    }
    int unit = (int)query_u64(q, "unit", (uint64_t)m->view_cpu * 2);
    Cache *c = cache_unit(s, unit);
    if (!c) return api_error(b, "no such cache");
    uint64_t set0 = query_u64(q, "set", 0), n = query_u64(q, "n", 64);
    if (set0 >= (uint64_t)c->sets) set0 = 0;
    if (n > (uint64_t)c->sets - set0) n = (uint64_t)c->sets - set0;
    buf_printf(b, "{\"enabled\":true,\"unit\":%d,\"name\":\"%s\",\"sets\":%d,\"ways\":%d,\"set0\":%llu,\"rows\":[",
               unit, c->name, c->sets, c->ways, (unsigned long long)set0);
    for (uint64_t i = set0; i < set0 + n; i++) {
        CacheLine *set = c->lines + i * (uint64_t)c->ways;
        buf_append(b, i > set0 ? ",[" : "[", i > set0 ? 2 : 1);
        for (int w = 0; w < c->ways; w++) {
            CacheLine *l = &set[w];
            if (w) buf_append(b, ",", 1);
            if (l->state == MESI_I) buf_append(b, "0", 1);
            else buf_printf(b, "[%d,\"%llx\",%u]", l->state, (unsigned long long)(l->tag << 6), c->clock - l->lru);
        }
        buf_append(b, "]", 1);
    }
    buf_printf(b, "],\"occ\":\"");
    for (int i = 0; i < c->sets; i++) {
        int valid = 0, dirty = 0;
        for (int w = 0; w < c->ways; w++) {
            int st = c->lines[i * c->ways + w].state;
            valid += st != MESI_I;
            dirty |= st == MESI_M;
        }
        char ch = (char)((dirty ? 'A' : 'a') + valid);
        buf_append(b, &ch, 1);
    }
    buf_append(b, "\"", 1);
    char v[64];
    unsigned sset, sway;
    if (query_get(q, "sel", v, sizeof v) && sscanf(v, "%u,%u", &sset, &sway) == 2 && sset < (unsigned)c->sets &&
        sway < (unsigned)c->ways) {
        CacheLine *l = &c->lines[(uint64_t)sset * (uint64_t)c->ways + sway];
        buf_printf(b, ",\"sel\":{\"set\":%u,\"way\":%u,\"state\":%d", sset, sway, l->state);
        if (l->state != MESI_I) {
            uint8_t mem[CACHE_LINE];
            size_t got = mem_peek(s->mem, l->tag << 6, mem, CACHE_LINE);
            if (got < CACHE_LINE) memset(mem + got, 0xFF, CACHE_LINE - got);
            buf_printf(b, ",\"addr\":\"%llx\",\"age\":%u,\"data\":\"", (unsigned long long)(l->tag << 6),
                       c->clock - l->lru);
            for (int i = 0; i < CACHE_LINE; i++) buf_printf(b, "%02x", l->data[i]);
            buf_printf(b, "\",\"mem\":\"");
            for (int i = 0; i < CACHE_LINE; i++) buf_printf(b, "%02x", mem[i]);
            buf_append(b, "\"", 1);
        }
        buf_append(b, "}", 1);
    }
    buf_append(b, "}", 1);
}

/* Cache statistics, the states of one line (addr= physical) and recent coherence events. */
static void api_cache(Machine *m, const char *q, Buf *b) {
    CacheSys *s = m->cache;
    if (!s) {
        buf_printf(b, "{\"enabled\":false}");
        return;
    }
    buf_printf(b, "{\"enabled\":true,\"caches\":[");
    for (int i = 0; i < s->ncores; i++) {
        if (i) buf_append(b, ",", 1);
        json_cache_stats(b, &s->l1d[i]);
        buf_append(b, ",", 1);
        json_cache_stats(b, &s->l1i[i]);
    }
    buf_append(b, ",", 1);
    json_cache_stats(b, &s->l2);
    buf_printf(b, "],\"bus\":{\"BusRd\":%llu,\"BusRdX\":%llu,\"BusUpgr\":%llu,\"BusWB\":%llu,\"c2c\":%llu,"
               "\"mem_reads\":%llu,\"mem_writes\":%llu,\"uncached\":%llu}",
               (unsigned long long)s->bus.bus_rd, (unsigned long long)s->bus.bus_rdx,
               (unsigned long long)s->bus.bus_upgr, (unsigned long long)s->bus.bus_wb, (unsigned long long)s->bus.c2c,
               (unsigned long long)s->bus.mem_reads, (unsigned long long)s->bus.mem_writes,
               (unsigned long long)s->bus.uncached);
    char v[64];
    if (query_get(q, "addr", v, sizeof v) && v[0]) {
        uint64_t pa = strtoull(v, NULL, 16);
        uint8_t data[CACHE_LINE];
        machine_phys_peek(m, pa & ~(uint64_t)(CACHE_LINE - 1), data, CACHE_LINE);
        buf_printf(b, ",\"line\":{\"addr\":");
        json_hex(b, pa & ~(uint64_t)(CACHE_LINE - 1));
        buf_printf(b, ",\"cacheable\":%s,\"l1_set\":%llu,\"l2_set\":%llu,\"states\":[",
                   cache_cacheable(s, pa) ? "true" : "false", (unsigned long long)((pa >> 6) & 63),
                   (unsigned long long)((pa >> 6) & 4095));
        for (int u = 0; u < s->ncores * 2; u++)
            buf_printf(b, "%s[\"%s\",\"%c\"]", u ? "," : "", cache_unit_name(s, u),
                       cache_state_char[cache_unit_state(s, u, pa)]);
        int l2 = cache_unit_state(s, CACHE_UNIT_L2, pa);
        buf_printf(b, ",[\"L2\",\"%s\"]],\"data\":\"", l2 == MESI_M ? "dirty" : l2 ? "clean" : "I");
        for (int i = 0; i < CACHE_LINE; i++) buf_printf(b, "%02x", data[i]);
        buf_printf(b, "\"}");
    }
    int n = (int)query_u64(q, "log", 40);
    if (n > CACHE_LOG_LEN) n = CACHE_LOG_LEN;
    uint64_t first = s->nlog > (uint64_t)n ? s->nlog - (uint64_t)n : 0;
    buf_printf(b, ",\"events\":%llu,\"log\":[", (unsigned long long)s->nlog);
    for (uint64_t i = first; i < s->nlog; i++) {
        char line[256];
        cache_format_event(s, &s->log[i % CACHE_LOG_LEN], line, sizeof line);
        if (i > first) buf_append(b, ",", 1);
        json_str(b, line);
    }
    buf_printf(b, "]}");
}

static int set_register(Machine *m, const char *name, uint64_t v) {
    static const char *names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    for (int i = 0; i < 16; i++)
        if (!strcmp(name, names[i])) {
            machine_cpu(m)->r[i] = v;
            return 0;
        }
    if (!strcmp(name, "rip")) machine_cpu(m)->rip = v;
    else if (!strcmp(name, "rflags")) machine_cpu(m)->rflags = (v & 0x3F7FD7) | 2;
    else return -1;
    return 0;
}

static void api_error(Buf *b, const char *msg) {
    buf_printf(b, "{\"error\":");
    json_str(b, msg);
    buf_printf(b, "}");
}

/* ---- OS image library ---- */

/* Library files have flat names: letters, digits, '.', '-', '_', not starting with '.'. */
static int os_name_ok(const char *n) {
    if (!n[0] || n[0] == '.' || strlen(n) > 200) return 0;
    for (const char *p = n; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_') return 0;
    return 1;
}

/* What a file is, from its first bytes: a Linux bzImage, an initramfs (cpio,
 * possibly compressed), a bootable disk (boot signature; "floppy" when the size
 * is a standard floppy's) or something else. */
static const char *os_kind(const char *path, uint64_t size) {
    uint8_t h[0x210] = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return "other";
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n >= 0x206 && !memcmp(h + 0x202, "HdrS", 4)) return "kernel";
    static const struct { const char *magic; size_t len; } ird[] = {
        {"0707", 4}, {"\x1f\x8b", 2}, {"\xfd" "7zXZ", 5}, {"\x28\xb5\x2f\xfd", 4}, {"BZh", 3}, {"\x02\x21\x4c\x18", 4}, {"\x5d\0\0", 3}};
    for (size_t i = 0; i < sizeof ird / sizeof ird[0]; i++)
        if (n >= ird[i].len && !memcmp(h, ird[i].magic, ird[i].len)) return "initrd";
    if (n >= 512 && h[510] == 0x55 && h[511] == 0xAA) {
        static const uint64_t floppy[] = {368640, 737280, 1228800, 1474560, 2949120};
        for (size_t i = 0; i < sizeof floppy / sizeof floppy[0]; i++)
            if (size == floppy[i]) return "floppy";
        return "disk";
    }
    return "other";
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void api_os(Buf *b) {
    buf_printf(b, "{\"dir\":");
    json_str(b, os_dir);
    buf_printf(b, ",\"files\":[");
    DIR *d = opendir(os_dir);
    char *names[512];
    int n = 0;
    for (struct dirent *e; d && (e = readdir(d)) && n < 512;)
        if (os_name_ok(e->d_name)) names[n++] = strdup(e->d_name);
    if (d) closedir(d);
    qsort(names, (size_t)n, sizeof names[0], cmp_str);
    for (int i = 0, out = 0; i < n; i++) {
        char path[4400];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", os_dir, names[i]);
        if (!stat(path, &st) && S_ISREG(st.st_mode)) {
            buf_printf(b, "%s{\"name\":", out++ ? "," : "");
            json_str(b, names[i]);
            buf_printf(b, ",\"size\":%llu,\"kind\":\"%s\"}", (unsigned long long)st.st_size,
                       os_kind(path, (uint64_t)st.st_size));
        }
        free(names[i]);
    }
    buf_printf(b, "]}");
}

/* The library path of query parameter key, or NULL (absent or empty); *bad is
 * set for a name that is not a library file. */
static char *os_file(const char *q, const char *key, int *bad) {
    char v[256], path[4400];
    struct stat st;
    if (!query_get(q, key, v, sizeof v) || !v[0]) return NULL;
    snprintf(path, sizeof path, "%s/%s", os_dir, v);
    if (!os_name_ok(v) || stat(path, &st) || !S_ISREG(st.st_mode)) {
        *bad = 1;
        return NULL;
    }
    return strdup(path);
}

/* body: the file; name= its library name. Written beside the target, then renamed. */
static void api_upload(const char *q, const char *body, size_t len, Buf *b) {
    char name[256], tmp[4400], path[4400];
    if (!query_get(q, "name", name, sizeof name) || !os_name_ok(name))
        return api_error(b, "name= must be a plain file name (letters, digits, '.', '-', '_')");
    mkdir(os_dir, 0755);
    snprintf(path, sizeof path, "%s/%s", os_dir, name);
    snprintf(tmp, sizeof tmp, "%s/.upload-%s", os_dir, name);
    FILE *f = fopen(tmp, "wb");
    if (!f) return api_error(b, "cannot write to the OS library");
    int ok = fwrite(body, 1, len, f) == len;
    ok &= fclose(f) == 0;
    if (!ok || rename(tmp, path)) {
        unlink(tmp);
        return api_error(b, "cannot write to the OS library");
    }
    api_os(b);
}

static void handle_api(Machine *m, const char *method, const char *path, const char *q, const char *body,
                       size_t bodylen, Buf *b) {
    int post = !strcmp(method, "POST");
    char v[1024];
    if (!strcmp(path, "/api/state")) return api_state(m, b);
    if (!strcmp(path, "/api/disasm")) return api_disasm(m, q, b);
    if (!strcmp(path, "/api/mem")) return api_mem(m, q, b);
    if (!strcmp(path, "/api/stack")) return api_stack(m, q, b);
    if (!strcmp(path, "/api/output")) return api_output(m, q, b);
    if (!strcmp(path, "/api/syscalls")) return api_syscalls(m, b);
    if (!strcmp(path, "/api/trace")) return api_trace(m, q, b);
    if (!strcmp(path, "/api/regions")) return api_regions(m, b);
    if (!strcmp(path, "/api/symbols")) return api_symbols(m, b);
    if (!strcmp(path, "/api/screen")) return pc_screen(m, b);
    if (!strcmp(path, "/api/cache")) return api_cache(m, q, b);
    if (!strcmp(path, "/api/cachelines")) return api_cachelines(m, q, b);
    if (!strcmp(path, "/api/os")) return api_os(b);
    if (!post) return api_error(b, "unknown endpoint or method");

    if (!strcmp(path, "/api/step")) {
        uint64_t n = query_u64(q, "n", 1);
        for (uint64_t i = 0; i < n; i++)
            if (machine_step(m) != STOP_STEP) break;
    } else if (!strcmp(path, "/api/next")) {
        machine_step_over(m, RUN_CHUNK);
    } else if (!strcmp(path, "/api/finish")) {
        machine_step_out(m, RUN_CHUNK);
    } else if (!strcmp(path, "/api/continue")) {
        machine_run(m, query_u64(q, "max", RUN_CHUNK));
    } else if (!strcmp(path, "/api/reset")) {
        char err[512];
        if (machine_reset(m, err, sizeof err)) return api_error(b, err);
    } else if (!strcmp(path, "/api/load")) {
        char files[2048], args[1024] = "", bits[16] = "0", err[512];
        if (!query_get(q, "files", files, sizeof files) || !files[0]) return api_error(b, "files= is required");
        query_get(q, "args", args, sizeof args);
        query_get(q, "bits", bits, sizeof bits);
        char *fv[64], *av[65];
        int nf = 0, na = 0;
        for (char *t = strtok(files, ","); t && nf < 64; t = strtok(NULL, ",")) fv[nf++] = t;
        av[na++] = fv[0];
        for (char *t = strtok(args, " "); t && na < 65; t = strtok(NULL, " ")) av[na++] = t;
        Machine fresh;
        machine_init(&fresh);
        fresh.echo_output = 0;
        fresh.use_host_stdin = 0;
        fresh.pc_mode = m->pc_mode; /* keep the machine type (PC disks, kernel, RAM) */
        fresh.pc_ram_mb = m->pc_ram_mb;
        fresh.pc_hda = m->pc_hda;
        fresh.pc_fda = m->pc_fda;
        fresh.pc_kernel = m->pc_kernel;
        fresh.pc_initrd = m->pc_initrd;
        fresh.pc_append = m->pc_append;
        fresh.pc_boot = m->pc_boot;
        fresh.pc_smp = m->pc_smp;
        fresh.pc_cache = m->pc_cache;
        fresh.yield_on_idle = 1;
        if (machine_load(&fresh, nf, fv, na, av, atoi(bits), err, sizeof err)) {
            machine_destroy(&fresh);
            return api_error(b, err);
        }
        machine_destroy(m);
        *m = fresh;
        machine_rebind(m);
    } else if (!strcmp(path, "/api/newpc")) {
        /* an empty PC with smp= CPUs (cache=1: simulated caches) for the editor */
        char err[512];
        uint64_t smp = query_u64(q, "smp", (uint64_t)(m->pc ? m->ncpu : 1));
        if (smp < 1 || smp > MAX_CPUS) return api_error(b, "smp= must be 1 to 255");
        Machine fresh;
        machine_init(&fresh);
        fresh.echo_output = 0;
        fresh.use_host_stdin = 0;
        fresh.yield_on_idle = 1;
        fresh.pc_mode = 1;
        fresh.pc_ram_mb = m->pc_ram_mb;
        fresh.pc_smp = (int)smp;
        fresh.pc_cache = (int)query_u64(q, "cache", (uint64_t)m->pc_cache) != 0;
        char *av[1] = {"pc"};
        if (machine_load(&fresh, 0, NULL, 1, av, 0, err, sizeof err)) {
            machine_destroy(&fresh);
            return api_error(b, err);
        }
        machine_destroy(m);
        *m = fresh;
        machine_rebind(m);
    } else if (!strcmp(path, "/api/boot")) {
        /* a fresh PC booting an OS from the library: kernel= (initrd=, append=) or
         * hda=/fda= disk images (boot=a|c); smp=, cache=0|1, ram= MB */
        char err[512], app[1024] = "", drive[8] = "";
        int bad = 0;
        uint64_t smp = query_u64(q, "smp", (uint64_t)m->ncpu), ram = query_u64(q, "ram", m->pc_ram_mb);
        if (smp < 1 || smp > MAX_CPUS) return api_error(b, "smp= must be 1 to 255");
        if (ram && (ram < 16 || ram > 3072)) return api_error(b, "ram= must be 16 to 3072 MB");
        char *kernel = os_file(q, "kernel", &bad), *initrd = os_file(q, "initrd", &bad);
        char *hda = os_file(q, "hda", &bad), *fda = os_file(q, "fda", &bad);
        query_get(q, "append", app, sizeof app);
        query_get(q, "boot", drive, sizeof drive);
        const char *why = bad ? "no such file in the OS library" : !kernel && !hda && !fda ? "choose a kernel or a disk image"
                        : initrd && !kernel ? "an initrd needs a kernel" : NULL;
        if (!why) {
            Machine fresh;
            machine_init(&fresh);
            fresh.echo_output = 0;
            fresh.use_host_stdin = 0;
            fresh.yield_on_idle = 1;
            fresh.pc_mode = 1;
            fresh.pc_ram_mb = ram;
            fresh.pc_smp = (int)smp;
            fresh.pc_cache = query_u64(q, "cache", (uint64_t)m->pc_cache) != 0;
            fresh.pc_kernel = kernel;
            fresh.pc_initrd = initrd;
            fresh.pc_append = kernel && app[0] ? strdup(app) : NULL;
            fresh.pc_hda = hda;
            fresh.pc_fda = fda;
            fresh.pc_boot = drive[0] == 'a' || drive[0] == 'c' ? drive[0] : 0;
            char *av[1] = {"pc"};
            if (!machine_load(&fresh, 0, NULL, 1, av, 0, err, sizeof err)) {
                /* the strings stay with the machine (reset reloads them) */
                machine_destroy(m);
                *m = fresh;
                machine_rebind(m);
                api_state(m, b);
                return;
            }
            free(fresh.pc_append);
            machine_destroy(&fresh);
            why = err;
        }
        free(kernel), free(initrd), free(hda), free(fda);
        return api_error(b, why);
    } else if (!strcmp(path, "/api/upload")) {
        return api_upload(q, body, bodylen, b);
    } else if (!strcmp(path, "/api/asm")) {
        /* body: assembly source; bits=16|32|64, syntax=att|intel, cpu=N (-1: memory only), addr=hex;
         * check=1 only assembles */
        static char diag[16384];
        char bits[8] = "64", syntax[8] = "att", cpu[8] = "0", addr[32] = "";
        query_get(q, "bits", bits, sizeof bits);
        query_get(q, "syntax", syntax, sizeof syntax);
        query_get(q, "cpu", cpu, sizeof cpu);
        if (query_u64(q, "check", 0)) { /* assemble only */
            int bad = asm_check(body, bodylen, atoi(bits), !strcmp(syntax, "intel"), diag, sizeof diag);
            buf_printf(b, bad ? "{\"error\":\"assembly failed\",\"diagnostics\":" : "{\"ok\":true,\"warnings\":");
            json_str(b, diag);
            buf_append(b, "}", 1);
            return;
        }
        if (!query_get(q, "addr", addr, sizeof addr) || !addr[0]) return api_error(b, "addr= is required");
        AsmLoad info;
        if (asm_load(m, body, bodylen, atoi(bits), !strcmp(syntax, "intel"), atoi(cpu), strtoull(addr, NULL, 16), &info,
                     diag, sizeof diag)) {
            buf_printf(b, "{\"error\":\"%s\",\"diagnostics\":", strstr(diag, "program.s:") ? "assembly failed" : "load failed");
            json_str(b, diag);
            buf_append(b, "}", 1);
            return;
        }
        const Symbol *s = machine_find_symbol(m, info.bits == 16 ? info.entry - info.addr : info.entry);
        if (info.cpu >= 0)
            snprintf(m->status, sizeof m->status, "cpu%d: %llu bytes loaded at 0x%llx, %d-bit, ready to run", info.cpu,
                     (unsigned long long)info.size, (unsigned long long)info.addr, info.bits);
        else
            snprintf(m->status, sizeof m->status, "%llu bytes loaded at 0x%llx", (unsigned long long)info.size,
                     (unsigned long long)info.addr);
        buf_printf(b, "{\"asm\":{\"cpu\":%d,\"bits\":%d,\"size\":%llu,\"addr\":", info.cpu, info.bits,
                   (unsigned long long)info.size);
        json_hex(b, info.addr);
        buf_printf(b, ",\"entry\":");
        json_hex(b, info.entry);
        buf_printf(b, ",\"stack\":");
        json_hex(b, info.stack);
        buf_printf(b, ",\"entry_name\":");
        json_str(b, s && s->addr == (info.bits == 16 ? info.entry - info.addr : info.entry) ? s->name : "");
        buf_printf(b, ",\"warnings\":");
        json_str(b, diag);
        buf_printf(b, "},\"state\":");
        api_state(m, b);
        buf_append(b, "}", 1);
        return;
    } else if (!strcmp(path, "/api/break")) {
        uint64_t addr;
        if (!query_get(q, "addr", v, sizeof v) || !machine_parse_addr(m, v, &addr))
            return api_error(b, "bad breakpoint location");
        char action[16] = "toggle";
        query_get(q, "action", action, sizeof action);
        int has = machine_has_bp(m, addr);
        if (!strcmp(action, "del") || (!strcmp(action, "toggle") && has)) machine_del_bp(m, addr);
        else if (machine_add_bp(m, addr)) return api_error(b, "too many breakpoints");
    } else if (!strcmp(path, "/api/cpu")) {
        int n = (int)query_u64(q, "n", 0);
        if (n < 0 || n >= m->ncpu) return api_error(b, "no such CPU");
        m->view_cpu = n;
        m->bits = m->cpus[n]->bits;
    } else if (!strcmp(path, "/api/input")) {
        buf_append(&m->in, body, bodylen);
    } else if (!strcmp(path, "/api/setreg")) {
        char name[16];
        if (!query_get(q, "name", name, sizeof name) || !query_get(q, "value", v, sizeof v) ||
            set_register(m, name, strtoull(v, NULL, 16)))
            return api_error(b, "bad register or value");
    } else if (!strcmp(path, "/api/poke")) {
        uint64_t addr;
        if (!query_get(q, "addr", v, sizeof v) || !machine_parse_addr(m, v, &addr)) return api_error(b, "bad address");
        char hex[1024];
        if (!query_get(q, "bytes", hex, sizeof hex)) return api_error(b, "bytes= is required");
        uint8_t data[512];
        size_t n = 0;
        for (char *p = hex; p[0] && p[1] && n < sizeof data; p += 2) {
            while (*p == ' ') p++;
            char h[3] = {p[0], p[1], 0};
            data[n++] = (uint8_t)strtol(h, NULL, 16);
        }
        uint64_t lin = m->bits == 16 ? machine_cpu(m)->sbase[DS] + (addr & 0xFFFF) : addr;
        for (size_t i = 0; i < n; i++) {
            uint64_t pa = lin + i;
            if (machine_cpu(m)->sys && cpu_virt_to_phys(machine_cpu(m), lin + i, &pa)) return api_error(b, "address not mapped");
            if (machine_phys_poke(m, pa, data + i, 1) != 1) return api_error(b, "address not mapped");
        }
    } else {
        return api_error(b, "unknown endpoint");
    }
    api_state(m, b);
}

/* ---- HTTP ---- */

static const char *mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js")) return "text/javascript; charset=utf-8";
    if (!strcmp(dot, ".s")) return "text/plain; charset=utf-8";
    if (!strcmp(dot, ".css")) return "text/css; charset=utf-8";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    if (!strcmp(dot, ".png")) return "image/png";
    return "application/octet-stream";
}

static void send_all(int fd, const void *data, size_t n) {
    const char *p = data;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}

static void respond(int fd, int code, const char *type, const void *body, size_t n) {
    char hdr[256];
    const char *msg = code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request";
    int hl = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                      code, msg, type, n);
    send_all(fd, hdr, (size_t)hl);
    send_all(fd, body, n);
}

static void serve_file(int fd, const char *ui_dir, const char *path) {
    if (!strcmp(path, "/")) path = "/index.html";
    /* flat names, plus the editor's examples in asm/ */
    const char *name = !strncmp(path, "/asm/", 5) ? path + 5 : path + 1;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_') {
            respond(fd, 404, "text/plain", "not found", 9);
            return;
        }
    if (strstr(path, "..")) {
        respond(fd, 404, "text/plain", "not found", 9);
        return;
    }
    char full[4096];
    snprintf(full, sizeof full, "%s%s", ui_dir, path);
    FILE *f = fopen(full, "rb");
    if (!f) {
        respond(fd, 404, "text/plain", "not found", 9);
        return;
    }
    Buf b = {0};
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_append(&b, chunk, n);
    fclose(f);
    respond(fd, 200, mime_type(path), b.data ? b.data : "", b.len);
    buf_free(&b);
}

static void handle_conn(Machine *m, int fd, const char *ui_dir) {
    Buf req = {0};
    char chunk[8192];
    size_t hdr_end = 0, content_len = 0;
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        buf_append(&req, chunk, (size_t)n);
        if (!hdr_end) {
            char *e = strstr(req.data, "\r\n\r\n");
            if (e) {
                hdr_end = (size_t)(e - req.data) + 4;
                char *cl = strcasestr(req.data, "Content-Length:");
                if (cl && cl < e) content_len = strtoul(cl + 15, NULL, 10);
            }
        }
        if (hdr_end && req.len >= hdr_end + content_len) break;
        if (hdr_end ? content_len > MAX_UPLOAD : req.len > (1 << 20)) break;
    }
    if (!hdr_end || req.len < hdr_end + content_len) {
        if (hdr_end) respond(fd, 400, "text/plain", "request too large or incomplete", 31);
        buf_free(&req);
        return;
    }
    char method[8] = "", target[2048] = "";
    sscanf(req.data, "%7s %2047s", method, target);
    char *q = strchr(target, '?');
    const char *query = "";
    if (q) {
        *q = 0;
        query = q + 1;
    }
    if (!strncmp(target, "/api/", 5)) {
        Buf out = {0};
        handle_api(m, method, target, query, req.data + hdr_end, content_len, &out);
        respond(fd, 200, "application/json", out.data ? out.data : "{}", out.len);
        buf_free(&out);
    } else {
        serve_file(fd, ui_dir, target);
    }
    buf_free(&req);
}

int run_server(Machine *m, int port, const char *ui_dir) {
    signal(SIGPIPE, SIG_IGN);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("x86sim: bind");
        return 1;
    }
    listen(s, 16);
    char lib[4400];
    snprintf(lib, sizeof lib, "%s/../os", ui_dir);
    if (!realpath(lib, os_dir)) snprintf(os_dir, sizeof os_dir, "%s", lib);
    printf("x86sim web UI: http://127.0.0.1:%d/   (serving %s, Ctrl-C to quit)\n", port, ui_dir);
    fflush(stdout);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) continue;
        handle_conn(m, fd, ui_dir);
        close(fd);
    }
}
