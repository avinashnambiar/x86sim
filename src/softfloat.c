/* Software floating point for the x87 and SSE units.
 *
 * Every operation unpacks to a sign, an unbounded exponent and a 128-bit
 * significand (value = sig/2^127 * 2^(exp-16383)), computes an exact result
 * or one with a sticky bit, and goes through a single rounding routine
 * parameterized by precision and exponent range. That routine implements
 * the four IEEE rounding modes, x86 tininess-after-rounding, overflow per
 * rounding mode, and the x87 C1 "rounded up" indication.
 *
 * Transcendentals use a separate format with a 128-bit significand and
 * truncating arithmetic (~125 good bits), then round once to 64 bits.
 * Trig argument reduction uses Intel's 66-bit pi, as real x87 hardware does. */
#include "softfloat.h"

#include <math.h>
#include <string.h>

typedef unsigned __int128 u128;

#define BIAS 16383
#define TOP ((u128)1 << 127)

typedef struct { int sign; int exp; u128 sig; } Unp;
typedef struct { int emin, emax, prec; } Range;

static const Range R80 = {1, 0x7FFE, 64};
static const Range R64 = {15361, 17406, 53};
static const Range R32 = {16257, 16510, 24};

static int clz128(u128 x) {
    uint64_t hi = (uint64_t)(x >> 64);
    return hi ? __builtin_clzll(hi) : 64 + __builtin_clzll((uint64_t)x);
}

static u128 shr_jam(u128 x, int n) {
    if (n <= 0) return x;
    if (n >= 128) return x != 0;
    return (x >> n) | ((x << (128 - n)) != 0);
}

static void normalize(Unp *u) {
    if (!u->sig) return;
    int s = clz128(u->sig);
    u->sig <<= s;
    u->exp -= s;
}

/* ---- rounding ---------------------------------------------------------- */

static int round_inc(int rc, int sign, u128 sig, u128 lsb, u128 rem) {
    if (!rem) return 0;
    switch (rc) {
    case RC_NEAR: return rem > (lsb >> 1) || (rem == (lsb >> 1) && (sig & lsb));
    case RC_DOWN: return sign;
    case RC_UP: return !sign;
    }
    return 0;
}

/* Round a normalized (or zero) value to `prec` significand bits within the
 * exponent range. Infinity comes back as exp = emax+1, sig = TOP. */
static Unp round_to(FxEnv *e, Unp u, int emin, int emax, int prec) {
    e->roundup = 0;
    e->tiny = 0;
    if (!u.sig) return u;
    u128 lsb = (u128)1 << (128 - prec), mask = lsb - 1;
    int tiny = 0;
    if (u.exp < emin) {
        if (u.exp < emin - 1) tiny = 1;
        else {
            /* tininess after rounding: does rounding at unbounded exponent carry to 2^emin? */
            u128 t = u.sig & ~mask;
            tiny = !(round_inc(e->rc, u.sign, u.sig, lsb, u.sig & mask) && t + lsb < t);
        }
        u.sig = shr_jam(u.sig, emin - u.exp);
        u.exp = emin;
    }
    if (tiny && e->ftz) {
        e->flags |= FX_UE | FX_PE;
        e->tiny = 1;
        u.sig = 0;
        return u;
    }
    u128 rem = u.sig & mask;
    int inc = round_inc(e->rc, u.sign, u.sig, lsb, rem);
    if (rem) {
        e->flags |= FX_PE;
        if (tiny) e->flags |= FX_UE;
    }
    e->tiny = tiny;
    u.sig &= ~mask;
    if (inc) {
        u128 old = u.sig;
        u.sig += lsb;
        e->roundup = 1;
        if (u.sig < old) { u.sig = TOP; u.exp++; }
    }
    if (u.exp > emax) {
        e->flags |= FX_OE | FX_PE;
        if (e->rc == RC_NEAR || (e->rc == RC_UP && !u.sign) || (e->rc == RC_DOWN && u.sign)) {
            u.exp = emax + 1;
            u.sig = TOP;
            e->roundup = 1;
        } else {
            u.exp = emax;
            u.sig = ~mask;
            e->roundup = 0;
        }
    }
    return u;
}

/* ---- 80-bit pack/unpack ---------------------------------------------------- */

int fx_class(fx80 a) {
    int e = a.se & 0x7FFF;
    if (e == 0) return a.sig == 0 ? FXC_ZERO : FXC_DENORMAL;
    if (!(a.sig >> 63)) return FXC_UNSUPPORTED;
    if (e == 0x7FFF) {
        uint64_t frac = a.sig & 0x7FFFFFFFFFFFFFFFULL;
        if (!frac) return FXC_INF;
        return (frac >> 62) ? FXC_QNAN : FXC_SNAN;
    }
    return FXC_NORMAL;
}

fx80 fx_make(int sign, int exp, uint64_t sig) {
    fx80 r = {sig, (uint16_t)((sign << 15) | exp)};
    return r;
}

fx80 fx_indefinite(void) { return fx_make(1, 0x7FFF, 0xC000000000000000ULL); }

static Unp unpack80(fx80 a) {
    Unp u;
    u.sign = a.se >> 15;
    int e = a.se & 0x7FFF;
    u.exp = e ? e : 1;
    u.sig = (u128)a.sig << 64;
    normalize(&u);
    return u;
}

static fx80 pack80(Unp u) {
    if (!u.sig) return fx_make(u.sign, 0, 0);
    int e = (u.sig >> 127) ? u.exp : 0;
    return fx_make(u.sign, e, (uint64_t)(u.sig >> 64));
}

static fx80 round80(FxEnv *e, Unp u, int prec) { return pack80(round_to(e, u, R80.emin, R80.emax, prec)); }

static fx80 quiet80(fx80 a) { a.sig |= 1ULL << 62; return a; }

/* x87 NaN propagation (Intel SDM table 4-7): an SNaN raises #IA; a QNaN
 * beats an SNaN; two NaNs of the same kind -> larger significand. */
static fx80 propagate80(FxEnv *e, fx80 a, fx80 b) {
    int ca = fx_class(a), cb = fx_class(b);
    int na = ca == FXC_QNAN || ca == FXC_SNAN, nb = cb == FXC_QNAN || cb == FXC_SNAN;
    if (ca == FXC_SNAN || cb == FXC_SNAN) e->flags |= FX_IE;
    if (!nb) return quiet80(a);
    if (!na) return quiet80(b);
    if (ca == FXC_SNAN && cb == FXC_QNAN) return b;
    if (cb == FXC_SNAN && ca == FXC_QNAN) return a;
    fx80 qa = quiet80(a), qb = quiet80(b);
    if (qa.sig != qb.sig) return qa.sig > qb.sig ? qa : qb;
    return (a.se < b.se) ? qa : qb;
}

/* Common NaN/unsupported/denormal screening for two-operand ops. */
static int special2(FxEnv *e, fx80 a, fx80 b, fx80 *res) {
    int ca = fx_class(a), cb = fx_class(b);
    if (ca == FXC_UNSUPPORTED || cb == FXC_UNSUPPORTED) {
        e->flags |= FX_IE;
        *res = fx_indefinite();
        return 1;
    }
    if (ca == FXC_QNAN || ca == FXC_SNAN || cb == FXC_QNAN || cb == FXC_SNAN) {
        *res = propagate80(e, a, b);
        return 1;
    }
    if (ca == FXC_DENORMAL || cb == FXC_DENORMAL) e->flags |= FX_DE;
    return 0;
}

