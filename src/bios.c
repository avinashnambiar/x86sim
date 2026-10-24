/* PC firmware, implemented in C (high-level emulation). Every real-mode
 * interrupt vector points at a 16-byte stub in the F000 segment:
 *
 *     out  0xEF, al     ; trap into bios_call(vector)
 *     jnz  done         ; ZF clear: the service finished
 *     sti               ; ZF set: wait (e.g. for a key) and retry
 *     hlt
 *     jmp  out
 *   done: iret
 *
 * Services return CF/ZF to the caller by editing the FLAGS word in the
 * interrupt frame. Also here: power-on setup (IVT, BIOS data area, PIC,
 * PIT, LAPIC virtual wire), the MP table, the E820 memory map, booting from
 * a disk image, and the Linux boot protocol (32-bit entry). */
#include "pc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern const char pc_kb_lower[], pc_kb_upper[];

#define BDA 0x400
#define EBDA_SEG 0x9FC0
#define TABLES 0xF2000u /* diskette/fixed-disk parameters, config table */
#define MPTABLE 0xF6000u /* up to 0xF7FFF: ~5.3 KiB with 255 CPUs */

/* ---- guest access ---- */

static CPU *cpu(PC *pc) { return pc_cpu(pc); }

/* Physical accesses by the firmware, coherent with the simulated caches. */
static void poke(PC *pc, uint64_t pa, const void *p, size_t n) { machine_phys_poke(pc->m, pa, p, n); }
static void poke8(PC *pc, uint64_t pa, uint8_t v) { poke(pc, pa, &v, 1); }
static void poke16(PC *pc, uint64_t pa, uint16_t v) { poke(pc, pa, &v, 2); }
static void poke32(PC *pc, uint64_t pa, uint32_t v) { poke(pc, pa, &v, 4); }
static uint8_t peek8(PC *pc, uint64_t pa) { uint8_t v = 0; machine_phys_peek(pc->m, pa, &v, 1); return v; }
static uint16_t peek16(PC *pc, uint64_t pa) { uint16_t v = 0; machine_phys_peek(pc->m, pa, &v, 2); return v; }
static uint32_t peek32(PC *pc, uint64_t pa) { uint32_t v = 0; machine_phys_peek(pc->m, pa, &v, 4); return v; }

/* Linear accesses from a service (paging applies in virtual-8086 mode). */
static void lread(PC *pc, uint64_t lin, void *buf, size_t n) {
    CPU *c = cpu(pc);
    uint8_t *b = buf;
    while (n) {
        size_t k = 4096 - (lin & 0xFFF);
        if (k > n) k = n;
        mmu_read(c, lin, b, (int)k, 0);
        lin += k, b += k, n -= k;
    }
}
static void lwrite(PC *pc, uint64_t lin, const void *buf, size_t n) {
    CPU *c = cpu(pc);
    const uint8_t *b = buf;
    while (n) {
        size_t k = 4096 - (lin & 0xFFF);
        if (k > n) k = n;
        mmu_write(c, lin, b, (int)k, 0);
        lin += k, b += k, n -= k;
    }
}
static uint64_t seg_lin(PC *pc, int seg, uint32_t off) { return cpu(pc)->sbase[seg] + (off & 0xFFFF); }

#define R16(x) ((uint16_t)c->r[x])
#define R8L(x) ((uint8_t)c->r[x])
#define R8H(x) ((uint8_t)(c->r[x] >> 8))
static void set16(CPU *c, int r, uint16_t v) { c->r[r] = (c->r[r] & ~0xFFFFULL) | v; }
static void set32(CPU *c, int r, uint32_t v) { c->r[r] = v; }
static void set8l(CPU *c, int r, uint8_t v) { c->r[r] = (c->r[r] & ~0xFFULL) | v; }
static void set8h(CPU *c, int r, uint8_t v) { c->r[r] = (c->r[r] & ~0xFF00ULL) | (uint64_t)v << 8; }

/* FLAGS in the caller's interrupt frame (SS:SP+4) */
static void frame_flag(PC *pc, uint16_t bit, int on) {
    CPU *c = cpu(pc);
    uint64_t lin = seg_lin(pc, SS, (uint32_t)(c->r[RSP] + 4));
    uint16_t f;
    lread(pc, lin, &f, 2);
    f = on ? f | bit : f & ~bit;
    lwrite(pc, lin, &f, 2);
}
static void set_cf(PC *pc, int on) { frame_flag(pc, 1, on); }
static void set_zf(PC *pc, int on) { frame_flag(pc, 0x40, on); }
static void ok(PC *pc) { set8h(cpu(pc), RAX, 0); set_cf(pc, 0); }
static void fail(PC *pc, uint8_t ah) { set8h(cpu(pc), RAX, ah); set_cf(pc, 1); }

/* ---- E820 memory map ---- */

typedef struct { uint64_t base, len; uint32_t type; } E820;

static int e820_map(PC *pc, E820 *e) {
    int n = 0;
    e[n++] = (E820){0, 0x9FC00, 1};
    e[n++] = (E820){0x9FC00, 0x400, 2};
    e[n++] = (E820){0xF0000, 0x10000, 2};
    if (pc->lowmem > 0x100000) e[n++] = (E820){0x100000, pc->lowmem - 0x100000, 1};
    e[n++] = (E820){IOAPIC_BASE, 0x1000, 2};
    e[n++] = (E820){LAPIC_BASE, 0x1000, 2};
    e[n++] = (E820){0xFFFC0000, 0x40000, 2};
    if (pc->highmem) e[n++] = (E820){0x100000000ULL, pc->highmem, 1};
    return n;
}

/* ---- video (text mode 80x25 at 0xB8000; mode 13h at 0xA0000) ---- */

static int cols(PC *pc) { return peek16(pc, BDA + 0x4A) ? peek16(pc, BDA + 0x4A) : 80; }
static int rows(PC *pc) { return peek8(pc, BDA + 0x84) + 1; }
static int page_now(PC *pc) { return peek8(pc, BDA + 0x62) & 7; }
static uint64_t cell(PC *pc, int page, int r, int col) { return 0xB8000 + (uint64_t)page * 0x1000 + (uint64_t)(r * cols(pc) + col) * 2; }

static void get_cursor(PC *pc, int page, int *r, int *col) {
    uint16_t v = peek16(pc, BDA + 0x50 + page * 2);
    *col = v & 0xFF;
    *r = v >> 8;
}

static void set_cursor(PC *pc, int page, int r, int col) {
    poke16(pc, BDA + 0x50 + page * 2, (uint16_t)(r << 8 | col));
    if (page == page_now(pc)) { /* hardware cursor, as read by the UI */
        uint16_t pos = (uint16_t)(r * cols(pc) + col);
        pc->vga.crtc[0x0E] = (uint8_t)(pos >> 8);
        pc->vga.crtc[0x0F] = (uint8_t)pos;
    }
}

