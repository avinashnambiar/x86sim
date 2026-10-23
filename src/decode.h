/* Table-driven x86 instruction decoder (16/32/64-bit modes). */
#ifndef DECODE_H
#define DECODE_H

#include <stddef.h>
#include <stdint.h>

#define MNEMONICS \
    X(INVALID, "(bad)") \
    X(ADD, "add") X(OR, "or") X(ADC, "adc") X(SBB, "sbb") X(AND, "and") X(SUB, "sub") X(XOR, "xor") X(CMP, "cmp") \
    X(INC, "inc") X(DEC, "dec") X(NEG, "neg") X(NOT, "not") X(TEST, "test") \
    X(MUL, "mul") X(IMUL, "imul") X(DIV, "div") X(IDIV, "idiv") \
    X(ROL, "rol") X(ROR, "ror") X(RCL, "rcl") X(RCR, "rcr") X(SHL, "shl") X(SHR, "shr") X(SAL, "sal") X(SAR, "sar") \
    X(SHLD, "shld") X(SHRD, "shrd") \
    X(MOV, "mov") X(MOVZX, "movzx") X(MOVSX, "movsx") X(MOVSXD, "movsxd") X(LEA, "lea") X(XCHG, "xchg") \
    X(CMPXCHG, "cmpxchg") X(CMPXCHG8B, "cmpxchg8b") X(XADD, "xadd") X(BSWAP, "bswap") X(CBW, "cbw") X(CWD, "cwd") \
    X(PUSH, "push") X(POP, "pop") X(PUSHA, "pusha") X(POPA, "popa") X(PUSHF, "pushf") X(POPF, "popf") \
    X(LEAVE, "leave") X(ENTER, "enter") \
    X(JMP, "jmp") X(JMPF, "jmp far") X(JCC, "j") X(CALL, "call") X(CALLF, "call far") X(RET, "ret") X(RETF, "retf") \
    X(LOOP, "loop") X(LOOPE, "loope") X(LOOPNE, "loopne") X(JRCXZ, "jrcxz") \
    X(SETCC, "set") X(CMOVCC, "cmov") \
    X(BT, "bt") X(BTS, "bts") X(BTR, "btr") X(BTC, "btc") X(BSF, "bsf") X(BSR, "bsr") \
    X(TZCNT, "tzcnt") X(LZCNT, "lzcnt") X(POPCNT, "popcnt") \
    X(MOVS, "movs") X(STOS, "stos") X(LODS, "lods") X(CMPS, "cmps") X(SCAS, "scas") X(INS, "ins") X(OUTS, "outs") \
    X(CLC, "clc") X(STC, "stc") X(CMC, "cmc") X(CLD, "cld") X(STD, "std") X(CLI, "cli") X(STI, "sti") \
    X(LAHF, "lahf") X(SAHF, "sahf") \
    X(NOP, "nop") X(PAUSE, "pause") X(HLT, "hlt") X(INT3, "int3") X(INT, "int") X(INT1, "int1") X(INTO, "into") \
    X(IRET, "iret") X(SYSCALL, "syscall") X(SYSENTER, "sysenter") X(CPUID, "cpuid") X(RDTSC, "rdtsc") \
    X(UD2, "ud2") X(XLAT, "xlat") X(IN, "in") X(OUT, "out") \
    X(DAA, "daa") X(DAS, "das") X(AAA, "aaa") X(AAS, "aas") X(AAM, "aam") X(AAD, "aad") X(SALC, "salc") \
    X(LES, "les") X(LDS, "lds") X(LSS, "lss") X(LFS, "lfs") X(LGS, "lgs") X(BOUND, "bound") X(ARPL, "arpl") \
    X(FWAIT, "fwait") X(X87, "(x87)") \
    X(GRP6, "(grp6)") X(GRP7R, "(grp7)") X(SGDT, "sgdt") X(SIDT, "sidt") X(LGDT, "lgdt") X(LIDT, "lidt") \
    X(SMSW, "smsw") X(LMSW, "lmsw") X(INVLPG, "invlpg") \
    X(PREFETCH, "prefetch") X(FXSAVE, "fxsave") X(FXRSTOR, "fxrstor") X(LDMXCSR, "ldmxcsr") X(STMXCSR, "stmxcsr") \
    X(XSAVE, "xsave") X(XRSTOR, "xrstor") X(XSAVEOPT, "xsaveopt") X(CLFLUSH, "clflush") \
    X(LFENCE, "lfence") X(MFENCE, "mfence") X(SFENCE, "sfence") X(RDRAND, "rdrand") X(RDSEED, "rdseed") \
    X(MOVUPS, "movups") X(MOVUPD, "movupd") X(MOVSS, "movss") X(MOVSD_X, "movsd") X(MOVAPS, "movaps") \
    X(MOVAPD, "movapd") X(MOVDQA, "movdqa") X(MOVDQU, "movdqu") X(MOVD, "movd") X(MOVQ, "movq") \
    X(ANDPS, "andps") X(ANDPD, "andpd") X(ANDNPS, "andnps") X(ANDNPD, "andnpd") X(ORPS, "orps") X(ORPD, "orpd") \
    X(XORPS, "xorps") X(XORPD, "xorpd") X(PAND, "pand") X(PANDN, "pandn") X(POR, "por") X(PXOR, "pxor") \
    X(PADDB, "paddb") X(PADDW, "paddw") X(PADDD, "paddd") X(PADDQ, "paddq") \
    X(PSUBB, "psubb") X(PSUBW, "psubw") X(PSUBD, "psubd") X(PSUBQ, "psubq") \
    X(PCMPEQB, "pcmpeqb") X(PCMPEQW, "pcmpeqw") X(PCMPEQD, "pcmpeqd") X(PMOVMSKB, "pmovmskb") \
    X(PSHUFD, "pshufd") X(PUNPCKLQDQ, "punpcklqdq") \
    X(PUNPCKLBW, "punpcklbw") X(PUNPCKLWD, "punpcklwd") X(PUNPCKLDQ, "punpckldq") X(PUNPCKHBW, "punpckhbw") \
    X(PUNPCKHWD, "punpckhwd") X(PUNPCKHDQ, "punpckhdq") X(PUNPCKHQDQ, "punpckhqdq") \
    X(PACKSSWB, "packsswb") X(PACKUSWB, "packuswb") X(PACKSSDW, "packssdw") \
    X(PCMPGTB, "pcmpgtb") X(PCMPGTW, "pcmpgtw") X(PCMPGTD, "pcmpgtd") X(PSHUFHW, "pshufhw") X(PSHUFLW, "pshuflw") \
    X(PSRLW, "psrlw") X(PSRAW, "psraw") X(PSLLW, "psllw") X(PSRLD, "psrld") X(PSRAD, "psrad") X(PSLLD, "pslld") \
    X(PSRLQ, "psrlq") X(PSLLQ, "psllq") X(PSRLDQ, "psrldq") X(PSLLDQ, "pslldq") \
    X(PMULLW, "pmullw") X(PMULHW, "pmulhw") X(PMULHUW, "pmulhuw") X(PMULUDQ, "pmuludq") X(PMADDWD, "pmaddwd") \
    X(PSUBUSB, "psubusb") X(PSUBUSW, "psubusw") X(PADDUSB, "paddusb") X(PADDUSW, "paddusw") \
    X(PSUBSB, "psubsb") X(PSUBSW, "psubsw") X(PADDSB, "paddsb") X(PADDSW, "paddsw") \
    X(PMINUB, "pminub") X(PMAXUB, "pmaxub") X(PMINSW, "pminsw") X(PMAXSW, "pmaxsw") \
    X(PAVGB, "pavgb") X(PAVGW, "pavgw") X(PSADBW, "psadbw") X(MASKMOVDQU, "maskmovdqu") \
    X(PINSRW, "pinsrw") X(PEXTRW, "pextrw") X(SHUFPS, "shufps") X(SHUFPD, "shufpd") \
    X(MOVMSKPS, "movmskps") X(MOVMSKPD, "movmskpd") X(UNPCKLPS, "unpcklps") X(UNPCKLPD, "unpcklpd") \
    X(UNPCKHPS, "unpckhps") X(UNPCKHPD, "unpckhpd") X(MOVLPS, "movlps") X(MOVLPD, "movlpd") \
    X(MOVHPS, "movhps") X(MOVHPD, "movhpd") X(MOVNTPS, "movntps") X(MOVNTPD, "movntpd") X(MOVNTDQ, "movntdq") \
    X(MOVNTI, "movnti") \
    X(VEX, "(vex)") X(EVEX, "(evex)")

