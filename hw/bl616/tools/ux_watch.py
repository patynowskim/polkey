#!/usr/bin/env python3
"""Print the raw state of button S2 as seen by the FPGA while you press it (20 s): bit0 = pressed now, bit1 = latched press."""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from vault_test import Vault

v = Vault()
end = time.time() + 20
last = None
print("watching S2 for 20 s - press and release it a few times", flush=True)
while time.time() < end:
    r = v.call(cmd="UX_STATE")["data"]
    st = (r["ux_ok"], r["ux_state"])
    if st != last:
        print(f"ux_ok={r['ux_ok']} state={r['ux_state']:02b}  (bit0 = pressed now, bit1 = press latched)", flush=True)
        last = st
    time.sleep(0.1)
