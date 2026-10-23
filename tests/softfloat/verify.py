"""Verify that x87 transcendental results from fxcheck (sim build) are
correctly rounded, using mpmath at 300 bits. Reads fxcheck lines on stdin."""
import sys
from mpmath import mp, mpf, sin, cos, tan, atan2, log, floor

mp.prec = 300
P66 = mpf(0x3243F6A8885A308D3) / mpf(2) ** 64  # Intel's 66-bit pi


def dec(h):
    se, sig = int(h[:4], 16), int(h[4:], 16)
    e = se & 0x7FFF
    if e == 0x7FFF or (e and not sig >> 63):
        return None
    v = mpf(sig) * mpf(2) ** ((e if e else 1) - 16383 - 63)
    return -v if se >> 15 else v


def red(x):
    q = floor(x / (P66 / 2) + mpf(1) / 2)
    return x - q * (P66 / 2), int(q) % 4


def fsin(x):
    r, q = red(x)
    return [sin(r), cos(r), -sin(r), -cos(r)][q]


def fcos(x):
    r, q = red(x)
    return [cos(r), -sin(r), -cos(r), sin(r)][q]


def ftan(x):
    r, q = red(x)
    return tan(r) if q % 2 == 0 else -1 / tan(r)


def candidates(v, rc):
    """Correctly rounded 80-bit encoding of v under rounding control rc."""
    if v == 0:
        return None
    s = 1 if v < 0 else 0
    a = abs(v)
    e = int(floor(mp.log(a, 2)))
    for _ in range(3):
        m = a / mpf(2) ** (e - 63)
        if m >= 2 ** 64:
            e += 1
        elif m < 2 ** 63:
            e -= 1
        else:
            break
    if e + 16383 <= 0 or e + 16383 >= 0x7FFF:
        return None  # denormal or overflowing results: not checked here
    lo = int(floor(m))
    frac = m - lo
    up = {0: frac > 0.5 or (frac == 0.5 and lo & 1), 1: s and frac > 0, 2: (not s) and frac > 0, 3: False}[rc]
    lo += int(bool(up))
    if lo == 2 ** 64:
        lo, e = 2 ** 63, e + 1
    return '%04x%016x' % ((s << 15) | (e + 16383), lo)


checked = bad = 0
for line in sys.stdin:
    f = line.split()
    op, cw, a, b = f[0], int(f[1], 16), f[2], f[3]
    r1, r2 = f[5], f[6]
    rc = (cw >> 10) & 3
    x, y = dec(a), dec(b)
    if x is None or (op in ('atan', 'yl2x', 'yl2xp1') and y is None) or 'sw=04' in line:
        continue
    mags = [abs(v) for v in (x, y) if v is not None and v != 0]
    tiny = min([0] + [int(mp.log(m, 2)) for m in mags])
    if tiny < -1000:
        continue
    mp.prec = 300 + 3 * max(0, -tiny)
    try:
        if op == 'sin': want, got = fsin(x), r1
        elif op == 'cos': want, got = fcos(x), r1
        elif op == 'tan': want, got = ftan(x), r2
        elif op == 'sincos': want, got = fsin(x), r2
        elif op == 'atan': want, got = atan2(y, x), r1
        elif op == '2xm1': want, got = mpf(2) ** x - 1, r1
        elif op == 'yl2x': want, got = y * log(x, 2), r1
        elif op == 'yl2xp1': want, got = y * log(1 + x, 2), r1
    except (ValueError, ZeroDivisionError):
        continue
    if not isinstance(want, type(mpf(1))):
        continue
    if want == 0 or x == 0 or (y is not None and y == 0 and op in ('atan', 'yl2x', 'yl2xp1')):
        continue
    if abs(want) < mpf(2) ** -200 and op == 'atan':
        continue  # atan(y/x) - (y/x)^3/3 needs more precision than we use
    exp = candidates(want, rc)
    if exp is None:
        continue
    checked += 1
    if exp != got:
        bad += 1
        if bad <= 10:
            print('  MISMATCH', line.strip(), 'expected', exp)
print('softfloat transcendentals: %d/%d correctly rounded (mpmath)' % (checked - bad, checked))
sys.exit(1 if bad else 0)
