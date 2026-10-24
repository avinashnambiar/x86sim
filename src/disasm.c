/* Intel-syntax disassembler driven by the decoded Insn. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cpu.h"
#include "decode.h"

static const char *const r64[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
static const char *const r32[16] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                                    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"};
static const char *const r16[16] = {"ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
                                    "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"};
static const char *const r8rex[16] = {"al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
                                      "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"};
static const char *const r8leg[8] = {"al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"};
static const char *const sregs[8] = {"es", "cs", "ss", "ds", "fs", "gs", "?", "?"};
static const char *const mmx[8] = {"mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7"};
static const char *const xmm[16] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                                    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"};

const char *reg_name(int reg, int size, int rex) {
    reg &= 15;
    switch (size) {
    case 1: return (rex || reg >= 8) ? r8rex[reg] : r8leg[reg];
    case 2: return r16[reg];
    case 4: return r32[reg];
    default: return r64[reg];
    }
}

typedef struct {
    char *p;
    size_t left;
} Out;

static void put(Out *o, const char *fmt, ...) {
    if (o->left <= 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->p, o->left, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= o->left) n = (int)o->left - 1;
    o->p += n;
    o->left -= n;
}

static const char *ptr_name(int size) {
    switch (size) {
    case 1: return "byte ptr ";
    case 2: return "word ptr ";
    case 4: return "dword ptr ";
    case 6: return "fword ptr ";
    case 8: return "qword ptr ";
    case 10: return "tbyte ptr ";
    case 16: return "xmmword ptr ";
    case 32: return "ymmword ptr ";
    }
    return "";
}

static void put_sym(Out *o, uint64_t addr, SymFn symfn, void *ctx) {
    char name[128];
    if (symfn && symfn(ctx, addr, name, sizeof name)) put(o, " <%s>", name);
}

static void put_mem(Out *o, const Insn *in, int size, uint64_t *riptarget) {
    put(o, "%s", ptr_name(size));
    if (in->seg >= 0) put(o, "%s:", sregs[in->seg]);
    put(o, "[");
    int any = 0;
    if (in->base == REG_RIP) {
        put(o, "%s", in->addrsize == 8 ? "rip" : "eip");
        any = 1;
        *riptarget = in->addr + in->len + (uint64_t)in->disp;
    } else if (in->base >= 0) {
        put(o, "%s", reg_name(in->base, in->addrsize, 1));
        any = 1;
    }
    if (in->index >= 0) {
        put(o, "%s%s", any ? "+" : "", reg_name(in->index, in->addrsize, 1));
        if (in->scale) put(o, "*%d", 1 << in->scale);
        any = 1;
    }
    if (!any) {
        uint64_t mask = in->addrsize == 8 ? ~0ULL : in->addrsize == 4 ? 0xFFFFFFFFULL : 0xFFFF;
        put(o, "0x%llx", (unsigned long long)((uint64_t)in->disp & mask));
    } else if (in->disp > 0) {
        put(o, "+0x%llx", (unsigned long long)in->disp);
    } else if (in->disp < 0) {
        put(o, "-0x%llx", (unsigned long long)-(uint64_t)in->disp);
    }
    put(o, "]");
}

static int string_elem_size(const Insn *in) {
    switch (in->opnd[0]) {
    case O_Yb: case O_Xb: case O_AL: return 1;
    case O_DX: return in->opnd[1] == O_Xb ? 1 : (in->opsize == 2 ? 2 : 4);
    }
    if (in->id == I_INS) return in->opnd[0] == O_Yb ? 1 : (in->opsize == 2 ? 2 : 4);
    return in->opsize;
}

static const char *grp7r_name(uint8_t modrm) {
    switch (modrm) {
    case 0xC1: return "vmcall";
    case 0xC2: return "vmlaunch";
    case 0xC3: return "vmresume";
    case 0xC4: return "vmxoff";
    case 0xE8: return "serialize";
    case 0xEE: return "rdpkru";
    case 0xEF: return "wrpkru";
    case 0xFA: return "monitorx";
    case 0xFB: return "mwaitx";
    case 0xC8: return "monitor";
    case 0xC9: return "mwait";
    case 0xCA: return "clac";
    case 0xCB: return "stac";
    case 0xD0: return "xgetbv";
    case 0xD5: return "xend";
    case 0xD6: return "xtest";
    case 0xD1: return "xsetbv";
    case 0xF8: return "swapgs";
    case 0xF9: return "rdtscp";
    }
    return "(grp7)";
}

/* ---- x87 ---- */

