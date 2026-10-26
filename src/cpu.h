/* Architectural CPU state and the single-instruction step interface. */
#ifndef CPU_H
#define CPU_H

#include <setjmp.h>
#include <stdint.h>

#include "mem.h"
#include "softfloat.h"

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { ES, CS, SS, DS, FS, GS };

#define FL_CF 0x0001ULL
#define FL_PF 0x0004ULL
#define FL_AF 0x0010ULL
#define FL_ZF 0x0040ULL
#define FL_SF 0x0080ULL
#define FL_TF 0x0100ULL
#define FL_IF 0x0200ULL
#define FL_DF 0x0400ULL
#define FL_OF 0x0800ULL
#define FL_IOPL 0x3000ULL
#define FL_NT 0x4000ULL
#define FL_RF 0x10000ULL
#define FL_VM 0x20000ULL
#define FL_AC 0x40000ULL
#define FL_VIF 0x80000ULL
#define FL_VIP 0x100000ULL
#define FL_ID 0x200000ULL

#define CR0_PE (1ULL << 0)
#define CR0_MP (1ULL << 1)
#define CR0_EM (1ULL << 2)
#define CR0_TS (1ULL << 3)
#define CR0_ET (1ULL << 4)
#define CR0_NE (1ULL << 5)
#define CR0_WP (1ULL << 16)
#define CR0_AM (1ULL << 18)
#define CR0_NW (1ULL << 29)
#define CR0_CD (1ULL << 30)
#define CR0_PG (1ULL << 31)

#define CR4_VME (1ULL << 0)
#define CR4_PVI (1ULL << 1)
#define CR4_TSD (1ULL << 2)
#define CR4_DE (1ULL << 3)
#define CR4_PSE (1ULL << 4)
#define CR4_PAE (1ULL << 5)
#define CR4_MCE (1ULL << 6)
#define CR4_PGE (1ULL << 7)
#define CR4_PCE (1ULL << 8)
#define CR4_OSFXSR (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_UMIP (1ULL << 11)
#define CR4_LA57 (1ULL << 12)
#define CR4_VMXE (1ULL << 13)
#define CR4_FSGSBASE (1ULL << 16)
#define CR4_PCIDE (1ULL << 17)
#define CR4_OSXSAVE (1ULL << 18)
#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)
#define CR4_PKE (1ULL << 22)
#define CR4_CET (1ULL << 23)

#define EFER_SCE (1ULL << 0)
#define EFER_LME (1ULL << 8)
#define EFER_LMA (1ULL << 10)
#define EFER_NXE (1ULL << 11)

/* Segment descriptor attributes as cached: access byte in bits 0-7
 * (type, S, DPL, P), then AVL, L, D/B, G in bits 12-15 (the layout of
 * descriptor bytes 5-6 with the limit nibble removed). */
#define SA_TYPE 0x0F
#define SA_S 0x10
#define SA_DPL(a) (((a) >> 5) & 3)
#define SA_P 0x80
#define SA_L 0x2000
#define SA_DB 0x4000
#define SA_G 0x8000
#define FL_ARITH (FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF)

enum {
    EXC_DE = 0, EXC_DB = 1, EXC_BP = 3, EXC_OF = 4, EXC_BR = 5, EXC_UD = 6,
    EXC_NM = 7, EXC_DF = 8, EXC_TS = 10, EXC_NP = 11, EXC_SS = 12, EXC_GP = 13, EXC_PF = 14,
    EXC_MF = 16, EXC_AC = 17, EXC_MC = 18, EXC_XM = 19, EXC_VE = 20, EXC_CP = 21
};

typedef struct {
    uint64_t base;
    uint32_t limit; /* in bytes, after granularity scaling */
    uint16_t sel;
    uint16_t attr;
} SegDesc;

typedef struct {
    uint64_t base;
    uint16_t limit;
} DescTable;

/* Software TLB entry: one 4 KiB linear page. */
typedef struct {
    uint64_t tag;    /* linear page number + 1 (0 = empty) */
    uint64_t phys;   /* physical page address */
    uint16_t pcid;
    uint8_t flags;   /* TLB_* below */
    uint8_t pkey;
} TlbEnt;

enum { TLB_W = 1, TLB_U = 2, TLB_NX = 4, TLB_D = 8, TLB_G = 16 };
#define TLB_SIZE 1024

typedef union {
    uint8_t b[64];
    uint16_t w[32];
    uint32_t d[16];
    uint64_t q[8];
} Vec512;

/* x87 FPU. st[] is indexed by physical register (R0-R7); ST(i) is
 * st[(TOP + i) & 7] where TOP is FSW bits 13:11. MMX register i aliases the
 * significand of physical register i. */
typedef struct {
    fx80 st[8];
    uint16_t fcw, fsw;
    uint8_t ftw;      /* abridged tag word: bit i set = R(i) holds a value */
    uint16_t fop;     /* last non-control instruction: opcode bits 10:0 */
    uint64_t fip, fdp;
    uint16_t fcs, fds;
} FPU;

