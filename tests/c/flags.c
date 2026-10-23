/* Executes individual ALU instructions via inline asm and prints the result
 * and the architecturally *defined* flags. Compared against real hardware
 * (Rosetta) this pins down flag semantics instruction by instruction.
 * Build with -mno-red-zone (the asm pushes onto the stack). x86-64 only. */
#include "../../runtime/sim.h"

static const uint64_t vals[] = {
    0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000,
    0xffffffff, 0x100000000ULL, 0x7fffffffffffffffULL, 0x8000000000000000ULL, 0xffffffffffffffffULL,
    0x123456789abcdef0ULL, 0xfedcba9876543210ULL, 0x5a5a5a5a5a5a5a5aULL,
};
#define NV (sizeof vals / sizeof vals[0])
static const unsigned counts[] = {0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63};
#define NC (sizeof counts / sizeof counts[0])

#define F_ALL 0x8D5UL   /* CF PF AF ZF SF OF */
#define F_LOGIC 0x8C5UL /* AF undefined */
#define F_CF 0x001UL
#define F_OF 0x800UL
#define F_SZP 0x0C4UL

static void out(const char *name, int bits, uint64_t a, uint64_t b, uint64_t r, unsigned long fl, unsigned long mask) {
    print_str(name);
    print_u64((uint64_t)bits);
    print_char(' ');
    print_hex(a);
    print_char(' ');
    print_hex(b);
    print_str(" -> ");
    print_hex(r);
    print_str(" f=");
    print_hex(fl & mask);
    print_nl();
}

