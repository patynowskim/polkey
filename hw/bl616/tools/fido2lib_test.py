#!/usr/bin/env python3
"""ClientPIN / hmac-secret / credProtect / GetNextAssertion test of the PolKey firmware using the python-fido2
library as an independent platform implementation.

Needs the asym FPGA bitstream (ECDH key agreement runs on the FPGA):
    openFPGALoader -c dirtyJtag -b tangnano20k hw/accel/build/accel_asym.fs
    nix-shell -p 'python3.withPackages(p:[p.fido2 p.cryptography p.hidapi])' --run 'python3 hw/bl616/tools/fido2lib_test.py'

The PIN persists in the key's flash. If one is already set it must be PIN below (replug and send authenticatorReset
within 10 s of power-up to start from scratch).
"""
import hashlib
import os
import subprocess
import sys
import time

from fido2.cose import CoseKey
from fido2.ctap import CtapError
from fido2.ctap1 import ApduError, Ctap1
from fido2.ctap2.blob import LargeBlobs
from fido2.ctap2.credman import CredentialManagement
from fido2.ctap2 import Ctap2
from fido2.ctap2.pin import ClientPin, PinProtocolV1, PinProtocolV2
from fido2.hid import CtapHidDevice

PIN = "PolKey-1234"
NEW_PIN = "PolKey-5678"
RP = {"id": "example.com", "name": "Example"}


def find_device():
    for d in CtapHidDevice.list_devices():
        if d.descriptor.vid == 0x1209 and d.descriptor.pid == 0xC0CA:
            return d
    sys.exit("PolKey not found")


def expect(err_code, fn, *a, **kw):
    try:
        fn(*a, **kw)
    except CtapError as e:
        assert e.code == err_code, f"expected {err_code!r}, got {e.code!r}"
        return
    raise AssertionError(f"expected {err_code!r}, call succeeded")


def token(cp, perms, rp_id=None, pin=PIN):
    return cp.get_pin_token(pin, perms, rp_id)


def make_cred(ctap, proto, tok, user_id, ext=None, rp=RP, alg=-7):
    cdh = os.urandom(32)
    att = ctap.make_credential(
        cdh, rp, {"id": user_id, "name": user_id.decode()}, [{"type": "public-key", "alg": alg}],
        extensions=ext, options={"rk": True},
        pin_uv_param=proto.authenticate(tok, cdh), pin_uv_protocol=proto.VERSION)
    return att


def hmac_ext(ctap, proto, salts):
    peer = ctap.client_pin(proto.VERSION, ClientPin.CMD.GET_KEY_AGREEMENT)[ClientPin.RESULT.KEY_AGREEMENT]
    key_agreement, shared = proto.encapsulate(peer)
    salt_enc = proto.encrypt(shared, b"".join(salts))
    salt_auth = proto.authenticate(shared, salt_enc)
    return {"hmac-secret": {1: key_agreement, 2: salt_enc, 3: salt_auth, 4: proto.VERSION}}, shared


