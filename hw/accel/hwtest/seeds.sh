#!/usr/bin/env bash
# Build one profile with several placer seeds in parallel (a build is single threaded and uses ~2 % of a core).
# usage: PLL_MUL=2 PLL_DIV=1 hwtest/seeds.sh main 11 12 13 14      -> build/accel_main_f54_s11.fs ...
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
profile=$1; shift
for sd in "$@"; do
  ( TAG=s$sd NEXTPNR_OPTS="--seed $sd" nix-shell --run "./hw/accel/build_accel.sh $profile" > "hw/accel/build/seed_${profile}_s$sd.log" 2>&1 ) &
done
wait
grep -h "Max frequency\|Bitstream:" hw/accel/build/seed_${profile}_s*.log | sed 's/Info: //' | sort | uniq | head -40
