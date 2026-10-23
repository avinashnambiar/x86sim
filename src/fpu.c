/* x87 FPU execution unit (opcodes D8-DF), the MMX register alias, and the
 * FXSAVE/FXRSTOR and XSAVE family state images. Arithmetic is in softfloat.c.
 *
 * Exceptions follow the x87 model: each instruction accumulates flags in
 * FSW; an unmasked one sets ES/B and is delivered as #MF by the next
 * waiting FP instruction. Unmasked #IA/#Z/#D leave the destination and the
 * stack unchanged. Stack overflow/underflow raise #IS (IE+SF, C1 = overflow). */
#include <string.h>

#include "cpu_internal.h"

enum {
    SW_SF = 0x40, SW_ES = 0x80, SW_C0 = 0x100, SW_C1 = 0x200, SW_C2 = 0x400, SW_C3 = 0x4000, SW_B = 0x8000,
    SW_CC = SW_C0 | SW_C1 | SW_C2 | SW_C3
};

#define MXCSR_MASK 0x0000FFFFu

static int top(CPU *c) { return (c->fpu.fsw >> 11) & 7; }
static void set_top(CPU *c, int t) { c->fpu.fsw = (uint16_t)((c->fpu.fsw & ~0x3800) | ((t & 7) << 11)); }
static int ph(CPU *c, int i) { return (top(c) + i) & 7; }
static int empty(CPU *c, int i) { return !((c->fpu.ftw >> ph(c, i)) & 1); }
static fx80 st(CPU *c, int i) { return c->fpu.st[ph(c, i)]; }

static void set_st(CPU *c, int i, fx80 v) {
    int p = ph(c, i);
    c->fpu.st[p] = v;
    c->fpu.ftw |= (uint8_t)(1 << p);
}

static void pop(CPU *c) {
    c->fpu.ftw &= (uint8_t)~(1 << ph(c, 0));
    set_top(c, top(c) + 1);
}

static void push(CPU *c, fx80 v) {
    set_top(c, top(c) - 1);
    set_st(c, 0, v);
}

static void set_cc(CPU *c, unsigned cc) { c->fpu.fsw = (uint16_t)((c->fpu.fsw & ~SW_CC) | cc); }

static FxEnv env(CPU *c) {
    static const int prec[4] = {24, 64, 53, 64}; /* PC=01 is reserved: behaves as extended */
    FxEnv e;
    memset(&e, 0, sizeof e);
    e.rc = (c->fpu.fcw >> 10) & 3;
    e.prec = prec[(c->fpu.fcw >> 8) & 3];
    return e;
}

static void update_es(CPU *c) {
    if (c->fpu.fsw & ~c->fpu.fcw & 0x3F) c->fpu.fsw |= SW_ES | SW_B;
    else c->fpu.fsw &= (uint16_t)~(SW_ES | SW_B);
}

/* Merge an operation's exception flags into FSW. Returns 1 if the result
 * may be stored (no unmasked invalid, zero-divide or denormal exception).
 * With set_c1, C1 reports whether the result was rounded up. */
static int commit(CPU *c, FxEnv *e, int set_c1) {
    unsigned f = e->flags & 0x3F;
    c->fpu.fsw |= (uint16_t)f;
    if (set_c1) {
        c->fpu.fsw &= (uint16_t)~SW_C1;
        if ((f & FX_PE) && e->roundup) c->fpu.fsw |= SW_C1;
    }
    unsigned unmasked = f & ~c->fpu.fcw & 0x3F;
    if (unmasked) c->fpu.fsw |= SW_ES | SW_B;
    return !(unmasked & (FX_IE | FX_ZE | FX_DE));
}

/* Stack overflow/underflow. Returns 1 if masked: the caller then proceeds
 * with the QNaN indefinite as the result. */
static int stack_fault(CPU *c, int overflow) {
    c->fpu.fsw |= FX_IE | SW_SF;
    if (overflow) c->fpu.fsw |= SW_C1;
    else c->fpu.fsw &= (uint16_t)~SW_C1;
    if (!(c->fpu.fcw & FX_IE)) {
        c->fpu.fsw |= SW_ES | SW_B;
        return 0;
    }
    return 1;
}

static int realmode(CPU *c) { return c->bits == 16; }

void fpu_check_pending(CPU *c) {
    if (c->fpu.fsw & SW_ES) raise_exc(c, EXC_MF, 0, NULL);
}

static void fninit(CPU *c) {
    c->fpu.fcw = 0x037F;
    c->fpu.fsw = 0;
    c->fpu.ftw = 0;
    c->fpu.fop = 0;
    c->fpu.fip = c->fpu.fdp = 0;
    c->fpu.fcs = c->fpu.fds = 0;
}

/* ---- MMX alias -------------------------------------------------------------- */

void mmx_enter(CPU *c) {
    fpu_check_pending(c);
    set_top(c, 0);
    c->fpu.ftw = 0xFF;
}

uint64_t mmx_get(CPU *c, int i) { return c->fpu.st[i & 7].sig; }

void mmx_set(CPU *c, int i, uint64_t v) {
    c->fpu.st[i & 7].sig = v;
    c->fpu.st[i & 7].se = 0xFFFF;
}

