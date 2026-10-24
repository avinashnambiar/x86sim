/* PC machine internals shared by pc.c (core and ISA devices), apic.c (local
 * APIC and I/O APIC) and bios.c (firmware, disk boot and Linux boot). */
#ifndef PC_INTERNAL_H
#define PC_INTERNAL_H

#include <stdint.h>

#include "cpu_internal.h"
#include "machine.h"
#include "pc.h"

#define NS_NEVER UINT64_MAX
#define LAPIC_BASE 0xFEE00000ULL
#define IOAPIC_BASE 0xFEC00000ULL
#define BIOS_STUBS 0xF0000u /* one 16-byte HLE stub per interrupt vector */
#define BIOS_PORT 0xEF      /* the stubs trap into C with OUT to this port */
#define CACHE_PORT 0xEC     /* debug: OUT dword = physical address, IN dword = its line states */
#define CACHE_CORE_PORT 0xED /* debug: OUT byte = first of the four cores CACHE_PORT reports */
#define EXC_PORT 0xEE        /* the editor's exception handlers report their vector here */

typedef struct {
    uint8_t irr, isr, imr, lines, elcr;
    uint8_t base, step, need_icw4, single, auto_eoi, read_isr, poll, prio;
} Pic;

typedef struct {
    uint16_t count;      /* reload value (0 = 65536) */
    uint8_t mode, rw, bcd, gate;
    uint64_t load_ns;    /* when counting started */
    uint64_t irq_k;      /* channel 0: periodic interrupts already raised */
    int armed;           /* channel 0: an interrupt is still due */
    uint16_t latch;
    int latched, status_latched, read_hi, write_hi;
    uint8_t status;
    uint8_t wlow;
} PitChan;

typedef struct {
    uint8_t ram[128];
    uint8_t index;
    int64_t base_time;   /* host seconds at reset */
    uint64_t next_periodic, next_update;
} Cmos;

#define QLEN 256
typedef struct {
    uint8_t q[QLEN];
    int head, tail;
} ByteQ;

typedef struct {
    uint8_t status, config, data, out_port;
    int pending_cmd;     /* controller command awaiting a data byte */
    int kbd_cmd;         /* keyboard command awaiting an argument */
    int kbd_scan;        /* keyboard scanning enabled */
    int aux_cmd;
    ByteQ kq, aq;
    int esc;             /* host escape-sequence parser state */
} Kbc;

#define RXLEN 4096
typedef struct {
    uint8_t ier, lcr, mcr, lsr_extra, scr, dll, dlm, fcr;
    int thre_pending;
    uint8_t rx[RXLEN];
    int rx_head, rx_tail;
    uint8_t msr_last;
} Uart;

typedef struct {
    uint8_t cfg[3][256]; /* 00.0 host bridge, 01.0 ISA bridge, 01.1 IDE */
    uint32_t addr;
} Pci;

typedef struct {
    uint8_t *data;
    uint64_t size;       /* bytes */
    int cyls, heads, spt;
    int present;
} Disk;

typedef struct {
    uint8_t feature, error, nsect, sect, lcyl, hcyl, select, status, control;
    uint8_t hob_nsect, hob_sect, hob_lcyl, hob_hcyl; /* previous values (LBA48) */
    uint8_t buf[512 * 16];
    int pos, len;        /* data transfer position within buf */
    uint64_t lba;
    uint32_t remain;     /* sectors still to transfer */
    int writing, multiple, mult_count;
    int irq;
} Ide;

typedef struct {
    uint32_t id, tpr, ldr, dfr, svr, esr, icr_lo, icr_hi;
    uint32_t irr[8], isr[8], tmr[8];
    uint32_t lvt[7];     /* CMCI timer thermal perf LINT0 LINT1 error */
    uint32_t init_count, div_conf;
    uint64_t timer_start, timer_k, deadline;
    int timer_armed;
} Lapic;

typedef struct {
    uint32_t sel, id;
    uint64_t redir[24];
    uint32_t lines;
} Ioapic;

