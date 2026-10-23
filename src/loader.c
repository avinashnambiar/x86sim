/* Program loading:
 *   - static ELF executables (ET_EXEC / static-pie ET_DYN), 32- or 64-bit
 *   - ELF relocatable objects (.o), linked in-process by a small linker, so
 *     programs can be built with nothing more than `clang -c`
 *   - flat binaries (DOS .COM style, 16-bit by default)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"

#define PAGE 4096ULL
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((uint64_t)(a) - 1))

enum { ET_REL = 1, ET_EXEC = 2, ET_DYN = 3 };
enum { EM_386 = 3, EM_X86_64 = 62 };
enum { SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_RELA = 4, SHT_NOBITS = 8, SHT_REL = 9 };
enum { SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4 };
enum { PT_LOAD = 1, PT_INTERP = 3, PT_PHDR = 6 };
enum { SHN_UNDEF = 0, SHN_ABS = 0xFFF1, SHN_COMMON = 0xFFF2 };
enum { STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2 };
enum { STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4 };

typedef struct {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t align, entsize;
} Shdr;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, filesz, memsz;
} Phdr;

typedef struct {
    const char *name;
    uint8_t bind, type;
    uint16_t shndx;
    uint64_t value, size;
} Sym;

typedef struct {
    uint8_t *data;
    size_t size;
    const char *path;
    int is64, machine, type;
    uint64_t entry, phoff;
    int nsh, nph;
    Shdr *sh;
    Phdr *ph;
    int symtab;            /* section index of .symtab or -1 */
    uint64_t *secaddr;     /* linker: final address of each section */
} Elf;

