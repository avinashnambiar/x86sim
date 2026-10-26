/* System architecture for full-system mode: segmentation with descriptor
 * tables, privilege levels, far control transfers (call gates, task
 * gates), IRET, interrupt and exception delivery (real-mode IVT, legacy
 * IDT, long-mode IDT with IST), hardware task switching, SYSCALL/SYSRET,
 * SYSENTER/SYSEXIT, control/debug registers, MSRs, the I/O permission
 * bitmap and the system-mode step loop (external interrupts, HLT, double
 * and triple faults, debug traps). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cache.h"
#include "cpu_internal.h"

#define SEL_ERR(sel) ((uint64_t)((sel) & 0xFFFC))

static inline int long_mode(CPU *c) { return (c->efer & EFER_LMA) != 0; }
static inline int pmode(CPU *c) { return (c->cr0 & CR0_PE) != 0; }
static inline int vm86(CPU *c) { return (c->rflags & FL_VM) != 0; }
static inline int iopl(CPU *c) { return (int)((c->rflags >> 12) & 3); }

static uint64_t sys_rd(CPU *c, uint64_t lin, int n) {
    uint64_t v = 0;
    lin_read(c, lin, &v, n, ACC_SUPER);
    return v;
}

static void sys_wr(CPU *c, uint64_t lin, int n, uint64_t v) { lin_write(c, lin, &v, n, ACC_SUPER); }

_Noreturn static void fault(CPU *c, int vec, uint64_t code) { raise_exc(c, vec, code, NULL); }

/* ---- state snapshots: complex transitions restore them when they fault ---- */

void sys_snapshot(CPU *c) {
    Snapshot *s = &c->snap;
    s->rsp = c->r[RSP];
    memcpy(s->sreg, c->sreg, sizeof s->sreg);
    memcpy(s->sbase, c->sbase, sizeof s->sbase);
    memcpy(s->slimit, c->slimit, sizeof s->slimit);
    memcpy(s->sattr, c->sattr, sizeof s->sattr);
    s->cpl = c->cpl;
    s->bits = c->bits;
    s->rflags = c->rflags;
    s->tr = c->tr;
    s->ldtr = c->ldtr;
    s->cr3 = c->cr3;
    s->ssp = c->ssp;
    c->snap_valid = 1;
}

static void sys_restore(CPU *c) {
    Snapshot *s = &c->snap;
    c->r[RSP] = s->rsp;
    c->ssp = s->ssp;
    memcpy(c->sreg, s->sreg, sizeof s->sreg);
    memcpy(c->sbase, s->sbase, sizeof s->sbase);
    memcpy(c->slimit, s->slimit, sizeof s->slimit);
    memcpy(c->sattr, s->sattr, sizeof s->sattr);
    c->cpl = s->cpl;
    c->bits = s->bits;
    c->rflags = s->rflags;
    c->tr = s->tr;
    c->ldtr = s->ldtr;
    if (c->cr3 != s->cr3) {
        c->cr3 = s->cr3;
        cpu_tlb_flush(c, 1);
    }
    c->snap_valid = 0;
}

/* ---- modes ---- */

void cpu_update_mode(CPU *c) {
    if (!c->sys) return;
    if (long_mode(c) && (c->sattr[CS] & SA_L)) c->bits = 64;
    else if (vm86(c) && pmode(c)) c->bits = 16;
    else c->bits = (c->sattr[CS] & SA_DB) ? 32 : 16;
}

void cpu_reset_system(CPU *c) {
    Mem *m = c->mem;
    ICache *ic = c->icache;
    struct Tx *tx = c->tx;
    tx_reset(c);
    memset(c, 0, sizeof *c);
    c->mem = m;
    c->tx = tx;
    if (!ic) ic = malloc(sizeof *ic);
    memset(ic, 0, sizeof *ic);
    c->icache = ic;
    c->sys = 1;
    c->a20_mask = ~0ULL;
    c->rflags = 2;
    c->rip = 0xFFF0;
    c->smbase = 0x30000;
    c->cr0 = CR0_ET | CR0_CD | CR0_NW;
    c->mxcsr = 0x1F80;
    c->fpu.fcw = 0x0040;
    c->xcr0 = 1;
    c->r[RDX] = 0x000906EA; /* family/model/stepping, as in CPUID.1:EAX */
    for (int i = 0; i < 6; i++) {
        c->sreg[i] = 0;
        c->sbase[i] = 0;
        c->slimit[i] = 0xFFFF;
        c->sattr[i] = SA_P | SA_S | 0x3; /* read/write data, accessed */
    }
    c->sreg[CS] = 0xF000;
    c->sbase[CS] = 0xFFFF0000;
    c->sattr[CS] = SA_P | SA_S | 0xB; /* execute/read code, accessed */
    c->gdtr.limit = c->idtr.limit = 0xFFFF;
    c->ldtr.limit = 0xFFFF;
    c->ldtr.attr = SA_P | 2;
    c->tr.limit = 0xFFFF;
    c->tr.attr = SA_P | 0xB;
    c->dr[6] = 0xFFFF0FF0;
    c->dr[7] = 0x400;
    c->msr.pat = 0x0007040600070406ULL;
    c->msr.apic_base = 0xFEE00900; /* BSP, enabled */
    c->msr.misc_enable = 1;        /* fast strings */
    c->delivering = -1;
    c->bits = 16;
    c->cpl = 0;
    cpu_tlb_flush(c, 0);
}

/* ---- descriptors ---- */

static uint64_t desc_base(uint32_t lo, uint32_t hi) {
    return (lo >> 16) | ((uint64_t)(hi & 0xFF) << 16) | (hi & 0xFF000000);
}

static uint32_t desc_limit(uint32_t lo, uint32_t hi) {
    uint32_t l = (lo & 0xFFFF) | (hi & 0xF0000);
    return (hi & (1u << 23)) ? (l << 12) | 0xFFF : l;
}

static uint16_t desc_attr(uint32_t hi) { return (uint16_t)(((hi >> 8) & 0xFF) | ((hi >> 8) & 0xF000)); }

/* Linear address of the descriptor for sel, or a fault with the selector. */
static uint64_t desc_addr(CPU *c, uint16_t sel, int vec, int ext) {
    uint64_t base;
    uint32_t limit;
    if (sel & 4) {
        if ((c->ldtr.sel & ~3) == 0 || !(c->ldtr.attr & SA_P)) fault(c, vec, SEL_ERR(sel) | ext);
        base = c->ldtr.base;
        limit = c->ldtr.limit;
    } else {
        base = c->gdtr.base;
        limit = c->gdtr.limit;
    }
    if ((uint32_t)(sel | 7) > limit) fault(c, vec, SEL_ERR(sel) | ext);
    return base + (sel & ~7);
}

static void read_desc(CPU *c, uint16_t sel, uint32_t *lo, uint32_t *hi, int vec, int ext) {
    uint64_t a = desc_addr(c, sel, vec, ext);
    uint64_t d = sys_rd(c, a, 8);
    *lo = (uint32_t)d;
    *hi = (uint32_t)(d >> 32);
}

static void set_accessed(CPU *c, uint16_t sel, uint32_t hi) {
    if (hi & 0x100) return;
    sys_wr(c, desc_addr(c, sel, EXC_GP, 0) + 4, 4, hi | 0x100);
}

static void set_seg(CPU *c, int seg, uint16_t sel, uint32_t lo, uint32_t hi) {
    c->sreg[seg] = sel;
    c->sbase[seg] = desc_base(lo, hi);
    c->slimit[seg] = desc_limit(lo, hi);
    c->sattr[seg] = desc_attr(hi) | 1;
}

static void set_null_seg(CPU *c, int seg, uint16_t sel) {
    c->sreg[seg] = sel;
    c->sattr[seg] = 0;
    if (seg != FS && seg != GS) c->sbase[seg] = 0;
    else if (long_mode(c)) c->sbase[seg] = 0; /* Intel clears the base */
    c->slimit[seg] = 0;
}

static void load_cs(CPU *c, uint16_t sel, uint32_t lo, uint32_t hi, int cpl) {
    set_seg(c, CS, (uint16_t)((sel & ~3) | cpl), lo, hi);
    c->cpl = cpl;
    cpu_update_mode(c);
}

/* Real mode and virtual-8086 mode loads. */
static void load_seg_real(CPU *c, int seg, uint16_t sel) {
    c->sreg[seg] = sel;
    c->sbase[seg] = (uint64_t)sel << 4;
    if (vm86(c)) {
        c->slimit[seg] = 0xFFFF;
        c->sattr[seg] = SA_P | SA_S | (3 << 5) | (seg == CS ? 0xB : 0x3);
    }
}

void load_seg(CPU *c, int seg, uint16_t sel) {
    if (!c->sys) {
        if (c->bits == 16) cpu_load_seg_real(c, seg, sel);
        else c->sreg[seg] = sel;
        return;
    }
    if (!pmode(c) || vm86(c)) {
        load_seg_real(c, seg, sel);
        if (seg == CS) cpu_update_mode(c);
        return;
    }
    int rpl = sel & 3;
    if ((sel & ~3) == 0) {
        if (seg == SS && !(c->bits == 64 && c->cpl != 3 && rpl == c->cpl)) fault(c, EXC_GP, 0);
        set_null_seg(c, seg, sel);
        return;
    }
    uint32_t lo, hi;
    read_desc(c, sel, &lo, &hi, EXC_GP, 0);
    int a = desc_attr(hi), dpl = SA_DPL(a), type = a & 15;
    if (!(a & SA_S)) fault(c, EXC_GP, SEL_ERR(sel));
    if (seg == SS) {
        if (rpl != c->cpl || dpl != c->cpl || (type & 8) || !(type & 2)) fault(c, EXC_GP, SEL_ERR(sel));
        if (!(a & SA_P)) fault(c, EXC_SS, SEL_ERR(sel));
    } else {
        if ((type & 8) && !(type & 2)) fault(c, EXC_GP, SEL_ERR(sel)); /* execute-only code */
        if ((!(type & 8) || !(type & 4)) && (rpl > dpl || c->cpl > dpl)) fault(c, EXC_GP, SEL_ERR(sel));
        if (!(a & SA_P)) fault(c, EXC_NP, SEL_ERR(sel));
    }
    set_accessed(c, sel, hi);
    set_seg(c, seg, sel, lo, hi);
}

/* After a return to an outer level, data segments the new CPL may not use become null. */
static void invalidate_segs(CPU *c) {
    static const int segs[] = {ES, DS, FS, GS};
    for (int i = 0; i < 4; i++) {
        int s = segs[i], a = c->sattr[s];
        if (!(a & SA_P)) continue;
        int code_conf = (a & 8) && (a & 4);
        if (!code_conf && SA_DPL(a) < c->cpl) set_null_seg(c, s, 0);
    }
}

