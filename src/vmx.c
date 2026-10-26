/* VMX: virtual-machine extensions (Intel VT-x), enough for a hypervisor
 * such as KVM.
 *
 * - VMXON/VMXOFF, VMCLEAR, VMPTRLD, VMPTRST, VMREAD, VMWRITE, VMLAUNCH,
 *   VMRESUME, VMCALL, INVEPT, INVVPID, VMFUNC with the architectural
 *   VMsucceed / VMfailInvalid / VMfailValid results and VM-instruction
 *   error numbers. VMWRITE may write any field, also the exit information
 *   (IA32_VMX_MISC bit 29), as nested hypervisors using shadow VMCSs need.
 * - The VMCS lives in memory at its physical address. The layout is this
 *   implementation's own (as on real parts): the revision identifier and
 *   abort indicator in the first 8 bytes, every field at an offset derived
 *   from its encoding (width, type, index), the launch state at 0xFF8.
 *   Software must only use VMREAD/VMWRITE, but the debugger can look.
 * - VM entry checks the controls (VMfailValid 7), the host state
 *   (VMfailValid 8) and the guest state (VM-entry failure exit 33), loads
 *   the guest state, processes the VM-entry MSR-load list and injects the
 *   event in the VM-entry interruption-information field.
 * - VM exits save the guest state, record the exit reason, qualification,
 *   interruption and IDT-vectoring information, process the MSR-store and
 *   MSR-load lists and load the host state.
 * - Exits: exceptions (bitmap, #PF error-code mask/match), NMIs, external
 *   interrupts (with acknowledge on exit), interrupt and NMI windows,
 *   triple fault, INIT, task switches, CPUID, HLT, INVD, INVLPG, INVPCID,
 *   RDPMC, RDTSC(P), every VMX instruction, CR accesses (guest/host masks,
 *   read shadows, CR3-target list, CR8), MOV DR, I/O (unconditional or
 *   bitmaps), RDMSR/WRMSR (MSR bitmaps), MWAIT, MONITOR, PAUSE, monitor
 *   trap flag, descriptor-table accesses, WBINVD, XSETBV, RDRAND, RDSEED,
 *   EPT violations and misconfigurations.
 * - Virtual NMIs, TSC offsetting and scaling, EPT (4-level, 2M/1G pages,
 *   accessed and dirty flags, execute-only), VPID, unrestricted guests, the
 *   VMX-preemption timer, pause-loop exiting, page-modification logging,
 *   VMCS shadowing and VM functions (EPTP switching).
 * - APIC virtualization: the TPR shadow (CR8 and the TPR threshold), the
 *   APIC-access page (virtualized reads and writes, APIC-access and
 *   APIC-write exits), x2APIC-mode MSR virtualization, virtual-interrupt
 *   delivery (RVI/SVI, virtual EOIs with the EOI-exit bitmap, self-IPIs) and
 *   posted interrupts.
 *
 * Not modelled: SMM dual-monitor treatment, EPT-violation #VE, sub-page
 * permissions, mode-based execute control, IPI virtualization (tertiary
 * controls) and Intel PT interactions; the capability MSRs report them
 * absent. The TLB is flushed on every VM entry and exit, which VPIDs
 * permit. */
#include <string.h>

#include "cpu_internal.h"

#define VMCS_REVISION 1
#define LAUNCH_OFF 0xFF8
#define PHYS_BITS 39 /* CPUID.80000008H:EAX */
#define FAIL_INVALID (-1)
#define TIMER_RATE 5 /* the preemption timer counts every 2^5 TSC ticks */

/* ---- VMCS field encodings ---- */
enum {
    F_VPID = 0x0000, F_PI_VECTOR = 0x0002,
    F_G_SEL = 0x0800, /* + 2 * segment: ES CS SS DS FS GS LDTR TR */
    F_G_INTR_STATUS = 0x0810, F_PML_INDEX = 0x0812,
    F_H_SEL = 0x0C00, /* + 2 * segment: ES CS SS DS FS GS TR */
    F_IO_A = 0x2000, F_IO_B = 0x2002, F_MSR_BITMAP = 0x2004, F_EXIT_STORE = 0x2006, F_EXIT_LOAD = 0x2008,
    F_ENTRY_LOAD = 0x200A, F_PML_ADDR = 0x200E, F_TSC_OFFSET = 0x2010, F_VAPIC = 0x2012, F_APIC_ACCESS = 0x2014,
    F_PI_DESC = 0x2016, F_VMFUNC = 0x2018, F_EPTP = 0x201A, F_EOI_EXIT = 0x201C, F_EPTP_LIST = 0x2024,
    F_VMREAD_BM = 0x2026, F_VMWRITE_BM = 0x2028, F_TSC_MULT = 0x2032,
    F_GPA = 0x2400,
    F_LINK = 0x2800, F_G_PAT = 0x2804, F_G_EFER = 0x2806, F_G_PERF = 0x2808, F_G_PDPTE = 0x280A,
    F_H_PAT = 0x2C00, F_H_EFER = 0x2C02, F_H_PERF = 0x2C04,
    F_PIN = 0x4000, F_PROC = 0x4002, F_EXC_BITMAP = 0x4004, F_PF_MASK = 0x4006, F_PF_MATCH = 0x4008,
    F_CR3_COUNT = 0x400A, F_EXIT_CTL = 0x400C, F_EXIT_STORE_N = 0x400E, F_EXIT_LOAD_N = 0x4010,
    F_ENTRY_CTL = 0x4012, F_ENTRY_LOAD_N = 0x4014, F_ENTRY_INFO = 0x4016, F_ENTRY_ERR = 0x4018,
    F_ENTRY_LEN = 0x401A, F_TPR_THRESHOLD = 0x401C, F_PROC2 = 0x401E, F_PLE_GAP = 0x4020, F_PLE_WINDOW = 0x4022,
    F_INSN_ERR = 0x4400, F_REASON = 0x4402, F_X_INTINFO = 0x4404, F_X_INTERR = 0x4406, F_VEC_INFO = 0x4408,
    F_VEC_ERR = 0x440A, F_X_LEN = 0x440C, F_X_INFO = 0x440E,
    F_G_LIMIT = 0x4800, F_G_GDTR_LIMIT = 0x4810, F_G_IDTR_LIMIT = 0x4812, F_G_AR = 0x4814,
    F_G_INTR = 0x4824, F_G_ACTIVITY = 0x4826, F_G_SYSENTER_CS = 0x482A, F_G_TIMER = 0x482E,
    F_H_SYSENTER_CS = 0x4C00,
    F_CR0_MASK = 0x6000, F_CR4_MASK = 0x6002, F_CR0_SHADOW = 0x6004, F_CR4_SHADOW = 0x6006, F_CR3_TARGET = 0x6008,
    F_QUAL = 0x6400, F_GLA = 0x640A,
    F_G_CR0 = 0x6800, F_G_CR3 = 0x6802, F_G_CR4 = 0x6804, F_G_BASE = 0x6806, F_G_GDTR_BASE = 0x6816,
    F_G_IDTR_BASE = 0x6818, F_G_DR7 = 0x681A, F_G_RSP = 0x681C, F_G_RIP = 0x681E, F_G_RFLAGS = 0x6820,
    F_G_PENDING_DBG = 0x6822, F_G_SYSENTER_ESP = 0x6824, F_G_SYSENTER_EIP = 0x6826,
    F_H_CR0 = 0x6C00, F_H_CR3 = 0x6C02, F_H_CR4 = 0x6C04, F_H_FS_BASE = 0x6C06, F_H_GS_BASE = 0x6C08,
    F_H_TR_BASE = 0x6C0A, F_H_GDTR_BASE = 0x6C0C, F_H_IDTR_BASE = 0x6C0E, F_H_SYSENTER_ESP = 0x6C10,
    F_H_SYSENTER_EIP = 0x6C12, F_H_RSP = 0x6C14, F_H_RIP = 0x6C16,
};

/* Supported fields, as inclusive ranges of encodings (step 2). */
static const uint16_t field_ranges[][2] = {
    {0x0000, 0x0004}, {0x0800, 0x0812}, {0x0C00, 0x0C0C},
    {0x2000, 0x2034}, {0x2400, 0x2400}, {0x2800, 0x2818}, {0x2C00, 0x2C06},
    {0x4000, 0x4022}, {0x4400, 0x440E}, {0x4800, 0x482A}, {0x482E, 0x482E}, {0x4C00, 0x4C00},
    {0x6000, 0x600E}, {0x6400, 0x640A}, {0x6800, 0x682C}, {0x6C00, 0x6C1C},
};

/* width (encoding bits 14:13): 0 16-bit, 1 64-bit, 2 32-bit, 3 natural */
static const uint16_t width_base[4] = {0x010, 0x310, 0x110, 0x710};
static const uint8_t width_size[4] = {2, 8, 4, 8};
static uint8_t field_ok[4][4][32];

static void init_fields(void) {
    static int done;
    if (done) return;
    done = 1;
    for (size_t i = 0; i < sizeof field_ranges / sizeof field_ranges[0]; i++)
        for (unsigned e = field_ranges[i][0]; e <= field_ranges[i][1]; e += 2)
            field_ok[(e >> 13) & 3][(e >> 10) & 3][(e >> 1) & 31] = 1;
}

/* Offset of a field in the VMCS region, or -1 if unsupported. */
static int field_off(uint64_t enc) {
    init_fields();
    if (enc >> 15 || (enc & 0x1000) || ((enc >> 1) & 0x1FF) >= 32) return -1;
    int w = (int)(enc >> 13) & 3, t = (int)(enc >> 10) & 3, idx = (int)(enc >> 1) & 31;
    if ((enc & 1) && w != 1) return -1; /* "high" access exists for 64-bit fields only */
    if (!field_ok[w][t][idx]) return -1;
    return width_base[w] + (t * 32 + idx) * width_size[w];
}

/* A field of the VMCS at vmcs (the current one, or a shadow VMCS). */
static uint64_t rd_at(CPU *c, uint64_t vmcs, uint32_t enc) {
    int off = field_off(enc), w = (enc >> 13) & 3;
    uint64_t v = 0;
    phys_read(c, vmcs + (uint64_t)off, &v, width_size[w]);
    return v;
}

static void wr_at(CPU *c, uint64_t vmcs, uint32_t enc, uint64_t v) {
    int off = field_off(enc), w = (enc >> 13) & 3;
    phys_write(c, vmcs + (uint64_t)off, &v, width_size[w]);
}

