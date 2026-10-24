#include "decode.h"

#include <string.h>

#include "cpu.h"

const char *const mnemonic_names[I_COUNT] = {
#define X(id, name) name,
    MNEMONICS
#undef X
};

const char *const cc_names[16] = {"o", "no", "b", "ae", "e", "ne", "be", "a",
                                  "s", "ns", "p", "np", "l", "ge", "le", "g"};

/* Opcode definition flags */
enum {
    F_D64 = 1 << 0,   /* default 64-bit operand size in long mode */
    F_F64 = 1 << 1,   /* forced 64-bit operand size in long mode */
    F_I64 = 1 << 2,   /* invalid in long mode */
    F_GRP = 1 << 3,   /* ModRM.reg selects from a group table (grp field) */
    F_PFX = 1 << 4,   /* mandatory prefix selects from a prefix table (grp field) */
    F_MODRM = 1 << 5, /* has ModRM even without ModRM operands */
    F_LOCK = 1 << 6,  /* LOCK prefix allowed (memory destination) */
    F_REG = 1 << 7,   /* ModRM always names registers (mod is ignored) */
};

typedef struct {
    uint16_t id;
    uint8_t o[3];
    uint16_t f;
    uint8_t grp;
} OpDef;

#define OP0(i, f) {I_##i, {0, 0, 0}, (f), 0}
#define OP1(i, a, f) {I_##i, {O_##a, 0, 0}, (f), 0}
#define OP2(i, a, b, f) {I_##i, {O_##a, O_##b, 0}, (f), 0}
#define OP3(i, a, b, c, f) {I_##i, {O_##a, O_##b, O_##c}, (f), 0}
#define GR(g, a, b, f) {0, {O_##a, O_##b, 0}, (f) | F_GRP, g}
#define PF {0, {0, 0, 0}, F_PFX, 0}

#define ALU6(base, id) \
    [base + 0] = OP2(id, Eb, Gb, F_LOCK), [base + 1] = OP2(id, Ev, Gv, F_LOCK), \
    [base + 2] = OP2(id, Gb, Eb, 0), [base + 3] = OP2(id, Gv, Ev, 0), \
    [base + 4] = OP2(id, AL, Ib, 0), [base + 5] = OP2(id, rAX, Iz, 0)

enum { G1 = 1, G1A, G2, G3B, G3V, G4, G5, G11, G6, G7, G8, G9, G15, G12, G13, G14, NGROUPS };

static const OpDef map1[256] = {
    ALU6(0x00, ADD), [0x06] = OP1(PUSH, ES, F_I64), [0x07] = OP1(POP, ES, F_I64),
    ALU6(0x08, OR),  [0x0E] = OP1(PUSH, CS, F_I64),
    ALU6(0x10, ADC), [0x16] = OP1(PUSH, SS, F_I64), [0x17] = OP1(POP, SS, F_I64),
    ALU6(0x18, SBB), [0x1E] = OP1(PUSH, DS, F_I64), [0x1F] = OP1(POP, DS, F_I64),
    ALU6(0x20, AND), [0x27] = OP0(DAA, F_I64),
    ALU6(0x28, SUB), [0x2F] = OP0(DAS, F_I64),
    ALU6(0x30, XOR), [0x37] = OP0(AAA, F_I64),
    ALU6(0x38, CMP), [0x3F] = OP0(AAS, F_I64),
    [0x40 ... 0x47] = OP1(INC, Zv, F_I64),
    [0x48 ... 0x4F] = OP1(DEC, Zv, F_I64),
    [0x50 ... 0x57] = OP1(PUSH, Zv, F_D64),
    [0x58 ... 0x5F] = OP1(POP, Zv, F_D64),
    [0x60] = OP0(PUSHA, F_I64), [0x61] = OP0(POPA, F_I64), [0x62] = OP2(BOUND, Gv, M, F_I64),
    [0x63] = OP2(MOVSXD, Gv, Ed, 0),
    [0x68] = OP1(PUSH, Iz, F_D64), [0x69] = OP3(IMUL, Gv, Ev, Iz, 0),
    [0x6A] = OP1(PUSH, Ibs, F_D64), [0x6B] = OP3(IMUL, Gv, Ev, Ibs, 0),
    [0x6C] = OP2(INS, Yb, DX, 0), [0x6D] = OP2(INS, Yv, DX, 0),
    [0x6E] = OP2(OUTS, DX, Xb, 0), [0x6F] = OP2(OUTS, DX, Xv, 0),
    [0x70 ... 0x7F] = OP1(JCC, Jb, F_F64),
    [0x80] = GR(G1, Eb, Ib, F_LOCK), [0x81] = GR(G1, Ev, Iz, F_LOCK),
    [0x82] = GR(G1, Eb, Ib, F_I64 | F_LOCK), [0x83] = GR(G1, Ev, Ibs, F_LOCK),
    [0x84] = OP2(TEST, Eb, Gb, 0), [0x85] = OP2(TEST, Ev, Gv, 0),
    [0x86] = OP2(XCHG, Eb, Gb, F_LOCK), [0x87] = OP2(XCHG, Ev, Gv, F_LOCK),
    [0x88] = OP2(MOV, Eb, Gb, 0), [0x89] = OP2(MOV, Ev, Gv, 0),
    [0x8A] = OP2(MOV, Gb, Eb, 0), [0x8B] = OP2(MOV, Gv, Ev, 0),
    [0x8C] = OP2(MOV, Ev, Sw, 0), [0x8D] = OP2(LEA, Gv, M, 0),
    [0x8E] = OP2(MOV, Sw, Ew, 0), [0x8F] = GR(G1A, NONE, NONE, F_D64),
    [0x90] = OP0(NOP, 0), [0x91 ... 0x97] = OP2(XCHG, Zv, rAX, 0),
    [0x98] = OP0(CBW, 0), [0x99] = OP0(CWD, 0), [0x9A] = OP1(CALLF, Ap, F_I64), [0x9B] = OP0(FWAIT, 0),
    [0x9C] = OP0(PUSHF, F_D64), [0x9D] = OP0(POPF, F_D64), [0x9E] = OP0(SAHF, 0), [0x9F] = OP0(LAHF, 0),
    [0xA0] = OP2(MOV, AL, Ob, 0), [0xA1] = OP2(MOV, rAX, Ov, 0),
    [0xA2] = OP2(MOV, Ob, AL, 0), [0xA3] = OP2(MOV, Ov, rAX, 0),
    [0xA4] = OP2(MOVS, Yb, Xb, 0), [0xA5] = OP2(MOVS, Yv, Xv, 0),
    [0xA6] = OP2(CMPS, Xb, Yb, 0), [0xA7] = OP2(CMPS, Xv, Yv, 0),
    [0xA8] = OP2(TEST, AL, Ib, 0), [0xA9] = OP2(TEST, rAX, Iz, 0),
    [0xAA] = OP2(STOS, Yb, AL, 0), [0xAB] = OP2(STOS, Yv, rAX, 0),
    [0xAC] = OP2(LODS, AL, Xb, 0), [0xAD] = OP2(LODS, rAX, Xv, 0),
    [0xAE] = OP2(SCAS, AL, Yb, 0), [0xAF] = OP2(SCAS, rAX, Yv, 0),
    [0xB0 ... 0xB7] = OP2(MOV, Zb, Ib, 0),
    [0xB8 ... 0xBF] = OP2(MOV, Zv, Iv, 0),
    [0xC0] = GR(G2, Eb, Ib, 0), [0xC1] = GR(G2, Ev, Ib, 0),
    [0xC2] = OP1(RET, Iw, F_F64), [0xC3] = OP0(RET, F_F64),
    [0xC4] = OP2(LES, Gv, Mp, F_I64), [0xC5] = OP2(LDS, Gv, Mp, F_I64),
    [0xC6] = GR(G11, Eb, Ib, 0), [0xC7] = GR(G11, Ev, Iz, 0),
    [0xC8] = OP2(ENTER, Iw, Ib2, F_D64), [0xC9] = OP0(LEAVE, F_D64),
    [0xCA] = OP1(RETF, Iw, 0), [0xCB] = OP0(RETF, 0),
    [0xCC] = OP0(INT3, 0), [0xCD] = OP1(INT, Ib, 0), [0xCE] = OP0(INTO, F_I64), [0xCF] = OP0(IRET, 0),
    [0xD0] = GR(G2, Eb, 1, 0), [0xD1] = GR(G2, Ev, 1, 0),
    [0xD2] = GR(G2, Eb, CL, 0), [0xD3] = GR(G2, Ev, CL, 0),
    [0xD4] = OP1(AAM, Ib, F_I64), [0xD5] = OP1(AAD, Ib, F_I64), [0xD6] = OP0(SALC, F_I64), [0xD7] = OP0(XLAT, 0),
    [0xD8 ... 0xDF] = OP0(X87, F_MODRM),
    [0xE0] = OP1(LOOPNE, Jb, F_F64), [0xE1] = OP1(LOOPE, Jb, F_F64),
    [0xE2] = OP1(LOOP, Jb, F_F64), [0xE3] = OP1(JRCXZ, Jb, F_F64),
    [0xE4] = OP2(IN, AL, Ib, 0), [0xE5] = OP2(IN, eAX, Ib, 0),
    [0xE6] = OP2(OUT, Ib, AL, 0), [0xE7] = OP2(OUT, Ib, eAX, 0),
    [0xE8] = OP1(CALL, Jz, F_F64), [0xE9] = OP1(JMP, Jz, F_F64),
    [0xEA] = OP1(JMPF, Ap, F_I64), [0xEB] = OP1(JMP, Jb, F_F64),
    [0xEC] = OP2(IN, AL, DX, 0), [0xED] = OP2(IN, eAX, DX, 0),
    [0xEE] = OP2(OUT, DX, AL, 0), [0xEF] = OP2(OUT, DX, eAX, 0),
    [0xF1] = OP0(INT1, 0), [0xF4] = OP0(HLT, 0), [0xF5] = OP0(CMC, 0),
    [0xF6] = GR(G3B, NONE, NONE, 0), [0xF7] = GR(G3V, NONE, NONE, 0),
    [0xF8] = OP0(CLC, 0), [0xF9] = OP0(STC, 0), [0xFA] = OP0(CLI, 0), [0xFB] = OP0(STI, 0),
    [0xFC] = OP0(CLD, 0), [0xFD] = OP0(STD, 0),
    [0xFE] = GR(G4, NONE, NONE, 0), [0xFF] = GR(G5, NONE, NONE, 0),
};


