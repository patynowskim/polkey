`default_nettype none
// Experiment only: N unrelated registered 4-input functions fed by an LFSR, reduced to one output bit, to find how much
// extra logic the placer can still handle. Not part of any product build (enabled by -DBURN=n).
module lut_burner #(parameter N = 300) (input wire clk, output wire o);
reg [31:0] l = 32'hACE1ACE1;
always @(posedge clk) l <= {l[30:0], l[31] ^ l[21] ^ l[1] ^ l[0]};
(* keep *) reg [N-1:0] q = 0;
genvar i;
generate for (i = 0; i < N; i = i + 1) begin : g
    always @(posedge clk) q[i] <= (l[(i*7) % 32] & l[(i*11 + 3) % 32]) ^ (l[(i*5 + 9) % 32] | l[(i*13 + 1) % 32]) ^ q[(i + 1) % N];
end endgenerate
assign o = ^q;
endmodule
`default_nettype wire
