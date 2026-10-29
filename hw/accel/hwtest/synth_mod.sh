#!/usr/bin/env bash
# Quick area check of a single module: hwtest/synth_mod.sh <top> <files...>   (prints LUT+ALU, DFF*, MULT, BSRAM counts)
# e.g. hwtest/synth_mod.sh pq_pack rtl_pq/pq_pack.v
set -u
top=$1; shift
cd "$(dirname "${BASH_SOURCE[0]}")/.."
nix-shell ../../shell.nix --run "yosys -q -p 'read_verilog -Irtl -Irtl_pq $*; synth_gowin -top $top -nowidelut -json /dev/null; tee -o /dev/stdout stat'" 2>/dev/null | awk '/LUT[1-4]|ALU|DFF|MULT|DPB|SDP|SPX|SP |pROM/ {print}' | awk '{n[$2]=$1} END{lut=n["LUT1"]+n["LUT2"]+n["LUT3"]+n["LUT4"]+n["ALU"]; ff=0; for(k in n) if (k ~ /^DFF/) ff+=n[k]; printf "%s: LUT+ALU=%d  FF=%d  MULT=%d/%d/%d\n", "'"$top"'", lut, ff, n["MULT9X9"], n["MULT18X18"], n["MULT36X36"]}'
