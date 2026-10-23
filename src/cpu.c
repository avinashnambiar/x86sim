/* Instruction execution. Faults abort the instruction via longjmp so that
 * RIP still points at the faulting instruction (precise exceptions). */
#include "cpu.h"

#include <stdio.h>
#include <string.h>

#include "cpu_internal.h"
#include "decode.h"

const char *cpu_exc_name(int v) {
    switch (v) {
    case EXC_DE: return "#DE divide error";
    case EXC_DB: return "#DB debug";
    case EXC_BP: return "#BP breakpoint";
    case EXC_OF: return "#OF overflow";
    case EXC_BR: return "#BR bound range exceeded";
    case EXC_UD: return "#UD invalid opcode";
    case EXC_NM: return "#NM device not available";
    case EXC_GP: return "#GP general protection";
    case EXC_PF: return "#PF page fault";
    case EXC_MF: return "#MF x87 floating-point error";
    case EXC_AC: return "#AC alignment check";
    case EXC_XM: return "#XM SIMD floating-point error";
    }
    return "exception";
}

void cpu_init(CPU *c, Mem *m, int bits) {
    memset(c, 0, sizeof *c);
    c->mem = m;
    c->bits = bits;
    c->cpl = bits == 16 ? 0 : 3;
    c->rflags = 0x202;
    c->mxcsr = 0x1F80;
}

void cpu_load_seg_real(CPU *c, int seg, uint16_t sel) {
    c->sreg[seg] = sel;
    c->sbase[seg] = (uint64_t)sel << 4;
}

/* ---- exceptions ---- */

_Noreturn void raise_exc(CPU *c, int vec, uint64_t code, const char *msg) {
    c->exc_vector = vec;
    c->exc_code = code;
    c->exc_unimpl = 0;
    snprintf(c->exc_msg, sizeof c->exc_msg, "%s", msg ? msg : cpu_exc_name(vec));
    _longjmp(c->jb, 1);
}

_Noreturn static void raise_trap(CPU *c, int vec, const char *msg) {
    c->rip = c->next_rip; /* traps report the address after the instruction */
    c->icount++;
    raise_exc(c, vec, 0, msg);
}

_Noreturn void unimplemented(CPU *c, const Insn *in) {
    char text[96];
    disasm(in, text, sizeof text, NULL, NULL);
    c->exc_vector = EXC_UD;
    c->exc_code = 0;
    c->exc_unimpl = 1;
    snprintf(c->exc_msg, sizeof c->exc_msg, "instruction not implemented yet: %s", text);
    _longjmp(c->jb, 1);
}

_Noreturn static void page_fault(CPU *c, uint64_t addr, int write, int fetch) {
    char msg[96];
    c->exc_addr = addr;
    snprintf(msg, sizeof msg, "#PF page fault %s 0x%llx", fetch ? "fetching" : write ? "writing" : "reading",
             (unsigned long long)addr);
    raise_exc(c, EXC_PF, (write ? 2 : 0) | (fetch ? 16 : 0) | (c->cpl == 3 ? 4 : 0), msg);
}

/* ---- memory and registers ---- */

static inline uint64_t lin_mask(CPU *c) { return c->bits == 64 ? ~0ULL : 0xFFFFFFFFULL; }
static inline uint64_t ip_mask(CPU *c) { return c->bits == 64 ? ~0ULL : c->bits == 32 ? 0xFFFFFFFFULL : 0xFFFF; }
uint64_t linear(CPU *c, int seg, uint64_t off) { return (c->sbase[seg] + off) & lin_mask(c); }

/* Host is little-endian (x86-64 / arm64), so guest values copy directly. */
uint64_t rdm(CPU *c, uint64_t a, int s) {
    uint64_t v = 0;
    if (mem_read(c->mem, a, &v, s)) page_fault(c, mem_fault_addr(c->mem), 0, 0);
    return v;
}

void wrm(CPU *c, uint64_t a, int s, uint64_t v) {
    if (mem_write(c->mem, a, &v, s)) page_fault(c, mem_fault_addr(c->mem), 1, 0);
}

void rdm128(CPU *c, uint64_t a, void *dst) {
    if (mem_read(c->mem, a, dst, 16)) page_fault(c, mem_fault_addr(c->mem), 0, 0);
}

void wrm128(CPU *c, uint64_t a, const void *src) {
    if (mem_write(c->mem, a, src, 16)) page_fault(c, mem_fault_addr(c->mem), 1, 0);
}

uint64_t ea_offset(CPU *c, const Insn *in) {
    uint64_t a = (uint64_t)in->disp;
    if (in->base == REG_RIP) a += c->next_rip;
    else if (in->base >= 0) a += c->r[in->base];
    if (in->index >= 0) a += c->r[in->index] << in->scale;
    return a & MASK(in->addrsize);
}

static int ea_seg(const Insn *in) {
    if (in->seg >= 0) return in->seg;
    return (in->base == RSP || in->base == RBP) ? SS : DS;
}

uint64_t ea_linear(CPU *c, const Insn *in) { return linear(c, ea_seg(in), ea_offset(c, in)); }

static void load_seg(CPU *c, int seg, uint16_t sel) {
    if (c->bits == 16) cpu_load_seg_real(c, seg, sel);
    else c->sreg[seg] = sel; /* descriptor tables arrive with protected-mode support */
}