static const char *const x87_arith[8] = {"fadd", "fmul", "fcom", "fcomp", "fsub", "fsubr", "fdiv", "fdivr"};
static const char *const x87_iarith[8] = {"fiadd", "fimul", "ficom", "ficomp", "fisub", "fisubr", "fidiv", "fidivr"};

/* memory forms: name and operand size, indexed [opcode & 7][reg] */
static const struct { const char *name; uint8_t size; } x87_mem[8][8] = {
    {{"fadd", 4}, {"fmul", 4}, {"fcom", 4}, {"fcomp", 4}, {"fsub", 4}, {"fsubr", 4}, {"fdiv", 4}, {"fdivr", 4}},
    {{"fld", 4}, {0, 0}, {"fst", 4}, {"fstp", 4}, {"fldenv", 0}, {"fldcw", 2}, {"fnstenv", 0}, {"fnstcw", 2}},
    {{"fiadd", 4}, {"fimul", 4}, {"ficom", 4}, {"ficomp", 4}, {"fisub", 4}, {"fisubr", 4}, {"fidiv", 4}, {"fidivr", 4}},
    {{"fild", 4}, {"fisttp", 4}, {"fist", 4}, {"fistp", 4}, {0, 0}, {"fld", 10}, {0, 0}, {"fstp", 10}},
    {{"fadd", 8}, {"fmul", 8}, {"fcom", 8}, {"fcomp", 8}, {"fsub", 8}, {"fsubr", 8}, {"fdiv", 8}, {"fdivr", 8}},
    {{"fld", 8}, {"fisttp", 8}, {"fst", 8}, {"fstp", 8}, {"frstor", 0}, {0, 0}, {"fnsave", 0}, {"fnstsw", 2}},
    {{"fiadd", 2}, {"fimul", 2}, {"ficom", 2}, {"ficomp", 2}, {"fisub", 2}, {"fisubr", 2}, {"fidiv", 2}, {"fidivr", 2}},
    {{"fild", 2}, {"fisttp", 2}, {"fist", 2}, {"fistp", 2}, {"fbld", 10}, {"fild", 8}, {"fbstp", 10}, {"fistp", 8}},
};

static const char *const x87_d9[64] = {
    [0x10] = "fnop",
    [0x20] = "fchs", [0x21] = "fabs", [0x24] = "ftst", [0x25] = "fxam",
    [0x28] = "fld1", [0x29] = "fldl2t", [0x2A] = "fldl2e", [0x2B] = "fldpi", [0x2C] = "fldlg2", [0x2D] = "fldln2",
    [0x2E] = "fldz",
    [0x30] = "f2xm1", [0x31] = "fyl2x", [0x32] = "fptan", [0x33] = "fpatan", [0x34] = "fxtract", [0x35] = "fprem1",
    [0x36] = "fdecstp", [0x37] = "fincstp", [0x38] = "fprem", [0x39] = "fyl2xp1", [0x3A] = "fsqrt", [0x3B] = "fsincos",
    [0x3C] = "frndint", [0x3D] = "fscale", [0x3E] = "fsin", [0x3F] = "fcos",
};

static void disasm_x87(const Insn *in, Out *o, uint64_t *riptarget, int *has_rip) {
    int op = in->opcode & 7, reg = (in->modrm >> 3) & 7, i = in->modrm & 7;
    if (in->mod != 3) {
        const char *n = x87_mem[op][reg].name;
        if (!n) { put(o, "(bad)"); return; }
        put(o, "%s ", n);
        put_mem(o, in, x87_mem[op][reg].size, riptarget);
        *has_rip = in->base == REG_RIP;
        return;
    }
    uint8_t m = in->modrm;
    switch (op) {
    case 0:
        put(o, "%s st, st(%d)", x87_arith[reg], i);
        return;
    case 1:
        if (reg == 0) { put(o, "fld st(%d)", i); return; }
        if (reg == 1) { put(o, "fxch st(%d)", i); return; }
        if (reg == 3) { put(o, "fstp1 st(%d)", i); return; }
        if (x87_d9[m & 0x3F]) { put(o, "%s", x87_d9[m & 0x3F]); return; }
        break;
    case 2: {
        static const char *const fcmov[4] = {"fcmovb", "fcmove", "fcmovbe", "fcmovu"};
        if (reg < 4) { put(o, "%s st, st(%d)", fcmov[reg], i); return; }
        if (m == 0xE9) { put(o, "fucompp"); return; }
        break;
    }
    case 3: {
        static const char *const fcmov[4] = {"fcmovnb", "fcmovne", "fcmovnbe", "fcmovnu"};
        if (reg < 4) { put(o, "%s st, st(%d)", fcmov[reg], i); return; }
        if (m == 0xE2) { put(o, "fnclex"); return; }
        if (m == 0xE3) { put(o, "fninit"); return; }
        if (reg == 5) { put(o, "fucomi st, st(%d)", i); return; }
        if (reg == 6) { put(o, "fcomi st, st(%d)", i); return; }
        break;
    }
    case 4: {
        static const char *const n[8] = {"fadd", "fmul", "fcom2", "fcomp3", "fsubr", "fsub", "fdivr", "fdiv"};
        put(o, "%s st(%d), st", n[reg], i);
        return;
    }
    case 5: {
        static const char *const n[8] = {"ffree", "fxch4", "fst", "fstp", "fucom", "fucomp", 0, 0};
        if (n[reg]) { put(o, "%s st(%d)", n[reg], i); return; }
        break;
    }
    case 6: {
        static const char *const n[8] = {"faddp", "fmulp", "fcomp5", 0, "fsubrp", "fsubp", "fdivrp", "fdivp"};
        if (m == 0xD9) { put(o, "fcompp"); return; }
        if (n[reg]) { put(o, "%s st(%d), st", n[reg], i); return; }
        break;
    }
    case 7:
        if (m == 0xE0) { put(o, "fnstsw ax"); return; }
        if (reg == 0) { put(o, "ffreep st(%d)", i); return; }
        if (reg == 5) { put(o, "fucomip st, st(%d)", i); return; }
        if (reg == 6) { put(o, "fcomip st, st(%d)", i); return; }
        if (reg == 1) { put(o, "fxch7 st(%d)", i); return; }
        if (reg == 2 || reg == 3) { put(o, "fstp%d st(%d)", reg == 2 ? 8 : 9, i); return; }
        break;
    }
    (void)x87_iarith;
    put(o, "(bad)");
}

