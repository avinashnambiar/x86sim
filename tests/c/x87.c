/* x87 FPU test: results are printed as raw bits and compared exactly with
 * the reference CPU. Transcendentals are only printed after rounding to
 * double, since their last bits are implementation-specific. */
#include "../../runtime/sim.h"

typedef struct { unsigned long long sig; unsigned short se; } X;

static void px(const char *name, long double v) {
    X x;
    unsigned char *p = (unsigned char *)&v, *q = (unsigned char *)&x;
    for (int i = 0; i < 10; i++) q[i] = p[i];
    print_str(name);
    print_str(" ");
    print_hex(x.se);
    print_str(":");
    print_hex(x.sig);
    print_nl();
}

static void pu(const char *name, unsigned long long v) {
    print_str(name);
    print_str(" ");
    print_hex(v);
    print_nl();
}

static unsigned short getsw(void) {
    unsigned short sw;
    __asm__ volatile("fnstsw %0" : "=m"(sw));
    return sw;
}

static void setcw(unsigned short cw) { __asm__ volatile("fldcw %0" ::"m"(cw)); }
static void clex(void) { __asm__ volatile("fnclex"); }

static volatile long double vals[] = {
    1.0L, -2.5L, 3.14159265358979323846L, 1e300L, -1e-300L, 0.1L, 1.0L / 3, 7e4000L, 123456789.0L,
    0.0L, -0.0L, 1e-4940L, 2.0L, 1e10L, -7.75L, 0.5L, 6.0L,
};
#define NV (int)(sizeof vals / sizeof vals[0])

static long double ld_inf(void) { return __builtin_infl(); }
static long double ld_nan(void) { return __builtin_nanl(""); }

static unsigned short sw_after(void) { return getsw() & 0x47FF; }

static void arith_modes(void) {
    static const unsigned short cws[] = {0x037F, 0x077F, 0x0B7F, 0x0F7F, 0x027F, 0x007F, 0x0E7F};
    for (unsigned m = 0; m < sizeof cws / sizeof cws[0]; m++) {
        setcw(cws[m]);
        unsigned long long acc = 0;
        unsigned short sws = 0;
        for (int i = 0; i < NV; i++)
            for (int j = 0; j < NV; j++) {
                long double a = vals[i], b = vals[j], r[4];
                clex();
                r[0] = a + b;
                r[1] = a - b;
                r[2] = a * b;
                r[3] = a / b;
                sws |= sw_after();
                for (int k = 0; k < 4; k++) {
                    X x;
                    unsigned char *p = (unsigned char *)&r[k], *q = (unsigned char *)&x;
                    for (int t = 0; t < 10; t++) q[t] = p[t];
                    acc = acc * 1000003 + x.sig + x.se;
                }
            }
        pu("arith-hash", acc);
        pu("arith-sw", sws);
    }
    setcw(0x037F);
}

static long double fsqrt(long double x) { long double r; __asm__ volatile("fsqrt" : "=t"(r) : "0"(x)); return r; }
static long double frndint(long double x) { long double r; __asm__ volatile("frndint" : "=t"(r) : "0"(x)); return r; }
static long double fscale(long double x, long double y) { long double r; __asm__ volatile("fscale" : "=t"(r) : "0"(x), "u"(y)); return r; }
static long double fprem(long double x, long double y, unsigned short *sw) {
    long double r;
    __asm__ volatile("1: fprem\n fnstsw %%ax\n testw $0x400, %%ax\n jnz 1b" : "=t"(r) : "0"(x), "u"(y) : "ax");
    *sw = getsw() & 0x4700;
    return r;
}
static long double fprem1(long double x, long double y, unsigned short *sw) {
    long double r;
    __asm__ volatile("1: fprem1\n fnstsw %%ax\n testw $0x400, %%ax\n jnz 1b" : "=t"(r) : "0"(x), "u"(y) : "ax");
    *sw = getsw() & 0x4700;
    return r;
}
static void fxtract(long double x, long double *e, long double *s) { __asm__ volatile("fxtract" : "=t"(*s), "=u"(*e) : "0"(x)); }
static unsigned short fxam(long double x) {
    unsigned short sw;
    __asm__ volatile("fxam\n fnstsw %0" : "=m"(sw) : "t"(x));
    return sw & 0x4700;
}

