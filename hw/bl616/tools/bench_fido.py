#!/usr/bin/env python3
"""Timing of the FPGA-backed FIDO operations (needs user presence OFF: set_up_mode.py off; the FIDO PIN of fido2lib_test.py).
usage: bench_fido.py [--rsa]   (RSA-2048 key generation takes tens of seconds)"""
import os
import sys
import time

from fido2.ctap2 import Ctap2
from fido2.ctap2.pin import ClientPin, PinProtocolV2

sys.path.insert(0, os.path.dirname(__file__))
from fido2lib_test import PIN, find_device, make_cred

ALGS = [(-7, "P-256"), (-8, "Ed25519"), (-48, "ML-DSA-44"), (-49, "ML-DSA-65"), (-50, "ML-DSA-87")]
if "--rsa" in sys.argv:
    ALGS.append((-257, "RSA-2048"))

ctap = Ctap2(find_device())
proto = PinProtocolV2()
cp = ClientPin(ctap, protocol=proto)
rp = {"id": "bench.example", "name": "Bench"}
print(f"{'algorithm':10} {'MakeCredential':>15} {'GetAssertion':>13}")
for alg, name in ALGS:
    tok = cp.get_pin_token(PIN, ClientPin.PERMISSION.MAKE_CREDENTIAL, rp["id"])
    t0 = time.time()
    att = make_cred(ctap, proto, tok, b"bench-" + name.encode(), rp=rp, alg=alg)
    t_mc = time.time() - t0
    cred_id = att.auth_data.credential_data.credential_id
    tok = cp.get_pin_token(PIN, ClientPin.PERMISSION.GET_ASSERTION, rp["id"])
    cdh = os.urandom(32)
    t0 = time.time()
    ctap.get_assertion(rp["id"], cdh, [{"type": "public-key", "id": cred_id}],
                       pin_uv_param=proto.authenticate(tok, cdh), pin_uv_protocol=proto.VERSION)
    t_ga = time.time() - t0
    print(f"{name:10} {t_mc:13.2f} s {t_ga:11.2f} s", flush=True)
