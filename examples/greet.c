/* Interactive example: reads lines from stdin and answers. */
#include "../runtime/sim.h"

static int read_line(char *buf, int max) {
    int n = 0;
    while (n < max - 1) {
        char c;
        if (sim_read(0, &c, 1) <= 0) break;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

int main(void) {
    char name[64];
    print_str("What is your name? ");
    if (!read_line(name, sizeof name)) return 1;
    print_str("Hello, ");
    print_str(name);
    print_str("! Give me a number: ");
    char num[32];
    read_line(num, sizeof num);
    uint64_t n = 0;
    for (char *p = num; *p >= '0' && *p <= '9'; p++) n = n * 10 + (uint64_t)(*p - '0');
    uint64_t f = 1;
    for (uint64_t i = 2; i <= n && i <= 20; i++) f *= i;
    print_u64(n);
    print_str("! = ");
    print_u64(f);
    print_nl();
    return 0;
}