static void misc_ops(void) {
    for (int i = 0; i < NV; i++) {
        long double v = vals[i], e, s;
        clex();
        px("sqrt", fsqrt(v < 0 ? -v : v));
        long double w = v * 2.75L;
        for (int rc = 0; rc < 4; rc++) {
            setcw((unsigned short)(0x037F | (rc << 10)));
            px("rndint", frndint(w));
        }
        setcw(0x037F);
        px("scale", fscale(v, 7.9L));
        px("scale-", fscale(v, -3.2L));
        fxtract(v, &e, &s);
        px("xtract-e", e);
        px("xtract-s", s);
        pu("fxam", fxam(v));
        unsigned short sw;
        px("prem", fprem(v, 0.7L, &sw));
        pu("prem-sw", sw);
        px("prem1", fprem1(v, -1.3L, &sw));
        pu("prem1-sw", sw);
        px("prem-big", fprem(1e30L, 3.0L + i, &sw));
        pu("prem-big-sw", sw);
    }
    pu("fxam-inf", fxam(-ld_inf()));
    pu("fxam-nan", fxam(ld_nan()));
    pu("fxam-den", fxam(1e-4945L));
}

static void conversions(void) {
    static volatile double dv[] = {1.5, -0.1, 1e308, 4.9e-324, 3.0, -0.0};
    static volatile float fv[] = {1.5f, -0.1f, 3.4e38f, 1e-45f, 7.0f};
    for (unsigned i = 0; i < sizeof dv / sizeof dv[0]; i++) px("ld<-d", (long double)dv[i]);
    for (unsigned i = 0; i < sizeof fv / sizeof fv[0]; i++) px("ld<-f", (long double)fv[i]);
    for (int rc = 0; rc < 4; rc++) {
        setcw((unsigned short)(0x037F | (rc << 10)));
        for (int i = 0; i < NV; i++) {
            long double v = vals[i];
            double d;
            float f;
            long long q;
            int w;
            short h;
            clex();
            __asm__ volatile("fldt %1\n fstpl %0" : "=m"(d) : "m"(v));
            __asm__ volatile("fldt %1\n fstps %0" : "=m"(f) : "m"(v));
            unsigned short sw1 = sw_after();
            __asm__ volatile("fldt %1\n fistpll %0" : "=m"(q) : "m"(v));
            __asm__ volatile("fldt %1\n fistpl %0" : "=m"(w) : "m"(v));
            __asm__ volatile("fldt %1\n fistps %0" : "=m"(h) : "m"(v));
            unsigned short sw2 = sw_after();
            unsigned long long db, fb = 0;
            unsigned char *p = (unsigned char *)&d, *qq = (unsigned char *)&db;
            for (int t = 0; t < 8; t++) qq[t] = p[t];
            p = (unsigned char *)&f;
            qq = (unsigned char *)&fb;
            for (int t = 0; t < 4; t++) qq[t] = p[t];
            pu("to-double", db);
            pu("to-float", fb);
            pu("to-sw", sw1);
            pu("fist64", (unsigned long long)q);
            pu("fist32", (unsigned int)w);
            pu("fist16", (unsigned short)h);
            pu("fist-sw", sw2);
            __asm__ volatile("fldt %1\n fisttpll %0" : "=m"(q) : "m"(v));
            pu("fisttp", (unsigned long long)q);
        }
    }
    setcw(0x037F);
    static volatile long long iv[] = {0, 1, -1, 0x7FFFFFFFFFFFFFFFLL, -0x7FFFFFFFFFFFFFFFLL - 1, 123456789012345LL};
    for (unsigned i = 0; i < sizeof iv / sizeof iv[0]; i++) px("fild", (long double)iv[i]);
    /* precision control */
    for (int pc = 0; pc < 4; pc++) {
        setcw((unsigned short)(0x007F | (pc << 8)));
        long double a = vals[0], b = vals[6], c = vals[2], d = vals[5], r1, r2;
        __asm__ volatile("fdiv %%st(1), %%st" : "=t"(r1) : "0"(a), "u"(b));
        __asm__ volatile("faddp" : "=t"(r2) : "0"(c), "u"(d) : "st(1)");
        px("pc-div", r1);
        px("pc-add", r2);
    }
    setcw(0x037F);
}