enum {
#define X(id, name) I_##id,
    MNEMONICS
#undef X
    I_COUNT
};

/* Operand specifiers, after the Intel SDM opcode-map notation. */
enum {
    O_NONE,
    O_Eb, O_Ew, O_Ed, O_Eq, O_Ev, O_Ey,     /* ModRM r/m */
    O_Gb, O_Gw, O_Gd, O_Gq, O_Gv, O_Gy,     /* ModRM reg */
    O_Zb, O_Zv,                             /* register in low opcode bits */
    O_Ib, O_Ibs, O_Iw, O_Iz, O_Iv, O_Ib2,   /* immediates (Ibs: sign-extended) */
    O_Jb, O_Jz, O_Ap,                       /* branch targets, far pointer */
    O_AL, O_CL, O_DX, O_rAX, O_eAX,
    O_M, O_Mp, O_Mq, O_Md,                  /* memory-only ModRM */
    O_Sw, O_Ob, O_Ov,
    O_Xb, O_Xv, O_Yb, O_Yv,                 /* string operands DS:rSI / ES:rDI */
    O_1,
    O_ES, O_CS, O_SS, O_DS, O_FS, O_GS,
    O_Vx, O_Wx, O_Wq, O_Wd, O_Ux,           /* SSE: xmm reg, xmm/mem128, xmm/m64, xmm/m32, xmm reg only */
    O_COUNT
};

