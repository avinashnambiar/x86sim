/* Differential checker for src/softfloat.c.
 *
 * The same source builds two programs that walk an identical pseudo-random
 * sequence of operations and operands and print every result and status
 * word in hex:
 *   - built for x86-64 (run under Rosetta or on real hardware), it executes
 *     the real x87 instructions and is the reference;
 *   - built natively anywhere else, it calls the softfloat library.
 * tests/softfloat/run.sh diffs the two outputs. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct __attribute__((packed)) { uint64_t sig; uint16_t se; } X;

enum {
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_SQRT, OP_RNDINT, OP_SCALE, OP_PREM, OP_PREM1,
    OP_XTRACT, OP_SIN, OP_COS, OP_TAN, OP_SINCOS, OP_ATAN, OP_2XM1, OP_YL2X, OP_YL2XP1,
    OP_ST32, OP_ST64, OP_IST16, OP_IST32, OP_IST64, OP_ISTT64, OP_LD32, OP_LD64,
    OP_COM, OP_UCOM, OP_BST, OP_CONST, OP_COUNT
};
static const char *names[] = {
    "add", "sub", "mul", "div", "sqrt", "rndint", "scale", "prem", "prem1",
    "xtract", "sin", "cos", "tan", "sincos", "atan", "2xm1", "yl2x", "yl2xp1",
    "st32", "st64", "ist16", "ist32", "ist64", "istt64", "ld32", "ld64",
    "com", "ucom", "bst", "const"
};

static uint64_t rs = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}

static X mk(int s, int e, uint64_t sig) { X x = {sig, (uint16_t)((s << 15) | e)}; return x; }

static X gen(int expspread) {
    int s = rnd() & 1;
    unsigned k = rnd() % 100;
    uint64_t sig = rnd() | (1ULL << 63);
    if (rnd() % 4 == 0) sig &= ~0ULL << (rnd() % 64); /* short significands */
    if (k < 70) return mk(s, 16383 + (int)(rnd() % (2 * expspread + 1)) - expspread, sig);
    if (k < 76) { int64_t v = (int64_t)(rnd() % 2000) - 1000; long e = 0; uint64_t m = v < 0 ? -v : v;
                  if (!m) return mk(s, 0, 0);
                  while (!(m >> 63)) { m <<= 1; e++; } return mk(v < 0, 16383 + 63 - (int)e, m); }
    if (k < 79) return mk(s, 0, 0);
    if (k < 82) return mk(s, 0x7FFF, 1ULL << 63);
    if (k < 84) return mk(s, 0x7FFF, (1ULL << 63) | (1ULL << 62) | (rnd() >> 3));
    if (k < 86) return mk(s, 0x7FFF, (1ULL << 63) | ((rnd() >> 3) | 1));
    if (k < 89) return mk(s, 0, rnd() >> (1 + rnd() % 60));
    if (k < 92) return mk(s, 1 + (int)(rnd() % 80), sig);
    if (k < 95) return mk(s, 0x7FFE - (int)(rnd() % 80), sig);
    if (k < 96) return mk(s, 1 + (int)(rnd() % 0x7FFD), sig & ~(1ULL << 63)); /* unnormal */
    return mk(s, 16383 + (int)(rnd() % 3) - 1, (1ULL << 63) | (rnd() >> (rnd() % 64)));
}

static X near(X a) { /* operand close to a, for cancellation */
    X b = a;
    if (rnd() & 1) b.se ^= 0x8000;
    b.sig ^= rnd() >> (rnd() % 64);
    b.sig |= 1ULL << 63;
    if (rnd() & 1) b.se += (rnd() % 3) - 1;
    return b;
}

static X gen_for(int op, X *b) {
    X a;
    switch (op) {
    case OP_SIN: case OP_COS: case OP_TAN: case OP_SINCOS:
        a = gen(rnd() % 3 == 0 ? 70 : 4); break;
    case OP_2XM1: a = gen(0); a.se = (a.se & 0x8000) | (16382 - (int)(rnd() % (rnd() % 4 == 0 ? 70 : 6))); break;
    case OP_YL2XP1: a = gen(0); a.se = (a.se & 0x8000) | (16381 - (int)(rnd() % (rnd() % 4 == 0 ? 70 : 6))); break;
    case OP_YL2X: a = gen(rnd() % 2 ? 40 : 2); if (rnd() % 3) a.se &= 0x7FFF; break;
    case OP_SCALE: a = gen(60); *b = gen(4); return a;
    case OP_PREM: case OP_PREM1: a = gen(20); *b = gen(20); if (rnd() % 8 == 0) a.se += 70; return a;
    default: a = gen(rnd() % 4 == 0 ? 8000 : 40);
    }
    if (rnd() % 6 == 0) *b = near(a);
    else *b = gen(rnd() % 4 == 0 ? 8000 : 40);
    return a;
}

