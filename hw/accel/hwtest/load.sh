#!/usr/bin/env bash
# Load a bitstream into the FPGA SRAM through the PolKey (DirtyJTAG) and wait until the link answers.
# usage: hwtest/load.sh build/accel_main_f54_s12.fs
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
nix-shell --run "openFPGALoader -c dirtyJtag -b tangnano20k $1" > /dev/null 2>&1
sleep 4