uint64_t rd_op(CPU *c, const Insn *in, int k) {
    int spec = in->opnd[k], s = opnd_size(in, spec);
    switch (spec) {
    case O_Eb: case O_Ew: case O_Ed: case O_Eq: case O_Ev: case O_Ey:
        if (in->mod == 3) return getreg(c, in->rm, s, in->rex);
        return rdm(c, ea_linear(c, in), s);
    case O_Md:
        return rdm(c, ea_linear(c, in), s);
    case O_Gb: case O_Gw: case O_Gd: case O_Gq: case O_Gv: case O_Gy:
        return getreg(c, in->reg, s, in->rex);
    case O_Zb: case O_Zv:
        return getreg(c, in->zreg, s, in->rex);
    case O_Ib: case O_Iw: case O_Ibs: case O_Iz: case O_Iv:
        return in->imm & MASK(s);
    case O_Ib2:
        return in->imm2 & 0xFF;
    case O_Jb: case O_Jz:
        return in->target;
    case O_AL: case O_rAX: case O_eAX:
        return getreg(c, RAX, s, 1);
    case O_CL:
        return c->r[RCX] & 0xFF;
    case O_DX:
        return c->r[RDX] & 0xFFFF;
    case O_1:
        return 1;
    case O_Ob: case O_Ov:
        return rdm(c, linear(c, in->seg >= 0 ? in->seg : DS, in->imm & MASK(in->addrsize)), s);
    case O_Sw:
        if ((in->reg & 7) > 5) raise_exc(c, EXC_UD, 0, NULL);
        return c->sreg[in->reg & 7];
    case O_ES: case O_CS: case O_SS: case O_DS: case O_FS: case O_GS:
        return c->sreg[spec - O_ES];
    }
    unimplemented(c, in);
}

void wr_op(CPU *c, const Insn *in, int k, uint64_t v) {
    int spec = in->opnd[k], s = opnd_size(in, spec);
    switch (spec) {
    case O_Eb: case O_Ew: case O_Ed: case O_Eq: case O_Ev: case O_Ey:
        if (in->mod == 3) setreg(c, in->rm, s, in->rex, v);
        else wrm(c, ea_linear(c, in), s, v);
        return;
    case O_Md:
        wrm(c, ea_linear(c, in), s, v);
        return;
    case O_Gb: case O_Gw: case O_Gd: case O_Gq: case O_Gv: case O_Gy:
        setreg(c, in->reg, s, in->rex, v);
        return;
    case O_Zb: case O_Zv:
        setreg(c, in->zreg, s, in->rex, v);
        return;
    case O_AL: case O_rAX: case O_eAX:
        setreg(c, RAX, s, 1, v);
        return;
    case O_Ob: case O_Ov:
        wrm(c, linear(c, in->seg >= 0 ? in->seg : DS, in->imm & MASK(in->addrsize)), s, v);
        return;
    case O_Sw: {
        int seg = in->reg & 7;
        if (seg > 5 || seg == CS) raise_exc(c, EXC_UD, 0, NULL);
        load_seg(c, seg, (uint16_t)v);
        return;
    }
    case O_ES: case O_CS: case O_SS: case O_DS: case O_FS: case O_GS:
        load_seg(c, spec - O_ES, (uint16_t)v);
        return;
    }
    unimplemented(c, in);
}

/* ---- stack ---- */

static inline int stack_size(CPU *c) { return c->bits == 64 ? 8 : c->bits == 32 ? 4 : 2; }

static void push(CPU *c, uint64_t v, int s) {
    int ss = stack_size(c);
    uint64_t sp = ((c->r[RSP] & MASK(ss)) - s) & MASK(ss);
    wrm(c, linear(c, SS, sp), s, v);
    setreg(c, RSP, ss, 1, sp);
}

static uint64_t pop(CPU *c, int s) {
    int ss = stack_size(c);
    uint64_t sp = c->r[RSP] & MASK(ss);
    uint64_t v = rdm(c, linear(c, SS, sp), s);
    setreg(c, RSP, ss, 1, sp + s);
    return v;
}

/* ---- flags ---- */

static inline uint64_t szp(uint64_t r, int s) {
    uint64_t f = 0;
    r &= MASK(s);
    if (!r) f |= FL_ZF;
    if (r & SIGNBIT(s)) f |= FL_SF;
    if (!__builtin_parity((unsigned)(r & 0xFF))) f |= FL_PF;
    return f;
}

static inline void set_flags(CPU *c, uint64_t mask, uint64_t vals) {
    c->rflags = (c->rflags & ~mask) | (vals & mask);
}

static int cond(CPU *c, int cc) {
    uint64_t f = c->rflags;
    int r;
    switch (cc >> 1) {
    case 0: r = (f & FL_OF) != 0; break;
    case 1: r = (f & FL_CF) != 0; break;
    case 2: r = (f & FL_ZF) != 0; break;
    case 3: r = (f & (FL_CF | FL_ZF)) != 0; break;
    case 4: r = (f & FL_SF) != 0; break;
    case 5: r = (f & FL_PF) != 0; break;
    case 6: r = !(f & FL_SF) != !(f & FL_OF); break;
    default: r = (f & FL_ZF) || (!(f & FL_SF) != !(f & FL_OF)); break;
    }
    return (cc & 1) ? !r : r;
}

static uint64_t alu(CPU *c, int op, uint64_t a, uint64_t b, int s) {
    uint64_t m = MASK(s), sg = SIGNBIT(s), r, f = 0;
    a &= m;
    b &= m;
    switch (op) {
    case I_ADD: case I_ADC: {
        uint64_t ci = op == I_ADC ? (c->rflags & FL_CF) : 0;
        r = (a + b + ci) & m;
        if (s == 8 ? (r < a || (ci && r == a)) : (a + b + ci) > m) f |= FL_CF;
        if ((a ^ r) & (b ^ r) & sg) f |= FL_OF;
        f |= (a ^ b ^ r) & FL_AF;
        break;
    }
    case I_SUB: case I_SBB: case I_CMP: {
        uint64_t ci = op == I_SBB ? (c->rflags & FL_CF) : 0;
        r = (a - b - ci) & m;
        if (s == 8 ? (a < b || (ci && a == b)) : a < b + ci) f |= FL_CF;
        if ((a ^ b) & (a ^ r) & sg) f |= FL_OF;
        f |= (a ^ b ^ r) & FL_AF;
        break;
    }
    case I_AND: case I_TEST: r = a & b; break;
    case I_OR: r = a | b; break;
    case I_XOR: r = a ^ b; break;
    default: r = 0;
    }
    set_flags(c, FL_ARITH, f | szp(r, s));
    return r;
}