void mmx_emms(CPU *c) {
    fpu_check_pending(c);
    c->fpu.ftw = 0;
}

/* ---- memory operands ---------------------------------------------------------- */

static fx80 rd80(CPU *c, uint64_t a) {
    fx80 v;
    v.sig = rdm(c, a, 8);
    v.se = (uint16_t)rdm(c, a + 8, 2);
    return v;
}

static void wr80(CPU *c, uint64_t a, fx80 v) {
    wrm(c, a + 8, 2, v.se);
    wrm(c, a, 8, v.sig);
}

/* Load the memory operand of a D8/DA/DC/DE arithmetic instruction. */
static fx80 load_arith_operand(CPU *c, FxEnv *e, int op, uint64_t a) {
    switch (op) {
    case 0: return fx_from_f32(e, (uint32_t)rdm(c, a, 4));
    case 2: return fx_from_i64((int32_t)rdm(c, a, 4));
    case 4: return fx_from_f64(e, rdm(c, a, 8));
    default: return fx_from_i64((int16_t)rdm(c, a, 2));
    }
}

/* ---- arithmetic helpers ---------------------------------------------------------- */

/* op: 0 add, 1 mul, 4 a-b, 5 b-a, 6 a/b, 7 b/a */
static fx80 arith(FxEnv *e, int op, fx80 a, fx80 b) {
    switch (op) {
    case 0: return fx_add(e, a, b);
    case 1: return fx_mul(e, a, b);
    case 4: return fx_sub(e, a, b);
    case 5: return fx_sub(e, b, a);
    case 6: return fx_div(e, a, b);
    default: return fx_div(e, b, a);
    }
}

static unsigned cmp_cc(int r) {
    switch (r) {
    case CMP_LT: return SW_C0;
    case CMP_EQ: return SW_C3;
    case CMP_UN: return SW_C3 | SW_C2 | SW_C0;
    }
    return 0;
}

/* FCOM/FUCOM family: compare ST(0) with b. Returns 0 if aborted. */
static int fcom(CPU *c, fx80 b, int b_empty, int quiet, int to_eflags) {
    FxEnv e = env(c);
    int r;
    if (empty(c, 0) || b_empty) {
        if (!stack_fault(c, 0)) return 0;
        r = CMP_UN;
    } else {
        r = fx_compare(&e, st(c, 0), b, quiet);
        if (!commit(c, &e, 0)) return 0;
    }
    if (to_eflags) {
        uint64_t f = r == CMP_LT ? FL_CF : r == CMP_EQ ? FL_ZF : r == CMP_UN ? (FL_ZF | FL_PF | FL_CF) : 0;
        c->rflags = (c->rflags & ~FL_ARITH) | f;
        c->fpu.fsw &= (uint16_t)~SW_C1;
    } else {
        set_cc(c, cmp_cc(r));
    }
    return 1;
}

/* Unary operation on ST(0) replacing it. */
typedef fx80 (*UnFn)(FxEnv *, fx80);

static void unary(CPU *c, UnFn fn) {
    FxEnv e = env(c);
    fx80 r;
    if (empty(c, 0)) {
        if (!stack_fault(c, 0)) return;
        r = fx_indefinite();
    } else {
        r = fn(&e, st(c, 0));
        if (!commit(c, &e, 1)) return;
    }
    set_st(c, 0, r);
}

/* Binary operation ST(1) = fn(ST(1), ST(0)), then pop (FYL2X, FYL2XP1, FPATAN). */
typedef fx80 (*BinFn)(FxEnv *, fx80, fx80);

static void binary_pop(CPU *c, BinFn fn) {
    FxEnv e = env(c);
    fx80 r;
    if (empty(c, 0) || empty(c, 1)) {
        if (!stack_fault(c, 0)) return;
        r = fx_indefinite();
    } else {
        r = fn(&e, st(c, 1), st(c, 0));
        if (!commit(c, &e, 1)) return;
    }
    set_st(c, 1, r);
    pop(c);
}

static fx80 f_sqrt(FxEnv *e, fx80 a) { return fx_sqrt(e, a); }
static fx80 f_rndint(FxEnv *e, fx80 a) { return fx_round_int(e, a); }
static fx80 f_2xm1(FxEnv *e, fx80 a) { return fx_2xm1(e, a); }

static int push_ok(CPU *c) {
    /* the register that becomes ST(0) must be empty */
    if (!((c->fpu.ftw >> ((top(c) - 1) & 7)) & 1)) return 1;
    return 0;
}

/* Push with stack-overflow handling. */
static void fpush(CPU *c, fx80 v) {
    if (!push_ok(c)) {
        if (!stack_fault(c, 1)) return;
        v = fx_indefinite();
    }
    push(c, v);
}

static int cond_fcmov(CPU *c, int op, int sub) {
    uint64_t f = c->rflags;
    int r;
    switch (sub) {
    case 0: r = (f & FL_CF) != 0; break;
    case 1: r = (f & FL_ZF) != 0; break;
    case 2: r = (f & (FL_CF | FL_ZF)) != 0; break;
    default: r = (f & FL_PF) != 0; break;
    }
    return op == 2 ? r : !r;
}

