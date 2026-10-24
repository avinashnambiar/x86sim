/* PC-compatible machine (see pc.h): RAM layout, the I/O port bus, interrupt
 * routing, the virtual clock and the classic ISA devices -- dual 8259 PIC,
 * 8254 PIT, MC146818 CMOS/RTC, 8042 keyboard controller, 16550 UART, VGA
 * registers, PCI configuration space and an ATA (IDE) disk. The local APIC
 * and I/O APIC live in apic.c, the firmware in bios.c. */
#include "pc_internal.h"

#include "cache.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ---- clock and events ---- */

uint64_t pc_now(PC *pc) { return pc->m->cycles + pc->idle_ns; }
uint64_t pc_time_ns(Machine *m) { return m->pc ? pc_now(m->pc) : m->cpu.icount; }
static uint64_t tsc_read(CPU *c) { return pc_time_ns((Machine *)c->user); }

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t pit_next_event(PC *pc);
static uint64_t cmos_next_event(PC *pc);
static void pit_tick(PC *pc, uint64_t now);
static void cmos_tick(PC *pc, uint64_t now);
static void poll_input(PC *pc);
static void feed_uart(PC *pc, int polled);
static void feed_kbd(PC *pc);

static uint64_t min64(uint64_t a, uint64_t b) { return a < b ? a : b; }

void pc_resched(PC *pc) {
    uint64_t t = pit_next_event(pc);
    t = min64(t, cmos_next_event(pc));
    t = min64(t, lapic_next_event(pc));
    t = min64(t, pc->next_poll);
    pc->next_event = t;
}

void pc_tick(Machine *m) {
    PC *pc = m->pc;
    uint64_t now = pc_now(pc);
    if (now < pc->next_event) return;
    pit_tick(pc, now);
    cmos_tick(pc, now);
    lapic_tick(pc, now);
    if (now >= pc->next_poll) {
        poll_input(pc);
        pc->next_poll = now + 1000000; /* 1 ms */
    }
    pc_resched(pc);
    pc_update_irq(pc);
}

int pc_idle(Machine *m) {
    PC *pc = m->pc;
    int wakeable = 0;
    for (int i = 0; i < m->ncpu; i++) {
        CPU *c = m->cpus[i];
        if (!c->wait_sipi && ((c->rflags & FL_IF) || c->nmi_pending)) wakeable = 1;
    }
    if (!wakeable) return 0;
    uint64_t now = pc_now(pc), target = pc->next_event;
    int slept = 0;
    if (target == NS_NEVER) return 0;
    if (pc->realtime) {
        uint64_t real = mono_ns() - pc->real_start;
        if (target > real) {
            uint64_t d = min64(target - real, 10000000);
            struct timespec ts = {0, (long)d};
            nanosleep(&ts, NULL);
            slept = 1;
            real = mono_ns() - pc->real_start;
            if (real < target) target = real > now ? real : now; /* wake again soon */
        }
    }
    if (target > now) pc->idle_ns += target - now;
    return slept ? 2 : 1;
}

/* ---- interrupt routing ---- */

void pc_update_irq(PC *pc) {
    int pic = pic_output(pc);
    for (int i = 0; i < pc->m->ncpu; i++)
        pc->m->cpus[i]->irq_line = lapic_pending(pc, i) || (pic && lapic_accepts_pic(pc, i));
}

static int get_interrupt(CPU *c) {
    PC *pc = ((Machine *)c->user)->pc;
    int v = -1, i = c->index;
    if (lapic_pending(pc, i)) v = lapic_ack(pc, i);
    else if (pic_output(pc) && lapic_accepts_pic(pc, i)) v = pic_ack(pc);
    pc_update_irq(pc);
    return v;
}

static void pic_set_irq(Pic *p, int irq, int level);

/* ISA interrupt line: both the 8259s and the I/O APIC see it (IRQ 0 is wired
 * to I/O APIC pin 2, as the MP table says). */
void pc_set_irq(PC *pc, int irq, int level) {
    uint16_t bit = (uint16_t)(1u << irq);
    pc->isa_lines = level ? pc->isa_lines | bit : pc->isa_lines & ~bit;
    pic_set_irq(&pc->pic[irq >> 3], irq & 7, level);
    ioapic_set_irq(pc, irq == 0 ? 2 : irq, level);
    pc_update_irq(pc);
}

void pc_pulse_irq(PC *pc, int irq) {
    pc_set_irq(pc, irq, 1);
    pc_set_irq(pc, irq, 0);
}

void pc_set_a20(PC *pc, int on) {
    for (int i = 0; i < pc->m->ncpu; i++) pc->m->cpus[i]->a20_mask = on ? ~0ULL : ~(1ULL << 20);
}

void pc_system_reset(PC *pc) {
    Machine *m = pc->m;
    m->state = MS_EXITED;
    m->exit_code = 0;
    snprintf(m->status, sizeof m->status, "system reset requested");
    pc_cpu(pc)->stop_request = 1;
    m->last_stop = STOP_EXIT;
}

/* ---- 8259 PIC pair ---- */

static int pic_highest(Pic *p, uint8_t mask) {
    for (int i = 0; i < 8; i++) {
        int irq = (i + p->prio) & 7;
        if (mask >> irq & 1) return irq;
    }
    return -1;
}

static int pic_get(Pic *p) {
    int irq = pic_highest(p, p->irr & ~p->imr);
    if (irq < 0) return -1;
    int cur = pic_highest(p, p->isr);
    if (cur >= 0 && ((cur - p->prio) & 7) <= ((irq - p->prio) & 7)) return -1;
    return irq;
}

static void pic_set_irq(Pic *p, int irq, int level) {
    uint8_t bit = (uint8_t)(1u << irq);
    if (p->elcr & bit) p->irr = level ? p->irr | bit : p->irr & ~bit;
    else if (level && !(p->lines & bit)) p->irr |= bit;
    p->lines = level ? p->lines | bit : p->lines & ~bit;
}

static void pic_cascade(PC *pc) {
    if (pic_get(&pc->pic[1]) >= 0) pc->pic[0].irr |= 4;
    else pc->pic[0].irr &= ~4;
}

int pic_output(PC *pc) {
    pic_cascade(pc);
    return pic_get(&pc->pic[0]) >= 0;
}

static int pic_take(Pic *p, int irq) {
    uint8_t bit = (uint8_t)(1u << irq);
    if (!p->auto_eoi) p->isr |= bit;
    else p->prio = p->prio; /* rotate-in-AEOI is not modelled */
    if (!(p->elcr & bit)) p->irr &= ~bit;
    return irq;
}

int pic_ack(PC *pc) {
    Pic *m = &pc->pic[0], *s = &pc->pic[1];
    pic_cascade(pc);
    int irq = pic_get(m);
    if (irq < 0) return m->base + 7; /* spurious */
    pic_take(m, irq);
    if (irq != 2) return m->base + irq;
    int sirq = pic_get(s);
    if (sirq < 0) return s->base + 7;
    pic_take(s, sirq);
    pic_cascade(pc);
    return s->base + sirq;
}

static void pic_eoi1(Pic *p, int specific, int irq, int rotate) {
    if (!specific) irq = pic_highest(p, p->isr);
    if (irq < 0) return;
    p->isr &= ~(1u << irq);
    if (rotate) p->prio = (irq + 1) & 7;
}

void pic_eoi(PC *pc, int irq) {
    if (irq >= 8) pic_eoi1(&pc->pic[1], 0, 0, 0);
    pic_eoi1(&pc->pic[0], 0, 0, 0);
    pc_update_irq(pc);
}

static void pic_write(PC *pc, int n, int a0, uint8_t v) {
    Pic *p = &pc->pic[n];
    if (!a0) {
        if (v & 0x10) { /* ICW1 */
            p->imr = p->isr = p->irr = 0;
            p->prio = 0;
            p->read_isr = p->auto_eoi = p->poll = 0;
            p->need_icw4 = v & 1;
            p->single = (v & 2) != 0;
            p->step = 1;
        } else if (v & 0x08) { /* OCW3 */
            if (v & 4) p->poll = 1;
            if (v & 2) p->read_isr = v & 1;
        } else { /* OCW2 */
            int cmd = v >> 5;
            switch (cmd) {
            case 1: pic_eoi1(p, 0, 0, 0); break;
            case 3: pic_eoi1(p, 1, v & 7, 0); break;
            case 5: pic_eoi1(p, 0, 0, 1); break;
            case 7: pic_eoi1(p, 1, v & 7, 1); break;
            case 6: p->prio = (v + 1) & 7; break;
            }
        }
    } else {
        switch (p->step) {
        case 1:
            p->base = v & 0xF8;
            p->step = p->single ? (p->need_icw4 ? 3 : 0) : 2;
            break;
        case 2: p->step = p->need_icw4 ? 3 : 0; break;
        case 3:
            p->auto_eoi = (v & 2) != 0;
            p->step = 0;
            break;
        default: p->imr = v; break;
        }
    }
    pc_update_irq(pc);
}

static uint8_t pic_read(PC *pc, int n, int a0) {
    Pic *p = &pc->pic[n];
    if (p->poll) {
        p->poll = 0;
        if (n == 1) pic_cascade(pc);
        int irq = pic_get(p);
        if (irq < 0) return 0;
        pic_take(p, irq);
        pc_update_irq(pc);
        return (uint8_t)(0x80 | irq);
    }
    if (a0) return p->imr;
    return p->read_isr ? p->isr : p->irr;
}

/* ---- 8254 PIT ---- */

#define PIT_HZ 1193182ULL

static uint64_t pit_ticks(PC *pc, PitChan *ch) {
    return (uint64_t)((unsigned __int128)(pc_now(pc) - ch->load_ns) * PIT_HZ / 1000000000ULL);
}
static uint64_t ticks_ns(uint64_t t) { return (uint64_t)(((unsigned __int128)t * 1000000000ULL + PIT_HZ - 1) / PIT_HZ); }
static uint32_t pit_reload(PitChan *ch) { return ch->count ? ch->count : 0x10000; }