static void bcd(void) {
    static volatile long double bv[] = {0.0L, 123456789012345678.0L, -42.5L, 9.99e17L, 1e18L, -0.0L};
    for (unsigned i = 0; i < sizeof bv / sizeof bv[0]; i++) {
        unsigned char b[10];
        long double back;
        long double v = bv[i];
        clex();
        __asm__ volatile("fldt %1\n fbstp %0" : "=m"(b) : "m"(v));
        unsigned short sw = sw_after();
        unsigned long long lo = 0;
        for (int t = 7; t >= 0; t--) lo = (lo << 8) | b[t];
        pu("bcd-lo", lo);
        pu("bcd-hi", (unsigned long long)b[8] | ((unsigned long long)b[9] << 8));
        pu("bcd-sw", sw);
        if (sw & 1) continue;
        __asm__ volatile("fbld %1\n fstpt %0" : "=m"(back) : "m"(b));
        px("fbld", back);
    }
}

static void compares(void) {
    long double nan = ld_nan();
    for (int i = 0; i < NV; i++)
        for (int j = 0; j < NV; j += 3) {
            long double a = vals[i], b = vals[j];
            unsigned short sw;
            unsigned long long fl;
            clex();
            __asm__ volatile("fldt %2\n fldt %1\n fcompp\n fnstsw %0" : "=m"(sw) : "m"(a), "m"(b));
            __asm__ volatile("fldt %2\n fldt %1\n fcomip %%st(1), %%st\n fstp %%st(0)\n pushf\n pop %0"
                             : "=r"(fl) : "m"(a), "m"(b));
            pu("fcom", sw & 0x4500);
            pu("fcomi", fl & 0x8D5);
        }
    unsigned short sw;
    clex();
    __asm__ volatile("fldt %1\n fldz\n fucompp\n fnstsw %0" : "=m"(sw) : "m"(nan));
    pu("fucom-nan", sw & 0x4501);
    clex();
    __asm__ volatile("fldt %1\n fldz\n fcompp\n fnstsw %0" : "=m"(sw) : "m"(nan));
    pu("fcom-nan", sw & 0x4501);
    clex();
    __asm__ volatile("fldz\n ftst\n fnstsw %0\n fstp %%st(0)" : "=m"(sw));
    pu("ftst", sw & 0x4500);
}

static void stack_and_state(void) {
    unsigned short sw, cw;
    unsigned char env[28];
    __asm__ volatile("fninit");
    /* overflow: 9 pushes */
    __asm__ volatile("fld1\n fld1\n fld1\n fld1\n fld1\n fld1\n fld1\n fld1\n fld1\n fnstsw %0" : "=m"(sw));
    pu("overflow-sw", sw);
    long double top;
    __asm__ volatile("fstpt %0" : "=m"(top));
    px("overflow-top", top);
    __asm__ volatile("fninit\n fnstsw %0\n fnstcw %1" : "=m"(sw), "=m"(cw));
    pu("init-sw", sw);
    pu("init-cw", cw);
    /* underflow: add with empty stack */
    __asm__ volatile("fninit\n fld1\n faddp\n fnstsw %0" : "=m"(sw));
    pu("underflow-sw", sw);
    __asm__ volatile("fstpt %0" : "=m"(top));
    px("underflow-res", top);
    /* fnstenv after some work */
    __asm__ volatile("fninit\n fldpi\n fldz\n fld1\n fnstenv %0" : "=m"(env));
    for (int i = 0; i < 12; i += 4) {
        unsigned v = env[i] | env[i + 1] << 8 | (unsigned)env[i + 2] << 16 | (unsigned)env[i + 3] << 24;
        pu("env", v);
    }
    __asm__ volatile("fldenv %0\n fninit" ::"m"(env));
    /* fsave layout */
    static unsigned char save[108];
    __asm__ volatile("fninit\n fld1\n fldl2e\n fsave %0" : "=m"(save));
    unsigned long long h = 0;
    for (int i = 0; i < 108; i++)
        if (i < 12 || i >= 28) h = h * 31 + save[i]; /* skip instruction/data pointers */
    pu("fsave-hash", h);
    __asm__ volatile("frstor %0\n fstp %%st(0)\n fstpt %1" : : "m"(save), "m"(top));
    px("frstor", top);
    /* fxsave layout (x87 part) */
    static unsigned char fx[512] __attribute__((aligned(16)));
    __asm__ volatile("fninit\n fldln2\n fld1\n fchs\n fxsave %0" : "=m"(fx));
    h = 0;
    for (int i = 0; i < 160; i++)
        if (i < 6 || i >= 24) h = h * 31 + fx[i];
    pu("fxsave-hash", h);
    __asm__ volatile("fninit\n fxrstor %0\n fstpt %1" : : "m"(fx), "m"(top));
    px("fxrstor", top);
    /* fcmov, fxch, ffree, fincstp */
    long double r;
    __asm__ volatile("fninit\n fld1\n fldz\n stc\n fcmovb %%st(1), %%st\n fstpt %0\n fstp %%st(0)" : "=m"(r));
    px("fcmovb", r);
    __asm__ volatile("fninit\n fld1\n fldpi\n fxch %%st(1)\n fstpt %0\n fstp %%st(0)" : "=m"(r));
    px("fxch", r);
    __asm__ volatile("fninit\n fld1\n fldpi\n fincstp\n fnstsw %0\n fdecstp\n fstp %%st(0)\n fstp %%st(0)" : "=m"(sw));
    pu("fincstp-sw", sw);
    __asm__ volatile("fninit");
}