static int special1(FxEnv *e, fx80 a, fx80 *res) {
    int ca = fx_class(a);
    if (ca == FXC_UNSUPPORTED) {
        e->flags |= FX_IE;
        *res = fx_indefinite();
        return 1;
    }
    if (ca == FXC_QNAN || ca == FXC_SNAN) {
        if (ca == FXC_SNAN) e->flags |= FX_IE;
        *res = quiet80(a);
        return 1;
    }
    if (ca == FXC_DENORMAL) e->flags |= FX_DE;
    return 0;
}

/* An invalid operation suppresses the denormal-operand flag. */
static fx80 invalid80(FxEnv *e) {
    e->flags = (e->flags & ~FX_DE) | FX_IE;
    return fx_indefinite();
}

/* ---- exact cores on unpacked values (significands of at most 64 bits) -- */

static Unp core_add(FxEnv *e, Unp a, Unp b) {
    if (!a.sig && !b.sig) {
        Unp z = {a.sign == b.sign ? a.sign : e->rc == RC_DOWN, 0, 0};
        return z;
    }
    if (!a.sig) return b;
    if (!b.sig) return a;
    a.sig >>= 1; a.exp++;
    b.sig >>= 1; b.exp++;
    if (a.exp < b.exp || (a.exp == b.exp && a.sig < b.sig)) { Unp t = a; a = b; b = t; }
    b.sig = shr_jam(b.sig, a.exp - b.exp);
    Unp r = {a.sign, a.exp, 0};
    if (a.sign == b.sign) r.sig = a.sig + b.sig;
    else {
        r.sig = a.sig - b.sig;
        if (!r.sig) { r.sign = e->rc == RC_DOWN; return r; }
    }
    normalize(&r);
    return r;
}

static Unp core_mul(Unp a, Unp b) {
    Unp r = {a.sign ^ b.sign, 0, 0};
    if (!a.sig || !b.sig) return r;
    r.sig = (u128)(uint64_t)(a.sig >> 64) * (uint64_t)(b.sig >> 64);
    r.exp = a.exp + b.exp - BIAS + 1;
    normalize(&r);
    return r;
}

static Unp core_div(Unp a, Unp b) {
    Unp r = {a.sign ^ b.sign, 0, 0};
    if (!a.sig) return r;
    uint64_t sa = (uint64_t)(a.sig >> 64), sb = (uint64_t)(b.sig >> 64);
    int shift = sa < sb ? 64 : 63;
    u128 n = (u128)sa << shift;
    uint64_t q1 = (uint64_t)(n / sb), r1 = (uint64_t)(n % sb);
    u128 n2 = (u128)r1 << 64;
    uint64_t q2 = (uint64_t)(n2 / sb), r2 = (uint64_t)(n2 % sb);
    r.sig = ((u128)q1 << 64) | q2 | (r2 != 0);
    r.exp = a.exp - b.exp + BIAS + 63 - shift;
    normalize(&r);
    return r;
}

static void mul128(u128 a, u128 b, u128 *hi, u128 *lo) {
    uint64_t a1 = a >> 64, a0 = a, b1 = b >> 64, b0 = b;
    u128 p00 = (u128)a0 * b0, p01 = (u128)a0 * b1, p10 = (u128)a1 * b0, p11 = (u128)a1 * b1;
    u128 mid = (p00 >> 64) + (uint64_t)p01 + (uint64_t)p10;
    *lo = (mid << 64) | (uint64_t)p00;
    *hi = p11 + (p01 >> 64) + (p10 >> 64) + (mid >> 64);
}

static uint64_t isqrt128(u128 m) {
    double d = sqrt((double)m);
    uint64_t r = d >= 18446744073709549568.0 ? UINT64_MAX : (uint64_t)d;
    if (r) {
        u128 t = ((u128)(m / r) + r) >> 1;
        r = t > UINT64_MAX ? UINT64_MAX : (uint64_t)t;
    }
    while ((u128)r * r > m) r--;
    while (r < UINT64_MAX && (u128)(r + 1) * (r + 1) <= m) r++;
    return r;
}

/* compare S^2 with m * 2^128 */
static int sq_cmp(u128 s, u128 m) {
    u128 hi, lo;
    mul128(s, s, &hi, &lo);
    if (hi != m) return hi < m ? -1 : 1;
    return lo ? 1 : 0;
}

static Unp core_sqrt(Unp a) {
    Unp r = {a.sign, 0, 0};
    if (!a.sig) return r;
    int k = a.exp - BIAS;
    u128 m = a.sig >> 64;
    if (k & 1) { m <<= 1; k -= 1; }
    u128 M = m << 63;
    uint64_t root = isqrt128(M);
    u128 rem = M - (u128)root * root;
    uint64_t q = 0;
    int exact = rem == 0;
    if (!exact) {
        u128 est = (rem << 63) / root;
        q = est > UINT64_MAX ? UINT64_MAX : (uint64_t)est;
        while (sq_cmp(((u128)root << 64) | q, M) > 0) q--;
        while (q < UINT64_MAX && sq_cmp(((u128)root << 64) | (q + 1), M) <= 0) q++;
    }
    r.sign = 0;
    r.exp = BIAS + k / 2;
    r.sig = ((u128)root << 64) | q | !exact;
    return r;
}

/* ---- x87 arithmetic ------------------------------------------------------ */

static fx80 add80(FxEnv *e, fx80 a, fx80 b) {
    fx80 res;
    if (special2(e, a, b, &res)) return res;
    int ca = fx_class(a), cb = fx_class(b);
    if (ca == FXC_INF && cb == FXC_INF) return fx_sign(a) == fx_sign(b) ? a : invalid80(e);
    if (ca == FXC_INF) return a;
    if (cb == FXC_INF) return b;
    return round80(e, core_add(e, unpack80(a), unpack80(b)), e->prec);
}

fx80 fx_add(FxEnv *e, fx80 a, fx80 b) { return add80(e, a, b); }

fx80 fx_sub(FxEnv *e, fx80 a, fx80 b) {
    if (fx_isnan(b)) return add80(e, a, b); /* NaN sign must not flip */
    return add80(e, a, fx_neg(b));
}

fx80 fx_mul(FxEnv *e, fx80 a, fx80 b) {
    fx80 res;
    if (special2(e, a, b, &res)) return res;
    int ca = fx_class(a), cb = fx_class(b), s = fx_sign(a) ^ fx_sign(b);
    if (ca == FXC_INF || cb == FXC_INF) {
        if (ca == FXC_ZERO || cb == FXC_ZERO) return invalid80(e);
        return fx_make(s, 0x7FFF, 1ULL << 63);
    }
    return round80(e, core_mul(unpack80(a), unpack80(b)), e->prec);
}

fx80 fx_div(FxEnv *e, fx80 a, fx80 b) {
    fx80 res;
    if (special2(e, a, b, &res)) return res;
    int ca = fx_class(a), cb = fx_class(b), s = fx_sign(a) ^ fx_sign(b);
    if (ca == FXC_INF) return cb == FXC_INF ? invalid80(e) : fx_make(s, 0x7FFF, 1ULL << 63);
    if (cb == FXC_INF) return fx_make(s, 0, 0);
    if (cb == FXC_ZERO) {
        if (ca == FXC_ZERO) return invalid80(e);
        e->flags = (e->flags & ~FX_DE) | FX_ZE;
        return fx_make(s, 0x7FFF, 1ULL << 63);
    }
    return round80(e, core_div(unpack80(a), unpack80(b)), e->prec);
}

