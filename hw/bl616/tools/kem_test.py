#!/usr/bin/env python3
"""ML-KEM on the FPGA through the PolKey (CTAPHID vendor command 0xC3), cross-checked with OpenSSL >= 3.5.
Needs the `main` bitstream with ML-KEM (caps 0x800) and the vault PIN (default VAULT_PIN of vault_test.py).
    nix-shell -p openssl 'python3.withPackages(p:[p.hidapi p.fido2 p.cryptography p.pyusb])' --run 'python3 kem_test.py'"""
import os, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(__file__))
from vault_test import Vault, VAULT_PIN

LEVELS = ((0, 512, 800, 768, 2432), (1, 768, 1184, 1088, 3616 - 1184), (2, 1024, 1568, 1568, 4768 - 1568))   # (idx, name, ek, ct, blob)


def ossl(*a, inp=None):
    return subprocess.run(["openssl", *a], input=inp, capture_output=True, check=True).stdout


def kem(v, op, lvl, data=b""):
    t0 = time.time()
    cid, cmd, r = v.dev.exchange(0x43, bytes([op, lvl]) + data, timeout=30.0)
    assert cmd == 0xC3, hex(cmd)
    r = bytes(r)
    return r[0], r[1:], time.time() - t0


v = Vault()
r = v.call(cmd="RESTART_SYSTEM"); assert r["event"] == "BOOT", r
assert v.call(cmd="VERIFY_PIN", pin=VAULT_PIN).get("event") == "PIN_OK"
with tempfile.TemporaryDirectory() as t:
    for lvl, name, ek_len, ct_len, bl_len in LEVELS:
        alg = f"ML-KEM-{name}"
        # SPKI prefix of this level
        ossl("genpkey", "-algorithm", alg, "-out", f"{t}/d.pem")
        der = ossl("pkey", "-in", f"{t}/d.pem", "-pubout", "-outform", "DER"); prefix = der[:-ek_len]
        # --- device key, OpenSSL encapsulates, device decapsulates ---
        st, d, tk = kem(v, 1, lvl); assert st == 0, f"keygen status {st}"
        blob, ek = d[:-ek_len], d[-ek_len:]; assert len(ek) == ek_len, (len(d), ek_len)
        open(f"{t}/pub.der", "wb").write(prefix + ek)
        ossl("pkeyutl", "-encap", "-pubin", "-inkey", f"{t}/pub.der", "-out", f"{t}/ct", "-secret", f"{t}/ss")
        ct = open(f"{t}/ct", "rb").read(); ss = open(f"{t}/ss", "rb").read(); assert len(ct) == ct_len, (len(ct), ct_len)
        st, K, td = kem(v, 3, lvl, blob + ct); assert st == 0 and K == ss, f"decaps(openssl ct): status {st}"
        # implicit rejection: a flipped ciphertext decapsulates to a different, stable secret
        bad = bytearray(ct); bad[5] ^= 1
        st, K1, _ = kem(v, 3, lvl, blob + bytes(bad)); st2, K2, _ = kem(v, 3, lvl, blob + bytes(bad))
        assert st == 0 and st2 == 0 and K1 == K2 and K1 != ss, "implicit rejection"
        # --- OpenSSL key, device encapsulates, OpenSSL decapsulates ---
        pub = ossl("pkey", "-in", f"{t}/d.pem", "-pubout", "-outform", "DER"); ek2 = pub[-ek_len:]
        st, d, te = kem(v, 2, lvl, ek2); assert st == 0, f"encaps status {st}"
        K, ct2 = d[:32], d[32:]; assert len(ct2) == ct_len
        open(f"{t}/ct2", "wb").write(ct2)
        assert ossl("pkeyutl", "-decap", "-inkey", f"{t}/d.pem", "-in", f"{t}/ct2") == K, "openssl decaps(device ct)"
        # --- error paths ---
        badek = bytearray(ek2); badek[0] = 0xFF; badek[1] |= 0x0F
        assert kem(v, 2, lvl, bytes(badek))[0] == 3, "modulus check"
        assert kem(v, 2, lvl, ek2[:-1])[0] == 2, "bad length"
        tb = bytearray(blob); tb[100] ^= 1
        assert kem(v, 3, lvl, bytes(tb) + ct)[0] == 3, "tampered blob"
        print(f"{alg}: keygen {tk*1000:.0f} ms, encaps {te*1000:.0f} ms, decaps {td*1000:.0f} ms; OpenSSL interop (both directions), implicit rejection, error paths: OK")
print("ALL PASS")
