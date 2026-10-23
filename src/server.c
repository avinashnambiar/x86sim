/* A tiny single-threaded HTTP server exposing the machine as a JSON API and
 * serving the web UI. Binds to 127.0.0.1 only. */
#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cache.h"
#include "machine.h"
#include "pc.h"

#define RUN_CHUNK 3000000ULL

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
        const char *mode = !(c->cr0 & CR0_PE) ? "real" : (c->rflags & FL_VM) ? "vm86"
                         : c->bits == 64 ? "long" : (c->efer & EFER_LMA) ? "compat" : "protected";
        buf_printf(b, ",\"sys\":{\"mode\":\"%s\",\"cpl\":%d,\"paging\":%s,\"time_ns\":%llu", mode, c->cpl,
                   (c->cr0 & CR0_PG) ? "true" : "false", (unsigned long long)pc_time_ns(m));
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
    buf_printf(b, "],\"syscalls\":%llu,\"view_cpu\":%d,\"cache\":%s,\"cpus\":[", (unsigned long long)m->syslog_count,
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
    for (const char *p = path + 1; *p; p++)
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
        if (req.len > (1 << 20)) break;
    }
    if (!hdr_end) {
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
    printf("x86sim web UI: http://127.0.0.1:%d/   (serving %s, Ctrl-C to quit)\n", port, ui_dir);
    fflush(stdout);
    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) continue;
        handle_conn(m, fd, ui_dir);
        close(fd);
    }
}