void disasm(const Insn *in, char *outbuf, size_t n, SymFn symfn, void *ctx) {
    Out o = {outbuf, n};
    outbuf[0] = 0;
    char name[32];
    const char *mn = mnemonic_names[in->id];
    int show_operands = 1;
    int is_string = 0;

    switch (in->id) {
    case I_JCC: snprintf(name, sizeof name, "j%s", cc_names[in->cc]); mn = name; break;
    case I_SETCC: snprintf(name, sizeof name, "set%s", cc_names[in->cc]); mn = name; break;
    case I_CMOVCC: snprintf(name, sizeof name, "cmov%s", cc_names[in->cc]); mn = name; break;
    case I_CBW: mn = in->opsize == 2 ? "cbw" : in->opsize == 4 ? "cwde" : "cdqe"; break;
    case I_CWD: mn = in->opsize == 2 ? "cwd" : in->opsize == 4 ? "cdq" : "cqo"; break;
    case I_PUSHA: mn = in->opsize == 4 ? "pushad" : "pusha"; break;
    case I_POPA: mn = in->opsize == 4 ? "popad" : "popa"; break;
    case I_PUSHF: mn = in->opsize == 8 ? "pushfq" : in->opsize == 4 ? "pushfd" : "pushf"; break;
    case I_POPF: mn = in->opsize == 8 ? "popfq" : in->opsize == 4 ? "popfd" : "popf"; break;
    case I_IRET: mn = in->opsize == 8 ? "iretq" : in->opsize == 4 ? "iretd" : "iret"; break;
    case I_JRCXZ: mn = in->addrsize == 8 ? "jrcxz" : in->addrsize == 4 ? "jecxz" : "jcxz"; break;
    case I_MOVD: if (in->rex & 8) mn = "movq"; break;
    case I_PEXTRD: if (in->rex & 8) mn = "pextrq"; break;
    case I_PINSRD: if (in->rex & 8) mn = "pinsrq"; break;
    case I_MOVLPS: if (in->mod == 3) mn = "movhlps"; break;
    case I_MOVHPS: if (in->mod == 3) mn = "movlhps"; break;
    case I_CMPXCHG8B: if (in->rex & 8) mn = "cmpxchg16b"; break;
    case I_GRP7R: mn = grp7r_name(in->modrm); show_operands = 0; break;
    case I_NOP:
        if (in->map == 1 && in->opcode == 0x1E && in->rep == 0xF3 && (in->modrm == 0xFA || in->modrm == 0xFB)) {
            mn = in->modrm == 0xFA ? "endbr64" : "endbr32";
            show_operands = 0;
        }
        break;
    case I_X87: {
        uint64_t rt = 0;
        int hr = 0;
        disasm_x87(in, &o, &rt, &hr);
        if (hr) {
            put(&o, "    # 0x%llx", (unsigned long long)rt);
            put_sym(&o, rt, symfn, ctx);
        }
        return;
    }
    case I_VEX: case I_EVEX:
        show_operands = 0;
        break;
    case I_MOVS: case I_STOS: case I_LODS: case I_CMPS: case I_SCAS: case I_INS: case I_OUTS: {
        static const char suffix[9] = {0, 'b', 'w', 0, 'd', 0, 0, 0, 'q'};
        snprintf(name, sizeof name, "%s%c", mn, suffix[string_elem_size(in)]);
        mn = name;
        is_string = 1;
        show_operands = 0;
        break;
    }
    }

    if (in->lock) put(&o, "lock ");
    if (is_string && in->rep) {
        int cond = in->id == I_CMPS || in->id == I_SCAS;
        put(&o, "%s", in->rep == 0xF3 ? (cond ? "repe " : "rep ") : "repne ");
    }
    put(&o, "%s", mn);
    if (!show_operands) return;

    uint64_t riptarget = 0;
    int has_rip = 0;
    for (int k = 0; k < 3 && in->opnd[k]; k++) {
        int spec = in->opnd[k], s = opnd_size(in, spec);
        put(&o, k ? ", " : " ");
        switch (spec) {
        case O_Eb: case O_Ew: case O_Ed: case O_Eq: case O_Ev: case O_Ey:
            if (in->mod == 3) put(&o, "%s", reg_name(in->rm, s, in->rex));
            else { put_mem(&o, in, s, &riptarget); has_rip |= in->base == REG_RIP; }
            break;
        case O_M: case O_Mp: case O_Mq: case O_Md:
            put_mem(&o, in, spec == O_M ? 0 : s, &riptarget);
            has_rip |= in->base == REG_RIP;
            break;
        case O_Gb: case O_Gw: case O_Gd: case O_Gq: case O_Gv: case O_Gy:
            put(&o, "%s", reg_name(in->reg, s, in->rex));
            break;
        case O_Zb: case O_Zv:
            put(&o, "%s", reg_name(in->zreg, s, in->rex));
            break;
        case O_Ib: case O_Iw: case O_Ibs: case O_Iz: case O_Iv: {
            uint64_t mask = s >= 8 ? ~0ULL : (1ULL << (s * 8)) - 1;
            put(&o, "0x%llx", (unsigned long long)(in->imm & mask));
            break;
        }
        case O_Ib2: put(&o, "0x%llx", (unsigned long long)in->imm2); break;
        case O_Jb: case O_Jz:
            put(&o, "0x%llx", (unsigned long long)in->target);
            put_sym(&o, in->target, symfn, ctx);
            break;
        case O_Ap: put(&o, "0x%llx:0x%llx", (unsigned long long)in->imm2, (unsigned long long)in->imm); break;
        case O_AL: case O_rAX: case O_eAX: put(&o, "%s", reg_name(RAX, s, 1)); break;
        case O_CL: put(&o, "cl"); break;
        case O_DX: put(&o, "dx"); break;
        case O_1: put(&o, "1"); break;
        case O_Sw: put(&o, "%s", sregs[in->reg & 7]); break;
        case O_ES: case O_CS: case O_SS: case O_DS: case O_FS: case O_GS:
            put(&o, "%s", sregs[spec - O_ES]);
            break;
        case O_Ob: case O_Ov:
            put(&o, "%s%s:0x%llx", ptr_name(s), sregs[in->seg >= 0 ? in->seg : DS], (unsigned long long)in->imm);
            break;
        case O_Vx: put(&o, "%s", xmm[in->reg & 15]); break;
        case O_Ux: put(&o, "%s", xmm[in->rm & 15]); break;
        case O_Wx: case O_Wq: case O_Wd: case O_Ww:
            if (in->mod == 3) put(&o, "%s", xmm[in->rm & 15]);
            else { put_mem(&o, in, s, &riptarget); has_rip |= in->base == REG_RIP; }
            break;
        case O_Pq: put(&o, "%s", mmx[in->reg & 7]); break;
        case O_Cd: put(&o, "cr%d", in->reg); break;
        case O_Dd: put(&o, "dr%d", in->reg & 7); break;
        case O_Ry: put(&o, "%s", reg_name(in->rm, in->mode == 64 ? 8 : 4, 1)); break;
        case O_Nq: put(&o, "%s", mmx[in->rm & 7]); break;
        case O_Qq:
            if (in->mod == 3) put(&o, "%s", mmx[in->rm & 7]);
            else { put_mem(&o, in, s, &riptarget); has_rip |= in->base == REG_RIP; }
            break;
        default:
            put(&o, "?");
        }
    }
    if (has_rip) {
        put(&o, "    # 0x%llx", (unsigned long long)riptarget);
        put_sym(&o, riptarget, symfn, ctx);
    }
}