static unsigned fxam(CPU *c) {
    fx80 v = st(c, 0);
    unsigned cc = (v.se & 0x8000) ? SW_C1 : 0;
    if (empty(c, 0)) return cc | SW_C3 | SW_C0;
    switch (fx_class(v)) {
    case FXC_UNSUPPORTED: return cc;
    case FXC_QNAN: case FXC_SNAN: return cc | SW_C0;
    case FXC_NORMAL: return cc | SW_C2;
    case FXC_INF: return cc | SW_C2 | SW_C0;
    case FXC_ZERO: return cc | SW_C3;
    default: return cc | SW_C3 | SW_C2;
    }
}

/* ---- environment images ------------------------------------------------------------ */

static uint16_t full_tag(CPU *c) {
    uint16_t t = 0;
    for (int p = 0; p < 8; p++) {
        int tag;
        if (!((c->fpu.ftw >> p) & 1)) tag = 3;
        else {
            int cl = fx_class(c->fpu.st[p]);
            tag = cl == FXC_ZERO ? 1 : cl == FXC_NORMAL ? 0 : 2;
        }
        t |= (uint16_t)(tag << (2 * p));
    }
    return t;
}

static uint8_t abridge(uint16_t tag) {
    uint8_t r = 0;
    for (int p = 0; p < 8; p++)
        if (((tag >> (2 * p)) & 3) != 3) r |= (uint8_t)(1 << p);
    return r;
}

static int env_size(const Insn *in) { return in->opsize == 2 ? 14 : 28; }

static void store_env(CPU *c, const Insn *in, uint64_t a) {
    FPU *f = &c->fpu;
    uint16_t tag = full_tag(c);
    if (in->opsize == 2) {
        uint16_t w[7] = {f->fcw, f->fsw, tag, (uint16_t)f->fip, f->fcs, (uint16_t)f->fdp, f->fds};
        if (realmode(c)) {
            w[4] = (uint16_t)((((f->fip >> 16) & 0xF) << 12) | (f->fop & 0x7FF));
            w[6] = (uint16_t)(((f->fdp >> 16) & 0xF) << 12);
        }
        wrm(c, a + 12, 2, w[6]);
        for (int i = 0; i < 7; i++) wrm(c, a + 2 * i, 2, w[i]);
    } else {
        /* reserved upper words are written as zero */
        uint32_t d[7] = {f->fcw, f->fsw, tag, (uint32_t)f->fip,
                         f->fcs | ((uint32_t)(f->fop & 0x7FF) << 16), (uint32_t)f->fdp, f->fds};
        if (realmode(c)) {
            d[3] = (uint16_t)f->fip;
            d[4] = (uint32_t)(((f->fip >> 16) & 0xFFFF) << 12) | (f->fop & 0x7FF);
            d[5] = (uint16_t)f->fdp;
            d[6] = (uint32_t)(((f->fdp >> 16) & 0xFFFF) << 12);
        }
        wrm(c, a + 24, 4, d[6]);
        for (int i = 0; i < 7; i++) wrm(c, a + 4 * i, 4, d[i]);
    }
}

static void load_env(CPU *c, const Insn *in, uint64_t a) {
    FPU *f = &c->fpu;
    uint16_t tag;
    if (in->opsize == 2) {
        uint16_t w[7];
        for (int i = 0; i < 7; i++) w[i] = (uint16_t)rdm(c, a + 2 * i, 2);
        f->fcw = w[0];
        f->fsw = w[1];
        tag = w[2];
        if (realmode(c)) {
            f->fip = w[3] | ((uint64_t)(w[4] >> 12) << 16);
            f->fop = w[4] & 0x7FF;
            f->fdp = w[5] | ((uint64_t)(w[6] >> 12) << 16);
        } else {
            f->fip = w[3];
            f->fcs = w[4];
            f->fdp = w[5];
            f->fds = w[6];
        }
    } else {
        uint32_t d[7];
        for (int i = 0; i < 7; i++) d[i] = (uint32_t)rdm(c, a + 4 * i, 4);
        f->fcw = (uint16_t)d[0];
        f->fsw = (uint16_t)d[1];
        tag = (uint16_t)d[2];
        if (realmode(c)) {
            f->fip = (d[3] & 0xFFFF) | ((uint64_t)(d[4] >> 12) << 16);
            f->fop = d[4] & 0x7FF;
            f->fdp = (d[5] & 0xFFFF) | ((uint64_t)(d[6] >> 12) << 16);
        } else {
            f->fip = d[3];
            f->fcs = (uint16_t)d[4];
            f->fop = (d[4] >> 16) & 0x7FF;
            f->fdp = d[5];
            f->fds = (uint16_t)d[6];
        }
    }
    f->fcw = (uint16_t)((f->fcw & 0x1F3F) | 0x40);
    f->ftw = abridge(tag);
    update_es(c);
}

/* ---- the instruction dispatcher ------------------------------------------------------ */

