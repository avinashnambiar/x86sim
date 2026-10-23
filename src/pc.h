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
/* The display as JSON: text mode cells or mode 13h pixels (for the web UI). */
void pc_screen(struct Machine *m, void *buf);

#endif
