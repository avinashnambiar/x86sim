/* Tiny runtime for test programs. The same source builds for the simulator
 * (freestanding Linux .o, raw syscalls) and natively for macOS (libc), so
 * outputs can be compared byte for byte. */
#ifndef SIM_H
#define SIM_H

#include <stddef.h>
#include <stdint.h>

long sim_write(int fd, const void *buf, size_t n);
long sim_read(int fd, void *buf, size_t n);

void print_str(const char *s);
void print_char(char c);
void print_u64(uint64_t v);
void print_i64(int64_t v);
void print_hex(uint64_t v);
void print_nl(void);

size_t sim_strlen(const char *s);

#endif