static void exec_mem(CPU *c, const Insn *in, int op, int reg, uint64_t a) {
    FxEnv e = env(c);
    fx80 v;
    switch (op) {
    case 0: case 2: case 4: case 6: { /* arithmetic with a memory operand */
        v = load_arith_operand(c, &e, op, a);
        if (reg == 2 || reg == 3) {
            /* FCOM/FCOMP m: conversion flags count too */
            int r;
            if (empty(c, 0)) {
                if (!stack_fault(c, 0)) return;
                r = CMP_UN;
            } else {
                r = fx_compare(&e, st(c, 0), v, 0);
                if (!commit(c, &e, 0)) return;
            }
            set_cc(c, cmp_cc(r));
            if (reg == 3) pop(c);
            return;
        }
        fx80 r;
        if (empty(c, 0)) {
            if (!stack_fault(c, 0)) return;
            r = fx_indefinite();
        } else {
            r = arith(&e, reg, st(c, 0), v);
            if (!commit(c, &e, 1)) return;
        }
        set_st(c, 0, r);
        return;
    }
    case 1:
        switch (reg) {
        case 0: /* FLD m32 */
            v = fx_from_f32(&e, (uint32_t)rdm(c, a, 4));
            if (!push_ok(c)) { if (!stack_fault(c, 1)) return; v = fx_indefinite(); }
            else if (!commit(c, &e, 0)) return;
            push(c, v);
            c->fpu.fsw &= (uint16_t)~SW_C1;
            return;
        case 2: case 3: { /* FST/FSTP m32 */
            uint32_t r;
            if (empty(c, 0)) {
                if (!stack_fault(c, 0)) return;
                r = 0xFFC00000u;
            } else {
                r = fx_to_f32(&e, st(c, 0));
                if (e.flags & ~c->fpu.fcw & (FX_IE | FX_OE | FX_UE)) { commit(c, &e, 1); return; }
                wrm(c, a, 4, r);
                commit(c, &e, 1);
                if (reg == 3) pop(c);
                return;
            }
            wrm(c, a, 4, r);
            if (reg == 3) pop(c);
            return;
        }
        case 4: load_env(c, in, a); return;
        case 5: /* FLDCW */
            c->fpu.fcw = (uint16_t)((rdm(c, a, 2) & 0x1F3F) | 0x40);
            update_es(c);
            return;
        case 6: /* FNSTENV */
            store_env(c, in, a);
            c->fpu.fcw |= 0x3F;
            update_es(c);
            return;
        case 7: wrm(c, a, 2, c->fpu.fcw); return;
        }
        break;
    case 3: case 5: case 7:
        if ((op == 3 && reg <= 3) || (op == 7 && reg <= 3) || (op == 5 && (reg == 1 || reg == 2 || reg == 3)) ||
            (op == 7 && (reg == 5 || reg == 7))) {
            /* integer and real stores/loads by size */
            int isize = op == 3 ? 4 : op == 7 ? 2 : 8;
            if (op == 7 && reg >= 5) isize = 8;
            if (reg == 0 || (op == 7 && reg == 5)) { /* FILD */
                int64_t iv = isize == 2 ? (int16_t)rdm(c, a, 2) : isize == 4 ? (int32_t)rdm(c, a, 4) : (int64_t)rdm(c, a, 8);
                fpush(c, fx_from_i64(iv));
                c->fpu.fsw &= (uint16_t)~SW_C1;
                return;
            }
            if (op == 5 && reg != 1) { /* FST/FSTP m64 */
                uint64_t r;
                if (empty(c, 0)) {
                    if (!stack_fault(c, 0)) return;
                    r = 0xFFF8000000000000ULL;
                } else {
                    r = fx_to_f64(&e, st(c, 0));
                    if (e.flags & ~c->fpu.fcw & (FX_IE | FX_OE | FX_UE)) { commit(c, &e, 1); return; }
                    wrm(c, a, 8, r);
                    commit(c, &e, 1);
                    if (reg == 3) pop(c);
                    return;
                }
                wrm(c, a, 8, r);
                if (reg == 3) pop(c);
                return;
            }
            /* FIST/FISTP/FISTTP */
            int truncate = reg == 1;
            int doPop = reg != 2;
            int64_t r;
            if (empty(c, 0)) {
                if (!stack_fault(c, 0)) return;
                r = (int64_t)(1ULL << (isize * 8 - 1));
            } else {
                r = fx_to_int(&e, st(c, 0), isize * 8, truncate);
                if (e.flags & ~c->fpu.fcw & FX_IE) { commit(c, &e, 1); return; }
                wrm(c, a, isize, (uint64_t)r);
                commit(c, &e, 1);
                if (doPop) pop(c);
                return;
            }
            wrm(c, a, isize, (uint64_t)r);
            if (doPop) pop(c);
            return;
        }
        if (op == 3 && reg == 5) { /* FLD m80 */
            v = rd80(c, a);
            fpush(c, v);
            c->fpu.fsw &= (uint16_t)~SW_C1;
            return;
        }
        if (op == 3 && reg == 7) { /* FSTP m80 */
            if (empty(c, 0)) {
                if (!stack_fault(c, 0)) return;
                v = fx_indefinite();
            } else v = st(c, 0);
            wr80(c, a, v);
            c->fpu.fsw &= (uint16_t)~SW_C1;
            pop(c);
            return;
        }
        if (op == 5 && reg == 0) { /* FLD m64 */
            v = fx_from_f64(&e, rdm(c, a, 8));
            if (!push_ok(c)) { if (!stack_fault(c, 1)) return; v = fx_indefinite(); }
            else if (!commit(c, &e, 0)) return;
            push(c, v);
            c->fpu.fsw &= (uint16_t)~SW_C1;
            return;
        }
        if (op == 5 && reg == 4) { /* FRSTOR */
            int n = env_size(in);
            fx80 regs[8];
            for (int i = 0; i < 8; i++) regs[i] = rd80(c, a + n + 10 * i);
            load_env(c, in, a);
            for (int i = 0; i < 8; i++) c->fpu.st[ph(c, i)] = regs[i];
            return;
        }
        if (op == 5 && reg == 6) { /* FNSAVE */
            int n = env_size(in);
            wr80(c, a + n + 70, st(c, 7));
            store_env(c, in, a);
            for (int i = 0; i < 8; i++) wr80(c, a + n + 10 * i, st(c, i));
            fninit(c);
            return;
        }
        if (op == 5 && reg == 7) { wrm(c, a, 2, c->fpu.fsw); return; } /* FNSTSW m16 */
        if (op == 7 && reg == 4) { /* FBLD */
            uint8_t bcd[10];
            for (int i = 0; i < 10; i++) bcd[i] = (uint8_t)rdm(c, a + i, 1);
            fx_from_bcd(bcd, &v);
            fpush(c, v);
            c->fpu.fsw &= (uint16_t)~SW_C1;
            return;
        }
        if (op == 7 && reg == 6) { /* FBSTP */
            uint8_t bcd[10];
            if (empty(c, 0)) {
                if (!stack_fault(c, 0)) return;
                memset(bcd, 0, sizeof bcd);
                bcd[9] = bcd[8] = 0xFF;
                bcd[7] = 0xC0;
            } else {
                fx_to_bcd(&e, st(c, 0), bcd);
                if (e.flags & ~c->fpu.fcw & FX_IE) { commit(c, &e, 1); return; }
            }
            wrm(c, a + 8, 2, (uint64_t)bcd[8] | ((uint64_t)bcd[9] << 8));
            for (int i = 0; i < 10; i++) wrm(c, a + i, 1, bcd[i]);
            commit(c, &e, 1);
            pop(c);
            return;
        }
        break;
    }
    raise_exc(c, EXC_UD, 0, NULL);
}

