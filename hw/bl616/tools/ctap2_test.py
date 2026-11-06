#!/usr/bin/env python3
"""End-to-end CTAP2 test for the PolKey FIDO2 firmware (MakeCredential/GetAssertion).

Needs the asymmetric FPGA bitstream loaded first, e.g.:
    openFPGALoader -c dirtyJtag -b tangnano20k hw/accel/build/accel_asym.fs
Run inside the repo dev shell for `cryptography`:
    nix-shell --run 'python3 hw/bl616/tools/ctap2_test.py'
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from ctaphid_test import CtapHid, find_hidraw, CTAPHID_CBOR

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, ed25519


# ---------------- minimal CBOR ----------------

def _head(major, val):
    if val < 24:
        return bytes([(major << 5) | val])
    if val < 0x100:
        return bytes([(major << 5) | 24, val])
    if val < 0x10000:
        return bytes([(major << 5) | 25]) + val.to_bytes(2, "big")
    if val < 0x100000000:
        return bytes([(major << 5) | 26]) + val.to_bytes(4, "big")
    return bytes([(major << 5) | 27]) + val.to_bytes(8, "big")


def cbor(obj):
    if isinstance(obj, bool):
        return b"\xf5" if obj else b"\xf4"
    if isinstance(obj, int):
        if obj >= 0:
            return _head(0, obj)
        return _head(1, -1 - obj)
    if isinstance(obj, bytes):
        return _head(2, len(obj)) + obj
    if isinstance(obj, str):
        b = obj.encode()
        return _head(3, len(b)) + b
    if isinstance(obj, list):
        return _head(4, len(obj)) + b"".join(cbor(v) for v in obj)
    if isinstance(obj, dict):
        items = sorted(((cbor(k), cbor(v)) for k, v in obj.items()), key=lambda kv: (len(kv[0]), kv[0]))
        return _head(5, len(items)) + b"".join(k + v for k, v in items)
    raise TypeError(type(obj))


def cbor_decode(b, o=0):
    t = b[o]
    major, ai = t >> 5, t & 0x1F
    o += 1
    if ai < 24:
        v = ai
    elif ai == 24:
        v = b[o]; o += 1
    elif ai == 25:
        v = int.from_bytes(b[o:o + 2], "big"); o += 2
    elif ai == 26:
        v = int.from_bytes(b[o:o + 4], "big"); o += 4
    elif ai == 27:
        v = int.from_bytes(b[o:o + 8], "big"); o += 8
    else:
        raise ValueError(f"bad cbor at {o}")
    if major == 0:
        return v, o
    if major == 1:
        return -1 - v, o
    if major == 2:
        return b[o:o + v], o + v
    if major == 3:
        return b[o:o + v].decode(), o + v
    if major == 4:
        out = []
        for _ in range(v):
            x, o = cbor_decode(b, o)
            out.append(x)
        return out, o
    if major == 5:
        out = {}
        for _ in range(v):
            k, o = cbor_decode(b, o)
            x, o = cbor_decode(b, o)
            out[k] = x
        return out, o
    if major == 7:
        return {20: False, 21: True, 22: None}[v], o
    raise ValueError(f"major {major}")


# ---------------- test ----------------

def main():
    path = find_hidraw()
    if not path:
        print("PolKey FIDO HID interface not found")
        return 1
    dev = CtapHid(path)
    dev.init()

    cid, cmd, r = dev.exchange(CTAPHID_CBOR, b"\x04")
    assert r[0] == 0, f"GetInfo status 0x{r[0]:02x}"
    info, _ = cbor_decode(r[1:])
    print(f"GetInfo OK  versions={info[1]} options={info[4]}")

    cdh = hashlib.sha256(b"registration-client-data").digest()

    # ---- MakeCredential (ES256) ----
    req = {
        1: cdh,
        2: {"id": "example.com", "name": "Example"},
        3: {"id": b"user-1234", "name": "alice", "displayName": "Alice"},
        4: [{"alg": -7, "type": "public-key"}],
        7: {"rk": True},
    }
    cid, cmd, r = dev.exchange(CTAPHID_CBOR, b"\x01" + cbor(req))
    assert r[0] == 0, f"MakeCredential cmd 0x{cmd:02x} status 0x{r[0]:02x} payload {r[:8].hex()}"
    mc, _ = cbor_decode(r[1:])
    assert mc[1] == "packed", f"fmt={mc[1]}"
    ad, att = mc[2], mc[3]
    rp_hash, flags = ad[:32], ad[32]
    sign_count = struct.unpack(">I", ad[33:37])[0]
    assert rp_hash == hashlib.sha256(b"example.com").digest(), "rpIdHash mismatch"
    assert flags & 0x41 == 0x41, f"flags 0x{flags:02x}"
    aaguid = ad[37:53]
    cred_len = struct.unpack(">H", ad[53:55])[0]
    cred_id = ad[55:55 + cred_len]
    cose, _ = cbor_decode(ad[55 + cred_len:])
    assert cose[1] == 2 and cose[3] == -7 and cose[-1] == 1, f"COSE {cose.keys()}"
    pub = ec.EllipticCurvePublicNumbers(
        int.from_bytes(cose[-2], "big"), int.from_bytes(cose[-3], "big"), ec.SECP256R1()).public_key()
    assert att["alg"] == -7, "attStmt alg"
    pub.verify(att["sig"], ad + cdh, ec.ECDSA(hashes.SHA256()))
    print(f"MakeCredential OK  ES256 self-attested, credId={cred_id.hex()}, aaguid={aaguid.hex()}")

    # ---- GetAssertion (allowList) ----
    cdh2 = hashlib.sha256(b"assertion-client-data").digest()
    req = {1: "example.com", 2: cdh2, 3: [{"id": cred_id, "type": "public-key"}]}
    cid, cmd, r = dev.exchange(CTAPHID_CBOR, b"\x02" + cbor(req))
    assert r[0] == 0, f"GetAssertion status 0x{r[0]:02x}"
    ga, _ = cbor_decode(r[1:])
    assert ga[1]["id"] == cred_id, "credential id mismatch"
    assert ga[4]["name"] == "alice", f"user {ga[4]}"
    ad2, sig2 = ga[2], ga[3]
    sc2 = struct.unpack(">I", ad2[33:37])[0]
    assert sc2 > sign_count, "sign counter did not advance"
    pub.verify(sig2, ad2 + cdh2, ec.ECDSA(hashes.SHA256()))
    print(f"GetAssertion OK  signCount {sign_count} -> {sc2}, signature verified")

    # ---- GetAssertion (no allowList, by rpId) ----
    req = {1: "example.com", 2: cdh2}
    cid, cmd, r = dev.exchange(CTAPHID_CBOR, b"\x02" + cbor(req))
    assert r[0] == 0, f"GetAssertion(rp) status 0x{r[0]:02x}"
    print("GetAssertion by rpId OK")

    # ---- excludeList must refuse a duplicate ----
    req = {
        1: cdh,
        2: {"id": "example.com"},
        3: {"id": b"user-1234"},
        4: [{"alg": -7, "type": "public-key"}],
        5: [{"id": cred_id, "type": "public-key"}],
    }
    cid, cmd, r = dev.exchange(CTAPHID_CBOR, b"\x01" + cbor(req))
    assert r[0] == 0x19, f"excludeList gave 0x{r[0]:02x}, want 0x19"
    print("excludeList OK (0x19 CREDENTIAL_EXCLUDED)")

    # ---- MakeCredential + GetAssertion (EdDSA) ----
    req = {
        1: cdh,
        2: {"id": "ed.example", "name": "Ed"},
        3: {"id": b"user-ed", "name": "bob"},
        4: [{"alg": -8, "type": "public-key"}],
        7: {"rk": True},
    }
    cid, cmd, r = dev.exchange(CTAPHID_CBOR, b"\x01" + cbor(req))
    assert r[0] == 0, f"MakeCredential(EdDSA) status 0x{r[0]:02x}"
    mc, _ = cbor_decode(r[1:])
    ad, att = mc[2], mc[3]
    cred_len = struct.unpack(">H", ad[53:55])[0]
    ed_cred = ad[55:55 + cred_len]
    cose, _ = cbor_decode(ad[55 + cred_len:])
    ed_pub = ed25519.Ed25519PublicKey.from_public_bytes(cose[-2])
    ed_pub.verify(att["sig"], ad + cdh)
    print("MakeCredential OK  EdDSA self-attested")

    cdh3 = hashlib.sha256(b"ed-assert").digest()
    req = {1: "ed.example", 2: cdh3, 3: [{"id": ed_cred, "type": "public-key"}]}
    cid, cmd, r = dev.exchange(CTAPHID_CBOR, b"\x02" + cbor(req))
    assert r[0] == 0, f"GetAssertion(EdDSA) status 0x{r[0]:02x}"
    ga, _ = cbor_decode(r[1:])
    ed_pub.verify(ga[3], ga[2] + cdh3)
    print("GetAssertion OK  EdDSA signature verified")

    # ---- unknown credential must fail with NO_CREDENTIALS ----
    req = {1: "nope.example", 2: cdh2}
    cid, cmd, r = dev.exchange(CTAPHID_CBOR, b"\x02" + cbor(req))
    assert r[0] == 0x2E, f"unknown rp gave 0x{r[0]:02x}, want 0x2E"
    print("unknown-credential OK (0x2E NO_CREDENTIALS)")

    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
