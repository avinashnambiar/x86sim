/* SSE/SSE2 floating point, SSE3, SSSE3, SSE4.1, SSE4.2, AES-NI, PCLMULQDQ,
 * MMX and SHA, all through compiler intrinsics. Results are printed as raw
 * bits (or folded into hashes) and compared exactly with the reference CPU.
 *
 * The reference (Rosetta) has no SHA extensions; the SHA part checks CPUID
 * and falls back to a portable implementation, so both sides must print
 * the same digests. */
#include "../../runtime/sim.h"

/* Keep the compiler from moving FP operations across MXCSR accesses. */
#pragma STDC FENV_ACCESS ON

#pragma clang attribute push(__attribute__((target("sse4.2,aes,pclmul"))), apply_to = function)
#include <immintrin.h>

static uint64_t hash = 0xcbf29ce484222325ULL;
/* Build with -DTRACE to print every folded value with its source line. */
static void mix_(const void *p, int n, int line) {
    const uint8_t *b = p;
    for (int i = 0; i < n; i++) hash = (hash ^ b[i]) * 0x100000001b3ULL;
#ifdef TRACE
    print_u64((uint64_t)line);
    for (int i = 0; i < n; i++) {
        print_str(i % 8 ? "" : " ");
        print_char("0123456789abcdef"[b[n - 1 - i] >> 4]);
        print_char("0123456789abcdef"[b[n - 1 - i] & 15]);
    }
    print_nl();
#else
    (void)line;
#endif
}
static void mixv_(__m128i v, int line) {
    uint8_t t[16];
    _mm_storeu_si128((__m128i *)t, v);
    mix_(t, 16, line);
}
#define mix(p, n) mix_(p, n, __LINE__)
#define mixv(v) mixv_(v, __LINE__)
#define mixps(v) mixv_(_mm_castps_si128(v), __LINE__)
#define mixpd(v) mixv_(_mm_castpd_si128(v), __LINE__)
#define flags() flags_(__LINE__)

/* x86-64-only checks fold into their own hash and draw from their own
 * random stream, so the shared output is the same on i386; they are printed
 * on "x64:" lines, which the test runner drops for 32-bit builds. */
static uint64_t hash64 = 0xcbf29ce484222325ULL, rng64 = 0x2545F4914F6CDD1DULL;
static uint64_t rnd64(void) {
    rng64 ^= rng64 << 13;
    rng64 ^= rng64 >> 7;
    rng64 ^= rng64 << 17;
    return rng64;
}
static void mix64(const void *p, int n) {
    const uint8_t *b = p;
    for (int i = 0; i < n; i++) hash64 = (hash64 ^ b[i]) * 0x100000001b3ULL;
}
#define mix64v(v) do { __m128i t_ = (v); mix64(&t_, 16); } while (0)
#define flags64() do { unsigned f_ = _mm_getcsr() & 0x3F; mix64(&f_, 4); _mm_setcsr(_mm_getcsr() & ~0x3Fu); } while (0)

static void report(const char *name) {
    print_str(name);
    print_str(" ");
    print_hex(hash);
    print_nl();
    hash = 0xcbf29ce484222325ULL;
}

static void pv(const char *name, __m128i v) {
    uint64_t q[2];
    _mm_storeu_si128((__m128i *)q, v);
    print_str(name);
    print_str(" ");
    print_hex(q[1]);
    print_str(":");
    print_hex(q[0]);
    print_nl();
}


