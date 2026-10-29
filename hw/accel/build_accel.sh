#!/usr/bin/env bash
# Build an accelerator bitstream for the Tang Nano 20k (run inside nix-shell).
#   ./hw/accel/build_accel.sh asym   -> P-256, Ed25519, SHA-512, SHA-256/HMAC/PBKDF2   (build/accel_asym.fs)
#   ./hw/accel/build_accel.sh sym    -> AES-CBC/GCM, SHA-256/HMAC/PBKDF2               (build/accel_sym.fs)
#   ./hw/accel/build_accel.sh pq     -> ML-DSA-44/65/87 + RSA-2048 keygen/sign with wrapped keys (build/accel_pq.fs)
#   (env: PLL_MUL/PLL_DIV = clock 27*MUL/DIV MHz, TAG, NEXTPNR_OPTS, SYNTH_OPTS, EXTRA_DEFINES; profile "probe" = no engines)
#   ./hw/accel/build_accel.sh main   -> P-256, Ed25519, ML-DSA, RSA-2048, SHA-512, TRNG in ONE bitstream (all key operations; build/accel_main.fs)
#   ./hw/accel/build_accel.sh full   -> main + SHA-256/HMAC/PBKDF2 (too big for place & route: kept for experiments); all = full + AES
# Secrets generated once and kept in hw/tro/build/ (gitignored): uid.hex (public identity), master.hex
# (key-wrapping root key; anyone holding it - or the bitstream - can unwrap keys wrapped by this device).
set -euo pipefail
PROFILE="${1:-asym}"
PLL_MUL="${PLL_MUL:-1}"; PLL_DIV="${PLL_DIV:-1}"   # system clock = 27 MHz * PLL_MUL / PLL_DIV via the rPLL (1/1 = board clock; 3/1 = 81, 4/1 = 108, 8/3 = 72 MHz)
case "$PLL_MUL" in [1-9]|[1-5][0-9]|6[0-3]) ;; *) echo "PLL_MUL must be 1..63" >&2; exit 1 ;; esac
case "$PLL_DIV" in [1-8]) ;; *) echo "PLL_DIV must be 1..8" >&2; exit 1 ;; esac
FMHZ=$((27 * PLL_MUL / PLL_DIV))
[ $((27 * PLL_MUL % PLL_DIV)) -eq 0 ] || { echo "27*MUL/DIV must be an integer MHz" >&2; exit 1; }
[ $((FMHZ * 2 % 3)) -eq 0 ] || { echo "clock $FMHZ MHz: CPB = f/1.5 MHz is not an integer" >&2; exit 1; }
CPB=$((FMHZ * 2 / 3))
NAME="$PROFILE"; [ "$FMHZ" = 27 ] || NAME="${PROFILE}_f${FMHZ}"
NAME="${NAME}${TAG:+_$TAG}"                     # TAG=s2 etc.: separate output files for parallel experiments (builds are single threaded: run many)
NEXTPNR_OPTS="${NEXTPNR_OPTS:-}"                # e.g. "--seed 3" or "--router router1"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TRO="$HERE/../tro"; BUILD="$HERE/build"; mkdir -p "$BUILD" "$TRO/build"
case "$PROFILE" in
  asym) AES=0; SYM=0; ECC=1; PQ=0; CAPS=0x0F0 ;;
  sym)  AES=1; SYM=1; ECC=0; PQ=0; CAPS=0x00F ;;
  pq)   AES=0; SYM=0; ECC=0; PQ=1; CAPS=0xF00 ;;
  main) AES=0; SYM=0; ECC=1; PQ=1; CAPS=0xFF0 ;;     # all key operations: P-256, Ed25519, ML-DSA, ML-KEM, RSA-2048, SHA-512, TRNG
  full) AES=0; SYM=1; ECC=1; PQ=1; CAPS=0xFF7 ;;     # + SHA-256/HMAC/PBKDF2 (does not place & route on the GW2AR-18C)
  all)  AES=1; SYM=1; ECC=1; PQ=1; CAPS=0xFFF ;;
  probe) AES=0; SYM=0; ECC=0; PQ=0; CAPS=0x000 ;;   # no engines: link, button, LEDs only (fast builds for clock / PLL experiments)     # + AES (does not fit)
  *) echo "profile must be asym, sym, pq, main, full or all" >&2; exit 1 ;;
