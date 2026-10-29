#!/usr/bin/env bash
# Full verified regression per bitstream (every signature checked independently): fido2lib_test.py --extras --algs
# usage: hwtest/stress.sh a.fs b.fs ...    (user presence must be off)
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
for f in "$@"; do
  "$ROOT/hw/accel/hwtest/load.sh" "$f"
  t0=$(date +%s)
  out=$(cd hw/bl616/tools && timeout 900 nix-shell -p 'python3.withPackages(p:[p.hidapi p.fido2 p.cryptography p.pyusb])' --run "python3 fido2lib_test.py --extras --algs" 2>&1 | grep -v "^  File\|^    " | tail -3 | tr '\n' ' ')
  echo "$(basename "$f" .fs): $(( $(date +%s) - t0 ))s :: $out" | cut -c1-300
done
