/* Local APICs (xAPIC MMIO at 0xFEE00000, one per CPU, APIC ID = CPU index;
 * timer in one-shot, periodic and TSC-deadline modes; IPIs with fixed,
 * lowest-priority, NMI, INIT and STARTUP delivery to physical or logical
 * destinations) and an 82093AA-style I/O APIC (24 pins at 0xFEC00000).
 * APIC IDs are 8 bits and 0xFF is the broadcast destination, so a machine
 * has at most 255 CPUs; flat logical destinations reach the first 8,
 * cluster destinations 60 (15 clusters of 4).
 * The APIC bus clock is 1 GHz, the same as the TSC and the crystal reported
 * by CPUID leaf 0x15. */
#include "pc_internal.h"

#include <string.h>

enum { LVT_CMCI, LVT_TIMER, LVT_THERMAL, LVT_PERF, LVT_LINT0, LVT_LINT1, LVT_ERROR };
#define LVT_MASKED 0x10000u

static int ncpu(PC *pc) { return pc->m->ncpu; }
static CPU *cpu_at(PC *pc, int i) { return pc->m->cpus[i]; }

int lapic_enabled(PC *pc, int i) { return (cpu_at(pc, i)->msr.apic_base & 0x800) != 0; }
static int sw_enabled(Lapic *a) { return (a->svr & 0x100) != 0; }
static int lapic_on(PC *pc, int i) { return lapic_enabled(pc, i) && sw_enabled(&pc->lapic[i]); }

static int highest(const uint32_t *v) {
    for (int i = 7; i >= 0; i--)
        if (v[i]) return i * 32 + 31 - __builtin_clz(v[i]);
    return -1;
}
static void set_bit(uint32_t *v, int n) { v[n >> 5] |= 1u << (n & 31); }
static void clr_bit(uint32_t *v, int n) { v[n >> 5] &= ~(1u << (n & 31)); }
static int get_bit(const uint32_t *v, int n) { return (v[n >> 5] >> (n & 31)) & 1; }

static uint32_t ppr(Lapic *a) {
    int isrv = highest(a->isr);
    if (isrv < 0) isrv = 0;
    return (a->tpr >> 4) >= ((uint32_t)isrv >> 4) ? a->tpr & 0xFF : (uint32_t)isrv & 0xF0;
}

int lapic_pending(PC *pc, int i) {
    Lapic *a = &pc->lapic[i];
    if (!lapic_on(pc, i)) return 0;
    int v = highest(a->irr);
    return v >= 16 && ((uint32_t)v & 0xF0) > (ppr(a) & 0xF0);
}

int lapic_ack(PC *pc, int i) {
    Lapic *a = &pc->lapic[i];
    int v = highest(a->irr);
    if (v < 16 || ((uint32_t)v & 0xF0) <= (ppr(a) & 0xF0)) return a->svr & 0xFF; /* spurious */
    clr_bit(a->irr, v);
    set_bit(a->isr, v);
    return v;
}

/* The 8259 reaches a CPU through LINT0 (virtual wire); with its APIC off,
 * the INTR pin of the bootstrap processor takes it directly. */
int lapic_accepts_pic(PC *pc, int i) {
    if (!lapic_on(pc, i)) return i == 0;
    uint32_t l = pc->lapic[i].lvt[LVT_LINT0];
    return !(l & LVT_MASKED) && ((l >> 8) & 7) == 7;
}

void lapic_accept(PC *pc, int i, int vector, int level) {
    Lapic *a = &pc->lapic[i];
    if (!lapic_enabled(pc, i) || vector < 16) return;
    set_bit(a->irr, vector);
    if (level) set_bit(a->tmr, vector);
    else clr_bit(a->tmr, vector);
    pc_update_irq(pc);
}

static void ioapic_eoi(PC *pc, int vector);

static void lapic_eoi(PC *pc, int i) {
    Lapic *a = &pc->lapic[i];
    int v = highest(a->isr);
    if (v < 0) return;
    clr_bit(a->isr, v);
    if (get_bit(a->tmr, v)) ioapic_eoi(pc, v);
    pc_update_irq(pc);
}