/* Intrinsic immediates must be constants: dispatch a runtime value. */
#define SW16(v, E) switch ((v) & 15) { case 0: E(0); break; case 1: E(1); break; case 2: E(2); break; case 3: E(3); break; case 4: E(4); break; case 5: E(5); break; case 6: E(6); break; case 7: E(7); break; case 8: E(8); break; case 9: E(9); break; case 10: E(10); break; case 11: E(11); break; case 12: E(12); break; case 13: E(13); break; case 14: E(14); break; case 15: E(15); break; }
#define SW8(v, E) switch ((v) & 7) { case 0: E(0); break; case 1: E(1); break; case 2: E(2); break; case 3: E(3); break; case 4: E(4); break; case 5: E(5); break; case 6: E(6); break; case 7: E(7); break; }

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* Random doubles/floats biased toward interesting encodings. */
static const uint64_t special64[] = {
    0, 0x8000000000000000ULL, 0x3FF0000000000000ULL, 0xBFF0000000000000ULL, 0x7FF0000000000000ULL,
    0xFFF0000000000000ULL, 0x7FF8000000000000ULL, 0x7FF4000000000000ULL, 0xFFF8000000000001ULL,
    0x0000000000000001ULL, 0x000FFFFFFFFFFFFFULL, 0x0010000000000000ULL, 0x7FEFFFFFFFFFFFFFULL,
    0x43E0000000000000ULL, 0xC3E0000000000000ULL, 0x41E0000000000000ULL, 0xC1E0000000200000ULL,
    0x3FE0000000000000ULL, 0x4008000000000000ULL,
};
static const uint32_t special32[] = {
    0, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x7FA00000u,
    0xFFC00001u, 0x00000001u, 0x007FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0x5F000000u, 0xDF000000u,
    0x4F000000u, 0xCF000000u, 0x3F000000u, 0x40400000u,
};
static uint64_t r64(void) {
    uint64_t x = rnd();
    switch (x & 7) {
    case 0: return special64[(x >> 8) % (sizeof special64 / 8)];
    case 1: return (x & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)(1023 + (int)((x >> 52) & 31) - 15) << 52);
    case 2: return x & 0x800FFFFFFFFFFFFFULL; /* denormal */
    default: return (x & 0x800FFFFFFFFFFFFFULL) | ((uint64_t)(900 + (x >> 20) % 250) << 52);
    }
}
static uint32_t r32(void) {
    uint64_t x = rnd();
    switch (x & 7) {
    case 0: return special32[(x >> 8) % (sizeof special32 / 4)];
    case 1: return (uint32_t)((x & 0x807FFFFF) | ((uint32_t)(127 + (int)((x >> 40) & 15) - 7) << 23));
    case 2: return (uint32_t)(x & 0x807FFFFF);
    default: return (uint32_t)((x & 0x807FFFFF) | ((uint32_t)(60 + (x >> 33) % 130) << 23));
    }
}
static __m128d rpd(void) { return _mm_castsi128_pd(_mm_set_epi64x((long long)r64(), (long long)r64())); }
static __m128 rps(void) { return _mm_castsi128_ps(_mm_set_epi32((int)r32(), (int)r32(), (int)r32(), (int)r32())); }
static __m128i rvi(void) { return _mm_set_epi64x((long long)rnd(), (long long)rnd()); }

/* The compiler may commute add/mul intrinsics, which changes which NaN
 * operand is propagated; pin the operand order with inline asm. */
#define ASM2(name, insn, T)                                                   \
    static T name(T a, T b) {                                                 \
        __asm__ volatile(insn " %1, %0" : "+x"(a) : "x"(b));                  \
        return a;                                                             \
    }
ASM2(addpd, "addpd", __m128d)
ASM2(mulpd, "mulpd", __m128d)
ASM2(addsd, "addsd", __m128d)
ASM2(addps, "addps", __m128)
ASM2(mulps, "mulps", __m128)
ASM2(mulss, "mulss", __m128)

/* Rosetta's HSUBPS/ADDSUBPx flip the sign of a NaN subtrahend (hardware
 * does not), so keep NaNs out of those operands. */
static __m128 nonan(__m128 v) { return _mm_and_ps(v, _mm_cmpord_ps(v, v)); }
static __m128d nonanpd(__m128d v) { return _mm_and_pd(v, _mm_cmpord_pd(v, v)); }

/* Rosetta reports DE for (QNaN, denormal) in CMPUNORD/CMPORD only. */
#define flags_no_de() (_mm_setcsr(_mm_getcsr() & ~2u), flags())

static void flags_(int line) {
    unsigned f = _mm_getcsr() & 0x3F;
    mix_(&f, 4, line);
    _mm_setcsr(_mm_getcsr() & ~0x3Fu);
}

static const unsigned csrs[] = {0x1F80, 0x3F80, 0x5F80, 0x7F80, 0x9FC0, 0x1FC0, 0x9F80};
#define NCSR (int)(sizeof csrs / sizeof csrs[0])

