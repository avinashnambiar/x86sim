/* System management mode. An SMI (port 0xB2 on the PC, or an APIC/IPI
 * message with delivery mode SMI) saves the CPU state into SMRAM at
 * SMBASE + 0xFE00 and enters SMM at SMBASE + 0x8000 in "big real" mode
 * (16-bit, 4 GiB segment limits, paging and protection off, interrupts
 * and NMIs blocked). RSM restores the saved state, including a new SMBASE
 * written to the save area (relocation) and the auto-HALT restart flag.
 * An SMI arriving inside SMM is held until RSM.
 *
 * The save area uses the Intel 64 map for the architectural fields
 * (offsets from SMBASE + 0x8000):
 *   7FF8 CR0  7FF0 CR3  7FE8 RFLAGS  7FE0 EFER  7FD8 RIP  7FD0 DR6  7FC8 DR7
 *   7FC4 TR  7FC0 LDTR  7FBC GS  7FB8 FS  7FB4 DS  7FB0 SS  7FAC CS  7FA8 ES
 *   7F94 RDI 7F8C RSI 7F84 RBP 7F7C RSP 7F74 RBX 7F6C RDX 7F64 RCX 7F5C RAX
 *   7F54-7F1C R8-R15  7F02 auto-HALT restart  7EFC revision  7EF8 SMBASE
 *   7E40 CR4
 * and keeps the hidden state (segment bases/limits/attributes, GDTR, IDTR,
 * CR2, SSP) in the reserved area at 7D00-7DBF, an implementation-specific
 * layout as on real parts.
 *
 * SMRR (IA32_SMRR_PHYSBASE/PHYSMASK, accessible only in SMM) protects
 * SMRAM: outside SMM, reads of the range return all ones and writes are
 * dropped. */
#include "cpu_internal.h"
#include <string.h>

#define SMM_REVISION 0x00020064 /* Intel 64 map, SMBASE relocation */
#define SAVE(off) (c->smbase + 0x8000 + (off))
#define HIDDEN 0x7D00

static void st(CPU *c, uint32_t off, uint64_t v, int n) { phys_write(c, SAVE(off), &v, n); }
static uint64_t ld(CPU *c, uint32_t off, int n) {
    uint64_t v = 0;
    phys_read(c, SAVE(off), &v, n);
    return v;
}

static const uint32_t sel_off[6] = {0x7FA8, 0x7FAC, 0x7FB0, 0x7FB4, 0x7FB8, 0x7FBC}; /* ES CS SS DS FS GS */
static const int gpr_reg[16] = {RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, 8, 9, 10, 11, 12, 13, 14, 15};
static const uint32_t gpr_off[16] = {0x7F5C, 0x7F64, 0x7F6C, 0x7F74, 0x7F7C, 0x7F84, 0x7F8C, 0x7F94,
                                     0x7F54, 0x7F4C, 0x7F44, 0x7F3C, 0x7F34, 0x7F2C, 0x7F24, 0x7F1C};

void smm_enter(CPU *c) {
    int nmi_blocked = c->nmi_blocked;
    c->smi_pending = 0;
    c->smi_count++;
    c->smm = 1; /* first: the save area is SMRAM, which the SMRRs may protect */
    st(c, 0x7FF8, c->cr0, 8);
    st(c, 0x7FF0, c->cr3, 8);
    st(c, 0x7FE8, c->rflags, 8);
    st(c, 0x7FE0, c->efer, 8);
    st(c, 0x7FD8, c->rip, 8);
    st(c, 0x7FD0, c->dr[6], 8);
    st(c, 0x7FC8, c->dr[7], 8);
    st(c, 0x7FC4, c->tr.sel, 4);
    st(c, 0x7FC0, c->ldtr.sel, 4);
    for (int i = 0; i < 6; i++) {
        st(c, sel_off[i], c->sreg[i], 4);
        st(c, HIDDEN + i * 16, c->sattr[i], 2);
        st(c, HIDDEN + i * 16 + 4, c->slimit[i], 4);
        st(c, HIDDEN + i * 16 + 8, c->sbase[i], 8);
    }
    for (int i = 0; i < 16; i++) st(c, gpr_off[i], c->r[gpr_reg[i]], 8);
    st(c, 0x7F02, c->halted ? 1 : 0, 2);
    st(c, 0x7F00, 0, 2);
    st(c, 0x7EFC, SMM_REVISION, 4);
    st(c, 0x7EF8, c->smbase, 4);
    st(c, 0x7E40, c->cr4, 8);
    st(c, HIDDEN + 0x60, c->gdtr.base, 8);
    st(c, HIDDEN + 0x68, c->gdtr.limit, 2);
    st(c, HIDDEN + 0x70, c->idtr.base, 8);
    st(c, HIDDEN + 0x78, c->idtr.limit, 2);
    st(c, HIDDEN + 0x80, c->ldtr.base, 8);
    st(c, HIDDEN + 0x88, c->ldtr.limit, 4);
    st(c, HIDDEN + 0x8C, c->ldtr.attr, 2);
    st(c, HIDDEN + 0x90, c->tr.base, 8);
    st(c, HIDDEN + 0x98, c->tr.limit, 4);
    st(c, HIDDEN + 0x9C, c->tr.attr, 2);
    st(c, HIDDEN + 0xA0, c->ssp, 8);
    st(c, HIDDEN + 0xA8, c->cr2, 8);
    st(c, HIDDEN + 0xB0, nmi_blocked, 1);
    st(c, HIDDEN + 0xB1, c->cpl, 1);

    /* the SMM execution environment */
    c->halted = 0;
    c->nmi_blocked = 1;
    c->intr_shadow = 0;
    c->ibt_wait = 0;
    c->cr0 &= ~(CR0_PE | CR0_EM | CR0_TS | CR0_PG);
    c->cr4 = 0;
    c->efer &= ~(EFER_LME | EFER_LMA);
    c->dr[7] = 0x400;
    c->rflags = 2;
    c->rip = 0x8000;
    for (int i = 0; i < 6; i++) {
        c->sreg[i] = 0;
        c->sbase[i] = 0;
        c->slimit[i] = 0xFFFFFFFF;
        c->sattr[i] = SA_P | SA_S | 0x3; /* read/write data, accessed */
    }
    c->sreg[CS] = (uint16_t)(c->smbase >> 4);
    c->sbase[CS] = c->smbase;
    c->sattr[CS] = SA_P | SA_S | 0xB; /* execute/read code, accessed */
    c->cpl = 0;
    c->bits = 16;
    cpu_tlb_flush(c, 0);
}

