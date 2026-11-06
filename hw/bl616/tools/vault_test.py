#!/usr/bin/env python3
"""Vault / TOTP / management JSON protocol test of the PolKey firmware (CTAPHID vendor command 0xC1).

Needs the asym or main FPGA bitstream (the vault key is bound to an FPGA-wrapped device key):
    openFPGALoader -c dirtyJtag -b tangnano20k hw/accel/build/accel_main.fs
    python3 hw/bl616/tools/vault_test.py [--reset] [--usb]   (--usb: vendor bulk tunnel, needs pyusb)

--reset sends FACTORY_RESET first (wipes vault, FIDO credentials and PIN). Without it an already
initialised vault must use PIN VAULT_PIN below.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from ctaphid_test import CtapHid, find_hidraw


VAULT_PIN = "vault-1234"
NEW_PIN = "vault-5678"


class VaultUsb:
    """Same JSON protocol through the vendor bulk tunnel (0xF0 frames) that the browser extension uses via WebUSB."""

    def __init__(self):
        import usb.core
        import usb.util
        self.usb = usb.util
        self.dev = usb.core.find(idVendor=0x1209, idProduct=0xC0CA)
        assert self.dev is not None, "PolKey not found"
        usb.util.claim_interface(self.dev, 0)

    def call_all(self, **req):
        """Send one request; returns every JSON message of the answer (frames 0xF1 ... closed by an empty 0xF0)."""
        payload = json.dumps(req).encode()
        frame = bytes([0xF0, len(payload) & 0xFF, len(payload) >> 8]) + payload
        for off in range(0, len(frame), 64):
            self.dev.write(0x01, frame[off:off + 64], 5000)
        buf, msgs = b"", []
        deadline = time.time() + 60
        while True:
            while len(buf) >= 3:
                typ, ln = buf[0], buf[1] | (buf[2] << 8)
                if len(buf) < 3 + ln:
                    break
                body, buf = buf[3:3 + ln], buf[3 + ln:]
                if typ == 0xF0:
                    return msgs
                assert typ == 0xF1, hex(typ)
                msgs.append(json.loads(body))
                deadline = time.time() + 60
            assert time.time() < deadline, "timeout"
            buf += bytes(self.dev.read(0x82, 64, 60000))

    def call(self, **req):
        msgs = self.call_all(**req)
        assert len(msgs) == 1, msgs
        return msgs[0]


class Vault:
    def __init__(self):
        path = find_hidraw()
        assert path, "PolKey FIDO HID interface not found"
        self.dev = CtapHid(path)
        self.dev.init()

    def call(self, **req):
        cid, cmd, data = self.dev.exchange(0x41, json.dumps(req).encode(), timeout=15.0)
        assert cmd == 0xC1, f"unexpected response command 0x{cmd:02x}: {bytes(data)[:20]!r}"
        return json.loads(bytes(data))

    def call_all(self, **req):
        return [self.call(**req)]


def ok(msg):
    print(f"{msg}: OK")


def event(r, module, name):
    assert r["type"] == "event" and r["module"] == module and r["event"] == name, r
    return r.get("data", {})


def error(r, module, code):
    assert r["type"] == "error" and r["module"] == module and r["error_code"] == code, r


def main():
    v = VaultUsb() if "--usb" in sys.argv else Vault()
    print("transport:", "vendor bulk tunnel" if "--usb" in sys.argv else "CTAPHID vendor command")
    boot = event(v.call(cmd="RESTART_SYSTEM"), "SYS", "BOOT")
    if "--reset" in sys.argv and boot["pin_set"]:  # commands other than CREATE_PIN need an unlocked vault
        event(v.call(cmd="VERIFY_PIN", pin=VAULT_PIN), "SECURITY", "PIN_OK")
        event(v.call(cmd="FACTORY_RESET"), "SYS", "FACTORY_RESET_COMPLETE")
        ok("factory reset")
        boot = event(v.call(cmd="RESTART_SYSTEM"), "SYS", "BOOT")
    error(v.call(cmd="LIST_PASS"), "AUTH", "NEW_PIN_REQ" if not boot["pin_set"] else "PIN_REQ")
    if not boot["pin_set"]:
        error(v.call(cmd="CREATE_PIN", pin="abc"), "AUTH", "PIN_TOO_SHORT")
        t = time.time()
        event(v.call(cmd="CREATE_PIN", pin=VAULT_PIN), "AUTH", "PIN_CREATED")
        ok(f"vault PIN created ({time.time() - t:.1f}s)")

    r = v.call(cmd="VERIFY_PIN", pin="wrong-pin")
    error(r, "SECURITY", "BAD_PIN_ATTEMPT")
    t = time.time()
    event(v.call(cmd="VERIFY_PIN", pin=VAULT_PIN), "SECURITY", "PIN_OK")
    ok(f"wrong PIN rejected, correct PIN unlocks ({time.time() - t:.1f}s)")

    # --- passwords
    d = event(v.call(cmd="SAVE_PASS", site="example.com", login="alice", autogen=True), "PASS", "SAVED")
    gen = d["generated_password"]
    assert len(gen) == 16
    d = event(v.call(cmd="GET_PASS", site="example.com", login="alice"), "PASS", "TRANSMITTED")
    assert d["password"] == gen
    event(v.call(cmd="SAVE_PASS", site="example.com", login="bob", password="short"), "PASS", "SAVED")
    event(v.call(cmd="SAVE_PASS", site="other.org", login="alice", password="short"), "PASS", "SAVED")
    event(v.call(cmd="UPDATE_PASS", site="example.com", login="bob", password="Str0ng!Passw0rd#42"), "PASS", "UPDATED")
    error(v.call(cmd="UPDATE_PASS", site="nowhere", login="x", password="y"), "PASS", "NOT_FOUND")
    items = event(v.call(cmd="LIST_PASS"), "PASS", "LIST")["items"]
    status = {(i["website"], i["login"]): i["status"] for i in items}
    assert status[("example.com", "alice")] == "OK", status
    assert status[("example.com", "bob")] == "OK", status
    assert status[("other.org", "alice")] == "WEAK", status
    assert "password" not in items[0]
    ok(f"save / get / update / list ({len(items)} items, weak detection)")
    event(v.call(cmd="DELETE_PASS", site="other.org", login="alice"), "PASS", "DELETED")
    error(v.call(cmd="GET_PASS", site="other.org", login="alice"), "PASS", "NOT_FOUND")
    ok("delete")

    # --- TOTP (RFC 6238 test vectors, secret '12345678901234567890')
    secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
    event(v.call(cmd="SAVE_TOTP", name="github", secret=secret), "TOTP", "SAVED")
    for epoch, code in ((59, "287082"), (1111111109, "081804"), (1234567890, "005924"), (2000000000, "279037")):
        codes = event(v.call(cmd="GET_TOTP", epoch=epoch), "TOTP", "CODES")["codes"]
        assert codes["github"] == code, (epoch, codes)
    event(v.call(cmd="DELETE_TOTP", name="github"), "TOTP", "DELETED")
    assert event(v.call(cmd="GET_TOTP", epoch=59), "TOTP", "CODES")["codes"] == {}
    ok("TOTP against the RFC 6238 vectors")

    # --- PIN change keeps the data readable
    event(v.call(cmd="UPDATE_PIN", current_pin=VAULT_PIN, new_pin=NEW_PIN), "AUTH", "PIN_UPDATED")
    event(v.call(cmd="DISCONNECT"), "SYS", "DISCONNECTED")
    error(v.call(cmd="GET_PASS", site="example.com", login="alice"), "AUTH", "PIN_REQ")
    event(v.call(cmd="VERIFY_PIN", pin=NEW_PIN), "SECURITY", "PIN_OK")
    assert event(v.call(cmd="GET_PASS", site="example.com", login="alice"), "PASS", "TRANSMITTED")["password"] == gen
    event(v.call(cmd="UPDATE_PIN", current_pin=NEW_PIN, new_pin=VAULT_PIN), "AUTH", "PIN_UPDATED")
    ok("PIN change re-encrypts the vault")

    # --- misc
    d = event(v.call(cmd="STORAGE_INFO"), "STORAGE", "STATS")
    assert d["passwords_count"] == 2 and d["total_bytes"] > d["used_bytes"] > 0, d
    d = event(v.call(cmd="LIST_FIDO"), "FIDO2", "LIST")
    assert "websites" in d
    d = event(v.call(cmd="RUN_DIAGNOSTICS"), "SYS", "DIAGNOSTICS_COMPLETE")
    assert d["passed"] == d["total"] == 4, d
    event(v.call(cmd="SET_CRYPTO_ALG", algId=-7), "SYS", "SETTINGS_UPDATED")
    error(v.call(cmd="NOPE"), "SYS", "UNKNOWN_CMD")
    ok(f"storage info ({d['total']} diagnostics), FIDO list, settings")
    # --- backup export / import (VBK1, 200000 PBKDF2 iterations) -- tunnel only, the export is a stream of messages
    if "--usb" in sys.argv:
        import hashlib
        t = time.time()
        msgs = v.call_all(cmd="BACKUP_EXPORT", passphrase="correct horse battery")
        assert msgs[0]["event"] == "EXPORT_START" and msgs[-1]["event"] == "EXPORT_DONE", [m.get("event") for m in msgs[:3]]
        start = msgs[0]["data"]
        hexblob = "".join(m["data"]["data"] for m in msgs[1:-1])
        assert len(hexblob) == start["total_len"] and hashlib.sha256(hexblob.encode()).hexdigest() == start["sha256"]
        ok(f"backup export: {len(msgs) - 2} chunks, checksum ({time.time() - t:.1f}s)")
        # independent decryption of the archive
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
        blob = bytes.fromhex(hexblob)
        assert blob[:4] == b"VBK1"
        salt, iters, nonce, tag, n = blob[5:21], int.from_bytes(blob[21:25], "big"), blob[25:37], blob[37:53], int.from_bytes(blob[53:57], "big")
        key = hashlib.pbkdf2_hmac("sha256", b"correct horse battery", salt, iters, 32)
        plain = json.loads(AESGCM(key).decrypt(nonce, blob[57:57 + n] + tag, None))
        assert {(p["site"], p["login"]) for p in plain["passwords"]} == {("example.com", "alice"), ("example.com", "bob")}
        ok("backup decrypts with PBKDF2/AES-GCM from the standard library")
        event(v.call(cmd="DELETE_PASS", site="example.com", login="alice"), "PASS", "DELETED")
        step = 2048
        event(v.call(cmd="BACKUP_IMPORT_BEGIN", total_len=len(hexblob), sha256=start["sha256"]), "BACKUP", "IMPORT_READY")
        for i in range(0, len(hexblob), step):
            r = v.call_all(cmd="BACKUP_IMPORT_CHUNK", data=hexblob[i:i + step])
            assert r == [], r
        error(v.call(cmd="BACKUP_IMPORT_COMMIT", passphrase="wrong passphrase", overwrite=False), "BACKUP", "IMPORT_FAILED")
        event(v.call(cmd="BACKUP_IMPORT_BEGIN", total_len=len(hexblob), sha256=start["sha256"]), "BACKUP", "IMPORT_READY")
        for i in range(0, len(hexblob), step):
            v.call_all(cmd="BACKUP_IMPORT_CHUNK", data=hexblob[i:i + step])
        d = event(v.call(cmd="BACKUP_IMPORT_COMMIT", passphrase="correct horse battery", overwrite=False), "BACKUP", "IMPORT_DONE")
        assert d["imported"] == 1 and d["skipped"] == 1, d
        assert event(v.call(cmd="GET_PASS", site="example.com", login="alice"), "PASS", "TRANSMITTED")["password"] == gen
        ok(f"backup import: wrong passphrase rejected, restore imported {d['imported']} / skipped {d['skipped']}")

    for item in ("example.com/alice", "example.com/bob"):
        site, login = item.split("/")
        event(v.call(cmd="DELETE_PASS", site=site, login=login), "PASS", "DELETED")
    print("ALL PASS")


if __name__ == "__main__":
    main()