static void fp_arith(void) {
    for (int m = 0; m < NCSR; m++) {
        _mm_setcsr(csrs[m]);
        for (int i = 0; i < 300; i++) {
            __m128d a = rpd(), b = rpd();
            __m128 x = rps(), y = rps();
            mixpd(addpd(a, b)); flags();
            mixpd(_mm_sub_pd(a, b)); flags();
            mixpd(mulpd(a, b)); flags();
            mixpd(_mm_div_pd(a, b)); flags();
            mixpd(_mm_sqrt_pd(a)); flags();
            mixpd(_mm_min_pd(a, b)); flags();
            mixpd(_mm_max_pd(a, b)); flags();
            mixpd(addsd(a, b)); flags();
            mixpd(_mm_div_sd(a, b)); flags();
            mixpd(_mm_sqrt_sd(a, b)); flags();
            mixps(addps(x, y)); flags();
            mixps(_mm_sub_ps(x, y)); flags();
            mixps(mulps(x, y)); flags();
            mixps(_mm_div_ps(x, y)); flags();
            mixps(_mm_sqrt_ps(x)); flags();
            mixps(_mm_min_ps(x, y)); flags();
            mixps(_mm_max_ps(x, y)); flags();
            mixps(mulss(x, y)); flags();
            mixps(_mm_sqrt_ss(x)); flags();
            mixpd(_mm_addsub_pd(a, nonanpd(b))); flags();
            mixps(_mm_addsub_ps(x, nonan(y))); flags();
            mixpd(_mm_hadd_pd(a, b)); flags();
            mixps(_mm_hsub_ps(nonan(x), nonan(y))); flags();
#define E(k) mixps(_mm_dp_ps(x, y, 0xF1 ^ ((k) * 0x1D & 0xEE)))
            SW16(i, E)
#undef E
            flags();
#define E(k) mixpd(_mm_dp_pd(a, b, 0x31 ^ ((k) * 0x12 & 0x32)))
            SW8(i, E)
#undef E
            flags();
        }
        report("arith");
    }
    _mm_setcsr(0x1F80);
}

static void fp_compare(void) {
    for (int i = 0; i < 400; i++) {
        __m128d a = rpd(), b = i & 1 ? rpd() : a;
        __m128 x = rps(), y = i & 1 ? rps() : x;
        mixpd(_mm_cmpeq_pd(a, b)); flags();
        mixpd(_mm_cmplt_pd(a, b)); flags();
        mixpd(_mm_cmple_pd(a, b)); flags();
        mixpd(_mm_cmpunord_pd(a, b)); flags_no_de();
        mixpd(_mm_cmpneq_pd(a, b)); flags();
        mixpd(_mm_cmpnlt_pd(a, b)); flags();
        mixpd(_mm_cmpnle_pd(a, b)); flags();
        mixpd(_mm_cmpord_sd(a, b)); flags_no_de();
        mixps(_mm_cmpeq_ps(x, y)); flags();
        mixps(_mm_cmplt_ps(x, y)); flags();
        mixps(_mm_cmpunord_ps(x, y)); flags_no_de();
        mixps(_mm_cmpnle_ss(x, y)); flags();
        int r = _mm_comieq_sd(a, b) | _mm_comilt_sd(a, b) << 1 | _mm_comile_sd(a, b) << 2 |
                _mm_comigt_sd(a, b) << 3 | _mm_ucomineq_sd(a, b) << 4 | _mm_ucomige_sd(a, b) << 5 |
                _mm_comieq_ss(x, y) << 6 | _mm_ucomilt_ss(x, y) << 7 | _mm_comineq_ss(x, y) << 8;
        mix(&r, 4);
        flags();
    }
    report("compare");
}