static uint64_t rd(CPU *c, uint32_t enc) { return rd_at(c, c->vmx.vmcs, enc); }
static void wr(CPU *c, uint32_t enc, uint64_t v) { wr_at(c, c->vmx.vmcs, enc, v); }

static uint32_t launch_state(CPU *c, uint64_t vmcs) {
    uint32_t v = 0;
    phys_read(c, vmcs + LAUNCH_OFF, &v, 4);
    return v;
}

static void set_launch_state(CPU *c, uint64_t vmcs, uint32_t v) { phys_write(c, vmcs + LAUNCH_OFF, &v, 4); }

/* ---- capabilities ---- */

#define PIN_DEFAULT1 0x16u
#define PIN_ALLOWED (PIN_DEFAULT1 | PIN_EXTINT | PIN_NMI | PIN_VNMI | PIN_PREEMPT | PIN_POSTED)
#define PROC_DEFAULT1 0x0401E172u
#define PROC_TRUE_DEFAULT1 0x04006172u /* CR3-load/store exiting may be 0 */
#define PROC_ALLOWED (PROC_DEFAULT1 | PROC_INTWIN | PROC_TSCOFF | PROC_HLT | PROC_INVLPG | PROC_MWAIT | PROC_RDPMC | \
                      PROC_RDTSC | PROC_CR3LOAD | PROC_CR3STORE | PROC_CR8LOAD | PROC_CR8STORE | PROC_TPR | PROC_NMIWIN | \
                      PROC_MOVDR | PROC_UNCONDIO | PROC_IOBITMAP | PROC_MTF | PROC_MSRBITMAP | PROC_MONITOR | \
                      PROC_PAUSE | PROC_SEC)
#define SEC_ALLOWED (SEC_APIC | SEC_EPT | SEC_DESC | SEC_RDTSCP | SEC_X2APIC | SEC_VPID | SEC_WBINVD | SEC_UG | \
                     SEC_APICREG | SEC_VID | SEC_PLE | SEC_RDRAND | SEC_INVPCID | SEC_VMFUNC | SEC_SHADOW | SEC_RDSEED | \
                     SEC_PML | SEC_TSCSCALE)
#define EXIT_DEFAULT1 0x00036DFFu
#define EXIT_TRUE_DEFAULT1 0x00036DFBu
#define EXIT_ALLOWED (EXIT_DEFAULT1 | EXIT_SAVE_DBG | EXIT_HOST64 | EXIT_LOAD_PERF | EXIT_ACK | EXIT_SAVE_PAT | \
                      EXIT_LOAD_PAT | EXIT_SAVE_EFER | EXIT_LOAD_EFER | EXIT_SAVE_TIMER)
#define ENTRY_DEFAULT1 0x11FFu
#define ENTRY_TRUE_DEFAULT1 0x11FBu
#define ENTRY_ALLOWED (ENTRY_DEFAULT1 | ENTRY_LOAD_DBG | ENTRY_IA32E | ENTRY_LOAD_PERF | ENTRY_LOAD_PAT | ENTRY_LOAD_EFER)
#define CR0_FIXED0 (CR0_PE | CR0_NE | CR0_PG)

static uint64_t ctls(uint32_t default1, uint32_t allowed1) { return default1 | (uint64_t)allowed1 << 32; }

/* The VMX capability MSRs (read-only) and IA32_FEATURE_CONTROL's lock. */
int vmx_msr(CPU *c, uint32_t idx, uint64_t *v, int write, int *err) {
    *err = 0;
    if (idx == 0x3A) { /* IA32_FEATURE_CONTROL: writable until locked */
        if (!write) *v = c->msr.feature_control;
        else if (c->msr.feature_control & 1) *err = 1;
        else c->msr.feature_control = *v;
        return 1;
    }
    if (idx < 0x480 || idx > 0x491) return 0;
    if (write) {
        *err = 1;
        return 1;
    }
    switch (idx) {
    case 0x480: /* BASIC: revision, 4 KiB VMCS, write-back, INS/OUTS info, TRUE controls, any error code */
        *v = VMCS_REVISION | 4096ULL << 32 | 6ULL << 50 | 1ULL << 54 | 1ULL << 55 | 1ULL << 56;
        break;
    case 0x481: case 0x48D: *v = ctls(PIN_DEFAULT1, PIN_ALLOWED); break;
    case 0x482: *v = ctls(PROC_DEFAULT1, PROC_ALLOWED); break;
    case 0x48E: *v = ctls(PROC_TRUE_DEFAULT1, PROC_ALLOWED); break;
    case 0x483: *v = ctls(EXIT_DEFAULT1, EXIT_ALLOWED); break;
    case 0x48F: *v = ctls(EXIT_TRUE_DEFAULT1, EXIT_ALLOWED); break;
    case 0x484: *v = ctls(ENTRY_DEFAULT1, ENTRY_ALLOWED); break;
    case 0x490: *v = ctls(ENTRY_TRUE_DEFAULT1, ENTRY_ALLOWED); break;
    case 0x485: /* MISC: preemption-timer rate, EFER.LMA saved in the entry controls, HLT activity,
                 * 4 CR3 targets, 512 MSRs, VMWRITE to any field, length-0 software-event injection */
        *v = TIMER_RATE | 1u << 5 | 1u << 6 | 4u << 16 | 1u << 29 | 1u << 30;
        break;
    case 0x486: *v = CR0_FIXED0; break;
    case 0x487: *v = 0xFFFFFFFF; break;
    case 0x488: *v = CR4_VMXE; break;
    case 0x489: *v = CR4_SUPPORTED; break;
    case 0x48A: *v = 0x1A << 1; break; /* highest field index (TSC multiplier... tertiary controls) */
    case 0x48B: *v = (uint64_t)SEC_ALLOWED << 32; break;
    case 0x48C: /* EPT: execute-only, 4 levels, UC/WB, 2M/1G pages, INVEPT (single, all), A/D;
                 * VPID: INVVPID (address, single, all, single keeping globals) */
        *v = 1u << 0 | 1u << 6 | 1u << 8 | 1u << 14 | 1u << 16 | 1u << 17 | 1u << 20 | 1u << 21 | 1u << 25 |
             1u << 26 | 1ULL << 32 | 1ULL << 40 | 1ULL << 41 | 1ULL << 42 | 1ULL << 43;
        break;
    default: *v = 1; break; /* 0x491 VMFUNC: EPTP switching */
    }
    return 1;
}

/* ---- instruction results ---- */

static void succeed(CPU *c) { c->rflags &= ~FL_ARITH; }

static void fail(CPU *c, int err) {
    c->rflags &= ~FL_ARITH;
    if (err == FAIL_INVALID || c->vmx.vmcs == ~0ULL) {
        c->rflags |= FL_CF;
        return;
    }
    c->rflags |= FL_ZF;
    wr(c, F_INSN_ERR, (uint64_t)err);
}

static uint32_t read_rev(CPU *c, uint64_t a) {
    uint32_t rev = 0;
    phys_read(c, a, &rev, 4);
    return rev;
}

/* ---- the virtual APIC ----
 * The virtual-APIC page holds VTPR (0x80), VPPR (0xA0), VEOI (0xB0), VISR
 * (0x100) and VIRR (0x200); RVI and SVI (the guest interrupt status) are
 * held here while the guest runs. Pending virtual interrupts are evaluated
 * at every instruction boundary, which is equivalent to the architectural
 * evaluation points since RVI and VPPR change only at those points. */

static uint32_t vapic_rd(CPU *c, uint32_t off) {
    uint32_t v = 0;
    phys_read(c, c->vmx.vapic + off, &v, 4);
    return v;
}

static void vapic_wr(CPU *c, uint32_t off, uint32_t v) { phys_write(c, c->vmx.vapic + off, &v, 4); }

/* Bit vec of the 256-bit register at base (VISR, VIRR). */
static void vbit(CPU *c, uint32_t base, uint32_t vec, int set) {
    uint32_t off = base + (vec >> 5) * 16, w = vapic_rd(c, off), b = 1u << (vec & 31);
    vapic_wr(c, off, set ? w | b : w & ~b);
}

static uint32_t vhighest(CPU *c, uint32_t base) {
    for (int i = 7; i >= 0; i--) {
        uint32_t w = vapic_rd(c, base + (uint32_t)i * 16);
        if (w) return (uint32_t)i * 32 + 31 - (uint32_t)__builtin_clz(w);
    }
    return 0;
}

static void ppr_virtualize(CPU *c) {
    uint32_t tpr = vapic_rd(c, 0x80) & 0xFF, svi = c->vmx.svi;
    vapic_wr(c, 0xA0, (tpr >> 4) >= (svi >> 4) ? tpr : svi & 0xF0);
}

/* Is a virtual interrupt pending (RVI above VPPR)? */
static int vint_pending(CPU *c) {
    return (c->vmx.proc2 & SEC_VID) && (c->vmx.rvi >> 4) > ((vapic_rd(c, 0xA0) >> 4) & 15);
}

static void tpr_virtualize(CPU *c) {
    Vmx *v = &c->vmx;
    if (v->proc2 & SEC_VID) ppr_virtualize(c);
    else if (((vapic_rd(c, 0x80) >> 4) & 15) < (v->tpr_threshold & 15)) v->trap_reason = 43;
}

static void eoi_virtualize(CPU *c) {
    Vmx *v = &c->vmx;
    uint32_t vec = v->svi;
    vbit(c, 0x100, vec, 0);
    v->svi = vhighest(c, 0x100);
    ppr_virtualize(c);
    if ((v->eoi_exit[vec >> 6] >> (vec & 63)) & 1) {
        v->trap_reason = 45;
        v->trap_qual = vec;
    }
}

static void self_ipi_virtualize(CPU *c, uint32_t vec) {
    vbit(c, 0x200, vec, 1);
    if (vec > c->vmx.rvi) c->vmx.rvi = vec;
}

/* ---- VM exits ---- */

static int pmode(CPU *c) { return (c->cr0 & CR0_PE) != 0; }

/* The processor's own TSC, which the preemption timer and PAUSE-loop detection count. */
static uint64_t tsc_now(CPU *c) { return c->read_tsc ? c->read_tsc(c) : c->icount; }

/* Record an exit; the caller performs it (vmx_exit) or unwinds to it (vmx_raise). */
static void pend(CPU *c, uint32_t reason, uint64_t qual) {
    Vmx *v = &c->vmx;
    v->x_reason = reason;
    v->x_qual = qual;
    v->x_intinfo = v->x_interr = v->x_info = 0;
    v->x_len = (uint32_t)v->insn_len;
    v->x_gpa = v->x_gla = 0;
}

