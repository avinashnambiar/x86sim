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
#define PF(p) {0, {0, 0, 0}, F_PFX, p}

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

enum {
    P_10 = 1, P_11, P_28, P_29, P_54, P_55, P_56, P_57, P_6C, P_6E, P_6F, P_70, P_74, P_75, P_76,
    P_7E, P_7F, P_B8, P_BC, P_BD, P_D4, P_D6, P_D7, P_DB, P_DF, P_EB, P_EF,
    P_F8, P_F9, P_FA, P_FB, P_FC, P_FD, P_FE,
    P_12, P_13, P_14, P_15, P_16, P_17, P_2B, P_50, P_60, P_61, P_62, P_63, P_64, P_65, P_66, P_67,
    P_68, P_69, P_6A, P_6B, P_6D, P_71, P_72, P_73, P_C4, P_C5, P_C6, P_D1, P_D2, P_D3, P_D5,
    P_D8, P_D9, P_DA, P_DC, P_DD, P_DE, P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E7, P_E8, P_E9,
    P_EA, P_EC, P_ED, P_EE, P_F1, P_F2, P_F3, P_F4, P_F5, P_F6, P_F7, NPFX
};

static const OpDef map0f[256] = {
    [0x00] = GR(G6, NONE, NONE, 0), [0x01] = GR(G7, NONE, NONE, 0),
    [0x05] = OP0(SYSCALL, 0), [0x0B] = OP0(UD2, 0), [0x0D] = OP1(PREFETCH, Ev, 0),
    [0x10] = PF(P_10), [0x11] = PF(P_11),
    [0x18] = OP1(PREFETCH, Ev, 0), [0x19 ... 0x1F] = OP1(NOP, Ev, 0),
    [0x12] = PF(P_12), [0x13] = PF(P_13), [0x14] = PF(P_14), [0x15] = PF(P_15),
    [0x16] = PF(P_16), [0x17] = PF(P_17),
    [0x28] = PF(P_28), [0x29] = PF(P_29), [0x2B] = PF(P_2B),
    [0x50] = PF(P_50),
    [0x60] = PF(P_60), [0x61] = PF(P_61), [0x62] = PF(P_62), [0x63] = PF(P_63),
    [0x64] = PF(P_64), [0x65] = PF(P_65), [0x66] = PF(P_66), [0x67] = PF(P_67),
    [0x68] = PF(P_68), [0x69] = PF(P_69), [0x6A] = PF(P_6A), [0x6B] = PF(P_6B), [0x6D] = PF(P_6D),
    [0x71] = PF(P_71), [0x72] = PF(P_72), [0x73] = PF(P_73),
    [0xC3] = OP2(MOVNTI, Ey, Gy, 0), [0xC4] = PF(P_C4), [0xC5] = PF(P_C5), [0xC6] = PF(P_C6),
    [0xD1] = PF(P_D1), [0xD2] = PF(P_D2), [0xD3] = PF(P_D3), [0xD5] = PF(P_D5),
    [0xD8] = PF(P_D8), [0xD9] = PF(P_D9), [0xDA] = PF(P_DA), [0xDC] = PF(P_DC), [0xDD] = PF(P_DD), [0xDE] = PF(P_DE),
    [0xE0] = PF(P_E0), [0xE1] = PF(P_E1), [0xE2] = PF(P_E2), [0xE3] = PF(P_E3), [0xE4] = PF(P_E4), [0xE5] = PF(P_E5),
    [0xE7] = PF(P_E7), [0xE8] = PF(P_E8), [0xE9] = PF(P_E9), [0xEA] = PF(P_EA), [0xEC] = PF(P_EC), [0xED] = PF(P_ED),
    [0xEE] = PF(P_EE),
    [0xF1] = PF(P_F1), [0xF2] = PF(P_F2), [0xF3] = PF(P_F3), [0xF4] = PF(P_F4), [0xF5] = PF(P_F5), [0xF6] = PF(P_F6),
    [0xF7] = PF(P_F7),
    [0x31] = OP0(RDTSC, 0), [0x34] = OP0(SYSENTER, 0),
    [0x40 ... 0x4F] = OP2(CMOVCC, Gv, Ev, 0),
    [0x54] = PF(P_54), [0x55] = PF(P_55), [0x56] = PF(P_56), [0x57] = PF(P_57),
    [0x6C] = PF(P_6C), [0x6E] = PF(P_6E), [0x6F] = PF(P_6F),
    [0x70] = PF(P_70), [0x74] = PF(P_74), [0x75] = PF(P_75), [0x76] = PF(P_76),
    [0x7E] = PF(P_7E), [0x7F] = PF(P_7F),
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
    [0xB8] = PF(P_B8), [0xBA] = GR(G8, NONE, NONE, 0), [0xBB] = OP2(BTC, Ev, Gv, F_LOCK),
    [0xBC] = PF(P_BC), [0xBD] = PF(P_BD),
    [0xBE] = OP2(MOVSX, Gv, Eb, 0), [0xBF] = OP2(MOVSX, Gv, Ew, 0),
    [0xC0] = OP2(XADD, Eb, Gb, F_LOCK), [0xC1] = OP2(XADD, Ev, Gv, F_LOCK),
    [0xC7] = GR(G9, NONE, NONE, 0),
    [0xC8 ... 0xCF] = OP1(BSWAP, Zv, 0),
    [0xD4] = PF(P_D4), [0xD6] = PF(P_D6), [0xD7] = PF(P_D7), [0xDB] = PF(P_DB), [0xDF] = PF(P_DF),
    [0xEB] = PF(P_EB), [0xEF] = PF(P_EF),
    [0xF8] = PF(P_F8), [0xF9] = PF(P_F9), [0xFA] = PF(P_FA), [0xFB] = PF(P_FB),
    [0xFC] = PF(P_FC), [0xFD] = PF(P_FD), [0xFE] = PF(P_FE),
};