static uint64_t shift(CPU *c, int op, uint64_t a, unsigned cnt, int s) {
    unsigned bits = s * 8;
    uint64_t m = MASK(s), sg = SIGNBIT(s), r = a & m;
    uint64_t f = c->rflags;
    int cf, of;
    a &= m;
    cnt &= s == 8 ? 63 : 31;
    if (!cnt) return a;
    switch (op) {
    case I_ROL: {
        unsigned n = cnt % bits;
        if (n) r = ((a << n) | (a >> (bits - n))) & m;
        cf = r & 1;
        of = ((r & sg) != 0) ^ cf;
        set_flags(c, FL_CF | FL_OF, (cf ? FL_CF : 0) | (of ? FL_OF : 0));
        return r;
    }
    case I_ROR: {
        unsigned n = cnt % bits;
        if (n) r = ((a >> n) | (a << (bits - n))) & m;
        cf = (r & sg) != 0;
        of = cf ^ ((r & (sg >> 1)) != 0);
        set_flags(c, FL_CF | FL_OF, (cf ? FL_CF : 0) | (of ? FL_OF : 0));
        return r;
    }
    case I_RCL: {
        unsigned n = cnt % (bits + 1);
        cf = (f & FL_CF) != 0;
        for (unsigned i = 0; i < n; i++) {
            int out = (r & sg) != 0;
            r = ((r << 1) | (uint64_t)cf) & m;
            cf = out;
        }
        of = ((r & sg) != 0) ^ cf;
        set_flags(c, FL_CF | FL_OF, (cf ? FL_CF : 0) | (of ? FL_OF : 0));
        return r;
    }
    case I_RCR: {
        unsigned n = cnt % (bits + 1);
        cf = (f & FL_CF) != 0;
        of = ((a & sg) != 0) ^ cf;
        for (unsigned i = 0; i < n; i++) {
            int out = r & 1;
            r = (r >> 1) | (cf ? sg : 0);
            cf = out;
        }
        set_flags(c, FL_CF | FL_OF, (cf ? FL_CF : 0) | (of ? FL_OF : 0));
        return r;
    }
    case I_SHL: case I_SAL:
        cf = cnt <= bits ? (a >> (bits - cnt)) & 1 : 0;
        r = (a << cnt) & m;
        of = ((r & sg) != 0) ^ cf;
        break;
    case I_SHR:
        cf = cnt <= bits ? (a >> (cnt - 1)) & 1 : 0;
        r = a >> cnt;
        of = (a & sg) != 0;
        break;
    default: { /* SAR */
        int64_t sa = (int64_t)sext(a, s);
        cf = (int)((sa >> (cnt - 1)) & 1);
        r = (uint64_t)(sa >> cnt) & m;
        of = 0;
        break;
    }
    }
    set_flags(c, FL_ARITH, szp(r, s) | (cf ? FL_CF : 0) | (of ? FL_OF : 0));
    return r;
}

static uint64_t shift_double(CPU *c, int op, uint64_t dst, uint64_t src, unsigned cnt, int s) {
    unsigned bits = s * 8;
    uint64_t m = MASK(s), sg = SIGNBIT(s), r;
    int cf;
    dst &= m;
    src &= m;
    cnt &= s == 8 ? 63 : 31;
    if (!cnt) return dst;
    if (cnt > bits) cnt = bits; /* result undefined by the SDM */
    if (op == I_SHLD) {
        u128 v = ((u128)dst << bits) | src;
        r = (uint64_t)((v << cnt) >> bits) & m;
        cf = (dst >> (bits - cnt)) & 1;
    } else {
        u128 v = ((u128)src << bits) | dst;
        r = (uint64_t)(v >> cnt) & m;
        cf = (dst >> (cnt - 1)) & 1;
    }
    set_flags(c, FL_ARITH, szp(r, s) | (cf ? FL_CF : 0) | (((r ^ dst) & sg) ? FL_OF : 0));
    return r;
}

/* ---- string instructions ---- */

static void set_areg(CPU *c, int r, int asz, uint64_t v) { setreg(c, r, asz, 1, v); }

static void string_op(CPU *c, const Insn *in) {
    int s = 1;
    switch (in->opnd[0] == O_DX ? in->opnd[1] : in->opnd[0]) {
    case O_Xb: case O_Yb: case O_AL: s = 1; break;
    default: s = in->opsize;
    }
    if (in->opnd[1] == O_Xb || in->opnd[1] == O_Yb) s = 1;
    if ((in->id == I_INS || in->id == I_OUTS) && s == 8) s = 4;

    int asz = in->addrsize, seg = in->seg >= 0 ? in->seg : DS;
    uint64_t am = MASK(asz);
    int64_t d = (c->rflags & FL_DF) ? -s : s;
    int rep = in->rep;
    int id = in->id;

    for (;;) {
        if (rep && (c->r[RCX] & am) == 0) break;
        uint64_t si = c->r[RSI] & am, di = c->r[RDI] & am;
        switch (id) {
        case I_MOVS:
            wrm(c, linear(c, ES, di), s, rdm(c, linear(c, seg, si), s));
            set_areg(c, RSI, asz, si + d);
            set_areg(c, RDI, asz, di + d);
            break;
        case I_STOS:
            wrm(c, linear(c, ES, di), s, getreg(c, RAX, s, 1));
            set_areg(c, RDI, asz, di + d);
            break;
        case I_LODS:
            setreg(c, RAX, s, 1, rdm(c, linear(c, seg, si), s));
            set_areg(c, RSI, asz, si + d);
            break;
        case I_CMPS: {
            uint64_t a = rdm(c, linear(c, seg, si), s), b = rdm(c, linear(c, ES, di), s);
            alu(c, I_CMP, a, b, s);
            set_areg(c, RSI, asz, si + d);
            set_areg(c, RDI, asz, di + d);
            break;
        }
        case I_SCAS:
            alu(c, I_CMP, getreg(c, RAX, s, 1), rdm(c, linear(c, ES, di), s), s);
            set_areg(c, RDI, asz, di + d);
            break;
        default: { /* INS / OUTS */
            uint16_t port = c->r[RDX] & 0xFFFF;
            if (c->cpl > 0) raise_exc(c, EXC_GP, 0, "#GP I/O instruction in user mode");
            uint32_t v = 0xFFFFFFFF;
            if (id == I_INS) {
                if (c->on_port_in) c->on_port_in(c, port, s, &v);
                wrm(c, linear(c, ES, di), s, v);
                set_areg(c, RDI, asz, di + d);
            } else {
                v = (uint32_t)rdm(c, linear(c, seg, si), s);
                if (c->on_port_out) c->on_port_out(c, port, s, v);
                set_areg(c, RSI, asz, si + d);
            }
        }
        }
        if (!rep) break;
        set_areg(c, RCX, asz, (c->r[RCX] & am) - 1);
        if (id == I_CMPS || id == I_SCAS) {
            int zf = (c->rflags & FL_ZF) != 0;
            if ((rep == 0xF3 && !zf) || (rep == 0xF2 && zf)) break;
        }
    }
}

