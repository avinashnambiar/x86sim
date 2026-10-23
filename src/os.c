/* OS emulation: Linux syscalls (x86-64 `syscall`, i386 `int 0x80`) and a
 * minimal DOS/BIOS (int 10h/16h/1Ah/20h/21h) for 16-bit real-mode programs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "machine.h"

#define PAGE 4096ULL
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((uint64_t)(a) - 1))

enum { E_NOENT = 2, E_BADF = 9, E_NOMEM = 12, E_FAULT = 14, E_NODEV = 19, E_INVAL = 22,
       E_NOTTY = 25, E_SPIPE = 29, E_NOSYS = 38 };

#define WAIT_INPUT INT64_MIN

static Machine *machine_of(CPU *c) { return (Machine *)c->user; }

static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint64_t rand64(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void request_stop(Machine *m, int reason) {
    m->cpu.stop_request = 1;
    m->last_stop = reason;
}

static void do_exit(Machine *m, int code) {
    m->state = MS_EXITED;
    m->exit_code = code;
    snprintf(m->status, sizeof m->status, "exited with code %d", code);
    request_stop(m, STOP_EXIT);
}

/* Read up to n bytes of guest stdin. Returns WAIT_INPUT when the UI must supply more. */
static int64_t read_stdin(Machine *m, uint8_t *dst, size_t n) {
    if (m->use_host_stdin) {
        ssize_t r = read(0, dst, n);
        return r < 0 ? 0 : r;
    }
    size_t avail = m->in.len - m->in_pos;
    if (!avail) return WAIT_INPUT;
    if (n > avail) n = avail;
    memcpy(dst, m->in.data + m->in_pos, n);
    m->in_pos += n;
    return (int64_t)n;
}

static void wait_for_input(Machine *m) {
    m->cpu.next_rip = m->cpu.rip; /* re-execute once input arrives */
    request_stop(m, STOP_INPUT);
}

/* ---------------- Linux ---------------- */

static int64_t sys_write_buf(Machine *m, int fd, uint64_t buf, uint64_t n) {
    if (fd != 1 && fd != 2) return -E_BADF;
    uint8_t tmp[4096];
    uint64_t done = 0;
    while (done < n) {
        size_t c = n - done > sizeof tmp ? sizeof tmp : (size_t)(n - done);
        if (mem_read(m->mem, buf + done, tmp, c)) return done ? (int64_t)done : -E_FAULT;
        machine_output(m, tmp, c);
        done += c;
    }
    return (int64_t)done;
}

static int64_t sys_read_buf(Machine *m, int fd, uint64_t buf, uint64_t n) {
    if (fd != 0) return -E_BADF;
    if (!n) return 0;
    uint8_t tmp[4096];
    int64_t r = read_stdin(m, tmp, n > sizeof tmp ? sizeof tmp : (size_t)n);
    if (r <= 0) return r;
    if (mem_write(m->mem, buf, tmp, (size_t)r)) return -E_FAULT;
    return r;
}

static uint64_t rdw(Machine *m, uint64_t addr, int ws) {
    uint64_t v = 0;
    mem_read(m->mem, addr, &v, ws);
    return v;
}

static int64_t sys_mmap(Machine *m, uint64_t addr, uint64_t len, int prot, int flags, int64_t fd) {
    if (!(flags & 0x20)) return -E_NODEV; /* only MAP_ANONYMOUS */
    (void)fd;
    if (!len) return -E_INVAL;
    len = ALIGN_UP(len, PAGE);
    if (flags & 0x10) { /* MAP_FIXED */
        if (addr & (PAGE - 1)) return -E_INVAL;
    } else {
        if (m->mmap_next < len + 0x10000) return -E_NOMEM;
        m->mmap_next -= len;
        addr = m->mmap_next;
    }
    mem_map(m->mem, addr, len, prot & 7, "mmap");
    return (int64_t)addr;
}

static int64_t sys_brk(Machine *m, uint64_t addr) {
    if (addr <= m->brk_start) return (int64_t)m->brk_cur;
    uint64_t mapped = ALIGN_UP(m->brk_cur, PAGE), want = ALIGN_UP(addr, PAGE);
    if (want > mapped) mem_map(m->mem, mapped, want - mapped, MEM_R | MEM_W, "heap");
    m->brk_cur = addr;
    return (int64_t)addr;
}

static int64_t put_guest(Machine *m, uint64_t addr, const void *data, size_t n) {
    return mem_write(m->mem, addr, data, n) ? -E_FAULT : 0;
}

