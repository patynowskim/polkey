#!/usr/bin/env python3
"""ML-KEM microcode programs (keygen / encaps / decaps with device-wrapped decapsulation keys) for the pq engine.

Commands (p = 0,1,2 for ML-KEM-512/768/1024):
  0x80+p  keygen, TRNG seed          -> blob | ek          (payload empty)
  0x84+p  keygen, explicit seed      -> blob | ek          (payload d[32] | z[32])
  0x88+p  encaps, TRNG message       -> K[32] | ct         (payload ek)
  0x8C+p  encaps, explicit message   -> K[32] | ct         (payload ek | m[32])
  0x90+p  decaps                     -> K[32]              (payload blob | ct)
blob = nonce16 | z32 | h32 | s(384k) | ek(384k+32) | tag16   (SHAKE256 keystream 'E' / MAC 'M' under MASTER, like ML-DSA)
Status: 2 bad length, 3 ek failed the modulus check, 4 bad blob tag.
Byte-descriptor modes (low 4 bits of the descriptor): see pq_asm.KD.
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pq"))
import pq_asm as P
from pq_asm import Asm, desc, RX, TX, SC
import kem_model as KM
from mlkem import PARAMS

# scratch layout
SC_D, SC_Z, SC_G, SC_M, SC_H, SC_KB, SC_TAG, SC_TMP = 0, 32, 64, 128, 160, 192, 224, 256
SC_RHO, SC_SIG, SC_K, SC_R = 64, 96, 64, 96
TX_CT = 4096                                            # decaps: the received ciphertext (copy), TX[32..] holds the re-encryption
# polynomial slots
S0, E, TA, T, TT, MU, V, U0, TN = 0, 4, 5, 6, 7, 8, 9, 12, 16
IX_I, IX_J = 32, 48                                     # indexed slot bases (multiples of 4, added to {ri, rj} in bits [3:2]): IX + base
OFF_K, OFF_C1, OFF_2K = 1 << 2, 2 << 2, 3 << 2         # offset-add modes: +384k, +32*du*k, +768k
LEN_CT, LEN_EK, LEN_S = 1, 2, 3                        # length modes
IDX_I, IDX_J = 1, 2
D_DU, D_DV = 13, 14                                     # KPK/KUPK width codes
def d_(sp, off, ln=0, mode=0): return desc(sp, off, ln) | mode

def wrap_stream(A, nonce_d, init_byte):
    A.op("SHINIT", 1); A.op("SHABSI", 0, 0, 0, 0, init_byte); A.op("SHABSM"); A.op("SHABSB", 0, 0, 0, 0, nonce_d)

def loop_i(A, label, body, reg=0):
    A.op("SETR", reg, 0); A.label(label); body(); A.op("INCR", reg); A.op("LTK", reg); A.jump("JF", label)

def prog_kem(base=0):
    A = Asm(base); A.entries = {}
    kp = {p: (PARAMS[l][0], PARAMS[l][1], PARAMS[l][3], PARAMS[l][4]) for p, l in enumerate((512, 768, 1024))}
    def stub(name, p, xf, target):
        A.entries[f"{name}{p}"] = base + len(A.w); A.op("KPAR", *kp[p]); A.op("XSET1" if xf else "XSET0"); A.jump("JMP", target)
    for p in range(3):
        stub("kg_trng", p, 0, "kg"); stub("kg_expl", p, 1, "kg")
    for p in range(3):
        A.entries[f"enc_trng{p}"] = base + len(A.w); A.op("KPAR", *kp[p]); A.op("CHKLN", 0); A.jump("JF", "bad_len"); A.op("RNGB", 0, 0, 0, 0, desc(SC, SC_M, 32)); A.jump("JMP", "enc")
        A.entries[f"enc_expl{p}"] = base + len(A.w); A.op("KPAR", *kp[p]); A.op("CHKLN", 1); A.jump("JF", "bad_len")
        A.op("BCPY", 0, 0, 0, 0, desc(SC, SC_M, 32), d_(RX, 32, 0, OFF_K)); A.jump("JMP", "enc")
    for p in range(3):
        A.entries[f"dec{p}"] = base + len(A.w); A.op("KPAR", *kp[p]); A.op("XSET1"); A.jump("JMP", "dec")

    # ------------------------------------------------------------------ keygen
    A.label("kg"); A.op("KEM", 1)
    A.jump("JXF", "kg_expl"); A.op("CHKLEN", 0, 0, 0, 0, 0); A.jump("JF", "bad_len"); A.op("RNGB", 0, 0, 0, 0, desc(SC, SC_D, 64)); A.jump("JMP", "kg_go")
    A.label("kg_expl"); A.op("CHKLEN", 0, 0, 0, 0, 64); A.jump("JF", "bad_len"); A.op("BCPY", 0, 0, 0, 0, desc(SC, SC_D, 64), desc(RX, 0))
    A.label("kg_go")
    A.op("SHINIT", 3); A.op("SHABSB", 0, 0, 0, 0, desc(SC, SC_D, 32)); A.op("SHABSX", 3); A.op("SHFIN"); A.op("SHSQB", 0, 0, 0, 0, desc(SC, SC_G, 64))
    A.op("BCPY", 0, 0, 0, 0, desc(TX, 16, 32), desc(SC, SC_Z))                                        # z
    A.op("NCLR")
    loop_i(A, "kg_s", lambda: (A.op("SHINIT", 1), A.op("SHABSB", 0, 0, 0, 0, desc(SC, SC_SIG, 32)), A.op("SHABSX", 2), A.op("SHFIN"),
                               A.op("SMPC", IX_I + S0, 0), A.op("PNTT", IX_I + S0)))
    loop_i(A, "kg_sp", lambda: (A.op("PCOPY", TT, IX_I + S0), A.op("PSCL", TT, 0, 0, 0, KM.R_MOD), A.op("PCADDQ", TT),
                                A.op("KPK", TT, 12, 0, 0, d_(TX, 80, 0, IDX_I))))
    def kg_row():
        A.op("PCLR", T); A.op("PCLR", TN)
        loop_i(A, "kg_c", lambda: (A.op("SHINIT", 0), A.op("SHABSB", 0, 0, 0, 0, desc(SC, SC_RHO, 32)), A.op("SHABSX", 1), A.op("SHABSX", 0), A.op("SHFIN"),
                                   A.op("SMPK", TA), A.op("PMULM", T, TA, IX_J + S0), A.op("PMULN", TN, TA, IX_J + S0)), reg=1)
        A.op("PCOMB", T, T, TN)
        A.op("PSCL", T, 0, 0, 0, KM.R2_MOD)
        A.op("SHINIT", 1); A.op("SHABSB", 0, 0, 0, 0, desc(SC, SC_SIG, 32)); A.op("SHABSX", 2); A.op("SHFIN"); A.op("SMPC", E, 0); A.op("PNTT", E)
        A.op("PADD", T, T, E); A.op("PSCL", T, 0, 0, 0, KM.R_MOD); A.op("PCADDQ", T)
        A.op("KPK", T, 12, 0, 0, d_(TX, 80, 0, OFF_K | IDX_I))
    loop_i(A, "kg_r", kg_row)
    A.op("BCPY", 0, 0, 0, 0, d_(TX, 80, 32, OFF_2K), desc(SC, SC_RHO))                            # rho at the end of ek
    A.op("SHINIT", 2); A.op("SHABSB", 0, 0, 0, 0, d_(TX, 80, 0, OFF_K | LEN_EK)); A.op("SHFIN"); A.op("SHSQB", 0, 0, 0, 0, desc(TX, 48, 32))   # h
    A.op("BCPY", 0, 0, 0, 0, d_(TX, 128, 0, OFF_2K | LEN_EK), d_(TX, 80, 0, OFF_K))                 # public ek after the blob (plaintext)
    A.op("RNGB", 0, 0, 0, 0, desc(TX, 0, 16))
    def pieces(op, space, base_off, tagmode=False):
        A.op(op, 0, 0, 0, 0, desc(space, base_off, 64)); A.op(op, 0, 0, 0, 0, d_(space, base_off + 64, 0, LEN_S)); A.op(op, 0, 0, 0, 0, d_(space, base_off + 64, 0, OFF_K | LEN_EK))
    wrap_stream(A, desc(TX, 0, 16), 0x45); A.op("SHFIN"); pieces("SHXORKS", TX, 16)
    wrap_stream(A, desc(TX, 0, 16), 0x4D); pieces("SHABSB", TX, 16); A.op("SHFIN"); A.op("SHSQB", 0, 0, 0, 0, d_(TX, 112, 16, OFF_2K))
    A.op("FINK", 0, 2)

    # ------------------------------------------------------------------ PKE encryption (ek at RX[0], m at SC_M, r at SC_R) -> c at TX[32..]
    def pke_enc():
        A.op("NCLR")
        loop_i(A, "pe_y", lambda: (A.op("SHINIT", 1), A.op("SHABSB", 0, 0, 0, 0, desc(SC, SC_R, 32)), A.op("SHABSX", 2), A.op("SHFIN"),
                                   A.op("SMPC", IX_I + S0, 0), A.op("PNTT", IX_I + S0)))
        def row():
            A.op("PCLR", T); A.op("PCLR", TN)
            loop_i(A, "pe_c", lambda: (A.op("SHINIT", 0), A.op("SHABSB", 0, 0, 0, 0, d_(RX, 0, 32, OFF_K)), A.op("SHABSX", 0), A.op("SHABSX", 1), A.op("SHFIN"),
                                       A.op("SMPK", TA), A.op("PMULM", T, TA, IX_J + S0), A.op("PMULN", TN, TA, IX_J + S0)), reg=1)
            A.op("PCOMB", T, T, TN); A.op("PINTT", T)
            A.op("SHINIT", 1); A.op("SHABSB", 0, 0, 0, 0, desc(SC, SC_R, 32)); A.op("SHABSX", 2); A.op("SHFIN"); A.op("SMPC", E, 2)
            A.op("PADD", T, T, E); A.op("PSCL", T, 0, 0, 0, KM.R_MOD); A.op("PCADDQ", T)
            A.op("KPK", T, D_DU, 1, 0, d_(TX, 32, 0, IDX_I))
        loop_i(A, "pe_r", row)
        A.op("PCLR", V); A.op("PCLR", TN)
        loop_i(A, "pe_v", lambda: (A.op("KUPK", TT, 12, 0, 0, d_(RX, 0, 0, IDX_J)), A.op("PMULM", V, TT, IX_J + S0), A.op("PMULN", TN, TT, IX_J + S0)), reg=1)
        A.op("PCOMB", V, V, TN); A.op("PINTT", V)
        A.op("SHINIT", 1); A.op("SHABSB", 0, 0, 0, 0, desc(SC, SC_R, 32)); A.op("SHABSX", 2); A.op("SHFIN"); A.op("SMPC", E, 2)
        A.op("PADD", V, V, E); A.op("KUPK", MU, 1, 1, 0, desc(SC, SC_M)); A.op("PADD", V, V, MU)
        A.op("PSCL", V, 0, 0, 0, KM.R_MOD); A.op("PCADDQ", V); A.op("KPK", V, D_DV, 1, 0, d_(TX, 32, 0, OFF_C1))

    # ------------------------------------------------------------------ encaps
    A.label("enc"); A.op("KEM", 1)
    A.op("SETF0")
    loop_i(A, "en_mc", lambda: (A.op("KUPK", TT, 12, 0, 0, d_(RX, 0, 0, IDX_I)), A.op("KPK", TT, 12, 0, 0, d_(SC, SC_TMP, 0, IDX_I))))
    A.op("BCMP", 0, 0, 0, 0, d_(SC, SC_TMP, 0, LEN_S), desc(RX, 0)); A.jump("JF", "bad_key")
    A.op("SHINIT", 2); A.op("SHABSB", 0, 0, 0, 0, d_(RX, 0, 0, LEN_EK)); A.op("SHFIN"); A.op("SHSQB", 0, 0, 0, 0, desc(SC, SC_H, 32))
    A.op("SHINIT", 3); A.op("SHABSB", 0, 0, 0, 0, desc(SC, SC_M, 32)); A.op("SHABSB", 0, 0, 0, 0, desc(SC, SC_H, 32)); A.op("SHFIN"); A.op("SHSQB", 0, 0, 0, 0, desc(SC, SC_G, 64))
    A.op("XSET0")
    A.label("pke"); pke_enc()
    A.jump("JXF", "dec_tail")
    A.op("BCPY", 0, 0, 0, 0, desc(TX, 0, 32), desc(SC, SC_K)); A.op("FINK", 0, 1)

    # ------------------------------------------------------------------ decaps
    A.label("dec"); A.op("KEM", 1)
    A.op("CHKLN", 2); A.jump("JF", "bad_len")
    wrap_stream(A, desc(RX, 0, 16), 0x4D); pieces("SHABSB", RX, 16); A.op("SHFIN"); A.op("SHSQB", 0, 0, 0, 0, desc(SC, SC_TAG, 16))
    A.op("SETF0"); A.op("BCMP", 0, 0, 0, 0, desc(SC, SC_TAG, 16), d_(RX, 112, 0, OFF_2K)); A.jump("JF", "auth_fail")
    wrap_stream(A, desc(RX, 0, 16), 0x45); A.op("SHFIN"); pieces("SHXORKS", RX, 16)
    A.op("BCPY", 0, 0, 0, 0, desc(SC, SC_Z, 32), desc(RX, 16)); A.op("BCPY", 0, 0, 0, 0, desc(SC, SC_H, 32), desc(RX, 48))
    A.op("BCPY", 0, 0, 0, 0, d_(TX, TX_CT, 0, LEN_CT), d_(RX, 128, 0, OFF_2K))                      # ct -> TX[4096..]
    loop_i(A, "de_u", lambda: (A.op("KUPK", IX_I + U0, D_DU, 1, 0, d_(TX, TX_CT, 0, IDX_I)), A.op("PNTT", IX_I + U0)))
    A.op("KUPK", V, D_DV, 1, 0, d_(TX, TX_CT, 0, OFF_C1))
    A.op("PCLR", T); A.op("PCLR", TN)
    loop_i(A, "de_s", lambda: (A.op("KUPK", TT, 12, 0, 0, d_(RX, 80, 0, IDX_J)), A.op("PMULM", T, TT, IX_J + U0), A.op("PMULN", TN, TT, IX_J + U0)), reg=1)
    A.op("PCOMB", T, T, TN); A.op("PINTT", T); A.op("PSUB", T, V, T); A.op("PSCL", T, 0, 0, 0, KM.R_MOD); A.op("PCADDQ", T); A.op("KPK", T, 1, 1, 0, desc(SC, SC_M))
    A.op("SHINIT", 3); A.op("SHABSB", 0, 0, 0, 0, desc(SC, SC_M, 32)); A.op("SHABSB", 0, 0, 0, 0, desc(SC, SC_H, 32)); A.op("SHFIN"); A.op("SHSQB", 0, 0, 0, 0, desc(SC, SC_G, 64))
    A.op("BCPY", 0, 0, 0, 0, d_(RX, 0, 0, LEN_EK), d_(RX, 80, 0, OFF_K))                           # ek -> RX[0..] (the key material before it is no longer needed)
    A.jump("JMP", "pke")
    A.label("dec_tail")
    A.op("SETF0"); A.op("BCMP", 0, 0, 0, 0, d_(TX, 32, 0, LEN_CT), desc(TX, TX_CT))
    A.op("SHINIT", 1); A.op("SHABSB", 0, 0, 0, 0, desc(SC, SC_Z, 32)); A.op("SHABSB", 0, 0, 0, 0, d_(TX, TX_CT, 0, LEN_CT)); A.op("SHFIN"); A.op("SHSQB", 0, 0, 0, 0, desc(SC, SC_KB, 32))
    A.op("BCPYN", 0, 0, 0, 0, desc(TX, 0, 32), desc(SC, SC_K)); A.op("BCPYF", 0, 0, 0, 0, desc(TX, 0, 32), desc(SC, SC_KB))
    A.op("FINK", 0, 0)
    A.label("bad_len"); A.op("FIN", 2, 0, 0, 0, 0)
    A.label("bad_key"); A.op("FIN", 3, 0, 0, 0, 0)
    A.label("auth_fail"); A.op("FIN", 4, 0, 0, 0, 0)
    A.code = A.link(); return A

CMDS = {}
for _p in range(3):
    CMDS.update({0x80 + _p: f"kg_trng{_p}", 0x84 + _p: f"kg_expl{_p}", 0x88 + _p: f"enc_trng{_p}", 0x8C + _p: f"enc_expl{_p}", 0x90 + _p: f"dec{_p}"})

if __name__ == "__main__":
    import hashlib, mlkem
    master = bytes(range(32)); A = prog_kem(0); code = A.code
    print(len(code), "microcode words")
    for p, level in enumerate((512, 768, 1024)):
        k, e1, e2, du, dv = PARAMS[level]
        d, z, m = os.urandom(32), os.urandom(32), os.urandom(32)
        ek, dk = mlkem.keygen(d, z, level); K, c = mlkem.encaps(ek, m, level)
        dev = P.Dev(d + z, master, trng=lambda n: bytes(range(100, 100 + n))); P.run(code, A.entries[f"kg_expl{p}"], dev)
        assert dev.fin == (0, 1152 * k + 160), dev.fin
        blob = bytes(dev.tx[:768 * k + 128]); pub = bytes(dev.tx[768 * k + 128:dev.fin[1]])
        assert pub == ek, "ek"
        nonce = bytes(range(100, 116))
        plain = z + hashlib.sha3_256(ek).digest() + dk[:384 * k] + ek
        assert blob == P.wrap_py(master, plain, nonce), "blob"
        dev = P.Dev(ek + m, master); P.run(code, A.entries[f"enc_expl{p}"], dev)
        assert dev.fin == (0, 32 + len(c)) and bytes(dev.tx[:32]) == K and bytes(dev.tx[32:32 + len(c)]) == c, "encaps"
        dev = P.Dev(ek, master, trng=lambda n: m); P.run(code, A.entries[f"enc_trng{p}"], dev)
        assert bytes(dev.tx[:32]) == K and bytes(dev.tx[32:32 + len(c)]) == c, "encaps trng"
        dev = P.Dev(blob + c, master); P.run(code, A.entries[f"dec{p}"], dev)
        assert dev.fin == (0, 32) and bytes(dev.tx[:32]) == K, "decaps"
        bad = bytearray(c); bad[9] ^= 2
        dev = P.Dev(blob + bytes(bad), master); P.run(code, A.entries[f"dec{p}"], dev)
        assert bytes(dev.tx[:32]) == mlkem.decaps(dk, bytes(bad), level), "implicit rejection"
        bb = bytearray(blob); bb[100] ^= 1; dev = P.Dev(bytes(bb) + c, master); P.run(code, A.entries[f"dec{p}"], dev); assert dev.fin[0] == 4
        bk = bytearray(ek); bk[0] = 0xFF; bk[1] |= 0x0F; dev = P.Dev(bytes(bk) + m, master); P.run(code, A.entries[f"enc_expl{p}"], dev); assert dev.fin[0] == 3
        dev = P.Dev(ek[:-1], master); P.run(code, A.entries[f"enc_trng{p}"], dev); assert dev.fin[0] == 2
        print(f"ML-KEM-{level}: microcode keygen / encaps / decaps / implicit rejection / tamper checks == reference")
    print("ALL PASS")
