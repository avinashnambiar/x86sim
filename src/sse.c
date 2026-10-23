/* SIMD execution unit for legacy-encoded instructions: MMX, SSE, SSE2,
 * SSE3, SSSE3, SSE4.1, SSE4.2, AES-NI, PCLMULQDQ and SHA.
 *
 * Integer operations are written once over 128 bits; MMX forms run the same
 * code on 64-bit operands (zero-extended) and keep the low half, except for
 * the few operations whose lane layout depends on the register width.
 * Floating point goes through softfloat.c with MXCSR rounding, DAZ/FTZ,
 * sticky flags and #XM for unmasked exceptions. Legacy-encoded SSE leaves
 * bits 128+ of YMM/ZMM untouched. */
#include <math.h>
#include <string.h>

#include "cpu_internal.h"

typedef union {
    uint8_t b[16];
    int8_t sb[16];
    uint16_t w[8];
    int16_t sw[8];
    uint32_t d[4];
    int32_t sd[4];
    uint64_t q[2];
    int64_t sq[2];
} X128;

static void xget(CPU *c, int r, X128 *v) { memcpy(v, c->zmm[r & 15].b, 16); }
static void xset(CPU *c, int r, const X128 *v) { memcpy(c->zmm[r & 15].b, v, 16); }

static int is_mmx_spec(int spec) { return spec == O_Pq || spec == O_Qq || spec == O_Nq; }

static void reg_get(CPU *c, int spec, int r, X128 *v) {
    if (is_mmx_spec(spec)) {
        v->q[0] = mmx_get(c, r & 7);
        v->q[1] = 0;
    } else {
        xget(c, r, v);
    }
}

static int unaligned_ok(int id) {
    switch (id) {
    case I_MOVUPS: case I_MOVUPD: case I_MOVDQU: case I_LDDQU:
    case I_PCMPESTRM: case I_PCMPESTRI: case I_PCMPISTRM: case I_PCMPISTRI:
        return 1;
    }
    return 0;
}

static int spec_msize(int spec) {
    switch (spec) {
    case O_Wq: case O_Qq: return 8;
    case O_Wd: return 4;
    case O_Ww: return 2;
    case O_M: case O_Wx: return 16;
    }
    return 16;
}

/* Read operand k. Memory operands read msize bytes (0: from the spec);
 * full 16-byte accesses must be aligned unless the instruction allows it. */
static void get(CPU *c, const Insn *in, int k, X128 *v, int msize) {
    int spec = in->opnd[k];
    if (spec == O_Vx || spec == O_Pq) { reg_get(c, spec, in->reg, v); return; }
    if (in->mod == 3) { reg_get(c, spec, in->rm, v); return; }
    if (!msize) msize = spec_msize(spec);
    uint64_t a = ea_linear(c, in);
    memset(v, 0, sizeof *v);
    if (msize == 16) {
        if ((a & 15) && !unaligned_ok(in->id)) raise_exc(c, EXC_GP, 0, "#GP misaligned 16-byte SSE access");
        rdm128(c, a, v);
    } else {
        uint64_t x = rdm(c, a, msize);
        memcpy(v->b, &x, msize);
    }
}

static void put(CPU *c, const Insn *in, int k, const X128 *v) {
    int spec = in->opnd[k];
    if (spec == O_Pq) { mmx_set(c, in->reg & 7, v->q[0]); return; }
    if (spec == O_Vx) { xset(c, in->reg, v); return; }
    if (in->mod == 3) {
        if (is_mmx_spec(spec)) mmx_set(c, in->rm & 7, v->q[0]);
        else xset(c, in->rm, v);
        return;
    }
    int msize = spec_msize(spec);
    uint64_t a = ea_linear(c, in);
    if (msize == 16) {
        if ((a & 15) && !unaligned_ok(in->id)) raise_exc(c, EXC_GP, 0, "#GP misaligned 16-byte SSE access");
        wrm128(c, a, v);
    } else {
        uint64_t x = 0;
        memcpy(&x, v->b, msize);
        wrm(c, a, msize, x);
    }
}

static inline uint8_t sat_u8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static inline int8_t sat_s8(int v) { return v < -128 ? -128 : v > 127 ? 127 : (int8_t)v; }
static inline uint16_t sat_u16(int v) { return v < 0 ? 0 : v > 65535 ? 65535 : (uint16_t)v; }
static inline int16_t sat_s16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }
static inline uint16_t sat_u16_32(int32_t v) { return v < 0 ? 0 : v > 65535 ? 65535 : (uint16_t)v; }

/* ---- MXCSR -------------------------------------------------------------------- */

static FxEnv sse_env(CPU *c) {
    FxEnv e;
    memset(&e, 0, sizeof e);
    e.rc = (c->mxcsr >> 13) & 3;
    e.daz = (c->mxcsr >> 6) & 1;
    e.ftz = (c->mxcsr >> 15) & 1;
    return e;
}

/* Record exception flags; unmasked ones raise #XM before any result is written. */
static void sse_commit(CPU *c, FxEnv *e) {
    unsigned f = e->flags & 0x3F;
    c->mxcsr |= f;
    if (f & ~(c->mxcsr >> 7) & 0x3F) raise_exc(c, EXC_XM, 0, NULL);
}

/* ---- packed shifts -------------------------------------------------------------- */

static void packed_shift(CPU *c, const Insn *in) {
    X128 v, cnt;
    uint64_t n;
    int k;
    if (in->opnd[1] == O_Ib) { /* 0F 71/72/73 /r ib: shift N/U register by immediate */
        if (in->opnd[0] == O_Nq && (in->id == I_PSRLDQ || in->id == I_PSLLDQ)) raise_exc(c, EXC_UD, 0, NULL);
        k = 0;
        get(c, in, 0, &v, 0);
        n = in->imm & 0xFF;
    } else {
        k = 0;
        get(c, in, 0, &v, 0);
        get(c, in, 1, &cnt, 0);
        n = cnt.q[0];
    }
    switch (in->id) {
    case I_PSRLW: for (int i = 0; i < 8; i++) v.w[i] = n > 15 ? 0 : (uint16_t)(v.w[i] >> n); break;
    case I_PSLLW: for (int i = 0; i < 8; i++) v.w[i] = n > 15 ? 0 : (uint16_t)(v.w[i] << n); break;
    case I_PSRAW: for (int i = 0; i < 8; i++) v.sw[i] = (int16_t)(v.sw[i] >> (n > 15 ? 15 : n)); break;
    case I_PSRLD: for (int i = 0; i < 4; i++) v.d[i] = n > 31 ? 0 : v.d[i] >> n; break;
    case I_PSLLD: for (int i = 0; i < 4; i++) v.d[i] = n > 31 ? 0 : v.d[i] << n; break;
    case I_PSRAD: for (int i = 0; i < 4; i++) v.sd[i] = v.sd[i] >> (n > 31 ? 31 : n); break;
    case I_PSRLQ: for (int i = 0; i < 2; i++) v.q[i] = n > 63 ? 0 : v.q[i] >> n; break;
    case I_PSLLQ: for (int i = 0; i < 2; i++) v.q[i] = n > 63 ? 0 : v.q[i] << n; break;
    case I_PSRLDQ: case I_PSLLDQ: {
        X128 r;
        memset(&r, 0, sizeof r);
        if (n < 16)
            for (int i = 0; i < 16; i++) {
                int src = in->id == I_PSRLDQ ? i + (int)n : i - (int)n;
                if (src >= 0 && src < 16) r.b[i] = v.b[src];
            }
        v = r;
        break;
    }
    }
    put(c, in, k, &v);
}

/* ---- floating point --------------------------------------------------------------- */

