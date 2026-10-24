/* Performance monitoring, architectural perfmon version 4: three fixed
 * counters (instructions retired, core cycles, reference cycles), eight
 * general counters programmed through PERFEVTSELn, global control and
 * status, overflow interrupts through the local APIC's LVT performance entry
 * (pmi_hook) and RDPMC. Counters are 48 bits wide. This is a behavioral
 * model: one instruction takes one cycle, there is no branch predictor (CPUID
 * reports the mispredict event unavailable), and cache events come from the
 * simulated hierarchy (--cache), so without it they stay at zero. */
#include "cpu_internal.h"

#define PMU_MASK ((1ULL << 48) - 1)
#define FIXED0 PMU_GP

/* (event, umask) of PERFEVTSELn -> PE_*; Skylake encodings */
static int event_of(uint64_t sel) {
    unsigned ev = sel & 0xFF, um = (sel >> 8) & 0xFF;
    switch (ev) {
    case 0xC0: return PE_INSN;                                   /* INST_RETIRED.ANY_P */
    case 0x3C: return um == 1 ? PE_REF : PE_CYCLES;              /* CPU_CLK_UNHALTED.THREAD_P / REF_XCLK */
    case 0xC4: return um == 1 ? PE_BRANCH_COND : PE_BRANCH;      /* BR_INST_RETIRED.CONDITIONAL / ALL */
    case 0x2E: return um == 0x41 ? PE_L2_MISS : um == 0x4F ? PE_L2_REF : PE_NONE; /* LONGEST_LAT_CACHE */
    case 0x24: return um == 0x3F ? PE_L2_MISS : um == 0xFF || um == 0xEF ? PE_L2_REF : PE_NONE; /* L2_RQSTS */
    case 0x51: return um == 1 ? PE_L1D_MISS : PE_NONE;           /* L1D.REPLACEMENT */
    case 0x83: return um == 2 ? PE_L1I_MISS : PE_NONE;           /* ICACHE_64B.IFTAG_MISS */
    case 0xD2: return um == 4 ? PE_HITM : PE_NONE;               /* MEM_LOAD_L3_HIT_RETIRED.XSNP_HITM */
    }
    return PE_NONE;
}

static const uint8_t fixed_event[PMU_FIXED] = {PE_INSN, PE_CYCLES, PE_REF};

/* Recompute which counters count what after a control MSR changed. */
static void pmu_update(CPU *c) {
    Pmu *p = &c->pmu;
    p->watch = 0;
    for (int k = 0; k < PMU_GP + PMU_FIXED; k++) {
        int en, ev;
        if (k < PMU_GP) {
            uint64_t s = p->evtsel[k];
            en = (p->global_ctrl >> k & 1) && (s >> 22 & 1);
            ev = event_of(s);
            p->usr[k] = s >> 16 & 1;
            p->os[k] = s >> 17 & 1;
            p->pmi[k] = s >> 20 & 1;
        } else {
            unsigned f = (unsigned)(p->fixed_ctrl >> (4 * (k - FIXED0))) & 0xF;
            en = (p->global_ctrl >> (32 + k - FIXED0) & 1) && (f & 3);
            ev = fixed_event[k - FIXED0];
            p->os[k] = f & 1;
            p->usr[k] = f >> 1 & 1;
            p->pmi[k] = f >> 3 & 1;
        }
        p->ev[k] = (uint8_t)(en ? ev : PE_NONE);
        if (en && ev) p->watch |= 1u << ev;
    }
}

static int status_bit(int k) { return k < PMU_GP ? k : 32 + k - FIXED0; }

void pmu_count(CPU *c, int ev, uint64_t n, int cpl) {
    Pmu *p = &c->pmu;
    if (!(p->watch & (1u << ev))) return;
    for (int k = 0; k < PMU_GP + PMU_FIXED; k++) {
        if (p->ev[k] != ev || !(cpl ? p->usr[k] : p->os[k])) continue;
        uint64_t old = p->ctr[k];
        p->ctr[k] = (old + n) & PMU_MASK;
        if (p->ctr[k] < old) { /* wrapped past 2^48: overflow */
            p->global_status |= 1ULL << status_bit(k);
            if (p->pmi[k] && c->pmi_hook) c->pmi_hook(c);
        }
    }
}

static int is_branch(int id) {
    switch (id) {
    case I_JCC: case I_LOOP: case I_LOOPE: case I_LOOPNE: case I_JRCXZ: case I_JMP: case I_CALL: case I_RET:
    case I_JMPF: case I_CALLF: case I_RETF: case I_IRET: case I_SYSCALL: case I_SYSRET: case I_SYSENTER: case I_SYSEXIT:
        return 1;
    }
    return 0;
}

