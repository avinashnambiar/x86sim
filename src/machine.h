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

enum { OS_NONE, OS_LINUX, OS_DOS };

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
#define TRACE_LEN 512
#define SYSLOG_LEN 256

typedef struct Machine {
    CPU cpu;
    Mem *mem;
    int bits;
    int os;

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

void machine_init(Machine *m);
void machine_destroy(Machine *m);
int machine_load(Machine *m, int nfiles, char **files, int argc, char **argv, int bits, char *err, size_t errn);
int machine_reset(Machine *m, char *err, size_t errn);

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
int machine_disasm(Machine *m, uint64_t addr, Insn *in, char *text, size_t n);
int machine_parse_addr(Machine *m, const char *s, uint64_t *addr);

void machine_output(Machine *m, const void *data, size_t n);
void machine_log(Machine *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* loader.c */
int load_program(Machine *m, char *err, size_t errn);
/* os.c */
void os_install(Machine *m);

#endif
