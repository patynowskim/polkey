#!/usr/bin/env python3
"""Hardware gate for an FPGA bitstream (loaded already): ECDH repeatability + MakeCredential/GetAssertion for every
algorithm. Prints one line: PASS/FAIL and the details. User presence must be OFF (set_up_mode.py off).
usage: gate.py [--rsa] [--ecdh N]"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from fido2.ctap2 import Ctap2
from fido2.ctap2.pin import ClientPin, PinProtocolV2

from fido2lib_test import PIN, find_device, make_cred
from vault_test import VaultUsb

n_ecdh = int(sys.argv[sys.argv.index("--ecdh") + 1]) if "--ecdh" in sys.argv else 15
res = {}
v = VaultUsb()
ok = 0
for _ in range(n_ecdh):
    d = v.call(cmd="BENCH")["data"]
    ok += bool(d["genkey_ok"] and d["ecdh_ok"])
res["ecdh"] = f"{ok}/{n_ecdh}"
good = ok == n_ecdh

try:
    ctap = Ctap2(find_device())
    proto = PinProtocolV2()
    cp = ClientPin(ctap, protocol=proto)
    rp = {"id": "gate.example", "name": "Gate"}
    algs = [(-7, "P256"), (-8, "Ed25519"), (-48, "MLDSA44"), (-49, "MLDSA65"), (-50, "MLDSA87")]
    if "--rsa" in sys.argv:
        algs.append((-257, "RSA"))
    for alg, name in algs:
        try:
            tok = cp.get_pin_token(PIN, ClientPin.PERMISSION.MAKE_CREDENTIAL, rp["id"])
            t0 = time.time()
            att = make_cred(ctap, proto, tok, b"gate-" + name.encode(), rp=rp, alg=alg)
            t_mc = time.time() - t0
            cid = att.auth_data.credential_data.credential_id
            tok = cp.get_pin_token(PIN, ClientPin.PERMISSION.GET_ASSERTION, rp["id"])
            cdh = os.urandom(32)
            t0 = time.time()
            ctap.get_assertion(rp["id"], cdh, [{"type": "public-key", "id": cid}],
                               pin_uv_param=proto.authenticate(tok, cdh), pin_uv_protocol=proto.VERSION)
            res[name] = f"ok {t_mc:.2f}/{time.time() - t0:.2f}s"
        except Exception as e:  # noqa: BLE001
            res[name] = f"FAIL {str(e)[:30]}"
            good = False
except Exception as e:  # noqa: BLE001
    res["fido"] = f"FAIL {str(e)[:40]}"
    good = False
print(("PASS " if good else "FAIL ") + " ".join(f"{k}={v}" for k, v in res.items()))
