#!/usr/bin/env python3
"""Cross-check mlkem.py against OpenSSL >= 3.5 (deterministic keygen via hexseed; encaps/decaps interop)."""
import os, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mlkem

def sh(*a, inp=None): return subprocess.run(a, input=inp, capture_output=True, check=True).stdout

for level in (512, 768, 1024):
    d, z = os.urandom(32), os.urandom(32)
    ek, dk = mlkem.keygen(d, z, level)
    with tempfile.TemporaryDirectory() as t:
        key = f"{t}/k.pem"; pub = f"{t}/pub.der"; ct = f"{t}/ct"; ss = f"{t}/ss"
        sh("openssl", "genpkey", "-algorithm", f"ML-KEM-{level}", "-pkeyopt", f"hexseed:{(d + z).hex()}", "-out", key)
        sh("openssl", "pkey", "-in", key, "-pubout", "-outform", "DER", "-out", pub)
        der = open(pub, "rb").read()
        assert der.endswith(ek), "ek mismatch"
        # OpenSSL encapsulates to our ek; we decapsulate
        sh("openssl", "pkeyutl", "-encap", "-pubin", "-inkey", pub, "-out", ct, "-secret", ss)
        c = open(ct, "rb").read(); s_ossl = open(ss, "rb").read()
        assert mlkem.decaps(dk, c, level) == s_ossl, "decaps(openssl ct) mismatch"
        # we encapsulate; OpenSSL decapsulates
        K, c2 = mlkem.encaps(ek, os.urandom(32), level)
        open(ct, "wb").write(c2)
        s2 = sh("openssl", "pkeyutl", "-decap", "-inkey", key, "-in", ct)
        assert s2 == K, "openssl decaps(our ct) mismatch"
        # implicit rejection
        bad = bytearray(c2); bad[5] ^= 1; open(ct, "wb").write(bytes(bad))
        assert sh("openssl", "pkeyutl", "-decap", "-inkey", key, "-in", ct) == mlkem.decaps(dk, bytes(bad), level)
    print(f"ML-KEM-{level}: keygen / encaps / decaps / implicit rejection match OpenSSL")
print("ALL PASS")
