`timescale 1ns/1ps
// ux_unit: debounce, latch + expiry, read/clear, LED control, bad length
module tb_ux_unit;
reg clk = 0; always #5 clk = ~clk;
reg rst_n = 0, btn = 0, go = 0; parameter INVERT = 0; reg [7:0] cmd = 0; reg [15:0] len = 0;
wire [12:0] mem_a, tx_wa; reg [7:0] mem_q; wire tx_we; wire [7:0] tx_wd; wire done, busy; wire [7:0] status, led_ctl; wire [15:0] tx_len;
ux_unit #(.DEB_BITS(4), .LATCH_MAX(2000)) dut (.clk(clk), .rst_n(rst_n), .btn(btn), .go(go), .cmd(cmd), .len(len), .mem_a(mem_a), .mem_q(mem_q),
    .tx_we(tx_we), .tx_wa(tx_wa), .tx_wd(tx_wd), .done(done), .status(status), .tx_len(tx_len), .busy(busy), .led_ctl(led_ctl), .diag(6'd0));
reg [7:0] resp; integer fails = 0;
task run(input [7:0] c, input [15:0] l, input [7:0] payload); begin
    mem_q = payload; cmd = c; len = l; @(posedge clk); go = 1; @(posedge clk); go = 0;
    resp = 8'hxx;
    while (!done) begin @(posedge clk); if (tx_we) resp = tx_wd; end
end endtask
task check(input [255:0] what, input ok); begin
    if (!ok) begin fails = fails + 1; $display("FAIL: %0s", what); end else $display("ok:   %0s", what);
end endtask
initial begin
    btn = INVERT; repeat (4) @(posedge clk); rst_n = 1; repeat (200) @(posedge clk);
    run(8'h04, 1, 8'h00); check("idle: not pressed, not latched", resp === 8'h00 && status == 0 && tx_len == 1);
    btn = !INVERT; repeat (5) @(posedge clk);                       // bounce shorter than the debounce time
    btn = INVERT; repeat (40) @(posedge clk);
    run(8'h04, 1, 8'h00); check("glitch is ignored", resp === 8'h00);
    btn = !INVERT; repeat (60) @(posedge clk);
    run(8'h04, 1, 8'h00); check("held: pressed + latched", resp === 8'h03);
    btn = INVERT; repeat (60) @(posedge clk);
    run(8'h04, 1, 8'h00); check("released: latch remains", resp === 8'h02);
    run(8'h04, 1, 8'h01); check("read with clear returns the latch", resp === 8'h02);
    run(8'h04, 1, 8'h00); check("latch cleared by the read", resp === 8'h00);
    btn = !INVERT; repeat (60) @(posedge clk); btn = INVERT; repeat (60) @(posedge clk);
    run(8'h04, 1, 8'h00); check("new press latches again", resp === 8'h02);
    repeat (2100) @(posedge clk);
    run(8'h04, 1, 8'h00); check("latch expires", resp === 8'h00);
    run(8'h05, 1, 8'h85); check("LED command accepted", status == 0 && tx_len == 0 && led_ctl == 8'h85);
    run(8'h05, 1, 8'h00); check("LED back to status display", led_ctl == 8'h00);
    run(8'h04, 2, 8'h00); check("bad length -> status 2", status == 2);
    run(8'h06, 1, 8'h00); check("unknown command -> status 1", status == 1);
    if (fails == 0) $display("UX UNIT PASS"); else $display("UX UNIT FAIL %0d", fails);
    $finish;
end
initial begin #20000000; $display("TIMEOUT"); $finish; end
endmodule