def run(ctap, proto):
    name = f"protocol {proto.VERSION}"
    cp = ClientPin(ctap, protocol=proto)
    info = ctap.get_info()
    assert info.options["clientPin"] is True
    assert proto.VERSION in info.pin_uv_protocols
    print(f"[{name}] retries = {cp.get_pin_retries()[0]}")

    # --- no pinUvAuthParam on a PIN-protected key
    cdh = os.urandom(32)
    expect(CtapError.ERR.PUAT_REQUIRED, ctap.make_credential, cdh, RP, {"id": b"u0", "name": "u0"},
           [{"type": "public-key", "alg": -7}], options={"rk": True})
    print(f"[{name}] PUAT_REQUIRED without PIN auth: OK")

    # --- wrong PIN decrements, correct PIN restores
    before = cp.get_pin_retries()[0]
    expect(CtapError.ERR.PIN_INVALID, cp.get_pin_token, "definitely-wrong")
    assert cp.get_pin_retries()[0] == before - 1
    token(cp, ClientPin.PERMISSION.MAKE_CREDENTIAL, "example.com")
    assert cp.get_pin_retries()[0] == 8
    print(f"[{name}] wrong PIN -> PIN_INVALID, retries {before}->{before - 1}->8: OK")

    # --- MakeCredential with hmac-secret + credProtect 2
    tok = token(cp, ClientPin.PERMISSION.MAKE_CREDENTIAL, "example.com")
    att = make_cred(ctap, proto, tok, b"alice", {"hmac-secret": True, "credProtect": 2})
    ad = att.auth_data
    assert ad.flags & 0x04, "UV flag not set"
    assert ad.extensions == {"credProtect": 2, "hmac-secret": True}, ad.extensions
    cred_id = ad.credential_data.credential_id
    pub = CoseKey.parse(ad.credential_data.public_key)
    print(f"[{name}] MakeCredential with PIN + hmac-secret + credProtect: OK")

    # token is single-use
    expect(CtapError.ERR.PIN_AUTH_INVALID, ctap.make_credential, cdh, RP, {"id": b"u1", "name": "u1"},
           [{"type": "public-key", "alg": -7}], options={"rk": True},
           pin_uv_param=proto.authenticate(tok, cdh), pin_uv_protocol=proto.VERSION)
    print(f"[{name}] token is single use: OK")

    # --- GetAssertion with allowList + hmac-secret
    salt1, salt2 = os.urandom(32), os.urandom(32)
    outs = []
    for salts in ([salt1], [salt1], [salt1, salt2]):
        tok = token(cp, ClientPin.PERMISSION.GET_ASSERTION, "example.com")
        ext, shared = hmac_ext(ctap, proto, salts)
        cdh = os.urandom(32)
        a = ctap.get_assertion("example.com", cdh, [{"type": "public-key", "id": cred_id}], extensions=ext,
                               pin_uv_param=proto.authenticate(tok, cdh), pin_uv_protocol=proto.VERSION)
        assert a.auth_data.flags & 0x04 and a.auth_data.flags & 0x80
        pub.verify(a.auth_data + cdh, a.signature)
        out = proto.decrypt(shared, a.auth_data.extensions["hmac-secret"])
        assert len(out) == 32 * len(salts)
        outs.append(out)
    assert outs[0] == outs[1] and outs[2][:32] == outs[0] and outs[2][32:] != outs[0]
    print(f"[{name}] GetAssertion + hmac-secret (1 and 2 salts, deterministic, signature verified): OK")

    # --- credProtect 2: without PIN and without allowList the credential is hidden; with allowList it is usable
    cdh = os.urandom(32)
    a = ctap.get_assertion("example.com", cdh, [{"type": "public-key", "id": cred_id}])
    pub.verify(a.auth_data + cdh, a.signature)
    assert not a.auth_data.flags & 0x04
    expect(CtapError.ERR.PUAT_REQUIRED, ctap.get_assertion, "example.com", cdh)
    print(f"[{name}] credProtect 2 policy: OK")

    # --- two credentials for the rp -> numberOfCredentials + GetNextAssertion
    tok = token(cp, ClientPin.PERMISSION.MAKE_CREDENTIAL, "example.com")
    make_cred(ctap, proto, tok, b"bob")
    tok = token(cp, ClientPin.PERMISSION.GET_ASSERTION, "example.com")
    cdh = os.urandom(32)
    a1 = ctap.get_assertion("example.com", cdh, pin_uv_param=proto.authenticate(tok, cdh), pin_uv_protocol=proto.VERSION)
    n = a1.number_of_credentials
    assert n and n >= 2, n
    users = {bytes(a1.user["id"])}
    for _ in range(n - 1):
        users.add(bytes(ctap.get_next_assertion().user["id"]))
    assert len(users) == n
    expect(CtapError.ERR.NOT_ALLOWED, ctap.get_next_assertion)
    print(f"[{name}] numberOfCredentials={n}, GetNextAssertion: OK ")


VERIFY = os.path.join(os.path.dirname(__file__), "..", "..", "accel", "ref", "pq", "verify")
ALGS = {-257: "RS256", -48: "ML-DSA-44", -49: "ML-DSA-65", -50: "ML-DSA-87"}


