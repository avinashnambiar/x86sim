/* Classic algorithms: recursion, sorting, sieving, hashing, jump tables,
 * function pointers, varargs, struct copies, global/bss data. */
#include <stdarg.h>

#include "../../runtime/sim.h"

static int primes[2000];        /* .bss */
static char sieve_buf[20000];
static const char *words[] = {"delta", "alpha", "echo", "charlie", "bravo", "foxtrot"}; /* .data relocations */
static int counter = 5;         /* .data */

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static unsigned ackermann(unsigned m, unsigned n) {
    if (!m) return n + 1;
    if (!n) return ackermann(m - 1, 1);
    return ackermann(m - 1, ackermann(m, n - 1));
}

static void quicksort(int *a, int lo, int hi) {
    if (lo >= hi) return;
    int p = a[(lo + hi) / 2], i = lo, j = hi;
    while (i <= j) {
        while (a[i] < p) i++;
        while (a[j] > p) j--;
        if (i <= j) {
            int t = a[i];
            a[i] = a[j];
            a[j] = t;
            i++;
            j--;
        }
    }
    quicksort(a, lo, j);
    quicksort(a, i, hi);
}

static int strcmp_(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static const char *classify(int x) {
    switch (x % 9) { /* dense switch -> jump table */
    case 0: return "zero";
    case 1: return "one";
    case 2: return "two";
    case 3: return "three";
    case 4: return "four";
    case 5: return "five";
    case 6: return "six";
    case 7: return "seven";
    default: return "eight";
    }
}

static long sum_va(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, long) * (i + 1);
    va_end(ap);
    return s;
}

typedef struct {
    int id;
    char name[20];
    long values[6];
} Record;

static Record make_record(int id) {
    Record r;
    r.id = id;
    for (int i = 0; i < 19; i++) r.name[i] = (char)('a' + (id + i) % 26);
    r.name[19] = 0;
    for (int i = 0; i < 6; i++) r.values[i] = (long)id * (i + 3) - 7;
    return r;
}

static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
static int op_mul(int a, int b) { return a * b; }
static int op_xor(int a, int b) { return a ^ b; }
static int (*const ops[])(int, int) = {op_add, op_sub, op_mul, op_xor};

static uint64_t collatz_max(uint64_t n) {
    uint64_t steps = 0;
    while (n != 1) {
        n = (n & 1) ? 3 * n + 1 : n / 2;
        steps++;
    }
    return steps;
}

int main(int argc, char **argv) {
    print_str("argc=");
    print_i64(argc);
    print_nl();

    print_str("fib(24)=");
    print_i64(fib(24));
    print_nl();
    print_str("ack(2,3)=");
    print_u64(ackermann(2, 3));
    print_nl();

    int n = 0;
    for (int i = 2; i < (int)sizeof sieve_buf; i++) {
        if (sieve_buf[i]) continue;
        if (n < 2000) primes[n++] = i;
        for (int j = i * 2; j < (int)sizeof sieve_buf; j += i) sieve_buf[j] = 1;
    }
    print_str("primes=");
    print_i64(n);
    print_str(" last=");
    print_i64(primes[n - 1]);
    print_nl();

    int arr[500];
    uint32_t seed = 12345;
    for (int i = 0; i < 500; i++) {
        seed = seed * 1103515245u + 12345u;
        arr[i] = (int)(seed >> 8) % 10000 - 5000;
    }
    quicksort(arr, 0, 499);
    int sorted = 1;
    for (int i = 1; i < 500; i++) sorted &= arr[i - 1] <= arr[i];
    print_str("sorted=");
    print_i64(sorted);
    print_str(" min=");
    print_i64(arr[0]);
    print_str(" max=");
    print_i64(arr[499]);
    print_nl();

    /* sort strings (pointer swaps) */
    for (int i = 0; i < 6; i++)
        for (int j = i + 1; j < 6; j++)
            if (strcmp_(words[i], words[j]) > 0) {
                const char *t = words[i];
                words[i] = words[j];
                words[j] = t;
            }
    for (int i = 0; i < 6; i++) {
        print_str(words[i]);
        print_char(i == 5 ? '\n' : ',');
    }

    print_str("crc32=");
    print_hex(crc32((const uint8_t *)"The quick brown fox jumps over the lazy dog", 43));
    print_nl();

    for (int i = 0; i < 12; i++) {
        print_str(classify(i * 7 + counter));
        print_char(' ');
    }
    print_nl();

    print_str("va=");
    print_i64(sum_va(5, 10L, -20L, 30L, -40L, 50L));
    print_nl();

    Record recs[4];
    for (int i = 0; i < 4; i++) recs[i] = make_record(i * 11);
    Record copy = recs[3];
    print_str(copy.name);
    print_char(' ');
    print_i64(copy.values[5]);
    print_nl();

    int acc = 1;
    for (int i = 0; i < 40; i++) acc = ops[i % 4](acc, i + 3);
    print_str("ops=");
    print_i64(acc);
    print_nl();

    uint64_t best = 0, besti = 0;
    for (uint64_t i = 1; i < 3000; i++) {
        uint64_t s = collatz_max(i);
        if (s > best) best = s, besti = i;
    }
    print_str("collatz ");
    print_u64(besti);
    print_char(' ');
    print_u64(best);
    print_nl();

    if (argc > 1) {
        print_str("arg1=");
        print_str(argv[1]);
        print_nl();
    }
    return 3;
}
