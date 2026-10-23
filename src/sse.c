/* SSE/SSE2 execution unit: data movement, logic and the full SSE2 packed
 * integer set on XMM registers. (Packed/scalar floating point comes with
 * the FPU work.) Legacy-encoded SSE leaves bits 128+ of YMM/ZMM untouched. */
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

static uint64_t mem_addr(CPU *c, const Insn *in, int aligned) {
    uint64_t a = ea_linear(c, in);
    if (aligned && (a & 15)) raise_exc(c, EXC_GP, 0, "#GP misaligned 16-byte SSE access");
    return a;
}

/* Read operand k as 128 bits (xmm register or m128). */
static void rd128(CPU *c, const Insn *in, int k, X128 *v, int aligned) {
    int spec = in->opnd[k];
    if (spec == O_Vx) xget(c, in->reg, v);
    else if (in->mod == 3) xget(c, in->rm, v);
    else rdm128(c, mem_addr(c, in, aligned), v);
}

static void wr128(CPU *c, const Insn *in, int k, const X128 *v, int aligned) {
    int spec = in->opnd[k];
    if (spec == O_Vx) xset(c, in->reg, v);
    else if (in->mod == 3) xset(c, in->rm, v);
    else wrm128(c, mem_addr(c, in, aligned), v);
}

static inline uint8_t sat_u8(int v) { return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v; }
static inline int8_t sat_s8(int v) { return v < -128 ? -128 : v > 127 ? 127 : (int8_t)v; }
static inline uint16_t sat_u16(int v) { return v < 0 ? 0 : v > 65535 ? 65535 : (uint16_t)v; }
static inline int16_t sat_s16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int16_t)v; }

static void packed_shift(CPU *c, const Insn *in) {
    X128 v, cnt;
    uint64_t n;
    int dst;
    if (in->opnd[1] == O_Ib) { /* 66 0F 71/72/73 /r ib: shift xmm(rm) by immediate */
        dst = in->rm;
        n = in->imm & 0xFF;
    } else {
        dst = in->reg;
        rd128(c, in, 1, &cnt, 1);
        n = cnt.q[0];
    }
    xget(c, dst, &v);
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
        X128 r = {{0}};
        if (n < 16)
            for (int i = 0; i < 16; i++) {
                int src = in->id == I_PSRLDQ ? i + (int)n : i - (int)n;
                if (src >= 0 && src < 16) r.b[i] = v.b[src];
            }
        v = r;
        break;
    }
    }
    xset(c, dst, &v);
}