typedef struct {
    uint8_t crtc_index, crtc[32];
    uint8_t seq_index, seq[8], gc_index, gc[16], attr_index, attr[32], attr_flip;
    uint8_t dac_windex, dac_rindex, dac_comp, dac[256 * 3];
    uint8_t misc;
} Vga;

/* Host keystrokes waiting until a device is ready to take them. */
#define INLEN 65536
typedef struct {
    uint8_t q[INLEN];
    int head, tail;
} InQ;

struct PC {
    Machine *m;
    uint64_t ram, lowmem, highmem; /* RAM below 4 GiB and above it (bytes) */
    uint64_t idle_ns;
    uint64_t next_event;
    uint64_t next_poll;
    int console;         /* read keystrokes from the host terminal */
    int realtime;        /* keep idle time in step with the wall clock */
    uint64_t real_start; /* host monotonic ns at reset */
    int stdin_eof;

    Pic pic[2];
    PitChan pit[3];
    uint8_t port61;
    Cmos cmos;
    Kbc kbc;
    Uart uart;
    Pci pci;
    Ide ide;
    Disk hda, fda;
    Lapic lapic[MAX_CPUS]; /* one per CPU, APIC ID = CPU index */
    Ioapic ioapic;
    Vga vga;
    uint8_t port92;
    uint16_t isa_lines;  /* current ISA IRQ input levels */
    InQ in_uart, in_kbd;
    uint32_t cache_query;  /* address for CACHE_PORT */
    uint8_t cache_core;    /* first core for CACHE_PORT */

    /* firmware state */
    uint8_t kb_flags, kb_e0;
    uint8_t disk_status;
};

/* The CPU executing right now (port and MMIO accesses come from it). */
static inline CPU *pc_cpu(PC *pc) { return pc->m->cpus[pc->m->cur_cpu]; }

/* pc.c */
uint64_t pc_now(PC *pc);
void pc_cpu_init(PC *pc, int i);              /* INIT: CPU i waits for a STARTUP IPI */
void pc_cpu_sipi(PC *pc, int i, int vector);  /* STARTUP: real mode at vector * 4 KiB */
void pc_set_irq(PC *pc, int irq, int level);
void pc_pulse_irq(PC *pc, int irq);
void pc_update_irq(PC *pc);
void pc_resched(PC *pc);
int pic_output(PC *pc);
int pic_ack(PC *pc);
void pic_eoi(PC *pc, int irq);
uint32_t pc_port_read(PC *pc, uint16_t port, int size);
void pc_port_write(PC *pc, uint16_t port, int size, uint32_t val);
uint8_t kbc_read_data(PC *pc);
void pc_set_a20(PC *pc, int on);
uint32_t pci_cfg_read(PC *pc, int bus, int devfn, int reg, int size);
void pci_cfg_write(PC *pc, int bus, int devfn, int reg, int size, uint32_t val);
int pci_find(PC *pc, uint16_t vendor, uint16_t device, int index);
int pci_find_class(PC *pc, uint32_t cls, int index);
void uart_tx(PC *pc, uint8_t ch);
int disk_rw(Disk *d, uint64_t lba, uint32_t count, void *buf, int write);
void pc_system_reset(PC *pc);

/* apic.c */
void apic_reset(PC *pc);
void lapic_reset(PC *pc, int i);
int apic_mmio(CPU *c, uint64_t pa, void *buf, int n, int write);
int lapic_enabled(PC *pc, int i);
int lapic_pending(PC *pc, int i);
int lapic_ack(PC *pc, int i);
int lapic_accepts_pic(PC *pc, int i);
void lapic_accept(PC *pc, int i, int vector, int level);
uint64_t lapic_next_event(PC *pc);
void lapic_tick(PC *pc, uint64_t now);
void lapic_pmi(PC *pc, int i);
int apic_msr(CPU *c, uint32_t msr, uint64_t *val, int write);
void apic_tpr(CPU *c, int tpr);
void ioapic_set_irq(PC *pc, int pin, int level);

/* bios.c */
void bios_post(PC *pc);
void bios_call(PC *pc, int vector);
int bios_boot_disk(PC *pc, char *err, size_t errn);
int bios_boot_linux(PC *pc, char *err, size_t errn);

#endif