/* Load SS for a new privilege level (stack switch); faults are #TS. */
static void load_ss_cpl(CPU *c, uint16_t sel, int cpl, int ext) {
    if ((sel & ~3) == 0) {
        if (long_mode(c) && c->bits == 64) {
            set_null_seg(c, SS, (uint16_t)((sel & ~3) | cpl));
            return;
        }
        fault(c, EXC_TS, (uint64_t)ext);
    }
    if ((sel & 3) != cpl) fault(c, EXC_TS, SEL_ERR(sel) | ext);
    uint32_t lo, hi;
    read_desc(c, sel, &lo, &hi, EXC_TS, ext);
    int a = desc_attr(hi);
    if (!(a & SA_S) || (a & 8) || !(a & 2) || SA_DPL(a) != cpl) fault(c, EXC_TS, SEL_ERR(sel) | ext);
    if (!(a & SA_P)) fault(c, EXC_SS, SEL_ERR(sel) | ext);
    set_accessed(c, sel, hi);
    set_seg(c, SS, sel, lo, hi);
}

static int stack_bytes(CPU *c) { return c->bits == 64 ? 8 : (c->sattr[SS] & SA_DB) ? 4 : 2; }

static void set_sp(CPU *c, uint64_t v) {
    int s = stack_bytes(c);
    setreg(c, RSP, s == 8 ? 8 : s, 1, v);
}

/* Push using an explicit operand size on the current stack. */
static void spush(CPU *c, uint64_t v, int size) { cpu_push(c, v, size); }

/* ---- TSS ---- */

static void tss_stack(CPU *c, int dpl, uint16_t *ss, uint64_t *sp, int ext) {
    int type = c->tr.attr & 15;
    if (type & 8) {
        uint32_t off = 4 + (uint32_t)dpl * 8;
        if (off + 5 > c->tr.limit) fault(c, EXC_TS, SEL_ERR(c->tr.sel) | ext);
        *sp = sys_rd(c, c->tr.base + off, 4);
        *ss = (uint16_t)sys_rd(c, c->tr.base + off + 4, 2);
    } else {
        uint32_t off = 2 + (uint32_t)dpl * 4;
        if (off + 3 > c->tr.limit) fault(c, EXC_TS, SEL_ERR(c->tr.sel) | ext);
        *sp = sys_rd(c, c->tr.base + off, 2);
        *ss = (uint16_t)sys_rd(c, c->tr.base + off + 2, 2);
    }
}

/* 64-bit TSS: idx 0-2 = RSP0-2, 4-10 = IST1-7. */
static uint64_t tss64_rsp(CPU *c, int idx, int ext) {
    uint32_t off = 4 + (uint32_t)idx * 8;
    if (off + 7 > c->tr.limit) fault(c, EXC_TS, SEL_ERR(c->tr.sel) | ext);
    return sys_rd(c, c->tr.base + off, 8);
}

/* I/O permission: CPL <= IOPL, or every port bit clear in the TSS bitmap. */
void sys_check_io(CPU *c, uint16_t port, int size) {
    if (!c->sys || !pmode(c)) return;
    if (!vm86(c) && c->cpl <= iopl(c)) return;
    int type = c->tr.attr & 15;
    if (!(type == 9 || type == 11) || c->tr.limit < 0x67) fault(c, EXC_GP, 0);
    uint32_t base = (uint32_t)sys_rd(c, c->tr.base + 0x66, 2);
    uint32_t off = base + port / 8;
    if (off + 1 > c->tr.limit) fault(c, EXC_GP, 0);
    uint32_t bits = (uint32_t)sys_rd(c, c->tr.base + off, 2);
    if ((bits >> (port & 7)) & ((1u << size) - 1)) fault(c, EXC_GP, 0);
}

/* ---- hardware task switch (16- and 32-bit TSS) ---- */

enum { TS_JMP, TS_CALL, TS_IRET, TS_INT };

static void set_busy(CPU *c, uint16_t sel, int busy) {
    uint64_t a = desc_addr(c, sel, EXC_GP, 0) + 4;
    uint32_t hi = (uint32_t)sys_rd(c, a, 4);
    hi = busy ? hi | 0x200 : hi & ~0x200u;
    sys_wr(c, a, 4, hi);
}

static void task_switch(CPU *c, uint16_t nsel, uint32_t nlo, uint32_t nhi, int reason, int has_err, uint32_t err,
                        uint64_t ret_rip) {
    if (c->vmx.guest) vmx_task_switch(c, nsel, reason == TS_CALL ? 0 : reason == TS_IRET ? 1 : reason == TS_JMP ? 2 : 3);
    uint64_t nb = desc_base(nlo, nhi);
    uint32_t nl = desc_limit(nlo, nhi);
    int ntype = (nhi >> 8) & 15, n32 = (ntype & 8) != 0;
    if (!(nhi & 0x8000)) fault(c, EXC_NP, SEL_ERR(nsel));
    if (nl < (n32 ? 0x67u : 0x2Bu)) fault(c, EXC_TS, SEL_ERR(nsel));
    if (reason == TS_IRET ? !(ntype & 2) : (ntype & 2)) fault(c, reason == TS_IRET ? EXC_TS : EXC_GP, SEL_ERR(nsel));

    /* save the outgoing task */
    uint64_t ob = c->tr.base;
    int o32 = (c->tr.attr & 8) != 0;
    uint32_t efl = (uint32_t)c->rflags;
    if (reason == TS_IRET) efl &= ~(uint32_t)FL_NT;
    if (o32) {
        sys_wr(c, ob + 0x20, 4, ret_rip);
        sys_wr(c, ob + 0x24, 4, efl);
        for (int i = 0; i < 8; i++) sys_wr(c, ob + 0x28 + 4 * i, 4, c->r[i]);
        for (int i = 0; i < 6; i++) sys_wr(c, ob + 0x48 + 4 * i, 2, c->sreg[i]);
    } else {
        sys_wr(c, ob + 0x0E, 2, ret_rip);
        sys_wr(c, ob + 0x10, 2, efl);
        for (int i = 0; i < 8; i++) sys_wr(c, ob + 0x12 + 2 * i, 2, c->r[i]);
        for (int i = 0; i < 4; i++) sys_wr(c, ob + 0x22 + 2 * i, 2, c->sreg[i]);
    }
    if (reason == TS_JMP || reason == TS_IRET) set_busy(c, c->tr.sel, 0);

    /* read the incoming task */
    uint32_t ncr3 = 0, neip, nefl, regs[8];
    uint16_t segs[6] = {0}, ldt;
    if (n32) {
        ncr3 = (uint32_t)sys_rd(c, nb + 0x1C, 4);
        neip = (uint32_t)sys_rd(c, nb + 0x20, 4);
        nefl = (uint32_t)sys_rd(c, nb + 0x24, 4);
        for (int i = 0; i < 8; i++) regs[i] = (uint32_t)sys_rd(c, nb + 0x28 + 4 * i, 4);
        for (int i = 0; i < 6; i++) segs[i] = (uint16_t)sys_rd(c, nb + 0x48 + 4 * i, 2);
        ldt = (uint16_t)sys_rd(c, nb + 0x60, 2);
    } else {
        neip = (uint32_t)sys_rd(c, nb + 0x0E, 2);
        nefl = (uint32_t)sys_rd(c, nb + 0x10, 2);
        for (int i = 0; i < 8; i++) regs[i] = (uint32_t)sys_rd(c, nb + 0x12 + 2 * i, 2);
        for (int i = 0; i < 4; i++) segs[i] = (uint16_t)sys_rd(c, nb + 0x22 + 2 * i, 2);
        ldt = (uint16_t)sys_rd(c, nb + 0x2A, 2);
    }
    if (reason == TS_CALL || reason == TS_INT) {
        sys_wr(c, nb, 2, c->tr.sel);
        nefl |= FL_NT;
    }
    if (reason != TS_IRET) set_busy(c, nsel, 1);

    c->tr.sel = nsel;
    c->tr.base = nb;
    c->tr.limit = nl;
    c->tr.attr = desc_attr(nhi) | 0x2;
    c->cr0 |= CR0_TS;
    if (n32 && (c->cr0 & CR0_PG) && ncr3 != c->cr3) {
        c->cr3 = ncr3;
        cpu_tlb_flush(c, 1);
    }
    for (int i = 0; i < 8; i++) c->r[i] = n32 ? regs[i] : (c->r[i] & ~0xFFFFULL) | regs[i];
    c->rflags = (n32 ? nefl : ((c->rflags & ~0xFFFFULL) | nefl)) | 2;

    /* LDT, then segments (faults here belong to the new task) */
    if ((ldt & ~3) == 0) {
        c->ldtr.sel = 0;
        c->ldtr.attr = 0;
    } else {
        if (ldt & 4) fault(c, EXC_TS, SEL_ERR(ldt));
        uint32_t lo, hi;
        read_desc(c, ldt, &lo, &hi, EXC_TS, 0);
        if (((hi >> 8) & 0x1F) != 2) fault(c, EXC_TS, SEL_ERR(ldt));
        if (!(hi & 0x8000)) fault(c, EXC_TS, SEL_ERR(ldt));
        c->ldtr.sel = ldt;
        c->ldtr.base = desc_base(lo, hi);
        c->ldtr.limit = desc_limit(lo, hi);
        c->ldtr.attr = desc_attr(hi);
    }
    if (c->rflags & FL_VM) {
        for (int i = 0; i < 6; i++) load_seg_real(c, i, segs[i]);
        c->cpl = 3;
        cpu_update_mode(c);
    } else {
        uint16_t cs = segs[CS];
        if ((cs & ~3) == 0) fault(c, EXC_TS, 0);
        uint32_t lo, hi;
        read_desc(c, cs, &lo, &hi, EXC_TS, 0);
        int a = desc_attr(hi);
        if (!(a & SA_S) || !(a & 8)) fault(c, EXC_TS, SEL_ERR(cs));
        if (!(a & SA_P)) fault(c, EXC_NP, SEL_ERR(cs));
        load_cs(c, cs, lo, hi, cs & 3);
        load_ss_cpl(c, segs[SS], c->cpl, 0);
        static const int ds[] = {ES, DS, FS, GS};
        for (int i = 0; i < 4; i++)
            if (n32 || ds[i] == ES || ds[i] == DS) load_seg(c, ds[i], segs[ds[i]]);
    }
    c->rip = c->next_rip = neip;
    if (has_err) spush(c, err, n32 ? 4 : 2);
}

/* ---- far JMP / CALL ---- */