static const char *linux_name(int nr) {
    switch (nr) {
    case 0: return "read"; case 1: return "write"; case 2: return "open"; case 3: return "close";
    case 5: return "fstat"; case 8: return "lseek"; case 9: return "mmap"; case 10: return "mprotect";
    case 11: return "munmap"; case 12: return "brk"; case 13: return "rt_sigaction";
    case 14: return "rt_sigprocmask"; case 16: return "ioctl"; case 19: return "readv"; case 20: return "writev";
    case 21: return "access"; case 24: return "sched_yield"; case 28: return "madvise"; case 35: return "nanosleep";
    case 39: return "getpid"; case 60: return "exit"; case 63: return "uname"; case 89: return "readlink";
    case 96: return "gettimeofday"; case 102: return "getuid"; case 104: return "getgid"; case 107: return "geteuid";
    case 108: return "getegid"; case 110: return "getppid"; case 131: return "sigaltstack";
    case 158: return "arch_prctl"; case 186: return "gettid"; case 201: return "time"; case 202: return "futex";
    case 218: return "set_tid_address"; case 228: return "clock_gettime"; case 231: return "exit_group";
    case 257: return "openat"; case 262: return "newfstatat"; case 273: return "set_robust_list";
    case 302: return "prlimit64"; case 318: return "getrandom"; case 334: return "rseq";
    case 1000: return "set_thread_area"; case 1001: return "mmap2"; case 1002: return "fstat64";
    }
    return NULL;
}

/* nr uses x86-64 numbering; i386 calls are translated first (custom 1000+ for i386-only calls). */
static int64_t linux_dispatch(Machine *m, int nr, const uint64_t *a, int ws) {
    CPU *c = &m->cpu;
    switch (nr) {
    case 0: return sys_read_buf(m, (int)a[0], a[1], a[2]);
    case 1: return sys_write_buf(m, (int)a[0], a[1], a[2]);
    case 19: case 20: { /* readv / writev */
        int64_t total = 0;
        for (uint64_t i = 0; i < a[2]; i++) {
            uint64_t base = rdw(m, a[1] + i * 2 * ws, ws), len = rdw(m, a[1] + i * 2 * ws + ws, ws);
            if (!len) continue;
            int64_t r = nr == 20 ? sys_write_buf(m, (int)a[0], base, len) : sys_read_buf(m, (int)a[0], base, len);
            if (r == WAIT_INPUT) return total ? total : WAIT_INPUT;
            if (r < 0) return total ? total : r;
            total += r;
            if (nr == 19 && (uint64_t)r < len) break;
        }
        return total;
    }
    case 3: return a[0] <= 2 ? 0 : -E_BADF;
    case 5: case 1002: case 262: { /* fstat / fstat64 / newfstatat */
        int fd = (int)a[0];
        uint64_t buf = nr == 262 ? a[2] : a[1];
        if (nr == 262) {
            char ch = 1;
            mem_read(m->mem, a[1], &ch, 1);
            if (ch != 0 || !(a[3] & 0x1000)) return -E_NOENT;
        }
        if (fd < 0 || fd > 2) return -E_BADF;
        uint8_t st[144] = {0};
        uint32_t mode = 0020620; /* S_IFCHR | rw--w---- */
        memcpy(st + (ws == 8 ? 24 : 16), &mode, 4);
        return put_guest(m, buf, st, ws == 8 ? 144 : 96);
    }
    case 8: return -E_SPIPE;
    case 9: case 1001: {
        uint64_t off = nr == 1001 ? a[5] * PAGE : a[5];
        (void)off;
        return sys_mmap(m, a[0], a[1], (int)a[2], (int)a[3], (int64_t)a[4]);
    }
    case 10: mem_protect(m->mem, a[0], ALIGN_UP(a[1], PAGE), (int)a[2] & 7); return 0;
    case 11: mem_unmap(m->mem, a[0], ALIGN_UP(a[1], PAGE)); return 0;
    case 12: return sys_brk(m, a[0]);
    case 16: /* ioctl */
        if (a[0] > 2) return -E_BADF;
        if (a[1] == 0x5413 && a[0] > 0) { /* TIOCGWINSZ: look like a terminal => line-buffered stdout */
            uint16_t ws4[4] = {24, 80, 0, 0};
            return put_guest(m, a[2], ws4, sizeof ws4);
        }
        return -E_NOTTY;
    case 13: case 14: case 24: case 28: case 35: case 131: case 202: case 273:
        return 0;
    case 2: case 21: case 89: case 257: return -E_NOENT;
    case 39: case 186: return 1000;
    case 110: return 1;
    case 102: case 104: case 107: case 108: return 1000;
    case 60: case 231:
        do_exit(m, (int)(a[0] & 0xFF));
        return 0;
    case 63: {
        char u[6][65] = {"Linux", "x86sim", "6.1.0-x86sim", "#1 SMP", "", ""};
        snprintf(u[4], 65, "%s", ws == 8 ? "x86_64" : "i686");
        return put_guest(m, a[0], u, sizeof u);
    }
    case 158: /* arch_prctl */
        switch (a[0]) {
        case 0x1001: c->sbase[GS] = a[1]; return 0;
        case 0x1002: c->sbase[FS] = a[1]; return 0;
        case 0x1003: return put_guest(m, a[1], &c->sbase[FS], 8);
        case 0x1004: return put_guest(m, a[1], &c->sbase[GS], 8);
        }
        return -E_INVAL;
    case 1000: { /* i386 set_thread_area(struct user_desc *) */
        uint32_t desc[4];
        if (mem_read(m->mem, a[0], desc, sizeof desc)) return -E_FAULT;
        if (desc[0] == 0xFFFFFFFF) desc[0] = 6;
        c->sbase[GS] = desc[1];
        return put_guest(m, a[0], desc, 4);
    }
    case 218: return 1000;
    case 96: case 228: case 201: {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        if (nr == 201) {
            if (a[0]) put_guest(m, a[0], &ts.tv_sec, ws);
            return ts.tv_sec;
        }
        uint64_t v[2] = {(uint64_t)ts.tv_sec, (uint64_t)(nr == 96 ? ts.tv_nsec / 1000 : ts.tv_nsec)};
        uint64_t ptr = nr == 96 ? a[0] : a[1];
        if (!ptr) return 0;
        if (ws == 8) return put_guest(m, ptr, v, 16);
        uint32_t v32[2] = {(uint32_t)v[0], (uint32_t)v[1]};
        return put_guest(m, ptr, v32, 8);
    }
    case 318: {
        uint8_t tmp[256];
        uint64_t n = a[1] > sizeof tmp ? sizeof tmp : a[1];
        for (uint64_t i = 0; i < n; i++) tmp[i] = (uint8_t)rand64();
        if (put_guest(m, a[0], tmp, (size_t)n)) return -E_FAULT;
        return (int64_t)n;
    }
    case 302: case 334: return -E_NOSYS;
    }
    machine_log(m, "unimplemented syscall %d", nr);
    return -E_NOSYS;
}

