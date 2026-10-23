/* A simulated machine: CPU + memory + loaded program + OS emulation +
 * debugger state. Shared by the CLI, the CLI debugger and the web UI. */
#ifndef MACHINE_H
#define MACHINE_H

#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "decode.h"

typedef struct {
    uint64_t addr, size;
    char *name;
} Symbol;

typedef struct {
    char *data;
    size_t len, cap;
} Buf;

void buf_append(Buf *b, const void *data, size_t n);
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void buf_free(Buf *b);

enum { OS_NONE, OS_LINUX, OS_DOS, OS_PC };

enum MachineState {
    MS_READY,       /* can execute */
    MS_WAIT_INPUT,  /* blocked reading stdin (UI mode) */
    MS_EXITED,
    MS_FAULT,
    MS_HALTED,
};

enum StopReason {
    STOP_NONE,
    STOP_STEP,
    STOP_BREAKPOINT,
    STOP_TRAP,       /* int3 executed by the program */
    STOP_EXIT,
    STOP_FAULT,
    STOP_HALT,
    STOP_INPUT,
    STOP_LIMIT,      /* instruction budget used up */
};

#define MAX_BP 64
#define MAX_CPUS 255 /* xAPIC: 8-bit APIC IDs, 0xFF is broadcast */
#define TRACE_LEN 512
#define SYSLOG_LEN 256

struct PC;

typedef struct Machine {
    CPU cpu;            /* the bootstrap processor */
    CPU *cpus[MAX_CPUS];/* cpus[0] == &cpu; application processors after it (PC mode) */
    int ncpu;
    int cur_cpu;        /* CPU executing right now */
    int view_cpu;       /* CPU shown by the debugger and the web UI */
    Mem *mem;
    int bits;
    int os;

    /* full-system PC mode (--pc) */
    int pc_mode;
    uint64_t pc_ram_mb;
    struct PC *pc;
    char *pc_hda, *pc_fda;          /* disk images */
    char *pc_kernel, *pc_initrd, *pc_append; /* Linux boot protocol */
    int pc_boot;                    /* boot drive: 'a', 'c' or 0 (hard disk, then floppy) */
    int pc_console;                 /* keystrokes come from the host terminal */
    int yield_on_idle;              /* web UI: end a run chunk when the guest idles */
    struct AsmLoad *asm_loads;      /* programs loaded from the UI's editor (asm.c) */
    int nasm_loads;
    int pc_smp;                     /* --smp: number of CPUs (1-MAX_CPUS) */
    int pc_cache;                   /* --cache: simulate L1/L2 caches with MESI coherence */
    struct CacheSys *cache;
    uint64_t cycles;                /* PC mode: instruction rounds (the virtual clock) */

    /* what was loaded, kept for reset */
    int nfiles;
    char **files;
    int argc;
    char **argv;
    int bits_override;

    uint64_t entry;
    Symbol *syms;
    int nsyms;
    uint64_t brk_start, brk_cur, mmap_next;

    Buf out;            /* everything the guest wrote to stdout/stderr */
    int echo_output;    /* also copy guest output to the host stdout */
    int use_host_stdin;
    Buf in;             /* pending guest stdin (UI mode) */
    size_t in_pos;

    int state;
    int exit_code;
    char status[256];
    int last_stop;

    uint64_t bp[MAX_BP];
    int nbp;

    uint64_t trace[TRACE_LEN];
    uint64_t trace_count;
    int trace_print;

    char *syslog[SYSLOG_LEN];
    uint64_t syslog_count;
    int log_syscalls;
    int print_syscalls; /* echo syscall log lines to stderr (--strace) */
} Machine;

/* The CPU the debugger and the UI look at. */
static inline CPU *machine_cpu(Machine *m) { return m->cpus[m->view_cpu] ? m->cpus[m->view_cpu] : &m->cpu; }

void machine_init(Machine *m);
void machine_destroy(Machine *m);
int machine_load(Machine *m, int nfiles, char **files, int argc, char **argv, int bits, char *err, size_t errn);
int machine_reset(Machine *m, char *err, size_t errn);
/* After a Machine struct was copied: point the CPUs and devices at the copy. */
void machine_rebind(Machine *m);

int machine_step(Machine *m);
int machine_run(Machine *m, uint64_t max_insns);
int machine_step_over(Machine *m, uint64_t max_insns);
int machine_step_out(Machine *m, uint64_t max_insns);
const char *stop_reason_name(int reason);

int machine_add_bp(Machine *m, uint64_t addr);
int machine_del_bp(Machine *m, uint64_t addr);
int machine_has_bp(Machine *m, uint64_t addr);

const Symbol *machine_find_symbol(Machine *m, uint64_t addr);
int machine_lookup_symbol(Machine *m, const char *name, uint64_t *addr);
int machine_symname(void *ctx, uint64_t addr, char *buf, size_t n);
size_t machine_peek(Machine *m, uint64_t lin, void *buf, size_t n);
/* Physical accesses that stay coherent with the simulated caches. */
size_t machine_phys_peek(Machine *m, uint64_t pa, void *buf, size_t n);
size_t machine_phys_poke(Machine *m, uint64_t pa, const void *buf, size_t n);
int machine_disasm(Machine *m, uint64_t addr, Insn *in, char *text, size_t n);
int machine_parse_addr(Machine *m, const char *s, uint64_t *addr);

void machine_output(Machine *m, const void *data, size_t n);
void machine_log(Machine *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* loader.c */
int load_program(Machine *m, char *err, size_t errn);
/* PC mode: link a relocatable object for address base and write it to
 * physical address phys; returns the entry point (_start or the start of
 * the code) and the image size. */
int pc_link_object(Machine *m, const uint8_t *data, size_t size, uint64_t base, uint64_t phys, uint64_t *entry,
                   uint64_t *img_size, char *err, size_t errn);
uint64_t pc_object_size(const uint8_t *data, size_t size);

/* asm.c: programs written in the web UI. Assemble (clang) and load at addr,
 * then start CPU cpu in that mode (cpu < 0: only write memory). Returns 0,
 * or -1 with clang's diagnostics or the reason in diag. Loads are remembered
 * and replayed after a machine reset. */
typedef struct AsmLoad {
    int cpu, bits;
    uint64_t addr, entry, size, stack;
    uint8_t *obj;
    size_t objn;
} AsmLoad;
int asm_load(Machine *m, const char *src, size_t n, int bits, int intel, int cpu, uint64_t addr, AsmLoad *info,
             char *diag, size_t diagn);
int asm_check(const char *src, size_t n, int bits, int intel, char *diag, size_t diagn); /* assemble only */
void asm_replay(Machine *m);
void asm_forget(Machine *m);
/* os.c */
void os_install(Machine *m);

#endif