void sys_far_transfer(CPU *c, int os, uint16_t sel, uint64_t off, int call) {
    if (!pmode(c) || vm86(c)) {
        if (call) {
            spush(c, c->sreg[CS], os);
            spush(c, c->next_rip, os);
        }
        load_seg_real(c, CS, sel);
        cpu_update_mode(c);
        c->next_rip = off & MASK(os);
        return;
    }
    sys_snapshot(c);
    if ((sel & ~3) == 0) fault(c, EXC_GP, 0);
    uint32_t lo, hi;
    read_desc(c, sel, &lo, &hi, EXC_GP, 0);
    int a = desc_attr(hi), dpl = SA_DPL(a), rpl = sel & 3, type = a & 15;

    if (a & SA_S) {
        if (!(type & 8)) fault(c, EXC_GP, SEL_ERR(sel));
        if (type & 4) {
            if (dpl > c->cpl) fault(c, EXC_GP, SEL_ERR(sel));
        } else if (rpl > c->cpl || dpl != c->cpl) {
            fault(c, EXC_GP, SEL_ERR(sel));
        }
        if (!(a & SA_P)) fault(c, EXC_NP, SEL_ERR(sel));
        if (long_mode(c) && (a & SA_L) && (a & SA_DB)) fault(c, EXC_GP, SEL_ERR(sel));
        if (!(long_mode(c) && (a & SA_L)) && off > desc_limit(lo, hi)) fault(c, EXC_GP, 0);
        if (call) {
            spush(c, c->sreg[CS], os);
            spush(c, c->next_rip, os);
        }
        set_accessed(c, sel, hi);
        load_cs(c, sel, lo, hi, c->cpl);
        c->next_rip = c->bits == 64 ? off : off & MASK(os);
        c->snap_valid = 0;
        return;
    }

    switch (type) {
    case 1: case 9: /* available TSS */
        if (long_mode(c) || dpl < c->cpl || dpl < rpl) fault(c, EXC_GP, SEL_ERR(sel));
        task_switch(c, sel, lo, hi, call ? TS_CALL : TS_JMP, 0, 0, c->next_rip);
        c->snap_valid = 0;
        return;
    case 5: { /* task gate */
        if (long_mode(c) || dpl < c->cpl || dpl < rpl) fault(c, EXC_GP, SEL_ERR(sel));
        if (!(a & SA_P)) fault(c, EXC_NP, SEL_ERR(sel));
        uint16_t tsel = (uint16_t)(lo >> 16);
        uint32_t tlo, thi;
        if (tsel & 4) fault(c, EXC_GP, SEL_ERR(tsel));
        read_desc(c, tsel, &tlo, &thi, EXC_GP, 0);
        int tt = (thi >> 8) & 0x1F;
        if (tt != 1 && tt != 9) fault(c, EXC_GP, SEL_ERR(tsel));
        task_switch(c, tsel, tlo, thi, call ? TS_CALL : TS_JMP, 0, 0, c->next_rip);
        c->snap_valid = 0;
        return;
    }
    case 4: case 12: { /* call gate */
        if (dpl < c->cpl || dpl < rpl) fault(c, EXC_GP, SEL_ERR(sel));
        if (long_mode(c) && type != 12) fault(c, EXC_GP, SEL_ERR(sel));
        if (!(a & SA_P)) fault(c, EXC_NP, SEL_ERR(sel));
        uint16_t csel = (uint16_t)(lo >> 16);
        uint64_t goff = (lo & 0xFFFF) | (hi & 0xFFFF0000);
        int pcount = (int)(hi & 31), gsize = type == 12 ? 4 : 2;
        if (long_mode(c)) {
            uint64_t up = sys_rd(c, desc_addr(c, sel, EXC_GP, 0) + 8, 8);
            if ((up >> 40) & 0x1F) fault(c, EXC_GP, SEL_ERR(sel));
            goff |= (up & 0xFFFFFFFF) << 32;
            pcount = 0;
            gsize = 8;
        } else if (type == 4) {
            goff &= 0xFFFF;
        }
        if ((csel & ~3) == 0) fault(c, EXC_GP, 0);
        uint32_t clo, chi;
        read_desc(c, csel, &clo, &chi, EXC_GP, 0);
        int ca = desc_attr(chi), cdpl = SA_DPL(ca);
        if (!(ca & SA_S) || !(ca & 8) || cdpl > c->cpl) fault(c, EXC_GP, SEL_ERR(csel));
        if (long_mode(c) && !((ca & SA_L) && !(ca & SA_DB))) fault(c, EXC_GP, SEL_ERR(csel));
        if (!(ca & SA_P)) fault(c, EXC_NP, SEL_ERR(csel));
        if (!call) {
            if (!(ca & 4) && cdpl != c->cpl) fault(c, EXC_GP, SEL_ERR(csel));
            load_cs(c, csel, clo, chi, c->cpl);
            c->next_rip = goff;
            c->snap_valid = 0;
            return;
        }
        uint16_t old_cs = c->sreg[CS], old_ss = c->sreg[SS];
        uint64_t old_sp = c->r[RSP] & MASK(stack_bytes(c)), ret = c->next_rip;
        if (!(ca & 4) && cdpl < c->cpl) {
            uint64_t params[32], old_ss_base = c->sbase[SS];
            int osz = stack_bytes(c);
            for (int i = 0; i < pcount; i++)
                params[i] = sys_rd(c, old_ss_base + ((old_sp + (uint64_t)i * gsize) & MASK(osz)), gsize);
            if (long_mode(c)) {
                uint64_t nsp = tss64_rsp(c, cdpl, 0);
                load_cs(c, csel, clo, chi, cdpl);
                set_null_seg(c, SS, (uint16_t)cdpl);
                c->r[RSP] = nsp;
            } else {
                uint16_t nss;
                uint64_t nsp;
                tss_stack(c, cdpl, &nss, &nsp, 0);
                load_ss_cpl(c, nss, cdpl, 0);
                set_sp(c, nsp);
                load_cs(c, csel, clo, chi, cdpl);
            }
            spush(c, old_ss, gsize);
            spush(c, old_sp, gsize);
            for (int i = pcount - 1; i >= 0; i--) spush(c, params[i], gsize);
        } else {
            load_cs(c, csel, clo, chi, c->cpl);
        }
        spush(c, old_cs, gsize);
        spush(c, ret, gsize);
        c->next_rip = goff;
        c->snap_valid = 0;
        return;
    }
    }
    fault(c, EXC_GP, SEL_ERR(sel));
}

/* ---- far RET ---- */

void sys_far_ret(CPU *c, int os, uint16_t imm) {
    if (!pmode(c) || vm86(c)) {
        uint64_t off = cpu_pop(c, os);
        uint16_t sel = (uint16_t)cpu_pop(c, os);
        set_sp(c, (c->r[RSP] & MASK(stack_bytes(c))) + imm);
        load_seg_real(c, CS, sel);
        cpu_update_mode(c);
        c->next_rip = off & MASK(os);
        return;
    }
    sys_snapshot(c);
    uint64_t off = cpu_pop(c, os);
    uint16_t sel = (uint16_t)cpu_pop(c, os);
    if ((sel & ~3) == 0) fault(c, EXC_GP, 0);
    uint32_t lo, hi;
    read_desc(c, sel, &lo, &hi, EXC_GP, 0);
    int a = desc_attr(hi), dpl = SA_DPL(a), rpl = sel & 3;
    if (!(a & SA_S) || !(a & 8) || rpl < c->cpl) fault(c, EXC_GP, SEL_ERR(sel));
    if ((a & 4) ? dpl > rpl : dpl != rpl) fault(c, EXC_GP, SEL_ERR(sel));
    if (!(a & SA_P)) fault(c, EXC_NP, SEL_ERR(sel));
    set_sp(c, (c->r[RSP] & MASK(stack_bytes(c))) + imm);
    if (rpl == c->cpl) {
        load_cs(c, sel, lo, hi, rpl);
    } else {
        uint64_t nsp = cpu_pop(c, os);
        uint16_t nss = (uint16_t)cpu_pop(c, os);
        load_cs(c, sel, lo, hi, rpl);
        if ((nss & ~3) == 0 && c->bits == 64 && rpl != 3) set_null_seg(c, SS, nss);
        else load_seg(c, SS, nss);
        set_sp(c, nsp + imm);
        invalidate_segs(c);
    }
    c->next_rip = c->bits == 64 ? off : off & MASK(os);
    c->snap_valid = 0;
}

/* ---- IRET ---- */

void sys_iret(CPU *c, int os) {
    if (!pmode(c)) {
        uint64_t ip = cpu_pop(c, os);
        uint16_t cs = (uint16_t)cpu_pop(c, os);
        uint64_t fl = cpu_pop(c, os);
        uint64_t mask = os == 2 ? 0x7FD5ULL : 0x257FD5ULL;
        c->rflags = (c->rflags & ~mask) | (fl & mask) | 2;
        load_seg_real(c, CS, cs);
        cpu_update_mode(c);
        c->next_rip = ip & MASK(os);
        c->nmi_blocked = 0;
        return;
    }
    if (vm86(c)) {
        if (iopl(c) < 3) fault(c, EXC_GP, 0);
        uint64_t ip = cpu_pop(c, os);
        uint16_t cs = (uint16_t)cpu_pop(c, os);
        uint64_t fl = cpu_pop(c, os);
        uint64_t mask = (os == 2 ? 0x4FD5ULL : 0x244FD5ULL) | FL_IF; /* not IOPL, VM, VIF, VIP */
        c->rflags = (c->rflags & ~mask) | (fl & mask) | 2;
        load_seg_real(c, CS, cs);
        c->next_rip = ip & MASK(os);
        c->nmi_blocked = 0;
        return;
    }
    if (c->rflags & FL_NT) {
        if (long_mode(c)) fault(c, EXC_GP, 0);
        sys_snapshot(c);
        uint16_t link = (uint16_t)sys_rd(c, c->tr.base, 2);
        if (link & 4) fault(c, EXC_TS, SEL_ERR(link));
        uint32_t lo, hi;
        read_desc(c, link, &lo, &hi, EXC_TS, 0);
        int t = (hi >> 8) & 0x1F;
        if (t != 3 && t != 11) fault(c, EXC_TS, SEL_ERR(link));
        task_switch(c, link, lo, hi, TS_IRET, 0, 0, c->next_rip);
        c->snap_valid = 0;
        c->nmi_blocked = 0;
        return;
    }
    sys_snapshot(c);
    uint64_t ip = cpu_pop(c, os);
    uint16_t cs = (uint16_t)cpu_pop(c, os);
    uint64_t fl = cpu_pop(c, os);

    if (os == 4 && (fl & FL_VM) && c->cpl == 0 && !long_mode(c)) { /* return to virtual-8086 mode */
        uint64_t sp = cpu_pop(c, 4);
        uint16_t ss = (uint16_t)cpu_pop(c, 4), es = (uint16_t)cpu_pop(c, 4), ds = (uint16_t)cpu_pop(c, 4);
        uint16_t fs = (uint16_t)cpu_pop(c, 4), gs = (uint16_t)cpu_pop(c, 4);
        c->rflags = (fl & 0x3F7FD7ULL) | 2;
        c->cpl = 3;
        load_seg_real(c, CS, cs);
        load_seg_real(c, SS, ss);
        load_seg_real(c, ES, es);
        load_seg_real(c, DS, ds);
        load_seg_real(c, FS, fs);
        load_seg_real(c, GS, gs);
        cpu_update_mode(c);
        c->r[RSP] = sp & 0xFFFFFFFF;
        c->next_rip = ip & 0xFFFF;
        c->snap_valid = 0;
        c->nmi_blocked = 0;
        return;
    }

    if ((cs & ~3) == 0) fault(c, EXC_GP, 0);
    uint32_t lo, hi;
    read_desc(c, cs, &lo, &hi, EXC_GP, 0);
    int a = desc_attr(hi), dpl = SA_DPL(a), rpl = cs & 3;
    if (!(a & SA_S) || !(a & 8) || rpl < c->cpl) fault(c, EXC_GP, SEL_ERR(cs));
    if ((a & 4) ? dpl > rpl : dpl != rpl) fault(c, EXC_GP, SEL_ERR(cs));
    if (!(a & SA_P)) fault(c, EXC_NP, SEL_ERR(cs));
    int outer = rpl > c->cpl || c->bits == 64;
    uint64_t nsp = 0;
    uint16_t nss = 0;
    if (outer) {
        nsp = cpu_pop(c, os);
        nss = (uint16_t)cpu_pop(c, os);
    }
    uint64_t mask = FL_ARITH | FL_TF | FL_DF | FL_NT | FL_RF | FL_AC | FL_ID;
    if (c->cpl == 0) mask |= FL_IOPL | FL_VIF | FL_VIP;
    if (c->cpl <= iopl(c)) mask |= FL_IF;
    if (os == 2) mask &= 0xFFFF;
    c->rflags = (c->rflags & ~mask) | (fl & mask) | 2;
    int old_cpl = c->cpl;
    load_cs(c, cs, lo, hi, rpl);
    if (outer) {
        if ((nss & ~3) == 0 && c->bits == 64 && rpl != 3) set_null_seg(c, SS, nss);
        else load_seg(c, SS, nss);
        if (c->bits == 64) c->r[RSP] = nsp;
        else set_sp(c, nsp);
    }
    if (rpl > old_cpl) invalidate_segs(c);
    c->next_rip = c->bits == 64 ? ip : ip & MASK(os);
    c->snap_valid = 0;
    c->nmi_blocked = 0;
    c->rf_keep = 1;
}

