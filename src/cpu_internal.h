/* Helpers shared by the execution units (cpu.c, sse.c, ...). Not a public API. */
#ifndef CPU_INTERNAL_H
#define CPU_INTERNAL_H

#include "cpu.h"
#include "decode.h"

#define MASK(s) ((s) >= 8 ? ~0ULL : ((1ULL << ((s) * 8)) - 1))
#define SIGNBIT(s) (1ULL << ((s) * 8 - 1))

typedef unsigned __int128 u128;
typedef __int128 s128;

static inline uint64_t sext(uint64_t v, int s) {
    switch (s) {
    case 1: return (uint64_t)(int64_t)(int8_t)v;
    case 2: return (uint64_t)(int64_t)(int16_t)v;
    case 4: return (uint64_t)(int64_t)(int32_t)v;
    }
    return v;
}

static inline uint64_t getreg(CPU *c, int r, int s, int rex) {
    switch (s) {
    case 1:
        if (!rex && r >= 4 && r < 8) return (c->r[r - 4] >> 8) & 0xFF;
        return c->r[r] & 0xFF;
    case 2: return c->r[r] & 0xFFFF;
    case 4: return c->r[r] & 0xFFFFFFFF;
    }
    return c->r[r];
}

static inline void setreg(CPU *c, int r, int s, int rex, uint64_t v) {
    switch (s) {
    case 1:
        if (!rex && r >= 4 && r < 8) c->r[r - 4] = (c->r[r - 4] & ~0xFF00ULL) | ((v & 0xFF) << 8);
        else c->r[r] = (c->r[r] & ~0xFFULL) | (v & 0xFF);
        break;
    case 2: c->r[r] = (c->r[r] & ~0xFFFFULL) | (v & 0xFFFF); break;
    case 4: c->r[r] = v & 0xFFFFFFFF; break; /* 32-bit writes zero-extend */
    default: c->r[r] = v;
    }
}

_Noreturn void raise_exc(CPU *c, int vec, uint64_t code, const char *msg);
_Noreturn void unimplemented(CPU *c, const Insn *in);
uint64_t linear(CPU *c, int seg, uint64_t off);
uint64_t rdm(CPU *c, uint64_t a, int s);
void wrm(CPU *c, uint64_t a, int s, uint64_t v);
void rdm128(CPU *c, uint64_t a, void *dst);
void wrm128(CPU *c, uint64_t a, const void *src);
uint64_t ea_offset(CPU *c, const Insn *in);
uint64_t ea_linear(CPU *c, const Insn *in);
uint64_t rd_op(CPU *c, const Insn *in, int k);
void wr_op(CPU *c, const Insn *in, int k, uint64_t v);

/* sse.c */
void exec_sse(CPU *c, const Insn *in);

#endif
