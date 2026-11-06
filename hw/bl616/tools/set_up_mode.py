#!/usr/bin/env python3
"""Switch the user-presence requirement (button S2 on the Tang Nano) on or off: set_up_mode.py on|off [vault-pin]

Off is for automated tests only: with it MakeCredential / GetAssertion / reset / U2F / GET_PASS are approved without a touch.
Needs the vault PIN (default VAULT_PIN of vault_test.py) because it changes a security setting."""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from vault_test import Vault, VAULT_PIN

mode = sys.argv[1] if len(sys.argv) > 1 else ""
if mode not in ("on", "off"):
    sys.exit(__doc__)
v = Vault()
r = v.call(cmd="RESTART_SYSTEM")
assert r["event"] == "BOOT", r
r = v.call(cmd="VERIFY_PIN", pin=sys.argv[2] if len(sys.argv) > 2 else VAULT_PIN)
assert r.get("event") == "PIN_OK", r
r = v.call(cmd="UPDATE_SETTINGS", up_required=(mode == "on"))
print(json.dumps(r["data"]))
v.call(cmd="DISCONNECT")
