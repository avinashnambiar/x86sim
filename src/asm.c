/* Programs written in the web UI's editor: assembled by clang (the
 * toolchain the project already uses; X86SIM_CLANG overrides the path),
 * linked by the simulator's own linker at the chosen address and started on
 * the chosen CPU. Loads are remembered so that a reset restarts them. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "machine.h"
#include "pc.h"

#define MAX_OBJ (16u << 20)

static int write_file(const char *path, const char *a, size_t an, const char *b, size_t bn) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(a, 1, an, f) == an && fwrite(b, 1, bn, f) == bn;
    return fclose(f) == 0 && ok ? 0 : -1;
}

/* Assemble src for 16/32/64-bit code into an ELF object (malloc'd). */
static int assemble(const char *src, size_t n, int bits, int intel, uint8_t **obj, size_t *objn, char *diag,
                    size_t diagn) {
    const char *tmp = getenv("TMPDIR");
    char dir[PATH_MAX], path[PATH_MAX + 32], cmd[2 * PATH_MAX + 256], prefix[96];
    int rc = -1;
    *obj = NULL;
    diag[0] = 0;
    snprintf(dir, sizeof dir, "%s", tmp && *tmp ? tmp : "/tmp");
    size_t dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dir[--dl] = 0;
    snprintf(dir + dl, sizeof dir - dl, "/x86sim-asm-XXXXXX");
    if (!mkdtemp(dir)) {
        snprintf(diag, diagn, "cannot create a temporary directory for the assembler");
        return -1;
    }
    /* the mode and syntax directives come first; the line marker keeps the
     * diagnostics' line numbers those of the editor */
    snprintf(prefix, sizeof prefix, "%s.code%d\n# 1 \"program.s\"\n", intel ? ".intel_syntax noprefix\n" : "", bits);
    snprintf(path, sizeof path, "%s/program.s", dir);
    int nl = n && src[n - 1] != '\n';
    char *body = malloc(n + 2);
    memcpy(body, src, n);
    if (nl) body[n] = '\n';
    if (write_file(path, prefix, strlen(prefix), body, n + (size_t)nl)) {
        snprintf(diag, diagn, "cannot write the source file");
        free(body);
        goto out;
    }
    free(body);
    const char *clang = getenv("X86SIM_CLANG");
    if (!clang || !*clang) clang = "clang";
    /* limits: 10 s of CPU, 16 MiB of output (.rept can make huge objects) */
    snprintf(cmd, sizeof cmd, "cd '%s' && ulimit -t 10 -f 32768 && '%s' -target %s -c -x assembler program.s -o program.o 2>&1",
             dir, clang, bits == 64 ? "x86_64-linux-gnu" : "i386-linux-gnu");
    FILE *p = popen(cmd, "r");
    if (!p) {
        snprintf(diag, diagn, "cannot run the assembler");
        goto out;
    }
    size_t k = fread(diag, 1, diagn - 1, p);
    diag[k] = 0;
    char sink[4096];
    while (fread(sink, 1, sizeof sink, p) > 0) {}
    int st = pclose(p);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
    if (code == 127) {
        snprintf(diag, diagn, "clang was not found: install the Xcode command line tools (xcode-select --install) "
                              "or set X86SIM_CLANG to an x86 assembler driver");
        goto out;
    }
    if (code) {
        if (!diag[0]) snprintf(diag, diagn, "the assembler failed (exit %d)", code);
        goto out;
    }
    snprintf(path, sizeof path, "%s/program.o", dir);
    FILE *f = fopen(path, "rb");
    struct stat sb;
    if (!f || fstat(fileno(f), &sb) || sb.st_size <= 0 || (size_t)sb.st_size > MAX_OBJ) {
        if (f) fclose(f);
        snprintf(diag, diagn, "the assembler produced no usable object");
        goto out;
    }
    *objn = (size_t)sb.st_size;
    *obj = malloc(*objn);
    if (fread(*obj, 1, *objn, f) != *objn) {
        free(*obj);
        *obj = NULL;
        fclose(f);
        snprintf(diag, diagn, "cannot read the assembled object");
        goto out;
    }
    fclose(f);
    rc = 0; /* diag keeps any warnings */
out:
    snprintf(path, sizeof path, "%s/program.s", dir);
    unlink(path);
    snprintf(path, sizeof path, "%s/program.o", dir);
    unlink(path);
    rmdir(dir);
    return rc;
}