/* ---- message delivery (IPIs and I/O APIC interrupts) ---- */

/* Does CPU i match an APIC destination? Logical mode uses the flat model
 * (DFR 0xF: LDR bit mask) or the cluster model (DFR 0x0). */
static int dest_match(PC *pc, int i, uint32_t dest, int logical) {
    Lapic *a = &pc->lapic[i];
    if (!logical) return dest == 0xFF || dest == a->id;
    uint32_t ldr = a->ldr >> 24;
    if (dest == 0xFF) return 1;
    if ((a->dfr >> 28) == 0xF) return (ldr & dest) != 0;
    return (ldr >> 4) == (dest >> 4) && (ldr & dest & 0xF) != 0;
}

/* Deliver to the CPUs with tgt[i] set. mode: 0 fixed, 1 lowest priority,
 * 4 NMI, 5 INIT, 6 STARTUP. */
static void deliver(PC *pc, const uint8_t *tgt, int mode, int vector, int level) {
    int best = -1;
    if (mode == 1) { /* lowest priority: the enabled target with the lowest TPR */
        for (int i = 0; i < ncpu(pc); i++)
            if (tgt[i] && lapic_on(pc, i) && (best < 0 || pc->lapic[i].tpr < pc->lapic[best].tpr)) best = i;
        if (best < 0) return;
        mode = 0;
    }
    for (int i = 0; i < ncpu(pc); i++) {
        if (best >= 0 ? i != best : !tgt[i]) continue;
        switch (mode) {
        case 0: lapic_accept(pc, i, vector, level); break;
        case 4: cpu_at(pc, i)->nmi_pending = 1; break;
        case 5: pc_cpu_init(pc, i); break;
        case 6: pc_cpu_sipi(pc, i, vector); break;
        }
    }
}

/* A performance counter overflowed with its interrupt enabled: deliver the
 * LVT performance entry (fixed or NMI) and mask it, as the hardware does. */
void lapic_pmi(PC *pc, int i) {
    Lapic *a = &pc->lapic[i];
    uint32_t l = a->lvt[LVT_PERF];
    if (!lapic_on(pc, i) || (l & LVT_MASKED)) return;
    if (((l >> 8) & 7) == 4) cpu_at(pc, i)->nmi_pending = 1;
    else lapic_accept(pc, i, l & 0xFF, 0);
    a->lvt[LVT_PERF] = l | LVT_MASKED;
    pc_update_irq(pc);
}

/* ---- timer ---- */

static uint64_t timer_div(Lapic *a) {
    uint32_t v = (a->div_conf & 3) | ((a->div_conf & 8) >> 1);
    return v == 7 ? 1 : 2ULL << v;
}
static int timer_mode(Lapic *a) { return (a->lvt[LVT_TIMER] >> 17) & 3; }

static uint32_t timer_current(PC *pc, Lapic *a) {
    if (!a->init_count || timer_mode(a) == 2) return 0;
    uint64_t ticks = (pc_now(pc) - a->timer_start) / timer_div(a);
    if (timer_mode(a) == 1) return a->init_count - (uint32_t)(ticks % a->init_count);
    return ticks >= a->init_count ? 0 : a->init_count - (uint32_t)ticks;
}

static uint64_t timer_next(PC *pc, int i) {
    Lapic *a = &pc->lapic[i];
    if (!lapic_enabled(pc, i) || !a->timer_armed) return NS_NEVER;
    if (timer_mode(a) == 2) return a->deadline;
    uint64_t period = (uint64_t)a->init_count * timer_div(a);
    return a->timer_start + period * (timer_mode(a) == 1 ? a->timer_k + 1 : 1);
}

uint64_t lapic_next_event(PC *pc) {
    uint64_t t = NS_NEVER;
    for (int i = 0; i < ncpu(pc); i++) {
        uint64_t n = timer_next(pc, i);
        if (n < t) t = n;
    }
    return t;
}

