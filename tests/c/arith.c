/* Integer arithmetic across widths: exercises add/sub/mul/div, shifts,
 * rotates, bit scans, overflow checks, sign/zero extension. */
#include "../../runtime/sim.h"

static const uint64_t vals[] = {
    0, 1, 2, 3, 7, 10, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x12345, 0x7fffffff,
    0x80000000, 0xffffffff, 0x100000000ULL, 0x123456789ULL, 0x7fffffffffffffffULL,
    0x8000000000000000ULL, 0xfffffffffffffffeULL, 0xffffffffffffffffULL, 0xdeadbeefcafebabeULL,
};
#define NV (sizeof vals / sizeof vals[0])

static volatile uint64_t sink;

static void line(const char *name, uint64_t v) {
    print_str(name);
    print_char(' ');
    print_hex(v);
    print_nl();
}

__attribute__((noinline)) static uint64_t mix8(uint8_t a, uint8_t b) {
    uint64_t r = (uint8_t)(a + b);
    r = r * 31 + (uint8_t)(a - b);
    r = r * 31 + (uint8_t)(a * b);
    if (b) r = r * 31 + (uint8_t)(a / b) + (uint8_t)(a % b);
    int8_t sa = (int8_t)a, sb = (int8_t)b;
    if (sb && !(sa == -128 && sb == -1)) r = r * 31 + (uint8_t)(sa / sb) + (uint8_t)(sa % sb);
    r = r * 31 + (uint8_t)(a << (b & 7)) + (uint8_t)(a >> (b & 7)) + (uint8_t)(sa >> (b & 7));
    r = r * 31 + (a < b) + 2 * (sa < sb) + 4 * (a == b);
    return r;
}

__attribute__((noinline)) static uint64_t mix16(uint16_t a, uint16_t b) {
    uint64_t r = (uint16_t)(a + b);
    r = r * 31 + (uint16_t)(a - b);
    r = r * 31 + (uint16_t)(a * b);
    if (b) r = r * 31 + (uint16_t)(a / b) + (uint16_t)(a % b);
    int16_t sa = (int16_t)a, sb = (int16_t)b;
    if (sb && !(sa == -32768 && sb == -1)) r = r * 31 + (uint16_t)(sa / sb) + (uint16_t)(sa % sb);
    r = r * 31 + (uint16_t)(a << (b & 15)) + (uint16_t)(a >> (b & 15)) + (uint16_t)(sa >> (b & 15));
    r = r * 31 + (uint16_t)__builtin_bswap16(a);
    return r;
}

__attribute__((noinline)) static uint64_t mix32(uint32_t a, uint32_t b) {
    uint64_t r = a + b;
    r = r * 31 + (uint32_t)(a - b);
    r = r * 31 + (uint32_t)(a * b);
    r = r * 31 + ((uint64_t)a * b >> 32);
    if (b) r = r * 31 + a / b + a % b;
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sb && !(sa == INT32_MIN && sb == -1)) r = r * 31 + (uint32_t)(sa / sb) + (uint32_t)(sa % sb);
    r = r * 31 + (uint32_t)(a << (b & 31)) + (a >> (b & 31)) + (uint32_t)(sa >> (b & 31));
    r = r * 31 + ((a << (b & 31)) | (a >> ((32 - (b & 31)) & 31)));
    r = r * 31 + (uint32_t)__builtin_popcount(a);
    if (a) r = r * 31 + (uint32_t)__builtin_clz(a) + (uint32_t)__builtin_ctz(a);
    r = r * 31 + __builtin_bswap32(a);
    int32_t ov;
    r = r * 31 + __builtin_add_overflow(sa, sb, &ov) + 2 * __builtin_sub_overflow(sa, sb, &ov) +
        4 * __builtin_mul_overflow(sa, sb, &ov);
    return r;
}

/* High half of 64x64 products: native 128-bit math on x86-64 (mul/imul),
 * an exact 32-bit decomposition elsewhere, so every build prints the same. */
static uint64_t mulhi_u64(uint64_t a, uint64_t b) {
#ifdef __x86_64__
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
#else
    uint64_t al = (uint32_t)a, ah = a >> 32, bl = (uint32_t)b, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
#endif
}

static uint64_t mulhi_s64(int64_t a, int64_t b) {
#ifdef __x86_64__
    return (uint64_t)((unsigned __int128)((__int128)a * b) >> 64);
#else
    uint64_t h = mulhi_u64((uint64_t)a, (uint64_t)b);
    if (a < 0) h -= (uint64_t)b;
    if (b < 0) h -= (uint64_t)a;
    return h;
#endif
}

__attribute__((noinline)) static uint64_t mix64(uint64_t a, uint64_t b) {
    uint64_t r = a + b;
    r = r * 31 + (a - b);
    r = r * 31 + a * b;
    if (b) r = r * 31 + a / b + a % b;
    int64_t sa = (int64_t)a, sb = (int64_t)b;
    if (sb && !(sa == INT64_MIN && sb == -1)) r = r * 31 + (uint64_t)(sa / sb) + (uint64_t)(sa % sb);
    r = r * 31 + (a << (b & 63)) + (a >> (b & 63)) + (uint64_t)(sa >> (b & 63));
    r = r * 31 + ((a >> (b & 63)) | (a << ((64 - (b & 63)) & 63)));
    r = r * 31 + (uint64_t)__builtin_popcountll(a);
    if (a) r = r * 31 + (uint64_t)__builtin_clzll(a) + (uint64_t)__builtin_ctzll(a);
    r = r * 31 + __builtin_bswap64(a);
    int64_t ov;
    r = r * 31 + __builtin_add_overflow(sa, sb, &ov) + 2 * __builtin_sub_overflow(sa, sb, &ov);
    uint64_t uo;
    r = r * 31 + __builtin_mul_overflow(a, b, &uo);
    r = r * 31 + (sa < sb) + 2 * (a < b) + 4 * (sa >= sb) + 8 * (a >= b);
    r = r * 31 + mulhi_u64(a, b) + mulhi_s64(sa, sb);
    return r;
}

int main(void) {
    for (unsigned i = 0; i < NV; i++) {
        uint64_t h8 = 0, h16 = 0, h32 = 0, h64 = 0;
        for (unsigned j = 0; j < NV; j++) {
            h8 = h8 * 1000003 ^ mix8((uint8_t)vals[i], (uint8_t)vals[j]);
            h16 = h16 * 1000003 ^ mix16((uint16_t)vals[i], (uint16_t)vals[j]);
            h32 = h32 * 1000003 ^ mix32((uint32_t)vals[i], (uint32_t)vals[j]);
            h64 = h64 * 1000003 ^ mix64(vals[i], vals[j]);
        }
        print_hex(vals[i]);
        print_str(": ");
        print_hex(h8);
        print_char(' ');
        print_hex(h16);
        print_char(' ');
        print_hex(h32);
        print_char(' ');
        print_hex(h64);
        print_nl();
    }
    /* sign/zero extension through memory */
    for (unsigned i = 0; i < NV; i++) {
        volatile int8_t c = (int8_t)vals[i];
        volatile int16_t s = (int16_t)vals[i];
        volatile int32_t w = (int32_t)vals[i];
        int64_t e = (int64_t)c + (int64_t)s * 3 + (int64_t)w * 5;
        uint64_t z = (uint64_t)(uint8_t)c + (uint64_t)(uint16_t)s * 3 + (uint64_t)(uint32_t)w * 5;
        line("ext", (uint64_t)e ^ (z << 1));
    }
    return 0;
}
