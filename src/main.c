/* x86sim command line: run a program, debug it in the terminal, or serve the web UI. */
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cache.h"
#include "machine.h"

int run_server(Machine *m, int port, const char *ui_dir);

#ifndef UI_DIR
#define UI_DIR "ui"
#endif

static void usage(void) {
    fprintf(stderr,
            "usage: x86sim [options] <program> [more.o ...] [-- program args]\n"
            "\n"
            "  <program> is a static ELF executable, one or more ELF .o files (linked\n"
            "  automatically; _start or main is the entry point), or a flat binary\n"
            "  (loaded as a 16-bit DOS .COM program).\n"
            "\n"
            "options:\n"
            "  -u, --ui          start the web UI debugger (http://127.0.0.1:8086)\n"
            "  -d, --debug       start the terminal debugger\n"
            "  -p, --port N      web UI port (default 8086)\n"
            "  -b, --bits N      force 16, 32 or 64-bit mode\n"
            "  -t, --trace       print every instruction as it executes\n"
            "  -s, --strace      print system calls as they happen\n"
            "  -m, --max N       stop after N instructions\n"
            "      --pc          full-system PC: the program is a boot image (flat binary or\n"
            "                    .o files linked at 0x7C00) started in real mode\n"
            "      --ram MB      PC memory size (default 256)\n"
            "      --hda FILE    PC hard disk image (boots from it when no program is given)\n"
            "      --fda FILE    PC floppy image\n"
            "      --boot a|c    PC boot drive (default: hard disk, then floppy)\n"
            "      --kernel F    PC: boot a Linux bzImage (--initrd F, --append \"cmdline\")\n"
            "      --smp N       PC with N CPUs (1-4); the others wait for INIT/STARTUP IPIs\n"
            "      --cache       simulate the caches: per CPU 48K L1D + 32K L1I, 3M shared L2,\n"
            "                    MESI coherence (statistics at exit with --cache-stats)\n"
            "\n"
            "  In PC mode the terminal is the serial port and keyboard; Ctrl-] quits.\n");
}

static void print_regs(Machine *m) {
    CPU *c = machine_cpu(m);
    static const char *names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    int n = m->bits == 64 ? 16 : 8;
    for (int i = 0; i < n; i++) {
        if (m->bits == 64) printf("  %-4s %016llx", names[i], (unsigned long long)c->r[i]);
        else printf("  %-4s %08llx", reg_name(i, 4, 1), (unsigned long long)(c->r[i] & 0xFFFFFFFF));
        if (i % 4 == 3) printf("\n");
    }
    uint64_t f = c->rflags;
    printf("  rip  %016llx  rflags %08llx [%s%s%s%s%s%s%s%s ]\n", (unsigned long long)c->rip,
           (unsigned long long)f, f & FL_CF ? " CF" : "", f & FL_PF ? " PF" : "", f & FL_AF ? " AF" : "",
           f & FL_ZF ? " ZF" : "", f & FL_SF ? " SF" : "", f & FL_DF ? " DF" : "", f & FL_OF ? " OF" : "",
           f & FL_IF ? " IF" : "");
    if (m->bits == 16)
        printf("  cs %04x  ds %04x  es %04x  ss %04x\n", c->sreg[CS], c->sreg[DS], c->sreg[ES], c->sreg[SS]);
    else if (c->sbase[FS] || c->sbase[GS])
        printf("  fs.base %016llx  gs.base %016llx\n", (unsigned long long)c->sbase[FS],
               (unsigned long long)c->sbase[GS]);
}

static void print_insn_at(Machine *m, uint64_t addr, int mark) {
    Insn in;
    char text[200], sym[128];
    int len = machine_disasm(m, addr, &in, text, sizeof text);
    if (machine_symname(m, addr, sym, sizeof sym) && !strchr(sym, '+')) printf("%s:\n", sym);
    char bytes[64] = "";
    for (int i = 0; i < len && i < 10; i++) snprintf(bytes + strlen(bytes), sizeof bytes - strlen(bytes), "%02x ", in.bytes[i]);
    printf("%s%c %08llx  %-30s %s\n", mark ? "=>" : "  ", machine_has_bp(m, addr) ? '*' : ' ',
           (unsigned long long)addr, bytes, text);
}

