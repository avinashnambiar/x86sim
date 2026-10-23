#include "machine.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- buffers ---- */

void buf_append(Buf *b, const void *data, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap) cap *= 2;
        b->data = realloc(b->data, cap);
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = 0;
}

void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) {
        buf_append(b, tmp, n);
        return;
    }
    char *big = malloc(n + 1);
    va_start(ap, fmt);
    vsnprintf(big, n + 1, fmt, ap);
    va_end(ap);
    buf_append(b, big, n);
    free(big);
}

void buf_free(Buf *b) {
    free(b->data);
    memset(b, 0, sizeof *b);
}

/* ---- lifecycle ---- */

void machine_init(Machine *m) {
    memset(m, 0, sizeof *m);
    m->echo_output = 1;
    m->use_host_stdin = 1;
    m->log_syscalls = 1;
}

static void free_program_state(Machine *m) {
    mem_free(m->mem);
    m->mem = NULL;
    for (int i = 0; i < m->nsyms; i++) free(m->syms[i].name);
    free(m->syms);
    m->syms = NULL;
    m->nsyms = 0;
    for (int i = 0; i < SYSLOG_LEN; i++) {
        free(m->syslog[i]);
        m->syslog[i] = NULL;
    }
    m->syslog_count = 0;
}

void machine_destroy(Machine *m) {
    free_program_state(m);
    buf_free(&m->out);
    buf_free(&m->in);
    for (int i = 0; i < m->nfiles; i++) free(m->files[i]);
    for (int i = 0; i < m->argc; i++) free(m->argv[i]);
    free(m->files);
    free(m->argv);
}

static char **dup_strings(int n, char **src) {
    char **out = calloc(n + 1, sizeof *out);
    for (int i = 0; i < n; i++) out[i] = strdup(src[i]);
    return out;
}

int machine_load(Machine *m, int nfiles, char **files, int argc, char **argv, int bits, char *err, size_t errn) {
    m->nfiles = nfiles;
    m->files = dup_strings(nfiles, files);
    m->argc = argc;
    m->argv = dup_strings(argc, argv);
    m->bits_override = bits;
    return machine_reset(m, err, errn);
}

int machine_reset(Machine *m, char *err, size_t errn) {
    free_program_state(m);
    m->mem = mem_new();
    m->out.len = 0;
    if (m->out.data) m->out.data[0] = 0;
    m->in.len = m->in_pos = 0;
    m->trace_count = 0;
    m->state = MS_READY;
    m->exit_code = 0;
    m->last_stop = STOP_NONE;
    snprintf(m->status, sizeof m->status, "ready");
    if (load_program(m, err, errn)) return -1;
    os_install(m);
    return 0;
}

/* ---- execution ---- */

const char *stop_reason_name(int r) {
    switch (r) {
    case STOP_NONE: return "none";
    case STOP_STEP: return "step";
    case STOP_BREAKPOINT: return "breakpoint";
    case STOP_TRAP: return "trap";
    case STOP_EXIT: return "exit";
    case STOP_FAULT: return "fault";
    case STOP_HALT: return "halt";
    case STOP_INPUT: return "input";
    case STOP_LIMIT: return "limit";
    }
    return "?";
}

static int blocked_reason(Machine *m) {
    switch (m->state) {
    case MS_EXITED: return STOP_EXIT;
    case MS_FAULT: return STOP_FAULT;
    case MS_HALTED: return STOP_HALT;
    case MS_WAIT_INPUT: return m->in_pos < m->in.len ? STOP_NONE : STOP_INPUT;
    }
    return STOP_NONE;
}