static const OpDef map0f[256] = {
    [0x00] = GR(G6, NONE, NONE, 0), [0x01] = GR(G7, NONE, NONE, 0),
    [0x02] = OP2(LAR, Gv, Ew, 0), [0x03] = OP2(LSL, Gv, Ew, 0),
    [0x05] = OP0(SYSCALL, 0), [0x06] = OP0(CLTS, 0), [0x07] = OP0(SYSRET, 0),
    [0x08] = OP0(INVD, 0), [0x09] = OP0(WBINVD, 0),
    [0x0B] = OP0(UD2, 0), [0x0D] = OP1(PREFETCH, Ev, 0),
    [0x20] = OP2(MOVCR, Ry, Cd, F_REG), [0x21] = OP2(MOVDR, Ry, Dd, F_REG),
    [0x22] = OP2(MOVCR, Cd, Ry, F_REG), [0x23] = OP2(MOVDR, Dd, Ry, F_REG),
    [0x30] = OP0(WRMSR, 0), [0x32] = OP0(RDMSR, 0), [0x33] = OP0(RDPMC, 0), [0x35] = OP0(SYSEXIT, 0),
    [0xAA] = OP0(RSM, 0), [0xB9] = OP2(UD1, Gv, Ev, 0), [0xFF] = OP2(UD0, Gv, Ev, 0),
    [0x10] = PF, [0x11] = PF,
    [0x18] = OP1(PREFETCH, Ev, 0), [0x19 ... 0x1F] = OP1(NOP, Ev, 0),
    [0x12] = PF, [0x13] = PF, [0x14] = PF, [0x15] = PF,
    [0x16] = PF, [0x17] = PF,
    [0x28] = PF, [0x29] = PF, [0x2A] = PF, [0x2B] = PF, [0x2C] = PF, [0x2D] = PF, [0x2E] = PF, [0x2F] = PF,
    [0x51] = PF, [0x52] = PF, [0x53] = PF, [0x58 ... 0x5F] = PF,
    [0x77] = OP0(EMMS, 0), [0x7C] = PF, [0x7D] = PF, [0xC2] = PF, [0xD0] = PF, [0xE6] = PF, [0xF0] = PF,
    [0x50] = PF,
    [0x60] = PF, [0x61] = PF, [0x62] = PF, [0x63] = PF,
    [0x64] = PF, [0x65] = PF, [0x66] = PF, [0x67] = PF,
    [0x68] = PF, [0x69] = PF, [0x6A] = PF, [0x6B] = PF, [0x6D] = PF,
    [0x71] = PF, [0x72] = PF, [0x73] = PF,
    [0xC3] = OP2(MOVNTI, Ey, Gy, 0), [0xC4] = PF, [0xC5] = PF, [0xC6] = PF,
    [0xD1] = PF, [0xD2] = PF, [0xD3] = PF, [0xD5] = PF,
    [0xD8] = PF, [0xD9] = PF, [0xDA] = PF, [0xDC] = PF, [0xDD] = PF, [0xDE] = PF,
    [0xE0] = PF, [0xE1] = PF, [0xE2] = PF, [0xE3] = PF, [0xE4] = PF, [0xE5] = PF,
    [0xE7] = PF, [0xE8] = PF, [0xE9] = PF, [0xEA] = PF, [0xEC] = PF, [0xED] = PF,
    [0xEE] = PF,
    [0xF1] = PF, [0xF2] = PF, [0xF3] = PF, [0xF4] = PF, [0xF5] = PF, [0xF6] = PF,
    [0xF7] = PF,
    [0x31] = OP0(RDTSC, 0), [0x34] = OP0(SYSENTER, 0),
    [0x40 ... 0x4F] = OP2(CMOVCC, Gv, Ev, 0),
    [0x54] = PF, [0x55] = PF, [0x56] = PF, [0x57] = PF,
    [0x6C] = PF, [0x6E] = PF, [0x6F] = PF,
    [0x70] = PF, [0x74] = PF, [0x75] = PF, [0x76] = PF,
    [0x7E] = PF, [0x7F] = PF,
    [0x80 ... 0x8F] = OP1(JCC, Jz, F_F64),
    [0x90 ... 0x9F] = OP1(SETCC, Eb, 0),
    [0xA0] = OP1(PUSH, FS, F_D64), [0xA1] = OP1(POP, FS, F_D64), [0xA2] = OP0(CPUID, 0),
    [0xA3] = OP2(BT, Ev, Gv, 0), [0xA4] = OP3(SHLD, Ev, Gv, Ib, 0), [0xA5] = OP3(SHLD, Ev, Gv, CL, 0),
    [0xA8] = OP1(PUSH, GS, F_D64), [0xA9] = OP1(POP, GS, F_D64),
    [0xAB] = OP2(BTS, Ev, Gv, F_LOCK), [0xAC] = OP3(SHRD, Ev, Gv, Ib, 0), [0xAD] = OP3(SHRD, Ev, Gv, CL, 0),
    [0xAE] = GR(G15, NONE, NONE, 0), [0xAF] = OP2(IMUL, Gv, Ev, 0),
    [0xB0] = OP2(CMPXCHG, Eb, Gb, F_LOCK), [0xB1] = OP2(CMPXCHG, Ev, Gv, F_LOCK),
    [0xB2] = OP2(LSS, Gv, Mp, 0), [0xB3] = OP2(BTR, Ev, Gv, F_LOCK),
    [0xB4] = OP2(LFS, Gv, Mp, 0), [0xB5] = OP2(LGS, Gv, Mp, 0),
    [0xB6] = OP2(MOVZX, Gv, Eb, 0), [0xB7] = OP2(MOVZX, Gv, Ew, 0),
    [0xB8] = PF, [0xBA] = GR(G8, NONE, NONE, 0), [0xBB] = OP2(BTC, Ev, Gv, F_LOCK),
    [0xBC] = PF, [0xBD] = PF,
    [0xBE] = OP2(MOVSX, Gv, Eb, 0), [0xBF] = OP2(MOVSX, Gv, Ew, 0),
    [0xC0] = OP2(XADD, Eb, Gb, F_LOCK), [0xC1] = OP2(XADD, Ev, Gv, F_LOCK),
    [0xC7] = GR(G9, NONE, NONE, 0),
    [0xC8 ... 0xCF] = OP1(BSWAP, Zv, 0),
    [0xD4] = PF, [0xD6] = PF, [0xD7] = PF, [0xDB] = PF, [0xDF] = PF,
    [0xEB] = PF, [0xEF] = PF,
    [0xF8] = PF, [0xF9] = PF, [0xFA] = PF, [0xFB] = PF,
    [0xFC] = PF, [0xFD] = PF, [0xFE] = PF,
};