static void exec_reg(CPU *c, const Insn *in, int op, int reg, int i) {
    FxEnv e = env(c);
    uint8_t m = in->modrm;
    fx80 v, r;

    /* register arithmetic: D8 (ST0 = ST0 op STi), DC (STi = STi op ST0), DE (same, pop) */
    if ((op == 0 || op == 4 || op == 6) && reg != 2 && reg != 3) {
        int dst = op == 0 ? 0 : i, src = op == 0 ? i : 0;
        int o = reg;
        if (op != 0 && o >= 4) o ^= 1;
        if (empty(c, dst) || empty(c, src)) {
            if (!stack_fault(c, 0)) return;
            r = fx_indefinite();
        } else {
            r = arith(&e, o, st(c, dst), st(c, src));
            if (!commit(c, &e, 1)) return;
        }
        set_st(c, dst, r);
        if (op == 6) pop(c);
        return;
    }
    if ((op == 0 || op == 4 || op == 6) && (reg == 2 || reg == 3)) { /* FCOM/FCOMP (and aliases) */
        if (op == 6 && m == 0xD9) { /* FCOMPP */
            if (!fcom(c, st(c, 1), empty(c, 1), 0, 0)) return;
            pop(c);
            pop(c);
            return;
        }
        if (!fcom(c, st(c, i), empty(c, i), 0, 0)) return;
        if (reg == 3 || op == 6) pop(c);
        return;
    }

    switch (op) {
    case 1:
        switch (reg) {
        case 0: /* FLD ST(i) */
            if (empty(c, i)) {
                if (!stack_fault(c, 0)) return;
                v = fx_indefinite();
            } else v = st(c, i);
            fpush(c, v);
            return;
        case 1: /* FXCH */
            if (empty(c, 0) || empty(c, i)) {
                if (!stack_fault(c, 0)) return;
                if (empty(c, 0)) set_st(c, 0, fx_indefinite());
                if (empty(c, i)) set_st(c, i, fx_indefinite());
            }
            v = st(c, 0);
            set_st(c, 0, st(c, i));
            set_st(c, i, v);
            c->fpu.fsw &= (uint16_t)~SW_C1;
            return;
        case 2:
            if (i == 0) return; /* FNOP */
            break;
        case 3: /* FSTP1 (alias) */
            goto fstp;
        case 4:
            switch (i) {
            case 0: case 1: /* FCHS, FABS */
                if (empty(c, 0)) {
                    if (!stack_fault(c, 0)) return;
                    set_st(c, 0, fx_indefinite());
                    return;
                }
                v = st(c, 0);
                set_st(c, 0, i == 0 ? fx_neg(v) : fx_abs(v));
                c->fpu.fsw &= (uint16_t)~SW_C1;
                return;
            case 4: /* FTST */
                fcom(c, fx_make(0, 0, 0), 0, 0, 0);
                return;
            case 5: /* FXAM */
                set_cc(c, fxam(c));
                return;
            }
            break;
        case 5: { /* constants */
            static const int k[8] = {FXK_ONE, FXK_L2T, FXK_L2E, FXK_PI, FXK_LG2, FXK_LN2, FXK_ZERO, -1};
            if (k[i] < 0) break;
            fpush(c, fx_const(&e, k[i]));
            if (!(c->fpu.fsw & SW_SF)) c->fpu.fsw &= (uint16_t)~SW_C1;
            return;
        }
        case 6:
            switch (i) {
            case 0: unary(c, f_2xm1); return;
            case 1: binary_pop(c, fx_yl2x); return;
            case 2: { /* FPTAN */
                if (empty(c, 0)) {
                    if (!stack_fault(c, 0)) return;
                    set_st(c, 0, fx_indefinite());
                    fpush(c, fx_indefinite());
                    return;
                }
                fx80 t;
                if (!fx_tan(&e, st(c, 0), &t)) { set_cc(c, SW_C2); return; }
                if (!commit(c, &e, 1)) return;
                c->fpu.fsw &= (uint16_t)~SW_C2;
                if (!push_ok(c)) {
                    if (!stack_fault(c, 1)) return;
                    set_st(c, 0, fx_indefinite());
                    push(c, fx_indefinite());
                    return;
                }
                set_st(c, 0, t);
                push(c, fx_isnan(t) ? t : fx_make(0, 16383, 1ULL << 63));
                return;
            }
            case 3: binary_pop(c, fx_atan2); return;
            case 4: { /* FXTRACT */
                if (empty(c, 0)) {
                    if (!stack_fault(c, 0)) return;
                    set_st(c, 0, fx_indefinite());
                    fpush(c, fx_indefinite());
                    return;
                }
                fx80 ex, sg;
                fx_xtract(&e, st(c, 0), &ex, &sg);
                if (!commit(c, &e, 1)) return;
                if (!push_ok(c)) {
                    if (!stack_fault(c, 1)) return;
                    ex = sg = fx_indefinite();
                }
                set_st(c, 0, ex);
                push(c, sg);
                return;
            }
            case 5: goto fprem; /* FPREM1 */
            case 6: set_top(c, top(c) - 1); c->fpu.fsw &= (uint16_t)~SW_C1; return; /* FDECSTP */
            case 7: set_top(c, top(c) + 1); c->fpu.fsw &= (uint16_t)~SW_C1; return; /* FINCSTP */
            }
            break;
        case 7:
            switch (i) {
            case 0: { /* FPREM (D9 F8); FPREM1 (D9 F5) jumps here */
            fprem:;
                int q, partial;
                if (empty(c, 0) || empty(c, 1)) {
                    if (!stack_fault(c, 0)) return;
                    set_st(c, 0, fx_indefinite());
                    return;
                }
                r = fx_prem(&e, st(c, 0), st(c, 1), reg == 6, &q, &partial);
                if (!commit(c, &e, 0)) return;
                set_st(c, 0, r);
                set_cc(c, (partial ? SW_C2 : 0) | (q & 4 ? SW_C0 : 0) | (q & 2 ? SW_C3 : 0) | (q & 1 ? SW_C1 : 0));
                return;
            }
            case 1: binary_pop(c, fx_yl2xp1); return;
            case 2: unary(c, f_sqrt); return;
            case 3: { /* FSINCOS */
                if (empty(c, 0)) {
                    if (!stack_fault(c, 0)) return;
                    set_st(c, 0, fx_indefinite());
                    fpush(c, fx_indefinite());
                    return;
                }
                fx80 s, co;
                if (!fx_sincos(&e, st(c, 0), &s, &co)) { set_cc(c, SW_C2); return; }
                if (!commit(c, &e, 1)) return;
                c->fpu.fsw &= (uint16_t)~SW_C2;
                if (!push_ok(c)) {
                    if (!stack_fault(c, 1)) return;
                    s = co = fx_indefinite();
                }
                set_st(c, 0, s);
                push(c, co);
                return;
            }
            case 4: unary(c, f_rndint); return;
            case 5: { /* FSCALE */
                if (empty(c, 0) || empty(c, 1)) {
                    if (!stack_fault(c, 0)) return;
                    set_st(c, 0, fx_indefinite());
                    return;
                }
                r = fx_scale(&e, st(c, 0), st(c, 1));
                if (!commit(c, &e, 1)) return;
                set_st(c, 0, r);
                return;
            }
            case 6: case 7: { /* FSIN, FCOS */
                if (empty(c, 0)) {
                    if (!stack_fault(c, 0)) return;
                    set_st(c, 0, fx_indefinite());
                    return;
                }
                fx80 s, co;
                if (!fx_sincos(&e, st(c, 0), i == 6 ? &s : NULL, i == 7 ? &co : NULL)) {
                    set_cc(c, (c->fpu.fsw & SW_C1) | SW_C2);
                    return;
                }
                if (!commit(c, &e, 1)) return;
                c->fpu.fsw &= (uint16_t)~SW_C2;
                set_st(c, 0, i == 6 ? s : co);
                return;
            }
            }
            break;
        }
        break;
    case 2: case 3:
        if (reg <= 3) { /* FCMOVcc */
            if (empty(c, 0) || empty(c, i)) {
                if (!stack_fault(c, 0)) return;
                set_st(c, 0, fx_indefinite());
                return;
            }
            if (cond_fcmov(c, op, reg)) set_st(c, 0, st(c, i));
            c->fpu.fsw &= (uint16_t)~SW_C1;
            return;
        }
        if (op == 2 && m == 0xE9) { /* FUCOMPP */
            if (!fcom(c, st(c, 1), empty(c, 1), 1, 0)) return;
            pop(c);
            pop(c);
            return;
        }
        if (op == 3) {
            if (m == 0xE2) { c->fpu.fsw &= 0x7F00; return; } /* FNCLEX */
            if (m == 0xE3) { fninit(c); return; }              /* FNINIT */
            if (m == 0xE0 || m == 0xE1 || m == 0xE4) return;   /* FENI/FDISI/FSETPM */
            if (reg == 5 || reg == 6) { fcom(c, st(c, i), empty(c, i), reg == 5, 1); return; } /* FUCOMI/FCOMI */
        }
        break;
    case 5:
        switch (reg) {
        case 0: /* FFREE */
            c->fpu.ftw &= (uint8_t)~(1 << ph(c, i));
            return;
        case 1: goto fxch_alias;
        case 2: case 3: /* FST/FSTP ST(i) */
        fstp:
            if (empty(c, 0)) {
                if (!stack_fault(c, 0)) return;
                v = fx_indefinite();
            } else v = st(c, 0);
            set_st(c, i, v);
            c->fpu.fsw &= (uint16_t)~SW_C1;
            if (reg == 3 || op != 5) pop(c);
            return;
        case 4: case 5: /* FUCOM/FUCOMP */
            if (!fcom(c, st(c, i), empty(c, i), 1, 0)) return;
            if (reg == 5) pop(c);
            return;
        }
        break;
    case 7:
        switch (reg) {
        case 0: /* FFREEP */
            c->fpu.ftw &= (uint8_t)~(1 << ph(c, i));
            pop(c);
            return;
        case 1:
        fxch_alias:
            exec_reg(c, in, 1, 1, i);
            return;
        case 2: case 3: goto fstp;
        case 4:
            if (i == 0) { setreg(c, RAX, 2, 1, c->fpu.fsw); return; } /* FNSTSW AX */
            break;
        case 5: case 6: /* FUCOMIP/FCOMIP */
            if (!fcom(c, st(c, i), empty(c, i), reg == 5, 1)) return;
            pop(c);
            return;
        }
        break;
    }
    raise_exc(c, EXC_UD, 0, NULL);
}