void lapic_tick(PC *pc, uint64_t now) {
    for (int i = 0; i < ncpu(pc); i++) {
        Lapic *a = &pc->lapic[i];
        if (timer_next(pc, i) > now) continue;
        if (timer_mode(a) == 1) {
            uint64_t period = (uint64_t)a->init_count * timer_div(a);
            a->timer_k = (now - a->timer_start) / period;
        } else {
            a->timer_armed = 0;
            if (timer_mode(a) == 2) a->deadline = 0;
        }
        uint32_t l = a->lvt[LVT_TIMER];
        if (!(l & LVT_MASKED)) lapic_accept(pc, i, l & 0xFF, 0);
    }
}

static void timer_start(PC *pc, Lapic *a) {
    a->timer_start = pc_now(pc);
    a->timer_k = 0;
    a->timer_armed = a->init_count != 0 && timer_mode(a) != 2;
    pc_resched(pc);
}

/* ---- registers ---- */

static int lvt_index(uint32_t off) {
    switch (off) {
    case 0x2F0: return LVT_CMCI;
    case 0x320: return LVT_TIMER;
    case 0x330: return LVT_THERMAL;
    case 0x340: return LVT_PERF;
    case 0x350: return LVT_LINT0;
    case 0x360: return LVT_LINT1;
    case 0x370: return LVT_ERROR;
    }
    return -1;
}

static uint32_t lapic_read(PC *pc, int i, uint32_t off) {
    Lapic *a = &pc->lapic[i];
    int l = lvt_index(off);
    if (l >= 0) return a->lvt[l];
    if (off >= 0x100 && off < 0x180) return a->isr[(off - 0x100) >> 4];
    if (off >= 0x180 && off < 0x200) return a->tmr[(off - 0x180) >> 4];
    if (off >= 0x200 && off < 0x280) return a->irr[(off - 0x200) >> 4];
    switch (off) {
    case 0x20: return a->id << 24;
    case 0x30: return 0x00060014; /* version 0x14, 7 LVT entries */
    case 0x80: return a->tpr;
    case 0xA0: return ppr(a);
    case 0xD0: return a->ldr;
    case 0xE0: return a->dfr;
    case 0xF0: return a->svr;
    case 0x280: return a->esr;
    case 0x300: return a->icr_lo & ~(1u << 12); /* delivery is immediate: never "send pending" */
    case 0x310: return a->icr_hi;
    case 0x380: return a->init_count;
    case 0x390: return timer_current(pc, a);
    case 0x3E0: return a->div_conf;
    }
    return 0;
}

static void send_ipi(PC *pc, int src) {
    Lapic *a = &pc->lapic[src];
    uint32_t lo = a->icr_lo;
    int shorthand = (lo >> 18) & 3, mode = (lo >> 8) & 7;
    uint8_t tgt[MAX_CPUS];
    if (mode == 5 && !(lo & (1u << 14)) && (lo & (1u << 15))) return; /* INIT level de-assert */
    for (int i = 0; i < ncpu(pc); i++) {
        switch (shorthand) {
        case 0: tgt[i] = (uint8_t)dest_match(pc, i, a->icr_hi >> 24, (lo & 0x800) != 0); break;
        case 1: tgt[i] = i == src; break;
        case 2: tgt[i] = 1; break;
        default: tgt[i] = i != src; break;
        }
    }
    if (mode == 5 || mode == 6) tgt[0] = 0; /* INIT/STARTUP never reset the bootstrap processor */
    deliver(pc, tgt, mode, (int)(lo & 0xFF), 0);
}

