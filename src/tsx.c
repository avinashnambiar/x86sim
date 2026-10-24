/* Restricted transactional memory (RTM): XBEGIN, XEND, XABORT, XTEST.
 *
 * A behavioral model at cache-line granularity. Inside a transaction the
 * CPU's stores go to a private write buffer and its loads are recorded in
 * a read set; XEND commits the buffer to memory in one step. Another CPU
 * aborts the transaction by writing a line it read or wrote, or by reading
 * a line it wrote (the requester wins, as on Intel parts). Exceptions,
 * interrupts, single-stepping, more than TX_LINES lines (capacity), and
 * instructions that cannot run transactionally (I/O, CPUID, HLT, system
 * instructions...) abort as well. An abort discards the buffer, restores
 * the registers saved by the outermost XBEGIN, puts the status in EAX and
 * continues at the XBEGIN fallback address. Nested transactions flatten
 * into the outermost one, up to 7 levels.
 *
 * Page walks and accessed/dirty updates are not transactional, as on real
 * hardware. Only system mode (the PC machine) has RTM; user-mode emulation
 * reports it absent. */
#include "cpu_internal.h"
#include <stdlib.h>
#include <string.h>

#define TX_LINES 512 /* lines read or written before a capacity abort */
#define TX_MAX_DEPTH 7

typedef struct {
    uint64_t line, wmask; /* line number; bytes written */
    uint8_t r, w;
    uint8_t data[64];
} TxLine;

struct Tx {
    int depth;
    uint32_t doom; /* nonzero: another CPU conflicted; abort with this status */
    uint64_t fallback;
    /* registers at the outermost XBEGIN */
    uint64_t r[16], rflags, sbase[6], ssp;
    uint16_t sreg[6], sattr[6];
    uint32_t slimit[6], mxcsr, pkru;
    Vec512 zmm[32];
    FPU fpu;
    int n;
    TxLine l[TX_LINES];
};

int tx_active(const CPU *c) { return c->tx && c->tx->depth; }
uint32_t tx_doomed(const CPU *c) { return tx_active(c) ? c->tx->doom : 0; }

void tx_reset(CPU *c) {
    if (tx_active(c) && c->tx_live) (*c->tx_live)--;
    if (c->tx) c->tx->depth = 0;
}

static TxLine *find(struct Tx *t, uint64_t line) {
    for (int i = 0; i < t->n; i++)
        if (t->l[i].line == line) return &t->l[i];
    return NULL;
}

/* Abort the transaction: restore the registers and go to the fallback. */
void tx_abort(CPU *c, uint32_t status) {
    struct Tx *t = c->tx;
    if (!tx_active(c)) return;
    if (t->depth > 1) status |= 0x20; /* aborted inside a nested transaction */
    memcpy(c->r, t->r, sizeof c->r);
    c->rflags = t->rflags;
    memcpy(c->sreg, t->sreg, sizeof c->sreg);
    memcpy(c->sbase, t->sbase, sizeof c->sbase);
    memcpy(c->slimit, t->slimit, sizeof c->slimit);
    memcpy(c->sattr, t->sattr, sizeof c->sattr);
    memcpy(c->zmm, t->zmm, sizeof c->zmm);
    c->mxcsr = t->mxcsr;
    c->fpu = t->fpu;
    c->ssp = t->ssp;
    c->pkru = t->pkru;
    t->depth = 0;
    t->n = 0;
    (*c->tx_live)--;
    c->rip = t->fallback;
    setreg(c, RAX, 4, 1, status);
}

/* Abort from inside an instruction: unwind to sys_step without delivering
 * anything. */
static _Noreturn void abort_now(CPU *c, uint32_t status) {
    tx_abort(c, status);
    c->tx_jump = 1;
    _longjmp(c->jb, 1);
}

void tx_begin(CPU *c, const Insn *in) {
    if (!c->tx_live) raise_exc(c, EXC_UD, 0, "#UD xbegin: no RTM in user-mode emulation");
    if (!c->tx) c->tx = calloc(1, sizeof *c->tx);
    struct Tx *t = c->tx;
    if (t->depth) {
        if (t->depth == TX_MAX_DEPTH) abort_now(c, 0);
        t->depth++;
        return;
    }
    memcpy(t->r, c->r, sizeof t->r);
    t->rflags = c->rflags;
    memcpy(t->sreg, c->sreg, sizeof t->sreg);
    memcpy(t->sbase, c->sbase, sizeof t->sbase);
    memcpy(t->slimit, c->slimit, sizeof t->slimit);
    memcpy(t->sattr, c->sattr, sizeof t->sattr);
    memcpy(t->zmm, c->zmm, sizeof t->zmm);
    t->mxcsr = c->mxcsr;
    t->fpu = c->fpu;
    t->ssp = c->ssp;
    t->pkru = c->pkru;
    t->fallback = in->target;
    t->n = 0;
    t->doom = 0;
    t->depth = 1;
    (*c->tx_live)++;
}