void exec_x87(CPU *c, const Insn *in) {
    int op = in->opcode & 7, reg = (in->modrm >> 3) & 7, mem = in->mod != 3;
    int nowait = (mem && (op == 1 || op == 5) && (reg == 6 || reg == 7)) ||
                 (!mem && op == 3 && (in->modrm == 0xE2 || in->modrm == 0xE3)) ||
                 (!mem && op == 7 && in->modrm == 0xE0);
    int control = nowait || (mem && op == 1 && (reg == 4 || reg == 5)) || (mem && op == 5 && reg == 4);
    if (!nowait) fpu_check_pending(c);
    uint64_t a = 0;
    if (mem) a = ea_linear(c, in);
    if (!control) {
        c->fpu.fop = (uint16_t)(((in->opcode & 7) << 8) | in->modrm);
        c->fpu.fip = in->addr;
        c->fpu.fcs = c->sreg[CS];
        if (mem) {
            c->fpu.fdp = ea_offset(c, in);
            c->fpu.fds = c->sreg[in->seg >= 0 ? in->seg : DS];
        }
    }
    if (mem) exec_mem(c, in, op, reg, a);
    else exec_reg(c, in, op, reg, in->modrm & 7);
}

/* ---- FXSAVE / FXRSTOR / XSAVE ----------------------------------------------------------- */