static uint16_t pit_value(PC *pc, PitChan *ch) {
    uint64_t t = pit_ticks(pc, ch);
    uint32_t r = pit_reload(ch);
    switch (ch->mode) {
    case 2: return (uint16_t)(r - t % r);
    case 3: return (uint16_t)((r - (2 * t) % r) & ~1u);
    default: return (uint16_t)(r - t);
    }
}

static int pit_out(PC *pc, PitChan *ch) {
    uint64_t t = pit_ticks(pc, ch);
    uint32_t r = pit_reload(ch);
    switch (ch->mode) {
    case 0: case 1: return t >= r;
    case 2: return t % r != r - 1;
    case 3: return t % r < (r + 1) / 2;
    default: return t != r;
    }
}

static uint64_t pit_next_event(PC *pc) {
    PitChan *ch = &pc->pit[0];
    if (!ch->armed) return NS_NEVER;
    uint32_t r = pit_reload(ch);
    if (ch->mode == 2 || ch->mode == 3) return ch->load_ns + ticks_ns((ch->irq_k + 1) * (uint64_t)r);
    return ch->load_ns + ticks_ns(r);
}

static void pit_tick(PC *pc, uint64_t now) {
    PitChan *ch = &pc->pit[0];
    int n = 0;
    while (ch->armed && pit_next_event(pc) <= now && n < 8) {
        if (ch->mode == 2 || ch->mode == 3) ch->irq_k++;
        else ch->armed = 0;
        pc_pulse_irq(pc, 0);
        n++;
    }
    if (ch->armed && pit_next_event(pc) <= now) { /* fell far behind: skip */
        uint32_t r = pit_reload(ch);
        ch->irq_k = (uint64_t)((unsigned __int128)(now - ch->load_ns) * PIT_HZ / 1000000000ULL) / r;
    }
}

static void pit_write(PC *pc, int port, uint8_t v) {
    if (port == 3) {
        int sc = v >> 6;
        if (sc == 3) { /* read-back */
            for (int i = 0; i < 3; i++) {
                if (!(v & (2 << i))) continue;
                PitChan *ch = &pc->pit[i];
                if (!(v & 0x20) && !ch->latched) { ch->latch = pit_value(pc, ch); ch->latched = 1; ch->read_hi = 0; }
                if (!(v & 0x10)) {
                    ch->status = (uint8_t)(pit_out(pc, ch) << 7 | ch->rw << 4 | ch->mode << 1 | ch->bcd);
                    ch->status_latched = 1;
                }
            }
            return;
        }
        PitChan *ch = &pc->pit[sc];
        int rw = (v >> 4) & 3;
        if (!rw) {
            if (!ch->latched) { ch->latch = pit_value(pc, ch); ch->latched = 1; ch->read_hi = 0; }
            return;
        }
        ch->rw = (uint8_t)rw;
        ch->mode = (v >> 1) & 7;
        if (ch->mode > 5) ch->mode -= 4;
        ch->bcd = v & 1;
        ch->read_hi = ch->write_hi = 0;
        ch->latched = 0;
        if (sc == 0) ch->armed = 0;
        pc_resched(pc);
        return;
    }
    PitChan *ch = &pc->pit[port];
    if (ch->rw == 1) ch->count = v;
    else if (ch->rw == 2) ch->count = (uint16_t)(v << 8);
    else if (!ch->write_hi) {
        ch->wlow = v;
        ch->write_hi = 1;
        return;
    } else {
        ch->count = (uint16_t)(ch->wlow | v << 8);
        ch->write_hi = 0;
    }
    ch->load_ns = pc_now(pc);
    ch->irq_k = 0;
    if (port == 0) ch->armed = 1;
    pc_resched(pc);
}

static uint8_t pit_read(PC *pc, int port) {
    PitChan *ch = &pc->pit[port];
    if (ch->status_latched) {
        ch->status_latched = 0;
        return ch->status;
    }
    uint16_t v = ch->latched ? ch->latch : pit_value(pc, ch);
    uint8_t r;
    if (ch->rw == 1) r = (uint8_t)v, ch->latched = 0;
    else if (ch->rw == 2) r = (uint8_t)(v >> 8), ch->latched = 0;
    else {
        r = ch->read_hi ? (uint8_t)(v >> 8) : (uint8_t)v;
        if (ch->read_hi) ch->latched = 0;
        ch->read_hi ^= 1;
    }
    return r;
}

static void port61_write(PC *pc, uint8_t v) {
    PitChan *ch = &pc->pit[2];
    int gate = v & 1;
    if (gate && !ch->gate) ch->load_ns = pc_now(pc); /* rising gate restarts the count */
    ch->gate = (uint8_t)gate;
    pc->port61 = v & 0x0F;
}

static uint8_t port61_read(PC *pc) {
    uint8_t v = pc->port61;
    if ((pc_now(pc) / 15085) & 1) v |= 0x10; /* DRAM refresh toggle */
    if (pc->pit[2].gate && pit_out(pc, &pc->pit[2])) v |= 0x20;
    return v;
}

/* ---- CMOS / RTC ---- */

static uint8_t bcd(Cmos *r, int v) { return (r->ram[0x0B] & 4) ? (uint8_t)v : (uint8_t)((v / 10) << 4 | v % 10); }

static uint64_t cmos_period(Cmos *r) {
    int rate = r->ram[0x0A] & 15;
    if (!rate) return 0;
    if (rate < 3) rate += 7;
    return 1000000000ULL * (1u << (rate - 1)) / 32768;
}

static uint64_t cmos_next_event(PC *pc) {
    Cmos *r = &pc->cmos;
    uint64_t t = NS_NEVER;
    if (r->ram[0x0B] & 0x40) t = r->next_periodic;
    if (r->ram[0x0B] & 0x10) t = min64(t, r->next_update);
    return t;
}

static void cmos_irq(PC *pc, uint8_t flag) {
    Cmos *r = &pc->cmos;
    r->ram[0x0C] |= flag;
    if ((r->ram[0x0C] & 0x70 & (r->ram[0x0B] & 0x70)) && !(r->ram[0x0C] & 0x80)) {
        r->ram[0x0C] |= 0x80;
        pc_set_irq(pc, 8, 1);
    }
}

static void cmos_tick(PC *pc, uint64_t now) {
    Cmos *r = &pc->cmos;
    uint64_t p = cmos_period(r);
    if (p && now >= r->next_periodic) {
        r->next_periodic = (now / p + 1) * p;
        if (r->ram[0x0B] & 0x40) cmos_irq(pc, 0x40);
    }
    if (now >= r->next_update) {
        r->next_update = (now / 1000000000ULL + 1) * 1000000000ULL;
        if (r->ram[0x0B] & 0x10) cmos_irq(pc, 0x10);
    }
}

static uint8_t cmos_read(PC *pc) {
    Cmos *r = &pc->cmos;
    int i = r->index & 0x7F;
    uint64_t now = pc_now(pc);
    if (i <= 9 || i == 0x32) {
        time_t t = (time_t)(r->base_time + (int64_t)(now / 1000000000ULL));
        struct tm tm;
        localtime_r(&t, &tm);
        int hour = tm.tm_hour;
        uint8_t pm = 0;
        if (!(r->ram[0x0B] & 2)) { /* 12-hour mode */
            pm = hour >= 12 ? 0x80 : 0;
            hour = hour % 12 ? hour % 12 : 12;
        }
        switch (i) {
        case 0: return bcd(r, tm.tm_sec);
        case 2: return bcd(r, tm.tm_min);
        case 4: return (uint8_t)(bcd(r, hour) | pm);
        case 6: return bcd(r, tm.tm_wday + 1);
        case 7: return bcd(r, tm.tm_mday);
        case 8: return bcd(r, tm.tm_mon + 1);
        case 9: return bcd(r, tm.tm_year % 100);
        case 0x32: return bcd(r, 19 + tm.tm_year / 100);
        default: return r->ram[i]; /* alarms */
        }
    }
    if (i == 0x0A) return (uint8_t)((r->ram[0x0A] & 0x7F) | ((now % 1000000000ULL) >= 1000000000ULL - 244000 ? 0x80 : 0));
    if (i == 0x0C) {
        uint8_t v = r->ram[0x0C];
        r->ram[0x0C] = 0;
        pc_set_irq(pc, 8, 0);
        return v;
    }
    if (i == 0x0D) return 0x80;
    return r->ram[i];
}

static void cmos_write(PC *pc, uint8_t v) {
    Cmos *r = &pc->cmos;
    int i = r->index & 0x7F;
    if (i == 0x0C || i == 0x0D) return;
    r->ram[i] = v;
    if (i == 0x0A || i == 0x0B) {
        uint64_t now = pc_now(pc), p = cmos_period(r);
        if (p) r->next_periodic = (now / p + 1) * p;
        pc_resched(pc);
    }
}

static void cmos_reset(PC *pc) {
    Cmos *r = &pc->cmos;
    memset(r, 0, sizeof *r);
    r->base_time = (int64_t)time(NULL);
    r->ram[0x0A] = 0x26;
    r->ram[0x0B] = 0x02;
    r->ram[0x0D] = 0x80;
    r->next_periodic = r->next_update = NS_NEVER;
    r->next_update = 1000000000ULL;
    uint64_t ext = pc->lowmem > 0x100000 ? (pc->lowmem - 0x100000) >> 10 : 0;
    if (ext > 0xFFFF) ext = 0xFFFF;
    r->ram[0x10] = pc->fda.present ? (pc->fda.size >= 1474560 ? 0x40 : 0x30) : 0;
    r->ram[0x12] = pc->hda.present ? 0xF0 : 0;
    r->ram[0x14] = 0x06 | (pc->fda.present ? 1 : 0);
    r->ram[0x15] = 0x80;
    r->ram[0x16] = 0x02;
    r->ram[0x17] = r->ram[0x30] = (uint8_t)ext;
    r->ram[0x18] = r->ram[0x31] = (uint8_t)(ext >> 8);
    uint64_t above16 = pc->lowmem > 0x1000000 ? (pc->lowmem - 0x1000000) >> 16 : 0;
    if (above16 > 0xFFFF) above16 = 0xFFFF;
    r->ram[0x34] = (uint8_t)above16;
    r->ram[0x35] = (uint8_t)(above16 >> 8);
    uint64_t high = pc->highmem >> 16;
    r->ram[0x5B] = (uint8_t)high;
    r->ram[0x5C] = (uint8_t)(high >> 8);
    r->ram[0x5D] = (uint8_t)(high >> 16);
}