static void fp_convert(void) {
    for (int m = 0; m < 4; m++) {
        _mm_setcsr(0x1F80 | (unsigned)m << 13);
        for (int i = 0; i < 300; i++) {
            __m128d a = rpd();
            __m128 x = rps();
            __m128i v = rvi();
            mixv(_mm_cvtpd_epi32(a)); flags();
            mixv(_mm_cvttpd_epi32(a)); flags();
            mixv(_mm_cvtps_epi32(x)); flags();
            mixv(_mm_cvttps_epi32(x)); flags();
            mixps(_mm_cvtpd_ps(a)); flags();
            mixpd(_mm_cvtps_pd(x)); flags();
            mixps(_mm_cvtepi32_ps(v)); flags();
            mixpd(_mm_cvtepi32_pd(v)); flags();
            int k = _mm_cvtsd_si32(a) ^ _mm_cvttss_si32(x) * 3 ^ _mm_cvtss_si32(x) * 7 ^ _mm_cvttsd_si32(a) * 13;
            mix(&k, 4); flags();
#ifdef __x86_64__
            long long q = _mm_cvtsd_si64(a) ^ _mm_cvttsd_si64(a) * 3 ^ _mm_cvtss_si64(x) * 5 ^ _mm_cvttss_si64(x) * 9;
            mix64(&q, 8); flags64();
            mix64v(_mm_castpd_si128(_mm_cvtsi64_sd(a, (long long)rnd64() >> (rnd64() & 63)))); flags64();
            mix64v(_mm_castps_si128(_mm_cvtsi64_ss(x, (long long)rnd64() >> (rnd64() & 63)))); flags64();
#endif
            mixpd(_mm_cvtsi32_sd(a, (int)rnd())); flags();
            mixps(_mm_cvtsi32_ss(x, (int)rnd())); flags();
            mixps(_mm_cvtsd_ss(x, a)); flags();
            mixpd(_mm_cvtss_sd(a, x)); flags();
            unsigned rm = (unsigned)rnd();
#define E(k) mixpd(_mm_round_pd(a, k))
            SW16(rm, E)
#undef E
            flags();
#define E(k) mixps(_mm_round_ps(x, k))
            SW16(rm >> 4, E)
#undef E
            flags();
#define E(k) mixpd(_mm_round_sd(a, a, k))
            SW16(rm >> 8, E)
#undef E
            flags();
#define E(k) mixps(_mm_round_ss(x, x, k))
            SW16(rm >> 12, E)
#undef E
            flags();
        }
        report("convert");
    }
    _mm_setcsr(0x1F80);
}

/* RCP/RSQRT are approximations whose exact bits differ between CPUs; only
 * check the architectural error bound. */
static void fp_approx(void) {
    int ok = 1;
    for (int i = 1; i < 2000; i++) {
        float f = (float)i * 0.37f + 0.001f, r[4], s[4];
        _mm_storeu_ps(r, _mm_rcp_ps(_mm_set1_ps(f)));
        _mm_storeu_ps(s, _mm_rsqrt_ps(_mm_set1_ps(f)));
        double er = r[0] * (double)f - 1, es = s[0] * s[0] * (double)f - 1;
        if (er < 0) er = -er;
        if (es < 0) es = -es;
        if (er > 1.5 / 4096 || es > 3.0 / 4096) ok = 0;
    }
    print_str(ok ? "approx ok\n" : "approx BAD\n");
}

static void moves(void) {
    double d[4] = {1.5, -2.25, 3.0, 4.0};
    float f[4] = {1, 2, 3, 4};
    __m128d a = _mm_loadu_pd(d + 1);
    __m128 x = _mm_loadu_ps(f);
    mixpd(_mm_loadh_pd(a, d)); mixpd(_mm_loadl_pd(a, d + 3)); mixpd(_mm_movedup_pd(a));
    mixps(_mm_movehdup_ps(x)); mixps(_mm_moveldup_ps(x)); mixps(_mm_movehl_ps(x, _mm_set1_ps(9)));
    mixps(_mm_movelh_ps(x, _mm_set1_ps(9))); mixps(_mm_shuffle_ps(x, x, 0x1B));
    mixpd(_mm_shuffle_pd(a, a, 1)); mixps(_mm_unpackhi_ps(x, x)); mixpd(_mm_unpacklo_pd(a, a));
    mixpd(_mm_load_sd(d + 2)); mixps(_mm_load_ss(f + 3)); mixps(_mm_move_ss(x, _mm_set1_ps(7)));
    int m = _mm_movemask_ps(_mm_set_ps(-1, 2, -3, 4)) | _mm_movemask_pd(_mm_set_pd(-1, 2)) << 4;
    mix(&m, 4);
    for (int i = 0; i < 100; i++) {
        __m128 p = rps(), q = rps();
        __m128d u = rpd(), w = rpd();
        unsigned im = (unsigned)rnd();
#define E(k) mixps(_mm_insert_ps(p, q, (k) * 0x11 ^ 0x5A))
        SW16(im, E)
#undef E
        int e = _mm_extract_ps(p, 2);
        mix(&e, 4);
        mixps(_mm_blend_ps(p, q, 5)); mixpd(_mm_blend_pd(u, w, 2));
        mixps(_mm_blendv_ps(p, q, rps())); mixpd(_mm_blendv_pd(u, w, rpd()));
        mixps(_mm_and_ps(p, q)); mixps(_mm_andnot_ps(p, q)); mixpd(_mm_or_pd(u, w)); mixpd(_mm_xor_pd(u, w));
    }
    report("moves");
}

