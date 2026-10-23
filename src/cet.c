/* Control-flow Enforcement Technology (CET).
 *
 * Shadow stacks: near CALL pushes the return address on a second stack at
 * SSP, near RET pops it and raises #CP if it differs from the return address
 * on the ordinary stack. Shadow-stack pages are read-only, dirty pages (see
 * mmu.c); only shadow-stack accesses may write them. Ring transitions save
 * the user SSP in IA32_PL3_SSP. Restore/previous-SSP tokens support
 * stack switching (RSTORSSP/SAVEPREVSSP) and supervisor busy tokens
 * (SETSSBSY/CLRSSBSY).
 *
 * Indirect branch tracking: after an indirect CALL/JMP (unless NOTRACK is
 * allowed and used), SYSCALL/SYSENTER or an interrupt, the next instruction
 * must be ENDBR64/ENDBR32 or #CP(ENDBRANCH) is raised. The user tracker is
 * kept in IA32_U_CET while the CPU runs in the kernel.
 *
 * Not modelled: shadow-stack frames for far CALL/RET and interrupts (CS,
 * LIP and SSP pushed on the new shadow stack), the IST shadow-stack table,
 * the legacy code bitmap and the CET XSAVE components. */
#include "cpu_internal.h"

#define CET_SH_STK_EN 1ULL
#define CET_WR_SHSTK_EN 2ULL
#define CET_ENDBR_EN 4ULL
#define CET_NO_TRACK_EN 16ULL
#define CET_TRACKER (1ULL << 11)

enum { CP_NEAR_RET = 1, CP_FAR_RET = 2, CP_ENDBRANCH = 3, CP_RSTORSSP = 4, CP_SETSSBSY = 5 };

static uint64_t ctl(CPU *c, int cpl) { return cpl == 3 ? c->msr.u_cet : c->msr.s_cet; }
static int cet_on(CPU *c) { return c->sys && (c->cr4 & CR4_CET); }
int cet_ss_on(CPU *c) { return cet_on(c) && (ctl(c, c->cpl) & CET_SH_STK_EN); }
int cet_ibt_on(CPU *c) { return cet_on(c) && (ctl(c, c->cpl) & CET_ENDBR_EN); }
static int long64(CPU *c) { return c->bits == 64; }
static uint64_t ssp_mask(CPU *c) { return long64(c) ? ~0ULL : 0xFFFFFFFFULL; }

static uint64_t ss_read(CPU *c, uint64_t lin, int n, int acc) {
    uint64_t v = 0;
    mmu_read(c, lin, &v, n, ACC_SHSTK | acc);
    return v;
}
static void ss_write(CPU *c, uint64_t lin, uint64_t v, int n, int acc) { mmu_write(c, lin, &v, n, ACC_SHSTK | acc); }

static _Noreturn void cp(CPU *c, int code) { raise_exc(c, EXC_CP, (uint64_t)code, "#CP control protection"); }

/* ---- near CALL / RET ---- */

/* Write the return address below SSP; the caller commits the returned SSP
 * once the ordinary push has also succeeded. */
uint64_t cet_call_push(CPU *c, uint64_t ret) {
    int w = long64(c) ? 8 : 4;
    uint64_t nssp = (c->ssp - (uint64_t)w) & ssp_mask(c);
    ss_write(c, nssp, ret, w, 0);
    return nssp;
}

void cet_ret_check(CPU *c, uint64_t ret) {
    int w = long64(c) ? 8 : 4;
    uint64_t v = ss_read(c, c->ssp, w, 0);
    if (v != ret) cp(c, CP_NEAR_RET);
    c->ssp = (c->ssp + (uint64_t)w) & ssp_mask(c);
}

/* An indirect near CALL/JMP was executed. */
void cet_indirect_branch(CPU *c, const Insn *in) {
    if (!cet_ibt_on(c)) return;
    if (in->seg == DS && (ctl(c, c->cpl) & CET_NO_TRACK_EN)) return; /* NOTRACK prefix */
    c->ibt_wait = 1;
}

/* Called before executing each instruction while IBT waits for ENDBR. */
void cet_check_endbr(CPU *c, const Insn *in) {
    c->ibt_wait = 0;
    if (in->id != (long64(c) ? I_ENDBR64 : I_ENDBR32)) cp(c, CP_ENDBRANCH);
}

/* ---- privilege transitions ---- */

/* old_cpl -> c->cpl just happened. how: 0 interrupt/exception/call gate,
 * 1 SYSCALL/SYSENTER, 2 return (IRET, SYSRET, SYSEXIT, far RET). */
