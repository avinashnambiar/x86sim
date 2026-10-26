/* Helpers shared by the execution units (cpu.c, sse.c, ...). Not a public API. */
#ifndef CPU_INTERNAL_H
#define CPU_INTERNAL_H

#include "cpu.h"
#include "decode.h"

#define MASK(s) ((s) >= 8 ? ~0ULL : ((1ULL << ((s) * 8)) - 1))
#define SIGNBIT(s) (1ULL << ((s) * 8 - 1))

typedef unsigned __int128 u128;
typedef __int128 s128;

static inline uint64_t sext(uint64_t v, int s) {
    switch (s) {
    case 1: return (uint64_t)(int64_t)(int8_t)v;
    case 2: return (uint64_t)(int64_t)(int16_t)v;
    case 4: return (uint64_t)(int64_t)(int32_t)v;
    }
    return v;
}

static inline uint64_t getreg(CPU *c, int r, int s, int rex) {
    switch (s) {
    case 1:
        if (!rex && r >= 4 && r < 8) return (c->r[r - 4] >> 8) & 0xFF;
        return c->r[r] & 0xFF;
    case 2: return c->r[r] & 0xFFFF;
    case 4: return c->r[r] & 0xFFFFFFFF;
    }
    return c->r[r];
}

static inline void setreg(CPU *c, int r, int s, int rex, uint64_t v) {
    switch (s) {
    case 1:
        if (!rex && r >= 4 && r < 8) c->r[r - 4] = (c->r[r - 4] & ~0xFF00ULL) | ((v & 0xFF) << 8);
        else c->r[r] = (c->r[r] & ~0xFFULL) | (v & 0xFF);
        break;
    case 2: c->r[r] = (c->r[r] & ~0xFFFFULL) | (v & 0xFFFF); break;
    case 4: c->r[r] = v & 0xFFFFFFFF; break; /* 32-bit writes zero-extend */
    default: c->r[r] = v;
    }
}

_Noreturn void raise_exc(CPU *c, int vec, uint64_t code, const char *msg);
_Noreturn void unimplemented(CPU *c, const Insn *in);
uint64_t linear(CPU *c, int seg, uint64_t off);
uint64_t rdm(CPU *c, uint64_t a, int s);
void wrm(CPU *c, uint64_t a, int s, uint64_t v);
void rdm128(CPU *c, uint64_t a, void *dst);
void wrm128(CPU *c, uint64_t a, const void *src);
uint64_t ea_offset(CPU *c, const Insn *in);
uint64_t ea_linear(CPU *c, const Insn *in);
uint64_t rd_op(CPU *c, const Insn *in, int k);
void wr_op(CPU *c, const Insn *in, int k, uint64_t v);

/* memory access kinds (mmu.c / lin_read) */
enum { ACC_W = 1, ACC_X = 2, ACC_SUPER = 4, ACC_SHSTK = 8, ACC_NOFAULT = 16, ACC_USER = 32 };
/* event kinds for sys_deliver */
enum { EV_EXT, EV_EXC, EV_SOFT, EV_SOFTEXC, EV_ICEBP };

/* cpu.c */
void lin_read(CPU *c, uint64_t a, void *buf, int n, int acc);
void lin_write(CPU *c, uint64_t a, const void *buf, int n, int acc);
void cpu_push(CPU *c, uint64_t v, int s);
uint64_t cpu_pop(CPU *c, int s);
void cpu_execute(CPU *c, const Insn *in);

/* mmu.c */
void phys_read(CPU *c, uint64_t pa, void *buf, int n);
void phys_write(CPU *c, uint64_t pa, const void *buf, int n);
void phys_fetch(CPU *c, uint64_t pa, void *buf, int n);

/* Decoded-instruction cache (system mode). Writes to a physical page that
 * holds cached code bump its generation, which invalidates the entries. */
#define IC_SIZE 32768
#define IC_PAGES (1u << 20)
typedef struct ICache {
    struct { uint64_t pa1; uint32_t gen; uint8_t bits; Insn in; } e[IC_SIZE];
    uint32_t gen[IC_PAGES];
    uint8_t code[IC_PAGES];
} ICache;
static inline void icache_written(CPU *c, uint64_t pa, size_t n) {
    ICache *ic = c->icache;
    if (!ic) return;
    for (uint64_t p = pa >> 12; p <= (pa + n - 1) >> 12; p++) {
        uint32_t i = (uint32_t)p & (IC_PAGES - 1);
        if (ic->code[i]) ic->code[i] = 0, ic->gen[i]++;
    }
}
uint64_t mmu_translate(CPU *c, uint64_t lin, int acc);
void mmu_read(CPU *c, uint64_t lin, void *buf, int n, int acc);
void mmu_write(CPU *c, uint64_t lin, const void *buf, int n, int acc);
size_t mmu_fetch(CPU *c, uint64_t lin, uint8_t *buf, size_t n);
_Noreturn void mmu_fetch_fault(CPU *c, uint64_t lin);
void tlb_flush_pcid(CPU *c, uint16_t pcid, int keep_global);
void tlb_invlpg(CPU *c, uint64_t lin);
void tlb_invlpg_pcid(CPU *c, uint64_t lin, uint16_t pcid);