static void integer(void) {
    for (int i = 0; i < 500; i++) {
        __m128i a = rvi(), b = rvi(), c = rvi();
        if (i & 1) b = _mm_and_si128(b, _mm_set1_epi8(0x8F)); /* pshufb indices */
        mixv(_mm_shuffle_epi8(a, b));
        mixv(_mm_alignr_epi8(a, b, 0)); mixv(_mm_alignr_epi8(a, b, 5)); mixv(_mm_alignr_epi8(a, b, 16));
        mixv(_mm_alignr_epi8(a, b, 21)); mixv(_mm_alignr_epi8(a, b, 33));
        mixv(_mm_hadd_epi16(a, b)); mixv(_mm_hadd_epi32(a, b)); mixv(_mm_hadds_epi16(a, b));
        mixv(_mm_hsub_epi16(a, b)); mixv(_mm_hsub_epi32(a, b)); mixv(_mm_hsubs_epi16(a, b));
        mixv(_mm_maddubs_epi16(a, b)); mixv(_mm_mulhrs_epi16(a, b));
        mixv(_mm_sign_epi8(a, b)); mixv(_mm_sign_epi16(a, b)); mixv(_mm_sign_epi32(a, b));
        mixv(_mm_abs_epi8(a)); mixv(_mm_abs_epi16(a)); mixv(_mm_abs_epi32(a));
        mixv(_mm_blendv_epi8(a, b, c)); mixv(_mm_blend_epi16(a, b, 0xA5));
        mixv(_mm_cvtepi8_epi16(a)); mixv(_mm_cvtepi8_epi32(a)); mixv(_mm_cvtepi8_epi64(a));
        mixv(_mm_cvtepi16_epi32(a)); mixv(_mm_cvtepi16_epi64(a)); mixv(_mm_cvtepi32_epi64(a));
        mixv(_mm_cvtepu8_epi16(a)); mixv(_mm_cvtepu8_epi32(a)); mixv(_mm_cvtepu8_epi64(a));
        mixv(_mm_cvtepu16_epi32(a)); mixv(_mm_cvtepu16_epi64(a)); mixv(_mm_cvtepu32_epi64(a));
        mixv(_mm_min_epi8(a, b)); mixv(_mm_max_epi8(a, b)); mixv(_mm_min_epu16(a, b)); mixv(_mm_max_epu16(a, b));
        mixv(_mm_min_epi32(a, b)); mixv(_mm_max_epi32(a, b)); mixv(_mm_min_epu32(a, b)); mixv(_mm_max_epu32(a, b));
        mixv(_mm_mullo_epi32(a, b)); mixv(_mm_mul_epi32(a, b)); mixv(_mm_cmpeq_epi64(a, i & 2 ? a : b));
        mixv(_mm_cmpgt_epi64(a, b)); mixv(_mm_packus_epi32(a, b)); mixv(_mm_minpos_epu16(a));
#define E(k) mixv(_mm_mpsadbw_epu8(a, b, k))
        SW8(i, E)
#undef E
        mixv(_mm_insert_epi8(a, (int)rnd(), 9)); mixv(_mm_insert_epi32(a, (int)rnd(), 2));
        int t = _mm_testz_si128(a, b) | _mm_testc_si128(a, i & 4 ? a : b) << 1 | _mm_testnzc_si128(a, b) << 2;
        t ^= _mm_extract_epi8(a, 13) << 3 ^ _mm_extract_epi32(a, 3) ^ _mm_extract_epi16(a, 6);
        mix(&t, 4);
#ifdef __x86_64__
        mix64v(_mm_insert_epi64(a, (long long)rnd64(), 1));
        long long e = _mm_extract_epi64(a, 1);
        mix64(&e, 8);
#endif
        /* SSE2 integer forms through the same unit */
        mixv(_mm_sad_epu8(a, b)); mixv(_mm_madd_epi16(a, b)); mixv(_mm_packs_epi32(a, b));
        mixv(_mm_packus_epi16(a, b)); mixv(_mm_unpackhi_epi8(a, b)); mixv(_mm_srli_si128(a, 3));
        mixv(_mm_slli_epi16(a, (int)(i & 15))); mixv(_mm_srai_epi32(a, (int)(i & 31)));
        mixv(_mm_sll_epi64(a, _mm_cvtsi32_si128(i % 70))); mixv(_mm_shufflehi_epi16(a, 0x6C));
    }
    report("integer");
}

