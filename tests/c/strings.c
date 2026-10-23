/* Byte/string processing that the optimizer likes to vectorize (SSE2),
 * plus string instructions via inline asm. */
#include "../../runtime/sim.h"

static char text[4096];
static uint8_t buf_a[1024], buf_b[1024];

__attribute__((noinline)) static int count_char(const char *s, size_t n, char c) {
    int k = 0;
    for (size_t i = 0; i < n; i++) k += s[i] == c;
    return k;
}

__attribute__((noinline)) static void add_arrays(uint32_t *d, const uint32_t *a, const uint32_t *b, int n) {
    for (int i = 0; i < n; i++) d[i] = a[i] + b[i] * 3;
}

__attribute__((noinline)) static uint64_t sum_u64(const uint64_t *a, int n) {
    uint64_t s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return s;
}

static void reverse(char *s, size_t n) {
    for (size_t i = 0; i < n / 2; i++) {
        char t = s[i];
        s[i] = s[n - 1 - i];
        s[n - 1 - i] = t;
    }
}

int main(void) {
    const char *pangram = "Pack my box with five dozen liquor jugs. ";
    size_t pl = sim_strlen(pangram), n = 0;
    while (n + pl < sizeof text - 1) {
        for (size_t i = 0; i < pl; i++) text[n + i] = pangram[i];
        n += pl;
    }
    text[n] = 0;
    print_str("len=");
    print_u64(sim_strlen(text));
    print_str(" spaces=");
    print_i64(count_char(text, n, ' '));
    print_str(" o=");
    print_i64(count_char(text, n, 'o'));
    print_nl();

    static uint32_t a[333], b[333], d[333];
    for (int i = 0; i < 333; i++) {
        a[i] = (uint32_t)i * 2654435761u;
        b[i] = (uint32_t)(i ^ 0x5555);
    }
    add_arrays(d, a, b, 333);
    uint64_t h = 0;
    for (int i = 0; i < 333; i++) h = h * 131 + d[i];
    print_str("arr=");
    print_hex(h);
    print_nl();

    static uint64_t q[257];
    for (int i = 0; i < 257; i++) q[i] = (uint64_t)i * 0x9E3779B97F4A7C15ULL;
    print_str("sum=");
    print_hex(sum_u64(q, 257));
    print_nl();

    char word[] = "simulator";
    reverse(word, sim_strlen(word));
    print_str(word);
    print_nl();

    /* explicit string instructions */
#if defined(__x86_64__) || defined(__i386__)
    for (int i = 0; i < 1024; i++) buf_a[i] = (uint8_t)(i * 7);
    {
        void *dst = buf_b, *src = buf_a;
        size_t cnt = 1000;
        __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(cnt) : : "memory");
    }
    {
        void *dst = buf_b + 1000;
        size_t cnt = 24;
        __asm__ volatile("rep stosb" : "+D"(dst), "+c"(cnt) : "a"(0xAB) : "memory");
    }
    uint32_t c = 0;
    for (int i = 0; i < 1024; i++) c = c * 33 + buf_b[i];
    print_str("rep=");
    print_hex(c);
    print_nl();
    {
        const char *p = text;
        size_t cnt = 4096;
        __asm__ volatile("repne scasb" : "+D"(p), "+c"(cnt) : "a"('j') : "memory", "cc");
        print_str("scas=");
        print_u64((uint64_t)(p - text));
        print_nl();
    }
    {
        const void *x = buf_a, *y = buf_b;
        size_t cnt = 1024;
        __asm__ volatile("repe cmpsb" : "+S"(x), "+D"(y), "+c"(cnt) : : "memory", "cc");
        print_str("cmps=");
        print_u64((uint64_t)((const uint8_t *)x - buf_a));
        print_nl();
    }
#endif
    return 0;
}