/* Mandatory-prefix variants: [none, 66, F3, F2] */
#define SSE2(id, a, b) {{0}, OP2(id, a, b, 0), {0}, {0}}
#define MX(id) {OP2(id, Pq, Qq, 0), OP2(id, Vx, Wx, 0), {0}, {0}}
#define FP4(n) {OP2(n##PS, Vx, Wx, 0), OP2(n##PD, Vx, Wx, 0), OP2(n##SS, Vx, Wd, 0), OP2(n##SD, Vx, Wq, 0)}
static const OpDef pfx0f[256][4] = {
    [0x10] = {OP2(MOVUPS, Vx, Wx, 0), OP2(MOVUPD, Vx, Wx, 0), OP2(MOVSS, Vx, Wd, 0), OP2(MOVSD_X, Vx, Wq, 0)},
    [0x11] = {OP2(MOVUPS, Wx, Vx, 0), OP2(MOVUPD, Wx, Vx, 0), OP2(MOVSS, Wd, Vx, 0), OP2(MOVSD_X, Wq, Vx, 0)},
    [0x12] = {OP2(MOVLPS, Vx, Wq, 0), OP2(MOVLPD, Vx, Wq, 0), OP2(MOVSLDUP, Vx, Wx, 0), OP2(MOVDDUP, Vx, Wq, 0)},
    [0x13] = {OP2(MOVLPS, Wq, Vx, 0), OP2(MOVLPD, Wq, Vx, 0)},
    [0x14] = {OP2(UNPCKLPS, Vx, Wx, 0), OP2(UNPCKLPD, Vx, Wx, 0)},
    [0x15] = {OP2(UNPCKHPS, Vx, Wx, 0), OP2(UNPCKHPD, Vx, Wx, 0)},
    [0x16] = {OP2(MOVHPS, Vx, Wq, 0), OP2(MOVHPD, Vx, Wq, 0), OP2(MOVSHDUP, Vx, Wx, 0)},
    [0x17] = {OP2(MOVHPS, Wq, Vx, 0), OP2(MOVHPD, Wq, Vx, 0)},
    [0x28] = {OP2(MOVAPS, Vx, Wx, 0), OP2(MOVAPD, Vx, Wx, 0)},
    [0x29] = {OP2(MOVAPS, Wx, Vx, 0), OP2(MOVAPD, Wx, Vx, 0)},
    [0x2A] = {OP2(CVTPI2PS, Vx, Qq, 0), OP2(CVTPI2PD, Vx, Qq, 0), OP2(CVTSI2SS, Vx, Ey, 0), OP2(CVTSI2SD, Vx, Ey, 0)},
    [0x2B] = {OP2(MOVNTPS, Wx, Vx, 0), OP2(MOVNTPD, Wx, Vx, 0)},
    [0x2C] = {OP2(CVTTPS2PI, Pq, Wq, 0), OP2(CVTTPD2PI, Pq, Wx, 0), OP2(CVTTSS2SI, Gy, Wd, 0), OP2(CVTTSD2SI, Gy, Wq, 0)},
    [0x2D] = {OP2(CVTPS2PI, Pq, Wq, 0), OP2(CVTPD2PI, Pq, Wx, 0), OP2(CVTSS2SI, Gy, Wd, 0), OP2(CVTSD2SI, Gy, Wq, 0)},
    [0x2E] = {OP2(UCOMISS, Vx, Wd, 0), OP2(UCOMISD, Vx, Wq, 0)},
    [0x2F] = {OP2(COMISS, Vx, Wd, 0), OP2(COMISD, Vx, Wq, 0)},
    [0x50] = {OP2(MOVMSKPS, Gd, Ux, 0), OP2(MOVMSKPD, Gd, Ux, 0)},
    [0x51] = FP4(SQRT),
    [0x52] = {OP2(RSQRTPS, Vx, Wx, 0), {0}, OP2(RSQRTSS, Vx, Wd, 0)},
    [0x53] = {OP2(RCPPS, Vx, Wx, 0), {0}, OP2(RCPSS, Vx, Wd, 0)},
    [0x54] = {OP2(ANDPS, Vx, Wx, 0), OP2(ANDPD, Vx, Wx, 0)},
    [0x55] = {OP2(ANDNPS, Vx, Wx, 0), OP2(ANDNPD, Vx, Wx, 0)},
    [0x56] = {OP2(ORPS, Vx, Wx, 0), OP2(ORPD, Vx, Wx, 0)},
    [0x57] = {OP2(XORPS, Vx, Wx, 0), OP2(XORPD, Vx, Wx, 0)},
    [0x58] = FP4(ADD), [0x59] = FP4(MUL),
    [0x5A] = {OP2(CVTPS2PD, Vx, Wq, 0), OP2(CVTPD2PS, Vx, Wx, 0), OP2(CVTSS2SD, Vx, Wd, 0), OP2(CVTSD2SS, Vx, Wq, 0)},
    [0x5B] = {OP2(CVTDQ2PS, Vx, Wx, 0), OP2(CVTPS2DQ, Vx, Wx, 0), OP2(CVTTPS2DQ, Vx, Wx, 0)},
    [0x5C] = FP4(SUB), [0x5D] = FP4(MIN), [0x5E] = FP4(DIV), [0x5F] = FP4(MAX),
    [0x60] = MX(PUNPCKLBW), [0x61] = MX(PUNPCKLWD), [0x62] = MX(PUNPCKLDQ), [0x63] = MX(PACKSSWB),
    [0x64] = MX(PCMPGTB), [0x65] = MX(PCMPGTW), [0x66] = MX(PCMPGTD), [0x67] = MX(PACKUSWB),
    [0x68] = MX(PUNPCKHBW), [0x69] = MX(PUNPCKHWD), [0x6A] = MX(PUNPCKHDQ), [0x6B] = MX(PACKSSDW),
    [0x6C] = SSE2(PUNPCKLQDQ, Vx, Wx), [0x6D] = SSE2(PUNPCKHQDQ, Vx, Wx),
    [0x6E] = {OP2(MOVD, Pq, Ey, 0), OP2(MOVD, Vx, Ey, 0)},
    [0x6F] = {OP2(MOVQ, Pq, Qq, 0), OP2(MOVDQA, Vx, Wx, 0), OP2(MOVDQU, Vx, Wx, 0)},
    [0x70] = {OP3(PSHUFW, Pq, Qq, Ib, 0), OP3(PSHUFD, Vx, Wx, Ib, 0), OP3(PSHUFHW, Vx, Wx, Ib, 0), OP3(PSHUFLW, Vx, Wx, Ib, 0)},
    [0x71] = {GR(G12, Nq, Ib, 0), GR(G12, Ux, Ib, 0)},
    [0x72] = {GR(G13, Nq, Ib, 0), GR(G13, Ux, Ib, 0)},
    [0x73] = {GR(G14, Nq, Ib, 0), GR(G14, Ux, Ib, 0)},
    [0x74] = MX(PCMPEQB), [0x75] = MX(PCMPEQW), [0x76] = MX(PCMPEQD),
    [0x7C] = {{0}, OP2(HADDPD, Vx, Wx, 0), {0}, OP2(HADDPS, Vx, Wx, 0)},
    [0x7D] = {{0}, OP2(HSUBPD, Vx, Wx, 0), {0}, OP2(HSUBPS, Vx, Wx, 0)},
    [0x7E] = {OP2(MOVD, Ey, Pq, 0), OP2(MOVD, Ey, Vx, 0), OP2(MOVQ, Vx, Wq, 0)},
    [0x7F] = {OP2(MOVQ, Qq, Pq, 0), OP2(MOVDQA, Wx, Vx, 0), OP2(MOVDQU, Wx, Vx, 0)},
    [0xB8] = {{0}, {0}, OP2(POPCNT, Gv, Ev, 0), {0}},
    [0xBC] = {OP2(BSF, Gv, Ev, 0), OP2(BSF, Gv, Ev, 0), OP2(TZCNT, Gv, Ev, 0), OP2(BSF, Gv, Ev, 0)},
    [0xBD] = {OP2(BSR, Gv, Ev, 0), OP2(BSR, Gv, Ev, 0), OP2(LZCNT, Gv, Ev, 0), OP2(BSR, Gv, Ev, 0)},
    [0xC2] = {OP3(CMPPS, Vx, Wx, Ib, 0), OP3(CMPPD, Vx, Wx, Ib, 0), OP3(CMPSS, Vx, Wd, Ib, 0), OP3(CMPSD_X, Vx, Wq, Ib, 0)},
    [0xC4] = {OP3(PINSRW, Pq, Ew, Ib, 0), OP3(PINSRW, Vx, Ew, Ib, 0)},
    [0xC5] = {OP3(PEXTRW, Gd, Nq, Ib, 0), OP3(PEXTRW, Gd, Ux, Ib, 0)},
    [0xC6] = {OP3(SHUFPS, Vx, Wx, Ib, 0), OP3(SHUFPD, Vx, Wx, Ib, 0)},
    [0xD0] = {{0}, OP2(ADDSUBPD, Vx, Wx, 0), {0}, OP2(ADDSUBPS, Vx, Wx, 0)},
    [0xD1] = MX(PSRLW), [0xD2] = MX(PSRLD), [0xD3] = MX(PSRLQ), [0xD4] = MX(PADDQ), [0xD5] = MX(PMULLW),
    [0xD6] = {{0}, OP2(MOVQ, Wq, Vx, 0), OP2(MOVQ2DQ, Vx, Nq, 0), OP2(MOVDQ2Q, Pq, Ux, 0)},
    [0xD7] = {OP2(PMOVMSKB, Gd, Nq, 0), OP2(PMOVMSKB, Gd, Ux, 0)},
    [0xD8] = MX(PSUBUSB), [0xD9] = MX(PSUBUSW), [0xDA] = MX(PMINUB), [0xDB] = MX(PAND),
    [0xDC] = MX(PADDUSB), [0xDD] = MX(PADDUSW), [0xDE] = MX(PMAXUB), [0xDF] = MX(PANDN),
    [0xE0] = MX(PAVGB), [0xE1] = MX(PSRAW), [0xE2] = MX(PSRAD), [0xE3] = MX(PAVGW),
    [0xE4] = MX(PMULHUW), [0xE5] = MX(PMULHW),
    [0xE6] = {{0}, OP2(CVTTPD2DQ, Vx, Wx, 0), OP2(CVTDQ2PD, Vx, Wq, 0), OP2(CVTPD2DQ, Vx, Wx, 0)},
    [0xE7] = {OP2(MOVNTQ, Qq, Pq, 0), OP2(MOVNTDQ, Wx, Vx, 0)},
    [0xE8] = MX(PSUBSB), [0xE9] = MX(PSUBSW), [0xEA] = MX(PMINSW), [0xEB] = MX(POR),
    [0xEC] = MX(PADDSB), [0xED] = MX(PADDSW), [0xEE] = MX(PMAXSW), [0xEF] = MX(PXOR),
    [0xF0] = {{0}, {0}, {0}, OP2(LDDQU, Vx, M, 0)},
    [0xF1] = MX(PSLLW), [0xF2] = MX(PSLLD), [0xF3] = MX(PSLLQ), [0xF4] = MX(PMULUDQ),
    [0xF5] = MX(PMADDWD), [0xF6] = MX(PSADBW),
    [0xF7] = {OP2(MASKMOVQ, Pq, Nq, 0), OP2(MASKMOVDQU, Vx, Ux, 0)},
    [0xF8] = MX(PSUBB), [0xF9] = MX(PSUBW), [0xFA] = MX(PSUBD), [0xFB] = MX(PSUBQ),
    [0xFC] = MX(PADDB), [0xFD] = MX(PADDW), [0xFE] = MX(PADDD),
};