static void exceptions(void) {
    unsigned short sw;
    long double z = vals[9], one = vals[0];
    __asm__ volatile("fninit\n fldt %1\n fldt %2\n fdivp\n fnstsw %0\n fstp %%st(0)" : "=m"(sw) : "m"(z), "m"(one));
    pu("zdiv-sw", sw);
    __asm__ volatile("fninit\n fldt %1\n fldt %1\n fdivp\n fnstsw %0\n fstp %%st(0)" : "=m"(sw) : "m"(z));
    pu("0/0-sw", sw);
    long double big = vals[7];
    __asm__ volatile("fninit\n fldt %1\n fldt %1\n fmulp\n fnstsw %0\n fstp %%st(0)" : "=m"(sw) : "m"(big));
    pu("ovf-sw", sw);
    long double tiny = vals[11];
    __asm__ volatile("fninit\n fldt %1\n fldt %1\n fmulp\n fnstsw %0\n fstp %%st(0)" : "=m"(sw) : "m"(tiny));
    pu("unf-sw", sw);
    /* Unmasked exceptions are not compared: Rosetta treats them as masked,
     * real hardware (and x86sim) leave the destination unchanged and set ES. */
    __asm__ volatile("fninit");
}

static void transcendentals(void) {
    static volatile long double tv[] = {0.5L, 1.0L, -0.3L, 2.0L, 0.001L, 100.0L};
    for (unsigned i = 0; i < sizeof tv / sizeof tv[0]; i++) {
        long double x = tv[i], s, c, t, one, a, l, m;
        __asm__ volatile("fsin" : "=t"(s) : "0"(x));
        __asm__ volatile("fcos" : "=t"(c) : "0"(x));
        __asm__ volatile("fptan" : "=t"(one), "=u"(t) : "0"(x));
        __asm__ volatile("fpatan" : "=t"(a) : "0"(x), "u"(1.5L) : "st(1)");
        __asm__ volatile("fyl2x" : "=t"(l) : "0"(x < 0 ? -x : x), "u"(1.25L) : "st(1)");
        __asm__ volatile("f2xm1" : "=t"(m) : "0"(x / 100));
        double d[6] = {(double)s, (double)c, (double)t, (double)a, (double)l, (double)m};
        for (int k = 0; k < 6; k++) {
            unsigned long long b;
            unsigned char *p = (unsigned char *)&d[k], *q = (unsigned char *)&b;
            for (int j = 0; j < 8; j++) q[j] = p[j];
            pu("trans", b);
        }
        px("ptan-one", one);
    }
    long double k[7];
    __asm__ volatile("fldpi" : "=t"(k[0]));
    __asm__ volatile("fldl2e" : "=t"(k[1]));
    __asm__ volatile("fldl2t" : "=t"(k[2]));
    __asm__ volatile("fldlg2" : "=t"(k[3]));
    __asm__ volatile("fldln2" : "=t"(k[4]));
    __asm__ volatile("fld1" : "=t"(k[5]));
    __asm__ volatile("fldz" : "=t"(k[6]));
    for (int i = 0; i < 7; i++) px("const", k[i]);
    setcw(0x0F7F);
    __asm__ volatile("fldpi" : "=t"(k[0]));
    __asm__ volatile("fldl2e" : "=t"(k[1]));
    setcw(0x037F);
    px("pi-rz", k[0]);
    px("l2e-rz", k[1]);
}

int main(void) {
    __asm__ volatile("fninit");
    arith_modes();
    misc_ops();
    conversions();
    bcd();
    compares();
    stack_and_state();
    exceptions();
    transcendentals();
    return 0;
}
