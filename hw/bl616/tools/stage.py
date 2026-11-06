#!/usr/bin/env python3
"""Read the firmware progress marker (vendor bulk command 0xF2); works even when the CTAPHID task hangs.
1..6 boot steps (ctap2, store, pin, vault, fpga init done), 7 idle loop, 100+ vault command stages."""
import usb.core
import usb.util

dev = usb.core.find(idVendor=0x1209, idProduct=0xC0CA)
assert dev is not None, "PolKey not found"
usb.util.claim_interface(dev, 0)
dev.write(0x01, bytes([0xF2]), 2000)
r = bytes(dev.read(0x82, 64, 2000))
print("stage", int.from_bytes(r[1:5], "little"))