/* system.c */
void cpu_update_mode(CPU *c);
void load_seg(CPU *c, int seg, uint16_t sel);
void sys_snapshot(CPU *c);
void sys_far_transfer(CPU *c, int os, uint16_t sel, uint64_t off, int call);
void sys_far_ret(CPU *c, int os, uint16_t imm);
void sys_iret(CPU *c, int os);
void sys_deliver(CPU *c, int vec, int kind, int has_err, uint64_t err, uint64_t ret);
void sys_soft_int(CPU *c, int vec, int kind);
void sys_check_io(CPU *c, uint16_t port, int size);
void sys_syscall(CPU *c);
void sys_sysret(CPU *c, int rexw);
void sys_sysenter(CPU *c);
void sys_sysexit(CPU *c, int rexw);
uint64_t sys_rdtsc(CPU *c);
int sys_rdmsr(CPU *c, uint32_t idx, uint64_t *v);
int sys_wrmsr(CPU *c, uint32_t idx, uint64_t v);
/* pmu.c: performance-monitoring MSRs (returns 1 if idx is one: *err set on #GP),
 * events, instruction retirement and RDPMC. */
int pmu_msr(CPU *c, uint32_t idx, uint64_t *v, int write, int *err);
void pmu_count(CPU *c, int ev, uint64_t n, int cpl);
void pmu_retire(CPU *c, const Insn *in, int cpl);
int pmu_rdpmc(CPU *c, uint32_t idx, uint64_t *v);
/* tsx.c: restricted transactional memory */
int tx_active(const CPU *c);
uint32_t tx_doomed(const CPU *c); /* abort status a conflict left, or 0 */
void tx_begin(CPU *c, const Insn *in);
void tx_end(CPU *c);
void tx_xabort(CPU *c, uint8_t imm);
void tx_xtest(CPU *c);
void tx_abort(CPU *c, uint32_t status);
int tx_forbidden(const Insn *in);
void tx_read(CPU *c, uint64_t pa, uint8_t *buf, int n);
int tx_write(CPU *c, uint64_t pa, const uint8_t *buf, int n);
void tx_reset(CPU *c);
void tx_free(CPU *c);
/* smm.c: system management mode */
void smm_enter(CPU *c);
void smm_rsm(CPU *c);
int smm_msr(CPU *c, uint32_t idx, uint64_t *v, int write, int *err);
/* SMRR: outside SMM the SMRAM range reads as all ones and ignores writes */
static inline int smrr_blocked(const CPU *c, uint64_t pa) {
    return (c->smrr_mask & 0x800) && !c->smm && ((pa ^ c->smrr_base) & c->smrr_mask & 0xFFFFF000ULL) == 0;
}
/* vmx.c: VMX operation */
enum {
    PIN_EXTINT = 1u << 0, PIN_NMI = 1u << 3, PIN_VNMI = 1u << 5, PIN_PREEMPT = 1u << 6, PIN_POSTED = 1u << 7,
    PROC_INTWIN = 1u << 2, PROC_TSCOFF = 1u << 3, PROC_HLT = 1u << 7, PROC_INVLPG = 1u << 9, PROC_MWAIT = 1u << 10,
    PROC_RDPMC = 1u << 11, PROC_RDTSC = 1u << 12, PROC_CR3LOAD = 1u << 15, PROC_CR3STORE = 1u << 16,
    PROC_CR8LOAD = 1u << 19, PROC_CR8STORE = 1u << 20, PROC_TPR = 1u << 21, PROC_NMIWIN = 1u << 22, PROC_MOVDR = 1u << 23,
    PROC_UNCONDIO = 1u << 24, PROC_IOBITMAP = 1u << 25, PROC_MTF = 1u << 27, PROC_MSRBITMAP = 1u << 28,
    PROC_MONITOR = 1u << 29, PROC_PAUSE = 1u << 30, PROC_SEC = 1u << 31,
    SEC_APIC = 1u << 0, SEC_EPT = 1u << 1, SEC_DESC = 1u << 2, SEC_RDTSCP = 1u << 3, SEC_X2APIC = 1u << 4,
    SEC_VPID = 1u << 5, SEC_WBINVD = 1u << 6, SEC_UG = 1u << 7, SEC_APICREG = 1u << 8, SEC_VID = 1u << 9,
    SEC_PLE = 1u << 10, SEC_RDRAND = 1u << 11, SEC_INVPCID = 1u << 12, SEC_VMFUNC = 1u << 13,
    SEC_SHADOW = 1u << 14, SEC_RDSEED = 1u << 16, SEC_PML = 1u << 17, SEC_TSCSCALE = 1u << 25,
    EXIT_SAVE_DBG = 1u << 2, EXIT_HOST64 = 1u << 9, EXIT_LOAD_PERF = 1u << 12, EXIT_ACK = 1u << 15,
    EXIT_SAVE_PAT = 1u << 18, EXIT_LOAD_PAT = 1u << 19, EXIT_SAVE_EFER = 1u << 20, EXIT_LOAD_EFER = 1u << 21, EXIT_SAVE_TIMER = 1u << 22,
    ENTRY_LOAD_DBG = 1u << 2, ENTRY_IA32E = 1u << 9, ENTRY_LOAD_PERF = 1u << 13, ENTRY_LOAD_PAT = 1u << 14,
    ENTRY_LOAD_EFER = 1u << 15,
};
#define CR4_SUPPORTED (CR4_VME | CR4_PVI | CR4_TSD | CR4_DE | CR4_PSE | CR4_PAE | CR4_MCE | CR4_PGE | CR4_PCE | \
                       CR4_OSFXSR | CR4_OSXMMEXCPT | CR4_UMIP | CR4_LA57 | CR4_VMXE | CR4_FSGSBASE | CR4_PCIDE | \
                       CR4_OSXSAVE | CR4_SMEP | CR4_SMAP | CR4_PKE | CR4_CET)