/* 0F 38 xx and 0F 3A xx (all entries are mandatory-prefix selected) */
#define S66(id, a, b) {{0}, OP2(id, a, b, 0)}
#define S66I(id, a, b) {{0}, OP3(id, a, b, Ib, 0)}
static const OpDef pfx0f38[256][4] = {
    [0xF5] = {{0}, OP2(WRUSS, Ey, Gy, 0)},
    [0xF6] = {OP2(WRSS, Ey, Gy, 0), OP2(ADCX, Gy, Ey, 0), OP2(ADOX, Gy, Ey, 0)},
    [0x00] = MX(PSHUFB), [0x01] = MX(PHADDW), [0x02] = MX(PHADDD), [0x03] = MX(PHADDSW),
    [0x04] = MX(PMADDUBSW), [0x05] = MX(PHSUBW), [0x06] = MX(PHSUBD), [0x07] = MX(PHSUBSW),
    [0x08] = MX(PSIGNB), [0x09] = MX(PSIGNW), [0x0A] = MX(PSIGND), [0x0B] = MX(PMULHRSW),
    [0x10] = S66(PBLENDVB, Vx, Wx), [0x14] = S66(BLENDVPS, Vx, Wx), [0x15] = S66(BLENDVPD, Vx, Wx),
    [0x17] = S66(PTEST, Vx, Wx),
    [0x1C] = MX(PABSB), [0x1D] = MX(PABSW), [0x1E] = MX(PABSD),
    [0x20] = S66(PMOVSXBW, Vx, Wq), [0x21] = S66(PMOVSXBD, Vx, Wd), [0x22] = S66(PMOVSXBQ, Vx, Ww),
    [0x23] = S66(PMOVSXWD, Vx, Wq), [0x24] = S66(PMOVSXWQ, Vx, Wd), [0x25] = S66(PMOVSXDQ, Vx, Wq),
    [0x28] = S66(PMULDQ, Vx, Wx), [0x29] = S66(PCMPEQQ, Vx, Wx), [0x2A] = S66(MOVNTDQA, Vx, M),
    [0x2B] = S66(PACKUSDW, Vx, Wx),
    [0x30] = S66(PMOVZXBW, Vx, Wq), [0x31] = S66(PMOVZXBD, Vx, Wd), [0x32] = S66(PMOVZXBQ, Vx, Ww),
    [0x33] = S66(PMOVZXWD, Vx, Wq), [0x34] = S66(PMOVZXWQ, Vx, Wd), [0x35] = S66(PMOVZXDQ, Vx, Wq),
    [0x37] = S66(PCMPGTQ, Vx, Wx),
    [0x38] = S66(PMINSB, Vx, Wx), [0x39] = S66(PMINSD, Vx, Wx), [0x3A] = S66(PMINUW, Vx, Wx),
    [0x3B] = S66(PMINUD, Vx, Wx), [0x3C] = S66(PMAXSB, Vx, Wx), [0x3D] = S66(PMAXSD, Vx, Wx),
    [0x3E] = S66(PMAXUW, Vx, Wx), [0x3F] = S66(PMAXUD, Vx, Wx),
    [0x40] = S66(PMULLD, Vx, Wx), [0x41] = S66(PHMINPOSUW, Vx, Wx),
    [0xC8] = {OP2(SHA1NEXTE, Vx, Wx, 0)}, [0xC9] = {OP2(SHA1MSG1, Vx, Wx, 0)},
    [0xCA] = {OP2(SHA1MSG2, Vx, Wx, 0)}, [0xCB] = {OP2(SHA256RNDS2, Vx, Wx, 0)},
    [0xCC] = {OP2(SHA256MSG1, Vx, Wx, 0)}, [0xCD] = {OP2(SHA256MSG2, Vx, Wx, 0)},
    [0xDB] = S66(AESIMC, Vx, Wx), [0xDC] = S66(AESENC, Vx, Wx), [0xDD] = S66(AESENCLAST, Vx, Wx),
    [0xDE] = S66(AESDEC, Vx, Wx), [0xDF] = S66(AESDECLAST, Vx, Wx),
    [0x82] = {{0}, OP2(INVPCID, Gy, M, 0)},
    [0xF0] = {OP2(MOVBE, Gv, Ev, 0), OP2(MOVBE, Gv, Ev, 0), {0}, OP2(CRC32, Gy, Eb, 0)},
    [0xF1] = {OP2(MOVBE, Ev, Gv, 0), OP2(MOVBE, Ev, Gv, 0), {0}, OP2(CRC32, Gy, Ev, 0)},
};