static int nxmm(CPU *c) { return c->bits == 64 ? 16 : 8; }

static void save_x87_legacy(CPU *c, uint64_t a, int rex_w) {
    FPU *f = &c->fpu;
    wrm(c, a + 0, 2, f->fcw);
    wrm(c, a + 2, 2, f->fsw);
    wrm(c, a + 4, 1, f->ftw);
    wrm(c, a + 5, 1, 0);
    wrm(c, a + 6, 2, f->fop);
    if (rex_w) {
        wrm(c, a + 8, 8, f->fip);
        wrm(c, a + 16, 8, f->fdp);
    } else {
        wrm(c, a + 8, 4, (uint32_t)f->fip);
        wrm(c, a + 12, 4, f->fcs);
        wrm(c, a + 16, 4, (uint32_t)f->fdp);
        wrm(c, a + 20, 4, f->fds);
    }
    for (int i = 0; i < 8; i++) {
        fx80 v = st(c, i);
        wrm(c, a + 32 + 16 * i, 8, v.sig);
        wrm(c, a + 40 + 16 * i, 8, v.se);
    }
}

static void load_x87_legacy(CPU *c, uint64_t a, int rex_w) {
    FPU *f = &c->fpu;
    f->fcw = (uint16_t)((rdm(c, a, 2) & 0x1F3F) | 0x40);
    f->fsw = (uint16_t)rdm(c, a + 2, 2);
    f->ftw = (uint8_t)rdm(c, a + 4, 1);
    f->fop = (uint16_t)(rdm(c, a + 6, 2) & 0x7FF);
    if (rex_w) {
        f->fip = rdm(c, a + 8, 8);
        f->fdp = rdm(c, a + 16, 8);
    } else {
        f->fip = rdm(c, a + 8, 4);
        f->fcs = (uint16_t)rdm(c, a + 12, 2);
        f->fdp = rdm(c, a + 16, 4);
        f->fds = (uint16_t)rdm(c, a + 20, 2);
    }
    for (int i = 0; i < 8; i++) {
        fx80 v;
        v.sig = rdm(c, a + 32 + 16 * i, 8);
        v.se = (uint16_t)rdm(c, a + 40 + 16 * i, 2);
        c->fpu.st[ph(c, i)] = v;
    }
    update_es(c);
}