/* ---- interrupt and exception delivery ---- */

static int exc_has_error(int vec) {
    return vec == EXC_DF || (vec >= EXC_TS && vec <= EXC_PF) || vec == EXC_AC || vec == EXC_CP;
}

static void deliver_real(CPU *c, int vec, uint64_t ret) {
    if ((uint32_t)vec * 4 + 3 > c->idtr.limit) fault(c, EXC_GP, (uint64_t)vec * 8 + 2);
    uint32_t ent = (uint32_t)sys_rd(c, c->idtr.base + (uint64_t)vec * 4, 4);
    spush(c, c->rflags & 0xFFFF, 2);
    spush(c, c->sreg[CS], 2);
    spush(c, ret, 2);
    c->rflags &= ~(FL_IF | FL_TF | FL_AC | FL_RF);
    load_seg_real(c, CS, (uint16_t)(ent >> 16));
    cpu_update_mode(c);
    c->rip = c->next_rip = ent & 0xFFFF;
}

static void deliver_event(CPU *c, int vec, int kind, int has_err, uint64_t err, uint64_t ret) {
    int ext = (kind == EV_EXT || kind == EV_EXC) ? 1 : 0;
    if (!pmode(c)) {
        deliver_real(c, vec, ret);
        return;
    }
    if (vm86(c) && kind == EV_SOFT && iopl(c) < 3) fault(c, EXC_GP, 0);
    sys_snapshot(c);
    uint64_t rflags = c->rflags;
    if (kind == EV_EXC && vec != EXC_DB && vec != EXC_BP && vec != EXC_OF) rflags |= FL_RF;
    int lm = long_mode(c), gsz = lm ? 16 : 8;
    uint64_t idt_err = (uint64_t)vec * 8 + 2 + ext;
    if ((uint32_t)vec * gsz + gsz - 1 > c->idtr.limit) fault(c, EXC_GP, idt_err);
    uint64_t ga = c->idtr.base + (uint64_t)vec * gsz;
    uint64_t g = sys_rd(c, ga, 8);
    uint32_t lo = (uint32_t)g, hi = (uint32_t)(g >> 32);
    int type = (hi >> 8) & 0x1F, gdpl = (hi >> 13) & 3;
    if (lm ? !(type == 0xE || type == 0xF) : !(type == 5 || type == 6 || type == 7 || type == 0xE || type == 0xF))
        fault(c, EXC_GP, idt_err);
    if ((kind == EV_SOFT || kind == EV_SOFTEXC) && gdpl < c->cpl) fault(c, EXC_GP, idt_err);
    if (!(hi & 0x8000)) fault(c, EXC_NP, idt_err);

    if (type == 5) { /* task gate */
        uint16_t tsel = (uint16_t)(lo >> 16);
        if (tsel & 4) fault(c, EXC_TS, SEL_ERR(tsel) | ext);
        uint32_t tlo, thi;
        read_desc(c, tsel, &tlo, &thi, EXC_TS, ext);
        int tt = (thi >> 8) & 0x1F;
        if (tt != 1 && tt != 9) fault(c, EXC_TS, SEL_ERR(tsel) | ext);
        task_switch(c, tsel, tlo, thi, TS_INT, has_err, (uint32_t)err, ret);
        c->snap_valid = 0;
        return;
    }

    uint16_t csel = (uint16_t)(lo >> 16);
    uint64_t off = (lo & 0xFFFF) | (hi & 0xFFFF0000);
    int ist = 0;
    if (lm) {
        uint64_t up = sys_rd(c, ga + 8, 8);
        off |= (up & 0xFFFFFFFF) << 32;
        ist = (int)(hi & 7);
    } else if (type == 6 || type == 7) {
        off &= 0xFFFF;
    }
    if ((csel & ~3) == 0) fault(c, EXC_GP, (uint64_t)ext);
    uint32_t clo, chi;
    read_desc(c, csel, &clo, &chi, EXC_GP, ext);
    int ca = desc_attr(chi), cdpl = SA_DPL(ca);
    if (!(ca & SA_S) || !(ca & 8) || cdpl > c->cpl) fault(c, EXC_GP, SEL_ERR(csel) | ext);
    if (!(ca & SA_P)) fault(c, EXC_NP, SEL_ERR(csel) | ext);
    if (lm && !((ca & SA_L) && !(ca & SA_DB))) fault(c, EXC_GP, SEL_ERR(csel) | ext);

    int psize = lm ? 8 : (type & 8) ? 4 : 2;
    int was_vm = vm86(c);
    uint16_t old_ss = c->sreg[SS], old_cs = c->sreg[CS];
    uint64_t old_sp = c->r[RSP];
    int new_cpl = (ca & 4) ? c->cpl : cdpl;
    int inter = new_cpl < c->cpl;
    if (was_vm && (!inter || new_cpl != 0)) fault(c, EXC_GP, SEL_ERR(csel) | ext);

    if (lm) {
        uint64_t nsp = old_sp;
        if (ist) nsp = tss64_rsp(c, 3 + ist, ext);
        else if (inter) nsp = tss64_rsp(c, new_cpl, ext);
        nsp &= ~15ULL;
        load_cs(c, csel, clo, chi, new_cpl);
        if (inter) set_null_seg(c, SS, (uint16_t)new_cpl);
        c->r[RSP] = nsp;
        spush(c, old_ss, 8);
        spush(c, old_sp, 8);
        spush(c, rflags, 8);
        spush(c, old_cs, 8);
        spush(c, ret, 8);
        if (has_err) spush(c, err, 8);
    } else {
        if (inter) {
            uint16_t nss;
            uint64_t nsp;
            tss_stack(c, new_cpl, &nss, &nsp, ext);
            uint16_t vsegs[4] = {c->sreg[GS], c->sreg[FS], c->sreg[DS], c->sreg[ES]};
            load_ss_cpl(c, nss, new_cpl, ext);
            set_sp(c, nsp);
            c->rflags &= ~FL_VM;
            load_cs(c, csel, clo, chi, new_cpl);
            if (was_vm) {
                for (int i = 0; i < 4; i++) spush(c, vsegs[i], psize);
                set_null_seg(c, GS, 0);
                set_null_seg(c, FS, 0);
                set_null_seg(c, DS, 0);
                set_null_seg(c, ES, 0);
            }
            spush(c, old_ss, psize);
            spush(c, old_sp, psize);
        } else {
            load_cs(c, csel, clo, chi, new_cpl);
        }
        spush(c, rflags, psize);
        spush(c, old_cs, psize);
        spush(c, ret, psize);
        if (has_err) spush(c, err, psize);
        if (off > c->slimit[CS]) fault(c, EXC_GP, (uint64_t)ext);
    }
    c->rflags &= ~(FL_TF | FL_NT | FL_RF | FL_VM);
    if (!(type & 1)) c->rflags &= ~FL_IF;
    cpu_update_mode(c);
    c->rip = c->next_rip = off;
    c->snap_valid = 0;
}

/* Deliver an event; in VMX non-root operation a VM exit midway records it
 * as IDT-vectoring information. */
void sys_deliver(CPU *c, int vec, int kind, int has_err, uint64_t err, uint64_t ret) {
    if (c->vmx.guest) vmx_delivering(c, vec, kind, has_err, err, ret);
    deliver_event(c, vec, kind, has_err, err, ret);
    c->vmx.in_delivery = 0;
}

/* INT n / INT3 / INTO / INT1 executed by the program (sys mode). */
void sys_soft_int(CPU *c, int vec, int kind) {
    if (c->vmx.guest && kind != EV_SOFT && vmx_exc_exits(c, vec, kind == EV_ICEBP ? 5 : 6, 0, 0, 0)) vmx_raise(c);
    sys_deliver(c, vec, kind, 0, 0, c->next_rip);
}

/* ---- SYSCALL / SYSRET / SYSENTER / SYSEXIT / SWAPGS ---- */

static void flat_seg(CPU *c, int seg, uint16_t sel, int code, int dpl, int l64) {
    c->sreg[seg] = sel;
    c->sbase[seg] = 0;
    c->slimit[seg] = 0xFFFFFFFF;
    c->sattr[seg] = (uint16_t)(SA_P | SA_S | (dpl << 5) | (code ? 0xB : 0x3) | SA_G |
                               (code && l64 ? SA_L : SA_DB));
}

void sys_syscall(CPU *c) {
    if (!(c->efer & EFER_SCE)) fault(c, EXC_UD, 0);
    uint16_t sel = (uint16_t)((c->msr.star >> 32) & 0xFFFC);
    if (long_mode(c)) {
        int from64 = c->bits == 64;
        c->r[RCX] = c->next_rip;
        c->r[R11] = c->rflags & ~FL_RF;
        flat_seg(c, CS, sel, 1, 0, 1);
        flat_seg(c, SS, (uint16_t)(sel + 8), 0, 0, 0);
        c->cpl = 0;
        cpu_update_mode(c);
        c->rflags = (c->rflags & ~(c->msr.sfmask | FL_RF)) | 2;
        c->next_rip = from64 ? c->msr.lstar : c->msr.cstar;
    } else {
        c->r[RCX] = c->next_rip & 0xFFFFFFFF;
        flat_seg(c, CS, sel, 1, 0, 0);
        flat_seg(c, SS, (uint16_t)(sel + 8), 0, 0, 0);
        c->cpl = 0;
        cpu_update_mode(c);
        c->rflags &= ~(FL_IF | FL_VM | FL_RF);
        c->next_rip = c->msr.star & 0xFFFFFFFF;
    }
}