fx80 fx_sqrt(FxEnv *e, fx80 a) {
    fx80 res;
    if (special1(e, a, &res)) return res;
    int ca = fx_class(a);
    if (ca == FXC_ZERO) return a;
    if (fx_sign(a)) return invalid80(e);
    if (ca == FXC_INF) return a;
    return round80(e, core_sqrt(unpack80(a)), e->prec);
}

static int cmp_unp(Unp a, Unp b) {
    if (!a.sig && !b.sig) return CMP_EQ;
    if (a.sign != b.sign) return a.sign ? CMP_LT : CMP_GT;
    int lt;
    if (!a.sig || !b.sig) lt = !a.sig;
    else if (a.exp != b.exp) lt = a.exp < b.exp;
    else if (a.sig != b.sig) lt = a.sig < b.sig;
    else return CMP_EQ;
    return lt ^ a.sign ? CMP_LT : CMP_GT;
}

int fx_compare(FxEnv *e, fx80 a, fx80 b, int quiet) {
    int ca = fx_class(a), cb = fx_class(b);
    if (ca == FXC_UNSUPPORTED || cb == FXC_UNSUPPORTED) { e->flags |= FX_IE; return CMP_UN; }
    if (ca == FXC_QNAN || ca == FXC_SNAN || cb == FXC_QNAN || cb == FXC_SNAN) {
        if (!quiet || ca == FXC_SNAN || cb == FXC_SNAN) e->flags |= FX_IE;
        return CMP_UN;
    }
    if (ca == FXC_DENORMAL || cb == FXC_DENORMAL) e->flags |= FX_DE;
    if (ca == FXC_INF || cb == FXC_INF) {
        if (ca == cb && fx_sign(a) == fx_sign(b)) return CMP_EQ;
        if (ca == FXC_INF) return fx_sign(a) ? CMP_LT : CMP_GT;
        return fx_sign(b) ? CMP_GT : CMP_LT;
    }
    return cmp_unp(unpack80(a), unpack80(b));
}

/* ---- integer conversions ------------------------------------------------- */

static Unp unp_from_u64(int sign, uint64_t v) {
    Unp u = {sign, BIAS + 63, (u128)v << 64};
    normalize(&u);
    return u;
}

/* Round |u| to an integer. Returns the magnitude (may be up to 2^64). */
static int last_inc; /* unp_to_uint rounded the magnitude up */

static u128 unp_to_uint(Unp u, int rc, int *inexact, int *overflow) {
    *inexact = *overflow = 0;
    last_inc = 0;
    if (!u.sig) return 0;
    int k = u.exp - BIAS;
    if (k >= 64) { *overflow = 1; return 0; }
    int sh = 127 - k;
    u128 ip, frac;
    if (sh > 128) { ip = 0; frac = 1; }
    else if (sh == 128) { ip = 0; frac = u.sig; }
    else { ip = u.sig >> sh; frac = u.sig << (128 - sh); }
    if (frac) {
        *inexact = 1;
        int inc = 0;
        switch (rc) {
        case RC_NEAR: inc = frac > TOP || (frac == TOP && (ip & 1)); break;
        case RC_DOWN: inc = u.sign; break;
        case RC_UP: inc = !u.sign; break;
        }
        ip += inc;
        last_inc = inc;
    }
    return ip;
}

static int64_t to_int_common(FxEnv *e, Unp u, int bits, int rc) {
    int inexact, ovf;
    u128 m = unp_to_uint(u, rc, &inexact, &ovf);
    u128 lim = (u128)1 << (bits - 1);
    if (ovf || m > lim || (m == lim && !u.sign)) {
        e->flags |= FX_IE;
        return -(int64_t)(lim - 1) - 1;
    }
    if (inexact) e->flags |= FX_PE;
    e->roundup = last_inc;
    return u.sign ? (int64_t)(0 - (uint64_t)m) : (int64_t)(uint64_t)m;
}

fx80 fx_from_i64(int64_t v) {
    if (!v) return fx_make(0, 0, 0);
    Unp u = unp_from_u64(v < 0, v < 0 ? 0 - (uint64_t)v : (uint64_t)v);
    return pack80(u);
}

int64_t fx_to_int(FxEnv *e, fx80 a, int bits, int truncate) {
    int ca = fx_class(a);
    if (ca == FXC_UNSUPPORTED || ca == FXC_QNAN || ca == FXC_SNAN || ca == FXC_INF) {
        e->flags |= FX_IE;
        return -(int64_t)((1ULL << (bits - 1)) - 1) - 1;
    }
    return to_int_common(e, unpack80(a), bits, truncate ? RC_ZERO : e->rc);
}

fx80 fx_round_int(FxEnv *e, fx80 a) {
    fx80 res;
    if (special1(e, a, &res)) return res;
    int ca = fx_class(a);
    if (ca == FXC_ZERO || ca == FXC_INF) return a;
    Unp u = unpack80(a);
    if (u.exp - BIAS >= 63) return a;
    int inexact, ovf;
    u128 m = unp_to_uint(u, e->rc, &inexact, &ovf);
    e->roundup = 0;
    if (inexact) {
        e->flags |= FX_PE;
        e->roundup = m != 0 && cmp_unp(unp_from_u64(0, (uint64_t)m), (Unp){0, u.exp, u.sig}) == CMP_GT;
    }
    if (!m) return fx_make(u.sign, 0, 0);
    return pack80(unp_from_u64(u.sign, (uint64_t)m));
}

/* ---- float/double interchange ------------------------------------------- */

typedef struct { int fbits, ebits; const Range *r; } Fmt;
static const Fmt FMT32 = {23, 8, &R32};
static const Fmt FMT64 = {52, 11, &R64};

#define FSIGN(f) (1ULL << ((f)->fbits + (f)->ebits))
#define FEXPM(f) ((1ULL << (f)->ebits) - 1)
#define FFRAC(f) ((1ULL << (f)->fbits) - 1)
#define FQUIET(f) (1ULL << ((f)->fbits - 1))

static int sf_class(const Fmt *f, uint64_t v) {
    uint64_t ex = (v >> f->fbits) & FEXPM(f), fr = v & FFRAC(f);
    if (ex == 0) return fr ? FXC_DENORMAL : FXC_ZERO;
    if (ex == FEXPM(f)) {
        if (!fr) return FXC_INF;
        return (fr & FQUIET(f)) ? FXC_QNAN : FXC_SNAN;
    }
    return FXC_NORMAL;
}

int sf32_class(uint32_t a) { return sf_class(&FMT32, a); }
int sf64_class(uint64_t a) { return sf_class(&FMT64, a); }

static Unp sf_unpack(const Fmt *f, uint64_t v) {
    Unp u;
    u.sign = (v & FSIGN(f)) != 0;
    uint64_t ex = (v >> f->fbits) & FEXPM(f), fr = v & FFRAC(f);
    int bias = (int)(FEXPM(f) >> 1);
    if (ex == 0) {
        u.exp = 1 - bias + BIAS;
        u.sig = (u128)fr << (127 - f->fbits);
    } else {
        u.exp = (int)ex - bias + BIAS;
        u.sig = (u128)(fr | (1ULL << f->fbits)) << (127 - f->fbits);
    }
    normalize(&u);
    return u;
}

static uint64_t sf_pack(const Fmt *f, Unp u) {
    uint64_t s = u.sign ? FSIGN(f) : 0;
    if (!u.sig) return s;
    int bias = (int)(FEXPM(f) >> 1);
    uint64_t fr = (uint64_t)(u.sig >> (127 - f->fbits));
    if (!(u.sig >> 127)) return s | fr;
    return s | ((uint64_t)(u.exp - BIAS + bias) << f->fbits) | (fr & FFRAC(f));
}

