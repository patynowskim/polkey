#!/usr/bin/env bash
# Hardware gate for one or more bitstreams: load, then ECDH repeatability + MakeCredential/GetAssertion of every algorithm.
# usage: hwtest/gate.sh a.fs b.fs ...    (user presence must be off: hw/bl616/tools/set_up_mode.py off)
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
for f in "$@"; do
  "$ROOT/hw/accel/hwtest/load.sh" "$f"
  r=$(cd hw/bl616/tools && timeout 300 nix-shell -p 'python3.withPackages(p:[p.hidapi p.fido2 p.cryptography p.pyusb])' --run "python3 gate.py ${GATE_OPTS:-}" 2>&1 | tail -1)
  echo "$(basename "$f" .fs): $r"
done