void sys_sysret(CPU *c, int rexw) {
    if (!(c->efer & EFER_SCE)) fault(c, EXC_UD, 0);
    if (c->cpl != 0 || !pmode(c)) fault(c, EXC_GP, 0);
    uint16_t base = (uint16_t)((c->msr.star >> 48) & 0xFFFF);
    if (long_mode(c)) {
        if (rexw) {
            uint64_t rip = c->r[RCX];
            if ((int64_t)(rip << 16) >> 16 != (int64_t)rip) fault(c, EXC_GP, 0);
            flat_seg(c, CS, (uint16_t)((base + 16) | 3), 1, 3, 1);
            c->next_rip = rip;
        } else {
            flat_seg(c, CS, (uint16_t)(base | 3), 1, 3, 0);
            c->next_rip = c->r[RCX] & 0xFFFFFFFF;
        }
        c->rflags = (c->r[R11] & 0x3C7FD7ULL) | 2;
    } else {
        flat_seg(c, CS, (uint16_t)(base | 3), 1, 3, 0);
        c->rflags |= FL_IF;
        c->next_rip = c->r[RCX] & 0xFFFFFFFF;
    }
    /* SS keeps its selector-derived cache; base/limit/attributes are fixed */
    flat_seg(c, SS, (uint16_t)((base + 8) | 3), 0, 3, 0);
    c->cpl = 3;
    cpu_update_mode(c);
}

void sys_sysenter(CPU *c) {
    if (!pmode(c)) fault(c, EXC_GP, 0);
    uint16_t cs = (uint16_t)(c->msr.sysenter_cs & 0xFFFC);
    if (cs == 0) fault(c, EXC_GP, 0);
    c->rflags &= ~(FL_VM | FL_IF | FL_RF);
    flat_seg(c, CS, cs, 1, 0, long_mode(c));
    flat_seg(c, SS, (uint16_t)(cs + 8), 0, 0, 0);
    c->cpl = 0;
    cpu_update_mode(c);
    c->r[RSP] = long_mode(c) ? c->msr.sysenter_esp : c->msr.sysenter_esp & 0xFFFFFFFF;
    c->next_rip = long_mode(c) ? c->msr.sysenter_eip : c->msr.sysenter_eip & 0xFFFFFFFF;
}

void sys_sysexit(CPU *c, int rexw) {
    uint16_t cs = (uint16_t)(c->msr.sysenter_cs & 0xFFFC);
    if (!pmode(c) || c->cpl != 0 || cs == 0) fault(c, EXC_GP, 0);
    if (rexw) {
        flat_seg(c, CS, (uint16_t)((cs + 32) | 3), 1, 3, 1);
        flat_seg(c, SS, (uint16_t)((cs + 40) | 3), 0, 3, 0);
        c->r[RSP] = c->r[RCX];
        c->next_rip = c->r[RDX];
    } else {
        flat_seg(c, CS, (uint16_t)((cs + 16) | 3), 1, 3, 0);
        flat_seg(c, SS, (uint16_t)((cs + 24) | 3), 0, 3, 0);
        c->r[RSP] = c->r[RCX] & 0xFFFFFFFF;
        c->next_rip = c->r[RDX] & 0xFFFFFFFF;
    }
    c->cpl = 3;
    cpu_update_mode(c);
}

/* ---- MSRs ---- */

static uint64_t read_tsc(CPU *c) {
    uint64_t t = (c->read_tsc ? c->read_tsc(c) : c->icount) + c->tsc_offset;
    return c->vmx.guest ? vmx_guest_tsc(c, t) : t;
}

uint64_t sys_rdtsc(CPU *c) {
    if (c->sys && (c->cr4 & CR4_TSD) && c->cpl > 0 && pmode(c)) fault(c, EXC_GP, 0);
    return read_tsc(c);
}

static int canonical(CPU *c, uint64_t v) {
    int sh = (c->cr4 & CR4_LA57) ? 7 : 16;
    return (uint64_t)((int64_t)(v << sh) >> sh) == v;
}

/* Client uncore PMU (global control, fixed counter, C-box counters): present
 * but with no C-boxes (0x396 reads 0) and counters that never count. */
static int uncore_msr(uint32_t idx) {
    return (idx >= 0x394 && idx <= 0x396) || (idx >= 0x700 && idx < 0x780) || (idx >= 0xE00 && idx < 0xE10);
}

int sys_rdmsr(CPU *c, uint32_t idx, uint64_t *v) {
    int err, h;
    if (c->vmx.guest && (c->vmx.proc2 & SEC_X2APIC) && (idx >> 8) == 8 && (h = vmx_x2apic_msr(c, idx, v, 0)))
        return h < 0 ? -1 : 0;
    if (c->msr_hook && (h = c->msr_hook(c, idx, v, 0))) return h < 0 ? -1 : 0;
    if (pmu_msr(c, idx, v, 0, &err) || smm_msr(c, idx, v, 0, &err) || vmx_msr(c, idx, v, 0, &err)) return err ? -1 : 0;
    switch (idx) {
    case 0x10: *v = read_tsc(c); return 0;
    case 0x1B: *v = c->msr.apic_base; return 0;
    case 0x3B: *v = c->msr.tsc_adjust; return 0;
    case 0x48: *v = c->msr.spec_ctrl; return 0;
    case 0x17: *v = 0; return 0; /* platform ID */
    case 0x8B: *v = 0xF0ULL << 32; return 0; /* microcode revision (new enough for errata checks) */
    case 0xCE: *v = 0; return 0; /* platform info */
    case 0xE7: case 0xE8: *v = read_tsc(c); return 0; /* MPERF / APERF */
    case 0xFE: *v = 0x908; return 0; /* MTRRCAP: 8 variable, fixed, WC, SMRR */
    case 0x174: *v = c->msr.sysenter_cs; return 0;
    case 0x175: *v = c->msr.sysenter_esp; return 0;
    case 0x176: *v = c->msr.sysenter_eip; return 0;
    case 0x179: *v = 0; return 0; /* MCG_CAP: no banks */
    case 0x17A: *v = 0; return 0;
    case 0x1A0: *v = c->msr.misc_enable; return 0;
    case 0x1D9: *v = 0; return 0; /* DEBUGCTL */
    case 0x277: *v = c->msr.pat; return 0;
    case 0x2FF: *v = c->msr.mtrr_def; return 0;
    case 0x6A0: *v = c->msr.u_cet; return 0;
    case 0x6A2: *v = c->msr.s_cet; return 0;
    case 0x6A4: case 0x6A5: case 0x6A6: case 0x6A7: *v = c->msr.pl_ssp[idx - 0x6A4]; return 0;
    case 0x6A8: *v = c->msr.isst; return 0;
    case 0xDA0: *v = c->msr.xss; return 0;
    case 0xC0000080: *v = c->efer; return 0;
    case 0xC0000081: *v = c->msr.star; return 0;
    case 0xC0000082: *v = c->msr.lstar; return 0;
    case 0xC0000083: *v = c->msr.cstar; return 0;
    case 0xC0000084: *v = c->msr.sfmask; return 0;
    case 0xC0000100: *v = c->sbase[FS]; return 0;
    case 0xC0000101: *v = c->sbase[GS]; return 0;
    case 0xC0000102: *v = c->msr.kernel_gs_base; return 0;
    case 0xC0000103: *v = c->msr.tsc_aux; return 0;
    }
    if (idx >= 0x200 && idx < 0x210) { *v = c->msr.mtrr[idx - 0x200]; return 0; }
    if (uncore_msr(idx)) { *v = 0; return 0; }
    if (idx == 0x250 || idx == 0x258 || idx == 0x259 || (idx >= 0x268 && idx <= 0x26F)) {
        *v = c->msr.mtrr[16 + (idx == 0x250 ? 0 : idx == 0x258 ? 1 : idx == 0x259 ? 2 : 3 + idx - 0x268)];
        return 0;
    }
    return -1;
}

int sys_wrmsr(CPU *c, uint32_t idx, uint64_t v) {
    int err, h;
    if (c->vmx.guest && (c->vmx.proc2 & SEC_X2APIC) && (idx >> 8) == 8 && (h = vmx_x2apic_msr(c, idx, &v, 1)))
        return h < 0 ? -1 : 0;
    if (c->msr_hook && (h = c->msr_hook(c, idx, &v, 1))) return h < 0 ? -1 : 0;
    if (pmu_msr(c, idx, &v, 1, &err) || smm_msr(c, idx, &v, 1, &err) || vmx_msr(c, idx, &v, 1, &err)) return err ? -1 : 0;
    switch (idx) {
    case 0x10: c->tsc_offset = v - (c->read_tsc ? c->read_tsc(c) : c->icount); return 0;
    case 0x1B: c->msr.apic_base = v; return 0;
    case 0x3B: c->tsc_offset += v - c->msr.tsc_adjust; c->msr.tsc_adjust = v; return 0;
    case 0x48: c->msr.spec_ctrl = v; return 0;
    case 0x49: return 0; /* PRED_CMD */
    case 0x8B: return 0;
    case 0x174: c->msr.sysenter_cs = v & 0xFFFF; return 0;
    case 0x175: if (!canonical(c, v)) return -1; c->msr.sysenter_esp = v; return 0;
    case 0x176: if (!canonical(c, v)) return -1; c->msr.sysenter_eip = v; return 0;
    case 0x17A: return 0;
    case 0x1A0: c->msr.misc_enable = v; return 0;
    case 0x1D9: return 0;
    case 0x277: c->msr.pat = v; return 0;
    case 0x2FF: c->msr.mtrr_def = v; return 0;
    case 0x6A0: c->msr.u_cet = v; return 0;
    case 0x6A2: c->msr.s_cet = v; return 0;
    case 0x6A4: case 0x6A5: case 0x6A6: case 0x6A7: c->msr.pl_ssp[idx - 0x6A4] = v; return 0;
    case 0x6A8: c->msr.isst = v; return 0;
    case 0xDA0: c->msr.xss = v; return 0;
    case 0xC0000080: {
        uint64_t allowed = EFER_SCE | EFER_LME | EFER_NXE;
        if (v & ~(allowed | EFER_LMA)) return -1;
        if ((c->cr0 & CR0_PG) && ((v ^ c->efer) & EFER_LME)) return -1;
        c->efer = (c->efer & EFER_LMA) | (v & allowed);
        cpu_tlb_flush(c, 0);
        return 0;
    }
    case 0xC0000081: c->msr.star = v; return 0;
    case 0xC0000082: if (!canonical(c, v)) return -1; c->msr.lstar = v; return 0;
    case 0xC0000083: if (!canonical(c, v)) return -1; c->msr.cstar = v; return 0;
    case 0xC0000084: c->msr.sfmask = v & 0xFFFFFFFF; return 0;
    case 0xC0000100: if (!canonical(c, v)) return -1; c->sbase[FS] = v; return 0;
    case 0xC0000101: if (!canonical(c, v)) return -1; c->sbase[GS] = v; return 0;
    case 0xC0000102: if (!canonical(c, v)) return -1; c->msr.kernel_gs_base = v; return 0;
    case 0xC0000103: c->msr.tsc_aux = v & 0xFFFFFFFF; return 0;
    }
    if (idx >= 0x200 && idx < 0x210) { c->msr.mtrr[idx - 0x200] = v; return 0; }
    if (uncore_msr(idx)) return 0;
    if (idx == 0x250 || idx == 0x258 || idx == 0x259 || (idx >= 0x268 && idx <= 0x26F)) {
        c->msr.mtrr[16 + (idx == 0x250 ? 0 : idx == 0x258 ? 1 : idx == 0x259 ? 2 : 3 + idx - 0x268)] = v;
        return 0;
    }
    return -1;
}