int vmx_msr(CPU *c, uint32_t idx, uint64_t *v, int write, int *err);
void exec_vmx(CPU *c, const Insn *in);       /* VMX instructions in root operation */
void vmx_intercept(CPU *c, const Insn *in);  /* non-root: exits the instruction causes (may not return) */
_Noreturn void vmx_raise(CPU *c);            /* unwind the instruction and perform the pending exit */
_Noreturn void vmx_ept_fault(CPU *c, int misconfig, uint64_t qual, uint64_t gpa, uint64_t gla);
void vmx_exit(CPU *c);                       /* perform the pending VM exit */
int vmx_boundary(CPU *c, int shadow);        /* exits at an instruction boundary: 1 if one happened */
void vmx_after_insn(CPU *c);
int vmx_exc_exits(CPU *c, int vec, int type, int has_err, uint64_t err, uint64_t qual);
void vmx_delivering(CPU *c, int vec, int kind, int has_err, uint64_t err, uint64_t ret);
void vmx_task_switch(CPU *c, uint16_t sel, int source);
uint64_t vmx_cr_write(CPU *c, int n, uint64_t v);
uint64_t vmx_cr_read(CPU *c, int n);
uint64_t vmx_cr8_read(CPU *c);               /* CR8 with the TPR shadow */
void vmx_cr8_write(CPU *c, uint64_t v);
int vmx_trap(CPU *c);                        /* a pending trap-like exit (TPR threshold, EOI, APIC write) */
void vmx_apic_access(CPU *c, uint64_t pa, void *buf, int n, int type); /* type 0 read, 1 write, 2 fetch */
int vmx_x2apic_msr(CPU *c, uint32_t idx, uint64_t *v, int write);     /* 1 virtualized, -1 #GP, 0 not */
void vmx_pml_log(CPU *c, uint64_t gpa);
uint64_t vmx_guest_tsc(CPU *c, uint64_t tsc);
/* Is physical address pa on the APIC-access page of the running guest? */
static inline int vmx_apic_page(const CPU *c, uint64_t pa) { return ((pa & ~0xFFFULL) | 1) == c->vmx.apic_page; }
uint64_t mmu_ept_probe(CPU *c, uint64_t gpa); /* guest-physical -> host-physical, ~0 if unmapped */
/* translated: linear addresses go through mmu_translate (paging, or EPT with paging off) */
static inline int cpu_translated(const CPU *c) { return (c->cr0 & CR0_PG) || c->vmx.ept; }
void sys_write_cr(CPU *c, int n, uint64_t v);
uint64_t sys_read_cr(CPU *c, int n);
void sys_write_dr(CPU *c, int n, uint64_t v);
uint64_t sys_read_dr(CPU *c, int n);
void sys_data_bp(CPU *c, uint64_t lin, int len, int write);
StepResult sys_step(CPU *c);
void exec_system(CPU *c, const Insn *in);   /* sysinsn.c */

/* sse.c */
void exec_sse(CPU *c, const Insn *in);

/* fpu.c */
void exec_x87(CPU *c, const Insn *in);
void exec_fxsave(CPU *c, const Insn *in);  /* FXSAVE/FXRSTOR/XSAVE/XRSTOR/XSAVEOPT */
void fpu_check_pending(CPU *c);            /* #MF if an unmasked x87 exception is pending */
void mmx_enter(CPU *c);                    /* any MMX instruction: TOP = 0, all tags valid */
uint64_t mmx_get(CPU *c, int i);
void mmx_set(CPU *c, int i, uint64_t v);
void mmx_emms(CPU *c);

/* cet.c */
int cet_ss_on(CPU *c);
int cet_ibt_on(CPU *c);
uint64_t cet_call_push(CPU *c, uint64_t ret);
void cet_ret_check(CPU *c, uint64_t ret);
void cet_indirect_branch(CPU *c, const Insn *in);
void cet_check_endbr(CPU *c, const Insn *in);
void cet_transition(CPU *c, int old_cpl, int how);
void exec_cet(CPU *c, const Insn *in);

#endif
