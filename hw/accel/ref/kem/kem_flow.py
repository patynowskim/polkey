#!/usr/bin/env python3
"""ML-KEM expressed with exactly the hardware ops of kem_model.py (NTT, INTT, basemul(+acc), PSCL, canon, compress)
- the microcode programs follow this structure. Checked against mlkem.py."""
import os, sys, hashlib, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mlkem
import kem_model as M
from mlkem import Q, PARAMS, H, G, J, PRF

def padd(a, b): return [x + y for x, y in zip(a, b)]
def psub(a, b): return [x - y for x, y in zip(a, b)]
MAXV = [0]
def track(a):
    m = max(abs(x) for x in a); MAXV[0] = max(MAXV[0], m); assert m < 2**30; return a

def matvec_row(A_row_hat, vec_hat):
    acc = None
    for a, v in zip(A_row_hat, vec_hat): acc = M.basemul2(a, v, acc); track(acc[0]); track(acc[1])
    return M.pcomb(*acc)

def pke_keygen(d, k, eta1):
    rho, sigma = G(d + bytes([k]))
    n = 0; s = []; e = []
    for _ in range(k): s.append(M.ntt(M.cbd_signed(PRF(eta1, sigma, n), eta1))); n += 1
    for _ in range(k): e.append(M.ntt(M.cbd_signed(PRF(eta1, sigma, n), eta1))); n += 1
    t = []
    for i in range(k):
        row = [mlkem.sample_ntt(rho + bytes([j, i])) for j in range(k)]
        acc = matvec_row(row, s)
        t.append(M.canon(padd(M.tomont(acc), e[i])))              # t = A s + e  (tomont removes the basemul R^-1)
    ek = b"".join(mlkem.byte_encode(x, 12) for x in t) + rho
    dk = b"".join(mlkem.byte_encode(M.canon(x), 12) for x in s)
    return ek, dk

def pke_encrypt(ek, m, r, k, eta1, eta2, du, dv):
    t = [mlkem.byte_decode(ek[384 * i:384 * (i + 1)], 12) for i in range(k)]; rho = ek[384 * k:]
    n = 0; y = []; e1 = []
    for _ in range(k): y.append(M.ntt(M.cbd_signed(PRF(eta1, r, n), eta1))); n += 1
    for _ in range(k): e1.append(M.cbd_signed(PRF(eta2, r, n), eta2)); n += 1
    e2 = M.cbd_signed(PRF(eta2, r, n), eta2)
    c1 = b""
    for i in range(k):
        row = [mlkem.sample_ntt(rho + bytes([i, j])) for j in range(k)]
        u = padd(M.intt(matvec_row(row, y)), e1[i])
        c1 += mlkem.byte_encode([M.compress_hw(x, du) for x in M.canon(u)], du)
    mu = [mlkem.decompress(b, 1) for b in mlkem.byte_decode(m, 1)]
    v = padd(padd(M.intt(matvec_row(t, y)), e2), mu)
    c2 = mlkem.byte_encode([M.compress_hw(x, dv) for x in M.canon(v)], dv)
    return c1 + c2

def pke_decrypt(dk, c, k, du, dv):
    c1 = c[:32 * du * k]; c2 = c[32 * du * k:]
    u = [[M.decompress_hw(x, du) for x in mlkem.byte_decode(c1[32 * du * i:32 * du * (i + 1)], du)] for i in range(k)]
    v = [M.decompress_hw(x, dv) for x in mlkem.byte_decode(c2, dv)]
    s = [mlkem.byte_decode(dk[384 * i:384 * (i + 1)], 12) for i in range(k)]
    acc = matvec_row(s, [M.ntt(x) for x in u])
    w = M.canon(psub(v, M.intt(acc)))
    return mlkem.byte_encode([M.compress_hw(x, 1) for x in w], 1)

def keygen(d, z, level):
    k, e1, e2, du, dv = PARAMS[level]; ek, dkp = pke_keygen(d, k, e1); return ek, dkp + ek + H(ek) + z
def encaps(ek, m, level):
    k, e1, e2, du, dv = PARAMS[level]; K, r = G(m + H(ek)); return K, pke_encrypt(ek, m, r, k, e1, e2, du, dv)
def decaps(dk, c, level):
    k, e1, e2, du, dv = PARAMS[level]
    dkp = dk[:384 * k]; ek = dk[384 * k:768 * k + 32]; h = dk[768 * k + 32:768 * k + 64]; z = dk[768 * k + 64:]
    m2 = pke_decrypt(dkp, c, k, du, dv); K2, r2 = G(m2 + h); Kbar = J(z + c)
    return K2 if pke_encrypt(ek, m2, r2, k, e1, e2, du, dv) == c else Kbar

if __name__ == "__main__":
    for level in (512, 768, 1024):
        for _ in range(3):
            d, z, m = os.urandom(32), os.urandom(32), os.urandom(32)
            ek, dk = keygen(d, z, level); assert (ek, dk) == mlkem.keygen(d, z, level), "keygen"
            K, c = encaps(ek, m, level); assert (K, c) == mlkem.encaps(ek, m, level), "encaps"
            assert decaps(dk, c, level) == K, "decaps"
            bad = bytearray(c); bad[7] ^= 4; assert decaps(dk, bytes(bad), level) == mlkem.decaps(dk, bytes(bad), level)
        print(f"ML-KEM-{level}: hardware-op flow == FIPS 203 reference")
    print("max |coefficient| seen:", MAXV[0], "(limit 2^30)"); print("ALL PASS")