static const OpDef pfx0f3a[256][4] = {
    [0x08] = S66I(ROUNDPS, Vx, Wx), [0x09] = S66I(ROUNDPD, Vx, Wx),
    [0x0A] = S66I(ROUNDSS, Vx, Wd), [0x0B] = S66I(ROUNDSD, Vx, Wq),
    [0x0C] = S66I(BLENDPS, Vx, Wx), [0x0D] = S66I(BLENDPD, Vx, Wx), [0x0E] = S66I(PBLENDW, Vx, Wx),
    [0x0F] = {OP3(PALIGNR, Pq, Qq, Ib, 0), OP3(PALIGNR, Vx, Wx, Ib, 0)},
    [0x14] = S66I(PEXTRB, Ed, Vx), [0x15] = S66I(PEXTRW, Ed, Vx), [0x16] = S66I(PEXTRD, Ey, Vx),
    [0x17] = S66I(EXTRACTPS, Ed, Vx),
    [0x20] = S66I(PINSRB, Vx, Ed), [0x21] = S66I(INSERTPS, Vx, Wd), [0x22] = S66I(PINSRD, Vx, Ey),
    [0x40] = S66I(DPPS, Vx, Wx), [0x41] = S66I(DPPD, Vx, Wx), [0x42] = S66I(MPSADBW, Vx, Wx),
    [0x44] = S66I(PCLMULQDQ, Vx, Wx),
    [0x60] = S66I(PCMPESTRM, Vx, Wx), [0x61] = S66I(PCMPESTRI, Vx, Wx),
    [0x62] = S66I(PCMPISTRM, Vx, Wx), [0x63] = S66I(PCMPISTRI, Vx, Wx),
    [0xCC] = {OP3(SHA1RNDS4, Vx, Wx, Ib, 0)},
    [0xDF] = S66I(AESKEYGENASSIST, Vx, Wx),
};