/* Segment/stack state saved before multi-step transitions (far calls,
 * IRET, interrupt delivery, task switches) so a fault midway is precise. */
typedef struct {
    uint64_t rsp, rflags, cr3, ssp;
    uint16_t sreg[6];
    uint64_t sbase[6];
    uint32_t slimit[6];
    uint16_t sattr[6];
    SegDesc tr, ldtr;
    int cpl, bits;
} Snapshot;

typedef enum { STEP_OK, STEP_EXCEPTION, STEP_STOP, STEP_HALT, STEP_SHUTDOWN } StepResult;

/* Performance monitoring unit (pmu.c). Counters 0-7 are general purpose,
 * 8-10 the fixed counters (instructions, core cycles, reference cycles). */
enum { PE_NONE, PE_INSN, PE_CYCLES, PE_REF, PE_BRANCH, PE_BRANCH_COND, PE_L1D_MISS, PE_L1I_MISS, PE_L2_REF, PE_L2_MISS, PE_HITM, PE_COUNT };
#define PMU_GP 8
#define PMU_FIXED 3
typedef struct {
    uint64_t ctr[PMU_GP + PMU_FIXED];  /* 48-bit counts */
    uint64_t evtsel[PMU_GP], fixed_ctrl, global_ctrl, global_status;
    uint8_t ev[PMU_GP + PMU_FIXED];    /* PE_* counted by each enabled counter, else PE_NONE */
    uint8_t os[PMU_GP + PMU_FIXED], usr[PMU_GP + PMU_FIXED], pmi[PMU_GP + PMU_FIXED];
    uint32_t watch;                    /* 1 << PE_* for every event some counter wants */
    uint64_t lbr[3][32], lbr_tos, lbr_select; /* last-branch-record MSRs: storage only, nothing records */
} Pmu;

/* VMX operation (vmx.c). The VMCS lives in guest memory at its physical
 * address (implementation-specific layout); the controls of the running
 * guest are cached here while in VMX non-root operation. */
typedef struct {
    int on;                /* in VMX operation (after VMXON) */
    int guest;             /* VMX non-root operation */
    int ept;               /* non-root with EPT: guest-physical addresses are translated */
    int init_pending;      /* INIT latched (blocked in root operation; a VM exit in non-root) */
    uint64_t vmxon, vmcs;  /* VMXON region, current-VMCS pointer (~0 = none) */
    uint32_t pin, proc, proc2, exit_ctl, entry_ctl, exc_bitmap, pf_mask, pf_match, cr3_count;
    uint64_t cr0_mask, cr4_mask, cr0_shadow, cr4_shadow, cr3_target[4];
    uint64_t io_a, io_b, msr_bitmap, tsc_offset, eptp;
    int host_nmi_blocked;  /* real NMI blocking while the guest has virtual-NMI blocking */
    int in_delivery;       /* delivering the event in vec_info (an exit now records IDT vectoring) */
    uint32_t vec_info, vec_err, vec_len;
    /* APIC virtualization: virtual-APIC page, APIC-access page (its address
     * | 1 while a guest runs with it, else 0), posted-interrupt descriptor and
     * vector, RVI/SVI, TPR threshold, EOI-exit bitmap */
    uint64_t vapic, apic_page, pi_desc, eoi_exit[4];
    uint32_t tpr_threshold, pi_vector, rvi, svi;
    uint32_t trap_reason;  /* pending trap-like exit after the instruction (0 = none) */
    uint64_t trap_qual;
    uint64_t timer_deadline; /* VMX-preemption timer: expiry in TSC ticks (without IA32_TSC offset) */
    uint64_t ple_first, ple_last; /* pause-loop exiting: first and latest PAUSE of the loop */
    uint32_t ple_gap, ple_window;
    uint64_t pml_addr, link, vmread_bm, vmwrite_bm, vmfunc, eptp_list, tsc_mult;
    uint32_t pml_index;
    int insn_len;          /* length of the instruction being executed (exit instruction length) */
    /* the pending VM exit (raised mid-instruction, performed by the step loop) */
    uint32_t x_reason, x_intinfo, x_interr, x_len, x_info;
    uint64_t x_qual, x_gpa, x_gla;
    uint64_t exits, entries; /* statistics */
} Vmx;

typedef struct CPU CPU;
struct CPU {
    uint64_t r[16];
    uint64_t rip;
    uint64_t rflags;
    uint16_t sreg[6];
    uint64_t sbase[6];
    Vec512 zmm[32];   /* XMM/YMM/ZMM register file (only XMM used so far) */
    uint32_t mxcsr;
    FPU fpu;
    uint64_t xcr0;    /* enabled XSAVE state components */
    int bits;         /* code size: 16 (real mode), 32 or 64 */
    int cpl;          /* 0 kernel/real mode, 3 user */
    Mem *mem;
    uint64_t icount;
    int index;        /* CPU number = initial APIC ID (0: bootstrap processor) */
    int ncpu;         /* CPUs in the machine (CPUID topology) */
    int wait_sipi;    /* application processor after INIT: waiting for a STARTUP IPI */
    struct CacheSys *cache; /* cache hierarchy (--cache), NULL when not simulated */