void smm_rsm(CPU *c) {
    if (!c->smm) raise_exc(c, EXC_UD, 0, "#UD rsm outside SMM");
    uint64_t smbase = ld(c, 0x7EF8, 4);
    c->cr0 = ld(c, 0x7FF8, 8);
    c->cr3 = ld(c, 0x7FF0, 8);
    c->cr4 = ld(c, 0x7E40, 8);
    c->efer = ld(c, 0x7FE0, 8);
    c->rflags = ld(c, 0x7FE8, 8) | 2;
    c->rip = ld(c, 0x7FD8, 8);
    c->dr[6] = ld(c, 0x7FD0, 8);
    c->dr[7] = ld(c, 0x7FC8, 8);
    c->dbg_active = (c->dr[7] & 0xFF) != 0;
    c->tr.sel = (uint16_t)ld(c, 0x7FC4, 4);
    c->ldtr.sel = (uint16_t)ld(c, 0x7FC0, 4);
    for (int i = 0; i < 6; i++) {
        c->sreg[i] = (uint16_t)ld(c, sel_off[i], 4);
        c->sattr[i] = (uint16_t)ld(c, HIDDEN + i * 16, 2);
        c->slimit[i] = (uint32_t)ld(c, HIDDEN + i * 16 + 4, 4);
        c->sbase[i] = ld(c, HIDDEN + i * 16 + 8, 8);
    }
    for (int i = 0; i < 16; i++) c->r[gpr_reg[i]] = ld(c, gpr_off[i], 8);
    c->gdtr.base = ld(c, HIDDEN + 0x60, 8);
    c->gdtr.limit = (uint16_t)ld(c, HIDDEN + 0x68, 2);
    c->idtr.base = ld(c, HIDDEN + 0x70, 8);
    c->idtr.limit = (uint16_t)ld(c, HIDDEN + 0x78, 2);
    c->ldtr.base = ld(c, HIDDEN + 0x80, 8);
    c->ldtr.limit = (uint32_t)ld(c, HIDDEN + 0x88, 4);
    c->ldtr.attr = (uint16_t)ld(c, HIDDEN + 0x8C, 2);
    c->tr.base = ld(c, HIDDEN + 0x90, 8);
    c->tr.limit = (uint32_t)ld(c, HIDDEN + 0x98, 4);
    c->tr.attr = (uint16_t)ld(c, HIDDEN + 0x9C, 2);
    c->ssp = ld(c, HIDDEN + 0xA0, 8);
    c->cr2 = ld(c, HIDDEN + 0xA8, 8);
    c->nmi_blocked = (int)ld(c, HIDDEN + 0xB0, 1);
    c->cpl = (int)ld(c, HIDDEN + 0xB1, 1) & 3;
    c->halted = (int)(ld(c, 0x7F02, 2) & 1); /* auto-HALT restart: back to HLT */
    c->smbase = smbase;
    c->smm = 0;
    cpu_update_mode(c);
    cpu_tlb_flush(c, 0);
    c->next_rip = c->rip;
}

int smm_msr(CPU *c, uint32_t idx, uint64_t *v, int write, int *err) {
    *err = 0;
    switch (idx) {
    case 0x34: /* MSR_SMI_COUNT */
        if (write) *err = 1;
        else *v = c->smi_count;
        return 1;
    case 0x9E: /* IA32_SMBASE: read-only, in SMM */
        if (write || !c->smm) *err = 1;
        else *v = c->smbase;
        return 1;
    case 0x1F2: /* IA32_SMRR_PHYSBASE */
    case 0x1F3: /* IA32_SMRR_PHYSMASK */
        if (!c->smm) {
            *err = 1;
            return 1;
        }
        uint64_t *r = idx == 0x1F2 ? &c->smrr_base : &c->smrr_mask;
        uint64_t valid = idx == 0x1F2 ? 0xFFFFF0FFULL : 0xFFFFF800ULL;
        if (!write) *v = *r;
        else if (*v & ~valid) *err = 1;
        else *r = *v;
        return 1;
    }
    return 0;
}