static uint64_t sf_round_pack(const Fmt *f, FxEnv *e, Unp u) {
    return sf_pack(f, round_to(e, u, f->r->emin, f->r->emax, f->r->prec));
}

static uint64_t sf_inf(const Fmt *f, int sign) { return (sign ? FSIGN(f) : 0) | (FEXPM(f) << f->fbits); }
static uint64_t sf_default_nan(const Fmt *f) { return FSIGN(f) | (FEXPM(f) << f->fbits) | FQUIET(f); }

static fx80 from_sf(const Fmt *f, FxEnv *e, uint64_t v) {
    int c = sf_class(f, v);
    int sign = (v & FSIGN(f)) != 0;
    if (c == FXC_INF) return fx_make(sign, 0x7FFF, 1ULL << 63);
    if (c == FXC_QNAN || c == FXC_SNAN) {
        if (c == FXC_SNAN) e->flags |= FX_IE;
        return fx_make(sign, 0x7FFF, (1ULL << 63) | (1ULL << 62) | ((v & FFRAC(f)) << (63 - f->fbits)));
    }
    if (c == FXC_DENORMAL) e->flags |= FX_DE;
    return pack80(sf_unpack(f, v));
}

static uint64_t to_sf(const Fmt *f, FxEnv *e, fx80 a) {
    int c = fx_class(a);
    int sign = fx_sign(a);
    switch (c) {
    case FXC_UNSUPPORTED: e->flags |= FX_IE; return sf_default_nan(f);
    case FXC_SNAN: e->flags |= FX_IE; /* fall through */
    case FXC_QNAN:
        return (sign ? FSIGN(f) : 0) | (FEXPM(f) << f->fbits) | FQUIET(f) |
               ((a.sig >> (63 - f->fbits)) & FFRAC(f));
    case FXC_INF: return sf_inf(f, sign);
    case FXC_ZERO: return sign ? FSIGN(f) : 0;
    }
    return sf_round_pack(f, e, unpack80(a));
}

fx80 fx_from_f64(FxEnv *e, uint64_t bits) { return from_sf(&FMT64, e, bits); }
fx80 fx_from_f32(FxEnv *e, uint32_t bits) { return from_sf(&FMT32, e, bits); }
uint64_t fx_to_f64(FxEnv *e, fx80 a) { return to_sf(&FMT64, e, a); }
uint32_t fx_to_f32(FxEnv *e, fx80 a) { return (uint32_t)to_sf(&FMT32, e, a); }

fx80 fx_round_prec(FxEnv *e, fx80 a) {
    int c = fx_class(a);
    if (c != FXC_NORMAL && c != FXC_DENORMAL) return a;
    return round80(e, unpack80(a), e->prec);
}

/* ---- FSCALE, FPREM, FXTRACT, BCD ------------------------------------------ */

fx80 fx_scale(FxEnv *e, fx80 a, fx80 b) {
    fx80 res;
    if (special2(e, a, b, &res)) return res;
    int ca = fx_class(a), cb = fx_class(b);
    if (cb == FXC_INF) {
        if (fx_sign(b)) return ca == FXC_INF ? invalid80(e) : fx_make(fx_sign(a), 0, 0);
        if (ca == FXC_ZERO) return invalid80(e);
        return fx_make(fx_sign(a), 0x7FFF, 1ULL << 63);
    }
    if (ca == FXC_ZERO || ca == FXC_INF) return a;
    int n;
    Unp ub = unpack80(b);
    if (ub.exp - BIAS >= 30) n = ub.sign ? -(1 << 30) : (1 << 30);
    else {
        int inexact, ovf;
        n = (int)unp_to_uint(ub, RC_ZERO, &inexact, &ovf);
        if (ub.sign) n = -n;
    }
    Unp u = unpack80(a);
    u.exp += n;
    return round80(e, u, 64);
}

fx80 fx_prem(FxEnv *e, fx80 a, fx80 b, int ieee, int *qbits, int *partial) {
    fx80 res;
    *qbits = 0;
    *partial = 0;
    if (special2(e, a, b, &res)) return res;
    int ca = fx_class(a), cb = fx_class(b);
    if (ca == FXC_INF || cb == FXC_ZERO) return invalid80(e);
    if (ca == FXC_ZERO) return a;
    if (cb == FXC_INF) return round80(e, unpack80(a), 64);
    Unp ua = unpack80(a), ub = unpack80(b);
    uint64_t sa = (uint64_t)(ua.sig >> 64), sb = (uint64_t)(ub.sig >> 64);
    int d = ua.exp - ub.exp;
    int sign = ua.sign;
    Unp r = {sign, 0, 0};
    if (d < -1) return round80(e, ua, 64);
    if (d >= 64) {
        int n = (d & 31) | 32;
        u128 A = (u128)sa << n;
        uint64_t R = (uint64_t)(A % sb);
        *partial = 1;
        r.exp = ua.exp - n;
        r.sig = (u128)R << 64;
    } else {
        int minexp = d < 0 ? ua.exp : ub.exp;
        u128 A = (u128)sa << (ua.exp - minexp), B = (u128)sb << (ub.exp - minexp);
        u128 Q = A / B, R = A % B;
        /* the x87 skips the round-to-nearest step when the quotient needs all 64 bits */
        if (ieee && Q >> 63 == 0 && (2 * R > B || (2 * R == B && (Q & 1)))) {
            Q++;
            R = B - R;
            r.sign ^= 1;
        }
        *qbits = (int)(Q & 7);
        if (!R) return fx_make(sign, 0, 0);
        /* R < 2^65; value = R * 2^(minexp - BIAS - 63) */
        int sh = clz128(R);
        r.sig = R << sh;
        r.exp = minexp + 64 - sh;
    }
    if (!r.sig) return fx_make(sign, 0, 0);
    normalize(&r);
    return round80(e, r, 64);
}

void fx_xtract(FxEnv *e, fx80 a, fx80 *exponent, fx80 *significand) {
    fx80 res;
    if (special1(e, a, &res)) { *exponent = *significand = res; return; }
    int ca = fx_class(a);
    if (ca == FXC_ZERO) {
        e->flags |= FX_ZE;
        *exponent = fx_make(1, 0x7FFF, 1ULL << 63);
        *significand = a;
        return;
    }
    if (ca == FXC_INF) {
        *exponent = fx_make(0, 0x7FFF, 1ULL << 63);
        *significand = a;
        return;
    }
    Unp u = unpack80(a);
    *exponent = fx_from_i64(u.exp - BIAS);
    *significand = fx_make(u.sign, BIAS, (uint64_t)(u.sig >> 64));
}

int fx_from_bcd(const uint8_t bcd[10], fx80 *out) {
    uint64_t v = 0;
    for (int i = 8; i >= 0; i--) v = v * 100 + (bcd[i] >> 4) * 10 + (bcd[i] & 15);
    *out = fx_from_i64((int64_t)v);
    if (bcd[9] & 0x80) out->se |= 0x8000;
    return 1;
}