_Noreturn void vmx_raise(CPU *c) {
    c->vmx_jump = 1;
    _longjmp(c->jb, 1);
}

_Noreturn static void exit_insn(CPU *c, uint32_t reason, uint64_t qual) {
    pend(c, reason, qual);
    vmx_raise(c);
}

_Noreturn void vmx_ept_fault(CPU *c, int misconfig, uint64_t qual, uint64_t gpa, uint64_t gla) {
    pend(c, misconfig ? 49 : 48, misconfig ? 0 : qual);
    c->vmx.x_gpa = gpa;
    c->vmx.x_gla = gla;
    vmx_raise(c);
}

/* Segment state in VMCS access-rights format (bit 16: unusable). */
static uint32_t seg_ar(CPU *c, uint16_t attr, int ss) {
    if (!pmode(c) || (attr & SA_P)) return attr;
    return 0x10000 | (ss ? attr & 0x60 : 0);
}

static void msr_list_store(CPU *c, uint64_t addr, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = 0;
        uint64_t val = 0;
        phys_read(c, addr + i * 16, &idx, 4);
        if (sys_rdmsr(c, idx, &val)) val = 0;
        phys_write(c, addr + i * 16 + 8, &val, 8);
    }
}

/* Returns 0, or the 1-based number of the entry that failed. */
static uint32_t msr_list_load(CPU *c, uint64_t addr, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ent[2] = {0, 0};
        uint64_t val = 0;
        phys_read(c, addr + i * 16, ent, 8);
        phys_read(c, addr + i * 16 + 8, &val, 8);
        uint32_t idx = ent[0];
        if (ent[1] || idx == 0xC0000100 || idx == 0xC0000101 || idx == 0x8B || (idx >> 8) == 8 || idx == 0x9B ||
            sys_wrmsr(c, idx, val))
            return i + 1;
    }
    return 0;
}

static void set_dr7(CPU *c, uint64_t v) {
    c->dr[7] = v;
    c->dbg_active = (v & 0xFF) != 0;
}

static void load_host(CPU *c) {
    Vmx *v = &c->vmx;
    int host64 = (v->exit_ctl & EXIT_HOST64) != 0;
    v->guest = 0;
    v->ept = 0;
    v->apic_page = 0;
    v->trap_reason = 0;
    c->cr0 = rd(c, F_H_CR0) | CR0_ET;
    c->cr3 = rd(c, F_H_CR3);
    c->cr4 = rd(c, F_H_CR4);
    set_dr7(c, 0x400);
    c->msr.sysenter_cs = rd(c, F_H_SYSENTER_CS);
    c->msr.sysenter_esp = rd(c, F_H_SYSENTER_ESP);
    c->msr.sysenter_eip = rd(c, F_H_SYSENTER_EIP);
    if (v->exit_ctl & EXIT_LOAD_EFER) c->efer = rd(c, F_H_EFER);
    else c->efer = host64 ? c->efer | EFER_LMA | EFER_LME : c->efer & ~(EFER_LMA | EFER_LME);
    if (v->exit_ctl & EXIT_LOAD_PAT) c->msr.pat = rd(c, F_H_PAT);
    if (v->exit_ctl & EXIT_LOAD_PERF) sys_wrmsr(c, 0x38F, rd(c, F_H_PERF));
    for (int s = 0; s < 6; s++) {
        uint16_t sel = (uint16_t)rd(c, F_H_SEL + 2 * s);
        c->sreg[s] = sel;
        c->sbase[s] = 0;
        c->slimit[s] = 0xFFFFFFFF;
        if (s == CS) c->sattr[s] = SA_P | SA_S | 0xB | SA_G | (host64 ? SA_L : SA_DB);
        else if (sel == 0) c->sattr[s] = 0;
        else c->sattr[s] = SA_P | SA_S | 0x3 | SA_G | SA_DB;
    }
    c->sbase[FS] = rd(c, F_H_FS_BASE);
    c->sbase[GS] = rd(c, F_H_GS_BASE);
    c->tr.sel = (uint16_t)rd(c, F_H_SEL + 12);
    c->tr.base = rd(c, F_H_TR_BASE);
    c->tr.limit = 0x67;
    c->tr.attr = SA_P | 0xB;
    c->ldtr.sel = 0;
    c->ldtr.base = 0;
    c->ldtr.limit = 0;
    c->ldtr.attr = 0;
    c->gdtr.base = rd(c, F_H_GDTR_BASE);
    c->gdtr.limit = 0xFFFF;
    c->idtr.base = rd(c, F_H_IDTR_BASE);
    c->idtr.limit = 0xFFFF;
    c->r[RSP] = rd(c, F_H_RSP);
    c->rip = c->next_rip = rd(c, F_H_RIP);
    c->rflags = 2;
    c->cpl = 0;
    c->intr_shadow = 0;
    c->halted = 0;
    c->ibt_wait = 0;
    if (v->pin & PIN_VNMI) c->nmi_blocked = v->host_nmi_blocked;
    cpu_update_mode(c);
    cpu_tlb_flush(c, 0);
    msr_list_load(c, rd(c, F_EXIT_LOAD), (uint32_t)rd(c, F_EXIT_LOAD_N)); /* a failure would be a VMX abort */
}

/* Perform the pending VM exit: save the guest, report, load the host. */
void vmx_exit(CPU *c) {
    Vmx *v = &c->vmx;
    if (tx_active(c)) tx_abort(c, 0);
    uint32_t vec = v->in_delivery ? v->vec_info : 0;
    v->ept = 0;
    wr(c, F_REASON, v->x_reason);
    wr(c, F_QUAL, v->x_qual);
    wr(c, F_X_INTINFO, v->x_intinfo);
    wr(c, F_X_INTERR, v->x_interr);
    wr(c, F_VEC_INFO, vec);
    wr(c, F_VEC_ERR, vec ? v->vec_err : 0);
    int soft = vec && ((vec >> 8) & 7) >= 4;
    wr(c, F_X_LEN, soft ? v->vec_len : v->x_len);
    wr(c, F_X_INFO, v->x_info);
    wr(c, F_GPA, v->x_gpa);
    wr(c, F_GLA, v->x_gla);

    wr(c, F_G_CR0, c->cr0);
    wr(c, F_G_CR3, c->cr3);
    wr(c, F_G_CR4, c->cr4);
    if (v->exit_ctl & EXIT_SAVE_DBG) wr(c, F_G_DR7, c->dr[7]);
    wr(c, F_G_RSP, c->r[RSP]);
    wr(c, F_G_RIP, c->rip);
    wr(c, F_G_RFLAGS, c->rflags);
    for (int s = 0; s < 6; s++) {
        wr(c, F_G_SEL + 2 * s, c->sreg[s]);
        wr(c, F_G_BASE + 2 * s, c->sbase[s]);
        wr(c, F_G_LIMIT + 2 * s, c->slimit[s]);
        wr(c, F_G_AR + 2 * s, seg_ar(c, c->sattr[s], s == SS));
    }
    const SegDesc *sd[2] = {&c->ldtr, &c->tr};
    for (int i = 0; i < 2; i++) {
        wr(c, F_G_SEL + 12 + 2 * i, sd[i]->sel);
        wr(c, F_G_BASE + 12 + 2 * i, sd[i]->base);
        wr(c, F_G_LIMIT + 12 + 2 * i, sd[i]->limit);
        wr(c, F_G_AR + 12 + 2 * i, (sd[i]->attr & SA_P) ? sd[i]->attr : 0x10000);
    }
    wr(c, F_G_GDTR_BASE, c->gdtr.base);
    wr(c, F_G_GDTR_LIMIT, c->gdtr.limit);
    wr(c, F_G_IDTR_BASE, c->idtr.base);
    wr(c, F_G_IDTR_LIMIT, c->idtr.limit);
    wr(c, F_G_SYSENTER_CS, c->msr.sysenter_cs);
    wr(c, F_G_SYSENTER_ESP, c->msr.sysenter_esp);
    wr(c, F_G_SYSENTER_EIP, c->msr.sysenter_eip);
    if (v->exit_ctl & EXIT_SAVE_PAT) wr(c, F_G_PAT, c->msr.pat);
    if (v->exit_ctl & EXIT_SAVE_EFER) wr(c, F_G_EFER, c->efer);
    uint32_t ectl = (uint32_t)rd(c, F_ENTRY_CTL);
    ectl = (c->efer & EFER_LMA) ? ectl | ENTRY_IA32E : ectl & ~ENTRY_IA32E;
    wr(c, F_ENTRY_CTL, ectl);
    uint32_t intr = c->intr_shadow ? ((c->rflags & FL_IF) ? 1 : 2) : 0;
    wr(c, F_G_INTR, intr | (c->nmi_blocked ? 8 : 0));
    wr(c, F_G_ACTIVITY, c->halted ? 1 : 0);
    wr(c, F_G_PENDING_DBG, 0);
    if (v->proc2 & SEC_VID) wr(c, F_G_INTR_STATUS, v->rvi | v->svi << 8);
    if (v->proc2 & SEC_PML) wr(c, F_PML_INDEX, v->pml_index);
    if (v->exit_ctl & EXIT_SAVE_TIMER) {
        uint64_t now = tsc_now(c) >> TIMER_RATE, end = v->timer_deadline >> TIMER_RATE;
        wr(c, F_G_TIMER, end > now ? end - now : 0);
    }
    if ((v->proc2 & SEC_EPT) && (c->cr0 & CR0_PG) && (c->cr4 & CR4_PAE) && !(c->efer & EFER_LMA)) {
        v->ept = 1;
        for (int i = 0; i < 4; i++) {
            uint64_t gpa = (c->cr3 & 0xFFFFFFE0) + (uint64_t)i * 8, pdpte = 0;
            uint64_t hpa = mmu_ept_probe(c, gpa);
            if (hpa != ~0ULL) phys_read(c, hpa, &pdpte, 8);
            wr(c, F_G_PDPTE + 2 * i, pdpte);
        }
        v->ept = 0;
    }
    wr(c, F_ENTRY_INFO, rd(c, F_ENTRY_INFO) & ~0x80000000ULL);
    msr_list_store(c, rd(c, F_EXIT_STORE), (uint32_t)rd(c, F_EXIT_STORE_N));

    int nmi_exit = v->x_reason == 0 && ((v->x_intinfo >> 8) & 7) == 2;
    load_host(c);
    if (nmi_exit) c->nmi_blocked = 1;
    v->in_delivery = 0;
    c->delivering = -1;
    c->snap_valid = 0;
    v->exits++;
}