static void strings(void) {
    static const int modes[] = {0x00, 0x04, 0x08, 0x0C, 0x01, 0x05, 0x09, 0x0D, 0x02, 0x06, 0x0A, 0x0E,
                                0x10, 0x34, 0x18, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x71, 0x5D, 0x03, 0x0F};
    for (int i = 0; i < 300; i++) {
        uint8_t s[16], t[16];
        for (int k = 0; k < 16; k++) {
            s[k] = (uint8_t)("abcxyz09\0AZ"[rnd() % 11]);
            t[k] = (uint8_t)("abcdxyz019\0AZ"[rnd() % 13]);
        }
        if (i % 3 == 0)
            for (int k = 0; k < 5; k++) t[k + 2] = s[k];
        __m128i a = _mm_loadu_si128((const __m128i *)s), b = _mm_loadu_si128((const __m128i *)t);
        int la = (int)(rnd() % 20) - 2, lb = (int)(rnd() % 20) - 2;
#define ONE(M)                                                                                    \
    do {                                                                                          \
        int r = _mm_cmpistri(a, b, M) | _mm_cmpestri(a, la, b, lb, M) << 5;                     \
        r |= _mm_cmpistrc(a, b, M) << 10 | _mm_cmpistrz(a, b, M) << 11 | _mm_cmpistrs(a, b, M) << 12 | \
             _mm_cmpistro(a, b, M) << 13 | _mm_cmpestra(a, la, b, lb, M) << 14 |                 \
             _mm_cmpestrz(a, la, b, lb, M) << 15 | _mm_cmpestrs(a, la, b, lb, M) << 16;         \
        mix(&r, 4);                                                                               \
        mixv(_mm_cmpistrm(a, b, M));                                                              \
        mixv(_mm_cmpestrm(a, la, b, lb, M));                                                      \
    } while (0)
        ONE(0x00); ONE(0x04); ONE(0x08); ONE(0x0C); ONE(0x01); ONE(0x05); ONE(0x09); ONE(0x0D);
        ONE(0x02); ONE(0x06); ONE(0x0A); ONE(0x0E); ONE(0x10); ONE(0x34); ONE(0x18); ONE(0x3C);
        ONE(0x40); ONE(0x44); ONE(0x48); ONE(0x4C); ONE(0x71); ONE(0x5D); ONE(0x03); ONE(0x0F);
#undef ONE
    }
    (void)modes;
    report("pcmpstr");
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < 1000; i++) {
        uint64_t x = rnd();
        crc = _mm_crc32_u8(crc, (uint8_t)x);
        crc = _mm_crc32_u16(crc, (uint16_t)(x >> 8));
        crc = _mm_crc32_u32(crc, (uint32_t)(x >> 24));
#ifdef __x86_64__
        uint32_t c64 = (uint32_t)_mm_crc32_u64(crc, x);
        mix64(&c64, 4);
#endif
    }
    print_str("crc32c ");
    print_hex(crc);
    print_nl();
}

static void crypto(void) {
    /* FIPS-197 C.1: AES-128 */
    __m128i key = _mm_set_epi64x(0x0f0e0d0c0b0a0908LL, 0x0706050403020100LL);
    __m128i pt = _mm_set_epi64x((long long)0xffeeddccbbaa9988ULL, 0x7766554433221100LL);
    __m128i rk[11];
    rk[0] = key;
#define EXPAND(i, rcon)                                                          \
    do {                                                                         \
        __m128i t = _mm_aeskeygenassist_si128(rk[i - 1], rcon);                  \
        t = _mm_shuffle_epi32(t, 0xFF);                                          \
        __m128i k = rk[i - 1];                                                   \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4));                              \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4));                              \
        k = _mm_xor_si128(k, _mm_slli_si128(k, 4));                              \
        rk[i] = _mm_xor_si128(k, t);                                             \
    } while (0)
    EXPAND(1, 0x01); EXPAND(2, 0x02); EXPAND(3, 0x04); EXPAND(4, 0x08); EXPAND(5, 0x10);
    EXPAND(6, 0x20); EXPAND(7, 0x40); EXPAND(8, 0x80); EXPAND(9, 0x1B); EXPAND(10, 0x36);
    __m128i s = _mm_xor_si128(pt, rk[0]);
    for (int i = 1; i < 10; i++) s = _mm_aesenc_si128(s, rk[i]);
    s = _mm_aesenclast_si128(s, rk[10]);
    pv("aes128", s);
    s = _mm_xor_si128(s, rk[10]);
    for (int i = 9; i > 0; i--) s = _mm_aesdec_si128(s, _mm_aesimc_si128(rk[i]));
    s = _mm_aesdeclast_si128(s, rk[0]);
    pv("aes128 decrypt", s);
    for (int i = 0; i < 300; i++) {
        __m128i a = rvi(), b = rvi();
        mixv(_mm_aesenc_si128(a, b)); mixv(_mm_aesdec_si128(a, b)); mixv(_mm_aesenclast_si128(a, b));
        mixv(_mm_aesdeclast_si128(a, b)); mixv(_mm_aesimc_si128(a)); mixv(_mm_aeskeygenassist_si128(a, 0x6D));
        mixv(_mm_clmulepi64_si128(a, b, 0x00)); mixv(_mm_clmulepi64_si128(a, b, 0x01));
        mixv(_mm_clmulepi64_si128(a, b, 0x10)); mixv(_mm_clmulepi64_si128(a, b, 0x11));
    }
    report("aes+clmul");
}