/* Three-byte maps: populated as SSSE3/SSE4/AES/SHA support is added. */
static const OpDef map0f38[256];
static const OpDef map0f3a[256];

/* Mandatory-prefix variants: [none, 66, F3, F2] */
#define SSE2(id, a, b) {{0}, OP2(id, a, b, 0), {0}, {0}}
static const OpDef pfxtab[NPFX][4] = {
    [P_10] = {OP2(MOVUPS, Vx, Wx, 0), OP2(MOVUPD, Vx, Wx, 0), OP2(MOVSS, Vx, Wd, 0), OP2(MOVSD_X, Vx, Wq, 0)},
    [P_11] = {OP2(MOVUPS, Wx, Vx, 0), OP2(MOVUPD, Wx, Vx, 0), OP2(MOVSS, Wd, Vx, 0), OP2(MOVSD_X, Wq, Vx, 0)},
    [P_28] = {OP2(MOVAPS, Vx, Wx, 0), OP2(MOVAPD, Vx, Wx, 0)},
    [P_29] = {OP2(MOVAPS, Wx, Vx, 0), OP2(MOVAPD, Wx, Vx, 0)},
    [P_54] = {OP2(ANDPS, Vx, Wx, 0), OP2(ANDPD, Vx, Wx, 0)},
    [P_55] = {OP2(ANDNPS, Vx, Wx, 0), OP2(ANDNPD, Vx, Wx, 0)},
    [P_56] = {OP2(ORPS, Vx, Wx, 0), OP2(ORPD, Vx, Wx, 0)},
    [P_57] = {OP2(XORPS, Vx, Wx, 0), OP2(XORPD, Vx, Wx, 0)},
    [P_6C] = SSE2(PUNPCKLQDQ, Vx, Wx),
    [P_6E] = SSE2(MOVD, Vx, Ey),
    [P_6F] = {{0}, OP2(MOVDQA, Vx, Wx, 0), OP2(MOVDQU, Vx, Wx, 0), {0}},
    [P_70] = {{0}, OP3(PSHUFD, Vx, Wx, Ib, 0), OP3(PSHUFHW, Vx, Wx, Ib, 0), OP3(PSHUFLW, Vx, Wx, Ib, 0)},
    [P_12] = {OP2(MOVLPS, Vx, Wq, 0), OP2(MOVLPD, Vx, Wq, 0)},
    [P_13] = {OP2(MOVLPS, Wq, Vx, 0), OP2(MOVLPD, Wq, Vx, 0)},
    [P_14] = {OP2(UNPCKLPS, Vx, Wx, 0), OP2(UNPCKLPD, Vx, Wx, 0)},
    [P_15] = {OP2(UNPCKHPS, Vx, Wx, 0), OP2(UNPCKHPD, Vx, Wx, 0)},
    [P_16] = {OP2(MOVHPS, Vx, Wq, 0), OP2(MOVHPD, Vx, Wq, 0)},
    [P_17] = {OP2(MOVHPS, Wq, Vx, 0), OP2(MOVHPD, Wq, Vx, 0)},
    [P_2B] = {OP2(MOVNTPS, Wx, Vx, 0), OP2(MOVNTPD, Wx, Vx, 0)},
    [P_50] = {OP2(MOVMSKPS, Gd, Ux, 0), OP2(MOVMSKPD, Gd, Ux, 0)},
    [P_60] = SSE2(PUNPCKLBW, Vx, Wx), [P_61] = SSE2(PUNPCKLWD, Vx, Wx), [P_62] = SSE2(PUNPCKLDQ, Vx, Wx),
    [P_63] = SSE2(PACKSSWB, Vx, Wx), [P_64] = SSE2(PCMPGTB, Vx, Wx), [P_65] = SSE2(PCMPGTW, Vx, Wx),
    [P_66] = SSE2(PCMPGTD, Vx, Wx), [P_67] = SSE2(PACKUSWB, Vx, Wx), [P_68] = SSE2(PUNPCKHBW, Vx, Wx),
    [P_69] = SSE2(PUNPCKHWD, Vx, Wx), [P_6A] = SSE2(PUNPCKHDQ, Vx, Wx), [P_6B] = SSE2(PACKSSDW, Vx, Wx),
    [P_6D] = SSE2(PUNPCKHQDQ, Vx, Wx),
    [P_71] = {{0}, GR(G12, Ux, Ib, 0)}, [P_72] = {{0}, GR(G13, Ux, Ib, 0)}, [P_73] = {{0}, GR(G14, Ux, Ib, 0)},
    [P_C4] = {{0}, OP3(PINSRW, Vx, Ew, Ib, 0)}, [P_C5] = {{0}, OP3(PEXTRW, Gd, Ux, Ib, 0)},
    [P_C6] = {OP3(SHUFPS, Vx, Wx, Ib, 0), OP3(SHUFPD, Vx, Wx, Ib, 0)},
    [P_D1] = SSE2(PSRLW, Vx, Wx), [P_D2] = SSE2(PSRLD, Vx, Wx), [P_D3] = SSE2(PSRLQ, Vx, Wx),
    [P_D5] = SSE2(PMULLW, Vx, Wx), [P_D8] = SSE2(PSUBUSB, Vx, Wx), [P_D9] = SSE2(PSUBUSW, Vx, Wx),
    [P_DA] = SSE2(PMINUB, Vx, Wx), [P_DC] = SSE2(PADDUSB, Vx, Wx), [P_DD] = SSE2(PADDUSW, Vx, Wx),
    [P_DE] = SSE2(PMAXUB, Vx, Wx), [P_E0] = SSE2(PAVGB, Vx, Wx), [P_E1] = SSE2(PSRAW, Vx, Wx),
    [P_E2] = SSE2(PSRAD, Vx, Wx), [P_E3] = SSE2(PAVGW, Vx, Wx), [P_E4] = SSE2(PMULHUW, Vx, Wx),
    [P_E5] = SSE2(PMULHW, Vx, Wx), [P_E7] = SSE2(MOVNTDQ, Wx, Vx), [P_E8] = SSE2(PSUBSB, Vx, Wx),
    [P_E9] = SSE2(PSUBSW, Vx, Wx), [P_EA] = SSE2(PMINSW, Vx, Wx), [P_EC] = SSE2(PADDSB, Vx, Wx),
    [P_ED] = SSE2(PADDSW, Vx, Wx), [P_EE] = SSE2(PMAXSW, Vx, Wx), [P_F1] = SSE2(PSLLW, Vx, Wx),
    [P_F2] = SSE2(PSLLD, Vx, Wx), [P_F3] = SSE2(PSLLQ, Vx, Wx), [P_F4] = SSE2(PMULUDQ, Vx, Wx),
    [P_F5] = SSE2(PMADDWD, Vx, Wx), [P_F6] = SSE2(PSADBW, Vx, Wx), [P_F7] = SSE2(MASKMOVDQU, Vx, Ux),
    [P_74] = SSE2(PCMPEQB, Vx, Wx), [P_75] = SSE2(PCMPEQW, Vx, Wx), [P_76] = SSE2(PCMPEQD, Vx, Wx),
    [P_7E] = {{0}, OP2(MOVD, Ey, Vx, 0), OP2(MOVQ, Vx, Wq, 0), {0}},
    [P_7F] = {{0}, OP2(MOVDQA, Wx, Vx, 0), OP2(MOVDQU, Wx, Vx, 0), {0}},
    [P_B8] = {{0}, {0}, OP2(POPCNT, Gv, Ev, 0), {0}},
    [P_BC] = {OP2(BSF, Gv, Ev, 0), OP2(BSF, Gv, Ev, 0), OP2(TZCNT, Gv, Ev, 0), OP2(BSF, Gv, Ev, 0)},
    [P_BD] = {OP2(BSR, Gv, Ev, 0), OP2(BSR, Gv, Ev, 0), OP2(LZCNT, Gv, Ev, 0), OP2(BSR, Gv, Ev, 0)},
    [P_D4] = SSE2(PADDQ, Vx, Wx),
    [P_D6] = SSE2(MOVQ, Wq, Vx),
    [P_D7] = SSE2(PMOVMSKB, Gd, Ux),
    [P_DB] = SSE2(PAND, Vx, Wx), [P_DF] = SSE2(PANDN, Vx, Wx),
    [P_EB] = SSE2(POR, Vx, Wx), [P_EF] = SSE2(PXOR, Vx, Wx),
    [P_F8] = SSE2(PSUBB, Vx, Wx), [P_F9] = SSE2(PSUBW, Vx, Wx),
    [P_FA] = SSE2(PSUBD, Vx, Wx), [P_FB] = SSE2(PSUBQ, Vx, Wx),
    [P_FC] = SSE2(PADDB, Vx, Wx), [P_FD] = SSE2(PADDW, Vx, Wx), [P_FE] = SSE2(PADDD, Vx, Wx),
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
    [G6] = SAME(OP1(GRP6, Ew, 0), OP1(GRP6, Ew, 0), OP1(GRP6, Ew, 0), OP1(GRP6, Ew, 0),
                OP1(GRP6, Ew, 0), OP1(GRP6, Ew, 0)),
    [G7] = {{OP1(SGDT, M, 0), OP1(SIDT, M, 0), OP1(LGDT, M, 0), OP1(LIDT, M, 0),
             OP1(SMSW, Ew, 0), {0}, OP1(LMSW, Ew, 0), OP1(INVLPG, M, 0)},
            {OP0(GRP7R, 0), OP0(GRP7R, 0), OP0(GRP7R, 0), OP0(GRP7R, 0),
             OP1(SMSW, Ev, 0), OP0(GRP7R, 0), OP1(LMSW, Ew, 0), OP0(GRP7R, 0)}},
    [G8] = SAME({0}, {0}, {0}, {0}, OP2(BT, Ev, Ib, 0), OP2(BTS, Ev, Ib, F_LOCK),
                OP2(BTR, Ev, Ib, F_LOCK), OP2(BTC, Ev, Ib, F_LOCK)),
    [G9] = {{{0}, OP1(CMPXCHG8B, Mq, F_LOCK)},
            {[6] = OP1(RDRAND, Ev, 0), [7] = OP1(RDSEED, Ev, 0)}},
    [G12] = {{{0}}, {[2] = OP0(PSRLW, 0), [4] = OP0(PSRAW, 0), [6] = OP0(PSLLW, 0)}},
    [G13] = {{{0}}, {[2] = OP0(PSRLD, 0), [4] = OP0(PSRAD, 0), [6] = OP0(PSLLD, 0)}},
    [G14] = {{{0}}, {[2] = OP0(PSRLQ, 0), [3] = OP0(PSRLDQ, 0), [6] = OP0(PSLLQ, 0), [7] = OP0(PSLLDQ, 0)}},
    [G15] = {{OP1(FXSAVE, M, 0), OP1(FXRSTOR, M, 0), OP1(LDMXCSR, Md, 0), OP1(STMXCSR, Md, 0),
              OP1(XSAVE, M, 0), OP1(XRSTOR, M, 0), OP1(XSAVEOPT, M, 0), OP1(CLFLUSH, M, 0)},
             {[5] = OP0(LFENCE, 0), [6] = OP0(MFENCE, 0), [7] = OP0(SFENCE, 0)}},
};