/* ---- 8042 keyboard controller, keyboard and mouse ---- */

static void q_put(ByteQ *q, uint8_t v) {
    int n = (q->tail + 1) % QLEN;
    if (n == q->head) return;
    q->q[q->tail] = v;
    q->tail = n;
}
static int q_empty(ByteQ *q) { return q->head == q->tail; }
static uint8_t q_get(ByteQ *q) {
    uint8_t v = q->q[q->head];
    q->head = (q->head + 1) % QLEN;
    return v;
}

#define KBC_OBF 0x01
#define KBC_SYS 0x04
#define KBC_CMD 0x08
#define KBC_UNLOCKED 0x10
#define KBC_AUXOBF 0x20

static void kbc_update(PC *pc) {
    Kbc *k = &pc->kbc;
    if (!(k->status & KBC_OBF)) {
        if (!q_empty(&k->kq) && !(k->config & 0x10)) {
            k->data = q_get(&k->kq);
            k->status = (k->status | KBC_OBF) & ~KBC_AUXOBF;
        } else if (!q_empty(&k->aq) && !(k->config & 0x20)) {
            k->data = q_get(&k->aq);
            k->status |= KBC_OBF | KBC_AUXOBF;
        }
    }
    int obf = k->status & KBC_OBF, aux = k->status & KBC_AUXOBF;
    pc_set_irq(pc, 1, obf && !aux && (k->config & 1));
    pc_set_irq(pc, 12, obf && aux && (k->config & 2));
}

/* A byte from the controller itself goes through the keyboard queue's head. */
static void kbc_reply(PC *pc, uint8_t v) {
    Kbc *k = &pc->kbc;
    k->kq.head = (k->kq.head + QLEN - 1) % QLEN;
    k->kq.q[k->kq.head] = v;
    if (k->kq.head == k->kq.tail) k->kq.tail = (k->kq.tail + QLEN - 1) % QLEN;
    int dis = k->config & 0x10;
    k->config &= ~0x10;
    kbc_update(pc);
    k->config |= (uint8_t)dis;
}

uint8_t kbc_read_data(PC *pc) {
    Kbc *k = &pc->kbc;
    uint8_t v = k->data;
    k->status &= ~(KBC_OBF | KBC_AUXOBF);
    pc_set_irq(pc, 1, 0); /* the line drops before the next byte (a new edge) */
    pc_set_irq(pc, 12, 0);
    kbc_update(pc);
    feed_kbd(pc);
    return v;
}

static void kbd_command(PC *pc, uint8_t v) {
    Kbc *k = &pc->kbc;
    if (k->kbd_cmd) {
        int cmd = k->kbd_cmd;
        k->kbd_cmd = 0;
        q_put(&k->kq, 0xFA);
        if (v == 0 && cmd == 0xF0) q_put(&k->kq, 2); /* current scan set */
        kbc_update(pc);
        return;
    }
    switch (v) {
    case 0xFF: q_put(&k->kq, 0xFA); q_put(&k->kq, 0xAA); k->kbd_scan = 1; break;
    case 0xF2: q_put(&k->kq, 0xFA); q_put(&k->kq, 0xAB); q_put(&k->kq, 0x83); break;
    case 0xF4: q_put(&k->kq, 0xFA); k->kbd_scan = 1; break;
    case 0xF5: q_put(&k->kq, 0xFA); k->kbd_scan = 0; break;
    case 0xF6: q_put(&k->kq, 0xFA); k->kbd_scan = 1; break;
    case 0xEE: q_put(&k->kq, 0xEE); break;
    case 0xED: case 0xF3: case 0xF0: q_put(&k->kq, 0xFA); k->kbd_cmd = v; break;
    default: q_put(&k->kq, 0xFA); break;
    }
    kbc_update(pc);
}

static void aux_command(PC *pc, uint8_t v) {
    Kbc *k = &pc->kbc;
    if (k->aux_cmd) {
        k->aux_cmd = 0;
        q_put(&k->aq, 0xFA);
    } else {
        switch (v) {
        case 0xFF: q_put(&k->aq, 0xFA); q_put(&k->aq, 0xAA); q_put(&k->aq, 0x00); break;
        case 0xF2: q_put(&k->aq, 0xFA); q_put(&k->aq, 0x00); break;
        case 0xE9: q_put(&k->aq, 0xFA); q_put(&k->aq, 0); q_put(&k->aq, 2); q_put(&k->aq, 100); break;
        case 0xF3: case 0xE8: q_put(&k->aq, 0xFA); k->aux_cmd = v; break;
        default: q_put(&k->aq, 0xFA); break;
        }
    }
    kbc_update(pc);
}

static void kbc_write_cmd(PC *pc, uint8_t v) {
    Kbc *k = &pc->kbc;
    k->status |= KBC_CMD;
    switch (v) {
    case 0x20: kbc_reply(pc, k->config); break;
    case 0x60: case 0xD1: case 0xD2: case 0xD3: case 0xD4: k->pending_cmd = v; break;
    case 0xA7: k->config |= 0x20; break;
    case 0xA8: k->config &= ~0x20; break;
    case 0xA9: kbc_reply(pc, 0); break;
    case 0xAA: k->config |= 0x10; kbc_reply(pc, 0x55); break;
    case 0xAB: kbc_reply(pc, 0); break;
    case 0xAD: k->config |= 0x10; break;
    case 0xAE: k->config &= ~0x10; kbc_update(pc); break;
    case 0xC0: kbc_reply(pc, 0xBF); break;
    case 0xD0: kbc_reply(pc, k->out_port); break;
    case 0xDD: pc_set_a20(pc, 0); k->out_port &= ~2; break;
    case 0xDF: pc_set_a20(pc, 1); k->out_port |= 2; break;
    default:
        if (v >= 0xF0 && !(v & 1)) pc_system_reset(pc); /* pulse the reset line */
        break;
    }
}

static void kbc_write_data(PC *pc, uint8_t v) {
    Kbc *k = &pc->kbc;
    k->status &= ~KBC_CMD;
    int cmd = k->pending_cmd;
    k->pending_cmd = 0;
    switch (cmd) {
    case 0x60:
        k->config = v;
        k->status = (uint8_t)((k->status & ~KBC_SYS) | (v & 4));
        kbc_update(pc);
        break;
    case 0xD1:
        k->out_port = v;
        pc_set_a20(pc, (v & 2) != 0);
        if (!(v & 1)) pc_system_reset(pc);
        break;
    case 0xD2: q_put(&k->kq, v); kbc_update(pc); break;
    case 0xD3: q_put(&k->aq, v); kbc_update(pc); break;
    case 0xD4: aux_command(pc, v); break;
    default: kbd_command(pc, v); break;
    }
}

/* Scan code set 1 make codes indexed by scan code; the BIOS uses the same tables. */
const char pc_kb_lower[] = "\0\0331234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
const char pc_kb_upper[] = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";

static void kbd_key(PC *pc, int code, int shift, int ctrl, int e0) {
    Kbc *k = &pc->kbc;
    if (!k->kbd_scan) return;
    if (ctrl) q_put(&k->kq, 0x1D);
    if (shift) q_put(&k->kq, 0x2A);
    if (e0) q_put(&k->kq, 0xE0);
    q_put(&k->kq, (uint8_t)code);
    if (e0) q_put(&k->kq, 0xE0);
    q_put(&k->kq, (uint8_t)(code | 0x80));
    if (shift) q_put(&k->kq, 0xAA);
    if (ctrl) q_put(&k->kq, 0x9D);
}

static void kbd_ascii(PC *pc, uint8_t ch) {
    Kbc *k = &pc->kbc;
    if (k->esc == 1) {
        k->esc = ch == '[' ? 2 : 0;
        if (ch != '[') kbd_key(pc, 1, 0, 0, 0);
        else return;
    }
    if (k->esc == 2) {
        k->esc = 0;
        int code = ch == 'A' ? 0x48 : ch == 'B' ? 0x50 : ch == 'C' ? 0x4D : ch == 'D' ? 0x4B : ch == 'H' ? 0x47 : ch == 'F' ? 0x4F : 0;
        if (code) kbd_key(pc, code, 0, 0, 1);
        return;
    }
    if (ch == 0x1B) { k->esc = 1; return; }
    if (ch == '\n') ch = '\r';
    if (ch == 0x7F) ch = '\b';
    for (int i = 1; i < 0x3A; i++) {
        if (pc_kb_lower[i] == (char)ch && ch) { kbd_key(pc, i, 0, 0, 0); return; }
        if (pc_kb_upper[i] == (char)ch && ch) { kbd_key(pc, i, 1, 0, 0); return; }
    }
    if (ch >= 1 && ch <= 26)
        for (int i = 1; i < 0x3A; i++)
            if (pc_kb_lower[i] == 'a' + ch - 1) { kbd_key(pc, i, 0, 1, 0); return; }
}

/* ---- 16550 UART (COM1) ---- */

static uint8_t uart_iir(Uart *u) {
    uint8_t fifo = (u->fcr & 1) ? 0xC0 : 0;
    if ((u->ier & 1) && u->rx_head != u->rx_tail) return fifo | 0x04;
    if ((u->ier & 2) && u->thre_pending) return fifo | 0x02;
    return fifo | 0x01;
}

static void uart_update(PC *pc) {
    Uart *u = &pc->uart;
    pc_set_irq(pc, 4, !(uart_iir(u) & 1) && (u->mcr & 8));
}

static void uart_rx(PC *pc, uint8_t ch) {
    Uart *u = &pc->uart;
    int n = (u->rx_tail + 1) % RXLEN;
    if (n == u->rx_head) return;
    u->rx[u->rx_tail] = ch;
    u->rx_tail = n;
    uart_update(pc);
}

