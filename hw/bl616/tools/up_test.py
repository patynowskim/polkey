#!/usr/bin/env python3
"""Interactive user-presence test: needs user presence ON (set_up_mode.py on) and a human at the board.

1. MakeCredential must wait for a touch of S2 (keepalive UPNEEDED), succeed after the press
2. without a press it must fail with ACTION_TIMEOUT (waits the full 30 s)
3. GetAssertion with up=false must not need a touch
4. U2F register answers CONDITIONS_NOT_SATISFIED until S2 is pressed
"""
import os
import sys
import threading
import time

from fido2.ctap import CtapError
from fido2.ctap1 import ApduError, Ctap1
from fido2.ctap2 import Ctap2
from fido2.ctap2.pin import ClientPin, PinProtocolV2

sys.path.insert(0, os.path.dirname(__file__))
from fido2lib_test import PIN, RP, find_device, make_cred


def main():
    dev = find_device()
    ctap = Ctap2(dev)
    proto = PinProtocolV2()
    cp = ClientPin(ctap, protocol=proto)
    info = ctap.get_info()
    assert info.options["clientPin"], "run fido2lib_test.py first (it sets the PIN)"

    # --- 3: up=false needs no touch (the PIN token still authorizes)
    tok = cp.get_pin_token(PIN, ClientPin.PERMISSION.MAKE_CREDENTIAL, RP["id"])
    t0 = time.time()
    if "--timeout-test" in sys.argv:
        print("\n>>> Do NOT touch the board for the next 35 s <<<")
        try:
            make_cred(ctap, proto, tok, b"untouched")
            raise AssertionError("MakeCredential succeeded without a touch")
        except CtapError as e:
            dt = time.time() - t0
            assert e.code == CtapError.ERR.USER_ACTION_TIMEOUT, e.code
            assert 25 < dt < 40, dt
            print(f"no touch -> USER_ACTION_TIMEOUT after {dt:.0f} s: OK")

    # --- 1: touch completes the operation
    tok = cp.get_pin_token(PIN, ClientPin.PERMISSION.MAKE_CREDENTIAL, RP["id"])
    result = {}

    def run():
        try:
            result["att"] = make_cred(ctap, proto, tok, b"touched")
        except Exception as ex:  # noqa: BLE001
            result["err"] = ex

    th = threading.Thread(target=run)
    print(">>> PRESS S2 (once) within 60 s (the LEDs blink) <<<")
    th.start()
    th.join(70)
    assert "err" not in result, result.get("err")
    assert "att" in result, "MakeCredential did not finish"
    print("touch -> MakeCredential completed: OK")

    # --- 4: U2F answers "conditions not satisfied" until a touch, polled like a browser does
    u2f = Ctap1(dev)
    app, chal = os.urandom(32), os.urandom(32)
    print(">>> PRESS S2 again (keep tapping about every 5 s) <<<")
    deadline = time.time() + 60
    refused = 0
    while True:
        try:
            reg = u2f.register(chal, app)
            break
        except ApduError as e:
            assert e.code == 0x6985, hex(e.code)
            refused += 1
            assert time.time() < deadline, "no touch"
            time.sleep(0.2)
    reg.verify(app, chal)
    print(f"U2F register: {refused} x 0x6985 until the touch, then OK")
    print("ALL PASS")


if __name__ == "__main__":
    main()