esac
# Tang Nano 20K = GW2AR-LV18QN88C8/I7, family GW2A-18C (the plain GW2A-18 family builds fine but its PLL never locks)
DEVICE="${DEVICE_OVERRIDE:-GW2AR-LV18QN88C8/I7}"; FAMILY="${FAMILY_OVERRIDE:-GW2A-18C}"; TOP="accel_top"
gen() { [ -s "$1" ] || head -c"$2" /dev/urandom | od -An -tx1 | tr -d ' \n' > "$1"; }
gen "$TRO/build/uid.hex" 16; gen "$TRO/build/master.hex" 32
UID_HEX="$(cat "$TRO/build/uid.hex")"; MASTER_HEX="$(cat "$TRO/build/master.hex")"
echo "Profile: $PROFILE   clock: $FMHZ MHz (PLL_MUL=$PLL_MUL/PLL_DIV=$PLL_DIV)   Device UID: $UID_HEX"

# Build from a private copy of rtl/ so the per-device MASTER never lands in the simulation files:
# microcode / constant ROMs are regenerated there with the real master key.
SRC="$BUILD/rtl_$NAME"; rm -rf "$SRC"; cp -r "$HERE/rtl" "$SRC"
cp "$HERE"/rtl_pq/* "$SRC"/
if [ "$PQ" = 1 ]; then PQ_OUT="$SRC" PQ_CAPS=$CAPS python3 "$HERE/ref/pq/pq_asm.py" >/dev/null 2>&1 || { echo "pq_asm.py failed" >&2; exit 1; }; fi
GEN_OUT="$SRC" TRO_MASTER_HEX="$MASTER_HEX" python3 "$HERE/ref/gen_ecc.py" >/dev/null

RTL=$(ls "$SRC"/*.v | tr '\n' ' ')
echo "==> [1/3] Synthesis"
# SYNTH_OPTS: synth_gowin options (default -nowidelut: no wide-LUT mux leaves, markedly smaller; SYNTH_OPTS= to disable)
yosys -q -l "$BUILD/yosys_$NAME.log" -p "
  verilog_defines -DUID_VAL=128'h$UID_HEX -DMASTER_VAL=256'h$MASTER_HEX ${EXTRA_DEFINES:-}
  read_verilog -I$SRC $RTL $TRO/root_of_trust.v
  chparam -set HAS_AES $AES -set HAS_SYM $SYM -set HAS_ECC $ECC -set HAS_PQ $PQ -set PLL_MUL $PLL_MUL -set PLL_DIV $PLL_DIV -set CPB $CPB $TOP
  synth_gowin -top $TOP ${SYNTH_OPTS--nowidelut} -json $BUILD/accel_$NAME.json
  tee -o $BUILD/stat_$NAME.txt stat
"
if [ "${SYNTH_ONLY:-0}" = 1 ]; then
  grep -E "^ +[0-9]+ +(LUT[1-4]|ALU|DFF[A-Z]*|DPB|DPX9B|SP|SPX9|SDPB|SDPX9B|pROM|MULT[0-9X]+)( |$)" "$BUILD/stat_$NAME.txt" | tail -22
  exit 0
fi
echo "==> [2/3] Place & route"
nextpnr-himbaechel --device "$DEVICE" -o family="$FAMILY" -o cst="$HERE/accel.cst" \
  --ignore-loops $NEXTPNR_OPTS --freq "$FMHZ" --json "$BUILD/accel_$NAME.json" --write "$BUILD/accel_${NAME}_routed.json" \
  --report "$BUILD/accel_${NAME}_report.json" 2>&1 | tee "$BUILD/nextpnr_$NAME.log" | grep -E "Max frequency|LUT4:|DFF:|BSRAM:|ALU:|ERROR|Error" || true
echo "==> [3/3] Bitstream"
gowin_pack -d "$FAMILY" -o "$BUILD/accel_$NAME.fs" "$BUILD/accel_${NAME}_routed.json"
echo "Bitstream: $BUILD/accel_$NAME.fs"