static void scroll(PC *pc, int page, int up, int lines, uint8_t attr, int r0, int c0, int r1, int c1) {
    if (r1 >= rows(pc)) r1 = rows(pc) - 1;
    if (c1 >= cols(pc)) c1 = cols(pc) - 1;
    int h = r1 - r0 + 1;
    if (h <= 0 || c1 < c0) return;
    if (!lines || lines > h) lines = h;
    uint16_t blank = (uint16_t)(attr << 8 | ' ');
    for (int i = 0; i < h; i++) {
        int dst = up ? r0 + i : r1 - i, src = up ? dst + lines : dst - lines;
        for (int col = c0; col <= c1; col++) {
            uint16_t v = (i < h - lines) ? peek16(pc, cell(pc, page, src, col)) : blank;
            poke16(pc, cell(pc, page, dst, col), v);
        }
    }
}

static void teletype(PC *pc, int page, uint8_t ch, int attr, int echo) {
    int r, col;
    get_cursor(pc, page, &r, &col);
    if (echo) machine_output(pc->m, &ch, 1);
    switch (ch) {
    case 7: return;
    case 8: if (col) col--; break;
    case 10: r++; break;
    case 13: col = 0; break;
    default:
        poke8(pc, cell(pc, page, r, col), ch);
        if (attr >= 0) poke8(pc, cell(pc, page, r, col) + 1, (uint8_t)attr);
        if (++col >= cols(pc)) col = 0, r++;
    }
    if (r >= rows(pc)) {
        uint8_t a = peek8(pc, cell(pc, page, rows(pc) - 1, cols(pc) - 1) + 1);
        scroll(pc, page, 1, 1, a ? a : 7, 0, 0, rows(pc) - 1, cols(pc) - 1);
        r = rows(pc) - 1;
    }
    set_cursor(pc, page, r, col);
}

static void set_mode(PC *pc, int mode) {
    int clear = !(mode & 0x80);
    mode &= 0x7F;
    poke8(pc, BDA + 0x49, (uint8_t)mode);
    poke8(pc, BDA + 0x62, 0);
    poke16(pc, BDA + 0x4E, 0);
    for (int p = 0; p < 8; p++) poke16(pc, BDA + 0x50 + p * 2, 0);
    if (mode == 0x13) {
        poke16(pc, BDA + 0x4A, 40);
        if (clear) for (uint64_t a = 0xA0000; a < 0xAFA00; a += 4) poke32(pc, a, 0);
        return;
    }
    int w = mode <= 1 ? 40 : 80;
    poke16(pc, BDA + 0x4A, (uint16_t)w);
    poke16(pc, BDA + 0x4C, w == 40 ? 0x800 : 0x1000);
    poke8(pc, BDA + 0x84, 24);
    if (clear)
        for (int i = 0; i < 0x4000; i++) poke16(pc, 0xB8000 + (uint64_t)i * 2, 0x0720);
    set_cursor(pc, 0, 0, 0);
}

static void int10(PC *pc) {
    CPU *c = cpu(pc);
    uint8_t ah = R8H(RAX), al = R8L(RAX), bh = R8H(RBX), bl = R8L(RBX);
    int page = bh & 7, r, col;
    switch (ah) {
    case 0x00: set_mode(pc, al); break;
    case 0x01: poke16(pc, BDA + 0x60, R16(RCX)); pc->vga.crtc[0x0A] = R8H(RCX); pc->vga.crtc[0x0B] = R8L(RCX); break;
    case 0x02: set_cursor(pc, page, R8H(RDX), R8L(RDX)); break;
    case 0x03:
        get_cursor(pc, page, &r, &col);
        set16(c, RDX, (uint16_t)(r << 8 | col));
        set16(c, RCX, peek16(pc, BDA + 0x60));
        break;
    case 0x05:
        poke8(pc, BDA + 0x62, al & 7);
        poke16(pc, BDA + 0x4E, (uint16_t)((al & 7) * 0x1000));
        get_cursor(pc, al & 7, &r, &col);
        set_cursor(pc, al & 7, r, col);
        break;
    case 0x06: case 0x07:
        scroll(pc, page_now(pc), ah == 6, al, bh, R8H(RCX), R8L(RCX), R8H(RDX), R8L(RDX));
        break;
    case 0x08:
        get_cursor(pc, page, &r, &col);
        set16(c, RAX, peek16(pc, cell(pc, page, r, col)));
        break;
    case 0x09: case 0x0A: {
        get_cursor(pc, page, &r, &col);
        uint64_t a = cell(pc, page, r, col);
        for (int i = 0; i < R16(RCX) && a < 0xC0000; i++, a += 2) {
            poke8(pc, a, al);
            if (ah == 9) poke8(pc, a + 1, bl);
        }
        break;
    }
    case 0x0E: teletype(pc, page_now(pc), al, -1, 1); break;
    case 0x0F:
        set8h(c, RAX, (uint8_t)cols(pc));
        set8l(c, RAX, peek8(pc, BDA + 0x49));
        set8h(c, RBX, (uint8_t)page_now(pc));
        break;
    case 0x11:
        if (al == 0x30) { set16(c, RCX, 16); set8l(c, RDX, (uint8_t)(rows(pc) - 1)); }
        break;
    case 0x12:
        if (bl == 0x10) { set16(c, RBX, 0x0003); set16(c, RCX, 0); }
        break;
    case 0x13: {
        uint64_t s = seg_lin(pc, ES, R16(RBP));
        int pg = bh & 7, n = R16(RCX), r0, c0;
        get_cursor(pc, pg, &r0, &c0);
        set_cursor(pc, pg, R8H(RDX), R8L(RDX));
        for (int i = 0; i < n; i++) {
            uint8_t ch, at = bl;
            lread(pc, s++, &ch, 1);
            if (al & 2) lread(pc, s++, &at, 1);
            teletype(pc, pg, ch, at, 1);
        }
        if (!(al & 1)) set_cursor(pc, pg, r0, c0);
        break;
    }
    case 0x1A:
        if (al == 0) { set8l(c, RAX, 0x1A); set16(c, RBX, 0x0008); }
        break;
    case 0x4F: set16(c, RAX, 0x014F); break; /* VESA BIOS extensions: not supported */
    }
}

/* ---- disk ---- */

static Disk *drive(PC *pc, uint8_t dl) {
    if (dl == 0x80 && pc->hda.present) return &pc->hda;
    if (dl == 0x00 && pc->fda.present) return &pc->fda;
    return NULL;
}