enum { K_PS, K_PD, K_SS, K_SD };

static void fp_arith(CPU *c, const Insn *in) {
    int k = in->id - I_ADDPS, op = k >> 2, kind = k & 3;
    FxEnv e = sse_env(c);
    X128 a, b, r;
    xget(c, in->reg, &a);
    get(c, in, 1, &b, 0);
    r = a;
    switch (kind) {
    case K_PS: for (int i = 0; i < 4; i++) r.d[i] = sf32_op(&e, op, a.d[i], b.d[i]); break;
    case K_PD: for (int i = 0; i < 2; i++) r.q[i] = sf64_op(&e, op, a.q[i], b.q[i]); break;
    case K_SS: r.d[0] = sf32_op(&e, op, a.d[0], b.d[0]); break;
    case K_SD: r.q[0] = sf64_op(&e, op, a.q[0], b.q[0]); break;
    }
    sse_commit(c, &e);
    xset(c, in->reg, &r);
}

/* RCP/RSQRT: hardware gives a table-based approximation with relative error
 * below 1.5*2^-12; this one is the exact value rounded to 12 bits. */
static uint32_t approx(uint32_t a, int rsqrt) {
    int cls = sf32_class(a);
    uint32_t sign = a & 0x80000000u;
    if (cls == FXC_QNAN || cls == FXC_SNAN) return a | 0x00400000u;
    if (cls == FXC_ZERO || cls == FXC_DENORMAL) return sign | 0x7F800000u;
    if (rsqrt && sign) return 0xFFC00000u;
    if (cls == FXC_INF) return sign;
    float f;
    memcpy(&f, &a, 4);
    double x = f, r = rsqrt ? 1.0 / sqrt(x) : 1.0 / x;
    int ex;
    double m = frexp(r, &ex);
    m = ldexp(nearbyint(ldexp(m, 12)), -12);
    float out = (float)ldexp(m, ex);
    uint32_t bits;
    memcpy(&bits, &out, 4);
    if ((bits & 0x7F800000u) == 0) bits &= 0x80000000u; /* tiny results flush to zero */
    return bits;
}

static int cmp_pred(int r, int p) {
    switch (p & 7) {
    case 0: return r == CMP_EQ;
    case 1: return r == CMP_LT;
    case 2: return r == CMP_LT || r == CMP_EQ;
    case 3: return r == CMP_UN;
    case 4: return r != CMP_EQ;
    case 5: return r != CMP_LT;
    case 6: return !(r == CMP_LT || r == CMP_EQ);
    default: return r != CMP_UN;
    }
}

static int pred_signals(int p) { p &= 7; return p == 1 || p == 2 || p == 5 || p == 6; }

static void set_cmp_flags(CPU *c, int r) {
    uint64_t f = r == CMP_UN ? (FL_ZF | FL_PF | FL_CF) : r == CMP_LT ? FL_CF : r == CMP_EQ ? FL_ZF : 0;
    c->rflags = (c->rflags & ~FL_ARITH) | f;
}

/* ---- SSE4.2 string compare ---------------------------------------------------------- */

static void pcmpstr(CPU *c, const Insn *in) {
    X128 a, b;
    unsigned imm = (unsigned)in->imm;
    int words = imm & 1, sgn = (imm >> 1) & 1, agg = (imm >> 2) & 3, pol = (imm >> 4) & 3;
    int n = words ? 8 : 16;
    int explicit_len = in->id == I_PCMPESTRM || in->id == I_PCMPESTRI;
    xget(c, in->reg, &a);
    get(c, in, 1, &b, 16);
    int la, lb;
    if (explicit_len) {
        int64_t ra = (in->rex & 8) ? (int64_t)c->r[RAX] : (int32_t)c->r[RAX];
        int64_t rb = (in->rex & 8) ? (int64_t)c->r[RDX] : (int32_t)c->r[RDX];
        la = ra < 0 ? (ra < -n ? n : (int)-ra) : (ra > n ? n : (int)ra);
        lb = rb < 0 ? (rb < -n ? n : (int)-rb) : (rb > n ? n : (int)rb);
    } else {
        la = lb = n;
        for (int i = 0; i < n; i++) if ((words ? a.w[i] : a.b[i]) == 0) { la = i; break; }
        for (int i = 0; i < n; i++) if ((words ? b.w[i] : b.b[i]) == 0) { lb = i; break; }
    }
#define ELEM(v, i) (words ? (sgn ? (int32_t)(v).sw[i] : (int32_t)(v).w[i]) : (sgn ? (int32_t)(v).sb[i] : (int32_t)(v).b[i]))
    unsigned res = 0;
    for (int j = 0; j < n; j++) {
        int bit = 0;
        switch (agg) {
        case 0: /* equal any */
            if (j < lb)
                for (int i = 0; i < la; i++)
                    if (ELEM(a, i) == ELEM(b, j)) { bit = 1; break; }
            break;
        case 1: /* ranges */
            if (j < lb)
                for (int i = 0; i + 1 < la; i += 2)
                    if (ELEM(a, i) <= ELEM(b, j) && ELEM(b, j) <= ELEM(a, i + 1)) { bit = 1; break; }
            break;
        case 2: { /* equal each */
            int va = j < la, vb = j < lb;
            bit = (va && vb) ? ELEM(a, j) == ELEM(b, j) : (!va && !vb);
            break;
        }
        case 3: /* equal ordered */
            bit = 1;
            for (int i = 0; i < n - j; i++) {
                int va = i < la, vb = j + i < lb;
                int r = !va ? 1 : !vb ? 0 : ELEM(a, i) == ELEM(b, j + i);
                if (!r) { bit = 0; break; }
            }
            break;
        }
        res |= (unsigned)bit << j;
    }
#undef ELEM
    unsigned all = (1u << n) - 1;
    if (pol == 1) res ^= all;
    else if (pol == 3) res ^= ((1u << lb) - 1) & all;
    uint64_t f = 0;
    if (res) f |= FL_CF;
    if (lb < n) f |= FL_ZF;
    if (la < n) f |= FL_SF;
    if (res & 1) f |= FL_OF;
    c->rflags = (c->rflags & ~FL_ARITH) | f;
    if (in->id == I_PCMPESTRI || in->id == I_PCMPISTRI) {
        int idx = n;
        if (res) idx = (imm & 0x40) ? 31 - __builtin_clz(res) : __builtin_ctz(res);
        c->r[RCX] = (uint32_t)idx;
    } else {
        X128 m;
        memset(&m, 0, sizeof m);
        if (imm & 0x40) {
            for (int j = 0; j < n; j++)
                if ((res >> j) & 1) {
                    if (words) m.w[j] = 0xFFFF;
                    else m.b[j] = 0xFF;
                }
        } else {
            m.d[0] = res;
        }
        xset(c, 0, &m);
    }
}

/* ---- AES -------------------------------------------------------------------------- */

static uint8_t sbox[256], inv_sbox[256];

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    while (b) {
        if (b & 1) p ^= a;
        a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
        b >>= 1;
    }
    return p;
}

static void aes_init(void) {
    if (sbox[0]) return;
    for (int x = 0; x < 256; x++) {
        uint8_t inv = 0;
        if (x)
            for (int y = 1; y < 256; y++)
                if (gmul((uint8_t)x, (uint8_t)y) == 1) { inv = (uint8_t)y; break; }
        uint8_t s = inv, r = inv;
        for (int i = 0; i < 4; i++) {
            r = (uint8_t)((r << 1) | (r >> 7));
            s ^= r;
        }
        s ^= 0x63;
        sbox[x] = s;
        inv_sbox[s] = (uint8_t)x;
    }
}

