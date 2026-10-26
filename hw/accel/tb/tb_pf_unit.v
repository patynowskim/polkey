`timescale 1ns/1ps
// pf_unit: fingerprint UART transaction (echo of a canned reply, gap/timeout ends) and OLED SPI bytes
module tb_pf_unit;
reg clk = 0; always #5 clk = ~clk;                       // 100 MHz "FSYS_MHZ=1" => 1 ms = 1000 clk is too slow; use FSYS_MHZ=2 -> 2000 clk/ms
reg rst_n = 0, go = 0; reg [7:0] cmd = 0; reg [15:0] len = 0;
wire [12:0] mem_a, tx_wa; wire tx_we, done, busy, fp_tx; wire [7:0] tx_wd, status; wire [15:0] tx_len;
reg fp_rx = 1; wire osck, omosi, ocs, odc, ores;
reg [7:0] ram [0:63]; reg [7:0] mem_q; always @(posedge clk) mem_q <= ram[mem_a[5:0]];
reg [7:0] txr [0:63];
always @(posedge clk) if (tx_we) txr[tx_wa[5:0]] <= tx_wd;
localparam MHZ = 2, BIT = (MHZ*1000000/57600) * 10;      // ns per bit at 100 MHz clock: CPB clocks * 10 ns
pf_unit #(.FSYS_MHZ(MHZ), .SPI_HALF(2)) dut (.clk(clk), .rst_n(rst_n), .go(go), .cmd(cmd), .len(len), .mem_a(mem_a), .mem_q(mem_q),
    .tx_we(tx_we), .tx_wa(tx_wa), .tx_wd(tx_wd), .done(done), .status(status), .tx_len(tx_len), .busy(busy),
    .fp_tx(fp_tx), .fp_rx(fp_rx), .oled_sck(osck), .oled_mosi(omosi), .oled_cs_n(ocs), .oled_dc(odc), .oled_res_n(ores));
integer fails = 0, i;
task check(input [255:0] what, input ok); begin if (!ok) begin fails = fails + 1; $display("FAIL: %0s", what); end else $display("ok:   %0s", what); end endtask
task run(input [7:0] c, input [15:0] l); begin
    cmd = c; len = l; @(posedge clk); go = 1; @(posedge clk); go = 0; while (!done) @(posedge clk);
end endtask
task send_rx(input [7:0] d); integer k; begin   // drive a byte on fp_rx
    fp_rx = 0; #(BIT/10*10); for (k = 0; k < 8; k = k + 1) begin fp_rx = d[k]; #(BIT/10*10); end fp_rx = 1; #(BIT/10*10);
end endtask
reg [7:0] cap; integer cn; reg [7:0] capb [0:15];
// capture what the DUT sends on fp_tx
initial begin cn = 0; forever begin @(negedge fp_tx); #(BIT/10*10/2*10/10); #(BIT/10*10); cap = 0;
    for (i = 0; i < 8; i = i + 1) begin cap[i] = fp_tx; #(BIT/10*10); end capb[cn] = cap; cn = cn + 1; end end
// SPI capture
reg [7:0] spib [0:15]; integer sn = 0; reg [7:0] ssh; integer sbit = 0;
always @(posedge osck) begin ssh = {ssh[6:0], omosi}; sbit = sbit + 1; if (sbit == 8) begin spib[sn] = ssh; sn = sn + 1; sbit = 0; end end
initial begin
    repeat (4) @(posedge clk); rst_n = 1; repeat (20) @(posedge clk);
    // FP_XFER: gap 2 ms, timeout 50 ms, max 8, tx = EF 01
    ram[0] = 2; ram[1] = 50; ram[2] = 0; ram[3] = 8; ram[4] = 0; ram[5] = 8'hEF; ram[6] = 8'h01;
    fork
        run(8'h06, 7);
        begin #(BIT*25); send_rx(8'hAA); send_rx(8'hBB); send_rx(8'hCC); end
    join
    check("fp: 3 bytes then gap end", status == 0 && tx_len == 3 && txr[0] == 8'hAA && txr[1] == 8'hBB && txr[2] == 8'hCC);
    check("fp: tx bytes on the wire", cn == 2 && capb[0] == 8'hEF && capb[1] == 8'h01);
    ram[3] = 4;                                              // max 4
    fork
        run(8'h06, 5);
        begin #(BIT*5); send_rx(8'h11); send_rx(8'h22); send_rx(8'h33); send_rx(8'h44); send_rx(8'h55); end
    join
    check("fp: stops at max_rx", tx_len == 4 && txr[3] == 8'h44);
    ram[0] = 2; ram[1] = 3; ram[3] = 8; run(8'h06, 5);
    check("fp: timeout with nothing received", status == 0 && tx_len == 0);
    run(8'h06, 3); check("fp: short header -> status 2", status == 2);
    // OLED: ctl = DC1 RES1 apply, data A5 3C
    ram[0] = 8'b111; ram[1] = 8'hA5; ram[2] = 8'h3C; sn = 0; sbit = 0;
    run(8'h07, 3);
    check("oled: bytes shifted", sn == 2 && spib[0] == 8'hA5 && spib[1] == 8'h3C);
    check("oled: lines", odc == 1 && ores == 1 && ocs == 1 && osck == 0);
    ram[0] = 8'b100; run(8'h07, 1); check("oled: RES low applied", ores == 0 && ocs == 1 && status == 0);
    run(8'h08, 1); check("unknown -> status 1", status == 1);
    if (fails == 0) $display("PF UNIT PASS"); else $display("PF UNIT FAIL %0d", fails);
    $finish;
end
initial begin #200000000; $display("TIMEOUT"); $finish; end
endmodule
