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

static const char *grp6_names[8] = {"sldt", "str", "lldt", "ltr", "verr", "verw", "(bad)", "(bad)"};

static const char *grp7r_name(uint8_t modrm) {
    switch (modrm) {
    case 0xC1: return "vmcall";
    case 0xC8: return "monitor";
    case 0xC9: return "mwait";
    case 0xCA: return "clac";
    case 0xCB: return "stac";
    case 0xD0: return "xgetbv";
    case 0xD1: return "xsetbv";
    case 0xF8: return "swapgs";
    case 0xF9: return "rdtscp";
    }
    return "(grp7)";
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
    case I_MOVLPS: if (in->mod == 3) mn = "movhlps"; break;
    case I_MOVHPS: if (in->mod == 3) mn = "movlhps"; break;
    case I_CMPXCHG8B: if (in->rex & 8) mn = "cmpxchg16b"; break;
    case I_GRP6: mn = grp6_names[in->reg & 7]; break;
    case I_GRP7R: mn = grp7r_name(in->modrm); show_operands = 0; break;
    case I_NOP:
        if (in->map == 1 && in->opcode == 0x1E && in->rep == 0xF3 && (in->modrm == 0xFA || in->modrm == 0xFB)) {
            mn = in->modrm == 0xFA ? "endbr64" : "endbr32";
            show_operands = 0;
        }
        break;
    case I_X87: case I_VEX: case I_EVEX:
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
        case O_Wx: case O_Wq: case O_Wd:
            if (in->mod == 3) put(&o, "%s", xmm[in->rm & 15]);
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