static int do_step(Machine *m) {
    CPU *c = &m->cpu;
    uint64_t rip = c->rip;
    if (m->trace_print) {
        Insn in;
        char text[160];
        machine_disasm(m, rip, &in, text, sizeof text);
        fprintf(stderr, "%016llx  %s\n", (unsigned long long)rip, text);
    }
    m->trace[m->trace_count++ % TRACE_LEN] = rip;
    if (m->state == MS_WAIT_INPUT) m->state = MS_READY;
    m->last_stop = STOP_NONE;

    StepResult r = cpu_step(c);
    if (r == STEP_OK) return STOP_NONE;
    if (r == STEP_STOP) {
        if (m->last_stop == STOP_INPUT) {
            /* The read will be retried once input arrives. */
            c->icount--;
            m->trace_count--;
            m->state = MS_WAIT_INPUT;
            snprintf(m->status, sizeof m->status, "waiting for input");
        }
        return m->last_stop;
    }
    if (c->exc_vector == EXC_BP) {
        snprintf(m->status, sizeof m->status, "int3 breakpoint at 0x%llx", (unsigned long long)rip);
        return STOP_TRAP;
    }
    if (c->exc_vector == -1) {
        m->state = MS_HALTED;
        snprintf(m->status, sizeof m->status, "halted at 0x%llx", (unsigned long long)rip);
        return STOP_HALT;
    }
    m->state = MS_FAULT;
    char where[160];
    machine_symname(m, rip, where, sizeof where);
    snprintf(m->status, sizeof m->status, "%s at 0x%llx%s%s%s", c->exc_msg, (unsigned long long)rip,
             where[0] ? " <" : "", where, where[0] ? ">" : "");
    return STOP_FAULT;
}

int machine_step(Machine *m) {
    int b = blocked_reason(m);
    if (b) return m->last_stop = b;
    int r = do_step(m);
    return m->last_stop = r ? r : STOP_STEP;
}

int machine_run(Machine *m, uint64_t max) {
    int b = blocked_reason(m);
    if (b) return m->last_stop = b;
    for (uint64_t i = 0; i < max; i++) {
        if (i > 0 && m->nbp && machine_has_bp(m, m->cpu.rip)) {
            snprintf(m->status, sizeof m->status, "breakpoint at 0x%llx", (unsigned long long)m->cpu.rip);
            return m->last_stop = STOP_BREAKPOINT;
        }
        int r = do_step(m);
        if (r) return m->last_stop = r;
    }
    return m->last_stop = STOP_LIMIT;
}

int machine_step_over(Machine *m, uint64_t max) {
    int b = blocked_reason(m);
    if (b) return m->last_stop = b;
    Insn in;
    machine_disasm(m, m->cpu.rip, &in, NULL, 0);
    if (in.id != I_CALL) return machine_step(m);
    uint64_t target = m->cpu.rip + in.len, sp0 = m->cpu.r[RSP];
    for (uint64_t i = 0; i < max; i++) {
        if (i > 0 && m->nbp && machine_has_bp(m, m->cpu.rip)) {
            snprintf(m->status, sizeof m->status, "breakpoint at 0x%llx", (unsigned long long)m->cpu.rip);
            return m->last_stop = STOP_BREAKPOINT;
        }
        int r = do_step(m);
        if (r) return m->last_stop = r;
        if (m->cpu.rip == target && m->cpu.r[RSP] >= sp0) return m->last_stop = STOP_STEP;
    }
    return m->last_stop = STOP_LIMIT;
}

int machine_step_out(Machine *m, uint64_t max) {
    int b = blocked_reason(m);
    if (b) return m->last_stop = b;
    uint64_t sp0 = m->cpu.r[RSP];
    for (uint64_t i = 0; i < max; i++) {
        if (i > 0 && m->nbp && machine_has_bp(m, m->cpu.rip)) {
            snprintf(m->status, sizeof m->status, "breakpoint at 0x%llx", (unsigned long long)m->cpu.rip);
            return m->last_stop = STOP_BREAKPOINT;
        }
        Insn in;
        machine_disasm(m, m->cpu.rip, &in, NULL, 0);
        int is_ret = in.id == I_RET && m->cpu.r[RSP] >= sp0;
        int r = do_step(m);
        if (r) return m->last_stop = r;
        if (is_ret) return m->last_stop = STOP_STEP;
    }
    return m->last_stop = STOP_LIMIT;
}

/* ---- breakpoints ---- */

int machine_has_bp(Machine *m, uint64_t addr) {
    for (int i = 0; i < m->nbp; i++)
        if (m->bp[i] == addr) return 1;
    return 0;
}