/* ---- misc helpers ---- */

static void cpuid(CPU *c) {
    uint32_t leaf = (uint32_t)c->r[RAX], sub = (uint32_t)c->r[RCX];
    uint32_t a = 0, b = 0, cc = 0, d = 0;
    static const char brand[48] = "x86sim Virtual x86-64 CPU";
    (void)sub;
    switch (leaf) {
    case 0:
        a = 7;
        memcpy(&b, "Genu", 4);
        memcpy(&d, "ineI", 4);
        memcpy(&cc, "ntel", 4);
        break;
    case 1:
        a = 0x000906EA;            /* family 6 */
        b = 0x00010800;            /* CLFLUSH line 64 bytes, 1 logical CPU */
        cc = (1u << 13)            /* CMPXCHG16B */
           | (1u << 23)            /* POPCNT */
           | (1u << 30);           /* RDRAND */
        d = (1u << 4)              /* TSC */
          | (1u << 8)              /* CX8 */
          | (1u << 15)             /* CMOV */
          | (1u << 19)             /* CLFSH */
          | (1u << 25)             /* SSE (partial) */
          | (1u << 26);            /* SSE2 (partial) */
        break;
    case 7:
        if (sub == 0) b = 1u << 18; /* RDSEED */
        break;
    case 0x80000000:
        a = 0x80000004;
        break;
    case 0x80000001:
        cc = (1u << 0) | (1u << 5); /* LAHF/SAHF in 64-bit, LZCNT */
        d = (1u << 11) | (1u << 20) | (1u << 29); /* SYSCALL, NX, long mode */
        break;
    case 0x80000002: case 0x80000003: case 0x80000004: {
        const uint32_t *p = (const uint32_t *)(brand + (leaf - 0x80000002) * 16);
        a = p[0]; b = p[1]; cc = p[2]; d = p[3];
        break;
    }
    }
    c->r[RAX] = a;
    c->r[RBX] = b;
    c->r[RCX] = cc;
    c->r[RDX] = d;
}

static void load_cs(CPU *c, uint16_t sel) {
    if (c->bits != 16) return; /* protected-mode far transfers not yet supported */
    cpu_load_seg_real(c, CS, sel);
}

static void do_interrupt(CPU *c, int vec) {
    if (c->on_interrupt && c->on_interrupt(c, vec)) return;
    if (c->bits == 16) {
        /* Real mode: dispatch through the interrupt vector table at 0. */
        uint32_t ent = (uint32_t)rdm(c, (uint64_t)vec * 4, 4);
        if (ent) {
            push(c, c->rflags & 0xFFFF, 2);
            push(c, c->sreg[CS], 2);
            push(c, c->next_rip, 2);
            c->rflags &= ~(FL_IF | FL_TF);
            load_cs(c, (uint16_t)(ent >> 16));
            c->next_rip = ent & 0xFFFF;
            return;
        }
    }
    char msg[64];
    snprintf(msg, sizeof msg, "unhandled software interrupt 0x%02x", vec);
    raise_exc(c, EXC_GP, (uint64_t)vec * 8 + 2, msg);
}

static uint64_t rng_state = 0x853c49e6748fea9bULL;
static uint64_t next_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* ---- the instruction dispatcher ---- */