static void linux_call(Machine *m, int nr, uint64_t *args, int ws, const char *abi) {
    CPU *c = &m->cpu;
    int64_t r = linux_dispatch(m, nr, args, ws);
    if (r == WAIT_INPUT) {
        wait_for_input(m);
        return;
    }
    if (ws == 8) c->r[RAX] = (uint64_t)r;
    else c->r[RAX] = (uint32_t)r;
    const char *name = linux_name(nr);
    if (m->state == MS_EXITED)
        machine_log(m, "%s%s(%lld)", abi, name ? name : "?", (long long)args[0]);
    else if (name)
        machine_log(m, "%s%s(0x%llx, 0x%llx, 0x%llx) = %lld", abi, name, (unsigned long long)args[0],
                    (unsigned long long)args[1], (unsigned long long)args[2], (long long)r);
}

static int on_syscall(CPU *c) {
    Machine *m = machine_of(c);
    if (m->os != OS_LINUX) return 0;
    uint64_t args[6] = {c->r[RDI], c->r[RSI], c->r[RDX], c->r[R10], c->r[R8], c->r[R9]};
    linux_call(m, (int)c->r[RAX], args, 8, "");
    return 1;
}

static int i386_to_generic(int nr) {
    switch (nr) {
    case 1: return 60; case 3: return 0; case 4: return 1; case 5: return 2; case 6: return 3;
    case 13: return 201; case 20: return 39; case 33: return 21; case 45: return 12; case 54: return 16;
    case 78: return 96; case 91: return 11; case 122: return 63; case 125: return 10; case 145: return 19;
    case 146: return 20; case 174: return 13; case 175: return 14; case 192: return 1001; case 197: return 1002;
    case 199: return 102; case 200: return 104; case 201: return 107; case 202: return 108;
    case 224: return 186; case 240: return 202; case 243: return 1000; case 252: return 231;
    case 258: return 218; case 265: return 228; case 295: return 257; case 355: return 318;
    }
    return 100000 + nr;
}

