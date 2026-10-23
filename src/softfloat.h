/* Software 80-bit extended-precision floating point with x87 semantics:
 * IEEE rounding (4 modes), precision control (24/53/64-bit significands),
 * x87 NaN propagation, exception flags, and correctly rounded
 * transcendentals computed with a 128-bit-significand internal format. */
#ifndef SOFTFLOAT_H
#define SOFTFLOAT_H

#include <stdint.h>

typedef struct {
    uint64_t sig;  /* explicit integer bit at bit 63 */
    uint16_t se;   /* bit 15 sign, bits 14..0 biased exponent */
} fx80;

enum { FX_IE = 1, FX_DE = 2, FX_ZE = 4, FX_OE = 8, FX_UE = 16, FX_PE = 32 };
enum { RC_NEAR = 0, RC_DOWN = 1, RC_UP = 2, RC_ZERO = 3 };
enum { FXC_ZERO, FXC_NORMAL, FXC_DENORMAL, FXC_INF, FXC_QNAN, FXC_SNAN, FXC_UNSUPPORTED };
enum { CMP_LT, CMP_EQ, CMP_GT, CMP_UN };

typedef struct {
    int rc;          /* rounding control */
    int prec;        /* significand bits for arithmetic results: 24, 53 or 64 */
    unsigned flags;  /* accumulated FX_* exception flags */
    int tiny;        /* last rounded result was tiny (after rounding) */
    int roundup;     /* last rounding increased the magnitude (x87 C1) */
    int daz, ftz;    /* SSE: denormals-are-zero, flush-to-zero */
} FxEnv;

int fx_class(fx80 a);
static inline int fx_sign(fx80 a) { return a.se >> 15; }
static inline int fx_isnan(fx80 a) { int c = fx_class(a); return c == FXC_QNAN || c == FXC_SNAN; }
fx80 fx_indefinite(void);
fx80 fx_make(int sign, int exp, uint64_t sig);
static inline fx80 fx_neg(fx80 a) { a.se ^= 0x8000; return a; }
static inline fx80 fx_abs(fx80 a) { a.se &= 0x7FFF; return a; }

fx80 fx_add(FxEnv *e, fx80 a, fx80 b);
fx80 fx_sub(FxEnv *e, fx80 a, fx80 b);
fx80 fx_mul(FxEnv *e, fx80 a, fx80 b);
fx80 fx_div(FxEnv *e, fx80 a, fx80 b);
fx80 fx_sqrt(FxEnv *e, fx80 a);
int fx_compare(FxEnv *e, fx80 a, fx80 b, int quiet);

fx80 fx_from_i64(int64_t v);
int64_t fx_to_int(FxEnv *e, fx80 a, int bits, int truncate);
fx80 fx_round_int(FxEnv *e, fx80 a);
fx80 fx_from_f64(FxEnv *e, uint64_t bits);
fx80 fx_from_f32(FxEnv *e, uint32_t bits);
uint64_t fx_to_f64(FxEnv *e, fx80 a);
uint32_t fx_to_f32(FxEnv *e, fx80 a);
fx80 fx_round_prec(FxEnv *e, fx80 a); /* round to e->prec (used after loads) */

fx80 fx_scale(FxEnv *e, fx80 a, fx80 b);
fx80 fx_prem(FxEnv *e, fx80 a, fx80 b, int ieee, int *quotient_bits, int *partial);
void fx_xtract(FxEnv *e, fx80 a, fx80 *exponent, fx80 *significand);
int fx_from_bcd(const uint8_t bcd[10], fx80 *out);
void fx_to_bcd(FxEnv *e, fx80 a, uint8_t bcd[10]);

/* Transcendentals. Results are correctly rounded to 64 bits in practice. */
enum { FXK_ONE, FXK_L2T, FXK_L2E, FXK_PI, FXK_LG2, FXK_LN2, FXK_ZERO };
fx80 fx_const(FxEnv *e, int which);
int fx_sincos(FxEnv *e, fx80 a, fx80 *s, fx80 *c);   /* returns 0 if |a| >= 2^63 (C2) */
int fx_tan(FxEnv *e, fx80 a, fx80 *t);
fx80 fx_atan2(FxEnv *e, fx80 y, fx80 x);
fx80 fx_2xm1(FxEnv *e, fx80 a);
fx80 fx_yl2x(FxEnv *e, fx80 y, fx80 x);
fx80 fx_yl2xp1(FxEnv *e, fx80 y, fx80 x);

/* SSE scalar arithmetic on IEEE single/double bit patterns, with SSE NaN
 * rules (first source wins, default NaN is negative QNaN) and DAZ/FTZ. */
enum { SOP_ADD, SOP_SUB, SOP_MUL, SOP_DIV, SOP_MIN, SOP_MAX, SOP_SQRT };
uint32_t sf32_op(FxEnv *e, int op, uint32_t a, uint32_t b);
uint64_t sf64_op(FxEnv *e, int op, uint64_t a, uint64_t b);
int sf32_cmp(FxEnv *e, uint32_t a, uint32_t b, int signal_qnan);
int sf64_cmp(FxEnv *e, uint64_t a, uint64_t b, int signal_qnan);
uint32_t sf32_from_i64(FxEnv *e, int64_t v);
uint64_t sf64_from_i64(FxEnv *e, int64_t v);
uint32_t sf32_from_u64(FxEnv *e, uint64_t v);
uint64_t sf64_from_u64(FxEnv *e, uint64_t v);
int64_t sf32_to_int(FxEnv *e, uint32_t a, int bits, int truncate);
int64_t sf64_to_int(FxEnv *e, uint64_t a, int bits, int truncate);
uint64_t sf32_to_f64(FxEnv *e, uint32_t a);
uint32_t sf64_to_f32(FxEnv *e, uint64_t a);
uint32_t sf32_round(FxEnv *e, uint32_t a, int rc, int suppress_pe);
uint64_t sf64_round(FxEnv *e, uint64_t a, int rc, int suppress_pe);
int sf32_class(uint32_t a);
int sf64_class(uint64_t a);

#endif