static void disk_status(PC *pc, uint8_t dl, uint8_t st) {
    pc->disk_status = st;
    poke8(pc, BDA + (dl & 0x80 ? 0x74 : 0x41), st);
}

static int disk_xfer(PC *pc, Disk *d, uint64_t lba, uint32_t count, uint64_t lin, int write) {
    uint8_t buf[512];
    for (uint32_t i = 0; i < count; i++) {
        if (!write) {
            if (disk_rw(d, lba + i, 1, buf, 0)) return -1;
            lwrite(pc, lin + (uint64_t)i * 512, buf, 512);
        } else {
            lread(pc, lin + (uint64_t)i * 512, buf, 512);
            if (disk_rw(d, lba + i, 1, buf, 1)) return -1;
        }
    }
    return 0;
}

static void int13(PC *pc) {
    CPU *c = cpu(pc);
    uint8_t ah = R8H(RAX), al = R8L(RAX), dl = R8L(RDX);
    Disk *d = drive(pc, dl);
    if (ah == 0x01) { set8h(c, RAX, pc->disk_status); set_cf(pc, pc->disk_status != 0); return; }
    if (ah == 0x08 && (dl & 0x80) && !d) { fail(pc, 0x07); set8l(c, RDX, 0); return; }
    if (!d) {
        if (ah == 0x15) { set8h(c, RAX, 0); set_cf(pc, 0); return; } /* no such drive */
        disk_status(pc, dl, 0x01);
        fail(pc, 0x01);
        return;
    }
    uint8_t st = 0;
    switch (ah) {
    case 0x00: case 0x0D: case 0x10: case 0x11: case 0x0C: case 0x04: case 0x47: case 0x44: break;
    case 0x02: case 0x03: {
        int cyl = R8H(RCX) | (R8L(RCX) & 0xC0) << 2, sec = R8L(RCX) & 0x3F, head = R8H(RDX);
        if (!sec || sec > d->spt || head >= d->heads) { st = 0x04; set8l(c, RAX, 0); break; }
        uint64_t lba = ((uint64_t)cyl * d->heads + head) * d->spt + sec - 1;
        if (disk_xfer(pc, d, lba, al, seg_lin(pc, ES, R16(RBX)), ah == 3)) { st = 0x04; set8l(c, RAX, 0); }
        break;
    }
    case 0x08: {
        int maxc = d->cyls - 1;
        set16(c, RAX, 0);
        set8l(c, RBX, dl & 0x80 ? 0 : 4);
        set8h(c, RCX, (uint8_t)maxc);
        set8l(c, RCX, (uint8_t)(d->spt | (maxc >> 2 & 0xC0)));
        set8h(c, RDX, (uint8_t)(d->heads - 1));
        set8l(c, RDX, (uint8_t)((dl & 0x80) ? (pc->hda.present ? 1 : 0) : (pc->fda.present ? 1 : 0)));
        if (!(dl & 0x80)) {
            load_seg(c, ES, (uint16_t)(TABLES >> 4) & 0xF000);
            set16(c, RDI, (uint16_t)(TABLES & 0xFFFF));
        }
        break;
    }
    case 0x15:
        if (dl & 0x80) {
            uint64_t n = d->size / 512;
            set8h(c, RAX, 3);
            set16(c, RCX, (uint16_t)(n >> 16));
            set16(c, RDX, (uint16_t)n);
        } else set8h(c, RAX, 1);
        set_cf(pc, 0);
        return;
    case 0x16: break;
    case 0x41:
        if (R16(RBX) != 0x55AA) { st = 0x01; break; }
        set16(c, RBX, 0xAA55);
        set8h(c, RAX, 0x30);
        set16(c, RCX, 0x0001);
        set_cf(pc, 0);
        return;
    case 0x42: case 0x43: {
        uint64_t p = seg_lin(pc, DS, R16(RSI));
        uint8_t pkt[24] = {0};
        lread(pc, p, pkt, 16);
        uint16_t count, off, seg;
        uint64_t lba, lin;
        memcpy(&count, pkt + 2, 2);
        memcpy(&off, pkt + 4, 2);
        memcpy(&seg, pkt + 6, 2);
        memcpy(&lba, pkt + 8, 8);
        lin = (uint64_t)seg * 16 + off;
        if (off == 0xFFFF && seg == 0xFFFF && pkt[0] >= 24) {
            lread(pc, p + 16, &lin, 8);
        }
        if (disk_xfer(pc, d, lba, count, lin, ah == 0x43)) {
            st = 0x04;
            uint16_t z = 0;
            lwrite(pc, p + 2, &z, 2);
        }
        break;
    }
    case 0x48: {
        uint64_t p = seg_lin(pc, DS, R16(RSI));
        uint8_t b[30] = {0};
        uint16_t size = 26;
        uint32_t v;
        uint64_t n = d->size / 512;
        memcpy(b, &size, 2);
        b[2] = 2; /* geometry valid */
        v = (uint32_t)d->cyls; memcpy(b + 4, &v, 4);
        v = (uint32_t)d->heads; memcpy(b + 8, &v, 4);
        v = (uint32_t)d->spt; memcpy(b + 12, &v, 4);
        memcpy(b + 16, &n, 8);
        b[24] = 0; b[25] = 2; /* 512 bytes per sector */
        lwrite(pc, p, b, 26);
        break;
    }
    default: st = 0x01; break;
    }
    disk_status(pc, dl, st);
    if (st) fail(pc, st);
    else ok(pc);
}

/* ---- keyboard ---- */

static int kb_push(PC *pc, uint16_t key) {
    uint16_t head = peek16(pc, BDA + 0x1A), tail = peek16(pc, BDA + 0x1C);
    uint16_t start = peek16(pc, BDA + 0x80), end = peek16(pc, BDA + 0x82);
    uint16_t next = (uint16_t)(tail + 2);
    if (next >= end) next = start;
    if (next == head) return -1;
    poke16(pc, BDA + tail, key);
    poke16(pc, BDA + 0x1C, next);
    return 0;
}

static int kb_peek(PC *pc, uint16_t *key, int remove) {
    uint16_t head = peek16(pc, BDA + 0x1A), tail = peek16(pc, BDA + 0x1C);
    if (head == tail) return 0;
    *key = peek16(pc, BDA + head);
    if (remove) {
        head = (uint16_t)(head + 2);
        if (head >= peek16(pc, BDA + 0x82)) head = peek16(pc, BDA + 0x80);
        poke16(pc, BDA + 0x1A, head);
    }
    return 1;
}

