# Hardware-verified bitstreams (Tang Nano 20K, device GW2AR-LV18QN88C8/I7, family GW2A-18C)

Each file passed `tools/gate.py` and the full regression `tools/fido2lib_test.py --extras --algs` on the board
(P-256, Ed25519, ML-DSA-44/65/87, RS256, U2F, PIN, hmac-secret, credMgmt, large blobs, ...).

| file | clock | notes |
|---|---|---|
| `accel_main_f27.fs` | 27 MHz (board clock) | seed 11 |
| `accel_main_f54.fs` | 54 MHz (rPLL x2) | seed 12; P-256 MakeCredential 0.46 s vs 0.87 s at 27 MHz |

**nextpnr "PASS" is not enough.** Equal RTL built with a different `--seed` (placement) often computes wrong results on the chip
(P-256 keys/signatures invalid, or ML-DSA fails) although timing analysis passes; from about 45 MHz up most seeds fail ML-DSA.
Always load a new build on the board and run the gate before using it:

    PLL_MUL=2 PLL_DIV=1 TAG=sNN NEXTPNR_OPTS="--seed NN" ./hw/accel/build_accel.sh main      # many seeds in parallel
    openFPGALoader -c dirtyJtag -b tangnano20k hw/accel/build/accel_main_f54_sNN.fs
    python3 hw/bl616/tools/gate.py          # ECDH repeatability + MakeCredential/GetAssertion of every algorithm
    python3 hw/bl616/tools/fido2lib_test.py --extras --algs      # signatures verified independently

(user presence must be off for the tests: `tools/set_up_mode.py off`).

- accel_full_f27.fs — profile `full` (main + SHA-256/HMAC/PBKDF2 on the FPGA), 27 MHz, nextpnr --seed 3. Passed gate.py and the full regression (fido2lib_test --extras --algs, vault_test --usb incl. PBKDF2 backup). Became placeable after removing async resets from sha2_unit data registers.