static void lapic_write(PC *pc, int i, uint32_t off, uint32_t v) {
    Lapic *a = &pc->lapic[i];
    int l = lvt_index(off);
    if (l >= 0) {
        int old_mode = timer_mode(a);
        if (!sw_enabled(a)) v |= LVT_MASKED;
        a->lvt[l] = v;
        if (l == LVT_TIMER && timer_mode(a) != old_mode) {
            a->timer_armed = 0;
            a->deadline = 0;
            pc_resched(pc);
        }
        pc_update_irq(pc);
        return;
    }
    switch (off) {
    case 0x20: a->id = v >> 24; break;
    case 0x80:
        a->tpr = v & 0xFF;
        cpu_at(pc, i)->cr8 = a->tpr >> 4;
        break;
    case 0xB0: lapic_eoi(pc, i); break;
    case 0xD0: a->ldr = v & 0xFF000000; break;
    case 0xE0: a->dfr = v | 0x0FFFFFFF; break;
    case 0xF0:
        a->svr = v & 0x13FF;
        if (!(v & 0x100))
            for (int k = 0; k < 7; k++) a->lvt[k] |= LVT_MASKED;
        break;
    case 0x280: a->esr = 0; break;
    case 0x300:
        a->icr_lo = v;
        send_ipi(pc, i);
        break;
    case 0x310: a->icr_hi = v; break;
    case 0x380:
        a->init_count = v;
        if (timer_mode(a) != 2) timer_start(pc, a);
        break;
    case 0x3E0: a->div_conf = v & 0xB; break;
    }
    pc_update_irq(pc);
}

void apic_tpr(CPU *c, int tpr) {
    PC *pc = ((Machine *)c->user)->pc;
    pc->lapic[c->index].tpr = (uint32_t)(tpr & 15) << 4;
    pc_update_irq(pc);
}

int apic_msr(CPU *c, uint32_t msr, uint64_t *val, int write) {
    PC *pc = ((Machine *)c->user)->pc;
    Lapic *a = &pc->lapic[c->index];
    switch (msr) {
    case 0x1B:
        if (!write) {
            *val = c->msr.apic_base;
            return 1;
        }
        c->msr.apic_base = (*val & 0xFFFFFF000ULL) | (*val & 0x800) | (c->msr.apic_base & 0x100);
        pc_update_irq(pc);
        return 1;
    case 0x6E0: /* IA32_TSC_DEADLINE */
        if (!write) {
            *val = a->deadline ? a->deadline + c->tsc_offset : 0;
            return 1;
        }
        a->deadline = *val ? *val - c->tsc_offset : 0;
        if (timer_mode(a) == 2) {
            a->timer_armed = a->deadline != 0;
            pc_resched(pc);
        }
        return 1;
    }
    return 0;
}

/* ---- I/O APIC ---- */

#define RE_LEVEL (1ULL << 15)
#define RE_REMOTE_IRR (1ULL << 14)
#define RE_MASKED (1ULL << 16)
#define RE_LOGICAL (1ULL << 11)

static void ioapic_deliver(PC *pc, int pin) {
    uint64_t e = pc->ioapic.redir[pin];
    int mode = (int)(e >> 8) & 7;
    if (mode == 7) return; /* ExtINT: the 8259 path already carries it */
    uint8_t tgt[MAX_CPUS];
    for (int i = 0; i < ncpu(pc); i++) tgt[i] = (uint8_t)dest_match(pc, i, (uint32_t)(e >> 56), (e & RE_LOGICAL) != 0);
    if (mode == 0 || mode == 1 || mode == 4) deliver(pc, tgt, mode, (int)(e & 0xFF), (e & RE_LEVEL) != 0);
}

static void ioapic_service(PC *pc, int pin) {
    Ioapic *io = &pc->ioapic;
    uint64_t *e = &io->redir[pin];
    if ((*e & RE_MASKED) || !(io->lines >> pin & 1)) return;
    if (*e & RE_LEVEL) {
        if (*e & RE_REMOTE_IRR) return;
        *e |= RE_REMOTE_IRR;
    }
    ioapic_deliver(pc, pin);
}

void ioapic_set_irq(PC *pc, int pin, int level) {
    Ioapic *io = &pc->ioapic;
    if (pin < 0 || pin >= 24) return;
    uint32_t bit = 1u << pin, old = io->lines & bit;
    io->lines = level ? io->lines | bit : io->lines & ~bit;
    if (!level) return;
    if (io->redir[pin] & RE_LEVEL) ioapic_service(pc, pin);
    else if (!old) ioapic_service(pc, pin);
}

static void ioapic_eoi(PC *pc, int vector) {
    Ioapic *io = &pc->ioapic;
    for (int i = 0; i < 24; i++) {
        uint64_t *e = &io->redir[i];
        if ((*e & RE_LEVEL) && (*e & RE_REMOTE_IRR) && (int)(*e & 0xFF) == vector) {
            *e &= ~RE_REMOTE_IRR;
            ioapic_service(pc, i);
        }
    }
}