void fx_to_bcd(FxEnv *e, fx80 a, uint8_t bcd[10]) {
    int ca = fx_class(a);
    memset(bcd, 0, 10);
    int inexact = 0, ovf = 1;
    u128 m = 0;
    if (ca != FXC_UNSUPPORTED && ca != FXC_QNAN && ca != FXC_SNAN && ca != FXC_INF) {
        m = unp_to_uint(unpack80(a), e->rc, &inexact, &ovf);
        e->roundup = last_inc;
        if (m > 999999999999999999ULL) ovf = 1;
    }
    if (ovf) {
        e->flags |= FX_IE;
        bcd[9] = 0xFF; bcd[8] = 0xFF; bcd[7] = 0xC0;
        return;
    }
    if (inexact) e->flags |= FX_PE;
    uint64_t v = (uint64_t)m;
    for (int i = 0; i < 9; i++) {
        bcd[i] = (uint8_t)((v % 10) | ((v / 10 % 10) << 4));
        v /= 100;
    }
    bcd[9] = fx_sign(a) ? 0x80 : 0;
}

/* ---- 128-bit-significand engine for transcendentals ------------------------ */

typedef struct { int sign; int exp; u128 m; } F; /* m/2^127 * 2^exp */

static F f_norm(int sign, int exp, u128 m) {
    F r = {sign, 0, 0};
    if (!m) return r;
    int s = clz128(m);
    r.exp = exp - s;
    r.m = m << s;
    return r;
}

static F f_int(int64_t v) {
    uint64_t a = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    return f_norm(v < 0, 127, a);
}

static F f_neg(F a) { a.sign ^= 1; return a; }
static F f_scale(F a, int n) { if (a.m) a.exp += n; return a; }

static F f_mul(F a, F b) {
    if (!a.m || !b.m) return (F){a.sign ^ b.sign, 0, 0};
    u128 hi, lo;
    mul128(a.m, b.m, &hi, &lo);
    return f_norm(a.sign ^ b.sign, a.exp + b.exp + 1, hi);
}

static F f_add(F a, F b) {
    if (!a.m) return b;
    if (!b.m) return a;
    if (a.exp < b.exp || (a.exp == b.exp && a.m < b.m)) { F t = a; a = b; b = t; }
    int d = a.exp - b.exp;
    u128 bm = d >= 128 ? 0 : b.m >> d;
    if (a.sign == b.sign) {
        u128 s = a.m + bm;
        if (s < a.m) return (F){a.sign, a.exp + 1, (s >> 1) | TOP};
        return (F){a.sign, a.exp, s};
    }
    return f_norm(a.sign, a.exp, a.m - bm);
}

static F f_sub(F a, F b) { return f_add(a, f_neg(b)); }

static F f_div(F a, F b) {
    if (!a.m) return (F){a.sign ^ b.sign, 0, 0};
    u128 rem = a.m, d = b.m, q = 0;
    int top = 0;
    for (int i = 0; i < 128; i++) {
        q <<= 1;
        if (top || rem >= d) { rem -= d; q |= 1; }
        top = (int)(rem >> 127);
        rem <<= 1;
    }
    return f_norm(a.sign ^ b.sign, a.exp - b.exp, q);
}

static F f_divi(F a, uint64_t d) {
    if (!a.m) return a;
    u128 q1 = a.m / d, r = a.m % d;
    int lz = clz128(q1);
    u128 q = q1 << lz;
    if (lz) q |= (r << lz) / d;
    return (F){a.sign, a.exp - lz, q};
}

static F f_sqrt(F a) {
    if (!a.m) return a;
    int e = a.exp;
    double d = (double)(uint64_t)(a.m >> 64) / 9223372036854775808.0;
    if (e & 1) { d *= 2; e -= 1; }
    double y0 = sqrt(d);
    F y = {0, e / 2, (u128)(uint64_t)(y0 * 9223372036854775808.0) << 64};
    for (int i = 0; i < 3; i++) y = f_scale(f_add(y, f_div(a, y)), -1);
    return y;
}

/* Nudge by one unit in the last place: records that a dropped series term
 * makes the true value slightly larger (+1) or smaller (-1) in magnitude. */
static F f_nudge(F a, int up) {
    if (!a.m) return a;
    if (up) {
        if (a.m != ~(u128)0) a.m++;
        return a;
    }
    return f_norm(a.sign, a.exp, a.m - 1);
}

/* Terms below 2^-124 relative can no longer change the sum's 128-bit
 * significand reliably; the first such term only decides the sticky nudge. */
static int f_negligible(F term, F sum) { return !term.m || term.exp < sum.exp - 124; }

static F f_series_end(F sum, F term) {
    if (!term.m) return sum;
    return f_nudge(sum, term.sign == sum.sign);
}

static const F ONE = {0, 0, TOP};

static F f_sin_series(F r) {
    if (!r.m) return r;
    F r2 = f_mul(r, r), term = r, sum = r;
    for (uint64_t k = 2;; k += 2) {
        term = f_neg(f_divi(f_mul(term, r2), k * (k + 1)));
        if (f_negligible(term, sum)) return f_series_end(sum, term);
        sum = f_add(sum, term);
    }
}

static F f_cos_series(F r) {
    F r2 = f_mul(r, r), term = ONE, sum = ONE;
    if (!r.m) return sum;
    for (uint64_t k = 1;; k += 2) {
        term = f_neg(f_divi(f_mul(term, r2), k * (k + 1)));
        if (f_negligible(term, sum)) return f_series_end(sum, term);
        sum = f_add(sum, term);
    }
}

/* atan (alt = 1) or atanh (alt = 0) by Taylor series; for small arguments */
static F f_atan_series(F t, int alt) {
    if (!t.m) return t;
    F t2 = f_mul(t, t), p = t, sum = t;
    for (uint64_t k = 3;; k += 2) {
        p = f_mul(p, t2);
        if (alt) p = f_neg(p);
        F term = f_divi(p, k);
        if (f_negligible(term, sum)) return f_series_end(sum, term);
        sum = f_add(sum, term);
    }
}

static F f_expm1_series(F y) {
    if (!y.m) return y;
    F term = y, sum = y;
    for (uint64_t k = 2;; k++) {
        term = f_divi(f_mul(term, y), k);
        if (f_negligible(term, sum)) return f_series_end(sum, term);
        sum = f_add(sum, term);
    }
}

static F K_LN2, K_LOG2E, K_PI, K_L2T, K_LG2;
static int k_ready;

static void init_consts(void) {
    if (k_ready) return;
    K_LN2 = f_scale(f_atan_series(f_divi(ONE, 3), 0), 1);
    K_LOG2E = f_div(ONE, K_LN2);
    K_PI = f_sub(f_scale(f_atan_series(f_divi(ONE, 5), 1), 4), f_scale(f_atan_series(f_divi(ONE, 239), 1), 2));
    /* log2(10) = 3 + log2(1.25) = 3 + 2*atanh(1/9)/ln2 */
    K_L2T = f_add(f_int(3), f_mul(f_scale(f_atan_series(f_divi(ONE, 9), 0), 1), K_LOG2E));
    K_LG2 = f_div(ONE, K_L2T);
    k_ready = 1;
}

static F f_from80(fx80 a) {
    Unp u = unpack80(a);
    return (F){u.sign, u.exp - BIAS, u.sig};
}

static fx80 f_to80(FxEnv *e, F v) {
    Unp u = {v.sign, v.exp + BIAS, v.m};
    if (!v.m) return fx_make(v.sign, 0, 0);
    return round80(e, u, 64);
}

/* Constants are rounded per RC but raise no exceptions and leave C1 clear. */
fx80 fx_const(FxEnv *e0, int which) {
    FxEnv tmp = *e0, *e = &tmp;
    init_consts();
    switch (which) {
    case FXK_ONE: return fx_make(0, BIAS, 1ULL << 63);
    case FXK_ZERO: return fx_make(0, 0, 0);
    case FXK_L2T: return f_to80(e, K_L2T);
    case FXK_L2E: return f_to80(e, K_LOG2E);
    case FXK_PI: return f_to80(e, K_PI);
    case FXK_LG2: return f_to80(e, K_LG2);
    case FXK_LN2: return f_to80(e, K_LN2);
    }
    return fx_make(0, 0, 0);
}