void uart_tx(PC *pc, uint8_t ch) {
    Uart *u = &pc->uart;
    if (u->mcr & 0x10) uart_rx(pc, ch); /* loopback */
    else machine_output(pc->m, &ch, 1);
    u->thre_pending = 1;
    uart_update(pc);
}

static uint8_t uart_msr(Uart *u) {
    if (u->mcr & 0x10)
        return (uint8_t)(((u->mcr & 2) << 3) | ((u->mcr & 1) << 5) | ((u->mcr & 4) << 4) | ((u->mcr & 8) << 4));
    return 0xB0; /* DCD, DSR, CTS */
}

static uint8_t uart_read(PC *pc, int reg) {
    Uart *u = &pc->uart;
    uint8_t v = 0;
    switch (reg) {
    case 0:
        if (u->lcr & 0x80) return u->dll;
        if (u->rx_head != u->rx_tail) {
            v = u->rx[u->rx_head];
            u->rx_head = (u->rx_head + 1) % RXLEN;
        }
        break;
    case 1: return (u->lcr & 0x80) ? u->dlm : u->ier;
    case 2:
        v = uart_iir(u);
        if ((v & 0x0F) == 0x02) u->thre_pending = 0;
        break;
    case 3: return u->lcr;
    case 4: return u->mcr;
    case 5:
        feed_uart(pc, 1);
        v = (uint8_t)(0x60 | (u->rx_head != u->rx_tail));
        break;
    case 6: {
        uint8_t m = uart_msr(u);
        v = (uint8_t)(m | (((m ^ u->msr_last) >> 4) & 0x0F));
        u->msr_last = m;
        break;
    }
    case 7: return u->scr;
    }
    uart_update(pc);
    return v;
}

static void uart_write(PC *pc, int reg, uint8_t v) {
    Uart *u = &pc->uart;
    switch (reg) {
    case 0:
        if (u->lcr & 0x80) u->dll = v;
        else uart_tx(pc, v);
        break;
    case 1:
        if (u->lcr & 0x80) { u->dlm = v; break; }
        if ((v & 2) && !(u->ier & 2)) u->thre_pending = 1;
        u->ier = v & 0x0F;
        feed_uart(pc, 0);
        break;
    case 2:
        u->fcr = v & 0xC9;
        if (v & 2) u->rx_head = u->rx_tail;
        break;
    case 3: u->lcr = v; break;
    case 4: u->mcr = v & 0x1F; feed_uart(pc, 0); break;
    case 7: u->scr = v; break;
    }
    uart_update(pc);
}

/* ---- host input: terminal or web UI -> keyboard and serial port ---- */

static struct termios saved_tio;
static int tio_saved;

static void restore_terminal(void) {
    if (tio_saved) tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
}

static void setup_terminal(void) {
    if (tio_saved || !isatty(STDIN_FILENO)) return;
    tcgetattr(STDIN_FILENO, &saved_tio);
    struct termios t = saved_tio;
    t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(ICRNL | IXON);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    tio_saved = 1;
    atexit(restore_terminal);
}

static void inq_put(InQ *q, uint8_t ch) {
    int n = (q->tail + 1) % INLEN;
    if (n != q->head) q->q[q->tail] = ch, q->tail = n;
}
static int inq_get(InQ *q) {
    if (q->head == q->tail) return -1;
    uint8_t ch = q->q[q->head];
    q->head = (q->head + 1) % INLEN;
    return ch;
}

static int rx_count(Uart *u) { return (u->rx_tail - u->rx_head + RXLEN) % RXLEN; }

/* The UART takes input while receive interrupts can reach the PIC (IER bit
 * 0 and MCR.OUT2), up to its 16-byte FIFO. Real-mode software that polls
 * with interrupts off gets one byte per LSR read. Bytes are not offered to
 * a driver that is still probing the port, which would discard them. */
static void feed_uart(PC *pc, int polled) {
    Uart *u = &pc->uart;
    if (u->mcr & 0x10) return;
    if ((u->ier & 1) && (u->mcr & 8)) {
        while (rx_count(u) < 16) {
            int ch = inq_get(&pc->in_uart);
            if (ch < 0) break;
            uart_rx(pc, (uint8_t)ch);
        }
    } else if (polled && !(u->ier & 1) && !rx_count(u) && pc_cpu(pc)->bits == 16) {
        int ch = inq_get(&pc->in_uart);
        if (ch >= 0) uart_rx(pc, (uint8_t)ch);
    }
}

/* The keyboard sends the next key once the controller's buffer is empty and
 * the keyboard interrupt is enabled. */
static void feed_kbd(PC *pc) {
    Kbc *k = &pc->kbc;
    while (q_empty(&k->kq) && !(k->status & KBC_OBF) && (k->config & 1) && !(k->config & 0x10) && k->kbd_scan) {
        int ch = inq_get(&pc->in_kbd);
        if (ch < 0) return;
        kbd_ascii(pc, (uint8_t)ch);
        kbc_update(pc);
    }
}

static void input_byte(PC *pc, uint8_t ch) {
    inq_put(&pc->in_uart, ch);
    inq_put(&pc->in_kbd, ch);
}

static void poll_input(PC *pc) {
    Machine *m = pc->m;
    if (pc->console && !pc->stdin_eof) {
        struct pollfd p = {STDIN_FILENO, POLLIN, 0};
        if (poll(&p, 1, 0) > 0) {
            uint8_t buf[256];
            ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) pc->stdin_eof = 1;
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == 0x1D && tio_saved) { /* Ctrl-]: leave the simulator */
                    m->state = MS_EXITED;
                    snprintf(m->status, sizeof m->status, "quit from the console");
                    m->cpu.stop_request = 1;
                    m->last_stop = STOP_EXIT;
                    return;
                }
                input_byte(pc, buf[i]);
            }
        }
    }
    while (m->in_pos < m->in.len) input_byte(pc, (uint8_t)m->in.data[m->in_pos++]);
    feed_uart(pc, 0);
    feed_kbd(pc);
    kbc_update(pc);
}

/* ---- VGA registers (text mode: the frame buffer is plain RAM at 0xB8000) ---- */

static uint8_t vga_read(PC *pc, uint16_t port) {
    Vga *v = &pc->vga;
    switch (port) {
    case 0x3B4: case 0x3D4: return v->crtc_index;
    case 0x3B5: case 0x3D5: return v->crtc[v->crtc_index & 31];
    case 0x3BA: case 0x3DA: {
        v->attr_flip = 0;
        uint64_t t = pc_now(pc) % 16683000; /* 60 Hz frame, ~1.4 ms retrace */
        return (uint8_t)((t > 15250000 ? 0x09 : 0) | ((t / 31777) % 2));
    }
    case 0x3C0: return v->attr_index;
    case 0x3C1: return v->attr[v->attr_index & 31];
    case 0x3C4: return v->seq_index;
    case 0x3C5: return v->seq[v->seq_index & 7];
    case 0x3C7: return 3;
    case 0x3C8: return v->dac_windex;
    case 0x3C9: {
        uint8_t r = v->dac[v->dac_rindex * 3 + v->dac_comp];
        if (++v->dac_comp == 3) v->dac_comp = 0, v->dac_rindex++;
        return r;
    }
    case 0x3CC: return v->misc;
    case 0x3CE: return v->gc_index;
    case 0x3CF: return v->gc[v->gc_index & 15];
    }
    return 0xFF;
}

static void vga_write(PC *pc, uint16_t port, uint8_t val) {
    Vga *v = &pc->vga;
    switch (port) {
    case 0x3B4: case 0x3D4: v->crtc_index = val; break;
    case 0x3B5: case 0x3D5: v->crtc[v->crtc_index & 31] = val; break;
    case 0x3C0:
        if (!v->attr_flip) v->attr_index = val;
        else v->attr[v->attr_index & 31] = val;
        v->attr_flip ^= 1;
        break;
    case 0x3C2: v->misc = val; break;
    case 0x3C4: v->seq_index = val; break;
    case 0x3C5: v->seq[v->seq_index & 7] = val; break;
    case 0x3C7: v->dac_rindex = val; v->dac_comp = 0; break;
    case 0x3C8: v->dac_windex = val; v->dac_comp = 0; break;
    case 0x3C9:
        v->dac[v->dac_windex * 3 + v->dac_comp] = val & 0x3F;
        if (++v->dac_comp == 3) v->dac_comp = 0, v->dac_windex++;
        break;
    case 0x3CE: v->gc_index = val; break;
    case 0x3CF: v->gc[v->gc_index & 15] = val; break;
    }
}

/* ---- PCI configuration space (i440FX host bridge, PIIX3 ISA + IDE) ---- */

static int pci_fn(int bus, int devfn) {
    if (bus) return -1;
    return devfn == 0 ? 0 : devfn == 8 ? 1 : devfn == 9 ? 2 : -1;
}
static const int pci_devfn[3] = {0, 8, 9};

static void pci_reset(PC *pc) {
    static const struct { uint16_t ven, dev; uint8_t cls, sub, prog, hdr; uint16_t cmd; } ids[3] = {
        {0x8086, 0x1237, 6, 0, 0, 0, 6}, {0x8086, 0x7000, 6, 1, 0, 0x80, 7}, {0x8086, 0x7010, 1, 1, 0x80, 0, 1}};
    memset(&pc->pci, 0, sizeof pc->pci);
    for (int i = 0; i < 3; i++) {
        uint8_t *c = pc->pci.cfg[i];
        c[0] = (uint8_t)ids[i].ven; c[1] = (uint8_t)(ids[i].ven >> 8);
        c[2] = (uint8_t)ids[i].dev; c[3] = (uint8_t)(ids[i].dev >> 8);
        c[4] = (uint8_t)ids[i].cmd;
        c[6] = 0x80; c[7] = 0x02;
        c[9] = ids[i].prog; c[0xA] = ids[i].sub; c[0xB] = ids[i].cls;
        c[0xE] = ids[i].hdr;
    }
    uint8_t *isa = pc->pci.cfg[1];
    isa[0x60] = isa[0x61] = isa[0x62] = isa[0x63] = 0x80; /* PIRQ routing disabled */
    uint8_t *ide = pc->pci.cfg[2];
    ide[0x41] = 0x80; /* primary channel decode enable */
    ide[0x43] = 0x80; /* secondary */
}