static void execute(CPU *c, const Insn *in) {
    int s = opnd_size(in, in->opnd[0]);
    int os = in->opsize;
    uint64_t a, b, r;

    switch (in->id) {
    case I_ADD: case I_OR: case I_ADC: case I_SBB: case I_AND: case I_SUB: case I_XOR:
        a = rd_op(c, in, 0);
        b = rd_op(c, in, 1);
        wr_op(c, in, 0, alu(c, in->id, a, b, s));
        break;
    case I_CMP: case I_TEST:
        alu(c, in->id, rd_op(c, in, 0), rd_op(c, in, 1), s);
        break;
    case I_INC: case I_DEC: {
        uint64_t cf = c->rflags & FL_CF;
        a = rd_op(c, in, 0);
        r = alu(c, in->id == I_INC ? I_ADD : I_SUB, a, 1, s);
        c->rflags = (c->rflags & ~FL_CF) | cf;
        wr_op(c, in, 0, r);
        break;
    }
    case I_NEG:
        a = rd_op(c, in, 0);
        r = alu(c, I_SUB, 0, a, s);
        wr_op(c, in, 0, r);
        break;
    case I_NOT:
        wr_op(c, in, 0, ~rd_op(c, in, 0));
        break;

    case I_MUL: case I_IMUL: {
        int nops = in->opnd[2] ? 3 : in->opnd[1] ? 2 : 1;
        uint64_t m = MASK(s);
        int ov;
        if (nops == 1) {
            b = rd_op(c, in, 0);
            if (in->id == I_MUL) {
                if (s == 1) {
                    r = (c->r[RAX] & 0xFF) * b;
                    setreg(c, RAX, 2, 1, r);
                    ov = (r >> 8) != 0;
                } else {
                    u128 p = (u128)getreg(c, RAX, s, 1) * b;
                    uint64_t hi = (uint64_t)(p >> (s * 8)) & m;
                    setreg(c, RAX, s, 1, (uint64_t)p & m);
                    setreg(c, RDX, s, 1, hi);
                    ov = hi != 0;
                }
            } else {
                s128 p = (s128)(int64_t)sext(getreg(c, RAX, s, 1), s) * (int64_t)sext(b, s);
                uint64_t lo = (uint64_t)p & m;
                ov = p != (s128)(int64_t)sext(lo, s);
                if (s == 1) {
                    setreg(c, RAX, 2, 1, (uint64_t)p & 0xFFFF);
                } else {
                    setreg(c, RAX, s, 1, lo);
                    setreg(c, RDX, s, 1, (uint64_t)(p >> (s * 8)) & m);
                }
                set_flags(c, FL_SF | FL_ZF | FL_PF, szp(lo, s));
            }
        } else {
            a = nops == 3 ? rd_op(c, in, 1) : rd_op(c, in, 0);
            b = nops == 3 ? rd_op(c, in, 2) : rd_op(c, in, 1);
            s128 p = (s128)(int64_t)sext(a, s) * (int64_t)sext(b, s);
            uint64_t lo = (uint64_t)p & m;
            ov = p != (s128)(int64_t)sext(lo, s);
            set_flags(c, FL_SF | FL_ZF | FL_PF, szp(lo, s));
            wr_op(c, in, 0, lo);
        }
        set_flags(c, FL_CF | FL_OF, ov ? (FL_CF | FL_OF) : 0);
        break;
    }
    case I_DIV: {
        uint64_t d = rd_op(c, in, 0), m = MASK(s);
        if (!d) raise_exc(c, EXC_DE, 0, "#DE divide by zero");
        if (s == 1) {
            uint64_t n = c->r[RAX] & 0xFFFF, q = n / d;
            if (q > 0xFF) raise_exc(c, EXC_DE, 0, "#DE quotient overflow");
            setreg(c, RAX, 2, 1, ((n % d) << 8) | q);
        } else {
            u128 n = ((u128)getreg(c, RDX, s, 1) << (s * 8)) | getreg(c, RAX, s, 1);
            u128 q = n / d;
            if (q > m) raise_exc(c, EXC_DE, 0, "#DE quotient overflow");
            setreg(c, RAX, s, 1, (uint64_t)q);
            setreg(c, RDX, s, 1, (uint64_t)(n % d));
        }
        break;
    }
    case I_IDIV: {
        int64_t d = (int64_t)sext(rd_op(c, in, 0), s);
        if (!d) raise_exc(c, EXC_DE, 0, "#DE divide by zero");
        int bits = s * 8;
        s128 n;
        if (s == 1) {
            n = (int16_t)(c->r[RAX] & 0xFFFF);
        } else {
            u128 un = ((u128)getreg(c, RDX, s, 1) << bits) | getreg(c, RAX, s, 1);
            if (s == 8) n = (s128)un;
            else n = (int64_t)((uint64_t)un << (64 - 2 * bits)) >> (64 - 2 * bits);
        }
        s128 min128 = (s128)((u128)1 << 127);
        if (d == -1 && n == min128) raise_exc(c, EXC_DE, 0, "#DE quotient overflow");
        s128 q = n / d, rem = n % d;
        s128 hi = (s128)(((u128)1 << (bits - 1)) - 1), lo = -hi - 1;
        if (q > hi || q < lo) raise_exc(c, EXC_DE, 0, "#DE quotient overflow");
        if (s == 1) setreg(c, RAX, 2, 1, (((uint64_t)rem & 0xFF) << 8) | ((uint64_t)q & 0xFF));
        else {
            setreg(c, RAX, s, 1, (uint64_t)q);
            setreg(c, RDX, s, 1, (uint64_t)rem);
        }
        break;
    }

    case I_ROL: case I_ROR: case I_RCL: case I_RCR: case I_SHL: case I_SHR: case I_SAL: case I_SAR:
        a = rd_op(c, in, 0);
        b = rd_op(c, in, 1);
        wr_op(c, in, 0, shift(c, in->id, a, (unsigned)b, s));
        break;
    case I_SHLD: case I_SHRD:
        a = rd_op(c, in, 0);
        b = rd_op(c, in, 1);
        wr_op(c, in, 0, shift_double(c, in->id, a, b, (unsigned)rd_op(c, in, 2), s));
        break;

    case I_MOV:
        a = rd_op(c, in, 1);
        if (in->opnd[1] == O_Sw && in->mod != 3) wrm(c, ea_linear(c, in), 2, a);
        else wr_op(c, in, 0, a);
        break;
    case I_MOVZX:
        wr_op(c, in, 0, rd_op(c, in, 1));
        break;
    case I_MOVSX: case I_MOVSXD:
        a = rd_op(c, in, 1);
        wr_op(c, in, 0, sext(a, opnd_size(in, in->opnd[1])) & MASK(os));
        break;
    case I_LEA:
        if (in->mod == 3) raise_exc(c, EXC_UD, 0, NULL);
        wr_op(c, in, 0, ea_offset(c, in) & MASK(os));
        break;
    case I_XCHG:
        a = rd_op(c, in, 0);
        b = rd_op(c, in, 1);
        wr_op(c, in, 0, b);
        wr_op(c, in, 1, a);
        break;
    case I_CMPXCHG:
        a = rd_op(c, in, 0);
        alu(c, I_CMP, getreg(c, RAX, s, 1), a, s);
        if (c->rflags & FL_ZF) wr_op(c, in, 0, rd_op(c, in, 1));
        else {
            wr_op(c, in, 0, a);
            setreg(c, RAX, s, 1, a);
        }
        break;
    case I_CMPXCHG8B: {
        uint64_t addr = ea_linear(c, in);
        if (in->rex & 8) {
            if (addr & 15) raise_exc(c, EXC_GP, 0, "#GP misaligned cmpxchg16b");
            uint64_t lo = rdm(c, addr, 8), hi = rdm(c, addr + 8, 8);
            if (lo == c->r[RAX] && hi == c->r[RDX]) {
                wrm(c, addr, 8, c->r[RBX]);
                wrm(c, addr + 8, 8, c->r[RCX]);
                c->rflags |= FL_ZF;
            } else {
                c->r[RAX] = lo;
                c->r[RDX] = hi;
                c->rflags &= ~FL_ZF;
            }
        } else {
            uint64_t v = rdm(c, addr, 8);
            uint64_t cmpv = (c->r[RAX] & 0xFFFFFFFF) | (c->r[RDX] << 32);
            if (v == cmpv) {
                wrm(c, addr, 8, (c->r[RBX] & 0xFFFFFFFF) | (c->r[RCX] << 32));
                c->rflags |= FL_ZF;
            } else {
                setreg(c, RAX, 4, 1, v);
                setreg(c, RDX, 4, 1, v >> 32);
                c->rflags &= ~FL_ZF;
            }
        }
        break;
    }
    case I_XADD:
        a = rd_op(c, in, 0);
        b = rd_op(c, in, 1);
        r = alu(c, I_ADD, a, b, s);
        wr_op(c, in, 1, a);
        wr_op(c, in, 0, r);
        break;
    case I_BSWAP:
        a = rd_op(c, in, 0);
        wr_op(c, in, 0, s == 8 ? __builtin_bswap64(a) : s == 4 ? __builtin_bswap32((uint32_t)a) : 0);
        break;
    case I_CBW:
        setreg(c, RAX, os, 1, sext(getreg(c, RAX, os / 2, 1), os / 2));
        break;
    case I_CWD:
        setreg(c, RDX, os, 1, (getreg(c, RAX, os, 1) & SIGNBIT(os)) ? ~0ULL : 0);
        break;

    case I_PUSH:
        a = rd_op(c, in, 0);
        if (in->opnd[0] >= O_ES && in->opnd[0] <= O_GS) push(c, a, os);
        else push(c, a, s);
        break;
    case I_POP:
        if (in->opnd[0] >= O_ES && in->opnd[0] <= O_GS) wr_op(c, in, 0, pop(c, os));
        else wr_op(c, in, 0, pop(c, s));
        break;
    case I_PUSHA: {
        uint64_t sp = getreg(c, RSP, os, 1);
        for (int i = 0; i < 8; i++) push(c, i == RSP ? sp : getreg(c, i, os, 1), os);
        break;
    }
    case I_POPA:
        for (int i = 7; i >= 0; i--) {
            uint64_t v = pop(c, os);
            if (i != RSP) setreg(c, i, os, 1, v);
        }
        break;
    case I_PUSHF:
        push(c, c->rflags & 0x00FCFFFF & MASK(os), os);
        break;
    case I_POPF: {
        uint64_t v = pop(c, os);
        uint64_t mask = FL_ARITH | FL_TF | FL_DF | FL_AC | FL_ID;
        if (c->cpl == 0) mask |= FL_IF | 0x3000;
        if (os == 2) mask &= 0xFFFF;
        c->rflags = ((c->rflags & ~mask) | (v & mask)) | 2;
        break;
    }
    case I_LEAVE: {
        int ss = stack_size(c);
        setreg(c, RSP, ss, 1, getreg(c, RBP, ss, 1));
        setreg(c, RBP, os, 1, pop(c, os));
        break;
    }
    case I_ENTER: {
        uint64_t size = in->imm & 0xFFFF;
        int level = (int)(in->imm2 & 31), ss = stack_size(c);
        push(c, getreg(c, RBP, os, 1), os);
        uint64_t frame = getreg(c, RSP, ss, 1);
        uint64_t bp = getreg(c, RBP, ss, 1);
        for (int i = 1; i < level; i++) {
            bp -= os;
            push(c, rdm(c, linear(c, SS, bp & MASK(ss)), os), os);
        }
        if (level > 0) push(c, frame, os);
        setreg(c, RBP, os, 1, frame);
        setreg(c, RSP, ss, 1, getreg(c, RSP, ss, 1) - size);
        break;
    }

    case I_JMP:
        c->next_rip = rd_op(c, in, 0) & MASK(os) & ip_mask(c);
        break;
    case I_JCC:
        if (cond(c, in->cc)) c->next_rip = in->target;
        break;
    case I_CALL:
        a = rd_op(c, in, 0) & MASK(os) & ip_mask(c);
        push(c, c->next_rip, os);
        c->next_rip = a;
        break;
    case I_RET:
        a = pop(c, os);
        if (in->opnd[0] == O_Iw) {
            int ss = stack_size(c);
            setreg(c, RSP, ss, 1, getreg(c, RSP, ss, 1) + (in->imm & 0xFFFF));
        }
        c->next_rip = a & ip_mask(c);
        break;
    case I_LOOP: case I_LOOPE: case I_LOOPNE: {
        int asz = in->addrsize;
        uint64_t cnt = (getreg(c, RCX, asz, 1) - 1) & MASK(asz);
        setreg(c, RCX, asz, 1, cnt);
        int zf = (c->rflags & FL_ZF) != 0;
        int take = cnt != 0 && (in->id == I_LOOP || (in->id == I_LOOPE ? zf : !zf));
        if (take) c->next_rip = in->target;
        break;
    }
    case I_JRCXZ:
        if (getreg(c, RCX, in->addrsize, 1) == 0) c->next_rip = in->target;
        break;
    case I_SETCC:
        wr_op(c, in, 0, cond(c, in->cc));
        break;
    case I_CMOVCC:
        a = rd_op(c, in, 1);
        if (cond(c, in->cc)) wr_op(c, in, 0, a);
        else if (os == 4) setreg(c, in->reg, 4, 1, getreg(c, in->reg, 4, 1)); /* zero-extends */
        break;

    case I_JMPF: case I_CALLF: {
        if (c->bits != 16) unimplemented(c, in);
        uint64_t off;
        uint16_t sel;
        if (in->opnd[0] == O_Ap) {
            off = in->imm;
            sel = (uint16_t)in->imm2;
        } else {
            uint64_t ad = ea_linear(c, in);
            off = rdm(c, ad, os);
            sel = (uint16_t)rdm(c, ad + os, 2);
        }
        if (in->id == I_CALLF) {
            push(c, c->sreg[CS], os);
            push(c, c->next_rip, os);
        }
        load_cs(c, sel);
        c->next_rip = off & ip_mask(c);
        break;
    }
    case I_RETF: {
        if (c->bits != 16) unimplemented(c, in);
        uint64_t off = pop(c, os);
        uint16_t sel = (uint16_t)pop(c, os);
        if (in->opnd[0] == O_Iw) c->r[RSP] = (c->r[RSP] & ~0xFFFFULL) | ((c->r[RSP] + in->imm) & 0xFFFF);
        load_cs(c, sel);
        c->next_rip = off & ip_mask(c);
        break;
    }
    case I_IRET: {
        if (c->bits != 16) unimplemented(c, in);
        uint64_t off = pop(c, os);
        uint16_t sel = (uint16_t)pop(c, os);
        uint64_t fl = pop(c, os);
        load_cs(c, sel);
        c->next_rip = off & ip_mask(c);
        c->rflags = (c->rflags & ~0xFFFFULL) | (fl & 0x7FD5) | 2;
        break;
    }
    case I_LES: case I_LDS: case I_LSS: case I_LFS: case I_LGS: {
        static const int segs[] = {[I_LES] = ES, [I_LDS] = DS, [I_LSS] = SS, [I_LFS] = FS, [I_LGS] = GS};
        uint64_t ad = ea_linear(c, in);
        uint64_t off = rdm(c, ad, os);
        uint16_t sel = (uint16_t)rdm(c, ad + os, 2);
        load_seg(c, segs[in->id], sel);
        setreg(c, in->reg, os, in->rex, off);
        break;
    }

    case I_BT: case I_BTS: case I_BTR: case I_BTC: {
        unsigned bits = s * 8;
        uint64_t off = rd_op(c, in, 1);
        uint64_t v, addr = 0;
        unsigned bit;
        int memform = in->mod != 3;
        if (memform) {
            addr = ea_linear(c, in);
            if (in->opnd[1] != O_Ib) {
                int64_t soff = (int64_t)sext(off, s);
                addr = (addr + (uint64_t)((soff >> (bits == 64 ? 6 : bits == 32 ? 5 : 4)) * s)) & lin_mask(c);
                bit = (unsigned)(soff & (bits - 1));
            } else {
                bit = (unsigned)(off & (bits - 1));
            }
            v = rdm(c, addr, s);
        } else {
            bit = (unsigned)(off & (bits - 1));
            v = getreg(c, in->rm, s, in->rex);
        }
        int cf = (v >> bit) & 1;
        c->rflags = (c->rflags & ~FL_CF) | (uint64_t)cf;
        if (in->id == I_BT) break;
        if (in->id == I_BTS) v |= 1ULL << bit;
        else if (in->id == I_BTR) v &= ~(1ULL << bit);
        else v ^= 1ULL << bit;
        if (memform) wrm(c, addr, s, v);
        else setreg(c, in->rm, s, in->rex, v);
        break;
    }
    case I_BSF: case I_BSR:
        a = rd_op(c, in, 1);
        if (!a) {
            c->rflags |= FL_ZF;
        } else {
            c->rflags &= ~FL_ZF;
            wr_op(c, in, 0, in->id == I_BSF ? (uint64_t)__builtin_ctzll(a) : (uint64_t)(63 - __builtin_clzll(a)));
        }
        break;
    case I_TZCNT: case I_LZCNT: {
        unsigned bits = s * 8;
        a = rd_op(c, in, 1);
        if (in->id == I_TZCNT) r = a ? (uint64_t)__builtin_ctzll(a) : bits;
        else r = a ? (uint64_t)__builtin_clzll(a) - (64 - bits) : bits;
        set_flags(c, FL_CF | FL_ZF, (a == 0 ? FL_CF : 0) | (r == 0 ? FL_ZF : 0));
        wr_op(c, in, 0, r);
        break;
    }
    case I_POPCNT:
        a = rd_op(c, in, 1);
        r = (uint64_t)__builtin_popcountll(a);
        set_flags(c, FL_ARITH, a == 0 ? FL_ZF : 0);
        wr_op(c, in, 0, r);
        break;

    case I_MOVS: case I_STOS: case I_LODS: case I_CMPS: case I_SCAS: case I_INS: case I_OUTS:
        string_op(c, in);
        break;

    case I_CLC: c->rflags &= ~FL_CF; break;
    case I_STC: c->rflags |= FL_CF; break;
    case I_CMC: c->rflags ^= FL_CF; break;
    case I_CLD: c->rflags &= ~FL_DF; break;
    case I_STD: c->rflags |= FL_DF; break;
    case I_CLI: case I_STI:
        if (c->cpl > 0) raise_exc(c, EXC_GP, 0, "#GP cli/sti in user mode");
        if (in->id == I_CLI) c->rflags &= ~FL_IF;
        else c->rflags |= FL_IF;
        break;
    case I_LAHF:
        setreg(c, RSP, 1, 0, (c->rflags & 0xD5) | 2); /* AH is register 4 without REX */
        break;
    case I_SAHF:
        c->rflags = (c->rflags & ~0xD5ULL) | (getreg(c, RSP, 1, 0) & 0xD5) | 2;
        break;

    case I_NOP: case I_PAUSE: case I_PREFETCH: case I_FWAIT:
    case I_LFENCE: case I_MFENCE: case I_SFENCE: case I_CLFLUSH:
        break;
    case I_HLT:
        if (c->cpl > 0) raise_exc(c, EXC_GP, 0, "#GP hlt in user mode");
        raise_trap(c, -1, "CPU halted");
    case I_INT3:
        raise_trap(c, EXC_BP, "breakpoint (int3)");
    case I_INT1:
        raise_trap(c, EXC_DB, "int1");
    case I_INTO:
        if (c->rflags & FL_OF) raise_trap(c, EXC_OF, NULL);
        break;
    case I_INT:
        do_interrupt(c, (int)(in->imm & 0xFF));
        break;
    case I_SYSCALL:
        if (c->bits != 64) raise_exc(c, EXC_UD, 0, NULL);
        c->r[RCX] = c->next_rip;
        c->r[R11] = c->rflags;
        if (!c->on_syscall || !c->on_syscall(c)) raise_exc(c, EXC_UD, 0, "syscall with no OS emulation");
        break;
    case I_CPUID:
        cpuid(c);
        break;
    case I_RDTSC:
        /* Deterministic time stamp: the instruction count. */
        setreg(c, RAX, 4, 1, c->icount);
        setreg(c, RDX, 4, 1, c->icount >> 32);
        break;
    case I_RDRAND: case I_RDSEED:
        wr_op(c, in, 0, next_random());
        set_flags(c, FL_ARITH, FL_CF);
        break;
    case I_UD2:
        raise_exc(c, EXC_UD, 0, "#UD (ud2)");
    case I_XLAT: {
        uint64_t off = (getreg(c, RBX, in->addrsize, 1) + (c->r[RAX] & 0xFF)) & MASK(in->addrsize);
        setreg(c, RAX, 1, 1, rdm(c, linear(c, in->seg >= 0 ? in->seg : DS, off), 1));
        break;
    }
    case I_IN: case I_OUT: {
        if (c->cpl > 0) raise_exc(c, EXC_GP, 0, "#GP I/O instruction in user mode");
        int sz = in->id == I_IN ? s : opnd_size(in, in->opnd[1]);
        uint16_t port = (uint16_t)(in->id == I_IN ? rd_op(c, in, 1) : rd_op(c, in, 0));
        if (in->id == I_IN) {
            uint32_t v = 0xFFFFFFFF;
            if (c->on_port_in) c->on_port_in(c, port, sz, &v);
            wr_op(c, in, 0, v);
        } else if (c->on_port_out) {
            c->on_port_out(c, port, sz, (uint32_t)rd_op(c, in, 1));
        }
        break;
    }

    case I_DAA: case I_DAS: {
        uint8_t al = c->r[RAX] & 0xFF, old = al;
        int cf = (c->rflags & FL_CF) != 0, af = 0;
        if ((al & 0xF) > 9 || (c->rflags & FL_AF)) {
            al = in->id == I_DAA ? al + 6 : al - 6;
            af = 1;
        }
        if (old > 0x99 || cf) {
            al = in->id == I_DAA ? al + 0x60 : al - 0x60;
            cf = 1;
        }
        setreg(c, RAX, 1, 1, al);
        set_flags(c, FL_ARITH, szp(al, 1) | (cf ? FL_CF : 0) | (af ? FL_AF : 0));
        break;
    }
    case I_AAA: case I_AAS: {
        uint16_t ax = c->r[RAX] & 0xFFFF;
        int adj = (ax & 0xF) > 9 || (c->rflags & FL_AF);
        if (adj) {
            if (in->id == I_AAA) ax = (uint16_t)(ax + 0x106);
            else ax = (uint16_t)((((ax >> 8) - 1) << 8) | ((ax - 6) & 0xFF));
        }
        ax &= 0xFF0F;
        setreg(c, RAX, 2, 1, ax);
        set_flags(c, FL_CF | FL_AF, adj ? (FL_CF | FL_AF) : 0);
        break;
    }
    case I_AAM: {
        uint8_t base = in->imm & 0xFF, al = c->r[RAX] & 0xFF;
        if (!base) raise_exc(c, EXC_DE, 0, "#DE aam with zero base");
        uint8_t res = al % base;
        setreg(c, RAX, 2, 1, ((uint16_t)(al / base) << 8) | res);
        set_flags(c, FL_SF | FL_ZF | FL_PF, szp(res, 1));
        break;
    }
    case I_AAD: {
        uint8_t base = in->imm & 0xFF;
        uint8_t al = (uint8_t)((c->r[RAX] & 0xFF) + ((c->r[RAX] >> 8) & 0xFF) * base);
        setreg(c, RAX, 2, 1, al);
        set_flags(c, FL_SF | FL_ZF | FL_PF, szp(al, 1));
        break;
    }
    case I_SALC:
        setreg(c, RAX, 1, 1, (c->rflags & FL_CF) ? 0xFF : 0);
        break;

    case I_GRP7R:
        if (in->modrm == 0xD0) { /* xgetbv */
            if ((c->r[RCX] & 0xFFFFFFFF) != 0) raise_exc(c, EXC_GP, 0, NULL);
            setreg(c, RAX, 4, 1, 3); /* x87 + SSE state */
            setreg(c, RDX, 4, 1, 0);
            break;
        }
        if (in->modrm == 0xF9) { /* rdtscp */
            setreg(c, RAX, 4, 1, c->icount);
            setreg(c, RDX, 4, 1, c->icount >> 32);
            setreg(c, RCX, 4, 1, 0);
            break;
        }
        if (c->cpl > 0) raise_exc(c, EXC_GP, 0, "#GP privileged instruction");
        unimplemented(c, in);
    case I_SGDT: case I_SIDT: case I_LGDT: case I_LIDT: case I_LMSW: case I_INVLPG: case I_GRP6:
        if (c->cpl > 0) raise_exc(c, EXC_GP, 0, "#GP privileged instruction");
        unimplemented(c, in);

    case I_LDMXCSR:
        a = rd_op(c, in, 0);
        if (a & 0xFFFF0000) raise_exc(c, EXC_GP, 0, NULL);
        c->mxcsr = (uint32_t)a;
        break;
    case I_STMXCSR:
        wr_op(c, in, 0, c->mxcsr);
        break;

    case I_MOVNTI:
        wr_op(c, in, 0, rd_op(c, in, 1));
        break;

    default:
        if (in->id >= I_MOVUPS && in->id < I_MOVNTI) {
            exec_sse(c, in);
            break;
        }
        unimplemented(c, in);
    }
}

StepResult cpu_step(CPU *c) {
    Insn in;
    uint8_t buf[15];
    c->stop_request = 0;
    if (_setjmp(c->jb)) return STEP_EXCEPTION;

    uint64_t pc = linear(c, CS, c->rip);
    size_t n = mem_fetch(c->mem, pc, buf, sizeof buf);
    if (n == 0) page_fault(c, pc, 0, 1);
    int r = decode(buf, (int)n, c->rip, c->bits, &in);
    if (r == DEC_TRUNC) page_fault(c, pc + n, 0, 1);
    if (r == DEC_INVALID) raise_exc(c, EXC_UD, 0, "#UD invalid opcode");

    c->next_rip = (c->rip + in.len) & ip_mask(c);
    execute(c, &in);
    c->rip = c->next_rip;
    c->icount++;
    return c->stop_request ? STEP_STOP : STEP_OK;
}