static void print_disasm(Machine *m, uint64_t addr, int count) {
    for (int i = 0; i < count; i++) {
        Insn in;
        int len = machine_disasm(m, addr, &in, NULL, 0);
        print_insn_at(m, addr, addr == machine_cpu(m)->rip);
        addr += len;
    }
}

static void hexdump(Machine *m, uint64_t addr, int len) {
    uint64_t lin = (machine_cpu(m)->sbase[DS] + addr);
    for (int off = 0; off < len; off += 16) {
        uint8_t b[16];
        size_t got = machine_peek(m, lin + off, b, 16);
        printf("  %016llx  ", (unsigned long long)(addr + off));
        for (int i = 0; i < 16; i++) {
            if ((size_t)i < got) printf("%02x ", b[i]);
            else printf("?? ");
        }
        printf(" ");
        for (int i = 0; i < 16; i++) putchar((size_t)i < got && b[i] >= 32 && b[i] < 127 ? b[i] : '.');
        printf("\n");
    }
}

static void report_stop(Machine *m, int r) {
    switch (r) {
    case STOP_EXIT: printf("[program exited with code %d]\n", m->exit_code); break;
    case STOP_FAULT: printf("[fault] %s\n", m->status); break;
    case STOP_TRAP: case STOP_BREAKPOINT: case STOP_HALT: printf("[%s]\n", m->status); break;
    case STOP_LIMIT: printf("[instruction limit reached]\n"); break;
    }
}

static void backtrace(Machine *m) {
    CPU *c = machine_cpu(m);
    int ws = m->bits == 64 ? 8 : 4;
    char sym[128];
    machine_symname(m, c->rip, sym, sizeof sym);
    printf("  #0 %016llx %s\n", (unsigned long long)c->rip, sym);
    uint64_t bp = c->r[RBP];
    for (int i = 1; i < 32 && bp; i++) {
        uint64_t ret = 0, next = 0;
        if (machine_peek(m, bp + ws, &ret, ws) != (size_t)ws || machine_peek(m, bp, &next, ws) != (size_t)ws) break;
        if (!ret) break;
        machine_symname(m, ret, sym, sizeof sym);
        printf("  #%d %016llx %s\n", i, (unsigned long long)ret, sym);
        if (next <= bp) break;
        bp = next;
    }
}