static uint32_t ioapic_read(PC *pc) {
    Ioapic *io = &pc->ioapic;
    uint32_t r = io->sel & 0xFF;
    if (r == 0 || r == 2) return io->id << 24;
    if (r == 1) return 0x00170011;
    if (r >= 0x10 && r < 0x40) {
        uint64_t e = io->redir[(r - 0x10) >> 1];
        return (r & 1) ? (uint32_t)(e >> 32) : (uint32_t)e;
    }
    return 0;
}

static void ioapic_write(PC *pc, uint32_t v) {
    Ioapic *io = &pc->ioapic;
    uint32_t r = io->sel & 0xFF;
    if (r == 0) { io->id = (v >> 24) & 15; return; }
    if (r < 0x10 || r >= 0x40) return;
    int pin = (int)(r - 0x10) >> 1;
    uint64_t *e = &io->redir[pin];
    if (r & 1) *e = (*e & 0xFFFFFFFFULL) | (uint64_t)v << 32;
    else *e = (*e & (0xFFFFFFFF00000000ULL | RE_REMOTE_IRR)) | (v & ~(uint32_t)((1u << 12) | (1u << 14)));
    if (!(*e & RE_LEVEL)) *e &= ~RE_REMOTE_IRR;
    if (*e & RE_LEVEL) ioapic_service(pc, pin);
}

/* ---- MMIO and reset ---- */

int apic_mmio(CPU *c, uint64_t pa, void *buf, int n, int write) {
    PC *pc = ((Machine *)c->user)->pc;
    uint32_t v = 0;
    if (pa >= LAPIC_BASE && pa < LAPIC_BASE + 0x1000 && lapic_enabled(pc, c->index)) {
        uint32_t off = (uint32_t)(pa & 0xFF0), sh = (uint32_t)(pa & 3) * 8;
        if (write) {
            memcpy(&v, buf, n < 4 ? (size_t)n : 4);
            if ((pa & 0xF) == 0) lapic_write(pc, c->index, off, v);
        } else {
            uint64_t r = lapic_read(pc, c->index, off) >> sh;
            memset(buf, 0, (size_t)n);
            memcpy(buf, &r, n < 4 ? (size_t)n : 4);
        }
        return 1;
    }
    if (pa >= IOAPIC_BASE && pa < IOAPIC_BASE + 0x1000) {
        uint32_t off = (uint32_t)(pa & 0xFF);
        if (write) {
            memcpy(&v, buf, n < 4 ? (size_t)n : 4);
            if (off == 0) pc->ioapic.sel = v;
            else if (off == 0x10) ioapic_write(pc, v);
        } else {
            uint32_t r = off == 0 ? pc->ioapic.sel : off == 0x10 ? ioapic_read(pc) : 0;
            memset(buf, 0, (size_t)n);
            memcpy(buf, &r, n < 4 ? (size_t)n : 4);
        }
        return 1;
    }
    return 0;
}

/* Power-on / INIT state of one local APIC (the ID survives). */
void lapic_reset(PC *pc, int i) {
    Lapic *a = &pc->lapic[i];
    memset(a, 0, sizeof *a);
    a->id = (uint32_t)i;
    a->dfr = 0xFFFFFFFF;
    a->svr = 0xFF;
    for (int k = 0; k < 7; k++) a->lvt[k] = LVT_MASKED;
}

void apic_reset(PC *pc) {
    for (int i = 0; i < ncpu(pc); i++) {
        lapic_reset(pc, i);
        cpu_at(pc, i)->msr.apic_base = LAPIC_BASE | 0x800 | (i == 0 ? 0x100 : 0); /* enabled; BSP flag */
    }
    memset(&pc->ioapic, 0, sizeof pc->ioapic);
    pc->ioapic.id = (uint32_t)(ncpu(pc) < 15 ? ncpu(pc) : 15); /* after the local APIC IDs (4-bit register) */
    for (int i = 0; i < 24; i++) pc->ioapic.redir[i] = RE_MASKED;
}