static int is_modrm_opnd(int o) {
    switch (o) {
    case O_Eb: case O_Ew: case O_Ed: case O_Eq: case O_Ev: case O_Ey:
    case O_Gb: case O_Gw: case O_Gd: case O_Gq: case O_Gv: case O_Gy:
    case O_M: case O_Mp: case O_Mq: case O_Md: case O_Sw:
    case O_Vx: case O_Wx: case O_Wq: case O_Wd: case O_Ux:
        return 1;
    }
    return 0;
}

int opnd_size(const Insn *in, int spec) {
    int os = in->opsize;
    switch (spec) {
    case O_Eb: case O_Gb: case O_Zb: case O_Ib: case O_Ib2: case O_AL: case O_CL:
    case O_Ob: case O_Xb: case O_Yb: case O_1: case O_Jb:
        return 1;
    case O_Ew: case O_Gw: case O_Iw: case O_Sw: case O_DX:
    case O_ES: case O_CS: case O_SS: case O_DS: case O_FS: case O_GS:
        return 2;
    case O_Ed: case O_Gd: case O_Md: case O_Wd:
        return 4;
    case O_Eq: case O_Gq: case O_Wq:
        return 8;
    case O_Mq:
        return (in->rex & 8) ? 16 : 8;
    case O_Ev: case O_Gv: case O_Zv: case O_rAX: case O_Ov: case O_Xv: case O_Yv:
    case O_Iv: case O_Ibs: case O_Iz: case O_Jz:
        return os;
    case O_Ey: case O_Gy:
        return os == 8 ? 8 : 4;
    case O_eAX:
        return os == 2 ? 2 : 4;
    case O_Mp:
        return os + 2;
    case O_Vx: case O_Wx: case O_Ux:
        return 16;
    }
    return 0;
}

int opnd_is_mem(const Insn *in, int spec) {
    switch (spec) {
    case O_Eb: case O_Ew: case O_Ed: case O_Eq: case O_Ev: case O_Ey:
    case O_Wx: case O_Wq: case O_Wd:
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
            def = (in->map == 2 ? map0f38 : map0f3a)[in->opcode];
            if (!def.id) {
                def.f = F_MODRM;
                imm8_follows = in->map == 3;
            }
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
        def = pfxtab[def.grp][slot];
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
        case O_Ux:
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

    if (!def.id) return DEC_INVALID;
    if ((f & F_I64) && mode == 64) return DEC_INVALID;
    if (in->lock && (!(f & F_LOCK) || in->mod == 3)) return DEC_INVALID;
    return DEC_OK;
#undef NEED
}