/* VM-entry failure while checking or loading the guest state. */
static void entry_fail(CPU *c, uint32_t reason, uint64_t qual) {
    wr(c, F_REASON, reason | 0x80000000u);
    wr(c, F_QUAL, qual);
    load_host(c);
}

/* ---- VM entry ---- */

static int ctl_ok(uint32_t v, uint32_t default1, uint32_t allowed1) {
    return (v & default1) == default1 && !(v & ~allowed1);
}

static int phys_ok(uint64_t a, uint64_t align) { return !(a & (align - 1)) && !(a >> PHYS_BITS); }

static int canonical48(uint64_t v) { return (uint64_t)((int64_t)(v << 16) >> 16) == v; }

static int eptp_ok(uint64_t e) {
    return ((e & 7) == 0 || (e & 7) == 6) && ((e >> 3) & 7) == 3 && !(e & 0xF80) && !(e >> PHYS_BITS);
}

static int check_controls(CPU *c) {
    Vmx *v = &c->vmx;
    if (!ctl_ok(v->pin, PIN_DEFAULT1, PIN_ALLOWED) || !ctl_ok(v->proc, PROC_TRUE_DEFAULT1, PROC_ALLOWED) ||
        !ctl_ok(v->exit_ctl, EXIT_TRUE_DEFAULT1, EXIT_ALLOWED) ||
        !ctl_ok(v->entry_ctl, ENTRY_TRUE_DEFAULT1, ENTRY_ALLOWED) || (v->proc2 & ~SEC_ALLOWED))
        return 0;
    if (v->cr3_count > 4) return 0;
    if ((v->proc & PROC_IOBITMAP) && (!phys_ok(v->io_a, 4096) || !phys_ok(v->io_b, 4096))) return 0;
    if ((v->proc & PROC_MSRBITMAP) && !phys_ok(v->msr_bitmap, 4096)) return 0;
    if ((v->pin & PIN_VNMI) && !(v->pin & PIN_NMI)) return 0;
    if ((v->proc & PROC_NMIWIN) && !(v->pin & PIN_VNMI)) return 0;
    if ((v->proc2 & SEC_EPT) && !eptp_ok(v->eptp)) return 0;
    if ((v->proc2 & SEC_UG) && !(v->proc2 & SEC_EPT)) return 0;
    if ((v->proc2 & SEC_VPID) && (rd(c, F_VPID) & 0xFFFF) == 0) return 0;
    if ((v->exit_ctl & EXIT_SAVE_TIMER) && !(v->pin & PIN_PREEMPT)) return 0;
    if (v->proc & PROC_TPR) {
        if (!phys_ok(v->vapic, 4096)) return 0;
        if (!(v->proc2 & SEC_VID) && (v->tpr_threshold & ~15u)) return 0;
        if (!(v->proc2 & (SEC_VID | SEC_APIC)) && v->tpr_threshold > ((vapic_rd(c, 0x80) >> 4) & 15)) return 0;
    } else if (v->proc2 & (SEC_X2APIC | SEC_APICREG | SEC_VID)) {
        return 0;
    }
    if ((v->proc2 & SEC_X2APIC) && (v->proc2 & SEC_APIC)) return 0;
    if ((v->proc2 & SEC_APIC) && !phys_ok(rd(c, F_APIC_ACCESS), 4096)) return 0;
    if ((v->proc2 & SEC_VID) && !(v->pin & PIN_EXTINT)) return 0;
    if ((v->pin & PIN_POSTED) && (!(v->proc2 & SEC_VID) || !(v->exit_ctl & EXIT_ACK) || v->pi_vector > 0xFF ||
                                  !phys_ok(v->pi_desc, 64)))
        return 0;
    if ((v->proc2 & SEC_PML) && (!(v->proc2 & SEC_EPT) || !phys_ok(v->pml_addr, 4096))) return 0;
    if ((v->proc2 & SEC_VMFUNC) && ((v->vmfunc & ~1ULL) || ((v->vmfunc & 1) && (!(v->proc2 & SEC_EPT) ||
                                                                                 !phys_ok(v->eptp_list, 4096)))))
        return 0;
    if ((v->proc2 & SEC_SHADOW) && (!phys_ok(v->vmread_bm, 4096) || !phys_ok(v->vmwrite_bm, 4096))) return 0;
    uint32_t n[3] = {(uint32_t)rd(c, F_EXIT_STORE_N), (uint32_t)rd(c, F_EXIT_LOAD_N), (uint32_t)rd(c, F_ENTRY_LOAD_N)};
    uint64_t a[3] = {rd(c, F_EXIT_STORE), rd(c, F_EXIT_LOAD), rd(c, F_ENTRY_LOAD)};
    for (int i = 0; i < 3; i++)
        if (n[i] > 512 || (n[i] && !phys_ok(a[i], 16))) return 0;
    uint32_t info = (uint32_t)rd(c, F_ENTRY_INFO);
    if (info & 0x80000000u) {
        int type = (info >> 8) & 7, vec = info & 0xFF;
        if ((info & 0x7FFFF000u) || type == 1 || type == 7) return 0;
        if (type == 2 && vec != 2) return 0;
        if (type == 3 && vec > 31) return 0;
        if ((info & 0x800) && (type != 3 || !(rd(c, F_G_CR0) & CR0_PE))) return 0;
        if (type >= 4 && rd(c, F_ENTRY_LEN) > 15) return 0;
    }
    return 1;
}

static int check_host(CPU *c) {
    Vmx *v = &c->vmx;
    uint64_t cr0 = rd(c, F_H_CR0), cr4 = rd(c, F_H_CR4);
    int host64 = (v->exit_ctl & EXIT_HOST64) != 0;
    if ((cr0 & CR0_FIXED0) != CR0_FIXED0 || (cr0 >> 32) || !(cr4 & CR4_VMXE) || (cr4 & ~CR4_SUPPORTED)) return 0;
    if (host64 != ((c->efer & EFER_LMA) != 0)) return 0;
    if (host64 && !(cr4 & CR4_PAE)) return 0;
    if (!host64 && (v->entry_ctl & ENTRY_IA32E)) return 0;
    for (int s = 0; s < 7; s++)
        if (rd(c, F_H_SEL + 2 * s) & 7) return 0;
    if (!rd(c, F_H_SEL + 2 * CS) || !rd(c, F_H_SEL + 12)) return 0;
    if (!host64 && !rd(c, F_H_SEL + 2 * SS)) return 0;
    if (host64 && !canonical48(rd(c, F_H_RIP))) return 0;
    return 1;
}

static int check_guest(CPU *c) {
    Vmx *v = &c->vmx;
    uint64_t cr0 = rd(c, F_G_CR0), cr4 = rd(c, F_G_CR4), fl = rd(c, F_G_RFLAGS);
    int ia32e = (v->entry_ctl & ENTRY_IA32E) != 0;
    uint64_t f0 = (v->proc2 & SEC_UG) ? CR0_NE : CR0_FIXED0;
    if ((cr0 & f0) != f0 || (cr0 >> 32) || ((cr0 & CR0_PG) && !(cr0 & CR0_PE))) return 0;
    if ((cr0 & CR0_NW) && !(cr0 & CR0_CD)) return 0;
    if (!(cr4 & CR4_VMXE) || (cr4 & ~CR4_SUPPORTED)) return 0;
    if (ia32e && (!(cr0 & CR0_PG) || !(cr4 & CR4_PAE))) return 0;
    if (!ia32e && (cr4 & CR4_PCIDE)) return 0;
    if (v->entry_ctl & ENTRY_LOAD_EFER) {
        uint64_t efer = rd(c, F_G_EFER);
        if (!!(efer & EFER_LMA) != ia32e || ((cr0 & CR0_PG) && !!(efer & EFER_LME) != ia32e)) return 0;
    }
    if ((fl >> 22) || (fl & 0x8028) || !(fl & 2)) return 0;
    if ((fl & FL_VM) && (ia32e || !(cr0 & CR0_PE))) return 0;
    uint32_t cs_ar = (uint32_t)rd(c, F_G_AR + 2 * CS), tr_ar = (uint32_t)rd(c, F_G_AR + 14);
    uint32_t ldtr_ar = (uint32_t)rd(c, F_G_AR + 12), ss_ar = (uint32_t)rd(c, F_G_AR + 2 * SS);
    if (ia32e && (cs_ar & SA_L) && (cs_ar & SA_DB)) return 0;
    if ((cr0 & CR0_PE) && !(fl & FL_VM)) {
        if ((cs_ar & 0x10000) || !(cs_ar & SA_P)) return 0;
        if ((tr_ar & 0x10000) || (tr_ar & SA_S) || !(tr_ar & SA_P)) return 0;
        if ((tr_ar & 15) != 11 && (ia32e || (tr_ar & 15) != 3)) return 0;
        if (!(ldtr_ar & 0x10000) && ((ldtr_ar & 0x1F) != 2 || !(ldtr_ar & SA_P))) return 0;
    }
    uint32_t act = (uint32_t)rd(c, F_G_ACTIVITY), intr = (uint32_t)rd(c, F_G_INTR);
    if (act > 1 || (act == 1 && SA_DPL(ss_ar) != 0)) return 0;
    if ((intr & ~0x1Fu) || (intr & 3) == 3 || ((intr & 1) && !(fl & FL_IF))) return 0;
    uint64_t link = rd(c, F_LINK);
    if (link != ~0ULL) { /* a shadow VMCS for VMREAD/VMWRITE in the guest */
        uint32_t rev = 0;
        if (!phys_ok(link, 4096)) return 0;
        phys_read(c, link, &rev, 4);
        if ((rev & 0x7FFFFFFF) != VMCS_REVISION || (rev >> 31) != !!(v->proc2 & SEC_SHADOW)) return 0;
    }
    return 1;
}

