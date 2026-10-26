#!/usr/bin/env python3
"""Generate tb/tb_pq_kpoly.v: ML-KEM mode of pq_poly (NTT, INTT, basemul, basemul+acc, PSCL, canonicalisation)."""
import os, random, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kem_model as M
from mlkem import Q
random.seed(int(sys.argv[1]) if len(sys.argv) > 1 else 7)
def rp(lo, hi): return [random.randint(lo, hi) for _ in range(256)]
OPS = dict(NTT=0, INTT=1, PCADDQ=7, PMULM=14, PMULN=15, PSCL=16, PCOMB=17)
cases = []
def add(name, op, abc, ins, outs, bound=0): cases.append((name, OPS[op], abc, bound, ins, outs))
f = rp(0, Q - 1); g = rp(0, Q - 1); h = rp(-3, 3)
add("kntt", "NTT", (1, 0, 0), {1: f}, {1: M.ntt(f)})
add("kntt_small", "NTT", (2, 0, 0), {2: h}, {2: M.ntt(h)})
n = M.ntt(f); add("kintt", "INTT", (3, 0, 0), {3: n}, {3: M.intt(n)})
nf, ng = M.ntt(f), M.ntt(g)
m0, n0 = rp(-9, 9), rp(-9, 9)
add("kpmulm", "PMULM", (4, 5, 6), {5: nf, 6: ng, 4: m0}, {4: M.pmulm(m0, nf, ng)})
add("kpmuln", "PMULN", (7, 5, 6), {5: nf, 6: ng, 7: n0}, {7: M.pmuln(n0, nf, ng)})
mm, nn = M.pmulm(m0, nf, ng), M.pmuln(n0, nf, ng)
add("kpcomb", "PCOMB", (10, 4, 7), {4: mm, 7: nn}, {10: M.pcomb(mm, nn)})
acc = M.pcomb(*M.basemul2(nf, ng)); assert [x % Q for x in M.tomont(acc)] == __import__("mlkem").basemul(__import__("mlkem").ntt(f), __import__("mlkem").ntt(g))
add("kpscl_tomont", "PSCL", (8, 0, 0), {8: acc}, {8: M.pscl(acc, M.R2_MOD)}, bound=M.R2_MOD)
add("kpscl_canon", "PSCL", (9, 0, 0), {9: acc}, {9: M.pscl(acc, M.R_MOD)}, bound=M.R_MOD)
cq = M.pscl(acc, M.R_MOD); add("kcaddq", "PCADDQ", (9, 0, 0), {9: cq}, {9: [x % Q for x in cq]})
o = ["`timescale 1ns/1ps", "module tb_pq_kpoly;", "reg clk=0, rst_n=0; always #5 clk=~clk;",
     "reg start=0; reg [4:0] op=0; reg [5:0] sa=0,sb=0,sc=0; reg g2sel=0; reg [31:0] bound=0;",
     "wire busy, done, flag_or; wire [8:0] hint_cnt; wire [13:0] ra, wa; wire we; wire [31:0] wd, rq;",
     "pq_pm pm(.clk(clk), .we(we), .wa(wa), .wd(wd), .ra(ra), .rq(rq));",
     "pq_poly dut(clk,rst_n,start,op,sa,sb,sc,g2sel,bound,busy,done,flag_or,hint_cnt,ra,we,wa,wd,rq,1'b1);",
     "integer fails=0, n=0, i, c0, cyc=0; always @(posedge clk) cyc<=cyc+1; reg seen=0; always @(posedge clk) if (done) seen<=1;",
     "task setp(input [5:0] s, input [8191:0] v); begin for (i=0;i<256;i=i+1) pm.mem[s*256+i] = v[32*i +: 32]; end endtask",
     "task chk(input [5:0] s, input [8191:0] v, input [255:0] nm); begin for (i=0;i<256;i=i+1) if (pm.mem[s*256+i] !== v[32*i +: 32]) begin",
     "   if (fails < 10) $display(\"FAIL slot %0d idx %0d got %h exp %h\", s, i, pm.mem[s*256+i], v[32*i +: 32]); fails=fails+1; end end endtask",
     "task run(input [4:0] o, input [5:0] a, input [5:0] b, input [5:0] c, input [31:0] bd); begin",
     "  seen=0; c0=cyc; @(negedge clk); op=o; sa=a; sb=b; sc=c; bound=bd; start=1; @(negedge clk); start=0; wait(seen); repeat(3) @(negedge clk); end endtask",
     "initial begin #30 rst_n=1; #30;"]
def lit(pl):
    v = 0
    for i, x in enumerate(pl): v |= (x & 0xFFFFFFFF) << (32 * i)
    return f"8192'h{v:02048x}"
for name, op, (a, b, c), bound, ins, outs in cases:
    for s, pl in ins.items(): o.append(f"  setp({s}, {lit(pl)});")
    o.append(f"  run({op},{a},{b},{c},{bound & 0xFFFFFFFF}); $display(\"{name}: %0d clk\", cyc-c0);")
    for s, pl in outs.items(): o.append(f"  chk({s}, {lit(pl)}, \"{name}\");")
    o.append("  n=n+1;")
o += ['  if (fails==0) $display("ALL PASS (%0d ops)", n); else $display("FAILED (%0d mismatches)", fails);', "  $finish;", "end",
      'initial begin #800000000 $display("TIMEOUT"); $finish; end', "endmodule"]
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tb", "tb_pq_kpoly.v"), "w").write("\n".join(o) + "\n")
print(len(cases), "cases")