/* Group tables: [group][0 = memory form, 1 = register form][ModRM.reg].
 * An entry without operands inherits them from the primary opcode. */
#define SAME(...) {{__VA_ARGS__}, {__VA_ARGS__}}
static const OpDef groups[NGROUPS][2][8] = {
    [G1] = SAME(OP0(ADD, 0), OP0(OR, 0), OP0(ADC, 0), OP0(SBB, 0),
                OP0(AND, 0), OP0(SUB, 0), OP0(XOR, 0), OP0(CMP, 0)),
    [G1A] = SAME(OP1(POP, Ev, 0)),
    [G2] = SAME(OP0(ROL, 0), OP0(ROR, 0), OP0(RCL, 0), OP0(RCR, 0),
                OP0(SHL, 0), OP0(SHR, 0), OP0(SAL, 0), OP0(SAR, 0)),
    [G3B] = SAME(OP2(TEST, Eb, Ib, 0), OP2(TEST, Eb, Ib, 0), OP1(NOT, Eb, F_LOCK), OP1(NEG, Eb, F_LOCK),
                 OP1(MUL, Eb, 0), OP1(IMUL, Eb, 0), OP1(DIV, Eb, 0), OP1(IDIV, Eb, 0)),
    [G3V] = SAME(OP2(TEST, Ev, Iz, 0), OP2(TEST, Ev, Iz, 0), OP1(NOT, Ev, F_LOCK), OP1(NEG, Ev, F_LOCK),
                 OP1(MUL, Ev, 0), OP1(IMUL, Ev, 0), OP1(DIV, Ev, 0), OP1(IDIV, Ev, 0)),
    [G4] = SAME(OP1(INC, Eb, F_LOCK), OP1(DEC, Eb, F_LOCK)),
    [G5] = SAME(OP1(INC, Ev, F_LOCK), OP1(DEC, Ev, F_LOCK), OP1(CALL, Ev, F_F64), OP1(CALLF, Mp, 0),
                OP1(JMP, Ev, F_F64), OP1(JMPF, Mp, 0), OP1(PUSH, Ev, F_D64)),
    [G11] = SAME(OP0(MOV, 0)),
    [G6] = {{OP1(SLDT, Ew, 0), OP1(STR, Ew, 0), OP1(LLDT, Ew, 0), OP1(LTR, Ew, 0),
             OP1(VERR, Ew, 0), OP1(VERW, Ew, 0)},
            {OP1(SLDT, Ev, 0), OP1(STR, Ev, 0), OP1(LLDT, Ew, 0), OP1(LTR, Ew, 0),
             OP1(VERR, Ew, 0), OP1(VERW, Ew, 0)}},
    [G7] = {{OP1(SGDT, M, 0), OP1(SIDT, M, 0), OP1(LGDT, M, 0), OP1(LIDT, M, 0),
             OP1(SMSW, Ew, 0), OP1(RSTORSSP, M, 0), OP1(LMSW, Ew, 0), OP1(INVLPG, M, 0)},
            {OP0(GRP7R, 0), OP0(GRP7R, 0), OP0(GRP7R, 0), OP0(GRP7R, 0),
             OP1(SMSW, Ev, 0), OP0(GRP7R, 0), OP1(LMSW, Ew, 0), OP0(GRP7R, 0)}},
    [G8] = SAME({0}, {0}, {0}, {0}, OP2(BT, Ev, Ib, 0), OP2(BTS, Ev, Ib, F_LOCK),
                OP2(BTR, Ev, Ib, F_LOCK), OP2(BTC, Ev, Ib, F_LOCK)),
    [G9] = {{{0}, OP1(CMPXCHG8B, Mq, F_LOCK)},
            {[6] = OP1(RDRAND, Ev, 0), [7] = OP1(RDSEED, Ev, 0)}}, /* F3 /7 reg: RDPID (in exec) */
    [G12] = {{{0}}, {[2] = OP0(PSRLW, 0), [4] = OP0(PSRAW, 0), [6] = OP0(PSLLW, 0)}},
    [G13] = {{{0}}, {[2] = OP0(PSRLD, 0), [4] = OP0(PSRAD, 0), [6] = OP0(PSLLD, 0)}},
    [G14] = {{{0}}, {[2] = OP0(PSRLQ, 0), [3] = OP0(PSRLDQ, 0), [6] = OP0(PSLLQ, 0), [7] = OP0(PSLLDQ, 0)}},
    [G15] = {{OP1(FXSAVE, M, 0), OP1(FXRSTOR, M, 0), OP1(LDMXCSR, Md, 0), OP1(STMXCSR, Md, 0),
              OP1(XSAVE, M, 0), OP1(XRSTOR, M, 0), OP1(XSAVEOPT, M, 0), OP1(CLFLUSH, M, 0)},
             {OP1(RDFSBASE, Ey, 0), OP1(RDGSBASE, Ey, 0), OP1(WRFSBASE, Ey, 0), OP1(WRGSBASE, Ey, 0),
              {0}, OP0(LFENCE, 0), OP0(MFENCE, 0), OP0(SFENCE, 0)}},
};

static int is_modrm_opnd(int o) {
    switch (o) {
    case O_Eb: case O_Ew: case O_Ed: case O_Eq: case O_Ev: case O_Ey:
    case O_Gb: case O_Gw: case O_Gd: case O_Gq: case O_Gv: case O_Gy:
    case O_M: case O_Mp: case O_Mq: case O_Md: case O_Sw:
    case O_Vx: case O_Wx: case O_Wq: case O_Wd: case O_Ux: case O_Ww:
    case O_Pq: case O_Qq: case O_Nq: case O_Cd: case O_Dd: case O_Ry:
        return 1;
    }
    return 0;
}

/* Operand sizes for opnd_size(): bytes, or a negative code for sizes that
 * depend on the instruction (see decode.h). */
const int8_t opnd_kind[O_COUNT] = {
    [O_Eb] = 1, [O_Gb] = 1, [O_Zb] = 1, [O_Ib] = 1, [O_Ib2] = 1, [O_AL] = 1, [O_CL] = 1,
    [O_Ob] = 1, [O_Xb] = 1, [O_Yb] = 1, [O_1] = 1, [O_Jb] = 1,
    [O_Ew] = 2, [O_Gw] = 2, [O_Iw] = 2, [O_Sw] = 2, [O_DX] = 2,
    [O_ES] = 2, [O_CS] = 2, [O_SS] = 2, [O_DS] = 2, [O_FS] = 2, [O_GS] = 2, [O_Ww] = 2,
    [O_Ed] = 4, [O_Gd] = 4, [O_Md] = 4, [O_Wd] = 4,
    [O_Eq] = 8, [O_Gq] = 8, [O_Wq] = 8, [O_Pq] = 8, [O_Qq] = 8, [O_Nq] = 8,
    [O_Vx] = 16, [O_Wx] = 16, [O_Ux] = 16,
    [O_Mq] = OK_MQ,
    [O_Ev] = OK_OS, [O_Gv] = OK_OS, [O_Zv] = OK_OS, [O_rAX] = OK_OS, [O_Ov] = OK_OS, [O_Xv] = OK_OS,
    [O_Yv] = OK_OS, [O_Iv] = OK_OS, [O_Ibs] = OK_OS, [O_Iz] = OK_OS, [O_Jz] = OK_OS,
    [O_Ey] = OK_Y, [O_Gy] = OK_Y,
    [O_Ry] = OK_MODE, [O_Cd] = OK_MODE, [O_Dd] = OK_MODE,
    [O_eAX] = OK_EAX,
    [O_Mp] = OK_MP,
};