static uint64_t rd(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static void wr(uint8_t *p, int n, uint64_t v) {
    for (int i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int read_file(const char *path, uint8_t **data, size_t *size, char *err, size_t errn) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errn, "cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *data = malloc(n > 0 ? (size_t)n : 1);
    *size = fread(*data, 1, (size_t)n, f);
    fclose(f);
    return 0;
}

static int is_elf(const uint8_t *d, size_t n) { return n >= 52 && !memcmp(d, "\x7f" "ELF", 4); }

static int parse_elf(Elf *e, char *err, size_t errn) {
    const uint8_t *d = e->data;
    e->is64 = d[4] == 2;
    if (d[5] != 1) {
        snprintf(err, errn, "%s: not a little-endian ELF", e->path);
        return -1;
    }
    e->type = (int)rd(d + 16, 2);
    e->machine = (int)rd(d + 18, 2);
    if (e->machine != EM_386 && e->machine != EM_X86_64) {
        snprintf(err, errn, "%s: not an x86 ELF (machine %d)", e->path, e->machine);
        return -1;
    }
    uint64_t shoff, phoff;
    int shentsize, phentsize;
    if (e->is64) {
        e->entry = rd(d + 24, 8);
        phoff = rd(d + 32, 8);
        shoff = rd(d + 40, 8);
        phentsize = (int)rd(d + 54, 2);
        e->nph = (int)rd(d + 56, 2);
        shentsize = (int)rd(d + 58, 2);
        e->nsh = (int)rd(d + 60, 2);
    } else {
        e->entry = rd(d + 24, 4);
        phoff = rd(d + 28, 4);
        shoff = rd(d + 32, 4);
        phentsize = (int)rd(d + 42, 2);
        e->nph = (int)rd(d + 44, 2);
        shentsize = (int)rd(d + 46, 2);
        e->nsh = (int)rd(d + 48, 2);
    }
    e->phoff = phoff;
    if (shoff + (uint64_t)e->nsh * shentsize > e->size || phoff + (uint64_t)e->nph * phentsize > e->size) {
        snprintf(err, errn, "%s: truncated ELF", e->path);
        return -1;
    }
    e->sh = calloc(e->nsh + 1, sizeof *e->sh);
    e->ph = calloc(e->nph + 1, sizeof *e->ph);
    e->symtab = -1;
    for (int i = 0; i < e->nsh; i++) {
        const uint8_t *s = d + shoff + (uint64_t)i * shentsize;
        Shdr *h = &e->sh[i];
        h->name = (uint32_t)rd(s, 4);
        h->type = (uint32_t)rd(s + 4, 4);
        if (e->is64) {
            h->flags = rd(s + 8, 8);
            h->addr = rd(s + 16, 8);
            h->offset = rd(s + 24, 8);
            h->size = rd(s + 32, 8);
            h->link = (uint32_t)rd(s + 40, 4);
            h->info = (uint32_t)rd(s + 44, 4);
            h->align = rd(s + 48, 8);
            h->entsize = rd(s + 56, 8);
        } else {
            h->flags = rd(s + 8, 4);
            h->addr = rd(s + 12, 4);
            h->offset = rd(s + 16, 4);
            h->size = rd(s + 20, 4);
            h->link = (uint32_t)rd(s + 24, 4);
            h->info = (uint32_t)rd(s + 28, 4);
            h->align = rd(s + 32, 4);
            h->entsize = rd(s + 36, 4);
        }
        if (h->type != SHT_NOBITS && h->offset + h->size > e->size) {
            snprintf(err, errn, "%s: section %d out of bounds", e->path, i);
            return -1;
        }
        if (h->type == SHT_SYMTAB) e->symtab = i;
    }
    for (int i = 0; i < e->nph; i++) {
        const uint8_t *p = d + phoff + (uint64_t)i * phentsize;
        Phdr *h = &e->ph[i];
        h->type = (uint32_t)rd(p, 4);
        if (e->is64) {
            h->flags = (uint32_t)rd(p + 4, 4);
            h->offset = rd(p + 8, 8);
            h->vaddr = rd(p + 16, 8);
            h->filesz = rd(p + 32, 8);
            h->memsz = rd(p + 40, 8);
        } else {
            h->offset = rd(p + 4, 4);
            h->vaddr = rd(p + 8, 4);
            h->filesz = rd(p + 16, 4);
            h->memsz = rd(p + 20, 4);
            h->flags = (uint32_t)rd(p + 24, 4);
        }
    }
    return 0;
}

static int nsyms(const Elf *e) {
    if (e->symtab < 0) return 0;
    return (int)(e->sh[e->symtab].size / (e->is64 ? 24 : 16));
}

static Sym get_sym(const Elf *e, int i) {
    const Shdr *st = &e->sh[e->symtab];
    const char *strtab = (const char *)e->data + e->sh[st->link].offset;
    const uint8_t *p = e->data + st->offset + (uint64_t)i * (e->is64 ? 24 : 16);
    Sym s;
    if (e->is64) {
        s.name = strtab + rd(p, 4);
        s.bind = p[4] >> 4;
        s.type = p[4] & 15;
        s.shndx = (uint16_t)rd(p + 6, 2);
        s.value = rd(p + 8, 8);
        s.size = rd(p + 16, 8);
    } else {
        s.name = strtab + rd(p, 4);
        s.value = rd(p + 4, 4);
        s.size = rd(p + 8, 4);
        s.bind = p[12] >> 4;
        s.type = p[12] & 15;
        s.shndx = (uint16_t)rd(p + 14, 2);
    }
    return s;
}

static void add_symbol(Machine *m, const char *name, uint64_t addr, uint64_t size) {
    if (!name || !*name || name[0] == '.' || !strncmp(name, "$x", 2)) return;
    m->syms = realloc(m->syms, (m->nsyms + 1) * sizeof *m->syms);
    m->syms[m->nsyms++] = (Symbol){addr, size, strdup(name)};
}

static int cmp_sym(const void *a, const void *b) {
    const Symbol *x = a, *y = b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    /* prefer globals-looking names (no leading underscore) first for display */
    return (x->name[0] == '_') - (y->name[0] == '_');
}

static void sort_symbols(Machine *m) { qsort(m->syms, m->nsyms, sizeof *m->syms, cmp_sym); }

/* ---- process setup ---- */

static void setup_cpu(Machine *m, int bits) {
    cpu_init(&m->cpu, m->mem, bits);
    m->bits = bits;
    m->cpu.user = m;
}

typedef struct {
    uint64_t phdr, phent, phnum, entry;
} AuxInfo;

static uint64_t push_bytes(Machine *m, uint64_t *sp, const void *data, size_t n) {
    *sp -= n;
    mem_poke(m->mem, *sp, data, n);
    return *sp;
}

static void setup_linux_stack(Machine *m, const AuxInfo *aux) {
    int ws = m->bits == 64 ? 8 : 4;
    uint64_t top = m->bits == 64 ? 0x7ffffffff000ULL : 0xc0000000ULL;
    uint64_t size = 8ULL << 20;
    mem_map(m->mem, top - size, size, MEM_R | MEM_W, "stack");
    m->mmap_next = top - size - PAGE * 16;

    uint64_t sp = top - 16;
    const char *envs[] = {"X86SIM=1", "TERM=dumb", "HOME=/", "PATH=/bin:/usr/bin"};
    int nenv = 4;
    uint64_t argp[64], envp[8];
    int argc = m->argc < 64 ? m->argc : 64;
    for (int i = argc - 1; i >= 0; i--) argp[i] = push_bytes(m, &sp, m->argv[i], strlen(m->argv[i]) + 1);
    for (int i = nenv - 1; i >= 0; i--) envp[i] = push_bytes(m, &sp, envs[i], strlen(envs[i]) + 1);
    const char *plat = m->bits == 64 ? "x86_64" : "i686";
    uint64_t platp = push_bytes(m, &sp, plat, strlen(plat) + 1);
    uint8_t rnd[16];
    for (int i = 0; i < 16; i++) rnd[i] = (uint8_t)(i * 37 + 11);
    uint64_t rndp = push_bytes(m, &sp, rnd, 16);
    sp &= ~15ULL;

    uint64_t auxv[][2] = {
        {3, aux->phdr},  {4, aux->phent}, {5, aux->phnum}, {6, PAGE}, {9, aux->entry},
        {11, 0}, {12, 0}, {13, 0}, {14, 0}, {16, 0}, {17, 100}, {23, 0},
        {25, rndp}, {15, platp}, {0, 0},
    };
    int naux = sizeof auxv / sizeof auxv[0];
    int nwords = 1 + argc + 1 + nenv + 1 + naux * 2;
    sp -= (uint64_t)nwords * ws;
    sp &= ~15ULL;
    uint64_t p = sp;
#define PUT(v) do { uint64_t _v = (v); mem_poke(m->mem, p, &_v, ws); p += ws; } while (0)
    PUT((uint64_t)argc);
    for (int i = 0; i < argc; i++) PUT(argp[i]);
    PUT(0);
    for (int i = 0; i < nenv; i++) PUT(envp[i]);
    PUT(0);
    for (int i = 0; i < naux; i++) {
        PUT(auxv[i][0]);
        PUT(auxv[i][1]);
    }
#undef PUT
    m->cpu.r[RSP] = sp;
}

/* ---- static executables ---- */

static int load_exec(Machine *m, Elf *e, char *err, size_t errn) {
    int bits = m->bits_override ? m->bits_override : (e->is64 ? 64 : 32);
    if (bits == 16) {
        snprintf(err, errn, "%s: 16-bit mode needs a .o or flat binary", e->path);
        return -1;
    }
    setup_cpu(m, bits);
    m->os = OS_LINUX;
    uint64_t bias = 0;
    if (e->type == ET_DYN) bias = bits == 64 ? 0x555555554000ULL : 0x56555000ULL;

    uint64_t maxend = 0, phdr = 0;
    for (int i = 0; i < e->nph; i++) {
        Phdr *ph = &e->ph[i];
        if (ph->type == PT_INTERP) {
            snprintf(err, errn, "%s is dynamically linked; only static executables are supported "
                                "(link with -static)", e->path);
            return -1;
        }
        if (ph->type == PT_PHDR) phdr = ph->vaddr + bias;
        if (ph->type != PT_LOAD || !ph->memsz) continue;
        if (ph->offset + ph->filesz > e->size) {
            snprintf(err, errn, "%s: segment out of bounds", e->path);
            return -1;
        }
        int prot = ((ph->flags & 4) ? MEM_R : 0) | ((ph->flags & 2) ? MEM_W : 0) | ((ph->flags & 1) ? MEM_X : 0);
        uint64_t va = ph->vaddr + bias;
        uint64_t start = va & ~(PAGE - 1), end = ALIGN_UP(va + ph->memsz, PAGE);
        /* Segments may share a page: never wipe already-mapped pages. */
        for (uint64_t a = start; a < end; a += PAGE) {
            int old = mem_prot_at(m->mem, a);
            if (old < 0) mem_map(m->mem, a, PAGE, prot, prot & MEM_X ? "text" : prot & MEM_W ? "data" : "rodata");
            else mem_protect(m->mem, a, PAGE, old | prot);
        }
        mem_poke(m->mem, va, e->data + ph->offset, ph->filesz);
        if (!phdr && ph->offset == 0) phdr = va + e->phoff;
        if (end > maxend) maxend = end;
    }
    m->entry = e->entry + bias;
    m->brk_start = m->brk_cur = maxend;

    for (int i = 1; i < nsyms(e); i++) {
        Sym s = get_sym(e, i);
        if ((s.type == STT_FUNC || s.type == STT_OBJECT || s.type == STT_NOTYPE) && s.shndx != SHN_UNDEF &&
            s.shndx != SHN_ABS)
            add_symbol(m, s.name, s.value + bias, s.size);
    }
    sort_symbols(m);

    AuxInfo aux = {phdr, e->is64 ? 56 : 32, (uint64_t)e->nph, m->entry};
    setup_linux_stack(m, &aux);
    m->cpu.rip = m->entry;
    return 0;
}

/* ---- the in-process linker for relocatable objects ---- */

typedef struct {
    const char *name;
    uint64_t addr;
} GlobalSym;

typedef struct {
    Machine *m;
    Elf *objs;
    int nobjs;
    GlobalSym *globals;
    int nglobals;
    uint64_t base;         /* lowest address of the image */
    uint8_t *img;
    uint64_t img_size;
    uint64_t got, got_used, got_slots;
    uint64_t common_next;
    char *err;
    size_t errn;
} Linker;

static GlobalSym *find_global(Linker *L, const char *name) {
    for (int i = 0; i < L->nglobals; i++)
        if (!strcmp(L->globals[i].name, name)) return &L->globals[i];
    return NULL;
}

static int define_global(Linker *L, const char *name, uint64_t addr, int weak) {
    GlobalSym *g = find_global(L, name);
    if (g) {
        if (weak) return 0;
        snprintf(L->err, L->errn, "duplicate symbol: %s", name);
        return -1;
    }
    L->globals = realloc(L->globals, (L->nglobals + 1) * sizeof *L->globals);
    L->globals[L->nglobals++] = (GlobalSym){name, addr};
    return 0;
}

static int sym_value(Linker *L, Elf *e, int idx, uint64_t *out) {
    Sym s = get_sym(e, idx);
    if (s.type == STT_SECTION) {
        *out = e->secaddr[s.shndx];
        return 0;
    }
    if (s.shndx == SHN_ABS) {
        *out = s.value;
        return 0;
    }
    if (s.shndx != SHN_UNDEF && s.shndx != SHN_COMMON && s.bind == STB_LOCAL) {
        *out = e->secaddr[s.shndx] + s.value;
        return 0;
    }
    if (!strcmp(s.name, "_GLOBAL_OFFSET_TABLE_")) {
        *out = L->got;
        return 0;
    }
    GlobalSym *g = find_global(L, s.name);
    if (g) {
        *out = g->addr;
        return 0;
    }
    if (s.bind == STB_WEAK) {
        *out = 0;
        return 0;
    }
    snprintf(L->err, L->errn, "undefined symbol: %s (referenced from %s)", s.name, e->path);
    return -1;
}

static int is_got_reloc(int is64, uint32_t type) {
    if (is64) return type == 3 || type == 9 || type == 41 || type == 42;
    return type == 3 || type == 43;
}

static int apply_reloc(Linker *L, Elf *e, int target_sec, uint64_t off, uint32_t type, int symidx, int64_t addend,
                       int has_addend) {
    uint64_t P = e->secaddr[target_sec] + off;
    uint8_t *loc = L->img + (P - L->base);
    uint64_t S = 0;
    if (symidx && sym_value(L, e, symidx, &S)) return -1;
    int size;
    uint64_t val;
    int pcrel_check = 0, signed32 = 0, unsigned32 = 0;

    if (e->is64) {
        switch (type) {
        case 0: return 0;
        case 1: size = 8; break;                      /* R_X86_64_64 */
        case 2: case 4: size = 4; pcrel_check = 1; break; /* PC32, PLT32 */
        case 10: size = 4; unsigned32 = 1; break;     /* 32 */
        case 11: size = 4; signed32 = 1; break;       /* 32S */
        case 12: case 13: size = 2; break;            /* 16, PC16 */
        case 14: case 15: size = 1; break;            /* 8, PC8 */
        case 24: size = 8; break;                     /* PC64 */
        case 9: case 41: case 42: case 3: size = 4; pcrel_check = type != 3; break; /* GOT forms */
        case 25: size = 8; break;                     /* GOTOFF64 */
        case 26: size = 4; pcrel_check = 1; break;    /* GOTPC32 */
        default:
            snprintf(L->err, L->errn, "%s: unsupported x86-64 relocation type %u", e->path, type);
            return -1;
        }
    } else {
        switch (type) {
        case 0: return 0;
        case 1: case 2: case 4: case 3: case 43: case 9: case 10: size = 4; break;
        case 20: case 21: size = 2; break;
        case 22: case 23: size = 1; break;
        default:
            snprintf(L->err, L->errn, "%s: unsupported i386 relocation type %u", e->path, type);
            return -1;
        }
    }
    int64_t A = has_addend ? addend : (int64_t)rd(loc, size);
    if (!has_addend && size == 4) A = (int32_t)A;
    if (!has_addend && size == 2) A = (int16_t)A;
    if (!has_addend && size == 1) A = (int8_t)A;

    if (is_got_reloc(e->is64, type)) {
        uint64_t slot = L->got + L->got_used * 8;
        L->got_used++;
        wr(L->img + (slot - L->base), e->is64 ? 8 : 4, S);
        if (e->is64 && type != 3) val = slot + A - P;
        else val = slot - L->got + A;
    } else if (e->is64) {
        switch (type) {
        case 2: case 4: case 13: case 15: case 24: val = S + A - P; break;
        case 25: val = S + A - L->got; break;
        case 26: val = L->got + A - P; break;
        default: val = S + A;
        }
    } else {
        switch (type) {
        case 2: case 4: case 21: case 23: val = S + A - P; break;
        case 9: val = S + A - L->got; break;
        case 10: val = L->got + A - P; break;
        default: val = S + A;
        }
    }
    if ((pcrel_check || signed32) && (int64_t)val != (int32_t)val) goto overflow;
    if (unsigned32 && val > 0xFFFFFFFFULL) goto overflow;
    wr(loc, size, val);
    return 0;
overflow:
    snprintf(L->err, L->errn, "%s: relocation overflow (type %u)", e->path, type);
    return -1;
}

static int sec_class(const Shdr *h) {
    if (!(h->flags & SHF_ALLOC) || !h->size) return -1;
    if (h->flags & SHF_EXECINSTR) return 0;
    if (h->type == SHT_NOBITS) return 3;
    if (h->flags & SHF_WRITE) return 2;
    return 1;
}

static int link_objects(Machine *m, Elf *objs, int n, char *err, size_t errn) {
    Linker L = {.m = m, .objs = objs, .nobjs = n, .err = err, .errn = errn};
    int bits = m->bits_override ? m->bits_override : (objs[0].is64 ? 64 : 32);
    int real = bits == 16;
    L.base = real ? 0x100 : bits == 64 ? 0x400000 : 0x08048000;
    uint64_t seg_base = real ? 0x10000 : 0; /* load segment 0x1000 in real mode */

    for (int i = 0; i < n; i++) {
        if (objs[i].is64 != objs[0].is64) {
            snprintf(err, errn, "cannot mix 32- and 64-bit objects");
            return -1;
        }
        objs[i].secaddr = calloc(objs[i].nsh + 1, sizeof(uint64_t));
    }

    /* Does anything define _start? Otherwise synthesize one that calls main. */
    int have_start = 0, have_main = 0;
    for (int i = 0; i < n; i++)
        for (int j = 1; j < nsyms(&objs[i]); j++) {
            Sym s = get_sym(&objs[i], j);
            if (s.shndx == SHN_UNDEF || s.bind == STB_LOCAL) continue;
            if (!strcmp(s.name, "_start")) have_start = 1;
            if (!strcmp(s.name, "main")) have_main = 1;
        }

    /* Layout: text, rodata, data, bss — page-separated outside real mode. */
    uint64_t cur = L.base, class_start[5] = {0}, stub = 0;
    for (int cls = 0; cls < 4; cls++) {
        class_start[cls] = cur;
        for (int i = 0; i < n; i++)
            for (int j = 1; j < objs[i].nsh; j++) {
                Shdr *h = &objs[i].sh[j];
                if (sec_class(h) != cls) continue;
                cur = ALIGN_UP(cur, h->align ? h->align : 1);
                objs[i].secaddr[j] = cur;
                cur += h->size;
            }
        if (cls == 0 && !have_start && have_main) {
            stub = ALIGN_UP(cur, 16);
            cur = stub + 32;
        }
        if (cls == 3) {
            /* COMMON symbols go at the end of .bss */
            for (int i = 0; i < n; i++)
                for (int j = 1; j < nsyms(&objs[i]); j++) {
                    Sym s = get_sym(&objs[i], j);
                    if (s.shndx != SHN_COMMON || find_global(&L, s.name)) continue;
                    cur = ALIGN_UP(cur, s.value ? s.value : 8);
                    define_global(&L, s.name, cur, 0);
                    cur += s.size;
                }
        }
        if (!real && cls < 3 && cur != class_start[cls]) cur = ALIGN_UP(cur, PAGE);
    }
    /* GOT: one slot per GOT-referencing relocation (simple and always correct). */
    for (int i = 0; i < n; i++)
        for (int j = 1; j < objs[i].nsh; j++) {
            Shdr *h = &objs[i].sh[j];
            if (h->type != SHT_RELA && h->type != SHT_REL) continue;
            int entsz = objs[i].is64 ? (h->type == SHT_RELA ? 24 : 16) : (h->type == SHT_RELA ? 12 : 8);
            for (uint64_t k = 0; k < h->size / entsz; k++) {
                const uint8_t *r = objs[i].data + h->offset + k * entsz;
                uint32_t type = objs[i].is64 ? (uint32_t)rd(r + 8, 4) : r[4];
                if (is_got_reloc(objs[i].is64, type)) L.got_slots++;
            }
        }
    cur = ALIGN_UP(cur, 16);
    L.got = cur;
    cur += L.got_slots * 8 + 8;
    class_start[4] = cur;
    uint64_t end = cur;

    /* Global symbols */
    for (int i = 0; i < n; i++)
        for (int j = 1; j < nsyms(&objs[i]); j++) {
            Sym s = get_sym(&objs[i], j);
            if (s.bind == STB_LOCAL || s.shndx == SHN_UNDEF || s.shndx == SHN_COMMON) continue;
            uint64_t a = s.shndx == SHN_ABS ? s.value : objs[i].secaddr[s.shndx] + s.value;
            if (define_global(&L, s.name, a, s.bind == STB_WEAK)) return -1;
        }

    /* Build the image and apply relocations */
    L.img_size = end - L.base;
    L.img = calloc(1, L.img_size + 1);
    for (int i = 0; i < n; i++)
        for (int j = 1; j < objs[i].nsh; j++) {
            Shdr *h = &objs[i].sh[j];
            if (sec_class(h) < 0 || h->type == SHT_NOBITS) continue;
            memcpy(L.img + (objs[i].secaddr[j] - L.base), objs[i].data + h->offset, h->size);
        }
    for (int i = 0; i < n; i++) {
        Elf *e = &objs[i];
        for (int j = 1; j < e->nsh; j++) {
            Shdr *h = &e->sh[j];
            if (h->type != SHT_RELA && h->type != SHT_REL) continue;
            if (h->info >= (uint32_t)e->nsh || sec_class(&e->sh[h->info]) < 0) continue;
            int rela = h->type == SHT_RELA;
            int entsz = e->is64 ? (rela ? 24 : 16) : (rela ? 12 : 8);
            for (uint64_t k = 0; k < h->size / entsz; k++) {
                const uint8_t *r = e->data + h->offset + k * entsz;
                uint64_t off, info;
                int64_t addend = 0;
                uint32_t type;
                int sym;
                if (e->is64) {
                    off = rd(r, 8);
                    info = rd(r + 8, 8);
                    type = (uint32_t)info;
                    sym = (int)(info >> 32);
                    if (rela) addend = (int64_t)rd(r + 16, 8);
                } else {
                    off = rd(r, 4);
                    info = rd(r + 4, 4);
                    type = info & 0xFF;
                    sym = (int)(info >> 8);
                    if (rela) addend = (int32_t)rd(r + 8, 4);
                }
                if (apply_reloc(&L, e, (int)h->info, off, type, sym, addend, rela)) goto fail;
            }
        }
    }

    /* Entry point */
    GlobalSym *start = find_global(&L, "_start");
    if (start) {
        m->entry = start->addr;
    } else if (stub) {
        uint64_t main_addr = find_global(&L, "main")->addr;
        uint8_t *p = L.img + (stub - L.base);
        int k = 0;
        if (bits == 64) {
            static const uint8_t pre[] = {0x48, 0x8B, 0x3C, 0x24, 0x48, 0x8D, 0x74, 0x24, 0x08}; /* mov rdi,[rsp]; lea rsi,[rsp+8] */
            memcpy(p, pre, sizeof pre);
            k = sizeof pre;
            p[k++] = 0xE8;
            wr(p + k, 4, main_addr - (stub + k + 4));
            k += 4;
            static const uint8_t post[] = {0x89, 0xC7, 0xB8, 0x3C, 0, 0, 0, 0x0F, 0x05}; /* mov edi,eax; mov eax,60; syscall */
            memcpy(p + k, post, sizeof post);
        } else if (bits == 32) {
            /* sub esp,8; lea eax,[esp+12]; push eax; push [esp+12]  (keeps 16-byte alignment) */
            static const uint8_t pre[] = {0x83, 0xEC, 0x08, 0x8D, 0x44, 0x24, 0x0C, 0x50, 0xFF, 0x74, 0x24, 0x0C};
            memcpy(p, pre, sizeof pre);
            k = sizeof pre;
            p[k++] = 0xE8;
            wr(p + k, 4, main_addr - (stub + k + 4));
            k += 4;
            static const uint8_t post[] = {0x89, 0xC3, 0xB8, 1, 0, 0, 0, 0xCD, 0x80}; /* mov ebx,eax; mov eax,1; int 0x80 */
            memcpy(p + k, post, sizeof post);
        } else {
            p[k++] = 0xE8;
            wr(p + k, 2, main_addr - (stub + k + 2));
            k += 2;
            static const uint8_t post[] = {0xB4, 0x4C, 0xCD, 0x21}; /* mov ah,4Ch; int 21h (exit code in al) */
            memcpy(p + k, post, sizeof post);
        }
        m->entry = stub;
        add_symbol(m, "_sim_start", stub, 32);
    } else if (real) {
        m->entry = class_start[0];
    } else {
        snprintf(err, errn, "no _start or main symbol found");
        goto fail;
    }

    /* Map memory */
    if (real) {
        mem_map(m->mem, 0, 0x110000, MEM_R | MEM_W | MEM_X, "conventional memory");
        mem_poke(m->mem, seg_base + L.base, L.img, L.img_size);
    } else {
        const char *names[] = {"text", "rodata", "data", "bss"};
        int prots[] = {MEM_R | MEM_X, MEM_R, MEM_R | MEM_W, MEM_R | MEM_W};
        for (int cls = 0; cls < 4; cls++) {
            uint64_t s = class_start[cls], e = cls == 3 ? class_start[4] : class_start[cls + 1];
            if (e > s) mem_map(m->mem, s, e - s, prots[cls], names[cls]);
        }
        mem_poke(m->mem, L.base, L.img, L.img_size);
    }

    /* Debug symbols (locals too) */
    for (int i = 0; i < n; i++)
        for (int j = 1; j < nsyms(&objs[i]); j++) {
            Sym s = get_sym(&objs[i], j);
            if (s.type != STT_FUNC && s.type != STT_OBJECT && s.type != STT_NOTYPE) continue;
            if (s.shndx == SHN_UNDEF || s.shndx == SHN_ABS || s.shndx == SHN_COMMON) continue;
            if (s.shndx >= objs[i].nsh || sec_class(&objs[i].sh[s.shndx]) < 0) continue;
            add_symbol(m, s.name, objs[i].secaddr[s.shndx] + s.value, s.size);
        }
    for (int i = 0; i < L.nglobals; i++) {
        uint64_t a;
        if (!machine_lookup_symbol(m, L.globals[i].name, &a)) add_symbol(m, L.globals[i].name, L.globals[i].addr, 0);
    }
    sort_symbols(m);

    m->brk_start = m->brk_cur = ALIGN_UP(end, PAGE);
    free(L.img);
    free(L.globals);
    return bits;
fail:
    free(L.img);
    free(L.globals);
    return -1;
}

/* ---- DOS environment for 16-bit programs ---- */

static void setup_dos(Machine *m) {
    const uint16_t seg = 0x1000;
    setup_cpu(m, 16);
    m->os = OS_DOS;
    for (int s = 0; s < 6; s++) cpu_load_seg_real(&m->cpu, s, seg);
    uint64_t psp = (uint64_t)seg << 4;
    static const uint8_t int20[] = {0xCD, 0x20};
    mem_poke(m->mem, psp, int20, 2); /* PSP:0000 = int 20h, so a final `ret` exits */
    static const uint8_t tail[] = {0, 0x0D};
    mem_poke(m->mem, psp + 0x80, tail, 2);
    m->cpu.r[RSP] = 0xFFFE;
    uint16_t zero = 0;
    mem_poke(m->mem, psp + 0xFFFE, &zero, 2);
    m->cpu.rip = m->entry;
}

/* ---- entry point ---- */

int load_program(Machine *m, char *err, size_t errn) {
    if (m->nfiles == 0) {
        snprintf(err, errn, "no program given");
        return -1;
    }
    Elf *objs = calloc(m->nfiles, sizeof *objs);
    int rc = -1;
    for (int i = 0; i < m->nfiles; i++) {
        objs[i].path = m->files[i];
        if (read_file(m->files[i], &objs[i].data, &objs[i].size, err, errn)) goto out;
    }

    if (!is_elf(objs[0].data, objs[0].size)) {
        /* Flat binary: DOS .COM by default */
        int bits = m->bits_override ? m->bits_override : 16;
        if (m->nfiles > 1) {
            snprintf(err, errn, "flat binaries cannot be linked with other files");
            goto out;
        }
        if (bits == 16) {
            mem_map(m->mem, 0, 0x110000, MEM_R | MEM_W | MEM_X, "conventional memory");
            if (objs[0].size > 0xFF00) {
                snprintf(err, errn, "flat binary too large for a .COM image");
                goto out;
            }
            mem_poke(m->mem, 0x10100, objs[0].data, objs[0].size);
            m->entry = 0x100;
            setup_dos(m);
        } else {
            uint64_t base = bits == 64 ? 0x400000 : 0x08048000;
            setup_cpu(m, bits);
            m->os = OS_LINUX;
            mem_map(m->mem, base, ALIGN_UP(objs[0].size, PAGE), MEM_R | MEM_W | MEM_X, "image");
            mem_poke(m->mem, base, objs[0].data, objs[0].size);
            m->entry = base;
            m->brk_start = m->brk_cur = ALIGN_UP(base + objs[0].size, PAGE);
            AuxInfo aux = {0, 0, 0, base};
            setup_linux_stack(m, &aux);
            m->cpu.rip = base;
        }
        rc = 0;
        goto out;
    }

    for (int i = 0; i < m->nfiles; i++) {
        if (!is_elf(objs[i].data, objs[i].size)) {
            snprintf(err, errn, "%s: not an ELF file", objs[i].path);
            goto out;
        }
        if (parse_elf(&objs[i], err, errn)) goto out;
    }

    if (objs[0].type == ET_EXEC || objs[0].type == ET_DYN) {
        if (m->nfiles > 1) {
            snprintf(err, errn, "an executable cannot be combined with other files");
            goto out;
        }
        rc = load_exec(m, &objs[0], err, errn);
        goto out;
    }
    for (int i = 0; i < m->nfiles; i++)
        if (objs[i].type != ET_REL) {
            snprintf(err, errn, "%s: expected a relocatable object (.o)", objs[i].path);
            goto out;
        }
    int bits = link_objects(m, objs, m->nfiles, err, errn);
    if (bits < 0) goto out;
    if (bits == 16) {
        setup_dos(m);
    } else {
        setup_cpu(m, bits);
        m->os = OS_LINUX;
        AuxInfo aux = {0, 0, 0, m->entry};
        setup_linux_stack(m, &aux);
        m->cpu.rip = m->entry;
    }
    rc = 0;
out:
    for (int i = 0; i < m->nfiles; i++) {
        free(objs[i].data);
        free(objs[i].sh);
        free(objs[i].ph);
        free(objs[i].secaddr);
    }
    free(objs);
    return rc;
}
