/* Architectural CPU state and the single-instruction step interface. */
#ifndef CPU_H
#define CPU_H

#include <setjmp.h>
#include <stdint.h>

#include "mem.h"

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
#define FL_AC 0x40000ULL
#define FL_ID 0x200000ULL
#define FL_ARITH (FL_CF | FL_PF | FL_AF | FL_ZF | FL_SF | FL_OF)

enum {
    EXC_DE = 0, EXC_DB = 1, EXC_BP = 3, EXC_OF = 4, EXC_BR = 5, EXC_UD = 6,
    EXC_NM = 7, EXC_GP = 13, EXC_PF = 14, EXC_MF = 16, EXC_AC = 17, EXC_XM = 19
};

typedef union {
    uint8_t b[64];
    uint16_t w[32];
    uint32_t d[16];
    uint64_t q[8];
} Vec512;

typedef enum { STEP_OK, STEP_EXCEPTION, STEP_STOP } StepResult;

typedef struct CPU CPU;
struct CPU {
    uint64_t r[16];
    uint64_t rip;
    uint64_t rflags;
    uint16_t sreg[6];
    uint64_t sbase[6];
    Vec512 zmm[32];   /* XMM/YMM/ZMM register file (only XMM used so far) */
    uint32_t mxcsr;
    int bits;         /* code size: 16 (real mode), 32 or 64 */
    int cpl;          /* 0 kernel/real mode, 3 user */
    Mem *mem;
    uint64_t icount;

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
    int stop_request; /* hooks set this to stop after the current instruction */
};

void cpu_init(CPU *c, Mem *m, int bits);
StepResult cpu_step(CPU *c);
const char *cpu_exc_name(int vector);
void cpu_load_seg_real(CPU *c, int seg, uint16_t sel);

#endif
