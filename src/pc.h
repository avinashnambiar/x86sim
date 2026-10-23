/* A PC-compatible machine for full-system mode: RAM, I/O ports, devices and
 * a virtual clock (1 instruction = 1 ns; time skips ahead while halted). */
#ifndef PC_H
#define PC_H

#include <stddef.h>
#include <stdint.h>

struct Machine;
typedef struct PC PC;

PC *pc_create(struct Machine *m);
void pc_destroy(PC *pc);
void pc_rebind(struct Machine *m); /* the Machine struct moved */
/* Map memory, load the guest (boot image, .o files or kernel) and reset the CPU. */
int pc_load(struct Machine *m, char *err, size_t errn);
/* Called after every instruction batch to run devices whose time has come. */
void pc_tick(struct Machine *m);
/* The CPU executed HLT: advance time to the next device event. Returns 0 when
 * nothing can ever wake the CPU (interrupts disabled, no NMI source), 2 when
 * it waited in real time, 1 otherwise. */
int pc_idle(struct Machine *m);
uint64_t pc_time_ns(struct Machine *m);

/* Programs from the web UI's editor (asm.c). 16-bit code runs in real mode
 * in its own segment; 32-bit code in flat protected mode and 64-bit code in
 * long mode with the first 4 GiB identity-mapped, using a GDT (0x08 code32,
 * 0x10 data, 0x18 code64), IDTs whose handlers report unhandled exceptions
 * and page tables kept in a 64 KiB area at PC_EDITOR_SYS. Each CPU has a
 * default slot: code from the bottom, stack from the top. */
#define PC_SLOT16_BASE 0x10000ULL  /* cpu k: + k * PC_SLOT16 */
#define PC_SLOT16 0x1000ULL
#define PC_SLOT_BASE 0x200000ULL   /* 32/64-bit, cpu k: + k * PC_SLOT */
#define PC_SLOT 0x10000ULL
#define PC_EDITOR_SYS 0x100000ULL
/* Is [addr, addr + size) usable for a program of this mode (RAM, outside
 * the BIOS areas and the editor's tables)? 0 if so, else -1 with a reason. */
int pc_check_load(struct Machine *m, int bits, uint64_t addr, uint64_t size, char *err, size_t errn);
/* Put CPU i in the given mode and start it at entry (linear) with the stack
 * pointer at stack; 16-bit code uses segment base >> 4 for CS, DS, ES, SS. */
void pc_cpu_start(struct Machine *m, int i, int bits, uint64_t base, uint64_t entry, uint64_t stack);
/* The display as JSON: text mode cells or mode 13h pixels (for the web UI). */
void pc_screen(struct Machine *m, void *buf);

#endif