static void load_guest(CPU *c) {
    Vmx *v = &c->vmx;
    int ia32e = (v->entry_ctl & ENTRY_IA32E) != 0;
    c->cr0 = rd(c, F_G_CR0) | CR0_ET;
    c->cr3 = rd(c, F_G_CR3);
    c->cr4 = rd(c, F_G_CR4);
    if (v->entry_ctl & ENTRY_LOAD_DBG) set_dr7(c, rd(c, F_G_DR7));
    if (v->entry_ctl & ENTRY_LOAD_EFER) {
        c->efer = rd(c, F_G_EFER);
    } else {
        c->efer = ia32e ? c->efer | EFER_LMA : c->efer & ~EFER_LMA;
        if (c->cr0 & CR0_PG) c->efer = ia32e ? c->efer | EFER_LME : c->efer & ~EFER_LME;
    }
    if (v->entry_ctl & ENTRY_LOAD_PAT) c->msr.pat = rd(c, F_G_PAT);
    if (v->entry_ctl & ENTRY_LOAD_PERF) sys_wrmsr(c, 0x38F, rd(c, F_G_PERF));
    for (int s = 0; s < 6; s++) {
        uint32_t ar = (uint32_t)rd(c, F_G_AR + 2 * s);
        c->sreg[s] = (uint16_t)rd(c, F_G_SEL + 2 * s);
        c->sbase[s] = rd(c, F_G_BASE + 2 * s);
        c->slimit[s] = (uint32_t)rd(c, F_G_LIMIT + 2 * s);
        c->sattr[s] = (ar & 0x10000) ? (s == SS ? (uint16_t)(ar & 0x60) : 0) : (uint16_t)(ar & 0xF0FF);
    }
    SegDesc *sd[2] = {&c->ldtr, &c->tr};
    for (int i = 0; i < 2; i++) {
        uint32_t ar = (uint32_t)rd(c, F_G_AR + 12 + 2 * i);
        sd[i]->sel = (uint16_t)rd(c, F_G_SEL + 12 + 2 * i);
        sd[i]->base = rd(c, F_G_BASE + 12 + 2 * i);
        sd[i]->limit = (uint32_t)rd(c, F_G_LIMIT + 12 + 2 * i);
        sd[i]->attr = (ar & 0x10000) ? 0 : (uint16_t)(ar & 0xF0FF);
    }
    c->gdtr.base = rd(c, F_G_GDTR_BASE);
    c->gdtr.limit = (uint16_t)rd(c, F_G_GDTR_LIMIT);
    c->idtr.base = rd(c, F_G_IDTR_BASE);
    c->idtr.limit = (uint16_t)rd(c, F_G_IDTR_LIMIT);
    c->r[RSP] = rd(c, F_G_RSP);
    c->rip = c->next_rip = rd(c, F_G_RIP);
    c->rflags = rd(c, F_G_RFLAGS) | 2;
    c->msr.sysenter_cs = rd(c, F_G_SYSENTER_CS);
    c->msr.sysenter_esp = rd(c, F_G_SYSENTER_ESP);
    c->msr.sysenter_eip = rd(c, F_G_SYSENTER_EIP);
    uint32_t intr = (uint32_t)rd(c, F_G_INTR);
    c->intr_shadow = (intr & 3) != 0;
    if (v->pin & PIN_VNMI) v->host_nmi_blocked = c->nmi_blocked;
    c->nmi_blocked = (intr >> 3) & 1;
    c->halted = rd(c, F_G_ACTIVITY) == 1;
    c->ibt_wait = 0;
    c->cpl = !(c->cr0 & CR0_PE) ? 0 : (c->rflags & FL_VM) ? 3 : SA_DPL(c->sattr[SS]);
    if (v->proc2 & SEC_VID) {
        uint32_t st = (uint32_t)rd(c, F_G_INTR_STATUS);
        v->rvi = st & 0xFF;
        v->svi = (st >> 8) & 0xFF;
    }
    if (v->proc2 & SEC_PML) v->pml_index = (uint32_t)rd(c, F_PML_INDEX);
    if (v->pin & PIN_PREEMPT) v->timer_deadline = ((tsc_now(c) >> TIMER_RATE) + rd(c, F_G_TIMER)) << TIMER_RATE;
    v->ple_last = ~0ULL;
    cpu_update_mode(c);
    cpu_tlb_flush(c, 0);
}

/* Cache the controls the guest runs under. */
static void load_controls(CPU *c) {
    Vmx *v = &c->vmx;
    v->pin = (uint32_t)rd(c, F_PIN);
    v->proc = (uint32_t)rd(c, F_PROC);
    v->proc2 = (v->proc & PROC_SEC) ? (uint32_t)rd(c, F_PROC2) : 0;
    v->exit_ctl = (uint32_t)rd(c, F_EXIT_CTL);
    v->entry_ctl = (uint32_t)rd(c, F_ENTRY_CTL);
    v->exc_bitmap = (uint32_t)rd(c, F_EXC_BITMAP);
    v->pf_mask = (uint32_t)rd(c, F_PF_MASK);
    v->pf_match = (uint32_t)rd(c, F_PF_MATCH);
    v->cr3_count = (uint32_t)rd(c, F_CR3_COUNT);
    v->cr0_mask = rd(c, F_CR0_MASK);
    v->cr4_mask = rd(c, F_CR4_MASK);
    v->cr0_shadow = rd(c, F_CR0_SHADOW);
    v->cr4_shadow = rd(c, F_CR4_SHADOW);
    for (int i = 0; i < 4; i++) v->cr3_target[i] = rd(c, F_CR3_TARGET + 2 * i);
    v->io_a = rd(c, F_IO_A);
    v->io_b = rd(c, F_IO_B);
    v->msr_bitmap = rd(c, F_MSR_BITMAP);
    v->tsc_offset = rd(c, F_TSC_OFFSET);
    v->tsc_mult = (v->proc2 & SEC_TSCSCALE) ? rd(c, F_TSC_MULT) : 1ULL << 48;
    v->eptp = rd(c, F_EPTP);
    v->vapic = rd(c, F_VAPIC);
    v->tpr_threshold = (uint32_t)rd(c, F_TPR_THRESHOLD);
    v->pi_vector = (uint32_t)rd(c, F_PI_VECTOR);
    v->pi_desc = rd(c, F_PI_DESC);
    for (int i = 0; i < 4; i++) v->eoi_exit[i] = rd(c, F_EOI_EXIT + 2 * i);
    v->ple_gap = (uint32_t)rd(c, F_PLE_GAP);
    v->ple_window = (uint32_t)rd(c, F_PLE_WINDOW);
    v->pml_addr = rd(c, F_PML_ADDR);
    v->link = rd(c, F_LINK);
    v->vmread_bm = rd(c, F_VMREAD_BM);
    v->vmwrite_bm = rd(c, F_VMWRITE_BM);
    v->vmfunc = rd(c, F_VMFUNC);
    v->eptp_list = rd(c, F_EPTP_LIST);
}

/* Deliver the event in the VM-entry interruption-information field. */
static void inject(CPU *c, uint32_t info) {
    int vec = info & 0xFF, type = (info >> 8) & 7, has_err = (info >> 11) & 1;
    uint64_t err = rd(c, F_ENTRY_ERR), ret = c->rip;
    static const int kinds[8] = {EV_EXT, EV_EXT, EV_EXT, EV_EXC, EV_SOFT, EV_ICEBP, EV_SOFTEXC, EV_EXT};
    if (type >= 4) ret = c->rip + rd(c, F_ENTRY_LEN);
    c->halted = 0;
    c->delivering = type == 2 ? 2 : type == 0 ? 0x100 : vec;
    if (type == 2) c->nmi_blocked = 1;
    sys_deliver(c, vec, kinds[type], has_err, err, ret);
    c->delivering = -1;
}

static void vm_entry(CPU *c, int launch) {
    Vmx *v = &c->vmx;
    if (v->vmcs == ~0ULL) {
        fail(c, FAIL_INVALID);
        return;
    }
    if (c->intr_shadow && !(c->rflags & FL_IF)) { /* blocking by MOV SS */
        fail(c, 26);
        return;
    }
    if (read_rev(c, v->vmcs) >> 31) { /* a shadow VMCS */
        fail(c, FAIL_INVALID);
        return;
    }
    uint32_t ls = launch_state(c, v->vmcs);
    if (launch ? ls != 0 : ls != 1) {
        fail(c, launch ? 4 : 5);
        return;
    }
    load_controls(c);
    if (!check_controls(c)) {
        fail(c, 7);
        return;
    }
    if (!check_host(c)) {
        fail(c, 8);
        return;
    }
    if (!check_guest(c)) {
        entry_fail(c, 33, 0);
        return;
    }
    v->guest = 1;
    v->ept = (v->proc2 & SEC_EPT) != 0;
    load_guest(c);
    uint32_t bad = msr_list_load(c, rd(c, F_ENTRY_LOAD), (uint32_t)rd(c, F_ENTRY_LOAD_N));
    if (bad) {
        v->guest = 0;
        v->ept = 0;
        entry_fail(c, 34, bad);
        return;
    }
    if (launch) set_launch_state(c, v->vmcs, 1);
    v->entries++;
    c->rf_keep = 1; /* the guest's RFLAGS.RF stays */
    if (v->proc2 & SEC_APIC) v->apic_page = (rd(c, F_APIC_ACCESS) & ~0xFFFULL) | 1;
    if (v->proc2 & SEC_VID) ppr_virtualize(c);
    uint32_t info = (uint32_t)rd(c, F_ENTRY_INFO);
    if (info & 0x80000000u) inject(c, info);
    if ((v->proc & PROC_TPR) && !(v->proc2 & SEC_VID) && v->tpr_threshold > ((vapic_rd(c, 0x80) >> 4) & 15))
        v->trap_reason = 43; /* TPR below threshold, right after the entry */
}

/* ---- VMX instructions (VMX root operation, or outside VMX) ---- */

static void need_vmx(CPU *c) {
    if (!c->vmx.on || !pmode(c) || (c->rflags & FL_VM) || ((c->efer & EFER_LMA) && !(c->sattr[CS] & SA_L)))
        raise_exc(c, EXC_UD, 0, NULL);
    if (c->cpl > 0) raise_exc(c, EXC_GP, 0, NULL);
}

static uint64_t mem_ptr(CPU *c, const Insn *in) { return rdm(c, ea_linear(c, in), 8); }

static void vmxon(CPU *c, const Insn *in) {
    if (c->vmx.on) {
        need_vmx(c);
        fail(c, 15);
        return;
    }
    if (!(c->cr4 & CR4_VMXE) || !pmode(c) || (c->rflags & FL_VM) ||
        ((c->efer & EFER_LMA) && !(c->sattr[CS] & SA_L)))
        raise_exc(c, EXC_UD, 0, NULL);
    if (c->cpl > 0 || (c->cr0 & CR0_FIXED0) != CR0_FIXED0 || c->a20_mask != ~0ULL || (c->msr.feature_control & 5) != 5)
        raise_exc(c, EXC_GP, 0, NULL);
    uint64_t a = mem_ptr(c, in);
    if (!phys_ok(a, 4096) || read_rev(c, a) != VMCS_REVISION) {
        fail(c, FAIL_INVALID);
        return;
    }
    c->vmx.on = 1;
    c->vmx.vmxon = a;
    c->vmx.vmcs = ~0ULL;
    succeed(c);
}

