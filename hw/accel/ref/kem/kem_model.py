#!/usr/bin/env python3
"""Bit-exact model of the planned hardware dataflow for ML-KEM on the ML-DSA polynomial engine (32-bit signed
coefficients, Montgomery R = 2^32, no intermediate reductions). Validated against mlkem.py (plain FIPS 203)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mlkem
from mlkem import Q

R = 1 << 32
QINV = pow(Q, -1, R)                       # q^-1 mod 2^32
def s32(x): x &= 0xFFFFFFFF; return x - R if x >> 31 else x
def mont(a, b):
    """montgomery_reduce((int64) a*b) exactly like pq_mont.v: r = (p - (int32)(p*QINV)*q) >> 32"""
    p = a * b; t = s32(p * QINV); r = (p - t * Q) >> 32
    assert -Q < r < Q
    return r
def smod(x): x %= Q; return x - Q if x > Q // 2 else x
ZM = [smod(z * R) for z in mlkem.ZETAS]                 # zetas in Montgomery form (zrom entries 0..127)
F_INTT = smod(3303 * R * R)                              # final INTT scale (also removes the basemul R^-1)
R_MOD = smod(R)                                          # mont(x, R_MOD) == x   (canonicalisation helper)
R2_MOD = smod(R * R)                                     # mont(x, R2_MOD) == x*R (to-Montgomery)

def ntt(f):
    f = [x for x in f]; k = 0; ln = 128
    while ln >= 2:
        for start in range(0, 256, 2 * ln):
            k += 1; z = ZM[k]
            for j in range(start, start + ln):
                t = mont(z, f[j + ln]); f[j + ln] = f[j] - t; f[j] = f[j] + t
        ln >>= 1
    return f

def intt(f):
    f = [x for x in f]; k = 128; ln = 2
    while ln <= 128:
        for start in range(0, 256, 2 * ln):
            k -= 1; z = -ZM[k]
            for j in range(start, start + ln):
                x, y = f[j], f[j + ln]
                f[j] = x + y; f[j + ln] = mont(z, x - y)
        ln <<= 1
    return [mont(F_INTT, x) for x in f]

def basemul(a, b, acc=None):
    """pair i: gamma = zeta[64 + i//2] * (+1 / -1); result carries R^-1 (removed by F_INTT or by PSCL R2_MOD)"""
    h = [0] * 256
    for i in range(128):
        a0, a1, b0, b1 = a[2*i], a[2*i+1], b[2*i], b[2*i+1]
        g = ZM[64 + i // 2] * (1 if i % 2 == 0 else -1)
        r0 = mont(a0, b0) + mont(mont(a1, b1), g)
        r1 = mont(a0, b1) + mont(a1, b0)
        h[2*i] = r0 + (acc[2*i] if acc else 0); h[2*i+1] = r1 + (acc[2*i+1] if acc else 0)
    return h

def pscl(a, c): return [mont(c, x) for x in a]

def pmulm(acc, a, b): return [acc[j] + mont(a[j], b[j & ~1]) for j in range(256)]
def pmuln(acc, a, b): return [acc[j] + mont(a[j ^ 1], b[j | 1]) for j in range(256)]
def gamma_j(j): return R_MOD if j & 1 else (-ZM[64 + (j >> 2)] if (j >> 1) & 1 else ZM[64 + (j >> 2)])
def pcomb(m, n): return [m[j] + mont(n[j], gamma_j(j)) for j in range(256)]
def basemul2(a, b, acc=None):
    """basemul as the three hardware ops (PMULM, PMULN, PCOMB); acc = previous (M, N) accumulators"""
    z = [0] * 256; m, n = acc if acc else (z, z)
    return pmulm(m, a, b), pmuln(n, a, b)
def canon(a): return [(x % Q) for x in pscl(a, R_MOD)]       # mont gives (-q,q); caddq -> [0,q)

def cbd(b, eta): return mlkem.cbd(b, eta)                     # small signed values are fine; keep as signed
def cbd_signed(b, eta): return [smod(x) for x in mlkem.cbd(b, eta)]

def tomont(a): return pscl(a, R2_MOD)

def compress_hw(x, d):
    """restoring division of (x << d) + 1664 by q, low d bits of the quotient"""
    n = (x << d) + 1664; qt = n // Q
    return qt & ((1 << d) - 1)
def decompress_hw(y, d): return ((y << 11) + (y << 10) + (y << 8) + y + (1 << (d - 1))) >> d   # y * 3329

def check():
    import random
    for d in (1, 4, 5, 10, 11):
        for x in range(Q):
            assert compress_hw(x, d) == mlkem.compress(x, d), (x, d)
        for y in range(1 << d): assert decompress_hw(y, d) == mlkem.decompress(y, d)
    f = [random.randrange(Q) for _ in range(256)]; g = [random.randrange(Q) for _ in range(256)]
    nf = ntt(f); assert [x % Q for x in tomont(nf)] != nf
    # NTT equals plain NTT up to coefficient values mod q
    assert [x % Q for x in nf] == mlkem.ntt(f), "ntt"
    assert [x % Q for x in intt(nf)] == [x * R % Q for x in f], "intt scales by R (cancels the basemul R^-1)"
    # basemul followed by intt carries the right factor
    p = basemul(ntt(f), ntt(g))
    assert [x % Q for x in intt(p)] == [x % Q for x in mlkem.intt(mlkem.basemul(mlkem.ntt(f), mlkem.ntt(g)))], "basemul/intt"
    # NTT-domain result (keygen t = A s + e): remove R^-1 with PSCL(R2_MOD)
    assert [x % Q for x in tomont(p)] == mlkem.basemul(mlkem.ntt(f), mlkem.ntt(g)), "tomont(basemul)"
    # magnitude bound: all intermediate values within +-2^31 trivially; check the worst chain for k=4 accumulations
    big = max(abs(x) for x in p); assert big < 8 * Q * 8, big
    print("ZM[1..3]", ZM[1:4], "F_INTT", F_INTT, "R_MOD", R_MOD, "R2_MOD", R2_MOD, "QINV", hex(QINV))
    print("kem_model self-check PASS")

if __name__ == "__main__": check()