/* ---- trigonometry with Intel's 66-bit pi ----------------------------------- */

typedef struct { uint64_t w[4]; } U256;

static U256 u256(u128 x) { U256 r = {{(uint64_t)x, (uint64_t)(x >> 64), 0, 0}}; return r; }

static U256 u256_shl(U256 a, int n) {
    U256 r = {{0, 0, 0, 0}};
    int words = n / 64, bits = n % 64;
    for (int i = 3; i >= words; i--) {
        uint64_t v = a.w[i - words] << bits;
        if (bits && i - words - 1 >= 0) v |= a.w[i - words - 1] >> (64 - bits);
        r.w[i] = v;
    }
    return r;
}

static int u256_cmp(U256 a, U256 b) {
    for (int i = 3; i >= 0; i--)
        if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
    return 0;
}

static U256 u256_sub(U256 a, U256 b) {
    U256 r;
    unsigned borrow = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t t = a.w[i] - b.w[i];
        unsigned b1 = a.w[i] < b.w[i];
        uint64_t t2 = t - borrow;
        unsigned b2 = t < borrow;
        r.w[i] = t2;
        borrow = b1 | b2;
    }
    return r;
}

/* pi rounded to 66 bits, as used by x87 FSIN/FCOS/FPTAN/FSINCOS: value = P66 / 2^64 */
static const u128 P66 = ((u128)0x3ULL << 64) | 0x243F6A8885A308D3ULL;

/* Reduce |x| (finite, < 2^63) modulo pi66/2 to the nearest quadrant.
 * Returns r in [-pi/4, pi/4] (exact, up to one bit of truncation) and q. */
static F trig_reduce(Unp a, uint64_t *quad) {
    *quad = 0;
    F x = {0, a.exp - BIAS, a.sig};
    if (a.exp - BIAS < -2) return x;
    int k = a.exp - BIAS - 63; /* |x| = S * 2^k */
    U256 X = u256_shl(u256(a.sig >> 64), k + 128);
    U256 Y = u256_shl(u256(P66), 63); /* pi66/2 in units of 2^-128 */
    uint64_t q = 0;
    for (int i = 63; i >= 0; i--) {
        U256 t = u256_shl(Y, i);
        if (u256_cmp(X, t) >= 0) { X = u256_sub(X, t); q |= 1ULL << i; }
    }
    int neg = 0;
    if (u256_cmp(u256_shl(X, 1), Y) > 0) {
        X = u256_sub(Y, X);
        neg = 1;
        q++;
    }
    *quad = q;
    u128 lo = ((u128)X.w[1] << 64) | X.w[0];
    if (X.w[2]) return f_norm(neg, 0, (lo >> 1) | ((u128)X.w[2] << 127));
    return f_norm(neg, -1, lo);
}

static F f_tan_of(F r) {
    if (r.m && r.exp < -24) {
        /* tan r = r + r^3/3 + 2r^5/15 + 17r^7/315 + ... */
        F r2 = f_mul(r, r), p = r, sum = r;
        static const uint64_t num[] = {1, 2, 17}, den[] = {3, 15, 315};
        for (int i = 0; i < 3; i++) {
            p = f_mul(p, r2);
            sum = f_add(sum, f_divi(f_mul(p, f_int((int64_t)num[i])), den[i]));
        }
        return f_nudge(sum, 1);
    }
    return f_div(f_sin_series(r), f_cos_series(r));
}

static int trig_prep(FxEnv *e, fx80 a, fx80 *out1, fx80 *out2, Unp *u) {
    fx80 res;
    if (special1(e, a, &res)) { *out1 = res; if (out2) *out2 = res; return 1; }
    int ca = fx_class(a);
    if (ca == FXC_INF) { *out1 = invalid80(e); if (out2) *out2 = *out1; return 1; }
    *u = unpack80(a);
    return 0;
}

int fx_sincos(FxEnv *e, fx80 a, fx80 *s, fx80 *c) {
    Unp u;
    fx80 d1, d2;
    if (trig_prep(e, a, &d1, &d2, &u)) {
        if (s) *s = d1;
        if (c) *c = d2;
        return 1;
    }
    if (fx_class(a) == FXC_ZERO) {
        if (s) *s = a;
        if (c) *c = fx_make(0, BIAS, 1ULL << 63);
        return 1;
    }
    if (u.exp - BIAS >= 63) return 0;
    int sign = u.sign;
    u.sign = 0;
    uint64_t q;
    F r = trig_reduce(u, &q);
    F sr = f_sin_series(r), cr = f_cos_series(r);
    F sv, cv;
    switch (q & 3) {
    case 0: sv = sr; cv = cr; break;
    case 1: sv = cr; cv = f_neg(sr); break;
    case 2: sv = f_neg(sr); cv = f_neg(cr); break;
    default: sv = f_neg(cr); cv = sr; break;
    }
    if (sign) sv = f_neg(sv);
    unsigned flags = e->flags;
    int ru = 0;
    if (c) { *c = f_to80(e, cv); ru = e->roundup; }
    if (s) { *s = f_to80(e, sv); ru = e->roundup; }
    e->roundup = ru;
    (void)flags;
    return 1;
}

int fx_tan(FxEnv *e, fx80 a, fx80 *t) {
    Unp u;
    if (trig_prep(e, a, t, NULL, &u)) return 1;
    if (fx_class(a) == FXC_ZERO) { *t = a; return 1; }
    if (u.exp - BIAS >= 63) return 0;
    int sign = u.sign;
    u.sign = 0;
    uint64_t q;
    F r = trig_reduce(u, &q);
    F v;
    if (q & 1) v = f_neg(f_div(f_cos_series(r), f_sin_series(r)));
    else v = f_tan_of(r);
    if (sign) v = f_neg(v);
    *t = f_to80(e, v);
    return 1;
}

/* atan of t >= 0 by argument halving: atan t = 2 atan(t / (1 + sqrt(1 + t^2))) */
static F f_atan(F t) {
    int n = 0;
    while (t.m && t.exp > -5) {
        t = f_div(t, f_add(ONE, f_sqrt(f_add(ONE, f_mul(t, t)))));
        n++;
    }
    return f_scale(f_atan_series(t, 1), n);
}

fx80 fx_atan2(FxEnv *e, fx80 y, fx80 x) {
    fx80 res;
    if (special2(e, x, y, &res)) return res;
    init_consts();
    int cy = fx_class(y), cx = fx_class(x), sy = fx_sign(y), sx = fx_sign(x);
    F pi = K_PI, half = f_scale(K_PI, -1);
    F v;
    if (cy == FXC_INF && cx == FXC_INF) {
        v = f_scale(K_PI, -2);
        if (sx) v = f_mul(v, f_int(3));
    } else if (cy == FXC_INF) v = half;
    else if (cx == FXC_INF) {
        if (sx) v = pi;
        else return fx_make(sy, 0, 0);
    } else if (cy == FXC_ZERO) {
        if (sx) v = pi;
        else return fx_make(sy, 0, 0);
    } else if (cx == FXC_ZERO) v = half;
    else {
        F ay = f_from80(y), ax = f_from80(x);
        ay.sign = ax.sign = 0;
        int swap = ay.exp > ax.exp || (ay.exp == ax.exp && ay.m > ax.m);
        F t = swap ? f_div(ax, ay) : f_div(ay, ax);
        v = f_atan(t);
        if (swap) v = f_sub(half, v);
        if (sx) v = f_sub(pi, v);
    }
    v.sign = sy;
    return f_to80(e, v);
}