static void mmx(void) {
    for (int i = 0; i < 300; i++) {
        __m64 a = (__m64)rnd(), b = (__m64)rnd();
        __m64 r[] = {
            _mm_add_pi8(a, b), _mm_adds_pi16(a, b), _mm_subs_pu8(a, b), _mm_mullo_pi16(a, b),
            _mm_mulhi_pi16(a, b), _mm_madd_pi16(a, b), _mm_packs_pi16(a, b), _mm_packs_pi32(a, b),
            _mm_packs_pu16(a, b), _mm_unpackhi_pi8(a, b), _mm_unpackhi_pi16(a, b), _mm_unpackhi_pi32(a, b),
            _mm_unpacklo_pi8(a, b), _mm_cmpgt_pi16(a, b), _mm_cmpeq_pi8(a, a), _mm_sll_pi16(a, (__m64)(uint64_t)(i % 20)),
            _mm_srai_pi32(a, i % 40), _mm_srli_si64(a, i % 70), _mm_xor_si64(a, b), _mm_andnot_si64(a, b),
            _mm_shuffle_pi8(a, b), _mm_alignr_pi8(a, b, 3), _mm_alignr_pi8(a, b, 9), _mm_hadd_pi16(a, b),
            _mm_hsub_pi32(a, b), _mm_abs_pi8(a), _mm_sign_pi16(a, b), _mm_maddubs_pi16(a, b),
            _mm_mulhrs_pi16(a, b), _mm_avg_pu8(a, b), _mm_max_pi16(a, b), _mm_min_pu8(a, b),
            _mm_sad_pu8(a, b), _mm_mulhi_pu16(a, b), _mm_shuffle_pi16(a, 0x4E), _mm_insert_pi16(a, i, 2),
            _mm_add_si64(a, b), _mm_mul_su32(a, b),
        };
        mix(r, sizeof r);
        int m = _mm_movemask_pi8(a) | _mm_extract_pi16(a, 3) << 8;
        mix(&m, 4);
        __m128i x = _mm_movpi64_epi64(a);
        mixv(x);
        __m64 y = _mm_movepi64_pi64(rvi());
        mix(&y, 8);
        mixps(_mm_cvtpi32_ps(rps(), a));
        mixpd(_mm_cvtpi32_pd(a));
        __m64 c = _mm_cvtps_pi32(rps()), d = _mm_cvttpd_pi32(rpd());
        mix(&c, 8);
        mix(&d, 8);
    }
    _mm_empty();
    flags();
    report("mmx");
}

/* ---- SHA: hardware path if CPUID has it, portable path otherwise ---- */