int machine_add_bp(Machine *m, uint64_t addr) {
    if (machine_has_bp(m, addr)) return 0;
    if (m->nbp == MAX_BP) return -1;
    m->bp[m->nbp++] = addr;
    return 0;
}

int machine_del_bp(Machine *m, uint64_t addr) {
    for (int i = 0; i < m->nbp; i++)
        if (m->bp[i] == addr) {
            m->bp[i] = m->bp[--m->nbp];
            return 0;
        }
    return -1;
}

/* ---- symbols ---- */

const Symbol *machine_find_symbol(Machine *m, uint64_t addr) {
    int lo = 0, hi = m->nsyms - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m->syms[mid].addr <= addr) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0) return NULL;
    const Symbol *s = &m->syms[best];
    if (s->size ? addr >= s->addr + s->size : addr - s->addr > 0x10000) return NULL;
    return s;
}

int machine_lookup_symbol(Machine *m, const char *name, uint64_t *addr) {
    for (int i = 0; i < m->nsyms; i++)
        if (!strcmp(m->syms[i].name, name)) {
            *addr = m->syms[i].addr;
            return 1;
        }
    return 0;
}

int machine_symname(void *ctx, uint64_t addr, char *buf, size_t n) {
    Machine *m = ctx;
    const Symbol *s = machine_find_symbol(m, addr);
    if (!s) {
        if (n) buf[0] = 0;
        return 0;
    }
    if (addr == s->addr) snprintf(buf, n, "%s", s->name);
    else snprintf(buf, n, "%s+0x%llx", s->name, (unsigned long long)(addr - s->addr));
    return 1;
}

int machine_disasm(Machine *m, uint64_t addr, Insn *in, char *text, size_t n) {
    uint8_t buf[15];
    uint64_t lin = (m->cpu.sbase[CS] + addr) & (m->cpu.bits == 64 ? ~0ULL : 0xFFFFFFFFULL);
    size_t got = mem_peek(m->mem, lin, buf, sizeof buf);
    int r = decode(buf, (int)got, addr, m->cpu.bits, in);
    if (r != DEC_OK) {
        int len = in->len ? in->len : 1;
        memset(in, 0, sizeof *in);
        in->addr = addr;
        in->len = (uint8_t)len;
        memcpy(in->bytes, buf, got < (size_t)len ? got : (size_t)len);
        if (text && n) snprintf(text, n, "%s", got ? "(bad)" : "(unmapped)");
        return len;
    }
    if (text && n) disasm(in, text, n, machine_symname, m);
    return in->len;
}

int machine_parse_addr(Machine *m, const char *s, uint64_t *addr) {
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return 0;
    char name[128];
    const char *plus = strchr(s, '+');
    uint64_t off = 0;
    if (plus) {
        size_t len = (size_t)(plus - s);
        if (len >= sizeof name) return 0;
        memcpy(name, s, len);
        name[len] = 0;
        off = strtoull(plus + 1, NULL, 0);
    } else {
        snprintf(name, sizeof name, "%s", s);
    }
    size_t l = strlen(name);
    while (l && isspace((unsigned char)name[l - 1])) name[--l] = 0;
    if (machine_lookup_symbol(m, name, addr)) {
        *addr += off;
        return 1;
    }
    char *end;
    uint64_t v = strtoull(name, &end, 16);
    if (end == name || *end) return 0;
    *addr = v + off;
    return 1;
}

/* ---- guest I/O and logging ---- */

void machine_output(Machine *m, const void *data, size_t n) {
    buf_append(&m->out, data, n);
    if (m->echo_output) {
        fwrite(data, 1, n, stdout);
        fflush(stdout);
    }
}

void machine_log(Machine *m, const char *fmt, ...) {
    if (!m->log_syscalls) return;
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (m->print_syscalls) fprintf(stderr, "[sys] %s\n", tmp);
    int slot = (int)(m->syslog_count++ % SYSLOG_LEN);
    free(m->syslog[slot]);
    m->syslog[slot] = strdup(tmp);
}