/* Link the object at its address and start its CPU. */
static int place(Machine *m, AsmLoad *l, char *err, size_t errn) {
    uint64_t base = l->bits == 16 ? 0 : l->addr; /* real mode: offsets within the program's segment */
    uint64_t entry, size;
    if (pc_link_object(m, l->obj, l->objn, base, l->addr, &entry, &size, err, errn)) return -1;
    l->entry = l->addr - base + entry;
    l->size = size;
    if (l->cpu >= 0 && l->cpu < m->ncpu) pc_cpu_start(m, l->cpu, l->bits, l->addr, l->entry, l->stack);
    return 0;
}

int asm_load(Machine *m, const char *src, size_t n, int bits, int intel, int cpu, uint64_t addr, AsmLoad *info,
             char *diag, size_t diagn) {
    if (!m->pc) {
        snprintf(diag, diagn, "programs from the editor run on a PC machine: create one with New machine");
        return -1;
    }
    if (bits != 16 && bits != 32 && bits != 64) {
        snprintf(diag, diagn, "the mode must be 16, 32 or 64-bit");
        return -1;
    }
    if (cpu >= m->ncpu) {
        snprintf(diag, diagn, "this machine has %d CPU%s", m->ncpu, m->ncpu == 1 ? "" : "s");
        return -1;
    }
    AsmLoad l = {.cpu = cpu, .bits = bits, .addr = addr};
    if (assemble(src, n, bits, intel, &l.obj, &l.objn, diag, diagn)) return -1;
    /* the program goes at the bottom of its slot and the stack at the top */
    uint64_t slot = bits == 16 ? PC_SLOT16 : PC_SLOT, need = pc_object_size(l.obj, l.objn);
    l.stack = addr + (need + 256 > slot ? (need + 256 + 0xFFF) & ~0xFFFULL : slot);
    char err[256];
    uint64_t top = cpu >= 0 ? l.stack : addr + need;
    /* the load it replaces: the same CPU's (or, for data, the same address) */
    int i;
    for (i = 0; i < m->nasm_loads; i++)
        if (cpu >= 0 ? m->asm_loads[i].cpu == cpu : (m->asm_loads[i].cpu < 0 && m->asm_loads[i].addr == addr)) break;
    if (pc_check_load(m, bits, addr, top - addr, err, sizeof err)) goto fail;
    for (int k = 0; k < m->nasm_loads; k++) { /* code and stack must not land on another program's */
        AsmLoad *o = &m->asm_loads[k];
        uint64_t otop = o->cpu >= 0 ? o->stack : o->addr + o->size;
        if (k == i || addr >= otop || top <= o->addr) continue;
        if (o->cpu >= 0)
            snprintf(err, sizeof err,
                     "0x%llx-0x%llx (code and stack) overlaps CPU %d's program at 0x%llx-0x%llx: pick another address",
                     (unsigned long long)addr, (unsigned long long)top, o->cpu, (unsigned long long)o->addr,
                     (unsigned long long)otop);
        else
            snprintf(err, sizeof err, "0x%llx-0x%llx overlaps the data loaded at 0x%llx-0x%llx: pick another address",
                     (unsigned long long)addr, (unsigned long long)top, (unsigned long long)o->addr,
                     (unsigned long long)otop);
        goto fail;
    }
    if (place(m, &l, err, sizeof err)) goto fail;
    if (i == m->nasm_loads) m->asm_loads = realloc(m->asm_loads, (size_t)++m->nasm_loads * sizeof *m->asm_loads);
    else free(m->asm_loads[i].obj);
    m->asm_loads[i] = l;
    *info = l;
    return 0;
fail:
    snprintf(diag, diagn, "%s", err);
    free(l.obj);
    return -1;
}

int asm_check(const char *src, size_t n, int bits, int intel, char *diag, size_t diagn) {
    uint8_t *obj;
    size_t objn;
    if (bits != 16 && bits != 32 && bits != 64) {
        snprintf(diag, diagn, "the mode must be 16, 32 or 64-bit");
        return -1;
    }
    if (assemble(src, n, bits, intel, &obj, &objn, diag, diagn)) return -1;
    free(obj);
    return 0;
}

void asm_replay(Machine *m) {
    char err[256];
    for (int i = 0; i < m->nasm_loads; i++) place(m, &m->asm_loads[i], err, sizeof err);
    if (m->nasm_loads) {
        snprintf(m->status, sizeof m->status, "reset: %d editor program%s reloaded", m->nasm_loads,
                 m->nasm_loads == 1 ? "" : "s");
        m->view_cpu = 0;
    }
}

void asm_forget(Machine *m) {
    for (int i = 0; i < m->nasm_loads; i++) free(m->asm_loads[i].obj);
    free(m->asm_loads);
    m->asm_loads = NULL;
    m->nasm_loads = 0;
}