fx80 fx_2xm1(FxEnv *e, fx80 a) {
    fx80 res;
    if (special1(e, a, &res)) return res;
    init_consts();
    int ca = fx_class(a);
    if (ca == FXC_ZERO) return a;
    if (ca == FXC_INF) return fx_sign(a) ? fx_make(1, BIAS, 1ULL << 63) : a;
    F x = f_from80(a);
    if (x.exp == 0 && x.m == TOP) /* exactly +-1 */
        return x.sign ? fx_make(1, BIAS - 1, 1ULL << 63) : fx_make(0, BIAS, 1ULL << 63);
    F v;
    if (x.exp < 0) v = f_expm1_series(f_mul(x, K_LN2));
    else if (x.exp >= 15) {
        v = x.sign ? f_nudge(f_neg(ONE), 0) : (F){0, 20000, TOP};
    } else {
        /* 2^x - 1 = 2^n * 2^f - 1 with n = trunc(x) */
        int n = (int)(x.m >> (127 - x.exp));
        F fn = f_int(x.sign ? -n : n), f = f_sub(x, fn);
        F p = f_add(f_expm1_series(f_mul(f, K_LN2)), ONE);
        v = f_sub(f_scale(p, x.sign ? -n : n), ONE);
    }
    return f_to80(e, v);
}

static F f_log2(F x) {
    /* x > 0: x = m * 2^k, m in [sqrt2/2, sqrt2) */
    int k = x.exp;
    F m = {0, 0, x.m};
    if ((uint64_t)(m.m >> 64) > 0xB504F333F9DE6484ULL) { m.exp = -1; k++; }
    F s = f_div(f_sub(m, ONE), f_add(m, ONE));
    F l = f_mul(f_scale(f_atan_series(s, 0), 1), K_LOG2E);
    return f_add(f_int(k), l);
}

fx80 fx_yl2x(FxEnv *e, fx80 y, fx80 x) {
    fx80 res;
    if (special2(e, x, y, &res)) return res;
    init_consts();
    int cy = fx_class(y), cx = fx_class(x), sy = fx_sign(y), sx = fx_sign(x);
    if (sx && cx != FXC_ZERO) return invalid80(e);
    if (cx == FXC_ZERO) {
        if (cy == FXC_ZERO) return invalid80(e);
        if (cy != FXC_INF) e->flags |= FX_ZE;
        return fx_make(!sy, 0x7FFF, 1ULL << 63);
    }
    if (cx == FXC_INF) {
        if (cy == FXC_ZERO) return invalid80(e);
        return fx_make(sy, 0x7FFF, 1ULL << 63);
    }
    F lx = f_log2(f_from80(x));
    if (cy == FXC_INF) {
        if (!lx.m) return invalid80(e);
        return fx_make(sy ^ lx.sign, 0x7FFF, 1ULL << 63);
    }
    if (cy == FXC_ZERO || !lx.m) return fx_make(sy ^ lx.sign, 0, 0);
    return f_to80(e, f_mul(f_from80(y), lx));
}

fx80 fx_yl2xp1(FxEnv *e, fx80 y, fx80 x) {
    fx80 res;
    if (special2(e, x, y, &res)) return res;
    init_consts();
    int cy = fx_class(y), cx = fx_class(x), sy = fx_sign(y), sx = fx_sign(x);
    if (cx == FXC_ZERO) {
        if (cy == FXC_INF) return invalid80(e);
        return fx_make(sy ^ sx, 0, 0);
    }
    if (cx == FXC_INF) {
        if (sx || cy == FXC_ZERO) return invalid80(e);
        return fx_make(sy, 0x7FFF, 1ULL << 63);
    }
    F fx = f_from80(x);
    F onep = f_add(ONE, fx);
    if (!onep.m || onep.sign) {
        if (onep.m) return invalid80(e);
        if (cy == FXC_ZERO) return invalid80(e);
        e->flags |= FX_ZE;
        return fx_make(!sy, 0x7FFF, 1ULL << 63);
    }
    if (cy == FXC_INF) return fx_make(sy ^ sx, 0x7FFF, 1ULL << 63);
    if (cy == FXC_ZERO) return fx_make(sy ^ sx, 0, 0);
    F s = f_div(fx, f_add(f_int(2), fx));
    F l = f_mul(f_scale(f_atan_series(s, 0), 1), K_LOG2E);
    return f_to80(e, f_mul(f_from80(y), l));
}

/* ---- SSE scalar operations ------------------------------------------------- */

static uint64_t sf_quiet(const Fmt *f, uint64_t v) { return v | FQUIET(f); }

/* class after applying DAZ; denormal inputs set DE unless DAZ */
static int sse_class(const Fmt *f, FxEnv *e, uint64_t *v);

/* Conversions to integer and ROUND* apply DAZ but never report DE. */
static int sse_class_no_de(const Fmt *f, FxEnv *e, uint64_t *v) {
    unsigned de = e->flags & FX_DE;
    int c = sse_class(f, e, v);
    e->flags = (e->flags & ~FX_DE) | de;
    return c;
}

static int sse_class(const Fmt *f, FxEnv *e, uint64_t *v) {
    int c = sf_class(f, *v);
    if (c == FXC_DENORMAL) {
        if (e->daz) { *v &= FSIGN(f); return FXC_ZERO; }
        e->flags |= FX_DE;
    }
    return c;
}