uint32_t pci_cfg_read(PC *pc, int bus, int devfn, int reg, int size) {
    int f = pci_fn(bus, devfn);
    if (f < 0) return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)pc->pci.cfg[f][(reg + i) & 0xFF] << (8 * i);
    return v;
}

void pci_cfg_write(PC *pc, int bus, int devfn, int reg, int size, uint32_t val) {
    int f = pci_fn(bus, devfn);
    if (f < 0) return;
    for (int i = 0; i < size; i++) {
        int r = (reg + i) & 0xFF;
        if (r == 4 || r == 5 || r == 0xC || r == 0xD || r == 0x3C || r >= 0x40) pc->pci.cfg[f][r] = (uint8_t)(val >> (8 * i));
    }
}

int pci_find(PC *pc, uint16_t vendor, uint16_t device, int index) {
    for (int i = 0; i < 3; i++) {
        uint8_t *c = pc->pci.cfg[i];
        if ((c[0] | c[1] << 8) == vendor && (c[2] | c[3] << 8) == device && index-- == 0) return pci_devfn[i];
    }
    return -1;
}

int pci_find_class(PC *pc, uint32_t cls, int index) {
    for (int i = 0; i < 3; i++) {
        uint8_t *c = pc->pci.cfg[i];
        if ((uint32_t)(c[9] | c[0xA] << 8 | c[0xB] << 16) == cls && index-- == 0) return pci_devfn[i];
    }
    return -1;
}

/* ---- ATA disk on the primary IDE channel (master only) ---- */

int disk_rw(Disk *d, uint64_t lba, uint32_t count, void *buf, int write) {
    if (!d->present || lba + count > d->size / 512) return -1;
    if (write) memcpy(d->data + lba * 512, buf, (size_t)count * 512);
    else memcpy(buf, d->data + lba * 512, (size_t)count * 512);
    return 0;
}

#define ATA_ERR 0x01
#define ATA_DRQ 0x08
#define ATA_DSC 0x10
#define ATA_DRDY 0x40
#define ATA_BSY 0x80

static int ide_present(PC *pc) { return pc->hda.present && !(pc->ide.select & 0x10); }

static void ide_irq(PC *pc, int on) {
    pc->ide.irq = on;
    pc_set_irq(pc, 14, on && !(pc->ide.control & 2));
}

static void ide_abort(PC *pc, uint8_t err) {
    Ide *d = &pc->ide;
    d->status = ATA_DRDY | ATA_DSC | ATA_ERR;
    d->error = err;
    d->len = d->pos = 0;
    ide_irq(pc, 1);
}

static void ide_put_string(uint8_t *buf, int word, int words, const char *s) {
    for (int i = 0; i < words * 2; i++) {
        char ch = *s ? *s++ : ' ';
        buf[word * 2 + (i ^ 1)] = (uint8_t)ch;
    }
}

static void ide_identify(PC *pc) {
    Ide *d = &pc->ide;
    uint16_t w[256];
    memset(w, 0, sizeof w);
    uint64_t sectors = pc->hda.size / 512;
    uint32_t cyls = (uint32_t)(sectors / (16 * 63));
    if (cyls > 16383) cyls = 16383;
    w[0] = 0x0040;
    w[1] = (uint16_t)cyls;
    w[3] = 16;
    w[6] = 63;
    w[47] = 0x8010;
    w[49] = 0x0200;
    w[51] = 0x0200;
    w[53] = 0x0007;
    w[54] = (uint16_t)cyls;
    w[55] = 16;
    w[56] = 63;
    uint32_t chs = cyls * 16 * 63;
    w[57] = (uint16_t)chs;
    w[58] = (uint16_t)(chs >> 16);
    w[59] = (uint16_t)(d->mult_count ? 0x100 | d->mult_count : 0);
    uint32_t l28 = sectors > 0x0FFFFFFF ? 0x0FFFFFFF : (uint32_t)sectors;
    w[60] = (uint16_t)l28;
    w[61] = (uint16_t)(l28 >> 16);
    w[80] = 0x00F0;
    w[82] = 0x4000;
    w[83] = 0x7400;
    w[84] = 0x4000;
    w[85] = 0x4000;
    w[86] = 0x3400;
    w[87] = 0x4000;
    for (int i = 0; i < 4; i++) w[100 + i] = (uint16_t)(sectors >> (16 * i));
    memcpy(d->buf, w, 512);
    ide_put_string(d->buf, 10, 10, "X86SIM0001");
    ide_put_string(d->buf, 23, 4, "1.0");
    ide_put_string(d->buf, 27, 20, "x86sim HARDDISK");
    d->pos = 0;
    d->len = 512;
    d->remain = 0;
    d->writing = 0;
    d->status = ATA_DRDY | ATA_DSC | ATA_DRQ;
    ide_irq(pc, 1);
}

static void ide_next_block(PC *pc) {
    Ide *d = &pc->ide;
    uint32_t n = d->multiple && d->mult_count ? (uint32_t)d->mult_count : 1;
    if (n > d->remain) n = d->remain;
    d->len = (int)n * 512;
    d->pos = 0;
    if (!d->writing && disk_rw(&pc->hda, d->lba, n, d->buf, 0)) {
        ide_abort(pc, 0x10);
        return;
    }
    d->status = ATA_DRDY | ATA_DSC | ATA_DRQ;
    if (!d->writing) ide_irq(pc, 1);
}

static void ide_block_done(PC *pc) {
    Ide *d = &pc->ide;
    uint32_t n = (uint32_t)d->len / 512;
    if (d->writing && disk_rw(&pc->hda, d->lba, n, d->buf, 1)) {
        ide_abort(pc, 0x10);
        return;
    }
    d->lba += n;
    d->remain -= n;
    if (d->remain) ide_next_block(pc);
    else {
        d->len = d->pos = 0;
        d->status = ATA_DRDY | ATA_DSC;
    }
    if (d->writing) ide_irq(pc, 1);
}

static void ide_command(PC *pc, uint8_t cmd) {
    Ide *d = &pc->ide;
    if (!ide_present(pc)) return;
    int lba48 = cmd == 0x24 || cmd == 0x29 || cmd == 0x34 || cmd == 0x39 || cmd == 0x42;
    uint32_t count;
    if (lba48) {
        d->lba = (uint64_t)d->hob_hcyl << 40 | (uint64_t)d->hob_lcyl << 32 | (uint64_t)d->hob_sect << 24 |
                 (uint64_t)d->hcyl << 16 | (uint64_t)d->lcyl << 8 | d->sect;
        count = (uint32_t)(d->hob_nsect << 8 | d->nsect);
        if (!count) count = 65536;
    } else {
        if (d->select & 0x40) d->lba = (uint64_t)(d->select & 15) << 24 | (uint64_t)d->hcyl << 16 | (uint64_t)d->lcyl << 8 | d->sect;
        else d->lba = ((uint64_t)(d->hcyl << 8 | d->lcyl) * 16 + (d->select & 15)) * 63 + (d->sect ? d->sect - 1 : 0);
        count = d->nsect ? d->nsect : 256;
    }
    d->error = 0;
    switch (cmd) {
    case 0xEC: ide_identify(pc); return;
    case 0x20: case 0x21: case 0x24: case 0xC4: case 0x29:
    case 0x30: case 0x31: case 0x34: case 0xC5: case 0x39:
        d->writing = cmd >= 0x30 && cmd != 0xC4 && cmd != 0x29;
        d->multiple = cmd == 0xC4 || cmd == 0xC5 || cmd == 0x29 || cmd == 0x39;
        if (d->multiple && !d->mult_count) { ide_abort(pc, 0x04); return; }
        d->remain = count;
        ide_next_block(pc);
        return;
    case 0x40: case 0x41: case 0x42: case 0x10: case 0x91: case 0xEF: case 0xE7: case 0xEA:
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6:
        d->status = ATA_DRDY | ATA_DSC;
        ide_irq(pc, 1);
        return;
    case 0xE5:
        d->nsect = 0xFF;
        d->status = ATA_DRDY | ATA_DSC;
        ide_irq(pc, 1);
        return;
    case 0xC6:
        if (d->nsect > 16 || (d->nsect & (d->nsect - 1))) { ide_abort(pc, 0x04); return; }
        d->mult_count = d->nsect;
        d->status = ATA_DRDY | ATA_DSC;
        ide_irq(pc, 1);
        return;
    case 0xF8: case 0x27: { /* read native max address */
        uint64_t max = pc->hda.size / 512 - 1;
        d->sect = (uint8_t)max; d->lcyl = (uint8_t)(max >> 8); d->hcyl = (uint8_t)(max >> 16);
        if (cmd == 0xF8) d->select = (uint8_t)((d->select & 0xF0) | ((max >> 24) & 15));
        else { d->hob_sect = (uint8_t)(max >> 24); d->hob_lcyl = (uint8_t)(max >> 32); d->hob_hcyl = (uint8_t)(max >> 40); }
        d->status = ATA_DRDY | ATA_DSC;
        ide_irq(pc, 1);
        return;
    }
    }
    ide_abort(pc, 0x04);
}

static void ide_signature(Ide *d) {
    d->nsect = d->sect = 1;
    d->lcyl = d->hcyl = 0;
    d->error = 1;
    d->status = ATA_DRDY | ATA_DSC;
}

static uint32_t ide_data_read(PC *pc, int size) {
    Ide *d = &pc->ide;
    if (!(d->status & ATA_DRQ) || d->writing) return size == 4 ? 0xFFFFFFFF : 0xFFFF;
    uint32_t v = 0;
    for (int i = 0; i < size && d->pos < d->len; i++) v |= (uint32_t)d->buf[d->pos++] << (8 * i);
    if (d->pos >= d->len) {
        if (d->remain) ide_block_done(pc);
        else d->status = ATA_DRDY | ATA_DSC, d->len = 0;
    }
    return v;
}