/* In a guest (VMCS shadowing), VMREAD and VMWRITE access the shadow VMCS. */
static void vmread_write(CPU *c, const Insn *in, int write) {
    int sz = c->bits == 64 ? 8 : 4;
    uint64_t vmcs = c->vmx.guest ? c->vmx.link : c->vmx.vmcs;
    if (vmcs == ~0ULL) {
        fail(c, FAIL_INVALID);
        return;
    }
    uint64_t enc = getreg(c, in->reg, sz, 1);
    int off = field_off(enc);
    if (off < 0) {
        fail(c, 12);
        return;
    }
    uint32_t base = (uint32_t)enc & ~1u;
    int hi = (int)(enc & 1);
    if (!write) {
        uint64_t val = rd_at(c, vmcs, base);
        if (hi) val >>= 32;
        if (in->mod == 3) setreg(c, in->rm, sz, 1, val);
        else wrm(c, ea_linear(c, in), sz, val);
    } else {
        uint64_t val = in->mod == 3 ? getreg(c, in->rm, sz, 1) : rdm(c, ea_linear(c, in), sz);
        if (hi) val = (rd_at(c, vmcs, base) & 0xFFFFFFFF) | val << 32;
        wr_at(c, vmcs, base, val);
    }
    succeed(c);
}

/* VMFUNC in a guest: function 0 switches to an EPTP from the EPTP list. */
static void vmfunc(CPU *c) {
    Vmx *v = &c->vmx;
    uint64_t fn = c->r[RAX] & 0xFFFFFFFF, idx = c->r[RCX] & 0xFFFFFFFF, e = 0;
    if (!v->guest || !(v->proc2 & SEC_VMFUNC)) raise_exc(c, EXC_UD, 0, NULL);
    if (fn != 0 || !(v->vmfunc & 1) || idx >= 512) exit_insn(c, 59, 0);
    phys_read(c, v->eptp_list + idx * 8, &e, 8);
    if (!eptp_ok(e)) exit_insn(c, 59, 0);
    v->eptp = e;
    wr(c, F_EPTP, e);
    cpu_tlb_flush(c, 0);
}

void exec_vmx(CPU *c, const Insn *in) {
    Vmx *v = &c->vmx;
    if (in->id == I_GRP7R && in->modrm == 0xD4) {
        vmfunc(c);
        return;
    }
    if (in->id == I_VMXON) {
        vmxon(c, in);
        return;
    }
    if (in->id == I_GRP7R && in->modrm == 0xC1) { /* VMCALL */
        if (!v->on) raise_exc(c, EXC_UD, 0, NULL);
        if (c->cpl > 0) raise_exc(c, EXC_GP, 0, NULL);
        fail(c, 1); /* no dual-monitor treatment */
        return;
    }
    need_vmx(c);
    switch (in->id) {
    case I_VMPTRLD: {
        uint64_t a = mem_ptr(c, in);
        if (!phys_ok(a, 4096)) fail(c, 9);
        else if (a == v->vmxon) fail(c, 10);
        else if ((read_rev(c, a) & 0x7FFFFFFF) != VMCS_REVISION) fail(c, 11); /* bit 31: a shadow VMCS */
        else v->vmcs = a, succeed(c);
        return;
    }
    case I_VMCLEAR: {
        uint64_t a = mem_ptr(c, in);
        if (!phys_ok(a, 4096)) fail(c, 2);
        else if (a == v->vmxon) fail(c, 3);
        else {
            set_launch_state(c, a, 0);
            if (a == v->vmcs) v->vmcs = ~0ULL;
            succeed(c);
        }
        return;
    }
    case I_VMPTRST:
        wrm(c, ea_linear(c, in), 8, v->vmcs);
        succeed(c);
        return;
    case I_VMREAD: vmread_write(c, in, 0); return;
    case I_VMWRITE: vmread_write(c, in, 1); return;
    case I_INVEPT: case I_INVVPID: {
        uint64_t type = getreg(c, in->reg, c->bits == 64 ? 8 : 4, 1);
        uint64_t a = ea_linear(c, in);
        uint64_t d0 = rdm(c, a, 8), d1 = rdm(c, a + 8, 8);
        int ok = in->id == I_INVEPT ? type == 1 || type == 2
                                    : type <= 3 && (type == 2 || (d0 & 0xFFFF)) && !(d0 >> 16) &&
                                          (type != 0 || canonical48(d1));
        if (!ok) {
            fail(c, 28);
            return;
        }
        cpu_tlb_flush(c, 0);
        succeed(c);
        return;
    }
    case I_GRP7R:
        switch (in->modrm) {
        case 0xC2: case 0xC3: vm_entry(c, in->modrm == 0xC2); return;
        case 0xC4: /* VMXOFF; a latched INIT is processed at the next instruction boundary */
            v->on = 0;
            v->vmcs = ~0ULL;
            succeed(c);
            return;
        }
        break;
    }
    raise_exc(c, EXC_UD, 0, NULL);
}

/* ---- VMX non-root operation ---- */

/* Instruction information for exits with a memory or register operand. */
static uint32_t insn_info(const Insn *in) {
    uint32_t info = (uint32_t)(in->addrsize == 2 ? 0 : in->addrsize == 4 ? 1 : 2) << 7;
    if (in->mod == 3) {
        info |= 1u << 10 | (uint32_t)(in->rm & 15) << 3;
    } else {
        int seg = in->seg >= 0 ? in->seg : (in->base == RSP || in->base == RBP) ? SS : DS;
        info |= in->scale | (uint32_t)seg << 15;
        info |= in->index >= 0 ? (uint32_t)(in->index & 15) << 18 : 1u << 22;
        info |= in->base >= 0 && in->base != REG_RIP ? (uint32_t)(in->base & 15) << 23 : 1u << 27;
    }
    return info | (uint32_t)(in->reg & 15) << 28;
}

_Noreturn static void exit_info(CPU *c, uint32_t reason, uint64_t qual, uint32_t info) {
    pend(c, reason, qual);
    c->vmx.x_info = info;
    vmx_raise(c);
}

/* VMCS shadowing: does the VMREAD/VMWRITE bitmap let the guest access the shadow VMCS? */
static int shadowed(CPU *c, const Insn *in) {
    Vmx *v = &c->vmx;
    if (!(v->proc2 & SEC_SHADOW)) return 0;
    uint64_t enc = getreg(c, in->reg, c->bits == 64 ? 8 : 4, 1);
    uint8_t b = 0;
    if (enc >> 15) return 0;
    phys_read(c, (in->id == I_VMREAD ? v->vmread_bm : v->vmwrite_bm) + enc / 8, &b, 1);
    return !((b >> (enc & 7)) & 1);
}

static int msr_exits(CPU *c, uint32_t idx, int write) {
    Vmx *v = &c->vmx;
    if (!(v->proc & PROC_MSRBITMAP)) return 1;
    uint64_t a = v->msr_bitmap + (write ? 2048 : 0);
    if (idx >= 0xC0000000 && idx <= 0xC0001FFF) a += 1024, idx -= 0xC0000000;
    else if (idx > 0x1FFF) return 1;
    uint8_t b = 0;
    phys_read(c, a + idx / 8, &b, 1);
    return (b >> (idx & 7)) & 1;
}

static void io_exit(CPU *c, const Insn *in) {
    Vmx *v = &c->vmx;
    int string = in->id == I_INS || in->id == I_OUTS, input = in->id == I_IN || in->id == I_INS;
    int size, imm = 0;
    uint16_t port;
    if (string) {
        size = (in->opnd[0] == O_Yb || in->opnd[1] == O_Xb) ? 1 : in->opsize == 8 ? 4 : in->opsize;
        port = (uint16_t)c->r[RDX];
    } else if (input) {
        size = opnd_size(in, in->opnd[0]);
        imm = in->opnd[1] == O_Ib;
        port = imm ? (uint16_t)(in->imm & 0xFF) : (uint16_t)c->r[RDX];
    } else {
        size = opnd_size(in, in->opnd[1]);
        imm = in->opnd[0] == O_Ib;
        port = imm ? (uint16_t)(in->imm & 0xFF) : (uint16_t)c->r[RDX];
    }
    sys_check_io(c, port, size); /* the TSS permission check comes first */
    int ex = 0;
    if (v->proc & PROC_IOBITMAP) {
        for (int i = 0; i < size && !ex; i++) {
            uint32_t p = (uint32_t)port + (uint32_t)i;
            if (p > 0xFFFF) {
                ex = 1;
                break;
            }
            uint8_t b = 0;
            phys_read(c, (p < 0x8000 ? v->io_a : v->io_b) + (p & 0x7FFF) / 8, &b, 1);
            ex = (b >> (p & 7)) & 1;
        }
    } else {
        ex = (v->proc & PROC_UNCONDIO) != 0;
    }
    if (!ex) return;
    uint64_t qual = (uint64_t)(size - 1) | (input ? 8 : 0) | (string ? 16 : 0) | (in->rep ? 32 : 0) |
                    (imm ? 64 : 0) | (uint64_t)port << 16;
    pend(c, 30, qual);
    if (string) {
        uint64_t am = MASK(in->addrsize);
        int seg = input ? ES : in->seg >= 0 ? in->seg : DS;
        v->x_gla = linear(c, seg, (input ? c->r[RDI] : c->r[RSI]) & am);
        v->x_info = (uint32_t)(in->addrsize == 2 ? 0 : in->addrsize == 4 ? 1 : 2) << 7 | (uint32_t)seg << 15;
    }
    vmx_raise(c);
}

