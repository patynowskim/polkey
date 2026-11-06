#!/usr/bin/env python3
"""Raw CTAPHID smoke test for the PolKey FIDO2 firmware (no dependencies).

Finds the FIDO HID interface of USB 1209:c0ca via /sys/class/hidraw, then
exercises CTAPHID INIT, PING, WINK, GetInfo and an unknown command.
"""
import glob
import os
import select
import struct
import sys
import time

VID, PID = 0x1209, 0xC0CA
HID_ID = f"0003:{VID:08X}:{PID:08X}"

CTAPHID_PING = 0x81
CTAPHID_INIT = 0x86
CTAPHID_WINK = 0x88
CTAPHID_CBOR = 0x90
CTAPHID_ERROR = 0xBF

BROADCAST = 0xFFFFFFFF


def find_hidraw():
    for path in glob.glob("/sys/class/hidraw/hidraw*"):
        try:
            with open(os.path.join(path, "device/uevent")) as f:
                if HID_ID in f.read():
                    return "/dev/" + os.path.basename(path)
        except OSError:
            continue
    return None


class CtapHid:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR)
        self.cid = None

    def close(self):
        os.close(self.fd)

    def _send_packets(self, cid, cmd, data):
        pkt = bytearray(64)
        pkt[0:4] = struct.pack(">I", cid)
        pkt[4] = cmd | 0x80
        pkt[5:7] = struct.pack(">H", len(data))
        pkt[7:7 + min(57, len(data))] = data[:57]
        os.write(self.fd, bytes(pkt))
        off, seq = 57, 0
        while off < len(data):
            pkt = bytearray(64)
            pkt[0:4] = struct.pack(">I", cid)
            pkt[4] = seq & 0x7F
            chunk = data[off:off + 59]
            pkt[5:5 + len(chunk)] = chunk
            os.write(self.fd, bytes(pkt))
            off += len(chunk)
            seq += 1

    def _read_response(self, timeout=2.0):
        deadline = time.time() + timeout
        data = None
        seq = 0
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError("no response")
            r, _, _ = select.select([self.fd], [], [], left)
            if not r:
                raise TimeoutError("no response")
            pkt = os.read(self.fd, 64)
            if len(pkt) < 7:
                continue
            cid = struct.unpack(">I", pkt[0:4])[0]
            if pkt[4] & 0x80:
                cmd = pkt[4]
                total = struct.unpack(">H", pkt[5:7])[0]
                data = bytearray(pkt[7:7 + min(57, total)])
                seq = 0
                if len(data) >= total:
                    return cid, cmd, bytes(data)
            else:
                if data is None or pkt[4] != seq:
                    continue
                seq += 1
                remaining = total - len(data)
                data += pkt[5:5 + min(59, remaining)]
                if len(data) >= total:
                    return cid, cmd, bytes(data)

    def exchange(self, cmd, data=b"", cid=None, timeout=2.0):
        if cid is None:
            cid = self.cid if self.cid is not None else BROADCAST
        self._send_packets(cid, cmd, data)
        while True:
            resp = self._read_response(timeout)
            if resp[1] != 0xBB:  # skip CTAPHID_KEEPALIVE
                return resp

    def init(self):
        nonce = os.urandom(8)
        cid, cmd, resp = self.exchange(CTAPHID_INIT, nonce)
        assert cmd == CTAPHID_INIT, f"INIT: unexpected cmd 0x{cmd:02x}"
        assert resp[:8] == nonce, "INIT: nonce mismatch"
        self.cid = struct.unpack(">I", resp[8:12])[0]
        proto, major, minor, build, caps = resp[12], resp[13], resp[14], resp[15], resp[16]
        print(f"INIT  OK  cid=0x{self.cid:08x} protocol={proto} "
              f"version={major}.{minor}.{build} caps=0x{caps:02x}")
        return self.cid


def main():
    path = find_hidraw()
    if not path:
        print(f"PolKey FIDO HID interface ({HID_ID}) not found")
        return 1
    print(f"device: {path}")

    dev = CtapHid(path)
    try:
        dev.init()

        payload = bytes(range(64))
        cid, cmd, resp = dev.exchange(CTAPHID_PING, payload)
        assert cmd == CTAPHID_PING and resp == payload, "PING echo mismatch"
        print(f"PING  OK  {len(resp)} bytes echoed")

        cid, cmd, resp = dev.exchange(CTAPHID_WINK)
        assert cmd == CTAPHID_WINK and resp == b"", "WINK failed"
        print("WINK  OK")

        cid, cmd, resp = dev.exchange(CTAPHID_CBOR, b"\x04")
        assert cmd == CTAPHID_CBOR, f"GetInfo: unexpected cmd 0x{cmd:02x}"
        if resp[0] != 0:
            print(f"GetInfo FAIL status=0x{resp[0]:02x}")
            return 1
        print(f"GetInfo OK  {len(resp) - 1} bytes of CBOR: {resp[1:].hex()}")

        cid, cmd, resp = dev.exchange(CTAPHID_CBOR, b"\x7f")
        assert cmd == CTAPHID_CBOR and resp[0] == 0x01, f"unknown cmd gave {resp.hex()}"
        print("unknown-command error OK (0x01)")

        cid, cmd, resp = dev.exchange(0x99)
        assert cmd == CTAPHID_ERROR and resp[0] == 0x01, f"bad CTAPHID cmd gave 0x{cmd:02x} {resp.hex()}"
        print("bad-CTAPHID-command error OK (0x01)")

        print("ALL PASS")
        return 0
    finally:
        dev.close()


if __name__ == "__main__":
    sys.exit(main())
