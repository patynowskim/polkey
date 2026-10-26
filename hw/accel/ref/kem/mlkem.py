#!/usr/bin/env python3
"""Plain FIPS 203 (ML-KEM) reference, straight from the specification. Used to validate the hardware dataflow model
(kem_model.py) and the RTL; cross-checked against OpenSSL (see check_openssl.sh)."""
import hashlib

Q = 3329
PARAMS = {512: (2, 3, 2, 10, 4), 768: (3, 2, 2, 10, 4), 1024: (4, 2, 2, 11, 5)}   # k, eta1, eta2, du, dv
PARAMS[512] = (2, 3, 2, 10, 4)

def _br7(x): return int(format(x, "07b")[::-1], 2)
ZETAS = [pow(17, _br7(i), Q) for i in range(128)]
GAMMAS = [pow(17, 2 * _br7(i) + 1, Q) for i in range(128)]

def H(x): return hashlib.sha3_256(x).digest()
def G(x): d = hashlib.sha3_512(x).digest(); return d[:32], d[32:]
def J(x): return hashlib.shake_256(x).digest(32)
def PRF(eta, s, b): return hashlib.shake_256(s + bytes([b])).digest(64 * eta)

def ntt(f):
    f = list(f); i = 1; ln = 128
    while ln >= 2:
        for start in range(0, 256, 2 * ln):
            z = ZETAS[i]; i += 1
            for j in range(start, start + ln):
                t = z * f[j + ln] % Q
                f[j + ln] = (f[j] - t) % Q; f[j] = (f[j] + t) % Q
        ln //= 2
    return f

def intt(f):
    f = list(f); i = 127; ln = 2
    while ln <= 128:
        for start in range(0, 256, 2 * ln):
            z = ZETAS[i]; i -= 1
            for j in range(start, start + ln):
                t = f[j]
                f[j] = (t + f[j + ln]) % Q
                f[j + ln] = z * (f[j + ln] - t) % Q
        ln *= 2
    return [x * 3303 % Q for x in f]            # 128^-1 mod q

def basemul(a, b):
    h = [0] * 256
    for i in range(128):
        a0, a1, b0, b1, g = a[2*i], a[2*i+1], b[2*i], b[2*i+1], GAMMAS[i]
        h[2*i] = (a0 * b0 + a1 * b1 % Q * g) % Q
        h[2*i+1] = (a0 * b1 + a1 * b0) % Q
    return h

def sample_ntt(seed34):
    xof = hashlib.shake_128(seed34).digest(3 * 700)
    a = []; p = 0
    while len(a) < 256:
        d1 = xof[p] + 256 * (xof[p+1] & 15); d2 = (xof[p+1] >> 4) + 16 * xof[p+2]; p += 3
        if d1 < Q: a.append(d1)
        if d2 < Q and len(a) < 256: a.append(d2)
    return a