typedef struct { X r1, r2; uint64_t ir; uint8_t bcd[10]; unsigned sw; } Res;

#ifdef __x86_64__
#define PRE "fninit\n fldcw %[cw]\n"
#define BIN(insn) __asm__ volatile(PRE "fldt %[b]\n fldt %[a]\n" insn "\n fnstsw %[sw]\n fstpt %[r]\n fstp %%st(0)" \
        : [r] "=m"(o->r1), [sw] "=m"(sw) : [a] "m"(a), [b] "m"(b), [cw] "m"(cw))
#define TWO(insn) __asm__ volatile(PRE "fldt %[b]\n fldt %[a]\n" insn "\n fnstsw %[sw]\n fstpt %[r]" \
        : [r] "=m"(o->r1), [sw] "=m"(sw) : [a] "m"(a), [b] "m"(b), [cw] "m"(cw))
#define UN(insn) __asm__ volatile(PRE "fldt %[a]\n" insn "\n fnstsw %[sw]\n fstpt %[r]" \
        : [r] "=m"(o->r1), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw))
#define UN2(insn) __asm__ volatile(PRE "fldt %[a]\n" insn "\n fnstsw %[sw]\n fstpt %[r]\n fstpt %[r2]" \
        : [r] "=m"(o->r1), [r2] "=m"(o->r2), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw))
#define ST(insn) __asm__ volatile(PRE "fldt %[a]\n" insn " %[r]\n fnstsw %[sw]" \
        : [r] "=m"(o->ir), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw))

static void run(int op, unsigned short cw, X a, X b, Res *o) {
    unsigned short sw = 0;
    memset(o, 0, sizeof *o);
    switch (op) {
    case OP_ADD: BIN("fadd %%st(1), %%st"); break;
    case OP_SUB: BIN("fsub %%st(1), %%st"); break;
    case OP_MUL: BIN("fmul %%st(1), %%st"); break;
    case OP_DIV: BIN("fdiv %%st(1), %%st"); break;
    case OP_SQRT: UN("fsqrt"); break;
    case OP_RNDINT: UN("frndint"); break;
    case OP_SCALE: BIN("fscale"); break;
    case OP_PREM: BIN("fprem"); break;
    case OP_PREM1: BIN("fprem1"); break;
    case OP_XTRACT: UN2("fxtract"); break;
    case OP_SIN: UN("fsin"); break;
    case OP_COS: UN("fcos"); break;
    case OP_TAN: UN2("fptan"); break;
    case OP_SINCOS: UN2("fsincos"); break;
    case OP_ATAN: TWO("fpatan"); break;
    case OP_2XM1: UN("f2xm1"); break;
    case OP_YL2X: TWO("fyl2x"); break;
    case OP_YL2XP1: TWO("fyl2xp1"); break;
    case OP_ST32: ST("fstps"); break;
    case OP_ST64: ST("fstpl"); break;
    case OP_IST16: ST("fistps"); break;
    case OP_IST32: ST("fistpl"); break;
    case OP_IST64: ST("fistpll"); break;
    case OP_ISTT64: ST("fisttpll"); break;
    case OP_LD32: { uint32_t v = (uint32_t)a.sig;
        __asm__ volatile(PRE "flds %[v]\n fnstsw %[sw]\n fstpt %[r]" : [r] "=m"(o->r1), [sw] "=m"(sw) : [v] "m"(v), [cw] "m"(cw)); break; }
    case OP_LD64: { uint64_t v = a.sig;
        __asm__ volatile(PRE "fldl %[v]\n fnstsw %[sw]\n fstpt %[r]" : [r] "=m"(o->r1), [sw] "=m"(sw) : [v] "m"(v), [cw] "m"(cw)); break; }
    case OP_COM: __asm__ volatile(PRE "fldt %[b]\n fldt %[a]\n fcom %%st(1)\n fnstsw %[sw]\n fstp %%st(0)\n fstp %%st(0)"
        : [sw] "=m"(sw) : [a] "m"(a), [b] "m"(b), [cw] "m"(cw)); break;
    case OP_UCOM: __asm__ volatile(PRE "fldt %[b]\n fldt %[a]\n fucom %%st(1)\n fnstsw %[sw]\n fstp %%st(0)\n fstp %%st(0)"
        : [sw] "=m"(sw) : [a] "m"(a), [b] "m"(b), [cw] "m"(cw)); break;
    case OP_BST: __asm__ volatile(PRE "fldt %[a]\n fbstp %[r]\n fnstsw %[sw]" : [r] "=m"(o->bcd), [sw] "=m"(sw) : [a] "m"(a), [cw] "m"(cw)); break;
    case OP_CONST: switch (a.sig % 7) {
        case 0: UN("fstp %%st(0)\n fld1"); break;
        case 1: UN("fstp %%st(0)\n fldl2t"); break;
        case 2: UN("fstp %%st(0)\n fldl2e"); break;
        case 3: UN("fstp %%st(0)\n fldpi"); break;
        case 4: UN("fstp %%st(0)\n fldlg2"); break;
        case 5: UN("fstp %%st(0)\n fldln2"); break;
        default: UN("fstp %%st(0)\n fldz"); break;
        } break;
    }
    o->sw = sw;
}
#else
#include "softfloat.h"