def run_algs(ctap, proto, which):
    """MakeCredential + GetAssertion with RSA / ML-DSA credentials (keys generated and used inside the FPGA)."""
    cp = ClientPin(ctap, protocol=proto)
    rp = {"id": "alg.example", "name": "Alg"}
    for alg in which:
        name = ALGS[alg]
        tok = token(cp, ClientPin.PERMISSION.MAKE_CREDENTIAL, "alg.example")
        t0 = time.time()
        att = make_cred(ctap, proto, tok, b"user-" + name.encode(), rp=rp, alg=alg)
        dt = time.time() - t0
        cd = att.auth_data.credential_data
        pk = dict(cd.public_key)
        assert pk[3] == alg
        # self attestation: signature over authData || clientDataHash was produced by the same key
        print(f"[{name}] MakeCredential {dt:.1f}s, attestation sig {len(att.att_stmt['sig'])} bytes")
        tok = token(cp, ClientPin.PERMISSION.GET_ASSERTION, "alg.example")
        cdh = os.urandom(32)
        t0 = time.time()
        a = ctap.get_assertion("alg.example", cdh, [{"type": "public-key", "id": cd.credential_id}],
                               pin_uv_param=proto.authenticate(tok, cdh), pin_uv_protocol=proto.VERSION)
        dt = time.time() - t0
        msg = bytes(a.auth_data) + cdh
        if alg == -257:
            CoseKey.parse(pk).verify(msg, a.signature)
        else:
            ps = {-48: 44, -49: 65, -50: 87}[alg]
            r = subprocess.run([VERIFY, str(ps), bytes(pk[-1]).hex(), bytes(a.signature).hex(), msg.hex()],
                               capture_output=True, text=True)
            assert r.stdout.strip() == "OK", r.stdout + r.stderr
        print(f"[{name}] GetAssertion {dt:.1f}s, signature verified: OK")


def run_extras(dev, ctap, proto):
    """credBlob, largeBlobKey + large blob array, credential management, U2F."""
    cp = ClientPin(ctap, protocol=proto)
    rp = {"id": "extras.example", "name": "Extras"}
    info = ctap.get_info()
    assert info.options["credMgmt"] and info.options["largeBlobs"]
    assert info.max_cred_blob_length == 32 and info.max_large_blob == 4096, (info.max_cred_blob_length, info.max_large_blob)

    # --- credBlob + largeBlobKey
    tok = token(cp, ClientPin.PERMISSION.MAKE_CREDENTIAL, "extras.example")
    att = make_cred(ctap, proto, tok, b"carol", {"credBlob": b"hello-blob", "largeBlobKey": True}, rp=rp)
    assert att.auth_data.extensions.get("credBlob") is True, att.auth_data.extensions
    lbk = att.large_blob_key
    assert lbk and len(lbk) == 32
    cred_id = att.auth_data.credential_data.credential_id
    tok = token(cp, ClientPin.PERMISSION.GET_ASSERTION, "extras.example")
    cdh = os.urandom(32)
    a = ctap.get_assertion("extras.example", cdh, [{"type": "public-key", "id": cred_id}],
                           extensions={"credBlob": True, "largeBlobKey": True},
                           pin_uv_param=proto.authenticate(tok, cdh), pin_uv_protocol=proto.VERSION)
    assert a.auth_data.extensions["credBlob"] == b"hello-blob", a.auth_data.extensions
    assert a.large_blob_key == lbk
    print("credBlob + largeBlobKey: OK")

    # --- large blob array
    tok = token(cp, ClientPin.PERMISSION.LARGE_BLOB_WRITE)
    lb = LargeBlobs(ctap, proto, tok)
    lb.write_blob_array([])  # start from an empty array (earlier runs leave entries behind)
    data = b"large blob payload " * 40
    lb.put_blob(lbk, data)
    assert lb.get_blob(lbk) == data
    big = os.urandom(1500)
    lb.put_blob(os.urandom(32), big)       # a second entry, array now spans several fragments
    assert lb.get_blob(lbk) == data
    print(f"large blob array ({len(data)} + {len(big)} bytes, multi-fragment): OK")

    # --- credential management
    tok = token(cp, ClientPin.PERMISSION.CREDENTIAL_MGMT)
    cm = CredentialManagement(ctap, proto, tok)
    meta = cm.get_metadata()
    print("credMgmt metadata:", dict(meta))
    rps = cm.enumerate_rps()
    ids = {r[CredentialManagement.RESULT.RP]["id"]: r[CredentialManagement.RESULT.RP_ID_HASH] for r in rps}
    assert "extras.example" in ids and "example.com" in ids, list(ids)
    creds = cm.enumerate_creds(ids["extras.example"])
    assert len(creds) == 1
    c = creds[0]
    assert c[CredentialManagement.RESULT.CREDENTIAL_ID]["id"] == cred_id
    assert c[CredentialManagement.RESULT.USER]["id"] == b"carol"
    CoseKey.parse(c[CredentialManagement.RESULT.PUBLIC_KEY])
    cm.update_user_info({"type": "public-key", "id": cred_id}, {"id": b"carol", "name": "carol-renamed"})
    creds = cm.enumerate_creds(ids["extras.example"])
    assert creds[0][CredentialManagement.RESULT.USER]["name"] == "carol-renamed"
    cm.delete_cred({"type": "public-key", "id": cred_id})
    assert list(cm.enumerate_creds(ids["extras.example"])) == []
    print(f"credMgmt: metadata, {len(rps)} RPs, enumerate, update user, delete: OK")

    # --- U2F
    u2f = Ctap1(dev)
    assert u2f.get_version() == "U2F_V2"
    app, chal = os.urandom(32), os.urandom(32)
    reg = u2f.register(chal, app)
    reg.verify(app, chal)
    sig = u2f.authenticate(chal, app, reg.key_handle)
    sig.verify(app, chal, reg.public_key)
    assert sig.counter == 1 and sig.user_presence & 1
    sig = u2f.authenticate(chal, app, reg.key_handle)
    assert sig.counter == 2
    try:
        u2f.authenticate(chal, app, reg.key_handle, check_only=True)
    except ApduError as e:
        assert e.code == 0x6985, hex(e.code)
    try:
        u2f.authenticate(chal, os.urandom(32), reg.key_handle)
        raise AssertionError("foreign app id accepted")
    except ApduError as e:
        assert e.code == 0x6A80, hex(e.code)
    print("U2F register / authenticate / check-only / wrong app id: OK")