/* IRQ 1: read the scan code from the 8042 and translate it, like a real BIOS. */
static void int09(PC *pc) {
    uint8_t sc = (uint8_t)pc_port_read(pc, 0x60, 1);
    uint8_t *f = &pc->kb_flags;
    if (sc == 0xE0) { pc->kb_e0 = 1; goto eoi; }
    int brk = sc & 0x80, code = sc & 0x7F, e0 = pc->kb_e0;
    pc->kb_e0 = 0;
    switch (code) {
    case 0x2A: *f = brk ? *f & ~2 : *f | 2; goto flags;
    case 0x36: *f = brk ? *f & ~1 : *f | 1; goto flags;
    case 0x1D: *f = brk ? *f & ~4 : *f | 4; goto flags;
    case 0x38: *f = brk ? *f & ~8 : *f | 8; goto flags;
    case 0x3A: if (!brk) *f ^= 0x40; goto flags;
    }
    if (brk) goto eoi;
    {
        int shift = (*f & 3) != 0;
        uint8_t ascii = 0;
        if (!e0 && code < 0x3A) {
            ascii = (uint8_t)(shift ? pc_kb_upper[code] : pc_kb_lower[code]);
            if ((*f & 0x40) && ascii >= 'a' && ascii <= 'z') ascii -= 32;
            else if ((*f & 0x40) && ascii >= 'A' && ascii <= 'Z') ascii += 32;
            if (*f & 4) ascii = (ascii >= 'a' && ascii <= 'z') || (ascii >= 'A' && ascii <= 'Z') ? ascii & 0x1F : 0;
            if (*f & 8) ascii = 0;
        } else if (e0) ascii = 0xE0;
        kb_push(pc, (uint16_t)(code << 8 | ascii));
    }
    goto eoi;
flags:
    poke8(pc, BDA + 0x17, *f);
eoi:
    pic_eoi(pc, 1);
}

static void int16(PC *pc) {
    CPU *c = cpu(pc);
    uint8_t ah = R8H(RAX);
    uint16_t key;
    switch (ah) {
    case 0x00: case 0x10:
        if (!kb_peek(pc, &key, 1)) { c->rflags |= FL_ZF; return; } /* wait */
        if (ah == 0 && (key & 0xFF) == 0xE0) key &= 0xFF00;
        set16(c, RAX, key);
        break;
    case 0x01: case 0x11:
        if (kb_peek(pc, &key, 0)) {
            if (ah == 1 && (key & 0xFF) == 0xE0) key &= 0xFF00;
            set16(c, RAX, key);
            set_zf(pc, 0);
        } else set_zf(pc, 1);
        break;
    case 0x02: case 0x12: set8l(c, RAX, pc->kb_flags); break;
    case 0x05: set8l(c, RAX, kb_push(pc, R16(RCX)) ? 1 : 0); break;
    case 0x0A: set16(c, RBX, 0x41AB); break;
    }
    c->rflags &= ~FL_ZF;
}

/* ---- system services (INT 15h) ---- */

static void int15(PC *pc) {
    CPU *c = cpu(pc);
    uint8_t ah = R8H(RAX);
    uint16_t ax = R16(RAX);
    if (ax == 0xE820 && (uint32_t)c->r[RDX] == 0x534D4150) {
        E820 e[16];
        int n = e820_map(pc, e);
        uint32_t i = (uint32_t)c->r[RBX];
        if (i >= (uint32_t)n) { fail(pc, 0x86); return; }
        uint8_t b[20];
        memcpy(b, &e[i].base, 8);
        memcpy(b + 8, &e[i].len, 8);
        memcpy(b + 16, &e[i].type, 4);
        lwrite(pc, seg_lin(pc, ES, R16(RDI)), b, 20);
        set32(c, RAX, 0x534D4150);
        set32(c, RCX, 20);
        set32(c, RBX, i + 1 < (uint32_t)n ? i + 1 : 0);
        set_cf(pc, 0);
        return;
    }
    if (ax == 0xE801) {
        uint64_t below16 = pc->lowmem > 0x1000000 ? 0x3C00 : (pc->lowmem - 0x100000) >> 10;
        uint64_t above16 = pc->lowmem > 0x1000000 ? (pc->lowmem - 0x1000000) >> 16 : 0;
        set16(c, RAX, (uint16_t)below16); set16(c, RCX, (uint16_t)below16);
        set16(c, RBX, (uint16_t)above16); set16(c, RDX, (uint16_t)above16);
        set_cf(pc, 0);
        return;
    }
    switch (ax) {
    case 0x2400: pc_set_a20(pc, 0); ok(pc); return;
    case 0x2401: pc_set_a20(pc, 1); ok(pc); return;
    case 0x2402: set8l(c, RAX, (c->a20_mask >> 20) & 1); ok(pc); return;
    case 0x2403: set16(c, RBX, 3); ok(pc); return;
    }
    switch (ah) {
    case 0x4F: set_cf(pc, 1); return; /* keyboard intercept: process the key */
    case 0x86: /* wait CX:DX microseconds: let virtual time pass */
        pc->idle_ns += ((uint64_t)R16(RCX) << 16 | R16(RDX)) * 1000;
        ok(pc);
        return;
    case 0x87: { /* copy CX words between descriptors in the GDT at ES:SI */
        uint64_t g = seg_lin(pc, ES, R16(RSI));
        uint8_t sd[8], dd[8];
        lread(pc, g + 0x10, sd, 8);
        lread(pc, g + 0x18, dd, 8);
        uint32_t src = sd[2] | sd[3] << 8 | sd[4] << 16 | (uint32_t)sd[7] << 24;
        uint32_t dst = dd[2] | dd[3] << 8 | dd[4] << 16 | (uint32_t)dd[7] << 24;
        uint32_t n = (uint32_t)R16(RCX) * 2;
        uint8_t buf[512];
        for (uint32_t i = 0; i < n; i += 512) {
            uint32_t k = n - i < 512 ? n - i : 512;
            phys_read(c, src + i, buf, (int)k);
            phys_write(c, dst + i, buf, (int)k);
        }
        ok(pc);
        return;
    }
    case 0x88: {
        uint64_t kb = (pc->lowmem - 0x100000) >> 10;
        set16(c, RAX, (uint16_t)(kb > 0xFFFF ? 0xFFFF : kb));
        set_cf(pc, 0);
        return;
    }
    case 0x90: case 0x91: ok(pc); return;
    case 0xC0:
        load_seg(c, ES, 0xF000);
        set16(c, RBX, (uint16_t)((TABLES + 0x30) & 0xFFFF));
        ok(pc);
        return;
    case 0xC1:
        load_seg(c, ES, EBDA_SEG);
        ok(pc);
        return;
    }
    fail(pc, 0x86);
}

/* ---- time (INT 1Ah) and PCI BIOS ---- */