void exec_sse(CPU *c, const Insn *in) {
    X128 a, b, r;
    int id = in->id;

    switch (id) {
    /* ---- moves ---- */
    case I_MOVUPS: case I_MOVUPD: case I_MOVDQU:
        rd128(c, in, 1, &a, 0);
        wr128(c, in, 0, &a, 0);
        return;
    case I_MOVAPS: case I_MOVAPD: case I_MOVDQA: case I_MOVNTPS: case I_MOVNTPD: case I_MOVNTDQ:
        if ((id == I_MOVNTPS || id == I_MOVNTPD || id == I_MOVNTDQ) && in->mod == 3) raise_exc(c, EXC_UD, 0, NULL);
        rd128(c, in, 1, &a, 1);
        wr128(c, in, 0, &a, 1);
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
        if (in->opnd[0] == O_Vx) {
            uint64_t v = rd_op(c, in, 1);
            memset(&r, 0, sizeof r);
            r.q[0] = v;
            xset(c, in->reg, &r);
        } else {
            xget(c, in->reg, &a);
            wr_op(c, in, 0, a.q[0]);
        }
        return;
    case I_MOVQ:
        if (in->opnd[0] == O_Vx) { /* F3 0F 7E: load qword, zero upper */
            memset(&r, 0, sizeof r);
            if (in->mod == 3) {
                xget(c, in->rm, &b);
                r.q[0] = b.q[0];
            } else {
                r.q[0] = rdm(c, ea_linear(c, in), 8);
            }
            xset(c, in->reg, &r);
        } else { /* 66 0F D6: store qword */
            xget(c, in->reg, &a);
            if (in->mod == 3) {
                memset(&r, 0, sizeof r);
                r.q[0] = a.q[0];
                xset(c, in->rm, &r);
            } else {
                wrm(c, ea_linear(c, in), 8, a.q[0]);
            }
        }
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
    case I_MOVMSKPS: case I_MOVMSKPD: case I_PMOVMSKB: {
        uint32_t mask = 0;
        xget(c, in->rm, &b);
        if (id == I_MOVMSKPS) for (int i = 0; i < 4; i++) mask |= (b.d[i] >> 31) << i;
        else if (id == I_MOVMSKPD) for (int i = 0; i < 2; i++) mask |= (uint32_t)(b.q[i] >> 63) << i;
        else for (int i = 0; i < 16; i++) mask |= (uint32_t)(b.b[i] >> 7) << i;
        setreg(c, in->reg, 4, 1, mask);
        return;
    }
    case I_PEXTRW:
        xget(c, in->rm, &b);
        setreg(c, in->reg, 4, 1, b.w[in->imm & 7]);
        return;
    case I_PINSRW: {
        uint64_t v = rd_op(c, in, 1);
        xget(c, in->reg, &r);
        r.w[in->imm & 7] = (uint16_t)v;
        xset(c, in->reg, &r);
        return;
    }
    case I_MASKMOVDQU: {
        xget(c, in->reg, &a);
        xget(c, in->rm, &b);
        uint64_t base = (c->r[RDI] & MASK(in->addrsize));
        for (int i = 0; i < 16; i++)
            if (b.b[i] & 0x80) wrm(c, linear(c, in->seg >= 0 ? in->seg : DS, (base + i) & MASK(in->addrsize)), 1, a.b[i]);
        return;
    }
    case I_PSHUFD: case I_PSHUFHW: case I_PSHUFLW: {
        rd128(c, in, 1, &b, 1);
        unsigned imm = (unsigned)in->imm;
        r = b;
        if (id == I_PSHUFD) for (int i = 0; i < 4; i++) r.d[i] = b.d[(imm >> (2 * i)) & 3];
        else if (id == I_PSHUFLW) for (int i = 0; i < 4; i++) r.w[i] = b.w[(imm >> (2 * i)) & 3];
        else for (int i = 0; i < 4; i++) r.w[4 + i] = b.w[4 + ((imm >> (2 * i)) & 3)];
        xset(c, in->reg, &r);
        return;
    }
    case I_PSRLW: case I_PSRAW: case I_PSLLW: case I_PSRLD: case I_PSRAD: case I_PSLLD:
    case I_PSRLQ: case I_PSLLQ: case I_PSRLDQ: case I_PSLLDQ:
        packed_shift(c, in);
        return;
    }

    /* ---- two-operand packed operations: xmm(reg) = xmm(reg) OP src ---- */
    xget(c, in->reg, &a);
    rd128(c, in, 1, &b, 1);
    r = a;
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
    case I_PCMPGTB: for (int i = 0; i < 16; i++) r.b[i] = a.sb[i] > b.sb[i] ? 0xFF : 0; break;
    case I_PCMPGTW: for (int i = 0; i < 8; i++) r.w[i] = a.sw[i] > b.sw[i] ? 0xFFFF : 0; break;
    case I_PCMPGTD: for (int i = 0; i < 4; i++) r.d[i] = a.sd[i] > b.sd[i] ? ~0u : 0; break;

    case I_PMINUB: for (int i = 0; i < 16; i++) r.b[i] = a.b[i] < b.b[i] ? a.b[i] : b.b[i]; break;
    case I_PMAXUB: for (int i = 0; i < 16; i++) r.b[i] = a.b[i] > b.b[i] ? a.b[i] : b.b[i]; break;
    case I_PMINSW: for (int i = 0; i < 8; i++) r.sw[i] = a.sw[i] < b.sw[i] ? a.sw[i] : b.sw[i]; break;
    case I_PMAXSW: for (int i = 0; i < 8; i++) r.sw[i] = a.sw[i] > b.sw[i] ? a.sw[i] : b.sw[i]; break;
    case I_PAVGB: for (int i = 0; i < 16; i++) r.b[i] = (uint8_t)((a.b[i] + b.b[i] + 1) >> 1); break;
    case I_PAVGW: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)((a.w[i] + b.w[i] + 1) >> 1); break;

    case I_PMULLW: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(a.sw[i] * b.sw[i]); break;
    case I_PMULHW: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)((a.sw[i] * b.sw[i]) >> 16); break;
    case I_PMULHUW: for (int i = 0; i < 8; i++) r.w[i] = (uint16_t)(((uint32_t)a.w[i] * b.w[i]) >> 16); break;
    case I_PMULUDQ: for (int i = 0; i < 2; i++) r.q[i] = (uint64_t)a.d[2 * i] * b.d[2 * i]; break;
    case I_PMADDWD:
        for (int i = 0; i < 4; i++)
            r.d[i] = (uint32_t)(a.sw[2 * i] * b.sw[2 * i]) + (uint32_t)(a.sw[2 * i + 1] * b.sw[2 * i + 1]);
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

    case I_PUNPCKLBW: for (int i = 0; i < 8; i++) { r.b[2 * i] = a.b[i]; r.b[2 * i + 1] = b.b[i]; } break;
    case I_PUNPCKHBW: for (int i = 0; i < 8; i++) { r.b[2 * i] = a.b[8 + i]; r.b[2 * i + 1] = b.b[8 + i]; } break;
    case I_PUNPCKLWD: for (int i = 0; i < 4; i++) { r.w[2 * i] = a.w[i]; r.w[2 * i + 1] = b.w[i]; } break;
    case I_PUNPCKHWD: for (int i = 0; i < 4; i++) { r.w[2 * i] = a.w[4 + i]; r.w[2 * i + 1] = b.w[4 + i]; } break;
    case I_PUNPCKLDQ: case I_UNPCKLPS: r.d[0] = a.d[0]; r.d[1] = b.d[0]; r.d[2] = a.d[1]; r.d[3] = b.d[1]; break;
    case I_PUNPCKHDQ: case I_UNPCKHPS: r.d[0] = a.d[2]; r.d[1] = b.d[2]; r.d[2] = a.d[3]; r.d[3] = b.d[3]; break;
    case I_PUNPCKLQDQ: case I_UNPCKLPD: r.q[0] = a.q[0]; r.q[1] = b.q[0]; break;
    case I_PUNPCKHQDQ: case I_UNPCKHPD: r.q[0] = a.q[1]; r.q[1] = b.q[1]; break;
    case I_PACKSSWB:
        for (int i = 0; i < 8; i++) { r.sb[i] = sat_s8(a.sw[i]); r.sb[8 + i] = sat_s8(b.sw[i]); }
        break;
    case I_PACKUSWB:
        for (int i = 0; i < 8; i++) { r.b[i] = sat_u8(a.sw[i]); r.b[8 + i] = sat_u8(b.sw[i]); }
        break;
    case I_PACKSSDW:
        for (int i = 0; i < 4; i++) { r.sw[i] = sat_s16(a.sd[i]); r.sw[4 + i] = sat_s16(b.sd[i]); }
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
    default:
        unimplemented(c, in);
    }
    xset(c, in->reg, &r);
}