/* ---------------- DOS / BIOS ---------------- */

static uint64_t real_addr(CPU *c, int seg, uint64_t off) { return c->sbase[seg] + (off & 0xFFFF); }

static void set_al(CPU *c, uint8_t v) { c->r[RAX] = (c->r[RAX] & ~0xFFULL) | v; }
static uint8_t ah(CPU *c) { return (c->r[RAX] >> 8) & 0xFF; }
static void set_cf(CPU *c, int on) { c->rflags = on ? (c->rflags | FL_CF) : (c->rflags & ~FL_CF); }
static void set_zf(CPU *c, int on) { c->rflags = on ? (c->rflags | FL_ZF) : (c->rflags & ~FL_ZF); }

static void out_char(Machine *m, uint8_t ch) { machine_output(m, &ch, 1); }

/* Returns the next input byte, or -1 when blocked waiting for input. */
static int get_key(Machine *m) {
    uint8_t ch;
    int64_t r = read_stdin(m, &ch, 1);
    if (r == WAIT_INPUT) {
        wait_for_input(m);
        return -1;
    }
    if (r <= 0) return 0x1A; /* EOF reads as Ctrl-Z */
    return ch == '\n' ? '\r' : ch;
}

static int dos_int21(Machine *m) {
    CPU *c = &m->cpu;
    uint8_t fn = ah(c);
    uint8_t dl = c->r[RDX] & 0xFF;
    switch (fn) {
    case 0x01: case 0x07: case 0x08: {
        int k = get_key(m);
        if (k < 0) return 1;
        set_al(c, (uint8_t)k);
        if (fn == 0x01) out_char(m, k == '\r' ? '\n' : (uint8_t)k);
        return 1;
    }
    case 0x02: out_char(m, dl); set_al(c, dl); return 1;
    case 0x06:
        if (dl != 0xFF) { out_char(m, dl); return 1; }
        if (!m->use_host_stdin && m->in_pos >= m->in.len) { set_zf(c, 1); set_al(c, 0); return 1; }
        { int k = get_key(m); if (k < 0) return 1; set_al(c, (uint8_t)k); set_zf(c, 0); }
        return 1;
    case 0x09: {
        uint64_t p = real_addr(c, DS, c->r[RDX]);
        for (int i = 0; i < 0x10000; i++) {
            uint8_t ch;
            mem_peek(m->mem, p + i, &ch, 1);
            if (ch == '$') break;
            out_char(m, ch);
        }
        set_al(c, '$');
        return 1;
    }
    case 0x0A: { /* buffered line input */
        uint64_t p = real_addr(c, DS, c->r[RDX]);
        uint8_t max;
        mem_peek(m->mem, p, &max, 1);
        uint8_t line[256];
        int n = 0;
        if (m->use_host_stdin) {
            int ch;
            while ((ch = getchar()) != EOF && ch != '\n')
                if (n < max - 1) line[n++] = (uint8_t)ch;
        } else {
            char *nl = memchr(m->in.data + m->in_pos, '\n', m->in.len - m->in_pos);
            if (!nl) { wait_for_input(m); return 1; }
            while (m->in.data + m->in_pos < nl) {
                uint8_t ch = (uint8_t)m->in.data[m->in_pos++];
                if (n < max - 1) line[n++] = ch;
            }
            m->in_pos++;
        }
        line[n] = '\r';
        uint8_t cnt = (uint8_t)n;
        mem_poke(m->mem, p + 1, &cnt, 1);
        mem_poke(m->mem, p + 2, line, n + 1);
        machine_output(m, line, n);
        out_char(m, '\n');
        return 1;
    }
    case 0x25: { /* set interrupt vector AL to DS:DX */
        uint32_t v = (uint32_t)((c->sreg[DS] << 16) | (c->r[RDX] & 0xFFFF));
        mem_poke(m->mem, (c->r[RAX] & 0xFF) * 4, &v, 4);
        return 1;
    }
    case 0x35: { /* get interrupt vector AL into ES:BX */
        uint32_t v = 0;
        mem_peek(m->mem, (c->r[RAX] & 0xFF) * 4, &v, 4);
        cpu_load_seg_real(c, ES, (uint16_t)(v >> 16));
        c->r[RBX] = (c->r[RBX] & ~0xFFFFULL) | (v & 0xFFFF);
        return 1;
    }
    case 0x2A: case 0x2C: {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        if (fn == 0x2A) {
            c->r[RCX] = (c->r[RCX] & ~0xFFFFULL) | (uint64_t)(tm.tm_year + 1900);
            c->r[RDX] = (c->r[RDX] & ~0xFFFFULL) | (uint64_t)(((tm.tm_mon + 1) << 8) | tm.tm_mday);
            set_al(c, (uint8_t)tm.tm_wday);
        } else {
            c->r[RCX] = (c->r[RCX] & ~0xFFFFULL) | (uint64_t)((tm.tm_hour << 8) | tm.tm_min);
            c->r[RDX] = (c->r[RDX] & ~0xFFFFULL) | (uint64_t)(tm.tm_sec << 8);
        }
        return 1;
    }
    case 0x30: c->r[RAX] = (c->r[RAX] & ~0xFFFFULL) | 0x0005; return 1;
    case 0x4C: do_exit(m, c->r[RAX] & 0xFF); return 1;
    }
    machine_log(m, "unsupported DOS function int 21h AH=%02Xh", fn);
    set_cf(c, 1);
    return 1;
}