static void cr_exit(CPU *c, const Insn *in) {
    Vmx *v = &c->vmx;
    int n = in->reg, gpr = in->rm & 15;
    if (in->opcode == 0x22) { /* MOV to CR */
        uint64_t val = getreg(c, in->rm, c->bits == 64 ? 8 : 4, 1);
        int ex = 0;
        if (n == 0) ex = ((val ^ v->cr0_shadow) & v->cr0_mask) != 0;
        else if (n == 4) ex = ((val ^ v->cr4_shadow) & v->cr4_mask) != 0;
        else if (n == 3 && (v->proc & PROC_CR3LOAD)) {
            ex = 1;
            for (uint32_t i = 0; i < v->cr3_count && i < 4; i++)
                if (v->cr3_target[i] == val) ex = 0;
        } else if (n == 8) ex = (v->proc & PROC_CR8LOAD) != 0;
        if (ex) exit_insn(c, 28, (uint64_t)n | (uint64_t)gpr << 8);
    } else if ((n == 3 && (v->proc & PROC_CR3STORE)) || (n == 8 && (v->proc & PROC_CR8STORE))) {
        exit_insn(c, 28, (uint64_t)n | 1 << 4 | (uint64_t)gpr << 8);
    }
}

void vmx_intercept(CPU *c, const Insn *in) {
    Vmx *v = &c->vmx;
    int priv = c->cpl == 0 && !(c->rflags & FL_VM);
    int vmx_ud = !pmode(c) || (c->rflags & FL_VM) || ((c->efer & EFER_LMA) && !(c->sattr[CS] & SA_L));
    switch (in->id) {
    case I_CPUID: exit_insn(c, 10, 0);
    case I_HLT: if (priv && (v->proc & PROC_HLT)) exit_insn(c, 12, 0); return;
    case I_INVD: if (priv) exit_insn(c, 13, 0); return;
    case I_WBINVD: if (priv && (v->proc2 & SEC_WBINVD)) exit_insn(c, 54, 0); return;
    case I_INVLPG: if (priv && (v->proc & PROC_INVLPG)) exit_insn(c, 14, ea_linear(c, in)); return;
    case I_INVPCID:
        if (!(v->proc2 & SEC_INVPCID)) raise_exc(c, EXC_UD, 0, NULL);
        if (priv && (v->proc & PROC_INVLPG)) exit_info(c, 58, (uint64_t)in->disp, insn_info(in));
        return;
    case I_RDPMC:
        if ((v->proc & PROC_RDPMC) && !(!(c->cr4 & CR4_PCE) && c->cpl > 0 && pmode(c))) exit_insn(c, 15, 0);
        return;
    case I_RDTSC:
        if ((v->proc & PROC_RDTSC) && !((c->cr4 & CR4_TSD) && c->cpl > 0 && pmode(c))) exit_insn(c, 16, 0);
        return;
    case I_RSM: raise_exc(c, EXC_UD, 0, NULL);
    case I_VMXON: case I_VMCLEAR: case I_VMPTRLD: case I_VMPTRST: case I_VMREAD: case I_VMWRITE:
    case I_INVEPT: case I_INVVPID: {
        static const struct { uint16_t id; uint8_t reason; } r[] = {
            {I_VMXON, 27}, {I_VMCLEAR, 19}, {I_VMPTRLD, 21}, {I_VMPTRST, 22}, {I_VMREAD, 23},
            {I_VMWRITE, 25}, {I_INVEPT, 50}, {I_INVVPID, 53}};
        if (vmx_ud) raise_exc(c, EXC_UD, 0, NULL);
        if ((in->id == I_VMREAD || in->id == I_VMWRITE) && shadowed(c, in)) return;
        for (size_t i = 0; i < sizeof r / sizeof r[0]; i++)
            if (r[i].id == in->id) exit_info(c, r[i].reason, in->mod == 3 ? 0 : (uint64_t)in->disp, insn_info(in));
        return;
    }
    case I_MOVCR: if (priv) cr_exit(c, in); return;
    case I_CLTS:
        if (priv && (v->cr0_mask & CR0_TS) && (v->cr0_shadow & CR0_TS)) exit_insn(c, 28, 2 << 4);
        return;
    case I_LMSW: {
        if (!priv) return;
        uint64_t src = in->mod == 3 ? c->r[in->rm] & 0xFFFF : rdm(c, ea_linear(c, in), 2);
        uint64_t m = v->cr0_mask & 0xF, sh = v->cr0_shadow;
        if (((m & 0xE) & (src ^ sh)) || ((m & 1) && !(sh & 1) && (src & 1))) {
            uint64_t q = 3 << 4 | (in->mod != 3 ? 64 : (uint64_t)(in->rm & 15) << 8) | (src & 0xFFFF) << 16;
            pend(c, 28, q);
            if (in->mod != 3) v->x_gla = ea_linear(c, in);
            vmx_raise(c);
        }
        return;
    }
    case I_MOVDR:
        if (priv && (v->proc & PROC_MOVDR))
            exit_insn(c, 29, (uint64_t)(in->reg & 7) | (in->opcode == 0x21 ? 16 : 0) | (uint64_t)(in->rm & 15) << 8);
        return;
    case I_IN: case I_OUT: case I_INS: case I_OUTS: io_exit(c, in); return;
    case I_RDMSR: case I_WRMSR:
        if (priv && msr_exits(c, (uint32_t)c->r[RCX], in->id == I_WRMSR)) exit_insn(c, in->id == I_RDMSR ? 31 : 32, 0);
        return;
    case I_PAUSE:
        if (v->proc & PROC_PAUSE) exit_insn(c, 40, 0);
        if ((v->proc2 & SEC_PLE) && c->cpl == 0) { /* a PAUSE loop: PAUSEs at most PLE_Gap apart, for over PLE_Window */
            uint64_t now = tsc_now(c);
            if (v->ple_last == ~0ULL || now - v->ple_last > v->ple_gap) v->ple_first = now;
            v->ple_last = now;
            if (now - v->ple_first > v->ple_window) exit_insn(c, 40, 0);
        }
        return;
    case I_RDRAND:
        if (v->proc2 & SEC_RDRAND) exit_info(c, 57, 0, (uint32_t)(in->rm & 15) << 3 | (uint32_t)(in->opsize >> 2) << 11);
        return;
    case I_RDSEED:
        if (in->rep == 0xF3) { /* RDPID */
            if (!(v->proc2 & SEC_RDTSCP)) raise_exc(c, EXC_UD, 0, NULL);
            return;
        }
        if (v->proc2 & SEC_RDSEED) exit_info(c, 61, 0, (uint32_t)(in->rm & 15) << 3 | (uint32_t)(in->opsize >> 2) << 11);
        return;
    case I_SGDT: case I_SIDT: case I_LGDT: case I_LIDT:
        if ((v->proc2 & SEC_DESC) && (priv || in->id == I_SGDT || in->id == I_SIDT)) {
            uint32_t ident = in->id == I_SGDT ? 0 : in->id == I_SIDT ? 1 : in->id == I_LGDT ? 2 : 3;
            exit_info(c, 46, (uint64_t)in->disp, (insn_info(in) & ~0xF0000000u) | ident << 28 | (in->opsize == 4) << 11);
        }
        return;
    case I_SLDT: case I_STR: case I_LLDT: case I_LTR:
        if ((v->proc2 & SEC_DESC) && pmode(c) && !(c->rflags & FL_VM) &&
            (priv || in->id == I_SLDT || in->id == I_STR)) {
            uint32_t ident = in->id == I_SLDT ? 0 : in->id == I_STR ? 1 : in->id == I_LLDT ? 2 : 3;
            exit_info(c, 47, in->mod == 3 ? 0 : (uint64_t)in->disp, (insn_info(in) & ~0xF0000000u) | ident << 28);
        }
        return;
    case I_GRP7R:
        switch (in->modrm) {
        case 0xC1: exit_insn(c, 18, 0); /* VMCALL */
        case 0xC2: case 0xC3: case 0xC4:
            if (vmx_ud) raise_exc(c, EXC_UD, 0, NULL);
            exit_insn(c, in->modrm == 0xC2 ? 20 : in->modrm == 0xC3 ? 24 : 26, 0);
        case 0xD4: return; /* VMFUNC: executed by exec_vmx */
        case 0xC8: if (priv && (v->proc & PROC_MONITOR)) exit_insn(c, 39, 0); return;
        case 0xC9: if (priv && (v->proc & PROC_MWAIT)) exit_insn(c, 36, 0); return;
        case 0xD1: exit_insn(c, 55, 0); /* XSETBV */
        case 0xF9: /* RDTSCP */
            if (!(v->proc2 & SEC_RDTSCP)) raise_exc(c, EXC_UD, 0, NULL);
            if ((v->proc & PROC_RDTSC) && !((c->cr4 & CR4_TSD) && c->cpl > 0 && pmode(c))) exit_insn(c, 51, 0);
            return;
        }
        return;
    }
}

/* An exception in the guest: does the exception bitmap make it exit? type
 * is the VMX event type (3 hardware exception, 5 INT1, 6 INT3/INTO). */
int vmx_exc_exits(CPU *c, int vec, int type, int has_err, uint64_t err, uint64_t qual) {
    Vmx *v = &c->vmx;
    int bit = (v->exc_bitmap >> vec) & 1;
    if (vec == EXC_PF && ((uint32_t)err & v->pf_mask) != v->pf_match) bit = !bit;
    if (!bit) return 0;
    pend(c, 0, qual);
    v->x_intinfo = (uint32_t)vec | (uint32_t)type << 8 | (has_err ? 1u << 11 : 0) | 1u << 31;
    v->x_interr = has_err ? (uint32_t)err : 0;
    return 1;
}

/* The event sys_deliver is about to deliver in the guest (IDT vectoring). */
void vmx_delivering(CPU *c, int vec, int kind, int has_err, uint64_t err, uint64_t ret) {
    static const uint8_t types[] = {[EV_EXT] = 0, [EV_EXC] = 3, [EV_SOFT] = 4, [EV_SOFTEXC] = 6, [EV_ICEBP] = 5};
    Vmx *v = &c->vmx;
    int type = kind == EV_EXT && c->delivering == 2 ? 2 : types[kind];
    v->vec_info = (uint32_t)vec | (uint32_t)type << 8 | (has_err ? 1u << 11 : 0) | 1u << 31;
    v->vec_err = has_err ? (uint32_t)err : 0;
    v->vec_len = (uint32_t)(ret - c->rip);
    v->in_delivery = 1;
}

/* Hardware task switch in the guest. source: 0 CALL, 1 IRET, 2 JMP, 3 IDT task gate. */
void vmx_task_switch(CPU *c, uint16_t sel, int source) {
    pend(c, 9, sel | (uint64_t)source << 30);
    vmx_raise(c);
}

/* Posted-interrupt processing: clear the outstanding-notification bit,
 * dismiss the notification at the local APIC, and move the posted-interrupt
 * requests into VIRR and RVI. */