void cet_transition(CPU *c, int old_cpl, int how) {
    if (!cet_on(c) || old_cpl == c->cpl) {
        if (how != 2 && cet_ibt_on(c)) c->ibt_wait = 1;
        return;
    }
    if (old_cpl == 3) { /* into the kernel */
        if (c->msr.u_cet & CET_SH_STK_EN) c->msr.pl_ssp[3] = c->ssp;
        if (c->msr.u_cet & CET_ENDBR_EN) {
            c->msr.u_cet = c->ibt_wait ? c->msr.u_cet | CET_TRACKER : c->msr.u_cet & ~CET_TRACKER;
        }
        c->ibt_wait = 0;
        if (c->msr.s_cet & CET_SH_STK_EN) c->ssp = how == 1 ? 0 : c->msr.pl_ssp[c->cpl];
        if (c->msr.s_cet & CET_ENDBR_EN) c->ibt_wait = 1;
    } else if (c->cpl == 3) { /* back to user mode */
        if (c->msr.u_cet & CET_SH_STK_EN) c->ssp = c->msr.pl_ssp[3];
        c->ibt_wait = (c->msr.u_cet & CET_ENDBR_EN) && (c->msr.u_cet & CET_TRACKER);
        c->msr.u_cet &= ~CET_TRACKER;
    }
}

/* ---- instructions ---- */

static void need_ss(CPU *c) {
    if (!cet_ss_on(c)) raise_exc(c, EXC_UD, 0, NULL);
}

void exec_cet(CPU *c, const Insn *in) {
    int os = in->opsize == 8 ? 8 : 4;
    uint64_t mode = long64(c) ? 1 : 0;
    switch (in->id) {
    case I_ENDBR64: case I_ENDBR32:
        return;
    case I_RDSSP: /* a NOP unless shadow stacks are on */
        if (cet_ss_on(c)) setreg(c, in->rm, os, 1, c->ssp);
        return;
    case I_INCSSP: {
        need_ss(c);
        uint64_t n = getreg(c, in->rm, os, 1) & 0xFF;
        if (n) {
            ss_read(c, c->ssp, os, 0);
            ss_read(c, (c->ssp + (n - 1) * (uint64_t)os) & ssp_mask(c), os, 0);
        }
        c->ssp = (c->ssp + n * (uint64_t)os) & ssp_mask(c);
        return;
    }
    case I_RSTORSSP: {
        need_ss(c);
        uint64_t la = ea_linear(c, in);
        if (la & 7) raise_exc(c, EXC_GP, 0, NULL);
        uint64_t token = ss_read(c, la, 8, 0);
        int bad = (token & 3) != mode || (!mode && (token >> 32)) || (token & ~3ULL) != la + 8;
        ss_write(c, la, bad ? token : (c->ssp | mode | 2), 8, 0);
        if (bad) cp(c, CP_RSTORSSP);
        c->ssp = la;
        c->rflags &= ~(uint64_t)(FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF);
        return;
    }
    case I_SAVEPREVSSP: {
        need_ss(c);
        if (c->ssp & 7) raise_exc(c, EXC_GP, 0, NULL);
        uint64_t prev = ss_read(c, c->ssp, 8, 0);
        if (!(prev & 2) || (prev & 1) != mode) raise_exc(c, EXC_GP, 0, NULL);
        uint64_t old = prev & ~3ULL;
        ss_write(c, (old - 4) & ssp_mask(c), 0, 4, 0);
        ss_write(c, ((old & ~7ULL) - 8) & ssp_mask(c), old | mode, 8, 0);
        c->ssp = (c->ssp + 8) & ssp_mask(c);
        return;
    }
    case I_SETSSBSY: {
        if (!cet_on(c) || !(c->msr.s_cet & CET_SH_STK_EN)) raise_exc(c, EXC_UD, 0, NULL);
        if (c->cpl) raise_exc(c, EXC_GP, 0, NULL);
        uint64_t la = c->msr.pl_ssp[0];
        if (la & 7) raise_exc(c, EXC_GP, 0, NULL);
        uint64_t token = ss_read(c, la, 8, 0);
        if ((token & 1) || (token & ~7ULL) != la) cp(c, CP_SETSSBSY);
        ss_write(c, la, token | 1, 8, 0);
        c->ssp = la;
        return;
    }
    case I_CLRSSBSY: {
        if (!cet_on(c) || !(c->msr.s_cet & CET_SH_STK_EN)) raise_exc(c, EXC_UD, 0, NULL);
        if (c->cpl) raise_exc(c, EXC_GP, 0, NULL);
        uint64_t la = ea_linear(c, in);
        if (la & 7) raise_exc(c, EXC_GP, 0, NULL);
        uint64_t token = ss_read(c, la, 8, 0);
        int valid = (token & 1) && (token & ~7ULL) == la;
        if (valid) ss_write(c, la, token & ~1ULL, 8, 0);
        c->rflags = valid ? c->rflags & ~(uint64_t)FL_CF : c->rflags | FL_CF;
        c->rflags &= ~(uint64_t)(FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF);
        c->ssp = 0;
        return;
    }
    case I_WRSS:
        if (!cet_on(c) || !(ctl(c, c->cpl) & CET_WR_SHSTK_EN)) raise_exc(c, EXC_UD, 0, NULL);
        ss_write(c, ea_linear(c, in), getreg(c, in->reg, os, 1), os, 0);
        return;
    case I_WRUSS:
        if (!cet_on(c)) raise_exc(c, EXC_UD, 0, NULL);
        if (c->cpl) raise_exc(c, EXC_GP, 0, NULL);
        ss_write(c, ea_linear(c, in), getreg(c, in->reg, os, 1), os, ACC_USER);
        return;
    }
}