def cbd(b, eta):
    bits = [(b[i // 8] >> (i % 8)) & 1 for i in range(len(b) * 8)]
    f = []
    for i in range(256):
        x = sum(bits[2*i*eta + j] for j in range(eta)); y = sum(bits[2*i*eta + eta + j] for j in range(eta))
        f.append((x - y) % Q)
    return f

def byte_encode(f, d):
    v = 0
    for i, c in enumerate(f): v |= (c % (1 << d if d < 12 else Q)) << (d * i) if d < 12 else c << (12 * i)
    return v.to_bytes(32 * d, "little")

def byte_decode(b, d):
    v = int.from_bytes(b, "little"); m = (1 << d) - 1
    f = [(v >> (d * i)) & m for i in range(256)]
    return [x % Q for x in f] if d == 12 else f

def compress(x, d): return ((x << d) + Q // 2) // Q % (1 << d)
def decompress(y, d): return (y * Q + (1 << (d - 1))) >> d

def k_pke_keygen(d, k, eta1):
    rho, sigma = G(d + bytes([k]))
    A = [[sample_ntt(rho + bytes([j, i])) for j in range(k)] for i in range(k)]
    n = 0; s = []; e = []
    for i in range(k): s.append(cbd(PRF(eta1, sigma, n), eta1)); n += 1
    for i in range(k): e.append(cbd(PRF(eta1, sigma, n), eta1)); n += 1
    s = [ntt(x) for x in s]; e = [ntt(x) for x in e]
    t = []
    for i in range(k):
        acc = [0] * 256
        for j in range(k): acc = [(x + y) % Q for x, y in zip(acc, basemul(A[i][j], s[j]))]
        t.append([(x + y) % Q for x, y in zip(acc, e[i])])
    ek = b"".join(byte_encode(x, 12) for x in t) + rho
    dk = b"".join(byte_encode(x, 12) for x in s)
    return ek, dk

def k_pke_encrypt(ek, m, r, k, eta1, eta2, du, dv):
    t = [byte_decode(ek[384 * i:384 * (i + 1)], 12) for i in range(k)]; rho = ek[384 * k:]
    A = [[sample_ntt(rho + bytes([i, j])) for j in range(k)] for i in range(k)]      # transposed indices
    n = 0; y = []; e1 = []
    for i in range(k): y.append(cbd(PRF(eta1, r, n), eta1)); n += 1
    for i in range(k): e1.append(cbd(PRF(eta2, r, n), eta2)); n += 1
    e2 = cbd(PRF(eta2, r, n), eta2)
    y = [ntt(x) for x in y]
    u = []
    for i in range(k):
        acc = [0] * 256
        for j in range(k): acc = [(a + b) % Q for a, b in zip(acc, basemul(A[i][j], y[j]))]
        u.append([(a + b) % Q for a, b in zip(intt(acc), e1[i])])
    mu = [decompress(x, 1) for x in byte_decode(m, 1)]
    acc = [0] * 256
    for j in range(k): acc = [(a + b) % Q for a, b in zip(acc, basemul(t[j], y[j]))]
    v = [(a + b + c) % Q for a, b, c in zip(intt(acc), e2, mu)]
    c1 = b"".join(byte_encode([compress(x, du) for x in p], du) for p in u)
    c2 = byte_encode([compress(x, dv) for x in v], dv)
    return c1 + c2

def k_pke_decrypt(dk, c, k, du, dv):
    c1 = c[:32 * du * k]; c2 = c[32 * du * k:]
    u = [[decompress(x, du) for x in byte_decode(c1[32 * du * i:32 * du * (i + 1)], du)] for i in range(k)]
    v = [decompress(x, dv) for x in byte_decode(c2, dv)]
    s = [byte_decode(dk[384 * i:384 * (i + 1)], 12) for i in range(k)]
    acc = [0] * 256
    for j in range(k): acc = [(a + b) % Q for a, b in zip(acc, basemul(s[j], ntt(u[j])))]
    w = [(a - b) % Q for a, b in zip(v, intt(acc))]
    return byte_encode([compress(x, 1) for x in w], 1)

def keygen(d, z, level):
    k, e1, e2, du, dv = PARAMS[level]
    ek, dkp = k_pke_keygen(d, k, e1)
    return ek, dkp + ek + H(ek) + z

def encaps(ek, m, level):
    k, e1, e2, du, dv = PARAMS[level]
    K, r = G(m + H(ek))
    return K, k_pke_encrypt(ek, m, r, k, e1, e2, du, dv)

def decaps(dk, c, level):
    k, e1, e2, du, dv = PARAMS[level]
    dkp = dk[:384 * k]; ek = dk[384 * k:768 * k + 32]; h = dk[768 * k + 32:768 * k + 64]; z = dk[768 * k + 64:]
    m2 = k_pke_decrypt(dkp, c, k, du, dv)
    K2, r2 = G(m2 + h)
    Kbar = J(z + c)
    c2 = k_pke_encrypt(ek, m2, r2, k, e1, e2, du, dv)
    return K2 if c2 == c else Kbar
