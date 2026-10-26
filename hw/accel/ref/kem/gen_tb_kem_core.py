#!/usr/bin/env python3
"""Vectors for tb_pq_core (+vec=tb/vec_kem_core): ML-KEM keygen/encaps/decaps through the full engine.
usage: gen_tb_kem_core.py [levels,comma]   (tb TRNG: counter bytes 100,101,...; sim MASTER = bytes(range(32)))"""
import hashlib, os, random, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, "..", "pq"))
import mlkem, pq_asm as A
random.seed(3)
master = bytes(range(32)); nonce = bytes(range(100, 116))
levels = [int(x) for x in (sys.argv[1] if len(sys.argv) > 1 else "512,768,1024").split(",")]
out = os.path.join(HERE, "..", "..", "tb", "vec_kem_core"); os.makedirs(out, exist_ok=True)
def rb(n): return bytes(random.randrange(256) for _ in range(n))
tests = []
for lvl in levels:
    p = {512: 0, 768: 1, 1024: 2}[lvl]; k = mlkem.PARAMS[lvl][0]
    d, z, m = rb(32), rb(32), rb(32)
    ek, dk = mlkem.keygen(d, z, lvl); K, c = mlkem.encaps(ek, m, lvl)
    blob = A.wrap_py(master, z + hashlib.sha3_256(ek).digest() + dk[:384 * k] + ek, nonce)
    tests.append((f"kg{lvl}", 0x84 + p, d + z, 0, blob + ek))
    tests.append((f"enc{lvl}", 0x8C + p, ek + m, 0, K + c))
    tests.append((f"dec{lvl}", 0x90 + p, blob + c, 0, K))
    bad = bytearray(c); bad[11] ^= 8
    tests.append((f"rej{lvl}", 0x90 + p, blob + bytes(bad), 0, mlkem.decaps(dk, bytes(bad), lvl)))
    bb = bytearray(blob); bb[200] ^= 1; tests.append((f"badtag{lvl}", 0x90 + p, bytes(bb) + c, 4, b""))
    bk = bytearray(ek); bk[0] = 0xFF; bk[1] |= 0x0F; tests.append((f"badek{lvl}", 0x8C + p, bytes(bk) + m, 3, b""))
    tests.append((f"badlen{lvl}", 0x88 + p, ek[:-1], 2, b""))
with open(os.path.join(out, "index.txt"), "w") as ix:
    ix.write(f"{len(tests)}\n")
    for i, (name, cmd, pl, st, ex) in enumerate(tests):
        open(os.path.join(out, f"req_{i}.hex"), "w").write("\n".join(f"{b:02x}" for b in pl) + "\n" if pl else "00\n")
        open(os.path.join(out, f"exp_{i}.hex"), "w").write("\n".join(f"{b:02x}" for b in ex) + "\n" if ex else "00\n")
        ix.write(f"{cmd} {len(pl)} {st} {len(ex)} {name}\n")
print(len(tests), "tests")