/* An instruction retired at privilege level cpl (c->rip already advanced). */
void pmu_retire(CPU *c, const Insn *in, int cpl) {
    Pmu *p = &c->pmu;
    pmu_count(c, PE_INSN, 1, cpl);
    pmu_count(c, PE_CYCLES, 1, cpl);
    pmu_count(c, PE_REF, 1, cpl);
    if (!(p->watch & (1u << PE_BRANCH | 1u << PE_BRANCH_COND)) || !is_branch(in->id)) return;
    pmu_count(c, PE_BRANCH, 1, cpl);
    if (in->id == I_JCC || in->id == I_LOOP || in->id == I_LOOPE || in->id == I_LOOPNE || in->id == I_JRCXZ)
        pmu_count(c, PE_BRANCH_COND, 1, cpl);
}

int pmu_rdpmc(CPU *c, uint32_t idx, uint64_t *v) {
    uint32_t n = idx & 0x3FFFFFFF;
    if (idx & (1u << 30)) {
        if (n >= PMU_FIXED) return -1;
        *v = c->pmu.ctr[FIXED0 + n];
    } else {
        if (n >= PMU_GP) return -1;
        *v = c->pmu.ctr[n];
    }
    return 0;
}

int pmu_msr(CPU *c, uint32_t idx, uint64_t *v, int write, int *err) {
    Pmu *p = &c->pmu;
    *err = 0;
    if (idx >= 0xC1 && idx < 0xC1 + PMU_GP) { /* IA32_PMCn: writes sign-extend bit 31 (no full-width writes) */
        if (write) p->ctr[idx - 0xC1] = (uint64_t)(int64_t)(int32_t)*v & PMU_MASK;
        else *v = p->ctr[idx - 0xC1];
        return 1;
    }
    if (idx >= 0x186 && idx < 0x186 + PMU_GP) { /* IA32_PERFEVTSELn */
        if (!write) {
            *v = p->evtsel[idx - 0x186];
            return 1;
        }
        if (*v >> 34) { /* bits 32-33: IN_TX / IN_TXCP */
            *err = 1;
            return 1;
        }
        p->evtsel[idx - 0x186] = *v;
        pmu_update(c);
        return 1;
    }
    if (idx >= 0x309 && idx < 0x309 + PMU_FIXED) { /* IA32_FIXED_CTRn */
        if (write) p->ctr[FIXED0 + idx - 0x309] = *v & PMU_MASK;
        else *v = p->ctr[FIXED0 + idx - 0x309];
        return 1;
    }
    if ((idx >= 0x680 && idx < 0x6A0) || (idx >= 0x6C0 && idx < 0x6E0) || (idx >= 0xDC0 && idx < 0xDE0)) {
        uint64_t *r = &p->lbr[idx >= 0xDC0 ? 2 : idx >= 0x6C0][idx & 31]; /* LBR from / to / info */
        if (write) *r = *v;
        else *v = *r;
        return 1;
    }
    switch (idx) {
    case 0x1C8: /* MSR_LBR_SELECT */
        if (write) p->lbr_select = *v & 0x3FF;
        else *v = p->lbr_select;
        return 1;
    case 0x1C9: /* MSR_LASTBRANCH_TOS */
        if (write) p->lbr_tos = *v & 31;
        else *v = p->lbr_tos;
        return 1;
    case 0x38D: /* IA32_FIXED_CTR_CTRL */
        if (!write) *v = p->fixed_ctrl;
        else if (*v & ~0xFFFULL) *err = 1;
        else p->fixed_ctrl = *v, pmu_update(c);
        return 1;
    case 0x38E: /* IA32_PERF_GLOBAL_STATUS (read-only) */
        if (write) *err = 1;
        else *v = p->global_status;
        return 1;
    case 0x38F: { /* IA32_PERF_GLOBAL_CTRL */
        uint64_t valid = ((1ULL << PMU_GP) - 1) | ((1ULL << PMU_FIXED) - 1) << 32;
        if (!write) *v = p->global_ctrl;
        else if (*v & ~valid) *err = 1;
        else p->global_ctrl = *v, pmu_update(c);
        return 1;
    }
    case 0x390: /* IA32_PERF_GLOBAL_STATUS_RESET (was GLOBAL_OVF_CTRL) */
        if (write) p->global_status &= ~*v;
        else *v = 0;
        return 1;
    case 0x391: /* IA32_PERF_GLOBAL_STATUS_SET */
        if (write) p->global_status |= *v & (((1ULL << PMU_GP) - 1) | ((1ULL << PMU_FIXED) - 1) << 32);
        else *v = 0;
        return 1;
    case 0x392: /* IA32_PERF_GLOBAL_INUSE */
        if (write) {
            *err = 1;
            return 1;
        }
        *v = 0;
        for (int k = 0; k < PMU_GP; k++)
            if (p->evtsel[k] & 0xFF) *v |= 1ULL << k;
        for (int k = 0; k < PMU_FIXED; k++)
            if (p->fixed_ctrl >> (4 * k) & 3) *v |= 1ULL << (32 + k);
        return 1;
    }
    return 0;
}