static void debugger(Machine *m) {
    char line[512], last[512] = "";
    printf("x86sim debugger. Type 'help' for commands.\n");
    print_insn_at(m, machine_cpu(m)->rip, 1);
    for (;;) {
        printf("(x86sim) ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) strcpy(line, last);
        else strcpy(last, line);
        char cmd[64] = "", arg1[256] = "", arg2[64] = "";
        sscanf(line, "%63s %255s %63s", cmd, arg1, arg2);
        if (!cmd[0]) continue;
        uint64_t addr;
        int r = -1;
        if (!strcmp(cmd, "q") || !strcmp(cmd, "quit")) break;
        else if (!strcmp(cmd, "help") || !strcmp(cmd, "h")) {
            printf("  s|step [n]        execute n instructions (default 1)\n"
                   "  n|next            step over calls\n"
                   "  finish            run until the current function returns\n"
                   "  c|continue        run until breakpoint/exit/fault\n"
                   "  b|break <loc>     set breakpoint (hex address or symbol[+off])\n"
                   "  d|delete <loc>    remove breakpoint\n"
                   "  bl                list breakpoints\n"
                   "  r|regs            show registers\n"
                   "  u|disas [loc] [n] disassemble\n"
                   "  x <loc> [len]     hex dump memory\n"
                   "  stack [n]         show stack words\n"
                   "  bt                backtrace (frame-pointer based)\n"
                   "  maps              memory map\n"
                   "  sys               recent system calls\n"
                   "  trace [n]         recently executed addresses\n"
                   "  reset             reload the program\n"
                   "  cpu [n]           list CPUs / show CPU n in regs, disas, x, stack\n"
                   "  cache [addr]      cache statistics / MESI state of addr's line\n"
                   "  cachelog [n]      last n coherence events (bus transactions)\n"
                   "  q|quit\n");
            continue;
        } else if (!strcmp(cmd, "s") || !strcmp(cmd, "step") || !strcmp(cmd, "si")) {
            long n = arg1[0] ? strtol(arg1, NULL, 0) : 1;
            for (long i = 0; i < n; i++) {
                r = machine_step(m);
                if (r != STOP_STEP) break;
            }
        } else if (!strcmp(cmd, "n") || !strcmp(cmd, "next")) {
            r = machine_step_over(m, UINT64_MAX);
        } else if (!strcmp(cmd, "finish")) {
            r = machine_step_out(m, UINT64_MAX);
        } else if (!strcmp(cmd, "c") || !strcmp(cmd, "continue")) {
            r = machine_run(m, UINT64_MAX);
        } else if (!strcmp(cmd, "b") || !strcmp(cmd, "break")) {
            if (!machine_parse_addr(m, arg1, &addr)) printf("bad location '%s'\n", arg1);
            else if (machine_add_bp(m, addr)) printf("too many breakpoints\n");
            else printf("breakpoint at 0x%llx\n", (unsigned long long)addr);
            continue;
        } else if (!strcmp(cmd, "d") || !strcmp(cmd, "delete")) {
            if (!machine_parse_addr(m, arg1, &addr) || machine_del_bp(m, addr)) printf("no such breakpoint\n");
            continue;
        } else if (!strcmp(cmd, "bl")) {
            for (int i = 0; i < m->nbp; i++) {
                char sym[128];
                machine_symname(m, m->bp[i], sym, sizeof sym);
                printf("  0x%llx %s\n", (unsigned long long)m->bp[i], sym);
            }
            continue;
        } else if (!strcmp(cmd, "r") || !strcmp(cmd, "regs")) {
            print_regs(m);
            continue;
        } else if (!strcmp(cmd, "u") || !strcmp(cmd, "disas")) {
            addr = machine_cpu(m)->rip;
            if (arg1[0] && !machine_parse_addr(m, arg1, &addr)) { printf("bad location\n"); continue; }
            print_disasm(m, addr, arg2[0] ? atoi(arg2) : 12);
            continue;
        } else if (!strcmp(cmd, "x")) {
            if (!machine_parse_addr(m, arg1, &addr)) { printf("bad location\n"); continue; }
            hexdump(m, addr, arg2[0] ? (int)strtol(arg2, NULL, 0) : 64);
            continue;
        } else if (!strcmp(cmd, "stack")) {
            int n = arg1[0] ? atoi(arg1) : 8, ws = m->bits == 64 ? 8 : m->bits == 32 ? 4 : 2;
            uint64_t sp = machine_cpu(m)->sbase[SS] + (machine_cpu(m)->r[RSP] & (m->bits == 64 ? ~0ULL : 0xFFFFFFFF));
            for (int i = 0; i < n; i++) {
                uint64_t v = 0;
                char sym[128] = "";
                machine_peek(m, sp + i * ws, &v, ws);
                machine_symname(m, v, sym, sizeof sym);
                printf("  %016llx: %016llx %s\n", (unsigned long long)(sp + i * ws), (unsigned long long)v, sym);
            }
            continue;
        } else if (!strcmp(cmd, "bt")) {
            backtrace(m);
            continue;
        } else if (!strcmp(cmd, "maps")) {
            MemRegion *regs;
            int nr = mem_regions(m->mem, &regs);
            for (int i = 0; i < nr; i++)
                printf("  %016llx-%016llx %c%c%c %s\n", (unsigned long long)regs[i].start,
                       (unsigned long long)regs[i].end, regs[i].prot & MEM_R ? 'r' : '-',
                       regs[i].prot & MEM_W ? 'w' : '-', regs[i].prot & MEM_X ? 'x' : '-', regs[i].name);
            free(regs);
            continue;
        } else if (!strcmp(cmd, "sys")) {
            uint64_t start = m->syslog_count > 20 ? m->syslog_count - 20 : 0;
            for (uint64_t i = start; i < m->syslog_count; i++) printf("  %s\n", m->syslog[i % SYSLOG_LEN]);
            continue;
        } else if (!strcmp(cmd, "trace")) {
            uint64_t n = arg1[0] ? strtoull(arg1, NULL, 0) : 16;
            if (n > TRACE_LEN) n = TRACE_LEN;
            if (n > m->trace_count) n = m->trace_count;
            for (uint64_t i = m->trace_count - n; i < m->trace_count; i++) print_insn_at(m, m->trace[i % TRACE_LEN], 0);
            continue;
        } else if (!strcmp(cmd, "cpu")) {
            if (arg1[0]) {
                int n = atoi(arg1);
                if (n < 0 || n >= m->ncpu) printf("no CPU %d (have %d)\n", n, m->ncpu);
                else m->view_cpu = n, m->bits = m->cpus[n]->bits;
            }
            for (int i = 0; i < m->ncpu; i++) {
                CPU *c = m->cpus[i];
                printf("%c cpu%d  %s  rip %016llx  %llu instructions\n", i == m->view_cpu ? '*' : ' ', i,
                       c->wait_sipi ? "waiting for SIPI" : c->halted ? "halted" : "running",
                       (unsigned long long)c->rip, (unsigned long long)c->icount);
            }
            continue;
        } else if (!strcmp(cmd, "cache") || !strcmp(cmd, "cachelog")) {
            if (!m->cache) { printf("caches are not simulated (start with --cache)\n"); continue; }
            if (!strcmp(cmd, "cachelog")) cache_print_log(m->cache, arg1[0] ? atoi(arg1) : 20, stdout);
            else if (arg1[0]) cache_print_line(m->cache, strtoull(arg1, NULL, 16), stdout);
            else cache_print_stats(m->cache, stdout);
            continue;
        } else if (!strcmp(cmd, "reset")) {
            char err[256];
            if (machine_reset(m, err, sizeof err)) printf("reset failed: %s\n", err);
            else print_insn_at(m, machine_cpu(m)->rip, 1);
            continue;
        } else {
            printf("unknown command '%s' (try 'help')\n", cmd);
            continue;
        }
        report_stop(m, r);
        if (m->state == MS_READY || m->state == MS_WAIT_INPUT) print_insn_at(m, machine_cpu(m)->rip, 1);
    }
}

static char *find_ui_dir(const char *argv0) {
    static char path[PATH_MAX];
    char exe[PATH_MAX];
    if (realpath(argv0, exe)) {
        snprintf(path, sizeof path, "%s/ui", dirname(exe));
        if (access(path, R_OK) == 0) return path;
    }
    return UI_DIR;
}

int main(int argc, char **argv) {
    int ui = 0, dbg = 0, port = 8086, bits = 0, trace = 0, strace = 0, pc = 0, smp = 1, cache = 0, cstats = 0;
    uint64_t ram_mb = 0;
    char *hda = NULL, *fda = NULL, *kernel = NULL, *initrd = NULL, *append = NULL;
    int boot = 0;
    uint64_t max = UINT64_MAX;
    char *files[64];
    int nfiles = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "-u") || !strcmp(a, "--ui")) ui = 1;
        else if (!strcmp(a, "-d") || !strcmp(a, "--debug")) dbg = 1;
        else if (!strcmp(a, "-t") || !strcmp(a, "--trace")) trace = 1;
        else if (!strcmp(a, "-s") || !strcmp(a, "--strace")) strace = 1;
        else if ((!strcmp(a, "-p") || !strcmp(a, "--port")) && i + 1 < argc) port = atoi(argv[++i]);
        else if ((!strcmp(a, "-b") || !strcmp(a, "--bits")) && i + 1 < argc) bits = atoi(argv[++i]);
        else if ((!strcmp(a, "-m") || !strcmp(a, "--max")) && i + 1 < argc) max = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--pc")) pc = 1;
        else if (!strcmp(a, "--ram") && i + 1 < argc) ram_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--hda") && i + 1 < argc) hda = argv[++i], pc = 1;
        else if (!strcmp(a, "--fda") && i + 1 < argc) fda = argv[++i], pc = 1;
        else if (!strcmp(a, "--kernel") && i + 1 < argc) kernel = argv[++i], pc = 1;
        else if (!strcmp(a, "--initrd") && i + 1 < argc) initrd = argv[++i];
        else if (!strcmp(a, "--append") && i + 1 < argc) append = argv[++i];
        else if (!strcmp(a, "--boot") && i + 1 < argc) boot = argv[++i][0];
        else if (!strcmp(a, "--smp") && i + 1 < argc) smp = atoi(argv[++i]), pc = 1;
        else if (!strcmp(a, "--cache")) cache = 1, pc = 1;
        else if (!strcmp(a, "--cache-stats")) cache = cstats = 1, pc = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
        else if (nfiles < 64) files[nfiles++] = argv[i];
    }
    if (!nfiles && !hda && !fda && !kernel) {
        usage();
        return 2;
    }
    if (smp < 1 || smp > MAX_CPUS) {
        fprintf(stderr, "--smp must be 1 to %d\n", MAX_CPUS);
        return 2;
    }
    if (bits && bits != 16 && bits != 32 && bits != 64) {
        fprintf(stderr, "--bits must be 16, 32 or 64\n");
        return 2;
    }

    /* Guest argv: program name followed by everything after "--". */
    int gargc = 1 + (argc - i);
    char **gargv = calloc(gargc + 1, sizeof *gargv);
    gargv[0] = nfiles ? files[0] : "pc";
    for (int k = 1; k < gargc; k++) gargv[k] = argv[i + k - 1];

    static Machine m;
    machine_init(&m);
    m.print_syscalls = strace;
    m.pc_mode = pc;
    m.pc_ram_mb = ram_mb;
    m.pc_hda = hda;
    m.pc_fda = fda;
    m.pc_kernel = kernel;
    m.pc_initrd = initrd;
    m.pc_append = append;
    m.pc_boot = boot;
    m.pc_smp = smp;
    m.pc_cache = cache;
    m.pc_console = pc && !ui && !dbg;
    m.trace_print = trace;
    if (ui) {
        m.echo_output = 0;
        m.use_host_stdin = 0;
        m.yield_on_idle = 1;
    }
    char err[512];
    if (machine_load(&m, nfiles, files, gargc, gargv, bits, err, sizeof err)) {
        fprintf(stderr, "x86sim: %s\n", err);
        return 1;
    }

    if (ui) return run_server(&m, port, find_ui_dir(argv[0]));
    if (dbg) {
        debugger(&m);
        return 0;
    }

    int r = machine_run(&m, max);
    if (getenv("X86SIM_STATS"))
        for (int k = 0; k < m.ncpu; k++)
            fprintf(stderr, "\nx86sim: cpu%d: %llu instructions", k, (unsigned long long)m.cpus[k]->icount);
    if (cstats && m.cache) {
        fflush(stdout);
        fprintf(stderr, "\n");
        cache_print_stats(m.cache, stderr);
    }
    if (r == STOP_EXIT) return m.exit_code;
    fflush(stdout);
    fprintf(stderr, "\nx86sim: %s\n", r == STOP_LIMIT ? "instruction limit reached" : m.status);
    if (r == STOP_FAULT || r == STOP_TRAP) {
        print_regs(&m);
        print_insn_at(&m, machine_cpu(&m)->rip, 1);
    }
    return r == STOP_FAULT ? 139 : r == STOP_TRAP ? 133 : 1;
}