static void posted_interrupts(CPU *c) {
    Vmx *v = &c->vmx;
    uint32_t pir[8], zero[8] = {0};
    uint8_t on = 0;
    phys_read(c, v->pi_desc + 32, &on, 1);
    on &= (uint8_t)~1u;
    phys_write(c, v->pi_desc + 32, &on, 1);
    if (c->eoi_hook) c->eoi_hook(c);
    phys_read(c, v->pi_desc, pir, 32);
    phys_write(c, v->pi_desc, zero, 32);
    for (uint32_t i = 0; i < 8; i++) {
        if (!pir[i]) continue;
        vapic_wr(c, 0x200 + i * 16, vapic_rd(c, 0x200 + i * 16) | pir[i]);
        uint32_t top = i * 32 + 31 - (uint32_t)__builtin_clz(pir[i]);
        if (top > v->rvi) v->rvi = top;
    }
}

/* Exits at an instruction boundary, in priority order. shadow: blocking by
 * STI or MOV SS. Returns 1 when a VM exit happened. */
int vmx_boundary(CPU *c, int shadow) {
    Vmx *v = &c->vmx;
    if (v->trap_reason) { /* TPR below threshold right after VM entry */
        c->intr_shadow = shadow;
        return vmx_trap(c);
    }
    if (v->init_pending) {
        v->init_pending = 0;
        c->intr_shadow = shadow;
        pend(c, 3, 0);
        vmx_exit(c);
        return 1;
    }
    if ((v->pin & PIN_PREEMPT) && tsc_now(c) >= v->timer_deadline) {
        c->intr_shadow = shadow;
        pend(c, 52, 0);
        vmx_exit(c);
        return 1;
    }
    if (c->nmi_pending && (v->pin & PIN_NMI) && ((v->pin & PIN_VNMI) || !c->nmi_blocked) && !(shadow && !(c->rflags & FL_IF))) {
        c->nmi_pending = 0;
        c->intr_shadow = shadow;
        pend(c, 0, 0);
        v->x_intinfo = 2 | 2 << 8 | 1u << 31;
        vmx_exit(c);
        return 1;
    }
    if ((v->proc & PROC_NMIWIN) && !c->nmi_blocked && !shadow) {
        c->intr_shadow = shadow;
        pend(c, 8, 0);
        vmx_exit(c);
        return 1;
    }
    if (c->irq_line && (v->pin & PIN_EXTINT)) {
        int vec = -1;
        if (v->exit_ctl & EXIT_ACK) {
            vec = c->get_interrupt ? c->get_interrupt(c) : -1;
            if (vec < 0) goto window;
            if ((v->pin & PIN_POSTED) && (uint32_t)vec == v->pi_vector) { /* a notification: no exit */
                posted_interrupts(c);
                goto window;
            }
        }
        c->intr_shadow = shadow;
        pend(c, 1, 0);
        if (vec >= 0) v->x_intinfo = (uint32_t)vec | 1u << 31;
        vmx_exit(c);
        return 1;
    }
window:
    if ((v->proc & PROC_INTWIN) && (c->rflags & FL_IF) && !shadow) {
        c->intr_shadow = shadow;
        pend(c, 7, 0);
        vmx_exit(c);
        return 1;
    }
    if ((c->rflags & FL_IF) && !shadow && vint_pending(c)) { /* virtual-interrupt delivery */
        uint32_t vec = v->rvi;
        vbit(c, 0x100, vec, 1);
        v->svi = vec;
        vapic_wr(c, 0xA0, vec & 0xF0);
        vbit(c, 0x200, vec, 0);
        v->rvi = vhighest(c, 0x200);
        c->halted = 0;
        c->delivering = 0x100;
        sys_deliver(c, (int)vec, EV_EXT, 0, 0, c->rip);
        c->delivering = -1;
        return 1;
    }
    return 0;
}

/* A pending trap-like exit (after an instruction, or right after VM entry). */
int vmx_trap(CPU *c) {
    Vmx *v = &c->vmx;
    if (!v->trap_reason) return 0;
    uint32_t reason = v->trap_reason;
    v->trap_reason = 0;
    pend(c, reason, v->trap_qual);
    vmx_exit(c);
    return 1;
}

/* After a guest instruction completes: the monitor trap flag. */
void vmx_after_insn(CPU *c) {
    if (c->vmx.proc & PROC_MTF) {
        pend(c, 37, 0);
        vmx_exit(c);
    }
}

/* MOV to CR0/CR4 in the guest: bits owned by the host keep their value. */
uint64_t vmx_cr_write(CPU *c, int n, uint64_t v) {
    uint64_t mask = n == 0 ? c->vmx.cr0_mask : c->vmx.cr4_mask, cur = n == 0 ? c->cr0 : c->cr4;
    return (v & ~mask) | (cur & mask);
}

/* MOV from CR0/CR4 (and SMSW) in the guest: owned bits come from the read shadow. */
uint64_t vmx_cr_read(CPU *c, int n) {
    uint64_t mask = n == 0 ? c->vmx.cr0_mask : c->vmx.cr4_mask;
    uint64_t sh = n == 0 ? c->vmx.cr0_shadow : c->vmx.cr4_shadow, cur = n == 0 ? c->cr0 : c->cr4;
    return (cur & ~mask) | (sh & mask);
}

/* CR8 with the TPR shadow: the virtual TPR. */
uint64_t vmx_cr8_read(CPU *c) { return (vapic_rd(c, 0x80) >> 4) & 15; }

void vmx_cr8_write(CPU *c, uint64_t v) {
    vapic_wr(c, 0x80, (uint32_t)(v & 15) << 4);
    tpr_virtualize(c);
}

/* APIC registers that APIC-register virtualization reads and writes. */
static int apic_readable(uint32_t r) {
    switch (r) {
    case 0x20: case 0x30: case 0x80: case 0xB0: case 0xD0: case 0xE0: case 0xF0: case 0x280: case 0x380: case 0x3E0:
        return 1;
    }
    return (r >= 0x100 && r <= 0x270) || (r >= 0x2F0 && r <= 0x370);
}

static int apic_writable(uint32_t r) {
    switch (r) {
    case 0x20: case 0x80: case 0xB0: case 0xD0: case 0xE0: case 0xF0: case 0x280: case 0x380: case 0x3E0:
        return 1;
    }
    return r >= 0x2F0 && r <= 0x370;
}

/* After a virtualized write to the APIC-access page. */
static void apic_write_emulate(CPU *c, uint32_t reg) {
    Vmx *v = &c->vmx;
    if (reg == 0x80) {
        vapic_wr(c, 0x80, vapic_rd(c, 0x80) & 0xFF);
        tpr_virtualize(c);
        return;
    }
    if (reg == 0xB0 && (v->proc2 & SEC_VID)) {
        vapic_wr(c, 0xB0, 0);
        eoi_virtualize(c);
        return;
    }
    if (reg == 0x300 && (v->proc2 & SEC_VID)) { /* a fixed self IPI is virtualized */
        uint32_t icr = vapic_rd(c, 0x300);
        if ((icr & 0xFFFF2700u) == 0x40000u && (icr & 0xF0)) {
            self_ipi_virtualize(c, icr & 0xFF);
            return;
        }
    }
    v->trap_reason = 56; /* APIC write */
    v->trap_qual = reg;
}

/* A guest access to the APIC-access page: virtualized through the
 * virtual-APIC page, or an APIC-access exit (qualification: offset, type). */
void vmx_apic_access(CPU *c, uint64_t pa, void *buf, int n, int type) {
    Vmx *v = &c->vmx;
    uint32_t off = (uint32_t)pa & 0xFFF, reg = off & 0xFF0;
    int inside = (off & 15) + (uint32_t)n <= 4; /* within one 32-bit register */
    if (inside && type == 0 &&
        ((v->proc2 & SEC_APICREG) ? apic_readable(reg) : reg == 0x80 && (v->proc & PROC_TPR))) {
        phys_read(c, v->vapic + off, buf, n);
        return;
    }
    if (inside && type == 1 &&
        ((reg == 0x80 && (v->proc & PROC_TPR)) || ((reg == 0xB0 || reg == 0x300) && (v->proc2 & SEC_VID)) ||
         ((v->proc2 & SEC_APICREG) && apic_writable(reg)))) {
        phys_write(c, v->vapic + off, buf, n);
        apic_write_emulate(c, reg);
        return;
    }
    exit_insn(c, 44, off | (uint64_t)type << 12);
}

/* RDMSR/WRMSR of x2APIC MSRs with "virtualize x2APIC mode". */
int vmx_x2apic_msr(CPU *c, uint32_t idx, uint64_t *val, int write) {
    Vmx *v = &c->vmx;
    uint32_t reg = (idx & 0xFF) << 4;
    if (!write) {
        if (!(v->proc2 & SEC_APICREG) && idx != 0x808) return 0;
        *val = 0;
        phys_read(c, v->vapic + reg, val, 8);
        return 1;
    }
    switch (idx) {
    case 0x808: /* TPR */
        if (*val >> 8) return -1;
        vapic_wr(c, 0x80, (uint32_t)*val);
        vapic_wr(c, 0x84, 0);
        tpr_virtualize(c);
        return 1;
    case 0x80B: /* EOI */
        if (!(v->proc2 & SEC_VID)) return 0;
        if (*val) return -1;
        eoi_virtualize(c);
        return 1;
    case 0x83F: /* SELF IPI */
        if (!(v->proc2 & SEC_VID)) return 0;
        if (*val >> 8) return -1;
        vapic_wr(c, 0x3F0, (uint32_t)*val);
        self_ipi_virtualize(c, (uint32_t)*val);
        return 1;
    }
    return 0;
}

/* Page-modification logging: an EPT dirty flag is being set. */
void vmx_pml_log(CPU *c, uint64_t gpa) {
    Vmx *v = &c->vmx;
    if (v->pml_index > 511) exit_insn(c, 62, 0); /* log full */
    uint64_t e = gpa & ~0xFFFULL;
    phys_write(c, v->pml_addr + v->pml_index * 8, &e, 8);
    v->pml_index = (v->pml_index - 1) & 0xFFFF;
}

/* The guest's TSC: scaled, then offset. */
uint64_t vmx_guest_tsc(CPU *c, uint64_t t) {
    Vmx *v = &c->vmx;
    if (!(v->proc & PROC_TSCOFF)) return t;
    if (v->proc2 & SEC_TSCSCALE) t = (uint64_t)(((unsigned __int128)t * v->tsc_mult) >> 48);
    return t + v->tsc_offset;
}