/* ---- control registers ---- */

void sys_write_cr(CPU *c, int n, uint64_t v) {
    switch (n) {
    case 0: {
        if (c->bits != 64) v &= 0xFFFFFFFF;
        if (v >> 32) fault(c, EXC_GP, 0);
        if (c->vmx.guest) v = vmx_cr_write(c, 0, v);
        if (c->vmx.on) { /* fixed bits in VMX operation; an unrestricted guest may clear PE and PG */
            uint64_t f0 = c->vmx.guest && (c->vmx.proc2 & SEC_UG) ? CR0_NE : CR0_PE | CR0_NE | CR0_PG;
            if ((v & f0) != f0) fault(c, EXC_GP, 0);
        }
        if ((v & CR0_PG) && !(v & CR0_PE)) fault(c, EXC_GP, 0);
        if ((v & CR0_NW) && !(v & CR0_CD)) fault(c, EXC_GP, 0);
        if (!(v & CR0_WP) && (c->cr4 & CR4_CET)) fault(c, EXC_GP, 0);
        uint64_t old = c->cr0;
        v |= CR0_ET;
        if ((v & CR0_PG) && !(old & CR0_PG) && (c->efer & EFER_LME)) {
            if (!(c->cr4 & CR4_PAE) || (c->sattr[CS] & SA_L)) fault(c, EXC_GP, 0);
            c->efer |= EFER_LMA;
        }
        if (!(v & CR0_PG) && (old & CR0_PG) && long_mode(c)) {
            if (c->bits == 64) fault(c, EXC_GP, 0);
            c->efer &= ~EFER_LMA;
        }
        c->cr0 = v;
        if ((old ^ v) & (CR0_PG | CR0_WP | CR0_PE)) cpu_tlb_flush(c, 0);
        cpu_update_mode(c);
        return;
    }
    case 2: c->cr2 = v; return;
    case 3:
        if (c->cr4 & CR4_PCIDE) {
            int noflush = (int)(v >> 63);
            v &= ~(1ULL << 63);
            c->cr3 = v;
            if (!noflush) tlb_flush_pcid(c, (uint16_t)(v & 0xFFF), 1);
        } else {
            c->cr3 = c->bits == 64 ? v : v & 0xFFFFFFFF;
            cpu_tlb_flush(c, 1);
        }
        return;
    case 4: {
        if (c->vmx.guest) v = vmx_cr_write(c, 4, v);
        if (v & ~CR4_SUPPORTED) fault(c, EXC_GP, 0);
        if (c->vmx.on && !(v & CR4_VMXE)) fault(c, EXC_GP, 0);
        uint64_t old = c->cr4;
        if (long_mode(c) && !(v & CR4_PAE)) fault(c, EXC_GP, 0);
        if (long_mode(c) && ((v ^ old) & CR4_LA57)) fault(c, EXC_GP, 0);
        if ((v & CR4_PCIDE) && !(old & CR4_PCIDE) && (!long_mode(c) || (c->cr3 & 0xFFF))) fault(c, EXC_GP, 0);
        if ((v & CR4_CET) && !(c->cr0 & CR0_WP)) fault(c, EXC_GP, 0);
        c->cr4 = v;
        if ((old ^ v) & (CR4_PGE | CR4_PAE | CR4_PSE | CR4_PCIDE | CR4_SMEP | CR4_LA57 | CR4_PKE)) cpu_tlb_flush(c, 0);
        return;
    }
    case 8:
        if (v & ~15ULL) fault(c, EXC_GP, 0);
        if (c->vmx.guest && (c->vmx.proc & PROC_TPR)) {
            vmx_cr8_write(c, v);
            return;
        }
        c->cr8 = v;
        if (c->tpr_hook) c->tpr_hook(c, (int)v);
        return;
    }
    fault(c, EXC_UD, 0);
}

uint64_t sys_read_cr(CPU *c, int n) {
    switch (n) {
    case 0: return c->vmx.guest ? vmx_cr_read(c, 0) : c->cr0;
    case 2: return c->cr2;
    case 3: return c->cr3;
    case 4: return c->vmx.guest ? vmx_cr_read(c, 4) : c->cr4;
    case 8: return c->vmx.guest && (c->vmx.proc & PROC_TPR) ? vmx_cr8_read(c) : c->cr8;
    }
    fault(c, EXC_UD, 0);
}

/* ---- debug registers and breakpoints ---- */

static void update_dbg(CPU *c) { c->dbg_active = (c->dr[7] & 0xFF) != 0; }

void sys_write_dr(CPU *c, int n, uint64_t v) {
    if ((n == 4 || n == 5) && (c->cr4 & CR4_DE)) fault(c, EXC_UD, 0);
    if (n == 4) n = 6;
    if (n == 5) n = 7;
    if (c->bits != 64) v &= 0xFFFFFFFF;
    if (n == 6) v = (v & 0x1E00F) | 0xFFFF0FF0;
    if (n == 7) v = (v & 0xFFFF2BFF) | 0x400;
    c->dr[n] = v;
    update_dbg(c);
}

uint64_t sys_read_dr(CPU *c, int n) {
    if ((n == 4 || n == 5) && (c->cr4 & CR4_DE)) fault(c, EXC_UD, 0);
    if (n == 4) n = 6;
    if (n == 5) n = 7;
    return c->dr[n];
}

static int bp_match(CPU *c, int i, uint64_t addr, int len, int kind) {
    uint64_t dr7 = c->dr[7];
    if (!((dr7 >> (2 * i)) & 3)) return 0;
    int rw = (int)((dr7 >> (16 + 4 * i)) & 3), ln = (int)((dr7 >> (18 + 4 * i)) & 3);
    int blen = ln == 0 ? 1 : ln == 1 ? 2 : ln == 3 ? 4 : 8;
    if (kind == 0) { if (rw != 0) return 0; }
    else if (kind == 1) { if (rw != 1 && rw != 3) return 0; }
    else if (rw != 3) return 0;
    uint64_t b = c->dr[i] & ~(uint64_t)(blen - 1);
    return addr < b + blen && b < addr + (uint64_t)len;
}

/* kind: 1 = write, 2 = read */
void sys_data_bp(CPU *c, uint64_t lin, int len, int write) {
    for (int i = 0; i < 4; i++)
        if (bp_match(c, i, lin, len, write ? 1 : 2)) c->dbg_hits |= 1u << i;
}

static int insn_bp(CPU *c, uint64_t lin) {
    int hits = 0;
    for (int i = 0; i < 4; i++)
        if (bp_match(c, i, lin, 1, 0)) hits |= 1 << i;
    return hits;
}

/* ---- the system-mode step loop ---- */

static int contributory(int v) { return v == EXC_DE || (v >= EXC_TS && v <= EXC_GP); }