static void ide_data_write(PC *pc, int size, uint32_t v) {
    Ide *d = &pc->ide;
    if (!(d->status & ATA_DRQ) || !d->writing) return;
    for (int i = 0; i < size && d->pos < d->len; i++) d->buf[d->pos++] = (uint8_t)(v >> (8 * i));
    if (d->pos >= d->len) ide_block_done(pc);
}

static uint8_t ide_read(PC *pc, int reg) {
    Ide *d = &pc->ide;
    if (!pc->hda.present) return 0xFF;
    if (!ide_present(pc) && reg != 6) return 0;
    switch (reg) {
    case 1: return d->error;
    case 2: return d->nsect;
    case 3: return d->sect;
    case 4: return d->lcyl;
    case 5: return d->hcyl;
    case 6: return d->select;
    case 7:
        ide_irq(pc, 0);
        return d->status;
    }
    return 0xFF;
}

static void ide_write(PC *pc, int reg, uint8_t v) {
    Ide *d = &pc->ide;
    switch (reg) {
    case 1: d->feature = v; break;
    case 2: d->hob_nsect = d->nsect; d->nsect = v; break;
    case 3: d->hob_sect = d->sect; d->sect = v; break;
    case 4: d->hob_lcyl = d->lcyl; d->lcyl = v; break;
    case 5: d->hob_hcyl = d->hcyl; d->hcyl = v; break;
    case 6: d->select = v | 0xA0; break;
    case 7: ide_irq(pc, 0); ide_command(pc, v); break;
    }
}

static void ide_control(PC *pc, uint8_t v) {
    Ide *d = &pc->ide;
    if ((d->control & 4) && !(v & 4)) { /* end of software reset */
        ide_signature(d);
        d->len = d->pos = 0;
        d->remain = 0;
    } else if (v & 4) d->status = ATA_BSY;
    d->control = v;
    ide_irq(pc, d->irq);
}

/* ---- the port bus ---- */

static uint8_t port_read8(PC *pc, uint16_t port) {
    switch (port) {
    case 0x20: case 0x21: return pic_read(pc, 0, port & 1);
    case 0xA0: case 0xA1: return pic_read(pc, 1, port & 1);
    case 0x40: case 0x41: case 0x42: return pit_read(pc, port - 0x40);
    case 0x60: return kbc_read_data(pc);
    case 0x61: return port61_read(pc);
    case 0x64: return (uint8_t)(pc->kbc.status | KBC_UNLOCKED);
    case 0x71: return cmos_read(pc);
    case 0x70: return pc->cmos.index;
    case 0x92: return pc->port92 | (((pc_cpu(pc)->a20_mask >> 20) & 1) << 1);
    case 0x4D0: return pc->pic[0].elcr;
    case 0x4D1: return pc->pic[1].elcr;
    case 0x3F6: return pc->hda.present ? (ide_present(pc) ? pc->ide.status : 0) : 0xFF;
    }
    if (port >= 0x3F8 && port <= 0x3FF) return uart_read(pc, port - 0x3F8);
    if (port >= 0x1F1 && port <= 0x1F7) return ide_read(pc, port - 0x1F0);
    if (port >= 0x3B0 && port <= 0x3DF) return vga_read(pc, port);
    if (port >= 0xCFC && port <= 0xCFF) {
        uint32_t a = pc->pci.addr;
        if (!(a & 0x80000000)) return 0xFF;
        return (uint8_t)pci_cfg_read(pc, (a >> 16) & 0xFF, (a >> 8) & 0xFF, (int)(a & 0xFC) + (port & 3), 1);
    }
    return 0xFF;
}

static void port_write8(PC *pc, uint16_t port, uint8_t v) {
    switch (port) {
    case 0x20: case 0x21: pic_write(pc, 0, port & 1, v); return;
    case 0xA0: case 0xA1: pic_write(pc, 1, port & 1, v); return;
    case 0x40: case 0x41: case 0x42: case 0x43: pit_write(pc, port - 0x40, v); return;
    case 0x60: kbc_write_data(pc, v); return;
    case 0x61: port61_write(pc, v); return;
    case 0x64: kbc_write_cmd(pc, v); return;
    case 0x70:
        pc->cmos.index = v; /* bit 7 masks NMI: there are no NMI sources */
        return;
    case 0x71: cmos_write(pc, v); return;
    case 0x92:
        pc_set_a20(pc, (v & 2) != 0);
        pc->port92 = v & ~3;
        if (v & 1) pc_system_reset(pc);
        return;
    case 0x4D0: pc->pic[0].elcr = v & 0xF8; return;
    case 0x4D1: pc->pic[1].elcr = v & 0xDE; return;
    case 0x3F6: ide_control(pc, v); return;
    case 0xCF9:
        if (v & 4) pc_system_reset(pc);
        return;
    case 0xE9: machine_output(pc->m, &v, 1); return; /* debug console (Bochs/QEMU convention) */
    case 0xF4: { /* test exit: the value written is the exit code */
        Machine *m = pc->m;
        m->state = MS_EXITED;
        m->exit_code = v;
        snprintf(m->status, sizeof m->status, "exited with code %d", m->exit_code);
        pc_cpu(pc)->stop_request = 1;
        m->last_stop = STOP_EXIT;
        return;
    }
    }
    if (port >= 0x3F8 && port <= 0x3FF) { uart_write(pc, port - 0x3F8, v); return; }
    if (port >= 0x1F1 && port <= 0x1F7) { ide_write(pc, port - 0x1F0, v); return; }
    if (port >= 0x3B0 && port <= 0x3DF) { vga_write(pc, port, v); return; }
    if (port >= 0xCFC && port <= 0xCFF) {
        uint32_t a = pc->pci.addr;
        if (a & 0x80000000) pci_cfg_write(pc, (a >> 16) & 0xFF, (a >> 8) & 0xFF, (int)(a & 0xFC) + (port & 3), 1, v);
    }
}

uint32_t pc_port_read(PC *pc, uint16_t port, int size) {
    if (port == CACHE_PORT && size == 4) return pc->m->cache ? cache_line_states(pc->m->cache, pc->cache_query, pc->cache_core) : 0;
    if (port == 0x1F0) return ide_data_read(pc, size == 4 ? 4 : 2) & (size == 1 ? 0xFF : 0xFFFFFFFF);
    if (port == 0xCF8 && size == 4) return pc->pci.addr;
    if (port >= 0xCFC && port <= 0xCFF && (pc->pci.addr & 0x80000000)) {
        uint32_t a = pc->pci.addr;
        return pci_cfg_read(pc, (a >> 16) & 0xFF, (a >> 8) & 0xFF, (int)(a & 0xFC) + (port & 3), size);
    }
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= (uint32_t)port_read8(pc, (uint16_t)(port + i)) << (8 * i);
    return v;
}

/* An exception reached one of the editor's default handlers (stubs that
 * OUT their vector to EXC_PORT, then halt): report it and stop the run. The
 * frame is read at SS:RSP as a physical address (identity-mapped memory). */
static void editor_exception(PC *pc, int vector) {
    Machine *m = pc->m;
    CPU *c = pc_cpu(pc);
    int ws = c->bits == 64 ? 8 : 4;
    int has_err = vector == 8 || (vector >= 10 && vector <= 14) || vector == 17 || vector == 21 || vector == 29 ||
                  vector == 30;
    uint64_t sp = c->sbase[SS] + (c->bits == 64 ? c->r[RSP] : (uint32_t)c->r[RSP]);
    uint64_t code = 0, rip = 0;
    if (has_err) {
        machine_phys_peek(m, sp, &code, (size_t)ws);
        sp += (uint64_t)ws;
    }
    machine_phys_peek(m, sp, &rip, (size_t)ws);
    char msg[240];
    int k = snprintf(msg, sizeof msg, "cpu%d: unhandled %s (vector %d) at 0x%llx", c->index,
                     vector < 32 ? cpu_exc_name(vector) : "interrupt", vector, (unsigned long long)rip);
    if (has_err) k += snprintf(msg + k, sizeof msg - (size_t)k, ", error code 0x%llx", (unsigned long long)code);
    if (vector == EXC_PF) k += snprintf(msg + k, sizeof msg - (size_t)k, ", cr2 0x%llx", (unsigned long long)c->cr2);
    snprintf(msg + k, sizeof msg - (size_t)k, "; cpu%d halted", c->index);
    machine_output(m, "\n[x86sim] ", 10);
    machine_output(m, msg, strlen(msg));
    machine_output(m, "\n", 1);
    snprintf(m->status, sizeof m->status, "%s", msg);
    m->view_cpu = c->index;
    m->last_stop = STOP_TRAP;
    c->stop_request = 1;
}

void pc_port_write(PC *pc, uint16_t port, int size, uint32_t v) {
    if (port == CACHE_PORT && size == 4) { pc->cache_query = v; return; }
    if (port == CACHE_CORE_PORT) { pc->cache_core = (uint8_t)v; return; }
    if (port == EXC_PORT) { editor_exception(pc, v & 0xFF); return; }
    if (port == 0x1F0) { ide_data_write(pc, size == 4 ? 4 : 2, v); return; }
    if (port == 0xCF8 && size == 4) { pc->pci.addr = v; return; }
    if (port >= 0xCFC && port <= 0xCFF && (pc->pci.addr & 0x80000000)) {
        uint32_t a = pc->pci.addr;
        pci_cfg_write(pc, (a >> 16) & 0xFF, (a >> 8) & 0xFF, (int)(a & 0xFC) + (port & 3), size, v);
        return;
    }
    if (port == BIOS_PORT && size == 1) {
        CPU *c = pc_cpu(pc);
        uint64_t lin = c->sbase[CS] + c->rip;
        if ((c->bits == 16 || (c->rflags & FL_VM)) && lin >= BIOS_STUBS && lin < BIOS_STUBS + 256 * 16) {
            bios_call(pc, (int)((lin - BIOS_STUBS) >> 4));
            return;
        }
    }
    for (int i = 0; i < size; i++) port_write8(pc, (uint16_t)(port + i), (uint8_t)(v >> (8 * i)));
}

