`default_nettype none
// Pipelined Montgomery multiply for ML-DSA (and ML-KEM with kem=1: q = 3329): r = montgomery_reduce((int64)a * b)   (q = 8380417, QINV = 58728449)
// latency 4 clocks, one result per clock.
module pq_mont (
    input  wire               clk,
    input  wire               in_v,
    input  wire signed [31:0] a,
    input  wire signed [31:0] b,
    input  wire               kem,                                         // 1: ML-KEM (q = 3329, QINV = q^-1 mod 2^32)
    output reg                out_v,
    output reg  signed [31:0] r
);
localparam signed [31:0] Q = 32'sd8380417;
`ifdef MONT_2CYC
// experiment: every stage captures only every second clock (a multiplier then has two clocks to settle)
reg ph = 0; always @(posedge clk) ph <= ~ph;
wire ce = ph;
`else
wire ce = 1'b1;
`endif
reg v1, v2, v3;
reg signed [63:0] p1, p2, p3, m3;
reg [31:0] t2;
reg pend = 0;                                                            // a start pulse that arrives between two capture clocks
always @(posedge clk) if (!ce && in_v) pend <= 1'b1; else if (ce) pend <= 1'b0;
always @(posedge clk) if (ce) begin
    v1 <= in_v | pend;  p1 <= a * b;                                           // stage 1: full product
    v2 <= v1;    p2 <= p1; t2 <= p1[31:0] * (kem ? 32'h6BA8F301 : 32'd58728449);               // stage 2: t = (int32)(p * QINV)
    v3 <= v2;    m3 <= $signed(t2) * (kem ? 32'sd3329 : Q); p3 <= p2;                       // stage 3: t * q
    // stage 4: (p - t*q) >> 32. By construction t*q == p (mod 2^32), the low halves cancel exactly (no borrow), so a 32-bit
    // subtraction of the high halves is bit-exact with the 64-bit one.
    out_v <= v3; r <= p3[63:32] - m3[63:32];
end
endmodule
`default_nettype wire