static uint64_t sf_op(const Fmt *f, FxEnv *e, int op, uint64_t a, uint64_t b) {
    unsigned de_in = e->flags & FX_DE; /* packed lanes share e: suppress only this lane's DE */
    int ca = op == SOP_SQRT ? FXC_ZERO : sf_class(f, a), cb = sf_class(f, b);
    int nan_a = ca == FXC_QNAN || ca == FXC_SNAN, nan_b = cb == FXC_QNAN || cb == FXC_SNAN;
    if (op == SOP_MIN || op == SOP_MAX) {
        if (nan_a || nan_b) {
            e->flags |= FX_IE;
            if (!nan_b && e->daz && cb == FXC_DENORMAL) b &= FSIGN(f);
            return b;
        }
        ca = sse_class(f, e, &a);
        cb = sse_class(f, e, &b);
        if (ca == FXC_ZERO && cb == FXC_ZERO) return b;
        int r = cmp_unp(ca == FXC_INF ? (Unp){(a & FSIGN(f)) != 0, 1 << 20, TOP} : sf_unpack(f, a),
                        cb == FXC_INF ? (Unp){(b & FSIGN(f)) != 0, 1 << 20, TOP} : sf_unpack(f, b));
        if (op == SOP_MIN) return r == CMP_LT ? a : b;
        return r == CMP_GT ? a : b;
    }
    if (nan_a || nan_b) {
        if (ca == FXC_SNAN || cb == FXC_SNAN) e->flags |= FX_IE;
        return nan_a ? sf_quiet(f, a) : sf_quiet(f, b);
    }
    if (op != SOP_SQRT) ca = sse_class(f, e, &a);
    cb = sse_class(f, e, &b);
    int sa = (a & FSIGN(f)) != 0, sb = (b & FSIGN(f)) != 0;
    Unp ua = ca == FXC_ZERO || ca == FXC_INF ? (Unp){sa, 0, 0} : sf_unpack(f, a);
    Unp ub = cb == FXC_ZERO || cb == FXC_INF ? (Unp){sb, 0, 0} : sf_unpack(f, b);
    switch (op) {
    case SOP_SUB:
        sb ^= 1;
        ub.sign ^= 1;
        /* fall through */
    case SOP_ADD:
        if (ca == FXC_INF && cb == FXC_INF && sa != sb) break;
        if (ca == FXC_INF) return sf_inf(f, sa);
        if (cb == FXC_INF) return sf_inf(f, sb);
        return sf_round_pack(f, e, core_add(e, ua, ub));
    case SOP_MUL:
        if (ca == FXC_INF || cb == FXC_INF) {
            if (ca == FXC_ZERO || cb == FXC_ZERO) break;
            return sf_inf(f, sa ^ sb);
        }
        return sf_round_pack(f, e, core_mul(ua, ub));
    case SOP_DIV:
        if (ca == FXC_INF) {
            if (cb == FXC_INF) break;
            return sf_inf(f, sa ^ sb);
        }
        if (cb == FXC_INF) return (sa ^ sb) ? FSIGN(f) : 0;
        if (cb == FXC_ZERO) {
            if (ca == FXC_ZERO) break;
            e->flags = (e->flags & ~FX_DE) | de_in | FX_ZE;
            return sf_inf(f, sa ^ sb);
        }
        return sf_round_pack(f, e, core_div(ua, ub));
    case SOP_SQRT:
        if (cb == FXC_ZERO) return b;
        if (sb) { e->flags = (e->flags & ~FX_DE) | de_in; break; } /* invalid suppresses the denormal flag */
        if (cb == FXC_INF) return b;
        return sf_round_pack(f, e, core_sqrt(ub));
    }
    e->flags |= FX_IE;
    return sf_default_nan(f);
}

uint32_t sf32_op(FxEnv *e, int op, uint32_t a, uint32_t b) { return (uint32_t)sf_op(&FMT32, e, op, a, b); }
uint64_t sf64_op(FxEnv *e, int op, uint64_t a, uint64_t b) { return sf_op(&FMT64, e, op, a, b); }

static int sf_cmp(const Fmt *f, FxEnv *e, uint64_t a, uint64_t b, int signal) {
    int ca = sf_class(f, a), cb = sf_class(f, b);
    if (ca == FXC_QNAN || ca == FXC_SNAN || cb == FXC_QNAN || cb == FXC_SNAN) {
        if (signal || ca == FXC_SNAN || cb == FXC_SNAN) e->flags |= FX_IE;
        return CMP_UN;
    }
    ca = sse_class(f, e, &a);
    cb = sse_class(f, e, &b);
    Unp ua = ca == FXC_INF ? (Unp){(a & FSIGN(f)) != 0, 1 << 20, TOP} : ca == FXC_ZERO ? (Unp){0, 0, 0} : sf_unpack(f, a);
    Unp ub = cb == FXC_INF ? (Unp){(b & FSIGN(f)) != 0, 1 << 20, TOP} : cb == FXC_ZERO ? (Unp){0, 0, 0} : sf_unpack(f, b);
    return cmp_unp(ua, ub);
}

int sf32_cmp(FxEnv *e, uint32_t a, uint32_t b, int s) { return sf_cmp(&FMT32, e, a, b, s); }
int sf64_cmp(FxEnv *e, uint64_t a, uint64_t b, int s) { return sf_cmp(&FMT64, e, a, b, s); }

static uint64_t sf_from_int(const Fmt *f, FxEnv *e, int sign, uint64_t mag) {
    if (!mag) return 0;
    return sf_round_pack(f, e, unp_from_u64(sign, mag));
}

uint32_t sf32_from_i64(FxEnv *e, int64_t v) { return (uint32_t)sf_from_int(&FMT32, e, v < 0, v < 0 ? 0 - (uint64_t)v : (uint64_t)v); }
uint64_t sf64_from_i64(FxEnv *e, int64_t v) { return sf_from_int(&FMT64, e, v < 0, v < 0 ? 0 - (uint64_t)v : (uint64_t)v); }
uint32_t sf32_from_u64(FxEnv *e, uint64_t v) { return (uint32_t)sf_from_int(&FMT32, e, 0, v); }
uint64_t sf64_from_u64(FxEnv *e, uint64_t v) { return sf_from_int(&FMT64, e, 0, v); }

static int64_t sf_to_int(const Fmt *f, FxEnv *e, uint64_t a, int bits, int trunc) {
    int c = sse_class_no_de(f, e, &a);
    if (c == FXC_QNAN || c == FXC_SNAN || c == FXC_INF) {
        e->flags |= FX_IE;
        return -(int64_t)((1ULL << (bits - 1)) - 1) - 1;
    }
    if (c == FXC_ZERO) return 0;
    return to_int_common(e, sf_unpack(f, a), bits, trunc ? RC_ZERO : e->rc);
}

int64_t sf32_to_int(FxEnv *e, uint32_t a, int bits, int t) { return sf_to_int(&FMT32, e, a, bits, t); }
int64_t sf64_to_int(FxEnv *e, uint64_t a, int bits, int t) { return sf_to_int(&FMT64, e, a, bits, t); }

static uint64_t sf_convert(const Fmt *from, const Fmt *to, FxEnv *e, uint64_t a) {
    int c = sse_class(from, e, &a);
    int sign = (a & FSIGN(from)) != 0;
    uint64_t s = sign ? FSIGN(to) : 0;
    switch (c) {
    case FXC_SNAN: e->flags |= FX_IE; /* fall through */
    case FXC_QNAN: {
        uint64_t fr = a & FFRAC(from);
        fr = to->fbits > from->fbits ? fr << (to->fbits - from->fbits) : fr >> (from->fbits - to->fbits);
        return s | (FEXPM(to) << to->fbits) | FQUIET(to) | fr;
    }
    case FXC_INF: return sf_inf(to, sign);
    case FXC_ZERO: return s;
    }
    return sf_round_pack(to, e, sf_unpack(from, a));
}

uint64_t sf32_to_f64(FxEnv *e, uint32_t a) { return sf_convert(&FMT32, &FMT64, e, a); }
uint32_t sf64_to_f32(FxEnv *e, uint64_t a) { return (uint32_t)sf_convert(&FMT64, &FMT32, e, a); }

static uint64_t sf_roundint(const Fmt *f, FxEnv *e, uint64_t a, int rc, int suppress) {
    int c = sse_class_no_de(f, e, &a);
    if (c == FXC_QNAN || c == FXC_SNAN) {
        if (c == FXC_SNAN) e->flags |= FX_IE;
        return sf_quiet(f, a);
    }
    if (c == FXC_ZERO || c == FXC_INF) return a;
    Unp u = sf_unpack(f, a);
    if (u.exp - BIAS >= f->fbits) return a;
    int inexact, ovf;
    u128 m = unp_to_uint(u, rc, &inexact, &ovf);
    if (inexact && !suppress) e->flags |= FX_PE;
    if (!m) return u.sign ? FSIGN(f) : 0;
    return sf_pack(f, unp_from_u64(u.sign, (uint64_t)m));
}

uint32_t sf32_round(FxEnv *e, uint32_t a, int rc, int s) { return (uint32_t)sf_roundint(&FMT32, e, a, rc, s); }
uint64_t sf64_round(FxEnv *e, uint64_t a, int rc, int s) { return sf_roundint(&FMT64, e, a, rc, s); }