static int has_sha(void) {
    unsigned a = 7, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (b >> 29) & 1;
}

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha1_soft(uint32_t h[5], const uint8_t blk[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)blk[4 * i] << 24 | blk[4 * i + 1] << 16 | blk[4 * i + 2] << 8 | blk[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static void sha256_soft(uint32_t h[8], const uint8_t blk[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)blk[4 * i] << 24 | blk[4 * i + 1] << 16 | blk[4 * i + 2] << 8 | blk[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* Intel's reference flow for one block with the SHA extensions. */
__attribute__((target("sha,sse4.1"))) static void sha1_hw(uint32_t h[5], const uint8_t blk[64]) {
    const __m128i bswap = _mm_set_epi64x(0x0001020304050607LL, 0x08090a0b0c0d0e0fLL);
    __m128i abcd = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)h), 0x1B);
    __m128i e0 = _mm_set_epi32((int)h[4], 0, 0, 0), e, prev = abcd;
    __m128i abcd_save = abcd, e_save = e0;
    __m128i m[4];
    for (int i = 0; i < 4; i++) m[i] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(blk + 16 * i)), bswap);
    for (int r = 0; r < 20; r++) {
        if (r >= 4) /* W[r] = msg2(msg1(W[r-4], W[r-3]) ^ W[r-2], W[r-1]) */
            m[r & 3] = _mm_sha1msg2_epu32(_mm_xor_si128(_mm_sha1msg1_epu32(m[r & 3], m[(r + 1) & 3]), m[(r + 2) & 3]),
                                          m[(r + 3) & 3]);
        e = r == 0 ? _mm_add_epi32(e0, m[0]) : _mm_sha1nexte_epu32(prev, m[r & 3]);
        prev = abcd;
        switch (r / 5) {
        case 0: abcd = _mm_sha1rnds4_epu32(abcd, e, 0); break;
        case 1: abcd = _mm_sha1rnds4_epu32(abcd, e, 1); break;
        case 2: abcd = _mm_sha1rnds4_epu32(abcd, e, 2); break;
        default: abcd = _mm_sha1rnds4_epu32(abcd, e, 3); break;
        }
    }
    e0 = _mm_sha1nexte_epu32(prev, e_save);
    abcd = _mm_add_epi32(abcd, abcd_save);
    _mm_storeu_si128((__m128i *)h, _mm_shuffle_epi32(abcd, 0x1B));
    h[4] = (uint32_t)_mm_extract_epi32(e0, 3);
}

__attribute__((target("sha,sse4.1"))) static void sha256_hw(uint32_t h[8], const uint8_t blk[64]) {
    const __m128i bswap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    __m128i t = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)h), 0xB1);        /* CDAB */
    __m128i s1 = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)(h + 4)), 0x1B); /* EFGH */
    __m128i s0 = _mm_alignr_epi8(t, s1, 8);                                          /* ABEF */
    s1 = _mm_blend_epi16(s1, t, 0xF0);                                               /* CDGH */
    __m128i save0 = s0, save1 = s1, m[4];
    for (int i = 0; i < 4; i++) m[i] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(blk + 16 * i)), bswap);
    for (int r = 0; r < 16; r++) {
        if (r >= 4) {
            __m128i x = _mm_sha256msg1_epu32(m[r & 3], m[(r + 1) & 3]);
            x = _mm_add_epi32(x, _mm_alignr_epi8(m[(r + 3) & 3], m[(r + 2) & 3], 4));
            m[r & 3] = _mm_sha256msg2_epu32(x, m[(r + 3) & 3]);
        }
        __m128i k = _mm_add_epi32(m[r & 3], _mm_loadu_si128((const __m128i *)(K256 + 4 * r)));
        s1 = _mm_sha256rnds2_epu32(s1, s0, k);
        s0 = _mm_sha256rnds2_epu32(s0, s1, _mm_shuffle_epi32(k, 0x0E));
    }
    s0 = _mm_add_epi32(s0, save0);
    s1 = _mm_add_epi32(s1, save1);
    t = _mm_shuffle_epi32(s0, 0x1B);  /* FEBA */
    s1 = _mm_shuffle_epi32(s1, 0xB1); /* DCHG */
    _mm_storeu_si128((__m128i *)h, _mm_blend_epi16(t, s1, 0xF0));       /* DCBA */
    _mm_storeu_si128((__m128i *)(h + 4), _mm_alignr_epi8(s1, t, 8));    /* HGFE */
}

static void sha(void) {
    int hw = has_sha();
    uint8_t blk[64];
    uint32_t h1[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint32_t h2[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    for (int n = 0; n < 50; n++) {
        for (int i = 0; i < 64; i++) blk[i] = (uint8_t)rnd();
        if (hw) { sha1_hw(h1, blk); sha256_hw(h2, blk); }
        else { sha1_soft(h1, blk); sha256_soft(h2, blk); }
    }
    mix(h1, sizeof h1);
    report("sha1");
    mix(h2, sizeof h2);
    report("sha256");
}

int main(void) {
    fp_arith();
    fp_compare();
    fp_convert();
    fp_approx();
    moves();
    integer();
    strings();
    crypto();
    mmx();
    sha();
#ifdef __x86_64__
    print_str("x64: ");
    print_hex(hash64);
    print_nl();
#endif
    return 0;
}
#pragma clang attribute pop