void tx_end(CPU *c) {
    if (!c->tx_live) raise_exc(c, EXC_UD, 0, "#UD xend: no RTM in user-mode emulation");
    if (!tx_active(c)) raise_exc(c, EXC_GP, 0, "#GP xend outside a transaction");
    struct Tx *t = c->tx;
    if (--t->depth) return;
    (*c->tx_live)--;
    /* commit: ordinary stores now, so caches, other transactions and the
     * decoded-instruction cache all see them */
    for (int i = 0; i < t->n; i++) {
        TxLine *l = &t->l[i];
        for (int b = 0; b < 64;) {
            if (!(l->wmask >> b & 1)) {
                b++;
                continue;
            }
            int e = b;
            while (e < 64 && (l->wmask >> e & 1)) e++;
            phys_write(c, l->line * 64 + (uint64_t)b, l->data + b, e - b);
            b = e;
        }
    }
    t->n = 0;
}

void tx_xabort(CPU *c, uint8_t imm) {
    if (!c->tx_live) raise_exc(c, EXC_UD, 0, "#UD xabort: no RTM in user-mode emulation");
    if (tx_active(c)) abort_now(c, (uint32_t)imm << 24 | 1);
}

void tx_xtest(CPU *c) {
    if (!c->tx_live) raise_exc(c, EXC_UD, 0, "#UD xtest: no RTM in user-mode emulation");
    c->rflags &= ~(uint64_t)(FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF);
    if (!tx_active(c)) c->rflags |= FL_ZF;
}

/* Instructions that always abort a transaction. */
int tx_forbidden(const Insn *in) {
    switch (in->id) {
    case I_CPUID: case I_HLT: case I_IN: case I_OUT: case I_INS: case I_OUTS:
    case I_INT: case I_INT3: case I_INT1: case I_INTO: case I_SYSCALL: case I_SYSRET: case I_SYSENTER: case I_SYSEXIT:
    case I_IRET: case I_CLI: case I_STI: case I_POPF: case I_MOVCR: case I_MOVDR: case I_WRMSR: case I_RDMSR:
    case I_LGDT: case I_LIDT: case I_LLDT: case I_LTR: case I_LMSW: case I_CLTS: case I_INVLPG: case I_INVPCID:
    case I_INVD: case I_WBINVD: case I_RSM: case I_XSAVE: case I_XSAVEOPT: case I_XRSTOR:
        return 1;
    case I_GRP7R: /* XSETBV, SWAPGS, MONITOR/MWAIT, WRPKRU, VMX instructions */
        return in->modrm != 0xD5 && in->modrm != 0xD6 && in->modrm != 0xD0 && in->modrm != 0xF9 && in->modrm != 0xEE &&
               in->modrm != 0xCA && in->modrm != 0xCB;
    }
    return 0;
}

static void conflicts(CPU *c, uint64_t line, int write) {
    for (int i = 0; i < c->ncpu; i++) {
        CPU *p = c->peers[i];
        if (!p || p == c || !tx_active(p) || p->tx->doom) continue;
        TxLine *l = find(p->tx, line);
        if (l && (write || l->w)) p->tx->doom = 0x6; /* retry, conflict */
    }
}

static TxLine *track(CPU *c, uint64_t line) {
    struct Tx *t = c->tx;
    TxLine *l = find(t, line);
    if (l) return l;
    if (t->n == TX_LINES) abort_now(c, 0x8); /* capacity */
    l = &t->l[t->n++];
    memset(l, 0, sizeof *l);
    l->line = line;
    return l;
}

/* A load of n bytes at pa already in buf: check other transactions; inside
 * a transaction, record the lines and overlay our own buffered stores. */
void tx_read(CPU *c, uint64_t pa, uint8_t *buf, int n) {
    for (uint64_t a = pa; a < pa + (uint64_t)n;) {
        uint64_t line = a >> 6, end = (line + 1) * 64 < pa + (uint64_t)n ? (line + 1) * 64 : pa + (uint64_t)n;
        conflicts(c, line, 0);
        if (tx_active(c)) {
            TxLine *l = track(c, line);
            l->r = 1;
            for (uint64_t b = a; b < end; b++)
                if (l->wmask >> (b & 63) & 1) buf[b - pa] = l->data[b & 63];
        }
        a = end;
    }
}

/* A store: check other transactions; inside a transaction, buffer it and
 * return 1 (memory is not written). */
int tx_write(CPU *c, uint64_t pa, const uint8_t *buf, int n) {
    int mine = tx_active(c);
    for (uint64_t a = pa; a < pa + (uint64_t)n;) {
        uint64_t line = a >> 6, end = (line + 1) * 64 < pa + (uint64_t)n ? (line + 1) * 64 : pa + (uint64_t)n;
        conflicts(c, line, 1);
        if (mine) {
            TxLine *l = track(c, line);
            l->w = 1;
            for (uint64_t b = a; b < end; b++) {
                l->data[b & 63] = buf[b - pa];
                l->wmask |= 1ULL << (b & 63);
            }
        }
        a = end;
    }
    return mine;
}

void tx_free(CPU *c) {
    free(c->tx);
    c->tx = NULL;
}