static int bios_int10(Machine *m) {
    CPU *c = &m->cpu;
    uint8_t al = c->r[RAX] & 0xFF;
    switch (ah(c)) {
    case 0x0E: out_char(m, al); return 1;
    case 0x09: case 0x0A:
        for (uint64_t i = 0; i < (c->r[RCX] & 0xFFFF); i++) out_char(m, al);
        return 1;
    case 0x13: {
        uint64_t p = real_addr(c, ES, c->r[RBP]);
        for (uint64_t i = 0; i < (c->r[RCX] & 0xFFFF); i++) {
            uint8_t ch;
            mem_peek(m->mem, p + i * ((al & 2) ? 2 : 1), &ch, 1);
            out_char(m, ch);
        }
        return 1;
    }
    case 0x03: c->r[RDX] &= ~0xFFFFULL; c->r[RCX] = (c->r[RCX] & ~0xFFFFULL) | 0x0607; return 1;
    case 0x0F: c->r[RAX] = (c->r[RAX] & ~0xFFFFULL) | 0x5003; c->r[RBX] &= ~0xFF00ULL; return 1;
    }
    return 1; /* mode set, cursor moves, etc.: accepted and ignored */
}

static int bios_int16(Machine *m) {
    CPU *c = &m->cpu;
    switch (ah(c)) {
    case 0x00: case 0x10: {
        int k = get_key(m);
        if (k < 0) return 1;
        c->r[RAX] = (c->r[RAX] & ~0xFFFFULL) | (uint64_t)k;
        return 1;
    }
    case 0x01: case 0x11:
        if (!m->use_host_stdin && m->in_pos >= m->in.len) { set_zf(c, 1); return 1; }
        if (m->use_host_stdin) { set_zf(c, 1); return 1; }
        c->r[RAX] = (c->r[RAX] & ~0xFFFFULL) | (uint8_t)m->in.data[m->in_pos];
        set_zf(c, 0);
        return 1;
    }
    return 1;
}

static int on_interrupt(CPU *c, int vec) {
    Machine *m = machine_of(c);
    if (m->os == OS_LINUX && c->bits == 32 && vec == 0x80) {
        uint64_t args[6] = {c->r[RBX] & 0xFFFFFFFF, c->r[RCX] & 0xFFFFFFFF, c->r[RDX] & 0xFFFFFFFF,
                            c->r[RSI] & 0xFFFFFFFF, c->r[RDI] & 0xFFFFFFFF, c->r[RBP] & 0xFFFFFFFF};
        linux_call(m, i386_to_generic((int)(c->r[RAX] & 0xFFFFFFFF)), args, 4, "i386:");
        return 1;
    }
    if (m->os != OS_DOS) return 0;
    uint32_t ivt = 0;
    mem_peek(m->mem, (uint64_t)vec * 4, &ivt, 4);
    if (ivt) return 0; /* the program installed its own handler */
    switch (vec) {
    case 0x10: return bios_int10(m);
    case 0x16: return bios_int16(m);
    case 0x1A:
        c->r[RCX] = (c->r[RCX] & ~0xFFFFULL) | ((c->icount >> 16) & 0xFFFF);
        c->r[RDX] = (c->r[RDX] & ~0xFFFFULL) | (c->icount & 0xFFFF);
        set_al(c, 0);
        return 1;
    case 0x20: do_exit(m, 0); return 1;
    case 0x21: return dos_int21(m);
    }
    return 0;
}

void os_install(Machine *m) {
    m->cpu.user = m;
    m->cpu.on_syscall = on_syscall;
    m->cpu.on_interrupt = on_interrupt;
}