static void shift_rows(X128 *s, int inv) {
    X128 t = *s;
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            int from = inv ? (col - row + 4) & 3 : (col + row) & 3;
            s->b[row + 4 * col] = t.b[row + 4 * from];
        }
}

static void sub_bytes(X128 *s, int inv) {
    for (int i = 0; i < 16; i++) s->b[i] = inv ? inv_sbox[s->b[i]] : sbox[s->b[i]];
}

static void mix_columns(X128 *s, int inv) {
    for (int col = 0; col < 4; col++) {
        uint8_t *p = &s->b[4 * col], a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        if (!inv) {
            p[0] = (uint8_t)(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
            p[1] = (uint8_t)(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
            p[2] = (uint8_t)(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
            p[3] = (uint8_t)(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
        } else {
            p[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
            p[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
            p[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
            p[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
        }
    }
}

static uint32_t sub_word(uint32_t w) {
    return (uint32_t)sbox[w & 0xFF] | (uint32_t)sbox[(w >> 8) & 0xFF] << 8 |
           (uint32_t)sbox[(w >> 16) & 0xFF] << 16 | (uint32_t)sbox[w >> 24] << 24;
}

/* ---- SHA ----------------------------------------------------------------------------- */

static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static uint32_t sha1_f(int k, uint32_t b, uint32_t c, uint32_t d) {
    switch (k) {
    case 0: return (b & c) ^ (~b & d);
    case 2: return (b & c) ^ (b & d) ^ (c & d);
    default: return b ^ c ^ d;
    }
}

static void sha(CPU *c, const Insn *in, X128 *a, const X128 *b) {
    X128 r;
    switch (in->id) {
    case I_SHA1RNDS4: {
        static const uint32_t K[4] = {0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6};
        int k = in->imm & 3;
        uint32_t A = a->d[3], B = a->d[2], C = a->d[1], D = a->d[0], E = 0;
        uint32_t W[4] = {b->d[3], b->d[2], b->d[1], b->d[0]};
        for (int i = 0; i < 4; i++) {
            uint32_t nA = sha1_f(k, B, C, D) + rol32(A, 5) + W[i] + E + K[k];
            E = D;
            D = C;
            C = rol32(B, 30);
            B = A;
            A = nA;
        }
        r.d[3] = A; r.d[2] = B; r.d[1] = C; r.d[0] = D;
        break;
    }
    case I_SHA1NEXTE:
        r = *b;
        r.d[3] = b->d[3] + rol32(a->d[3], 30);
        break;
    case I_SHA1MSG1:
        r.d[3] = a->d[1] ^ a->d[3];
        r.d[2] = a->d[0] ^ a->d[2];
        r.d[1] = b->d[3] ^ a->d[1];
        r.d[0] = b->d[2] ^ a->d[0];
        break;
    case I_SHA1MSG2: {
        uint32_t w16 = rol32(a->d[3] ^ b->d[2], 1), w17 = rol32(a->d[2] ^ b->d[1], 1);
        uint32_t w18 = rol32(a->d[1] ^ b->d[0], 1), w19 = rol32(a->d[0] ^ w16, 1);
        r.d[3] = w16; r.d[2] = w17; r.d[1] = w18; r.d[0] = w19;
        break;
    }
    case I_SHA256RNDS2: {
        X128 k0;
        xget(c, 0, &k0);
        uint32_t A = b->d[3], B = b->d[2], C = a->d[3], D = a->d[2], E = b->d[1], F = b->d[0], G = a->d[1], H = a->d[0];
        for (int i = 0; i < 2; i++) {
            uint32_t ch = (E & F) ^ (~E & G), maj = (A & B) ^ (A & C) ^ (B & C);
            uint32_t s0 = ror32(A, 2) ^ ror32(A, 13) ^ ror32(A, 22), s1 = ror32(E, 6) ^ ror32(E, 11) ^ ror32(E, 25);
            uint32_t t = ch + s1 + k0.d[i] + H;
            uint32_t nA = t + maj + s0, nE = t + D;
            H = G; G = F; F = E; E = nE;
            D = C; C = B; B = A; A = nA;
        }
        r.d[3] = A; r.d[2] = B; r.d[1] = E; r.d[0] = F;
        break;
    }
    case I_SHA256MSG1: {
#define S0(x) (ror32(x, 7) ^ ror32(x, 18) ^ ((x) >> 3))
        r.d[3] = a->d[3] + S0(b->d[0]);
        r.d[2] = a->d[2] + S0(a->d[3]);
        r.d[1] = a->d[1] + S0(a->d[2]);
        r.d[0] = a->d[0] + S0(a->d[1]);
#undef S0
        break;
    }
    case I_SHA256MSG2: {
#define S1(x) (ror32(x, 17) ^ ror32(x, 19) ^ ((x) >> 10))
        uint32_t w16 = a->d[0] + S1(b->d[2]), w17 = a->d[1] + S1(b->d[3]);
        uint32_t w18 = a->d[2] + S1(w16), w19 = a->d[3] + S1(w17);
#undef S1
        r.d[0] = w16; r.d[1] = w17; r.d[2] = w18; r.d[3] = w19;
        break;
    }
    default:
        unimplemented(c, in);
    }
    *a = r;
}

/* ---- dispatcher ------------------------------------------------------------------------- */

void exec_sse(CPU *c, const Insn *in) {
    X128 a, b, r;
    int id = in->id;
    int mmx = is_mmx_spec(in->opnd[0]) || is_mmx_spec(in->opnd[1]);
    int w = 16;

    /* x87 -> MMX transition for anything touching an MMX register */
    if (mmx) {
        int mem_only_mmx = (in->opnd[0] == O_Vx || in->opnd[0] == O_Gd || in->opnd[0] == O_Gy) &&
                           in->opnd[1] == O_Qq && in->mod != 3; /* e.g. cvtpi2ps xmm, m64 */
        if (!mem_only_mmx) mmx_enter(c);
        if (in->opnd[0] == O_Pq || in->opnd[0] == O_Nq || in->opnd[0] == O_Qq) w = 8;
    }

    if (id >= I_ADDPS && id <= I_SQRTSD) { fp_arith(c, in); return; }

    switch (id) {
    /* ---- moves ---- */
    case I_MOVUPS: case I_MOVUPD: case I_MOVDQU: case I_LDDQU:
    case I_MOVAPS: case I_MOVAPD: case I_MOVDQA: case I_MOVNTDQA:
        get(c, in, 1, &a, 16);
        put(c, in, 0, &a);
        return;
    case I_MOVNTPS: case I_MOVNTPD: case I_MOVNTDQ: case I_MOVNTQ:
        if (in->mod == 3) raise_exc(c, EXC_UD, 0, NULL);
        get(c, in, 1, &a, 0);
        put(c, in, 0, &a);
        return;
    case I_MOVSS: case I_MOVSD_X: {
        int s = id == I_MOVSS ? 4 : 8;
        if (in->opnd[0] == O_Vx) {
            xget(c, in->reg, &r);
            if (in->mod == 3) {
                xget(c, in->rm, &b);
                memcpy(r.b, b.b, s);
            } else {
                uint64_t v = rdm(c, ea_linear(c, in), s);
                memset(r.b, 0, 16);
                memcpy(r.b, &v, s);
            }
            xset(c, in->reg, &r);
        } else {
            xget(c, in->reg, &a);
            if (in->mod == 3) {
                xget(c, in->rm, &r);
                memcpy(r.b, a.b, s);
                xset(c, in->rm, &r);
            } else {
                uint64_t v = 0;
                memcpy(&v, a.b, s);
                wrm(c, ea_linear(c, in), s, v);
            }
        }
        return;
    }
    case I_MOVD:
        if (in->opnd[0] == O_Vx || in->opnd[0] == O_Pq) {
            memset(&r, 0, sizeof r);
            r.q[0] = rd_op(c, in, 1);
            put(c, in, 0, &r);
        } else {
            get(c, in, 1, &a, 0);
            wr_op(c, in, 0, a.q[0]);
        }
        return;
    case I_MOVQ:
        if (in->opnd[0] == O_Vx) { /* F3 0F 7E: load qword, zero upper */
            get(c, in, 1, &b, 8);
            memset(&r, 0, sizeof r);
            r.q[0] = b.q[0];
            xset(c, in->reg, &r);
        } else if (in->opnd[0] == O_Wq) { /* 66 0F D6: store qword */
            xget(c, in->reg, &a);
            if (in->mod == 3) {
                memset(&r, 0, sizeof r);
                r.q[0] = a.q[0];
                xset(c, in->rm, &r);
            } else {
                wrm(c, ea_linear(c, in), 8, a.q[0]);
            }
        } else { /* MMX movq */
            get(c, in, 1, &a, 8);
            put(c, in, 0, &a);
        }
        return;
    case I_MOVQ2DQ:
        memset(&r, 0, sizeof r);
        r.q[0] = mmx_get(c, in->rm & 7);
        xset(c, in->reg, &r);
        return;
    case I_MOVDQ2Q:
        xget(c, in->rm, &b);
        mmx_set(c, in->reg & 7, b.q[0]);
        return;
    case I_MOVLPS: case I_MOVLPD: case I_MOVHPS: case I_MOVHPD: {
        int hi = id == I_MOVHPS || id == I_MOVHPD;
        if (in->opnd[0] == O_Vx) {
            xget(c, in->reg, &r);
            if (in->mod == 3) { /* movhlps / movlhps */
                if (id == I_MOVLPD || id == I_MOVHPD) raise_exc(c, EXC_UD, 0, NULL);
                xget(c, in->rm, &b);
                if (hi) r.q[1] = b.q[0];
                else r.q[0] = b.q[1];
            } else {
                r.q[hi] = rdm(c, ea_linear(c, in), 8);
            }
            xset(c, in->reg, &r);
        } else {
            if (in->mod == 3) raise_exc(c, EXC_UD, 0, NULL);
            xget(c, in->reg, &a);
            wrm(c, ea_linear(c, in), 8, a.q[hi]);
        }
        return;
    }
    case I_MOVSLDUP: case I_MOVSHDUP:
        get(c, in, 1, &b, 16);
        for (int i = 0; i < 4; i += 2) r.d[i] = r.d[i + 1] = b.d[i + (id == I_MOVSHDUP)];
        xset(c, in->reg, &r);
        return;
    case I_MOVDDUP:
        get(c, in, 1, &b, 8);
        r.q[0] = r.q[1] = b.q[0];
        xset(c, in->reg, &r);
        return;
    case I_MOVMSKPS: case I_MOVMSKPD: case I_PMOVMSKB: {
        uint32_t mask = 0;
        get(c, in, 1, &b, 0);
        if (id == I_MOVMSKPS) for (int i = 0; i < 4; i++) mask |= (b.d[i] >> 31) << i;
        else if (id == I_MOVMSKPD) for (int i = 0; i < 2; i++) mask |= (uint32_t)(b.q[i] >> 63) << i;
        else for (int i = 0; i < (mmx ? 8 : 16); i++) mask |= (uint32_t)(b.b[i] >> 7) << i;
        setreg(c, in->reg, 4, 1, mask);
        return;
    }
    case I_PEXTRW:
        if (in->opnd[0] == O_Gd) { /* 0F C5: from N/U register */
            get(c, in, 1, &b, 0);
            setreg(c, in->reg, 4, 1, b.w[in->imm & (mmx ? 3 : 7)]);
        } else { /* 66 0F 3A 15: to r32/m16 */
            xget(c, in->reg, &b);
            uint16_t v = b.w[in->imm & 7];
            if (in->mod == 3) setreg(c, in->rm, 4, 1, v);
            else wrm(c, ea_linear(c, in), 2, v);
        }
        return;
    case I_PINSRW: {
        uint64_t v = rd_op(c, in, 1);
        get(c, in, 0, &r, 0);
        r.w[in->imm & (mmx ? 3 : 7)] = (uint16_t)v;
        put(c, in, 0, &r);
        return;
    }
    case I_PEXTRB: case I_PEXTRD: case I_EXTRACTPS: {
        xget(c, in->reg, &b);
        int q = id == I_PEXTRD && (in->rex & 8);
        uint64_t v = id == I_PEXTRB ? b.b[in->imm & 15] : q ? b.q[in->imm & 1] : b.d[in->imm & 3];
        int s = id == I_PEXTRB ? 1 : q ? 8 : 4;
        if (in->mod == 3) setreg(c, in->rm, q ? 8 : 4, 1, v);
        else wrm(c, ea_linear(c, in), s, v);
        return;
    }
    case I_PINSRB: case I_PINSRD: {
        int q = id == I_PINSRD && (in->rex & 8);
        uint64_t v = in->mod == 3 ? c->r[in->rm] : rdm(c, ea_linear(c, in), id == I_PINSRB ? 1 : q ? 8 : 4);
        xget(c, in->reg, &r);
        if (id == I_PINSRB) r.b[in->imm & 15] = (uint8_t)v;
        else if (q) r.q[in->imm & 1] = v;
        else r.d[in->imm & 3] = (uint32_t)v;
        xset(c, in->reg, &r);
        return;
    }
    case I_INSERTPS: {
        unsigned imm = (unsigned)in->imm;
        uint32_t v;
        if (in->mod == 3) { xget(c, in->rm, &b); v = b.d[(imm >> 6) & 3]; }
        else v = (uint32_t)rdm(c, ea_linear(c, in), 4);
        xget(c, in->reg, &r);
        r.d[(imm >> 4) & 3] = v;
        for (int i = 0; i < 4; i++) if (imm & (1u << i)) r.d[i] = 0;
        xset(c, in->reg, &r);
        return;
    }
    case I_MASKMOVDQU: case I_MASKMOVQ: {
        get(c, in, 0, &a, 0);
        get(c, in, 1, &b, 0);
        uint64_t base = (c->r[RDI] & MASK(in->addrsize));
        for (int i = 0; i < (mmx ? 8 : 16); i++)
            if (b.b[i] & 0x80) wrm(c, linear(c, in->seg >= 0 ? in->seg : DS, (base + i) & MASK(in->addrsize)), 1, a.b[i]);
        return;
    }
    case I_PSHUFD: case I_PSHUFHW: case I_PSHUFLW: case I_PSHUFW: {
        get(c, in, 1, &b, 0);
        unsigned imm = (unsigned)in->imm;
        r = b;
        if (id == I_PSHUFD) for (int i = 0; i < 4; i++) r.d[i] = b.d[(imm >> (2 * i)) & 3];
        else if (id == I_PSHUFLW || id == I_PSHUFW) for (int i = 0; i < 4; i++) r.w[i] = b.w[(imm >> (2 * i)) & 3];
        else for (int i = 0; i < 4; i++) r.w[4 + i] = b.w[4 + ((imm >> (2 * i)) & 3)];
        put(c, in, 0, &r);
        return;
    }
    case I_PSRLW: case I_PSRAW: case I_PSLLW: case I_PSRLD: case I_PSRAD: case I_PSLLD:
    case I_PSRLQ: case I_PSLLQ: case I_PSRLDQ: case I_PSLLDQ:
        packed_shift(c, in);
        return;
    case I_EMMS:
        mmx_emms(c);
        return;

    /* ---- FP compares and conversions ---- */
    case I_CMPPS: case I_CMPPD: case I_CMPSS: case I_CMPSD_X: {
        FxEnv e = sse_env(c);
        int p = in->imm & 7, sig = pred_signals(p);
        xget(c, in->reg, &a);
        get(c, in, 1, &b, 0);
        r = a;
        if (id == I_CMPPS || id == I_CMPSS)
            for (int i = 0; i < (id == I_CMPPS ? 4 : 1); i++)
                r.d[i] = cmp_pred(sf32_cmp(&e, a.d[i], b.d[i], sig), p) ? ~0u : 0;
        else
            for (int i = 0; i < (id == I_CMPPD ? 2 : 1); i++)
                r.q[i] = cmp_pred(sf64_cmp(&e, a.q[i], b.q[i], sig), p) ? ~0ULL : 0;
        sse_commit(c, &e);
        xset(c, in->reg, &r);
        return;
    }
    case I_UCOMISS: case I_COMISS: case I_UCOMISD: case I_COMISD: {
        FxEnv e = sse_env(c);
        xget(c, in->reg, &a);
        get(c, in, 1, &b, 0);
        int sig = id == I_COMISS || id == I_COMISD;
        int res = (id == I_UCOMISS || id == I_COMISS) ? sf32_cmp(&e, a.d[0], b.d[0], sig) : sf64_cmp(&e, a.q[0], b.q[0], sig);
        sse_commit(c, &e);
        set_cmp_flags(c, res);
        return;
    }
    case I_RCPPS: case I_RCPSS: case I_RSQRTPS: case I_RSQRTSS: {
        int rs = id == I_RSQRTPS || id == I_RSQRTSS;
        xget(c, in->reg, &r);
        get(c, in, 1, &b, 0);
        for (int i = 0; i < ((id == I_RCPPS || id == I_RSQRTPS) ? 4 : 1); i++) r.d[i] = approx(b.d[i], rs);
        xset(c, in->reg, &r);
        return;
    }
    case I_CVTPI2PS: case I_CVTPI2PD: case I_CVTDQ2PS: case I_CVTDQ2PD: {
        FxEnv e = sse_env(c);
        get(c, in, 1, &b, 0);
        xget(c, in->reg, &r);
        if (id == I_CVTPI2PS) for (int i = 0; i < 2; i++) r.d[i] = sf32_from_i64(&e, b.sd[i]);
        else if (id == I_CVTDQ2PS) for (int i = 0; i < 4; i++) r.d[i] = sf32_from_i64(&e, b.sd[i]);
        else { int64_t lo = b.sd[0], hi = b.sd[1]; r.q[0] = sf64_from_i64(&e, lo); r.q[1] = sf64_from_i64(&e, hi); }
        sse_commit(c, &e);
        xset(c, in->reg, &r);
        return;
    }
    case I_CVTSI2SS: case I_CVTSI2SD: {
        FxEnv e = sse_env(c);
        int s = opnd_size(in, in->opnd[1]);
        int64_t v = s == 8 ? (int64_t)rd_op(c, in, 1) : (int32_t)rd_op(c, in, 1);
        xget(c, in->reg, &r);
        if (id == I_CVTSI2SS) r.d[0] = sf32_from_i64(&e, v);
        else r.q[0] = sf64_from_i64(&e, v);
        sse_commit(c, &e);
        xset(c, in->reg, &r);
        return;
    }
    case I_CVTTSS2SI: case I_CVTSS2SI: case I_CVTTSD2SI: case I_CVTSD2SI: {
        FxEnv e = sse_env(c);
        int s = opnd_size(in, in->opnd[0]), t = id == I_CVTTSS2SI || id == I_CVTTSD2SI;
        get(c, in, 1, &b, 0);
        int64_t v = (id == I_CVTTSS2SI || id == I_CVTSS2SI) ? sf32_to_int(&e, b.d[0], s * 8, t) : sf64_to_int(&e, b.q[0], s * 8, t);
        sse_commit(c, &e);
        setreg(c, in->reg, s, 1, (uint64_t)v);
        return;
    }
    case I_CVTPS2PI: case I_CVTTPS2PI: case I_CVTPD2PI: case I_CVTTPD2PI:
    case I_CVTPS2DQ: case I_CVTTPS2DQ: case I_CVTPD2DQ: case I_CVTTPD2DQ: {
        FxEnv e = sse_env(c);
        int t = id == I_CVTTPS2PI || id == I_CVTTPD2PI || id == I_CVTTPS2DQ || id == I_CVTTPD2DQ;
        get(c, in, 1, &b, 0);
        memset(&r, 0, sizeof r);
        if (id == I_CVTPS2PI || id == I_CVTTPS2PI) for (int i = 0; i < 2; i++) r.d[i] = (uint32_t)sf32_to_int(&e, b.d[i], 32, t);
        else if (id == I_CVTPS2DQ || id == I_CVTTPS2DQ) for (int i = 0; i < 4; i++) r.d[i] = (uint32_t)sf32_to_int(&e, b.d[i], 32, t);
        else for (int i = 0; i < 2; i++) r.d[i] = (uint32_t)sf64_to_int(&e, b.q[i], 32, t);
        sse_commit(c, &e);
        put(c, in, 0, &r);
        return;
    }
    case I_CVTPS2PD: case I_CVTPD2PS: case I_CVTSS2SD: case I_CVTSD2SS: {
        FxEnv e = sse_env(c);
        get(c, in, 1, &b, 0);
        xget(c, in->reg, &r);
        switch (id) {
        case I_CVTPS2PD: { uint32_t lo = b.d[0], hi = b.d[1]; r.q[0] = sf32_to_f64(&e, lo); r.q[1] = sf32_to_f64(&e, hi); break; }
        case I_CVTPD2PS: r.d[0] = sf64_to_f32(&e, b.q[0]); r.d[1] = sf64_to_f32(&e, b.q[1]); r.q[1] = 0; break;
        case I_CVTSS2SD: r.q[0] = sf32_to_f64(&e, b.d[0]); break;
        default: r.d[0] = sf64_to_f32(&e, b.q[0]); break;
        }
        sse_commit(c, &e);
        xset(c, in->reg, &r);
        return;
    }
    case I_ROUNDPS: case I_ROUNDPD: case I_ROUNDSS: case I_ROUNDSD: {
        FxEnv e = sse_env(c);
        unsigned imm = (unsigned)in->imm;
        int rc = (imm & 4) ? e.rc : (int)(imm & 3), sup = (imm >> 3) & 1;
        get(c, in, 1, &b, 0);
        xget(c, in->reg, &r);
        switch (id) {
        case I_ROUNDPS: for (int i = 0; i < 4; i++) r.d[i] = sf32_round(&e, b.d[i], rc, sup); break;
        case I_ROUNDPD: for (int i = 0; i < 2; i++) r.q[i] = sf64_round(&e, b.q[i], rc, sup); break;
        case I_ROUNDSS: r.d[0] = sf32_round(&e, b.d[0], rc, sup); break;
        default: r.q[0] = sf64_round(&e, b.q[0], rc, sup); break;
        }
        sse_commit(c, &e);
        xset(c, in->reg, &r);
        return;
    }
    case I_ADDSUBPS: case I_ADDSUBPD: case I_HADDPS: case I_HADDPD: case I_HSUBPS: case I_HSUBPD: {
        FxEnv e = sse_env(c);
        xget(c, in->reg, &a);
        get(c, in, 1, &b, 0);
        switch (id) {
        case I_ADDSUBPS: for (int i = 0; i < 4; i++) r.d[i] = sf32_op(&e, (i & 1) ? SOP_ADD : SOP_SUB, a.d[i], b.d[i]); break;
        case I_ADDSUBPD: r.q[0] = sf64_op(&e, SOP_SUB, a.q[0], b.q[0]); r.q[1] = sf64_op(&e, SOP_ADD, a.q[1], b.q[1]); break;
        case I_HADDPS: case I_HSUBPS: {
            int op = id == I_HADDPS ? SOP_ADD : SOP_SUB;
            r.d[0] = sf32_op(&e, op, a.d[0], a.d[1]);
            r.d[1] = sf32_op(&e, op, a.d[2], a.d[3]);
            r.d[2] = sf32_op(&e, op, b.d[0], b.d[1]);
            r.d[3] = sf32_op(&e, op, b.d[2], b.d[3]);
            break;
        }
        default: {
            int op = id == I_HADDPD ? SOP_ADD : SOP_SUB;
            r.q[0] = sf64_op(&e, op, a.q[0], a.q[1]);
            r.q[1] = sf64_op(&e, op, b.q[0], b.q[1]);
        }
        }
        sse_commit(c, &e);
        xset(c, in->reg, &r);
        return;
    }
    case I_DPPS: case I_DPPD: {
        FxEnv e = sse_env(c);
        unsigned imm = (unsigned)in->imm;
        xget(c, in->reg, &a);
        get(c, in, 1, &b, 0);
        memset(&r, 0, sizeof r);
        if (id == I_DPPS) {
            uint32_t t[4];
            for (int i = 0; i < 4; i++) t[i] = (imm >> (4 + i)) & 1 ? sf32_op(&e, SOP_MUL, a.d[i], b.d[i]) : 0;
            uint32_t s = sf32_op(&e, SOP_ADD, sf32_op(&e, SOP_ADD, t[0], t[1]), sf32_op(&e, SOP_ADD, t[2], t[3]));
            for (int i = 0; i < 4; i++) if ((imm >> i) & 1) r.d[i] = s;
        } else {
            uint64_t t[2];
            for (int i = 0; i < 2; i++) t[i] = (imm >> (4 + i)) & 1 ? sf64_op(&e, SOP_MUL, a.q[i], b.q[i]) : 0;
            uint64_t s = sf64_op(&e, SOP_ADD, t[0], t[1]);
            for (int i = 0; i < 2; i++) if ((imm >> i) & 1) r.q[i] = s;
        }
        sse_commit(c, &e);
        xset(c, in->reg, &r);
        return;
    }

    /* ---- SSE4.1 / 4.2 miscellany ---- */
    case I_PTEST:
        xget(c, in->reg, &a);
        get(c, in, 1, &b, 0);
        c->rflags &= ~FL_ARITH;
        if (!((a.q[0] & b.q[0]) | (a.q[1] & b.q[1]))) c->rflags |= FL_ZF;
        if (!((~a.q[0] & b.q[0]) | (~a.q[1] & b.q[1]))) c->rflags |= FL_CF;
        return;
    case I_PMOVSXBW: case I_PMOVSXBD: case I_PMOVSXBQ: case I_PMOVSXWD: case I_PMOVSXWQ: case I_PMOVSXDQ:
    case I_PMOVZXBW: case I_PMOVZXBD: case I_PMOVZXBQ: case I_PMOVZXWD: case I_PMOVZXWQ: case I_PMOVZXDQ: {
        static const struct { int from, to; } sz[6] = {{1, 2}, {1, 4}, {1, 8}, {2, 4}, {2, 8}, {4, 8}};
        int sx = id <= I_PMOVSXDQ, k = sx ? id - I_PMOVSXBW : id - I_PMOVZXBW;
        get(c, in, 1, &b, 0);
        int from = sz[k].from, to = sz[k].to, n = 16 / to;
        for (int i = 0; i < n; i++) {
            uint64_t v = 0;
            memcpy(&v, b.b + i * from, from);
            if (sx) v = sext(v, from);
            memcpy(r.b + i * to, &v, to);
        }
        xset(c, in->reg, &r);
        return;
    }
    case I_PHMINPOSUW: {
        get(c, in, 1, &b, 0);
        int idx = 0;
        for (int i = 1; i < 8; i++) if (b.w[i] < b.w[idx]) idx = i;
        memset(&r, 0, sizeof r);
        r.w[0] = b.w[idx];
        r.w[1] = (uint16_t)idx;
        xset(c, in->reg, &r);
        return;
    }
    case I_PCMPESTRM: case I_PCMPESTRI: case I_PCMPISTRM: case I_PCMPISTRI:
        pcmpstr(c, in);
        return;

    /* ---- crypto ---- */
    case I_AESENC: case I_AESENCLAST: case I_AESDEC: case I_AESDECLAST: case I_AESIMC: case I_AESKEYGENASSIST: {
        aes_init();
        xget(c, in->reg, &a);
        get(c, in, 1, &b, 0);
        switch (id) {
        case I_AESENC: case I_AESENCLAST:
            shift_rows(&a, 0);
            sub_bytes(&a, 0);
            if (id == I_AESENC) mix_columns(&a, 0);
            break;
        case I_AESDEC: case I_AESDECLAST:
            shift_rows(&a, 1);
            sub_bytes(&a, 1);
            if (id == I_AESDEC) mix_columns(&a, 1);
            break;
        case I_AESIMC:
            a = b;
            mix_columns(&a, 1);
            xset(c, in->reg, &a);
            return;
        default: {
            uint32_t rcon = (uint32_t)(in->imm & 0xFF), x1 = sub_word(b.d[1]), x3 = sub_word(b.d[3]);
            a.d[0] = x1;
            a.d[1] = ror32(x1, 8) ^ rcon;
            a.d[2] = x3;
            a.d[3] = ror32(x3, 8) ^ rcon;
            xset(c, in->reg, &a);
            return;
        }
        }
        a.q[0] ^= b.q[0];
        a.q[1] ^= b.q[1];
        xset(c, in->reg, &a);
        return;
    }
    case I_PCLMULQDQ: {
        xget(c, in->reg, &a);
        get(c, in, 1, &b, 0);
        uint64_t x = a.q[in->imm & 1], y = b.q[(in->imm >> 4) & 1], lo = 0, hi = 0;
        for (int i = 0; i < 64; i++)
            if ((y >> i) & 1) {
                lo ^= x << i;
                if (i) hi ^= x >> (64 - i);
            }
        r.q[0] = lo;
        r.q[1] = hi;
        xset(c, in->reg, &r);
        return;
    }
    case I_SHA1RNDS4: case I_SHA1NEXTE: case I_SHA1MSG1: case I_SHA1MSG2:
    case I_SHA256RNDS2: case I_SHA256MSG1: case I_SHA256MSG2:
        xget(c, in->reg, &a);
        get(c, in, 1, &b, 0);
        sha(c, in, &a, &b);
        xset(c, in->reg, &a);
        return;
    }

    /* ---- two-operand packed operations: dst = dst OP src ---- */
    get(c, in, 0, &a, 0);
    get(c, in, 1, &b, 0);
    r = a;
    int half = w / 2;
    switch (id) {
    case I_ANDPS: case I_ANDPD: case I_PAND: r.q[0] = a.q[0] & b.q[0]; r.q[1] = a.q[1] & b.q[1]; break;
    case I_ANDNPS: case I_ANDNPD: case I_PANDN: r.q[0] = ~a.q[0] & b.q[0]; r.q[1] = ~a.q[1] & b.q[1]; break;
    case I_ORPS: case I_ORPD: case I_POR: r.q[0] = a.q[0] | b.q[0]; r.q[1] = a.q[1] | b.q[1]; break;
    case I_XORPS: case I_XORPD: case I_PXOR: r.q[0] = a.q[0] ^ b.q[0]; r.q[1] = a.q[1] ^ b.q[1]; break;

    case I_PADDB: for (int i = 0; i < 16; i++) r.b[i] = a.b[i] + b.b[i]; break;
    case I_PADDW: for (int i = 0; i < 8; i++) r.w[i] = a.w[i] + b.w[i]; break;
    case I_PADDD: for (int i = 0; i < 4; i++) r.d[i] = a.d[i] + b.d[i]; break;
    case I_PADDQ: for (int i = 0; i < 2; i++) r.q[i] = a.q[i] + b.q[i]; break;
    case I_PSUBB: for (int i = 0; i < 16; i++) r.b[i] = a.b[i] - b.b[i]; break;
    case I_PSUBW: for (int i = 0; i < 8; i++) r.w[i] = a.w[i] - b.w[i]; break;
    case I_PSUBD: for (int i = 0; i < 4; i++) r.d[i] = a.d[i] - b.d[i]; break;
    case I_PSUBQ: for (int i = 0; i < 2; i++) r.q[i] = a.q[i] - b.q[i]; break;
    case I_PADDUSB: for (int i = 0; i < 16; i++) r.b[i] = sat_u8(a.b[i] + b.b[i]); break;
    case I_PADDUSW: for (int i = 0; i < 8; i++) r.w[i] = sat_u16(a.w[i] + b.w[i]); break;
    case I_PSUBUSB: for (int i = 0; i < 16; i++) r.b[i] = sat_u8(a.b[i] - b.b[i]); break;
    case I_PSUBUSW: for (int i = 0; i < 8; i++) r.w[i] = sat_u16(a.w[i] - b.w[i]); break;
    case I_PADDSB: for (int i = 0; i < 16; i++) r.sb[i] = sat_s8(a.sb[i] + b.sb[i]); break;
    case I_PADDSW: for (int i = 0; i < 8; i++) r.sw[i] = sat_s16(a.sw[i] + b.sw[i]); break;
    case I_PSUBSB: for (int i = 0; i < 16; i++) r.sb[i] = sat_s8(a.sb[i] - b.sb[i]); break;
    case I_PSUBSW: for (int i = 0; i < 8; i++) r.sw[i] = sat_s16(a.sw[i] - b.sw[i]); break;

    case I_PCMPEQB: for (int i = 0; i < 16; i++) r.b[i] = a.b[i] == b.b[i] ? 0xFF : 0; break;
    case I_PCMPEQW: for (int i = 0; i < 8; i++) r.w[i] = a.w[i] == b.w[i] ? 0xFFFF : 0; break;
    case I_PCMPEQD: for (int i = 0; i < 4; i++) r.d[i] = a.d[i] == b.d[i] ? ~0u : 0; break;
    case I_PCMPEQQ: for (int i = 0; i < 2; i++) r.q[i] = a.q[i] == b.q[i] ? ~0ULL : 0; break;
    case I_PCMPGTB: for (int i = 0; i < 16; i++) r.b[i] = a.sb[i] > b.sb[i] ? 0xFF : 0; break;
    case I_PCMPGTW: for (int i = 0; i < 8; i++) r.w[i] = a.sw[i] > b.sw[i] ? 0xFFFF : 0; break;
    case I_PCMPGTD: for (int i = 0; i < 4; i++) r.d[i] = a.sd[i] > b.sd[i] ? ~0u : 0; break;
    case I_PCMPGTQ: for (int i = 0; i < 2; i++) r.q[i] = a.sq[i] > b.sq[i] ? ~0ULL : 0; break;

    case I_PMINUB: for (int i = 0; i < 16; i++) r.b[i] = a.b[i] < b.b[i] ? a.b[i] : b.b[i]; break;
    case I_PMAXUB: for (int i = 0; i < 16; i++) r.b[i] = a.b[i] > b.b[i] ? a.b[i] : b.b[i]; break;
    case I_PMINSB: for (int i = 0; i < 16; i++) r.sb[i] = a.sb[i] < b.sb[i] ? a.sb[i] : b.sb[i]; break;
    case I_PMAXSB: for (int i = 0; i < 16; i++) r.sb[i] = a.sb[i] > b.sb[i] ? a.sb[i] : b.sb[i]; break;
    case I_PMINSW: for (int i = 0; i < 8; i++) r.sw[i] = a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i]; break;
    case I_PMAXSW: for (int i = 0; i < 8; i++) r.sw[i] = a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i]; break;
    case I_PMINUW: for (int i = 0; i < 8; i++) r.w[i] = a.w[i] < b.w[i] ? a.w[i] : b.w[i]; break;
    case I_PMAXUW: for (int i = 0; i < 8; i++) r.w[i] = a.w[i] > b.w[i] ? a.w[i] : b.w[i]; break;
    case I_PMINSD: for (int i = 0; i < 4; i++) r.sd[i] = a.sd[i] < b.sd[i] ? a.sd[i] : b.sd[i]; break;
    case I_PMAXSD: for (int i = 0; i < 4; i++) r.sd[i] = a.sd[i] > b.sd[i] ? a.sd[i] : b.sd[i]; break;
    case I_PMINUD: for (int i = 0; i < 4; i++) r.d[i] = a.d[i] < b.d[i] ? a.d[i] : b.d[i]; break;
    case I_PMAXUD: for (int i = 0; i < 4; i++) r.d[i] = a.d[i] > b.d[i] ? a.d[i] : b.d[i]; break;
    case I_PAVGB: for (int i = 0; i < 16; i++) r.b[i] = (uint8_t)((a.b[i] + b.b[i] + 1) >> 1); break;
    case I_PAVGW: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)((a.w[i] + b.w[i] + 1) >> 1); break;

    case I_PMULLW: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(a.sw[i] * b.sw[i]); break;
    case I_PMULHW: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)((a.sw[i] * b.sw[i]) >> 16); break;
    case I_PMULHUW: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(((uint32_t)a.w[i] * b.w[i]) >> 16); break;
    case I_PMULHRSW: for (int i = 0; i < 8; i++) r.sw[i] = (int16_t)((((a.sw[i] * b.sw[i]) >> 14) + 1) >> 1); break;
    case I_PMULLD: for (int i = 0; i < 4; i++) r.d[i] = a.d[i] * b.d[i]; break;
    case I_PMULUDQ: for (int i = 0; i < 2; i++) r.q[i] = (uint64_t)a.d[2 * i] * b.d[2 * i]; break;
    case I_PMULDQ: for (int i = 0; i < 2; i++) r.sq[i] = (int64_t)a.sd[2 * i] * b.sd[2 * i]; break;
    case I_PMADDWD:
        for (int i = 0; i < 4; i++)
            r.d[i] = (uint32_t)(a.sw[2 * i] * b.sw[2 * i]) + (uint32_t)(a.sw[2 * i + 1] * b.sw[2 * i + 1]);
        break;
    case I_PMADDUBSW:
        for (int i = 0; i < 8; i++) r.sw[i] = sat_s16(a.b[2 * i] * b.sb[2 * i] + a.b[2 * i + 1] * b.sb[2 * i + 1]);
        break;
    case I_PSADBW:
        for (int h = 0; h < 2; h++) {
            unsigned s = 0;
            for (int i = 0; i < 8; i++) {
                int d = a.b[h * 8 + i] - b.b[h * 8 + i];
                s += (unsigned)(d < 0 ? -d : d);
            }
            r.q[h] = s;
        }
        break;
    case I_MPSADBW: {
        unsigned imm = (unsigned)in->imm;
        int ao = (imm & 4) ? 4 : 0, bo = (imm & 3) * 4;
        for (int i = 0; i < 8; i++) {
            unsigned s = 0;
            for (int j = 0; j < 4; j++) {
                int d = a.b[ao + i + j] - b.b[bo + j];
                s += (unsigned)(d < 0 ? -d : d);
            }
            r.w[i] = (uint16_t)s;
        }
        break;
    }

    case I_PSIGNB: for (int i = 0; i < 16; i++) r.sb[i] = b.sb[i] < 0 ? (int8_t)-a.sb[i] : b.sb[i] ? a.sb[i] : 0; break;
    case I_PSIGNW: for (int i = 0; i < 8; i++) r.sw[i] = b.sw[i] < 0 ? (int16_t)-a.sw[i] : b.sw[i] ? a.sw[i] : 0; break;
    case I_PSIGND: for (int i = 0; i < 4; i++) r.sd[i] = b.sd[i] < 0 ? (int32_t)(0u - (uint32_t)a.sd[i]) : b.sd[i] ? a.sd[i] : 0; break;
    case I_PABSB: for (int i = 0; i < 16; i++) r.b[i] = (uint8_t)(b.sb[i] < 0 ? -b.sb[i] : b.sb[i]); break;
    case I_PABSW: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(b.sw[i] < 0 ? -b.sw[i] : b.sw[i]); break;
    case I_PABSD: for (int i = 0; i < 4; i++) r.d[i] = b.sd[i] < 0 ? 0u - (uint32_t)b.sd[i] : (uint32_t)b.sd[i]; break;

    case I_PSHUFB:
        for (int i = 0; i < w; i++) r.b[i] = (b.b[i] & 0x80) ? 0 : a.b[b.b[i] & (w - 1)];
        break;
    case I_PALIGNR: {
        uint8_t t[32];
        unsigned n = (unsigned)(in->imm & 0xFF);
        memcpy(t, b.b, w);
        memcpy(t + w, a.b, w);
        for (int i = 0; i < w; i++) r.b[i] = i + n < (unsigned)(2 * w) ? t[i + n] : 0;
        break;
    }
    case I_PHADDW: case I_PHSUBW: case I_PHADDSW: case I_PHSUBSW: {
        int n = half / 2; /* results per source */
        for (int i = 0; i < n; i++) {
            int x0 = a.sw[2 * i], x1 = a.sw[2 * i + 1], y0 = b.sw[2 * i], y1 = b.sw[2 * i + 1];
            int ra = (id == I_PHADDW || id == I_PHADDSW) ? x0 + x1 : x0 - x1;
            int rb = (id == I_PHADDW || id == I_PHADDSW) ? y0 + y1 : y0 - y1;
            if (id == I_PHADDSW || id == I_PHSUBSW) { ra = sat_s16(ra); rb = sat_s16(rb); }
            r.w[i] = (uint16_t)ra;
            r.w[n + i] = (uint16_t)rb;
        }
        break;
    }
    case I_PHADDD: case I_PHSUBD: {
        int n = half / 4;
        for (int i = 0; i < n; i++) {
            r.d[i] = id == I_PHADDD ? a.d[2 * i] + a.d[2 * i + 1] : a.d[2 * i] - a.d[2 * i + 1];
            r.d[n + i] = id == I_PHADDD ? b.d[2 * i] + b.d[2 * i + 1] : b.d[2 * i] - b.d[2 * i + 1];
        }
        break;
    }

    case I_PUNPCKLBW: for (int i = 0; i < 8; i++) { r.b[2 * i] = a.b[i]; r.b[2 * i + 1] = b.b[i]; } break;
    case I_PUNPCKHBW: for (int i = 0; i < half; i++) { r.b[2 * i] = a.b[half + i]; r.b[2 * i + 1] = b.b[half + i]; } break;
    case I_PUNPCKLWD: for (int i = 0; i < 4; i++) { r.w[2 * i] = a.w[i]; r.w[2 * i + 1] = b.w[i]; } break;
    case I_PUNPCKHWD: for (int i = 0; i < half / 2; i++) { r.w[2 * i] = a.w[half / 2 + i]; r.w[2 * i + 1] = b.w[half / 2 + i]; } break;
    case I_PUNPCKLDQ: case I_UNPCKLPS: r.d[0] = a.d[0]; r.d[1] = b.d[0]; r.d[2] = a.d[1]; r.d[3] = b.d[1]; break;
    case I_PUNPCKHDQ: case I_UNPCKHPS:
        if (mmx) { r.d[0] = a.d[1]; r.d[1] = b.d[1]; }
        else { r.d[0] = a.d[2]; r.d[1] = b.d[2]; r.d[2] = a.d[3]; r.d[3] = b.d[3]; }
        break;
    case I_PUNPCKLQDQ: case I_UNPCKLPD: r.q[0] = a.q[0]; r.q[1] = b.q[0]; break;
    case I_PUNPCKHQDQ: case I_UNPCKHPD: r.q[0] = a.q[1]; r.q[1] = b.q[1]; break;
    case I_PACKSSWB: {
        int n = half;
        for (int i = 0; i < n; i++) { r.sb[i] = sat_s8(a.sw[i]); r.sb[n + i] = sat_s8(b.sw[i]); }
        break;
    }
    case I_PACKUSWB: {
        int n = half;
        for (int i = 0; i < n; i++) { r.b[i] = sat_u8(a.sw[i]); r.b[n + i] = sat_u8(b.sw[i]); }
        break;
    }
    case I_PACKSSDW: {
        int n = half / 2;
        for (int i = 0; i < n; i++) { r.sw[i] = sat_s16(a.sd[i]); r.sw[n + i] = sat_s16(b.sd[i]); }
        break;
    }
    case I_PACKUSDW:
        for (int i = 0; i < 4; i++) { r.w[i] = sat_u16_32(a.sd[i]); r.w[4 + i] = sat_u16_32(b.sd[i]); }
        break;
    case I_SHUFPS: {
        unsigned imm = (unsigned)in->imm;
        r.d[0] = a.d[imm & 3];
        r.d[1] = a.d[(imm >> 2) & 3];
        r.d[2] = b.d[(imm >> 4) & 3];
        r.d[3] = b.d[(imm >> 6) & 3];
        break;
    }
    case I_SHUFPD:
        r.q[0] = a.q[in->imm & 1];
        r.q[1] = b.q[(in->imm >> 1) & 1];
        break;
    case I_BLENDPS: for (int i = 0; i < 4; i++) if ((in->imm >> i) & 1) r.d[i] = b.d[i]; break;
    case I_BLENDPD: for (int i = 0; i < 2; i++) if ((in->imm >> i) & 1) r.q[i] = b.q[i]; break;
    case I_PBLENDW: for (int i = 0; i < 8; i++) if ((in->imm >> i) & 1) r.w[i] = b.w[i]; break;
    case I_PBLENDVB: case I_BLENDVPS: case I_BLENDVPD: {
        X128 m;
        xget(c, 0, &m);
        if (id == I_PBLENDVB) for (int i = 0; i < 16; i++) { if (m.b[i] & 0x80) r.b[i] = b.b[i]; }
        else if (id == I_BLENDVPS) for (int i = 0; i < 4; i++) { if (m.d[i] >> 31) r.d[i] = b.d[i]; }
        else for (int i = 0; i < 2; i++) if (m.q[i] >> 63) r.q[i] = b.q[i];
        break;
    }
    default:
        unimplemented(c, in);
    }
    put(c, in, 0, &r);
}