static uint8_t to_bcd(int v) { return (uint8_t)((v / 10) << 4 | v % 10); }

static void int1a(PC *pc) {
    CPU *c = cpu(pc);
    uint8_t ah = R8H(RAX), al = R8L(RAX);
    time_t t = (time_t)(pc->cmos.base_time + (int64_t)(pc_now(pc) / 1000000000ULL));
    struct tm tm;
    localtime_r(&t, &tm);
    switch (ah) {
    case 0x00: {
        uint32_t ticks = peek32(pc, BDA + 0x6C);
        set16(c, RCX, (uint16_t)(ticks >> 16));
        set16(c, RDX, (uint16_t)ticks);
        set8l(c, RAX, peek8(pc, BDA + 0x70));
        poke8(pc, BDA + 0x70, 0);
        set_cf(pc, 0);
        return;
    }
    case 0x01: poke32(pc, BDA + 0x6C, (uint32_t)R16(RCX) << 16 | R16(RDX)); ok(pc); return;
    case 0x02:
        set8h(c, RCX, to_bcd(tm.tm_hour)); set8l(c, RCX, to_bcd(tm.tm_min));
        set8h(c, RDX, to_bcd(tm.tm_sec)); set8l(c, RDX, 0);
        ok(pc);
        return;
    case 0x04:
        set8h(c, RCX, to_bcd(19 + tm.tm_year / 100)); set8l(c, RCX, to_bcd(tm.tm_year % 100));
        set8h(c, RDX, to_bcd(tm.tm_mon + 1)); set8l(c, RDX, to_bcd(tm.tm_mday));
        ok(pc);
        return;
    case 0x03: case 0x05: ok(pc); return;
    case 0xB1: {
        int bus = R8H(RBX), devfn = R8L(RBX), reg = R16(RDI) & 0xFF, f;
        switch (al) {
        case 0x01:
            set16(c, RAX, 0x0001);
            set16(c, RBX, 0x0210);
            set8l(c, RCX, 0);
            set32(c, RDX, 0x20494350); /* "PCI " */
            set_cf(pc, 0);
            return;
        case 0x02:
            f = pci_find(pc, R16(RDX), R16(RCX), R16(RSI));
            if (f < 0) { fail(pc, 0x86); return; }
            set16(c, RBX, (uint16_t)f);
            ok(pc);
            return;
        case 0x03:
            f = pci_find_class(pc, (uint32_t)c->r[RCX] & 0xFFFFFF, R16(RSI));
            if (f < 0) { fail(pc, 0x86); return; }
            set16(c, RBX, (uint16_t)f);
            ok(pc);
            return;
        case 0x08: set8l(c, RCX, (uint8_t)pci_cfg_read(pc, bus, devfn, reg, 1)); ok(pc); return;
        case 0x09: set16(c, RCX, (uint16_t)pci_cfg_read(pc, bus, devfn, reg, 2)); ok(pc); return;
        case 0x0A: set32(c, RCX, pci_cfg_read(pc, bus, devfn, reg, 4)); ok(pc); return;
        case 0x0B: pci_cfg_write(pc, bus, devfn, reg, 1, R8L(RCX)); ok(pc); return;
        case 0x0C: pci_cfg_write(pc, bus, devfn, reg, 2, R16(RCX)); ok(pc); return;
        case 0x0D: pci_cfg_write(pc, bus, devfn, reg, 4, (uint32_t)c->r[RCX]); ok(pc); return;
        }
        fail(pc, 0x81);
        return;
    }
    }
    fail(pc, 0x86);
}

/* ---- booting ---- */

static int boot_from(PC *pc, int dl, char *err, size_t errn) {
    CPU *c = cpu(pc);
    Disk *d = drive(pc, (uint8_t)dl);
    uint8_t sec[512];
    if (!d || disk_rw(d, 0, 1, sec, 0)) {
        if (err) snprintf(err, errn, "no bootable disk");
        return -1;
    }
    if ((dl & 0x80) && (sec[510] != 0x55 || sec[511] != 0xAA)) {
        if (err) snprintf(err, errn, "disk has no boot signature");
        return -1;
    }
    poke(pc, 0x7C00, sec, 512);
    load_seg(c, CS, 0);
    load_seg(c, DS, 0);
    load_seg(c, ES, 0);
    load_seg(c, SS, 0);
    c->r[RSP] = 0x7C00;
    c->r[RDX] = (uint64_t)dl;
    c->rip = c->next_rip = 0x7C00;
    c->rflags |= FL_IF;
    return 0;
}

int bios_boot_disk(PC *pc, char *err, size_t errn) {
    int order[2] = {0x80, 0x00};
    if (pc->m->pc_boot == 'a') order[0] = 0x00, order[1] = 0x80;
    if (!pc->hda.present && !pc->fda.present) {
        snprintf(err, errn, "nothing to boot: give a boot image, --hda, --fda or --kernel");
        return -1;
    }
    for (int i = 0; i < 2; i++)
        if (drive(pc, (uint8_t)order[i]) && !boot_from(pc, order[i], err, errn)) return 0;
    return -1;
}

static void puts_screen(PC *pc, const char *s) {
    while (*s) teletype(pc, 0, (uint8_t)*s++, -1, 1);
}

/* ---- the service dispatcher ---- */

void bios_call(PC *pc, int v) {
    CPU *c = cpu(pc);
    c->rflags &= ~FL_ZF; /* finished, unless a service asks to wait */
    switch (v) {
    case 0x08: { /* IRQ 0 */
        uint32_t t = peek32(pc, BDA + 0x6C) + 1;
        if (t >= 0x1800B0) { t = 0; poke8(pc, BDA + 0x70, 1); }
        poke32(pc, BDA + 0x6C, t);
        pic_eoi(pc, 0);
        return;
    }
    case 0x09: int09(pc); return;
    case 0x10: int10(pc); return;
    case 0x11: set16(c, RAX, peek16(pc, BDA + 0x10)); return;
    case 0x12: set16(c, RAX, peek16(pc, BDA + 0x13)); return;
    case 0x13: int13(pc); return;
    case 0x14: {
        uint8_t ah = R8H(RAX);
        if (R16(RDX) != 0) { set8h(c, RAX, 0x80); return; }
        if (ah == 1) { uart_tx(pc, R8L(RAX)); set8h(c, RAX, 0x60); }
        else if (ah == 2) {
            uint8_t lsr = (uint8_t)pc_port_read(pc, 0x3FD, 1);
            if (lsr & 1) { set8l(c, RAX, (uint8_t)pc_port_read(pc, 0x3F8, 1)); set8h(c, RAX, 0); }
            else set8h(c, RAX, 0x80);
        } else set16(c, RAX, 0x60B0);
        return;
    }
    case 0x15: int15(pc); return;
    case 0x16: int16(pc); return;
    case 0x17: set8h(c, RAX, 0x01); return; /* printer timeout */
    case 0x18: {
        Machine *m = pc->m;
        puts_screen(pc, "No bootable device.\r\n");
        m->state = MS_HALTED;
        snprintf(m->status, sizeof m->status, "no bootable device (INT 18h)");
        c->stop_request = 1;
        m->last_stop = STOP_HALT;
        return;
    }
    case 0x19:
        if (boot_from(pc, pc->m->pc_boot == 'a' ? 0x00 : 0x80, NULL, 0) &&
            boot_from(pc, pc->m->pc_boot == 'a' ? 0x80 : 0x00, NULL, 0))
            bios_call(pc, 0x18);
        return;
    case 0x1A: int1a(pc); return;
    case 0x70: /* IRQ 8: acknowledge the RTC */
        pc_port_write(pc, 0x70, 1, 0x0C);
        pc_port_read(pc, 0x71, 1);
        pic_eoi(pc, 8);
        return;
    case 0x74: pc_port_read(pc, 0x60, 1); pic_eoi(pc, 12); return;
    }
    if (v >= 0x08 && v <= 0x0F) pic_eoi(pc, v - 8);
    else if (v >= 0x70 && v <= 0x77) pic_eoi(pc, v - 0x70 + 8);
}