static int port_in(CPU *c, uint16_t port, int size, uint32_t *val) {
    *val = pc_port_read(((Machine *)c->user)->pc, port, size);
    return 1;
}

static int port_out(CPU *c, uint16_t port, int size, uint32_t val) {
    pc_port_write(((Machine *)c->user)->pc, port, size, val);
    return 1;
}

/* ---- display snapshot for the UI ---- */

static void hex_bytes(Buf *b, const uint8_t *p, size_t n) {
    static const char hx[] = "0123456789abcdef";
    char tmp[512];
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        tmp[k++] = hx[p[i] >> 4];
        tmp[k++] = hx[p[i] & 15];
        if (k == sizeof tmp) buf_append(b, tmp, k), k = 0;
    }
    buf_append(b, tmp, k);
}

void pc_screen(Machine *m, void *out) {
    Buf *b = out;
    PC *pc = m->pc;
    if (!pc) {
        buf_printf(b, "{\"mode\":-1}");
        return;
    }
    Vga *v = &pc->vga;
    uint8_t mode = 3, rows = 24;
    uint16_t cols = 80;
    mem_peek(m->mem, 0x449, &mode, 1);
    mem_peek(m->mem, 0x44A, &cols, 2);
    mem_peek(m->mem, 0x484, &rows, 1);
    if (mode == 0x13) {
        static uint8_t px[64000];
        uint8_t pal[768];
        mem_peek(m->mem, 0xA0000, px, sizeof px);
        memcpy(pal, v->dac, sizeof pal);
        int any = 0;
        for (int i = 0; i < 768; i++) any |= pal[i];
        if (!any) /* default palette: EGA colours, greys, then a colour cube */
            for (int i = 0; i < 256; i++) {
                int r, g, bl;
                if (i < 16) {
                    int hi = (i & 8) ? 21 : 0;
                    r = ((i >> 2) & 1) * 42 + hi, g = ((i >> 1) & 1) * 42 + hi, bl = (i & 1) * 42 + hi;
                    if (i == 6) g = 21;
                } else if (i < 32) r = g = bl = (i - 16) * 4;
                else {
                    int k = (i - 32) % 216;
                    r = (k / 36) * 12, g = ((k / 6) % 6) * 12, bl = (k % 6) * 12;
                }
                pal[i * 3] = (uint8_t)r, pal[i * 3 + 1] = (uint8_t)g, pal[i * 3 + 2] = (uint8_t)bl;
            }
        buf_printf(b, "{\"mode\":19,\"width\":320,\"height\":200,\"palette\":\"");
        hex_bytes(b, pal, sizeof pal);
        buf_printf(b, "\",\"pixels\":\"");
        hex_bytes(b, px, sizeof px);
        buf_printf(b, "\"}");
        return;
    }
    if (cols < 40 || cols > 132) cols = 80;
    int nrows = rows >= 24 && rows < 60 ? rows + 1 : 25;
    uint32_t start = (uint32_t)(v->crtc[0x0C] << 8 | v->crtc[0x0D]);
    uint32_t cur = (uint32_t)(v->crtc[0x0E] << 8 | v->crtc[0x0F]);
    int cursor = (v->crtc[0x0A] & 0x20) || cur < start || cur >= start + (uint32_t)(cols * nrows) ? -1 : (int)(cur - start);
    static uint8_t cells[132 * 60 * 2];
    size_t n = (size_t)cols * nrows * 2;
    mem_peek(m->mem, 0xB8000 + (uint64_t)start * 2, cells, n);
    buf_printf(b, "{\"mode\":%d,\"cols\":%d,\"rows\":%d,\"cursor\":%d,\"cells\":\"", mode, cols, nrows, cursor);
    hex_bytes(b, cells, n);
    buf_printf(b, "\"}");
}

/* ---- lifecycle ---- */

static int open_disk(Disk *d, const char *path, int floppy, char *err, size_t errn) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size < 512) {
        snprintf(err, errn, "cannot open disk image %s", path);
        if (fd >= 0) close(fd);
        return -1;
    }
    /* Private mapping: the guest can write, but the image file never changes. */
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        snprintf(err, errn, "cannot map disk image %s", path);
        return -1;
    }
    d->data = p;
    d->size = (uint64_t)st.st_size & ~511ULL;
    d->present = 1;
    if (floppy) {
        static const struct { uint64_t size; int c, h, s; } fl[] = {
            {368640, 40, 2, 9}, {737280, 80, 2, 9}, {1228800, 80, 2, 15}, {1474560, 80, 2, 18}, {2949120, 80, 2, 36}};
        d->cyls = 80, d->heads = 2, d->spt = 18;
        for (size_t i = 0; i < sizeof fl / sizeof fl[0]; i++)
            if (d->size == fl[i].size) d->cyls = fl[i].c, d->heads = fl[i].h, d->spt = fl[i].s;
    } else {
        d->spt = 63;
        d->heads = d->size > 504ULL * 1024 * 1024 ? 255 : 16;
        d->cyls = (int)(d->size / ((uint64_t)d->heads * 63 * 512));
        if (d->cyls > 1024) d->cyls = 1024;
        if (d->cyls < 1) d->cyls = 1;
    }
    return 0;
}

PC *pc_create(Machine *m) {
    PC *pc = calloc(1, sizeof *pc);
    pc->m = m;
    pc->ram = (uint64_t)(m->pc_ram_mb ? m->pc_ram_mb : 256) << 20;
    pc->lowmem = pc->ram < 0xC0000000ULL ? pc->ram : 0xC0000000ULL;
    pc->highmem = pc->ram - pc->lowmem;
    return pc;
}

void pc_destroy(PC *pc) {
    if (pc->hda.data) munmap(pc->hda.data, pc->hda.size);
    if (pc->fda.data) munmap(pc->fda.data, pc->fda.size);
    free(pc);
}

static void devices_reset(PC *pc) {
    memset(pc->pic, 0, sizeof pc->pic);
    memset(pc->pit, 0, sizeof pc->pit);
    for (int i = 0; i < 3; i++) pc->pit[i].gate = i < 2, pc->pit[i].rw = 3;
    cmos_reset(pc);
    memset(&pc->kbc, 0, sizeof pc->kbc);
    pc->kbc.status = KBC_SYS;
    pc->kbc.config = 0x45; /* keyboard interrupt, system flag, scan code translation */
    pc->kbc.out_port = 0x03;
    pc->kbc.kbd_scan = 1;
    memset(&pc->uart, 0, sizeof pc->uart);
    pc->uart.thre_pending = 0;
    pci_reset(pc);
    memset(&pc->ide, 0, sizeof pc->ide);
    ide_signature(&pc->ide);
    pc->ide.select = 0xA0;
    memset(&pc->vga, 0, sizeof pc->vga);
    pc->vga.misc = 0x67;
    pc->vga.crtc[0x0A] = 0x0D;
    pc->vga.crtc[0x0B] = 0x0E;
    apic_reset(pc);
}

int pc_load_image(Machine *m, char *err, size_t errn); /* loader.c */

/* ---- CPUs ---- */

void pc_rebind(Machine *m) { m->pc->m = m; }

static void pmi(CPU *c) { lapic_pmi(((Machine *)c->user)->pc, c->index); }

/* Cache events count on the requesting core's performance counters. */
static void cache_event(void *ctx, int core, int ev) {
    static const uint8_t map[] = {[CE_L1D_MISS] = PE_L1D_MISS, [CE_L1I_MISS] = PE_L1I_MISS,
                                  [CE_L2_REF] = PE_L2_REF, [CE_L2_MISS] = PE_L2_MISS, [CE_HITM] = PE_HITM};
    Machine *m = ctx;
    if (core < m->ncpu && m->cpus[core]) pmu_count(m->cpus[core], map[ev], 1, m->cpus[core]->cpl);
}

/* Wire CPU i to the machine (after a reset cleared it). */
static void setup_cpu(Machine *m, CPU *c, int i) {
    c->mem = m->mem;
    c->user = m;
    c->index = i;
    c->ncpu = m->ncpu;
    c->cache = m->cache;
    c->on_port_in = port_in;
    c->on_port_out = port_out;
    c->read_tsc = tsc_read;
    c->get_interrupt = get_interrupt;
    c->msr_hook = apic_msr;
    c->pmi_hook = pmi;
    c->tpr_hook = apic_tpr;
    c->on_mmio = apic_mmio;
    c->mmio_lo = IOAPIC_BASE;
    c->mmio_hi = LAPIC_BASE + 0x1000;
    c->cpuid_1d |= 1u << 9;  /* APIC */
    c->cpuid_1c |= 1u << 24; /* TSC deadline timer */
}

/* INIT: an application processor resets and waits for a STARTUP IPI.
 * The APIC base MSR, the local APIC ID and the A20 state survive. */
void pc_cpu_init(PC *pc, int i) {
    Machine *m = pc->m;
    CPU *c = m->cpus[i];
    uint64_t apic_base = c->msr.apic_base ? c->msr.apic_base : LAPIC_BASE | 0x800;
    uint64_t a20 = m->cpu.a20_mask ? m->cpu.a20_mask : ~0ULL;
    c->icache = m->cpu.icache; /* the decoded-instruction cache is shared (keyed by physical address) */
    cpu_reset_system(c);
    setup_cpu(m, c, i);
    c->msr.apic_base = apic_base;
    c->a20_mask = a20;
    c->wait_sipi = 1;
    lapic_reset(pc, i);
    pc_update_irq(pc);
}

/* STARTUP: real mode at vector:0000 (physical vector * 4 KiB). */
void pc_cpu_sipi(PC *pc, int i, int vector) {
    CPU *c = pc->m->cpus[i];
    if (!c->wait_sipi) return;
    c->wait_sipi = 0;
    c->halted = 0;
    load_seg(c, CS, (uint16_t)(vector << 8));
    c->rip = 0;
}