static fx80 F(X x) { fx80 r = {x.sig, x.se}; return r; }
static X U(fx80 x) { X r = {x.sig, x.se}; return r; }

static void run(int op, unsigned short cw, X a, X b, Res *o) {
    static const int precs[4] = {24, 24, 53, 64};
    FxEnv e = {(cw >> 10) & 3, precs[(cw >> 8) & 3], 0, 0, 0, 0, 0};
    unsigned c = 0;
    int ok, q, partial, r;
    fx80 t1, t2;
    memset(o, 0, sizeof *o);
    switch (op) {
    case OP_ADD: o->r1 = U(fx_add(&e, F(a), F(b))); goto c1;
    case OP_SUB: o->r1 = U(fx_sub(&e, F(a), F(b))); goto c1;
    case OP_MUL: o->r1 = U(fx_mul(&e, F(a), F(b))); goto c1;
    case OP_DIV: o->r1 = U(fx_div(&e, F(a), F(b))); goto c1;
    case OP_SQRT: o->r1 = U(fx_sqrt(&e, F(a))); goto c1;
    case OP_RNDINT: o->r1 = U(fx_round_int(&e, F(a))); goto c1;
    case OP_SCALE: o->r1 = U(fx_scale(&e, F(a), F(b))); goto c1;
    case OP_PREM: case OP_PREM1:
        o->r1 = U(fx_prem(&e, F(a), F(b), op == OP_PREM1, &q, &partial));
        c = (q & 4 ? 0x100 : 0) | (q & 2 ? 0x4000 : 0) | (q & 1 ? 0x200 : 0) | (partial ? 0x400 : 0);
        break;
    case OP_XTRACT: fx_xtract(&e, F(a), &t1, &t2); o->r1 = U(t2); o->r2 = U(t1); break;
    case OP_SIN: ok = fx_sincos(&e, F(a), &t1, NULL); o->r1 = ok ? U(t1) : a; c = ok ? 0 : 0x400; goto c1t;
    case OP_COS: ok = fx_sincos(&e, F(a), NULL, &t1); o->r1 = ok ? U(t1) : a; c = ok ? 0 : 0x400; goto c1t;
    case OP_TAN: ok = fx_tan(&e, F(a), &t1);
        if (ok) { o->r1 = U(fx_make(0, 16383, 1ULL << 63)); o->r2 = U(t1);
                  if (fx_isnan(t1)) o->r1 = o->r2; }
        else { o->r1 = a; o->r2 = U(fx_indefinite()); c = 0x400; }
        goto c1t;
    case OP_SINCOS: ok = fx_sincos(&e, F(a), &t1, &t2);
        if (ok) { o->r1 = U(t2); o->r2 = U(t1); } else { o->r1 = a; o->r2 = U(fx_indefinite()); c = 0x400; }
        goto c1t;
    case OP_ATAN: o->r1 = U(fx_atan2(&e, F(b), F(a))); goto c1;
    case OP_2XM1: o->r1 = U(fx_2xm1(&e, F(a))); goto c1;
    case OP_YL2X: o->r1 = U(fx_yl2x(&e, F(b), F(a))); goto c1;
    case OP_YL2XP1: o->r1 = U(fx_yl2xp1(&e, F(b), F(a))); goto c1;
    case OP_ST32: o->ir = fx_to_f32(&e, F(a)); goto c1;
    case OP_ST64: o->ir = fx_to_f64(&e, F(a)); goto c1;
    case OP_IST16: o->ir = (uint16_t)fx_to_int(&e, F(a), 16, 0); goto c1;
    case OP_IST32: o->ir = (uint32_t)fx_to_int(&e, F(a), 32, 0); goto c1;
    case OP_IST64: o->ir = (uint64_t)fx_to_int(&e, F(a), 64, 0); goto c1;
    case OP_ISTT64: o->ir = (uint64_t)fx_to_int(&e, F(a), 64, 1); goto c1;
    case OP_LD32: o->r1 = U(fx_from_f32(&e, (uint32_t)a.sig)); break;
    case OP_LD64: o->r1 = U(fx_from_f64(&e, a.sig)); break;
    case OP_COM: case OP_UCOM:
        r = fx_compare(&e, F(a), F(b), op == OP_UCOM);
        c = r == CMP_LT ? 0x100 : r == CMP_EQ ? 0x4000 : r == CMP_UN ? 0x4500 : 0;
        break;
    case OP_BST: fx_to_bcd(&e, F(a), o->bcd); goto c1;
    case OP_CONST: {
        static const int k[7] = {FXK_ONE, FXK_L2T, FXK_L2E, FXK_PI, FXK_LG2, FXK_LN2, FXK_ZERO};
        o->r1 = U(fx_const(&e, k[a.sig % 7])); goto c1; }
    }
    o->sw = e.flags | c;
    return;
c1t:
c1:
    o->sw = e.flags | c | ((e.flags & FX_PE) && e.roundup ? 0x200 : 0);
}
#endif

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 3000;
    int only = -1;
    if (argc > 2) for (int i = 0; i < OP_COUNT; i++) if (!strcmp(argv[2], names[i])) only = i;
    for (int op = 0; op < OP_COUNT; op++) {
        for (int i = 0; i < n; i++) {
            X b;
            X a = gen_for(op, &b);
            if (op == OP_LD32) a.sig = (uint32_t)rnd() >> (rnd() % 4 == 0 ? rnd() % 32 : 0) | (rnd() % 8 == 0 ? 0x7F800000u : 0);
            if (op == OP_LD64) a.sig = rnd() >> (rnd() % 4 == 0 ? rnd() % 64 : 0);
            unsigned short cw = 0x037F & ~0x0F00;
            cw |= (rnd() % 4) << 10;
            cw |= (rnd() % 3 == 0 ? (rnd() % 2 ? 0 : 2) : 3) << 8;
            if (op >= OP_SIN && op <= OP_YL2XP1) cw |= 0x300;
            if (only >= 0 && op != only) continue;
            Res o;
            run(op, cw, a, b, &o);
            unsigned mask = 0x3F;
            if (op == OP_PREM || op == OP_PREM1) mask |= 0x4700;
            if (op == OP_COM || op == OP_UCOM) mask |= 0x4500;
            if (op >= OP_SIN && op <= OP_SINCOS) mask |= 0x400;
            printf("%-6s %04x %04x%016llx %04x%016llx -> %04x%016llx %04x%016llx %016llx ", names[op], cw,
                   a.se, (unsigned long long)a.sig, b.se, (unsigned long long)b.sig,
                   o.r1.se, (unsigned long long)o.r1.sig, o.r2.se, (unsigned long long)o.r2.sig,
                   (unsigned long long)o.ir);
            for (int j = 9; j >= 0; j--) printf("%02x", o.bcd[j]);
            printf(" sw=%04x c1=%d\n", o.sw & mask, (o.sw >> 9) & 1);
        }
    }
    return 0;
}