/* ---- power-on ---- */

static uint8_t checksum(PC *pc, uint64_t a, int n) {
    uint8_t s = 0;
    for (int i = 0; i < n; i++) s = (uint8_t)(s + peek8(pc, a + i));
    return (uint8_t)-s;
}

static void mp_table(PC *pc) {
    static uint8_t t[8192];
    int n = 44, entries = 0, ioapic_id = pc->ioapic.id;
    uint8_t *e;
    memset(t, 0, sizeof t);
    /* processors: APIC ID = CPU number, CPU 0 boots */
    for (int i = 0; i < pc->m->ncpu; i++) {
        e = t + n;
        e[0] = 0; e[1] = (uint8_t)i; e[2] = 0x14; e[3] = i == 0 ? 3 : 1;
        uint32_t sig = 0x000906EA, feat = 0x0789FBFF;
        memcpy(e + 4, &sig, 4);
        memcpy(e + 8, &feat, 4);
        n += 20, entries++;
    }
    /* bus */
    e = t + n;
    e[0] = 1; e[1] = 0;
    memcpy(e + 2, "ISA   ", 6);
    n += 8, entries++;
    /* I/O APIC */
    e = t + n;
    e[0] = 2; e[1] = (uint8_t)ioapic_id; e[2] = 0x11; e[3] = 1;
    uint32_t ioa = IOAPIC_BASE;
    memcpy(e + 4, &ioa, 4);
    n += 8, entries++;
    /* I/O interrupt assignments: ExtINT on pin 0, IRQ 0 on pin 2, others identity */
    for (int irq = -1; irq < 16; irq++) {
        if (irq == 2) continue;
        e = t + n;
        e[0] = 3;
        e[1] = irq < 0 ? 3 : 0;
        e[4] = 0;
        e[5] = (uint8_t)(irq < 0 ? 0 : irq);
        e[6] = (uint8_t)ioapic_id;
        e[7] = (uint8_t)(irq < 0 ? 0 : irq == 0 ? 2 : irq);
        n += 8, entries++;
    }
    /* local interrupts: LINT0 ExtINT, LINT1 NMI */
    for (int i = 0; i < 2; i++) {
        e = t + n;
        e[0] = 4; e[1] = i ? 1 : 3; e[4] = 0; e[5] = 0; e[6] = 0xFF; e[7] = (uint8_t)i;
        n += 8, entries++;
    }
    memcpy(t, "PCMP", 4);
    t[4] = (uint8_t)n; t[5] = (uint8_t)(n >> 8);
    t[6] = 4;
    memcpy(t + 8, "X86SIM  ", 8);
    memcpy(t + 16, "PC          ", 12);
    t[34] = (uint8_t)entries; t[35] = (uint8_t)(entries >> 8);
    uint32_t la = LAPIC_BASE;
    memcpy(t + 36, &la, 4);
    poke(pc, MPTABLE + 16, t, (size_t)n);
    poke8(pc, MPTABLE + 16 + 7, checksum(pc, MPTABLE + 16, n));
    /* floating pointer */
    uint8_t f[16] = {'_', 'M', 'P', '_'};
    uint32_t p = MPTABLE + 16;
    memcpy(f + 4, &p, 4);
    f[8] = 1; f[9] = 4;
    poke(pc, MPTABLE, f, 16);
    poke8(pc, MPTABLE + 10, checksum(pc, MPTABLE, 16));
}

/* ACPI: RSDP -> RSDT -> MADT (local APIC, I/O APIC, IRQ 0 -> GSI 2, LINT1 NMI)
 *                    -> DMAR (one VT-d unit for everything, interrupt remapping) */
#define ACPI_BASE 0xF4000u /* RSDP; RSDT at +0x100, MADT (~2.1 KiB with 255 CPUs) at +0x200, DMAR at +0xC00 */

static void acpi_header(uint8_t *t, const char *sig, uint32_t len) {
    memcpy(t, sig, 4);
    memcpy(t + 4, &len, 4);
    t[8] = 1;
    memcpy(t + 10, "X86SIM", 6);
    memcpy(t + 16, "X86SIMPC", 8);
    t[24] = 1;
    memcpy(t + 28, "XSIM", 4);
    t[32] = 1;
}