StepResult sys_step(CPU *c) {
    if (_setjmp(c->jb)) {
        if (c->tx_jump) { /* a transaction aborted mid-instruction: continue at its fallback */
            c->tx_jump = 0;
            c->snap_valid = 0;
            return STEP_OK;
        }
        if (c->vmx_jump) { /* the instruction (or event delivery) caused a VM exit */
            c->vmx_jump = 0;
            if (c->snap_valid) sys_restore(c);
            vmx_exit(c);
            c->dbg_hits = 0;
            return STEP_OK;
        }
        if (c->exc_unimpl) return STEP_EXCEPTION;
        if (c->snap_valid) sys_restore(c);
        if (tx_active(c)) { /* exceptions abort transactions and are not delivered */
            tx_abort(c, c->exc_vector == EXC_DB || c->exc_vector == EXC_BP ? 0x10 : 0);
            c->dbg_hits = 0;
            return STEP_OK;
        }
        int vec = c->exc_vector;
        uint64_t code = c->exc_code;
        if (c->vmx.guest) { /* the exception bitmap: exit instead of delivering (CR2 untouched) */
            if (vmx_exc_exits(c, vec, 3, exc_has_error(vec) && pmode(c), code, vec == EXC_PF ? c->exc_addr : 0)) {
                vmx_exit(c);
                c->dbg_hits = 0;
                return STEP_OK;
            }
            if (vec == EXC_PF) c->cr2 = c->exc_addr;
        }
        if (c->delivering >= 0) {
            int first = c->delivering;
            if (first == EXC_DF && c->vmx.guest) { /* triple fault in the guest */
                c->vmx.in_delivery = 0;
                c->vmx.x_reason = 2;
                c->vmx.x_qual = c->vmx.x_intinfo = c->vmx.x_interr = c->vmx.x_info = c->vmx.x_len = 0;
                c->vmx.x_gpa = c->vmx.x_gla = 0;
                vmx_exit(c);
                return STEP_OK;
            }
            if (first == EXC_DF) {
                c->delivering = -1;
                snprintf(c->exc_msg, sizeof c->exc_msg, "triple fault (shutdown)");
                return STEP_SHUTDOWN;
            }
            if ((contributory(first) && contributory(vec)) || (first == EXC_PF && (contributory(vec) || vec == EXC_PF))) {
                vec = EXC_DF;
                code = 0;
                if (c->vmx.guest && vmx_exc_exits(c, vec, 3, 1, 0, 0)) {
                    vmx_exit(c);
                    return STEP_OK;
                }
            }
        }
        if (c->break_on_exception && c->delivering < 0) return STEP_EXCEPTION;
        c->delivering = vec;
        c->exc_count++;
        c->last_exc = vec;
        c->halted = 0;
        int ocpl = c->cpl;
        sys_deliver(c, vec, EV_EXC, exc_has_error(vec) && pmode(c), code, c->rip);
        if (c->cr4 & CR4_CET) cet_transition(c, ocpl, 0);
        c->delivering = -1;
        c->dbg_hits = 0;
        return STEP_OK;
    }

    if (c->tx && tx_doomed(c)) { /* another CPU touched our transaction's lines */
        tx_abort(c, tx_doomed(c));
        return STEP_OK;
    }
    c->vmx.in_delivery = 0;
    if (c->vmx.init_pending && !c->vmx.on && c->on_init) { /* an INIT held during VMX operation */
        c->vmx.init_pending = 0;
        c->on_init(c);
        return STEP_OK;
    }
    if (c->smi_pending && !c->smm && !c->wait_sipi && !c->vmx.guest) { /* SMI: above NMI and interrupts */
        tx_abort(c, 0);
        smm_enter(c);
        return STEP_OK;
    }
    int shadow = c->intr_shadow;
    c->intr_shadow = 0;
    int guest = c->vmx.guest;
    if (guest && vmx_boundary(c, shadow)) return STEP_OK;
    if (c->nmi_pending && !c->nmi_blocked && !shadow && !(guest && (c->vmx.pin & PIN_NMI))) {
        tx_abort(c, 0);
        c->nmi_pending = 0;
        c->nmi_blocked = 1;
        c->halted = 0;
        c->delivering = EXC_DB + 1; /* NMI: faults during delivery count as benign */
        int ocpl = c->cpl;
        sys_deliver(c, 2, EV_EXT, 0, 0, c->rip);
        if (c->cr4 & CR4_CET) cet_transition(c, ocpl, 0);
        c->delivering = -1;
        return STEP_OK;
    }
    if (c->irq_line && (c->rflags & FL_IF) && !shadow && c->get_interrupt && !(guest && (c->vmx.pin & PIN_EXTINT))) {
        int v = c->get_interrupt(c);
        if (v >= 0) {
            tx_abort(c, 0); /* interrupts abort a transaction, then arrive at its fallback */
            c->halted = 0;
            c->delivering = 0x100; /* external: benign for double-fault purposes */
            int ocpl = c->cpl;
            sys_deliver(c, v, EV_EXT, 0, 0, c->rip);
            if (c->cr4 & CR4_CET) cet_transition(c, ocpl, 0);
            c->delivering = -1;
            return STEP_OK;
        }
    }
    if (c->halted) return STEP_HALT;

    uint64_t pc = linear(c, CS, c->rip);
    if (c->dbg_active && !(c->rflags & FL_RF)) {
        int hits = insn_bp(c, pc);
        if (hits) {
            c->dr[6] = (c->dr[6] & ~0xFULL) | (uint64_t)hits;
            raise_exc(c, EXC_DB, 0, "instruction breakpoint");
        }
    }
    if (pmode(c) && c->bits != 64 && c->rip > c->slimit[CS]) fault(c, EXC_GP, 0);

    /* fetch and decode, through the decoded-instruction cache */
    ICache *ic = c->icache;
    c->vmx.insn_len = 0;
    uint64_t pa = (cpu_translated(c) ? mmu_translate(c, pc, ACC_X) : pc) & c->a20_mask;
    uint32_t pg = (uint32_t)(pa >> 12) & (IC_PAGES - 1);
    __typeof__(ic->e[0]) *e = &ic->e[(pa ^ (pa >> 13)) & (IC_SIZE - 1)];
    Insn *ip = &e->in;
    if (e->pa1 == pa + 1 && e->bits == c->bits && e->gen == ic->gen[pg]) {
        if (ip->addr != c->rip) { /* same code at another linear address */
            uint64_t m = c->bits == 64 ? ~0ULL : ip->opsize == 2 ? 0xFFFF : 0xFFFFFFFF;
            ip->target = (ip->target + (c->rip - ip->addr)) & m;
            ip->addr = c->rip;
        }
        if (c->cache) cache_access(c->cache, c->index, CA_FETCH, pa, NULL, ip->len); /* the L1I still sees the fetch */
    } else {
        uint8_t buf[15];
        size_t n;
        e->pa1 = 0;
        if (cpu_translated(c)) n = mmu_fetch(c, pc, buf, sizeof buf);
        else {
            phys_fetch(c, pc, buf, sizeof buf);
            n = sizeof buf;
        }
        int r = decode(buf, (int)n, c->rip, c->bits, ip);
        if (r == DEC_TRUNC) mmu_fetch_fault(c, pc + n);
        if (r == DEC_INVALID) raise_exc(c, EXC_UD, 0, "#UD invalid opcode");
        if ((pa & 0xFFF) + ip->len <= 4096 && !(pa >= c->mmio_lo && pa < c->mmio_hi)) {
            e->pa1 = pa + 1;
            e->bits = (uint8_t)c->bits;
            e->gen = ic->gen[pg];
            ic->code[pg] = 1;
        }
    }
    const Insn *inp = ip;
    if (c->tx && tx_active(c) && tx_forbidden(inp)) {
        tx_abort(c, 0);
        return STEP_OK;
    }
    if (c->ibt_wait) cet_check_endbr(c, inp);
    int old_cpl = c->cpl;
    c->vmx.insn_len = inp->len;

    uint64_t tf = c->rflags & FL_TF;
    c->next_rip = (c->rip + inp->len) & (c->bits == 64 ? ~0ULL : c->bits == 32 ? 0xFFFFFFFFULL : 0xFFFF);
    c->rf_keep = 0;
    c->dbg_hits = 0;
    c->snap_valid = 0;
    if (guest) vmx_intercept(c, inp);
    cpu_execute(c, inp);
    if (c->cr4 & CR4_CET) {
        switch (inp->id) {
        case I_SYSCALL: case I_SYSENTER: cet_transition(c, old_cpl, 1); break;
        case I_INT: case I_INT3: case I_INT1: case I_INTO: case I_JMPF: case I_CALLF: cet_transition(c, old_cpl, 0); break;
        case I_IRET: case I_SYSRET: case I_SYSEXIT: case I_RETF: cet_transition(c, old_cpl, 2); break;
        }
    }
    c->rip = c->next_rip;
    c->icount++;
    if (c->pmu.watch) pmu_retire(c, inp, old_cpl);
    if (!c->rf_keep) c->rflags &= ~FL_RF;

    if (guest && c->vmx.guest && c->vmx.trap_reason) { /* trap-like exits come before debug traps */
        c->dbg_hits = 0;
        vmx_trap(c);
    } else if ((tf || c->dbg_hits) && tx_active(c)) { /* debug traps abort a transaction */
        tx_abort(c, 0x10);
        c->dbg_hits = 0;
    } else if ((tf || c->dbg_hits) && guest && c->vmx.guest &&
               vmx_exc_exits(c, EXC_DB, 3, 0, 0, c->dbg_hits | (tf ? 0x4000 : 0))) {
        c->dbg_hits = 0; /* the pending debug bits go in the exit qualification, not DR6 */
        vmx_exit(c);
    } else if (tf || c->dbg_hits) {
        c->dr[6] = (c->dr[6] & ~0xFULL) | c->dbg_hits | (tf ? 0x4000 : 0);
        c->dbg_hits = 0;
        c->delivering = EXC_DB;
        sys_deliver(c, EXC_DB, EV_EXC, 0, 0, c->rip);
        c->delivering = -1;
    } else if (guest && c->vmx.guest) {
        vmx_after_insn(c);
    }
    return c->stop_request ? STEP_STOP : STEP_OK;
}

/* ---- system instructions ---- */

static void need_cpl0(CPU *c) {
    if (c->sys && pmode(c) && (c->cpl != 0 || vm86(c))) fault(c, EXC_GP, 0);
    if (!c->sys && c->cpl != 0) fault(c, EXC_GP, 0);
}

static void need_pmode(CPU *c) {
    if (!c->sys || !pmode(c) || vm86(c)) fault(c, EXC_UD, 0);
}

static void umip_check(CPU *c) {
    if (c->sys && (c->cr4 & CR4_UMIP) && c->cpl > 0 && pmode(c)) fault(c, EXC_GP, 0);
}

static void set_zf(CPU *c, int z) { c->rflags = z ? c->rflags | FL_ZF : c->rflags & ~FL_ZF; }

/* Store a selector-like value: 16 bits to memory, operand size to a register. */
static void store_sel(CPU *c, const Insn *in, uint64_t v) {
    if (in->mod == 3) setreg(c, in->rm, in->opsize, 1, v);
    else wrm(c, ea_linear(c, in), 2, v & 0xFFFF);
}

/* LAR/LSL/VERR/VERW: look the selector up without faulting. 0 when invalid. */
static int probe_desc(CPU *c, uint16_t sel, uint32_t *lo, uint32_t *hi) {
    if ((sel & ~3) == 0) return 0;
    uint64_t base;
    uint32_t limit;
    if (sel & 4) {
        if ((c->ldtr.sel & ~3) == 0) return 0;
        base = c->ldtr.base;
        limit = c->ldtr.limit;
    } else {
        base = c->gdtr.base;
        limit = c->gdtr.limit;
    }
    if ((uint32_t)(sel | 7) > limit) return 0;
    uint64_t d = sys_rd(c, base + (sel & ~7), 8);
    *lo = (uint32_t)d;
    *hi = (uint32_t)(d >> 32);
    return 1;
}

static int priv_ok(CPU *c, uint16_t sel, int a) {
    if ((a & SA_S) && (a & 8) && (a & 4)) return 1; /* conforming code */
    return SA_DPL(a) >= c->cpl && SA_DPL(a) >= (sel & 3);
}

static void load_ldtr_tr(CPU *c, uint16_t sel, int tr) {
    if ((sel & ~3) == 0) {
        if (tr) fault(c, EXC_GP, 0);
        c->ldtr.sel = sel;
        c->ldtr.attr = 0;
        c->ldtr.base = 0;
        c->ldtr.limit = 0;
        return;
    }
    if (sel & 4) fault(c, EXC_GP, SEL_ERR(sel));
    uint32_t lo, hi;
    read_desc(c, sel, &lo, &hi, EXC_GP, 0);
    int type = (hi >> 8) & 0x1F;
    if (tr ? !(type == 1 || type == 9) : type != 2) fault(c, EXC_GP, SEL_ERR(sel));
    if (long_mode(c) && type == 1) fault(c, EXC_GP, SEL_ERR(sel));
    if (!(hi & 0x8000)) fault(c, EXC_NP, SEL_ERR(sel));
    uint64_t base = desc_base(lo, hi);
    if (long_mode(c)) base |= (sys_rd(c, desc_addr(c, sel, EXC_GP, 0) + 8, 4)) << 32;
    SegDesc *d = tr ? &c->tr : &c->ldtr;
    d->sel = sel;
    d->base = base;
    d->limit = desc_limit(lo, hi);
    d->attr = desc_attr(hi);
    if (tr) {
        set_busy(c, sel, 1);
        d->attr |= 2;
    }
}