int opnd_is_mem(const Insn *in, int spec) {
    switch (spec) {
    case O_Eb: case O_Ew: case O_Ed: case O_Eq: case O_Ev: case O_Ey:
    case O_Wx: case O_Wq: case O_Wd: case O_Ww: case O_Qq:
        return in->mod != 3;
    case O_M: case O_Mp: case O_Mq: case O_Md: case O_Ob: case O_Ov:
        return 1;
    }
    return 0;
}

static uint64_t rd_le(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static int64_t sx(uint64_t v, int n) {
    switch (n) {
    case 1: return (int8_t)v;
    case 2: return (int16_t)v;
    case 4: return (int32_t)v;
    }
    return (int64_t)v;
}

int decode(const uint8_t *buf, int avail, uint64_t addr, int mode, Insn *in) {
    memset(in, 0, sizeof *in);
    in->addr = addr;
    in->mode = (uint8_t)mode;
    in->seg = -1;
    in->base = in->index = -1;
    int p = 0;
    uint8_t b;

#define NEED(n) \
    do { \
        if (p + (n) > 15) return DEC_INVALID; \
        if (p + (n) > avail) return DEC_TRUNC; \
    } while (0)

    /* Legacy prefixes and REX */
    for (;;) {
        NEED(1);
        b = buf[p];
        if (mode == 64 && (b & 0xF0) == 0x40) {
            in->rex = b;
            p++;
            continue;
        }
        switch (b) {
        case 0xF0: in->lock = 1; break;
        case 0xF2: case 0xF3: in->rep = b; break;
        case 0x26: in->seg = ES; break;
        case 0x2E: in->seg = CS; break;
        case 0x36: in->seg = SS; break;
        case 0x3E: in->seg = DS; break;
        case 0x64: in->seg = FS; break;
        case 0x65: in->seg = GS; break;
        case 0x66: in->pfx66 = 1; break;
        case 0x67: in->pfx67 = 1; break;
        default: goto prefixes_done;
        }
        in->rex = 0; /* a legacy prefix after REX cancels it */
        p++;
    }
prefixes_done:
    if (mode == 64) in->addrsize = in->pfx67 ? 4 : 8;
    else if (mode == 32) in->addrsize = in->pfx67 ? 2 : 4;
    else in->addrsize = in->pfx67 ? 4 : 2;

    OpDef def;
    int imm8_follows = 0;

    if ((b == 0xC4 || b == 0xC5 || b == 0x62) && mode != 64) {
        NEED(2);
        if ((buf[p + 1] & 0xC0) != 0xC0) goto legacy; /* LES/LDS/BOUND */
    }
    if (b == 0xC4 || b == 0xC5) {
        /* VEX: decoded for length; execution arrives with AVX support. */
        int r, x = 0, bb = 0, w = 0, mm = 1;
        if (b == 0xC5) {
            NEED(3);
            uint8_t v1 = buf[p + 1];
            r = !(v1 & 0x80);
            in->vex_vvvv = (~v1 >> 3) & 15;
            in->vex_l = (v1 >> 2) & 1;
            in->vex = 2;
            p += 2;
        } else {
            NEED(4);
            uint8_t v1 = buf[p + 1], v2 = buf[p + 2];
            r = !(v1 & 0x80);
            x = !(v1 & 0x40);
            bb = !(v1 & 0x20);
            mm = v1 & 0x1F;
            w = v2 >> 7;
            in->vex_vvvv = (~v2 >> 3) & 15;
            in->vex_l = (v2 >> 2) & 1;
            in->vex = 3;
            p += 3;
        }
        in->vex_w = (uint8_t)w;
        if (mode == 64) in->rex = (uint8_t)(0x40 | (w << 3) | (r << 2) | (x << 1) | bb);
        in->map = mm;
        in->opcode = buf[p++];
        def = (OpDef){I_VEX, {0}, F_MODRM, 0};
        if (in->map == 1 && in->opcode == 0x77) def.f = 0;
        imm8_follows = in->map == 3;
        goto have_def;
    }
    if (b == 0x62 && mode == 64) {
        NEED(5);
        uint8_t p0 = buf[p + 1], p1 = buf[p + 2];
        in->vex = 4;
        in->map = p0 & 7;
        in->vex_w = p1 >> 7;
        in->rex = 0x40 | (in->vex_w << 3) | (!(p0 & 0x80) << 2) | (!(p0 & 0x40) << 1) | !(p0 & 0x20);
        p += 4;
        in->opcode = buf[p++];
        def = (OpDef){I_EVEX, {0}, F_MODRM, 0};
        imm8_follows = in->map == 3;
        goto have_def;
    }

legacy:
    p++;
    in->opcode = b;
    if (b == 0x0F) {
        NEED(1);
        b = buf[p++];
        if (b == 0x38 || b == 0x3A) {
            NEED(1);
            in->map = b == 0x38 ? 2 : 3;
            in->opcode = buf[p++];
            def = (OpDef)PF;
        } else {
            in->map = 1;
            in->opcode = b;
            def = map0f[b];
        }
    } else {
        in->map = 0;
        def = map1[b];
        if (b == 0x63 && mode != 64) def = (OpDef)OP2(ARPL, Ew, Gw, 0);
        if (b == 0x90) {
            if (in->rex & 1) def = (OpDef)OP2(XCHG, Zv, rAX, 0);
            else if (in->rep == 0xF3) def = (OpDef)OP0(PAUSE, 0);
        }
    }
    if (def.f & F_PFX) {
        int slot = in->rep == 0xF3 ? 2 : in->rep == 0xF2 ? 3 : in->pfx66 ? 1 : 0;
        const OpDef (*tab)[4] = in->map == 1 ? pfx0f : in->map == 2 ? pfx0f38 : pfx0f3a;
        def = tab[in->opcode][slot];
        if (!def.id && !(def.f & F_GRP)) {
            /* undefined: still determine the length (ModRM, plus imm8 in map 3) */
            def.f = F_MODRM;
            imm8_follows = in->map == 3;
        }
    }

have_def:;
    int need_modrm = (def.f & (F_GRP | F_MODRM)) != 0;
    for (int i = 0; i < 3; i++) need_modrm |= is_modrm_opnd(def.o[i]);

    if (need_modrm) {
        NEED(1);
        uint8_t m = buf[p++];
        in->has_modrm = 1;
        in->modrm = m;
        in->mod = m >> 6;
        if (def.f & F_REG) in->mod = 3;
        in->reg = ((m >> 3) & 7) | ((in->rex & 4) ? 8 : 0);
        uint8_t rm = m & 7;
        if (in->mod == 3) {
            in->rm = rm | ((in->rex & 1) ? 8 : 0);
        } else if (in->addrsize == 2) {
            static const int8_t b16[8] = {RBX, RBX, RBP, RBP, RSI, RDI, RBP, RBX};
            static const int8_t i16[8] = {RSI, RDI, RSI, RDI, -1, -1, -1, -1};
            in->rm = rm;
            if (in->mod == 0 && rm == 6) {
                NEED(2);
                in->disp = (int64_t)rd_le(buf + p, 2);
                p += 2;
            } else {
                in->base = b16[rm];
                in->index = i16[rm];
                if (in->mod == 1) { NEED(1); in->disp = sx(buf[p], 1); p += 1; }
                if (in->mod == 2) { NEED(2); in->disp = sx(rd_le(buf + p, 2), 2); p += 2; }
            }
        } else {
            int disp_size = in->mod == 1 ? 1 : in->mod == 2 ? 4 : 0;
            in->rm = rm | ((in->rex & 1) ? 8 : 0);
            if (rm == 4) {
                NEED(1);
                uint8_t sib = buf[p++];
                int idx = ((sib >> 3) & 7) | ((in->rex & 2) ? 8 : 0);
                in->scale = sib >> 6;
                if (idx != 4) in->index = (int8_t)idx;
                if ((sib & 7) == 5 && in->mod == 0) disp_size = 4;
                else in->base = (int8_t)((sib & 7) | ((in->rex & 1) ? 8 : 0));
            } else if (rm == 5 && in->mod == 0) {
                if (mode == 64) in->base = REG_RIP;
                disp_size = 4;
            } else {
                in->base = (int8_t)in->rm;
            }
            if (disp_size) {
                NEED(disp_size);
                in->disp = sx(rd_le(buf + p, disp_size), disp_size);
                p += disp_size;
            }
        }
        if ((def.f & F_GRP) && def.grp == G11 && ((m >> 3) & 7)) { /* C6/C7: only /0 (MOV) and F8 */
            if (m != 0xF8) {
                in->len = (uint8_t)p;
                return DEC_INVALID;
            }
            OpDef t = in->opcode == 0xC6 ? (OpDef)OP1(XABORT, Ib, 0) : (OpDef)OP1(XBEGIN, Jz, 0);
            def = t;
        }
        if (def.f & F_GRP) {
            OpDef g = groups[def.grp][in->mod == 3][(m >> 3) & 7];
            if (!g.id) {
                in->len = (uint8_t)p;
                return DEC_INVALID;
            }
            if (g.o[0] == O_NONE) memcpy(g.o, def.o, sizeof g.o);
            g.f |= def.f & ~F_GRP;
            def = g;
        }
    }

    /* Operand and address size */
    int f = def.f;
    if (mode == 64) {
        int w = (in->rex & 8) != 0;
        in->opsize = w ? 8 : in->pfx66 ? 2 : 4;
        if (f & F_F64) in->opsize = 8;
        else if ((f & F_D64) && !w) in->opsize = in->pfx66 ? 2 : 8;
    } else if (mode == 32) {
        in->opsize = in->pfx66 ? 2 : 4;
    } else {
        in->opsize = in->pfx66 ? 4 : 2;
    }

    in->id = def.id;
    in->flags = (uint16_t)f;
    memcpy(in->opnd, def.o, sizeof in->opnd);
    in->zreg = (in->opcode & 7) | ((in->rex & 1) ? 8 : 0);
    in->cc = in->opcode & 15;

    /* Immediates, in encoding order */
    int is_branch = 0;
    int64_t rel = 0;
    for (int i = 0; i < 3; i++) {
        int o = def.o[i], n = 0, sign = 0;
        switch (o) {
        case O_Ib: n = 1; break;
        case O_Ibs: n = 1; sign = 1; break;
        case O_Iw: n = 2; break;
        case O_Iz: n = in->opsize == 2 ? 2 : 4; sign = 1; break;
        case O_Iv: n = in->opsize; break;
        case O_Ob: case O_Ov: n = in->addrsize; break;
        case O_Ib2:
            NEED(1);
            in->imm2 = buf[p++];
            continue;
        case O_Jb: case O_Jz:
            n = o == O_Jb ? 1 : (in->opsize == 2 ? 2 : 4);
            NEED(n);
            rel = sx(rd_le(buf + p, n), n);
            p += n;
            is_branch = 1;
            continue;
        case O_Ap:
            n = in->opsize == 2 ? 2 : 4;
            NEED(n + 2);
            in->imm = rd_le(buf + p, n);
            in->imm2 = rd_le(buf + p + n, 2);
            p += n + 2;
            continue;
        case O_M: case O_Mp: case O_Mq: case O_Md:
            if (in->mod == 3) { in->len = (uint8_t)p; return DEC_INVALID; }
            continue;
        case O_Ux: case O_Nq:
            if (in->mod != 3) { in->len = (uint8_t)p; return DEC_INVALID; }
            continue;
        default:
            continue;
        }
        NEED(n);
        uint64_t v = rd_le(buf + p, n);
        in->imm = sign ? (uint64_t)sx(v, n) : v;
        p += n;
    }
    if (imm8_follows) {
        NEED(1);
        in->imm = buf[p++];
    }

    in->len = (uint8_t)p;
    memcpy(in->bytes, buf, p);
    if (is_branch) {
        uint64_t t = addr + p + (uint64_t)rel;
        if (mode != 64) t &= in->opsize == 2 ? 0xFFFF : 0xFFFFFFFF;
        in->target = t;
    }

    /* CET instructions that share encodings with hints and fences (F3 prefix) */
    if (in->map == 1 && in->rep == 0xF3) {
        if (in->opcode == 0x1E && in->modrm == 0xFA) in->id = I_ENDBR64, in->opnd[0] = 0;
        else if (in->opcode == 0x1E && in->modrm == 0xFB) in->id = I_ENDBR32, in->opnd[0] = 0;
        else if (in->opcode == 0x1E && in->mod == 3 && (in->reg & 7) == 1) in->id = I_RDSSP, in->opnd[0] = O_Ey;
        else if (in->id == I_LFENCE) in->id = I_INCSSP, in->opnd[0] = O_Ey;
        else if (in->id == I_XSAVEOPT) in->id = I_CLRSSBSY;
        else if (in->id == I_GRP7R && in->modrm == 0xE8) in->id = I_SETSSBSY;
        else if (in->id == I_GRP7R && in->modrm == 0xEA) in->id = I_SAVEPREVSSP;
    }
    if (in->id == I_RSTORSSP && in->rep != 0xF3) return DEC_INVALID;
    if ((in->id == I_WRSS || in->id == I_WRUSS) && in->mod == 3) return DEC_INVALID;

    if (!def.id) return DEC_INVALID;
    if ((f & F_I64) && mode == 64) return DEC_INVALID;
    if (in->lock && (!(f & F_LOCK) || in->mod == 3)) return DEC_INVALID;
    return DEC_OK;
#undef NEED
}