static void acpi_tables(PC *pc) {
    const uint32_t rsdt = ACPI_BASE + 0x100, madt = ACPI_BASE + 0x200, dmar = ACPI_BASE + 0xC00;
    static uint8_t m[44 + MAX_CPUS * 8 + 28];
    memset(m, 0, sizeof m);
    uint32_t n = 44, v;
    v = LAPIC_BASE;
    memcpy(m + 36, &v, 4);
    m[40] = 1; /* PCAT_COMPAT: 8259s present */
    for (int i = 0; i < pc->m->ncpu; i++) { /* processor UID = APIC ID = CPU number, enabled */
        uint8_t lapic[8] = {0, 8, (uint8_t)i, (uint8_t)i, 1, 0, 0, 0};
        memcpy(m + n, lapic, 8), n += 8;
    }
    uint8_t ioapic[12] = {1, 12, (uint8_t)pc->ioapic.id, 0};
    v = IOAPIC_BASE;
    memcpy(ioapic + 4, &v, 4);
    memcpy(m + n, ioapic, 12), n += 12;
    uint8_t iso[10] = {2, 10, 0, 0, 2, 0, 0, 0, 0, 0};
    memcpy(m + n, iso, 10), n += 10;
    uint8_t nmi[6] = {4, 6, 0xFF, 0, 0, 1};
    memcpy(m + n, nmi, 6), n += 6;
    acpi_header(m, "APIC", n);
    poke(pc, madt, m, n);
    poke8(pc, madt + 9, checksum(pc, madt, (int)n));

    /* DMAR: 48-bit host addresses, interrupt remapping; one DRHD covering all
     * PCI devices, with the I/O APIC in its scope (source ID F0:1F.0) */
    uint8_t d[48 + 24] = {0};
    d[36] = 47;                          /* host address width - 1 */
    d[37] = 1;                           /* INTR_REMAP (x2APIC opt-out clear) */
    uint8_t drhd[24] = {0, 0, 24, 0, 1}; /* type 0, length 24, INCLUDE_PCI_ALL */
    uint64_t base = DMAR_BASE;
    memcpy(drhd + 8, &base, 8);
    uint8_t scope[8] = {3, 8, 0, 0, (uint8_t)pc->ioapic.id, 0xF0, 0x1F, 0}; /* I/O APIC */
    memcpy(drhd + 16, scope, 8);
    memcpy(d + 48, drhd, 24);
    acpi_header(d, "DMAR", sizeof d);
    poke(pc, dmar, d, sizeof d);
    poke8(pc, dmar + 9, checksum(pc, dmar, (int)sizeof d));

    uint8_t r[44] = {0};
    acpi_header(r, "RSDT", 44);
    memcpy(r + 36, &madt, 4);
    memcpy(r + 40, &dmar, 4);
    poke(pc, rsdt, r, 44);
    poke8(pc, rsdt + 9, checksum(pc, rsdt, 44));

    uint8_t p[20] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    memcpy(p + 9, "X86SIM", 6);
    memcpy(p + 16, &rsdt, 4);
    poke(pc, ACPI_BASE, p, 20);
    poke8(pc, ACPI_BASE + 8, checksum(pc, ACPI_BASE, 20));
}

void bios_post(PC *pc) {
    CPU *c = cpu(pc);
    /* interrupt stubs and the vector table */
    static const uint8_t generic[] = {0xE6, BIOS_PORT, 0x75, 0x04, 0xFB, 0xF4, 0xEB, 0xF8, 0xCF};
    static const uint8_t irq0[] = {0xE6, BIOS_PORT, 0xCD, 0x1C, 0xCF};
    static const uint8_t iret[] = {0xCF};
    for (int v = 0; v < 256; v++) {
        uint32_t a = BIOS_STUBS + (uint32_t)v * 16;
        if (v == 0x08) poke(pc, a, irq0, sizeof irq0);
        else if (v == 0x1C || v == 0x4A || (v >= 0x20 && v <= 0x6F && v != 0x4A)) poke(pc, a, iret, 1);
        else poke(pc, a, generic, sizeof generic);
        poke32(pc, (uint64_t)v * 4, 0xF0000000u | (a & 0xFFFF));
    }
    /* parameter tables */
    static const uint8_t fdpt[11] = {0xAF, 0x02, 0x25, 0x02, 0x12, 0x1B, 0xFF, 0x6C, 0xF6, 0x0F, 0x08};
    poke(pc, TABLES, fdpt, sizeof fdpt);
    poke32(pc, 0x1E * 4, 0xF0000000u | (TABLES & 0xFFFF));
    if (pc->hda.present) {
        uint8_t h[16] = {0};
        uint16_t cyl = (uint16_t)pc->hda.cyls;
        memcpy(h, &cyl, 2);
        h[2] = (uint8_t)pc->hda.heads;
        h[5] = h[6] = 0xFF;
        h[8] = pc->hda.heads > 8 ? 0xC8 : 0xC0;
        memcpy(h + 12, &cyl, 2);
        h[14] = (uint8_t)pc->hda.spt;
        poke(pc, TABLES + 0x10, h, 16);
        poke32(pc, 0x41 * 4, 0xF0000000u | ((TABLES + 0x10) & 0xFFFF));
        poke32(pc, 0x46 * 4, 0xF0000000u | ((TABLES + 0x10) & 0xFFFF));
    }
    static const uint8_t cfg[10] = {0x08, 0x00, 0xFC, 0x00, 0x01, 0x74, 0, 0, 0, 0};
    poke(pc, TABLES + 0x30, cfg, sizeof cfg);
    poke32(pc, 0x1D * 4, 0);
    poke32(pc, 0x1F * 4, 0);
    poke32(pc, 0x43 * 4, 0);
    /* reset vector, date and model byte; mirrored at the top of 4 GiB */
    static const uint8_t reset[] = {0xCD, 0x19, 0xF4};
    poke(pc, 0xFFFF0, reset, sizeof reset);
    poke(pc, 0xFFFF5, "10/23/26", 8);
    poke8(pc, 0xFFFFE, 0xFC);
    mp_table(pc);
    acpi_tables(pc);
    uint8_t rom[4096];
    for (uint32_t a = 0; a < 0x10000; a += sizeof rom) {
        mem_peek(pc->m->mem, 0xF0000 + a, rom, sizeof rom);
        mem_poke(pc->m->mem, 0xFFFF0000ULL + a, rom, sizeof rom);
    }

    /* BIOS data area */
    for (int i = 0; i < 256; i++) poke8(pc, BDA + i, 0);
    poke16(pc, BDA + 0x00, 0x3F8); /* COM1 */
    uint16_t equip = 0x0020 | 0x0200 | 0x0004; /* 80x25 colour, one serial port, PS/2 mouse */
    if (pc->fda.present) equip |= 1;
    poke16(pc, BDA + 0x10, equip);
    poke16(pc, BDA + 0x13, 639);
    poke16(pc, BDA + 0x0E, EBDA_SEG);
    poke16(pc, BDA + 0x1A, 0x1E);
    poke16(pc, BDA + 0x1C, 0x1E);
    poke16(pc, BDA + 0x80, 0x1E);
    poke16(pc, BDA + 0x82, 0x3E);
    poke8(pc, BDA + 0x75, pc->hda.present ? 1 : 0);
    poke8(pc, BDA + 0x96, 0x10);
    poke16(pc, BDA + 0x63, 0x3D4);
    poke8(pc, BDA + 0x85, 16);
    poke16(pc, BDA + 0x60, 0x0D0E);
    poke8(pc, (uint64_t)EBDA_SEG * 16, 1);
    pc->kb_flags = pc->kb_e0 = 0;
    set_mode(pc, 3);

    /* PIC: vectors 08h and 70h, timer/keyboard/cascade unmasked */
    static const uint8_t init[][2] = {{0x20, 0x11}, {0x21, 0x08}, {0x21, 0x04}, {0x21, 0x01}, {0xA0, 0x11},
                                      {0xA1, 0x70}, {0xA1, 0x02}, {0xA1, 0x01}, {0x21, 0xF8}, {0xA1, 0xFF},
                                      {0x43, 0x36}, {0x40, 0x00}, {0x40, 0x00}};
    for (size_t i = 0; i < sizeof init / sizeof init[0]; i++) pc_port_write(pc, init[i][0], 1, init[i][1]);
    /* LAPIC: virtual wire (LINT0 ExtINT, LINT1 NMI) */
    pc->lapic[0].lvt[4] = 0x700;
    pc->lapic[0].lvt[5] = 0x400;
    pc_set_a20(pc, 1);
    pc->kbc.out_port |= 2;
    c->rflags &= ~FL_IF;
}