#define BINOP(fn, insn, T)                                                                         \
    static void fn(uint64_t a_, uint64_t b_, unsigned long fin, unsigned long mask, int bits) {     \
        T a = (T)a_, b = (T)b_;                                                                    \
        unsigned long fl;                                                                          \
        __asm__ volatile("push %[fi]\n\tpopf\n\t" insn " %[b], %[a]\n\tpushf\n\tpop %[fl]"          \
                         : [a] "+r"(a), [fl] "=&r"(fl)                                             \
                         : [b] "r"(b), [fi] "r"(fin)                                               \
                         : "cc");                                                                  \
        out(#insn, bits, (T)a_, (T)b_, (uint64_t)a, fl, mask);                                     \
    }

#define UNOP(fn, insn, T)                                                                          \
    static void fn(uint64_t a_, unsigned long fin, unsigned long mask, int bits) {                 \
        T a = (T)a_;                                                                               \
        unsigned long fl;                                                                          \
        __asm__ volatile("push %[fi]\n\tpopf\n\t" insn " %[a]\n\tpushf\n\tpop %[fl]"                \
                         : [a] "+r"(a), [fl] "=&r"(fl)                                             \
                         : [fi] "r"(fin)                                                           \
                         : "cc");                                                                  \
        out(#insn, bits, (T)a_, 0, (uint64_t)a, fl, mask);                                         \
    }

#define SHOP(fn, insn, T)                                                                          \
    static void fn(uint64_t a_, unsigned cnt, unsigned long fin, unsigned long mask, int bits) {    \
        T a = (T)a_;                                                                               \
        unsigned long fl;                                                                          \
        __asm__ volatile("push %[fi]\n\tpopf\n\t" insn " %%cl, %[a]\n\tpushf\n\tpop %[fl]"          \
                         : [a] "+r"(a), [fl] "=&r"(fl)                                             \
                         : "c"((uint8_t)cnt), [fi] "r"(fin)                                        \
                         : "cc");                                                                  \
        out(#insn, bits, (T)a_, cnt, (uint64_t)a, fl, mask);                                       \
    }

#define SHDOP(fn, insn, T)                                                                         \
    static void fn(uint64_t a_, uint64_t b_, unsigned cnt, unsigned long mask, int bits) {          \
        T a = (T)a_, b = (T)b_;                                                                    \
        unsigned long fl;                                                                          \
        __asm__ volatile("push $0\n\tpopf\n\t" insn " %%cl, %[b], %[a]\n\tpushf\n\tpop %[fl]"       \
                         : [a] "+r"(a), [fl] "=&r"(fl)                                             \
                         : [b] "r"(b), "c"((uint8_t)cnt)                                           \
                         : "cc");                                                                  \
        out(#insn, bits, (T)a_, cnt, (uint64_t)a, fl, mask);                                       \
    }

#define ALL_WIDTHS(M, name, insn) M(name##8, insn, uint8_t) M(name##16, insn, uint16_t) M(name##32, insn, uint32_t) M(name##64, insn, uint64_t)
#define WIDE_WIDTHS(M, name, insn) M(name##16, insn, uint16_t) M(name##32, insn, uint32_t) M(name##64, insn, uint64_t)

ALL_WIDTHS(BINOP, add, "add")
ALL_WIDTHS(BINOP, adc, "adc")
ALL_WIDTHS(BINOP, sub, "sub")
ALL_WIDTHS(BINOP, sbb, "sbb")
ALL_WIDTHS(BINOP, and, "and")
ALL_WIDTHS(BINOP, or, "or")
ALL_WIDTHS(BINOP, xor, "xor")
ALL_WIDTHS(BINOP, cmp, "cmp")
ALL_WIDTHS(BINOP, test, "test")
WIDE_WIDTHS(BINOP, imul, "imul")
WIDE_WIDTHS(BINOP, bt, "bt")
ALL_WIDTHS(UNOP, inc, "inc")
ALL_WIDTHS(UNOP, dec, "dec")
ALL_WIDTHS(UNOP, neg, "neg")
ALL_WIDTHS(SHOP, shl, "shl")
ALL_WIDTHS(SHOP, shr, "shr")
ALL_WIDTHS(SHOP, sar, "sar")
ALL_WIDTHS(SHOP, rol, "rol")
ALL_WIDTHS(SHOP, ror, "ror")
ALL_WIDTHS(SHOP, rcl, "rcl")
ALL_WIDTHS(SHOP, rcr, "rcr")
WIDE_WIDTHS(SHDOP, shld, "shld")
WIDE_WIDTHS(SHDOP, shrd, "shrd")

typedef void (*BinFn)(uint64_t, uint64_t, unsigned long, unsigned long, int);
typedef void (*UnFn)(uint64_t, unsigned long, unsigned long, int);
typedef void (*ShFn)(uint64_t, unsigned, unsigned long, unsigned long, int);
typedef void (*ShdFn)(uint64_t, uint64_t, unsigned, unsigned long, int);

static const int widths[4] = {8, 16, 32, 64};

static unsigned long shift_mask(int is_rot, int is_rc, unsigned cnt, int bits) {
    unsigned masked = cnt & (bits == 64 ? 63 : 31);
    if (masked == 0) return F_ALL; /* nothing changes */
    if (is_rc) return F_CF | (masked == 1 ? F_OF : 0);
    if (is_rot) return F_CF | (masked == 1 ? F_OF : 0);
    unsigned long m = F_SZP | (masked == 1 ? F_OF : 0);
    if (masked <= (unsigned)bits) m |= F_CF;
    return m;
}

int main(void) {
    BinFn bins[][4] = {{add8, add16, add32, add64}, {adc8, adc16, adc32, adc64}, {sub8, sub16, sub32, sub64},
                       {sbb8, sbb16, sbb32, sbb64}, {and8, and16, and32, and64}, {or8, or16, or32, or64},
                       {xor8, xor16, xor32, xor64}, {cmp8, cmp16, cmp32, cmp64}, {test8, test16, test32, test64}};
    unsigned long bin_masks[] = {F_ALL, F_ALL, F_ALL, F_ALL, F_LOGIC, F_LOGIC, F_LOGIC, F_ALL, F_LOGIC};
    for (unsigned op = 0; op < 9; op++)
        for (int w = 0; w < 4; w++)
            for (unsigned i = 0; i < NV; i++)
                for (unsigned j = 0; j < NV; j++)
                    for (unsigned long cf = 0; cf < ((op == 1 || op == 3) ? 2 : 1); cf++)
                        bins[op][w](vals[i], vals[j], 0x2 | cf, bin_masks[op], widths[w]);

    BinFn imuls[3] = {imul16, imul32, imul64}, bts[3] = {bt16, bt32, bt64};
    for (int w = 0; w < 3; w++)
        for (unsigned i = 0; i < NV; i++)
            for (unsigned j = 0; j < NV; j++) {
                imuls[w](vals[i], vals[j], 0x2, F_CF | F_OF, widths[w + 1]);
                bts[w](vals[i], vals[j], 0x2, F_CF, widths[w + 1]);
            }

    UnFn uns[][4] = {{inc8, inc16, inc32, inc64}, {dec8, dec16, dec32, dec64}, {neg8, neg16, neg32, neg64}};
    for (int op = 0; op < 3; op++)
        for (int w = 0; w < 4; w++)
            for (unsigned i = 0; i < NV; i++)
                for (unsigned long cf = 0; cf < 2; cf++) uns[op][w](vals[i], 0x2 | cf, F_ALL, widths[w]);

    ShFn shs[][4] = {{shl8, shl16, shl32, shl64}, {shr8, shr16, shr32, shr64}, {sar8, sar16, sar32, sar64},
                     {rol8, rol16, rol32, rol64}, {ror8, ror16, ror32, ror64}, {rcl8, rcl16, rcl32, rcl64},
                     {rcr8, rcr16, rcr32, rcr64}};
    for (int op = 0; op < 7; op++)
        for (int w = 0; w < 4; w++)
            for (unsigned i = 0; i < NV; i++)
                for (unsigned k = 0; k < NC; k++)
                    for (unsigned long cf = 0; cf < 2; cf++)
                        shs[op][w](vals[i], counts[k], 0x2 | cf, shift_mask(op >= 3, op >= 5, counts[k], widths[w]),
                                   widths[w]);

    ShdFn shds[][3] = {{shld16, shld32, shld64}, {shrd16, shrd32, shrd64}};
    for (int op = 0; op < 2; op++)
        for (int w = 0; w < 3; w++)
            for (unsigned i = 0; i < NV; i += 2)
                for (unsigned j = 1; j < NV; j += 3)
                    for (unsigned k = 0; k < NC; k++) {
                        int bits = widths[w + 1];
                        unsigned masked = counts[k] & (bits == 64 ? 63 : 31);
                        if (masked > (unsigned)bits) continue; /* result undefined */
                        unsigned long mask = masked == 0 ? F_ALL : (F_SZP | F_CF | (masked == 1 ? F_OF : 0));
                        shds[op][w](vals[i], vals[j], counts[k], mask, bits);
                    }
    return 0;
}