static void save_xmm(CPU *c, uint64_t a) {
    for (int i = 0; i < nxmm(c); i++) wrm128(c, a + 160 + 16 * i, c->zmm[i].b);
}

static void load_xmm(CPU *c, uint64_t a) {
    for (int i = 0; i < nxmm(c); i++) rdm128(c, a + 160 + 16 * i, c->zmm[i].b);
}

static void x87_init_state(CPU *c) {
    fninit(c);
    memset(c->fpu.st, 0, sizeof c->fpu.st);
}

void exec_fxsave(CPU *c, const Insn *in) {
    uint64_t a = ea_linear(c, in);
    int w = (in->rex & 8) != 0;
    switch (in->id) {
    case I_FXSAVE:
        if (a & 15) raise_exc(c, EXC_GP, 0, "#GP misaligned FXSAVE area");
        wrm(c, a + 511, 1, rdm(c, a + 511, 1)); /* probe the whole area first */
        save_x87_legacy(c, a, w);
        wrm(c, a + 24, 4, c->mxcsr);
        wrm(c, a + 28, 4, MXCSR_MASK);
        save_xmm(c, a);
        return;
    case I_FXRSTOR: {
        if (a & 15) raise_exc(c, EXC_GP, 0, "#GP misaligned FXRSTOR area");
        uint32_t mx = (uint32_t)rdm(c, a + 24, 4);
        if (mx & ~MXCSR_MASK) raise_exc(c, EXC_GP, 0, NULL);
        rdm(c, a + 511, 1);
        load_x87_legacy(c, a, w);
        c->mxcsr = mx;
        load_xmm(c, a);
        return;
    }
    case I_XSAVE: case I_XSAVEOPT: {
        if (a & 63) raise_exc(c, EXC_GP, 0, "#GP misaligned XSAVE area");
        uint64_t rfbm = (((c->r[RDX] & 0xFFFFFFFF) << 32) | (c->r[RAX] & 0xFFFFFFFF)) & c->xcr0;
        wrm(c, a + 575, 1, rdm(c, a + 575, 1));
        if (rfbm & 1) save_x87_legacy(c, a, w);
        if (rfbm & 2) {
            wrm(c, a + 24, 4, c->mxcsr);
            wrm(c, a + 28, 4, MXCSR_MASK);
            save_xmm(c, a);
        }
        uint64_t bv = rdm(c, a + 512, 8);
        bv = (bv & ~rfbm) | (rfbm & 3);
        wrm(c, a + 512, 8, bv);
        return;
    }
    case I_XRSTOR: {
        if (a & 63) raise_exc(c, EXC_GP, 0, "#GP misaligned XRSTOR area");
        uint64_t rfbm = (((c->r[RDX] & 0xFFFFFFFF) << 32) | (c->r[RAX] & 0xFFFFFFFF)) & c->xcr0;
        uint64_t bv = rdm(c, a + 512, 8), comp = rdm(c, a + 520, 8);
        if ((bv & ~c->xcr0) || comp) raise_exc(c, EXC_GP, 0, NULL);
        if (rfbm & 2) {
            uint32_t mx = (uint32_t)rdm(c, a + 24, 4);
            if (mx & ~MXCSR_MASK) raise_exc(c, EXC_GP, 0, NULL);
            c->mxcsr = mx;
        }
        if (rfbm & 1) {
            if (bv & 1) load_x87_legacy(c, a, w);
            else x87_init_state(c);
        }
        if (rfbm & 2) {
            if (bv & 2) load_xmm(c, a);
            else for (int i = 0; i < nxmm(c); i++) memset(c->zmm[i].b, 0, 16);
        }
        return;
    }
    }
    unimplemented(c, in);
}