#define REG_RIP 16

typedef struct {
    uint64_t addr;
    uint8_t len;
    uint8_t bytes[15];
    uint8_t mode;              /* 16, 32 or 64 */
    uint8_t lock, rep;         /* rep: 0, 0xF2 or 0xF3 */
    int8_t seg;                /* segment override or -1 */
    uint8_t pfx66, pfx67;
    uint8_t rex;               /* REX byte, 0 if absent */
    uint8_t map;               /* 0: one-byte, 1: 0F, 2: 0F38, 3: 0F3A */
    uint8_t opcode;
    uint8_t has_modrm, modrm, mod, reg, rm; /* reg/rm include REX extension bits */
    uint8_t opsize, addrsize;  /* bytes */
    int8_t base, index;        /* -1 for none; base REG_RIP for RIP-relative */
    uint8_t scale;             /* shift count 0..3 */
    int64_t disp;
    uint64_t imm, imm2;
    uint64_t target;           /* resolved Jb/Jz target */
    uint8_t zreg;
    uint8_t cc;                /* condition code for Jcc/SETcc/CMOVcc */
    uint16_t id;
    uint16_t flags;
    uint8_t opnd[3];
    uint8_t vex;               /* 0, 2/3 (VEX bytes) or 4 (EVEX) */
    uint8_t vex_l, vex_vvvv, vex_w;
} Insn;

enum { DEC_OK = 0, DEC_INVALID = -1, DEC_TRUNC = -2 };

/* Decode one instruction from buf (avail bytes). On DEC_INVALID, in->len is
 * still set when the length could be determined. */
int decode(const uint8_t *buf, int avail, uint64_t addr, int mode, Insn *in);
int opnd_size(const Insn *in, int spec);
int opnd_is_mem(const Insn *in, int spec);
extern const char *const mnemonic_names[I_COUNT];
extern const char *const cc_names[16];

/* Disassembly (Intel syntax). symfn may be NULL. */
typedef int (*SymFn)(void *ctx, uint64_t addr, char *buf, size_t n);
void disasm(const Insn *in, char *out, size_t n, SymFn symfn, void *ctx);
const char *reg_name(int reg, int size, int rex);

#endif