def main():
    dev = find_device()
    ctap = Ctap2(dev)
    info = ctap.get_info()
    print("extensions:", info.extensions, "protocols:", info.pin_uv_protocols, "options:", dict(info.options))
    if not info.options["clientPin"]:
        ClientPin(ctap, protocol=PinProtocolV2()).set_pin(PIN)
        print("setPIN OK")
    for proto in (PinProtocolV2(), PinProtocolV1()):
        run(ctap, proto)
    if "--extras" in sys.argv:
        run_extras(dev, ctap, PinProtocolV2())
    if "--algs" in sys.argv:  # RSA key generation takes tens of seconds
        run_algs(ctap, PinProtocolV2(), [-48, -49, -50, -257])

    # --- changePIN and back
    cp = ClientPin(ctap, protocol=PinProtocolV2())
    cp.change_pin(PIN, NEW_PIN)
    expect(CtapError.ERR.PIN_INVALID, cp.get_pin_token, PIN)
    cp.get_pin_token(NEW_PIN)
    cp.change_pin(NEW_PIN, PIN)
    cp.get_pin_token(PIN)
    print("changePIN round trip: OK")
    # the library refuses short PINs client-side, so build the request by hand
    proto = cp.protocol
    peer = ctap.client_pin(proto.VERSION, ClientPin.CMD.GET_KEY_AGREEMENT)[ClientPin.RESULT.KEY_AGREEMENT]
    key_agreement, shared = proto.encapsulate(peer)
    new_pin_enc = proto.encrypt(shared, b"abc".ljust(64, b"\0"))
    pin_hash_enc = proto.encrypt(shared, hashlib.sha256(PIN.encode()).digest()[:16])
    expect(CtapError.ERR.PIN_POLICY_VIOLATION, ctap.client_pin, proto.VERSION, ClientPin.CMD.CHANGE_PIN,
           key_agreement=key_agreement, new_pin_enc=new_pin_enc, pin_hash_enc=pin_hash_enc,
           pin_uv_param=proto.authenticate(shared, new_pin_enc + pin_hash_enc))
    cp.get_pin_token(PIN)  # PIN unchanged
    print("PIN policy (min 4): OK")
    print("ALL PASS")


if __name__ == "__main__":
    main()