    /* System state. In user-mode emulation (sys == 0) none of this is used:
     * memory is flat, segments are plain bases and the OS layer handles
     * system calls. With sys set the CPU behaves like bare hardware. */
    int sys;
    uint32_t slimit[6];
    uint16_t sattr[6];
    SegDesc ldtr, tr;
    DescTable gdtr, idtr;
    uint64_t cr0, cr2, cr3, cr4, cr8, efer;
    uint64_t dr[8];
    uint32_t pkru;
    struct {
        uint64_t star, lstar, cstar, sfmask, kernel_gs_base;
        uint64_t sysenter_cs, sysenter_esp, sysenter_eip;
        uint64_t tsc_aux, tsc_adjust, pat, apic_base, misc_enable, mtrr_def, mtrr[32];
        uint64_t spec_ctrl, xss, feature_control, u_cet, s_cet, pl_ssp[4], isst;
    } msr;
    uint64_t ssp;          /* CET shadow stack pointer */
    int ibt_wait;          /* CET IBT: the next instruction must be ENDBR */
    int halted;
    int intr_shadow;       /* interrupts inhibited until after the next instruction */
    int nmi_pending, nmi_blocked;
    int irq_line;          /* an external interrupt may be pending: ask get_interrupt */
    int (*get_interrupt)(CPU *c);     /* acknowledge: returns a vector, or -1 */
    uint64_t (*read_tsc)(CPU *c);
    uint64_t tsc_offset;
    int delivering;        /* exception vector being delivered, or -1 */
    int break_on_exception;/* report exceptions to the debugger instead of delivering */
    uint64_t exc_count;    /* exceptions delivered (sys mode) */
    int last_exc;
    uint64_t a20_mask;
    int dbg_active;        /* DR7 enables a breakpoint */
    unsigned dbg_hits;     /* data breakpoints hit by the current instruction */
    int rf_keep;           /* the instruction loaded RFLAGS.RF (IRET) */
    Snapshot snap;
    struct ICache *icache; /* decoded instructions by physical address (sys mode) */
    int snap_valid;
    int (*msr_hook)(CPU *c, uint32_t msr, uint64_t *val, int write); /* 1 handled, -1 #GP */
    Pmu pmu;
    struct Tx *tx;         /* RTM transaction state (tsx.c), allocated by the first XBEGIN */
    int *tx_live;          /* transactions active in the machine; NULL = no RTM (user mode) */
    CPU **peers;           /* every CPU in the machine, for transaction conflicts */
    int tx_jump;           /* the longjmp out of this instruction is a transaction abort */
    int smm, smi_pending;  /* in system management mode; an SMI is waiting (smm.c) */
    uint64_t smbase, smrr_base, smrr_mask, smi_count;
    Vmx vmx;
    int vmx_jump;          /* the longjmp out of this instruction is a VM exit */
    void (*on_init)(CPU *c); /* process an INIT latched during VMX operation (machine) */
    void (*pmi_hook)(CPU *c); /* a counter with its interrupt enabled overflowed */
    void (*tpr_hook)(CPU *c, int tpr);
    void (*eoi_hook)(CPU *c); /* EOI to the local APIC (posted-interrupt processing) */
    uint32_t cpuid_1c, cpuid_1d; /* extra CPUID.1 feature bits from the machine (APIC etc.) */
    TlbEnt tlb[TLB_SIZE];

    /* per-instruction scratch */
    uint64_t next_rip;
    jmp_buf jb;

    /* details of the last exception */
    int exc_vector;
    uint64_t exc_code;
    uint64_t exc_addr;
    int exc_unimpl;   /* valid instruction the simulator doesn't implement yet */
    char exc_msg[160];

    /* OS/firmware emulation hooks. Return 1 when handled. */
    void *user;
    int (*on_syscall)(CPU *c);
    int (*on_interrupt)(CPU *c, int vector);
    int (*on_port_in)(CPU *c, uint16_t port, int size, uint32_t *val);
    int (*on_port_out)(CPU *c, uint16_t port, int size, uint32_t val);
    /* Memory-mapped devices: physical accesses inside [mmio_lo, mmio_hi) go to
     * this hook first (aligned accesses of 1-8 bytes). Returns 1 when handled. */
    int (*on_mmio)(CPU *c, uint64_t pa, void *buf, int n, int write);
    uint64_t mmio_lo, mmio_hi;
    int stop_request; /* hooks set this to stop after the current instruction */
};

void cpu_init(CPU *c, Mem *m, int bits);
StepResult cpu_step(CPU *c);
const char *cpu_exc_name(int vector);
void cpu_load_seg_real(CPU *c, int seg, uint16_t sel);

/* Full-system mode: power-on state (real mode at F000:FFF0). */
void cpu_reset_system(CPU *c);
/* Linear -> physical without side effects (debugger). 0 on success. */
int cpu_virt_to_phys(CPU *c, uint64_t lin, uint64_t *phys);
void cpu_tlb_flush(CPU *c, int keep_global);

#endif