/* ---- Linux boot protocol: 32-bit entry ---- */

static uint8_t *read_file(const char *path, size_t *len, char *err, size_t errn) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errn, "cannot open %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(n > 0 ? (size_t)n : 1);
    if (n < 0 || fread(p, 1, (size_t)n, f) != (size_t)n) {
        snprintf(err, errn, "cannot read %s", path);
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

int bios_boot_linux(PC *pc, char *err, size_t errn) {
    Machine *m = pc->m;
    CPU *c = cpu(pc);
    size_t klen;
    uint8_t *k = read_file(m->pc_kernel, &klen, err, errn);
    if (!k) return -1;
    if (klen < 0x400 || memcmp(k + 0x202, "HdrS", 4)) {
        snprintf(err, errn, "%s is not a Linux bzImage", m->pc_kernel);
        free(k);
        return -1;
    }
    uint16_t version = (uint16_t)(k[0x206] | k[0x207] << 8);
    if (version < 0x0206 || !(k[0x211] & 1)) {
        snprintf(err, errn, "kernel boot protocol %x.%02x is too old (need 2.06+, bzImage)", version >> 8, version & 0xFF);
        free(k);
        return -1;
    }
    int setup_sects = k[0x1F1] ? k[0x1F1] : 4;
    size_t pm_off = (size_t)(setup_sects + 1) * 512;
    if (pm_off >= klen || 0x100000 + (klen - pm_off) > pc->lowmem) {
        snprintf(err, errn, "kernel does not fit in memory");
        free(k);
        return -1;
    }
    poke(pc, 0x100000, k + pm_off, klen - pm_off);

    /* boot_params ("zero page") at 0x10000, command line at 0x20000 */
    const uint64_t bp = 0x10000, cmd = 0x20000;
    uint8_t z[4096];
    memset(z, 0, sizeof z);
    int hdr_end = 0x202 + k[0x201];
    memcpy(z + 0x1F1, k + 0x1F1, (size_t)(hdr_end - 0x1F1));
    free(k);
    z[0x210] = 0xFF;                          /* type_of_loader: undefined */
    z[0x211] |= 0x80;                         /* CAN_USE_HEAP */
    z[0x224] = 0x00; z[0x225] = 0xFE;         /* heap_end_ptr */
    uint32_t v32 = (uint32_t)cmd;
    memcpy(z + 0x228, &v32, 4);               /* cmd_line_ptr */
    z[0x1FA] = 0xFF; z[0x1FB] = 0xFF;         /* vid_mode: normal */
    /* screen_info: 80x25 VGA text */
    z[0x06] = 3; z[0x07] = 80; z[0x0E] = 25; z[0x0F] = 1; z[0x10] = 16;
    uint32_t ext = (uint32_t)((pc->lowmem - 0x100000) >> 10);
    uint16_t ext16 = (uint16_t)(ext > 0xFFFF ? 0xFFFF : ext);
    memcpy(z + 0x02, &ext16, 2);
    memcpy(z + 0x1E0, &ext, 4);               /* alt_mem_k */

    const char *line = m->pc_append ? m->pc_append : "console=ttyS0 earlyprintk=serial";
    uint32_t cmax;
    memcpy(&cmax, z + 0x238, 4);
    size_t ln = strlen(line);
    if (cmax && ln > cmax) ln = cmax;
    poke(pc, cmd, line, ln);
    poke8(pc, cmd + ln, 0);

    if (m->pc_initrd) {
        size_t ilen;
        uint8_t *ird = read_file(m->pc_initrd, &ilen, err, errn);
        if (!ird) return -1;
        uint32_t maxaddr;
        memcpy(&maxaddr, z + 0x22C, 4);
        uint64_t top = pc->lowmem < (uint64_t)maxaddr + 1 ? pc->lowmem : (uint64_t)maxaddr + 1;
        if (top > 0x38000000) top = 0x38000000;
        uint64_t at = (top - ilen) & ~0xFFFULL;
        if (ilen >= top || at < 0x100000 + 0x2000000) {
            snprintf(err, errn, "initrd does not fit in memory (try more --ram)");
            free(ird);
            return -1;
        }
        poke(pc, at, ird, ilen);
        free(ird);
        v32 = (uint32_t)at;
        memcpy(z + 0x218, &v32, 4);
        v32 = (uint32_t)ilen;
        memcpy(z + 0x21C, &v32, 4);
    }

    E820 e[16];
    int n = e820_map(pc, e);
    z[0x1E8] = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        memcpy(z + 0x2D0 + i * 20, &e[i].base, 8);
        memcpy(z + 0x2D0 + i * 20 + 8, &e[i].len, 8);
        memcpy(z + 0x2D0 + i * 20 + 16, &e[i].type, 4);
    }
    poke(pc, bp, z, sizeof z);

    /* flat 32-bit protected mode: __BOOT_CS = 0x10, __BOOT_DS = 0x18 */
    static const uint64_t gdt[4] = {0, 0, 0x00CF9A000000FFFFULL, 0x00CF93000000FFFFULL};
    poke(pc, 0x800, gdt, sizeof gdt);
    c->gdtr.base = 0x800;
    c->gdtr.limit = sizeof gdt - 1;
    c->idtr.base = 0;
    c->idtr.limit = 0;
    c->cr0 |= CR0_PE;
    load_seg(c, CS, 0x10);
    load_seg(c, DS, 0x18);
    load_seg(c, ES, 0x18);
    load_seg(c, SS, 0x18);
    load_seg(c, FS, 0x18);
    load_seg(c, GS, 0x18);
    cpu_update_mode(c);
    c->rflags = 2;
    c->r[RSI] = bp;
    c->r[RSP] = 0x9F000;
    c->r[RBP] = c->r[RDI] = c->r[RBX] = 0;
    c->rip = 0x100000;
    return 0;
}
