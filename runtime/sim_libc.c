#include "sim.h"

#if defined(__APPLE__)
#include <unistd.h>
long sim_write(int fd, const void *buf, size_t n) { return write(fd, buf, n); }
long sim_read(int fd, void *buf, size_t n) { return read(fd, buf, n); }
#else

#if defined(__x86_64__)
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
#define SYS_READ 0
#define SYS_WRITE 1
#else
static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
#define SYS_READ 3
#define SYS_WRITE 4
#endif

long sim_write(int fd, const void *buf, size_t n) { return sc3(SYS_WRITE, fd, (long)buf, (long)n); }
long sim_read(int fd, void *buf, size_t n) { return sc3(SYS_READ, fd, (long)buf, (long)n); }

/* The compiler may emit calls to these even in freestanding code. */
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp < sp) while (n--) *dp++ = *sp++;
    else while (n--) dp[n] = sp[n];
    return d;
}

void *memset(void *d, int c, size_t n) {
    unsigned char *dp = d;
    while (n--) *dp++ = (unsigned char)c;
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

#if defined(__i386__)
/* 64-bit division helpers normally provided by libgcc/compiler-rt. */
static uint64_t udivmod64(uint64_t n, uint64_t d, uint64_t *rem) {
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1ULL << i;
        }
    }
    if (rem) *rem = r;
    return q;
}
uint64_t __udivdi3(uint64_t n, uint64_t d) { return udivmod64(n, d, 0); }
uint64_t __umoddi3(uint64_t n, uint64_t d) { uint64_t r; udivmod64(n, d, &r); return r; }
int64_t __divdi3(int64_t n, int64_t d) {
    int neg = (n < 0) ^ (d < 0);
    uint64_t q = udivmod64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, 0);
    return neg ? -(int64_t)q : (int64_t)q;
}
int64_t __moddi3(int64_t n, int64_t d) {
    uint64_t r;
    udivmod64(n < 0 ? -(uint64_t)n : (uint64_t)n, d < 0 ? -(uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}
#endif
#endif

size_t sim_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void print_str(const char *s) { sim_write(1, s, sim_strlen(s)); }
void print_char(char c) { sim_write(1, &c, 1); }
void print_nl(void) { print_char('\n'); }

void print_u64(uint64_t v) {
    char buf[24];
    int i = 23;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    print_str(buf + i);
}

void print_i64(int64_t v) {
    if (v < 0) {
        print_char('-');
        print_u64(-(uint64_t)v);
    } else {
        print_u64((uint64_t)v);
    }
}

void print_hex(uint64_t v) {
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    int n = 0;
    uint64_t t = v;
    do { n++; t >>= 4; } while (t);
    for (int i = 0; i < n; i++) buf[2 + i] = "0123456789abcdef"[(v >> (4 * (n - 1 - i))) & 15];
    buf[2 + n] = 0;
    print_str(buf);
}