/* ---- programs from the editor ---- */

#define ESYS_GDT (PC_EDITOR_SYS + 0x0000)
#define ESYS_IDT32 (PC_EDITOR_SYS + 0x1000)
#define ESYS_IDT64 (PC_EDITOR_SYS + 0x2000)
#define ESYS_STUBS (PC_EDITOR_SYS + 0x3000)
#define ESYS_PML4 (PC_EDITOR_SYS + 0x4000) /* then the PDPT and four page directories */

/* GDT, IDTs with one reporting stub per vector, identity page tables for 4 GiB. */
static void editor_tables(PC *pc) {
    Machine *m = pc->m;
    static const uint64_t gdt[4] = {0, 0x00CF9B000000FFFFULL, 0x00CF93000000FFFFULL, 0x00AF9B000000FFFFULL};
    machine_phys_poke(m, ESYS_GDT, gdt, sizeof gdt);
    static uint8_t stubs[256 * 8];
    static uint64_t idt32[256], idt64[512], pt[6 * 512];
    for (int v = 0; v < 256; v++) {
        const uint8_t stub[8] = {0xB0, (uint8_t)v, 0xE6, EXC_PORT, 0xF4, 0xEB, 0xFD, 0x90}; /* mov al,v; out; hlt; jmp hlt */
        memcpy(stubs + v * 8, stub, 8);
        uint64_t off = ESYS_STUBS + (uint64_t)v * 8;
        idt32[v] = (off & 0xFFFF) | 0x08ULL << 16 | 0x8E00ULL << 32 | (off >> 16) << 48;  /* interrupt gates */
        idt64[v * 2] = (off & 0xFFFF) | 0x18ULL << 16 | 0x8E00ULL << 32 | ((off >> 16) & 0xFFFF) << 48;
        idt64[v * 2 + 1] = off >> 32;
    }
    pt[0] = (ESYS_PML4 + 0x1000) | 3;                      /* PML4[0] -> PDPT */
    for (int k = 0; k < 4; k++) {
        pt[512 + k] = (ESYS_PML4 + 0x2000 + 0x1000ULL * (uint64_t)k) | 3; /* PDPT[k] -> page directory k */
        for (int j = 0; j < 512; j++) pt[1024 + k * 512 + j] = ((uint64_t)(k * 512 + j) << 21) | 0x83; /* 2 MiB pages */
    }
    machine_phys_poke(m, ESYS_STUBS, stubs, sizeof stubs);
    machine_phys_poke(m, ESYS_IDT32, idt32, sizeof idt32);
    machine_phys_poke(m, ESYS_IDT64, idt64, sizeof idt64);
    machine_phys_poke(m, ESYS_PML4, pt, sizeof pt);
}

int pc_check_load(Machine *m, int bits, uint64_t addr, uint64_t size, char *err, size_t errn) {
    PC *pc = m->pc;
    uint64_t end = addr + size;
    if (addr < 0x500) {
        snprintf(err, errn, "0x%llx is in the interrupt vector table / BIOS data area (below 0x500)",
                 (unsigned long long)addr);
        return -1;
    }
    if (bits == 16) {
        if (addr & 15) {
            snprintf(err, errn, "16-bit code needs a 16-byte aligned address (it runs in segment addr >> 4)");
            return -1;
        }
        if (size > 0xFFFF || end > 0x9FC00) {
            snprintf(err, errn, "16-bit code must fit in 64 KiB below 0x9FC00 (the EBDA)");
            return -1;
        }
        return 0;
    }
    if (end > 0xA0000 && addr < 0x100000) {
        snprintf(err, errn, "0x%llx-0x%llx overlaps video memory and the BIOS (0xA0000-0xFFFFF)",
                 (unsigned long long)addr, (unsigned long long)end);
        return -1;
    }
    if (end > PC_EDITOR_SYS && addr < PC_EDITOR_SYS + 0x10000) {
        snprintf(err, errn, "0x100000-0x10FFFF holds the editor's GDT, IDTs and page tables");
        return -1;
    }
    if (end > pc->lowmem || (bits == 64 && end > 0x100000000ULL)) {
        snprintf(err, errn, "0x%llx-0x%llx is beyond RAM (%llu MiB below 4 GiB)", (unsigned long long)addr,
                 (unsigned long long)end, (unsigned long long)(pc->lowmem >> 20));
        return -1;
    }
    return 0;
}

void pc_cpu_start(Machine *m, int i, int bits, uint64_t base, uint64_t entry, uint64_t stack) {
    PC *pc = m->pc;
    CPU *c = m->cpus[i];
    uint64_t apic_base = c->msr.apic_base;
    if (i) c->icache = m->cpu.icache; /* shared, keyed by physical address */
    cpu_reset_system(c);
    setup_cpu(m, c, i);
    c->msr.apic_base = apic_base;
    pc_set_a20(pc, 1);
    c->cr0 = (c->cr0 & ~(CR0_CD | CR0_NW)) | CR0_MP | CR0_NE; /* caches on, x87 errors as exceptions */
    c->cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;                     /* SSE usable */
    if (bits == 16) {
        uint16_t seg = (uint16_t)(base >> 4);
        for (int s = 0; s < 6; s++) load_seg(c, s, seg);
        c->rip = entry - base;
        c->r[RSP] = stack - base;
    } else {
        editor_tables(pc);
        c->gdtr.base = ESYS_GDT;
        c->gdtr.limit = 4 * 8 - 1;
        if (bits == 64) {
            c->idtr.base = ESYS_IDT64;
            c->idtr.limit = 256 * 16 - 1;
            c->cr4 |= CR4_PAE;
            c->cr3 = ESYS_PML4;
            c->efer |= EFER_LME | EFER_LMA;
            c->cr0 |= CR0_PE | CR0_PG;
            load_seg(c, CS, 0x18);
        } else {
            c->idtr.base = ESYS_IDT32;
            c->idtr.limit = 256 * 8 - 1;
            c->cr0 |= CR0_PE;
            load_seg(c, CS, 0x08);
        }
        for (int s = 0; s < 6; s++)
            if (s != CS) load_seg(c, s, 0x10);
        c->rip = entry;
        c->r[RSP] = stack;
    }
    cpu_update_mode(c);
    cpu_tlb_flush(c, 0);
    c->rflags = 2; /* interrupts off */
    c->wait_sipi = 0;
    c->halted = 0;
    m->view_cpu = i;
    if (m->state != MS_WAIT_INPUT) m->state = MS_READY;
    m->last_stop = STOP_NONE;
    m->bits = machine_cpu(m)->bits;
}

/* An empty machine (no image, disk or kernel; the UI's editor fills it):
 * the bootstrap processor waits with interrupts off. */
static void park_bsp(PC *pc) {
    CPU *c = &pc->m->cpu;
    static const uint8_t park[] = {0xFA, 0xF4, 0xEB, 0xFD}; /* cli; hlt; jmp hlt */
    machine_phys_poke(pc->m, 0xFFF80, park, sizeof park);
    load_seg(c, CS, 0xF000);
    c->rip = 0xFF80;
    snprintf(pc->m->status, sizeof pc->m->status, "empty machine: write a program in the editor and load it onto a CPU");
}

int pc_load(Machine *m, char *err, size_t errn) {
    PC *pc = m->pc;
    CPU *c = &m->cpu;
    if (m->pc_hda && open_disk(&pc->hda, m->pc_hda, 0, err, errn)) return -1;
    if (m->pc_fda && open_disk(&pc->fda, m->pc_fda, 1, err, errn)) return -1;
    mem_map(m->mem, 0, pc->lowmem, MEM_R | MEM_W | MEM_X, "ram");
    if (pc->highmem) mem_map(m->mem, 0x100000000ULL, pc->highmem, MEM_R | MEM_W | MEM_X, "ram (above 4G)");
    mem_map(m->mem, 0xFFFC0000ULL, 0x40000, MEM_R | MEM_X, "bios");
    m->ncpu = m->pc_smp < 1 ? 1 : m->pc_smp > MAX_CPUS ? MAX_CPUS : m->pc_smp;
    if (m->pc_cache) { /* write-back RAM; the legacy video/ROM hole and MMIO are uncached */
        m->cache = cache_new(m->mem, m->ncpu);
        m->cache->on_event = cache_event;
        m->cache->ev_ctx = m;
        cache_add_range(m->cache, 0, 0xA0000);
        cache_add_range(m->cache, 0x100000, pc->lowmem);
        if (pc->highmem) cache_add_range(m->cache, 0x100000000ULL, 0x100000000ULL + pc->highmem);
    }
    c->mem = m->mem;
    cpu_reset_system(c);
    setup_cpu(m, c, 0);
    for (int i = 1; i < m->ncpu; i++) m->cpus[i] = calloc(1, sizeof(CPU));
    for (int i = 1; i < m->ncpu; i++) pc_cpu_init(pc, i); /* scans every CPU: allocate them first */
    m->os = OS_PC;

    pc->console = m->pc_console;
    pc->realtime = m->pc_console ? isatty(STDIN_FILENO) : 0;
    if (!m->echo_output) pc->realtime = 1; /* web UI */
    pc->real_start = mono_ns();
    pc->next_poll = 0;
    devices_reset(pc);
    bios_post(pc);

    int r;
    if (m->pc_kernel) r = bios_boot_linux(pc, err, errn);
    else if (!m->nfiles && !pc->hda.present && !pc->fda.present) park_bsp(pc), r = 0;
    else if (!m->nfiles) r = bios_boot_disk(pc, err, errn);
    else {
        r = pc_load_image(m, err, errn);
        if (!r) { /* as if firmware had loaded the image: CS=0, stack below 0x7C00 */
            load_seg(c, CS, 0);
            c->rip = m->entry;
            c->r[RSP] = 0x7C00;
            c->r[RDX] = 0x80;
        }
    }
    if (r) return -1;
    m->bits = c->bits;
    if (pc->console && isatty(STDIN_FILENO)) setup_terminal();
    pc_resched(pc);
    return 0;
}