void exec_system(CPU *c, const Insn *in) {
    int os = in->opsize;
    switch (in->id) {
    case I_SGDT: case I_SIDT: {
        umip_check(c);
        DescTable *t = in->id == I_SGDT ? &c->gdtr : &c->idtr;
        uint64_t a = ea_linear(c, in);
        wrm(c, a, 2, t->limit);
        if (c->bits == 64) wrm(c, linear(c, in->seg >= 0 ? in->seg : DS, ea_offset(c, in) + 2), 8, t->base);
        else wrm(c, (a + 2) & 0xFFFFFFFF, 4, t->base & 0xFFFFFFFF);
        return;
    }
    case I_LGDT: case I_LIDT: {
        need_cpl0(c);
        if (!c->sys) fault(c, EXC_GP, 0);
        uint64_t off = ea_offset(c, in);
        int seg = in->seg >= 0 ? in->seg : (in->base == RSP || in->base == RBP) ? SS : DS;
        uint16_t limit = (uint16_t)rdm(c, linear(c, seg, off), 2);
        uint64_t base = c->bits == 64 ? rdm(c, linear(c, seg, off + 2), 8)
                                      : rdm(c, linear(c, seg, (off + 2) & MASK(in->addrsize)), 4);
        if (c->bits != 64 && os == 2) base &= 0xFFFFFF;
        DescTable *t = in->id == I_LGDT ? &c->gdtr : &c->idtr;
        t->limit = limit;
        t->base = base;
        return;
    }
    case I_SMSW:
        umip_check(c);
        if (in->mod == 3) setreg(c, in->rm, os, 1, c->sys ? sys_read_cr(c, 0) : 0x80050033);
        else wrm(c, ea_linear(c, in), 2, (c->sys ? sys_read_cr(c, 0) : 0x80050033) & 0xFFFF);
        return;
    case I_LMSW: {
        need_cpl0(c);
        if (!c->sys) fault(c, EXC_GP, 0);
        uint64_t v = in->mod == 3 ? c->r[in->rm] : rdm(c, ea_linear(c, in), 2);
        sys_write_cr(c, 0, (c->cr0 & ~0xEULL) | (v & 0xF) | (c->cr0 & CR0_PE));
        return;
    }
    case I_INVLPG:
        need_cpl0(c);
        if (!c->sys) fault(c, EXC_GP, 0);
        tlb_invlpg(c, ea_linear(c, in));
        return;
    case I_INVPCID: {
        need_cpl0(c);
        if (!c->sys) fault(c, EXC_GP, 0);
        uint64_t type = c->r[in->reg] & (c->bits == 64 ? ~0ULL : 0xFFFFFFFF);
        uint64_t a = ea_linear(c, in);
        uint64_t pcid = rdm(c, a, 8), la = rdm(c, a + 8, 8);
        if (pcid > 0xFFF || type > 3) fault(c, EXC_GP, 0);
        switch (type) {
        case 0: tlb_invlpg_pcid(c, la, (uint16_t)pcid); break;
        case 1: tlb_flush_pcid(c, (uint16_t)pcid, 1); break;
        case 2: cpu_tlb_flush(c, 0); break;
        default: cpu_tlb_flush(c, 1); break;
        }
        return;
    }
    case I_SLDT: case I_STR:
        need_pmode(c);
        umip_check(c);
        store_sel(c, in, in->id == I_SLDT ? c->ldtr.sel : c->tr.sel);
        return;
    case I_LLDT: case I_LTR: {
        need_pmode(c);
        need_cpl0(c);
        uint16_t sel = (uint16_t)(in->mod == 3 ? c->r[in->rm] : rdm(c, ea_linear(c, in), 2));
        load_ldtr_tr(c, sel, in->id == I_LTR);
        return;
    }
    case I_VERR: case I_VERW: {
        need_pmode(c);
        uint16_t sel = (uint16_t)(in->mod == 3 ? c->r[in->rm] : rdm(c, ea_linear(c, in), 2));
        uint32_t lo, hi;
        int ok = probe_desc(c, sel, &lo, &hi);
        if (ok) {
            int a = desc_attr(hi);
            if (!(a & SA_S) || !priv_ok(c, sel, a)) ok = 0;
            else if (in->id == I_VERR) ok = !(a & 8) || (a & 2);
            else ok = !(a & 8) && (a & 2);
        }
        set_zf(c, ok);
        return;
    }
    case I_LAR: case I_LSL: {
        need_pmode(c);
        uint16_t sel = (uint16_t)(in->mod == 3 ? c->r[in->rm] : rdm(c, ea_linear(c, in), 2));
        uint32_t lo, hi;
        int ok = probe_desc(c, sel, &lo, &hi);
        if (ok) {
            int a = desc_attr(hi), type = a & 15;
            if (!(a & SA_S)) {
                static const uint16_t lar_legacy = 0x1A3E, lsl_legacy = 0x0A0E; /* bit per type */
                static const uint16_t lar_long = 0x1A04, lsl_long = 0x0A04;
                uint16_t okset = in->id == I_LAR ? (long_mode(c) ? lar_long : lar_legacy)
                                                 : (long_mode(c) ? lsl_long : lsl_legacy);
                if (!((okset >> type) & 1)) ok = 0;
            }
            if (ok && !priv_ok(c, sel, a)) ok = 0;
        }
        if (ok) {
            uint64_t v = in->id == I_LAR ? (hi & 0x00FFFF00) : desc_limit(lo, hi);
            if (in->id == I_LAR && os == 2) v &= 0xFF00;
            setreg(c, in->reg, os, in->rex, v);
        }
        set_zf(c, ok);
        return;
    }
    case I_CLTS:
        need_cpl0(c);
        if (!(c->vmx.guest && (c->vmx.cr0_mask & CR0_TS))) c->cr0 &= ~CR0_TS; /* owned by the host: unchanged */
        return;
    case I_INVD: case I_WBINVD: /* INVD writes back too: dropping dirty lines would lose data */
        need_cpl0(c);
        if (c->cache) cache_flush_all(c->cache);
        return;
    case I_MOVCR: {
        need_cpl0(c);
        if (!c->sys) fault(c, EXC_GP, 0);
        int n = in->reg;
        if (!(n == 0 || n == 2 || n == 3 || n == 4 || (n == 8 && c->bits == 64))) fault(c, EXC_UD, 0);
        int sz = c->bits == 64 ? 8 : 4;
        if (in->opcode == 0x20) setreg(c, in->rm, sz, 1, sys_read_cr(c, n));
        else sys_write_cr(c, n, getreg(c, in->rm, sz, 1));
        return;
    }
    case I_MOVDR: {
        need_cpl0(c);
        if (!c->sys) fault(c, EXC_GP, 0);
        if (in->reg > 7) fault(c, EXC_UD, 0);
        int sz = c->bits == 64 ? 8 : 4;
        if (in->opcode == 0x21) setreg(c, in->rm, sz, 1, sys_read_dr(c, in->reg));
        else sys_write_dr(c, in->reg, getreg(c, in->rm, sz, 1));
        return;
    }
    case I_RDMSR: case I_WRMSR: {
        need_cpl0(c);
        if (!c->sys) fault(c, EXC_GP, 0);
        uint32_t idx = (uint32_t)c->r[RCX];
        if (in->id == I_RDMSR) {
            uint64_t v;
            if (sys_rdmsr(c, idx, &v)) fault(c, EXC_GP, 0);
            setreg(c, RAX, 4, 1, v);
            setreg(c, RDX, 4, 1, v >> 32);
        } else {
            uint64_t v = (c->r[RAX] & 0xFFFFFFFF) | (c->r[RDX] << 32);
            if (sys_wrmsr(c, idx, v)) fault(c, EXC_GP, 0);
        }
        return;
    }
    case I_RDPMC: {
        uint64_t v = 0;
        if (c->sys && !(c->cr4 & CR4_PCE) && c->cpl > 0 && pmode(c)) fault(c, EXC_GP, 0);
        if (c->sys && pmu_rdpmc(c, (uint32_t)c->r[RCX], &v)) fault(c, EXC_GP, 0);
        setreg(c, RAX, 4, 1, v);
        setreg(c, RDX, 4, 1, v >> 32);
        return;
    }
    case I_SYSRET:
        if (!c->sys) fault(c, EXC_UD, 0);
        sys_sysret(c, (in->rex & 8) != 0);
        return;
    case I_SYSEXIT:
        if (!c->sys) fault(c, EXC_UD, 0);
        sys_sysexit(c, (in->rex & 8) != 0);
        return;
    case I_RDFSBASE: case I_RDGSBASE: case I_WRFSBASE: case I_WRGSBASE: {
        if (in->rep != 0xF3 || c->bits != 64) fault(c, EXC_UD, 0);
        if (c->sys && !(c->cr4 & CR4_FSGSBASE)) fault(c, EXC_UD, 0);
        int seg = (in->id == I_RDFSBASE || in->id == I_WRFSBASE) ? FS : GS, sz = (in->rex & 8) ? 8 : 4;
        if (in->id == I_RDFSBASE || in->id == I_RDGSBASE) {
            setreg(c, in->rm, sz, 1, c->sbase[seg]);
        } else {
            uint64_t v = getreg(c, in->rm, sz, 1);
            if (!canonical(c, v)) fault(c, EXC_GP, 0);
            c->sbase[seg] = v;
        }
        return;
    }
    case I_GRP7R:
        switch (in->modrm) {
        case 0xD0: /* XGETBV */
            if (c->sys && !(c->cr4 & CR4_OSXSAVE)) fault(c, EXC_UD, 0);
            if ((uint32_t)c->r[RCX] != 0) fault(c, EXC_GP, 0);
            setreg(c, RAX, 4, 1, c->xcr0);
            setreg(c, RDX, 4, 1, c->xcr0 >> 32);
            return;
        case 0xD1: { /* XSETBV */
            if (!(c->cr4 & CR4_OSXSAVE)) fault(c, EXC_UD, 0);
            need_cpl0(c);
            uint64_t v = (c->r[RAX] & 0xFFFFFFFF) | (c->r[RDX] << 32);
            if ((uint32_t)c->r[RCX] != 0 || !(v & 1) || (v & ~3ULL)) fault(c, EXC_GP, 0);
            c->xcr0 = v;
            return;
        }
        case 0xF9: { /* RDTSCP */
            uint64_t t = sys_rdtsc(c);
            setreg(c, RAX, 4, 1, t);
            setreg(c, RDX, 4, 1, t >> 32);
            setreg(c, RCX, 4, 1, c->msr.tsc_aux);
            return;
        }
        case 0xF8: { /* SWAPGS */
            if (c->bits != 64) fault(c, EXC_UD, 0);
            need_cpl0(c);
            uint64_t t = c->sbase[GS];
            c->sbase[GS] = c->msr.kernel_gs_base;
            c->msr.kernel_gs_base = t;
            return;
        }
        case 0xC8: case 0xC9: /* MONITOR / MWAIT: no monitor hardware, MWAIT returns at once */
            need_cpl0(c);
            return;
        case 0xCA: case 0xCB: /* CLAC / STAC */
            need_cpl0(c);
            c->rflags = in->modrm == 0xCB ? c->rflags | FL_AC : c->rflags & ~FL_AC;
            return;
        case 0xEE: /* RDPKRU */
            if (!(c->cr4 & CR4_PKE)) fault(c, EXC_UD, 0);
            if ((uint32_t)c->r[RCX] != 0) fault(c, EXC_GP, 0);
            setreg(c, RAX, 4, 1, c->pkru);
            setreg(c, RDX, 4, 1, 0);
            return;
        case 0xEF: /* WRPKRU */
            if (!(c->cr4 & CR4_PKE)) fault(c, EXC_UD, 0);
            if ((uint32_t)c->r[RCX] != 0 || (uint32_t)c->r[RDX] != 0) fault(c, EXC_GP, 0);
            c->pkru = (uint32_t)c->r[RAX];
            return;
        case 0xE8: /* SERIALIZE */
            return;
        case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xD4: /* VMCALL VMLAUNCH VMRESUME VMXOFF VMFUNC */
            exec_vmx(c, in);
            return;
        case 0xD5: /* XEND */
            tx_end(c);
            return;
        case 0xD6: /* XTEST */
            tx_xtest(c);
            return;
        }
        fault(c, EXC_UD, 0);
    case I_RSM:
        smm_rsm(c);
        return;
    case I_UD0: case I_UD1:
        fault(c, EXC_UD, 0);
    case I_VMXON: case I_VMCLEAR: case I_VMPTRLD: case I_VMPTRST: case I_VMREAD: case I_VMWRITE:
    case I_INVEPT: case I_INVVPID:
        exec_vmx(c, in);
        return;
    }
    unimplemented(c, in);
}
