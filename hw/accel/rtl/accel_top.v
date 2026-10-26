`default_nettype none
// Tang Nano 20k crypto accelerator top: framed UART command link + Root of Trust.
// Profiles (chparam at build time), freely combinable:  HAS_SYM=1 -> SHA-256/HMAC/PBKDF2;  HAS_AES=1 -> sym incl. AES-256 CBC/GCM (implies HAS_SYM);
//   HAS_ECC=1 -> asym (P-256/Ed25519/SHA-512);  HAS_PQ=1 -> ML-DSA-44/65/87 + RSA-2048 (needs rtl_pq/).
module accel_core #(parameter CPB = 18, parameter HAS_AES = 1, parameter HAS_SYM = 0, parameter HAS_ECC = 0, parameter HAS_PQ = 0, parameter FSYS_MHZ = 27)(      // CPB = clock / 1.5 Mbaud (27 MHz: 18)
    input  wire clk,               // system clock (PLL output or the 27 MHz board clock)
    input  wire pll_lock,          // 0 holds the core in reset until the clock is stable
    input  wire [5:0] diag_in,     // diagnostic value shown in the button status byte
    input  wire btn_s1,            // active high (reset)
    input  wire btn_s2,            // active high (user presence)
    output wire led0, led1, led2,  // active low
    output wire uart_tx_pin,
    input  wire uart_rx_pin,
    // SPI slave link (ESP32 = master, mode 0)
    input  wire spi_sck, input wire spi_mosi, input wire spi_cs_n, output wire spi_miso,
    // peripherals bridged for the MCU: fingerprint sensor UART, SSD1306 OLED SPI
    output wire fp_tx, input wire fp_rx,
    output wire oled_sck, output wire oled_mosi, output wire oled_cs_n, output wire oled_dc, output wire oled_res_n
);
// power-on reset (or S1 held)
reg [16:0] por = 17'd0;
always @(posedge clk) begin
    if (btn_s1 || !pll_lock) por <= 17'd0; else if (!por[16]) por <= por + 17'd1;
end
wire rst_n = por[16];

// Root of Trust: UID + TRNG
`ifdef UID_VAL
localparam [127:0] UID_PARAM = `UID_VAL;
`else
localparam [127:0] UID_PARAM = 128'hA1B2C3D4_E5F60718_293A4B5C_6D7E8F00;
`endif
`ifdef MASTER_VAL
localparam [255:0] MASTER = `MASTER_VAL;
`else
localparam [255:0] MASTER = 256'h000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f;
`endif
wire [127:0] uid; wire [31:0] trng_data; wire trng_valid;
tro_lite u_tro (.clk(clk), .rst_n(rst_n), .uid_o(uid), .trng_o(trng_data), .trng_valid(trng_valid));

// link
wire u_valid; wire [7:0] u_byte;
uart_rx #(.CPB(CPB)) u_rx (.clk(clk), .rst_n(rst_n), .rx(uart_rx_pin), .valid(u_valid), .data(u_byte));
wire s_valid, s_take, s_have; wire [7:0] s_byte, s_data;
spi_slave u_spi (.clk(clk), .rst_n(rst_n), .sck(spi_sck), .mosi(spi_mosi), .cs_n(spi_cs_n), .miso(spi_miso),
                 .rx_valid(s_valid), .rx_byte(s_byte), .tx_data(s_data), .tx_have(s_have), .tx_take(s_take));
// the reply goes back over the transport on which the last request start byte (0xA5) arrived
reg via_spi;
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) via_spi <= 1'b0;
    else if (s_valid && s_byte == 8'hA5) via_spi <= 1'b1;
    else if (u_valid && u_byte == 8'hA5) via_spi <= 1'b0;
end
wire rx_valid = u_valid | s_valid;
wire [7:0] rx_byte = u_valid ? u_byte : s_byte;

wire cmd_busy, tx_busy;
wire [12:0] rx_wa; wire rx_we; wire [7:0] rx_wd;
wire frame_ok, frame_bad; wire [7:0] f_cmd; wire [15:0] f_len;
frame_rx u_frx (.clk(clk), .rst_n(rst_n), .byte_valid(rx_valid), .byte_in(rx_byte),
                .accept(!cmd_busy && !tx_busy), .wa(rx_wa), .we(rx_we), .wd(rx_wd),
                .frame_ok(frame_ok), .frame_bad(frame_bad), .cmd(f_cmd), .len(f_len));

wire [12:0] mem_a, a_mem_a, e_mem_a; wire [7:0] mem_q;
wire [12:0] tx_ra; wire [7:0] tx_rq;
wire a_we, e_we; wire [12:0] a_wa, e_wa; wire [7:0] a_wd, e_wd;
wire a_done, e_done, a_busy, e_busy; wire [7:0] a_status, e_status; wire [15:0] a_len, e_len;
wire q_done, q_busy; wire [7:0] q_status; wire [15:0] q_len;
wire [12:0] q_rx_ra, q_rx_wa, q_tx_wa, q_tx_ra; wire q_rx_we, q_tx_we; wire [7:0] q_rx_wd, q_tx_wd;

wire pf_we, pf_done, pf_busy; wire [12:0] pf_wa, pf_mem_a; wire [7:0] pf_wd, pf_status; wire [15:0] pf_len;
wire ux_we, ux_done, ux_busy; wire [12:0] ux_wa; wire [7:0] ux_wd, ux_status, led_ctl; wire [15:0] ux_len;
// ---- command classes: 0x0_ info, 0x1_/0x2_ symmetric, 0x3_..0x5_ asymmetric (ECC), 0x6_/0x7_ ML-DSA + RSA ----
// Info commands (ping/uid/rand) are answered by one processor: PQ if present, else ECC, else symmetric.
wire is_ux    = (f_cmd == 8'h04) || (f_cmd == 8'h05);          // user presence button / LEDs: every profile
wire is_pf    = (f_cmd == 8'h06) || (f_cmd == 8'h07);          // fingerprint UART / OLED SPI bridge: every profile
wire cls_info = (f_cmd[7:4] == 4'h0) && !is_ux && !is_pf;
wire cls_sym  = (f_cmd[7:4] == 4'h1) || (f_cmd[7:4] == 4'h2);
wire cls_ecc  = (f_cmd[7:4] == 4'h3) || (f_cmd[7:4] == 4'h4) || (f_cmd[7:4] == 4'h5);
wire cls_pq   = (f_cmd[7:4] == 4'h6) || (f_cmd[7:4] == 4'h7) || (f_cmd[7:4] == 4'h8) || (f_cmd[7:4] == 4'h9);   // ML-DSA, RSA (0x6x, 0x7x), ML-KEM (0x8x, 0x9x)
wire go_q = frame_ok && HAS_PQ && (cls_pq || cls_info);
wire go_e = frame_ok && HAS_ECC && (cls_ecc || (cls_info && !HAS_PQ));
localparam SYM_ON = (HAS_SYM != 0) || (HAS_AES != 0);
wire go_a = frame_ok && SYM_ON && (cls_sym || (cls_info && !HAS_PQ && !HAS_ECC));
// frames for a class that is not built in are answered by the processor owning the info commands (BAD_CMD)
wire go_ux = frame_ok && is_ux;
wire go_pf = frame_ok && is_pf;
wire go_none = frame_ok && !go_q && !go_e && !go_a && !go_ux && !go_pf;
reg  none_pend;
reg  act_q, act_e, act_a, act_ux, act_pf;
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin act_q <= 0; act_e <= 0; act_a <= 0; act_ux <= 0; act_pf <= 0; none_pend <= 0; end
    else begin
        none_pend <= go_none;
        if (go_q) act_q <= 1; else if (q_done) act_q <= 0;
        if (go_e) act_e <= 1; else if (e_done) act_e <= 0;
        if (go_a) act_a <= 1; else if (a_done) act_a <= 0;
        if (go_ux) act_ux <= 1; else if (ux_done) act_ux <= 0;
        if (go_pf) act_pf <= 1; else if (pf_done) act_pf <= 0;
    end
end

// ---- shared bignum engine (RSA-2048 / P-256 / Ed25519 field arithmetic, register file in block RAM) ----
wire        r_start, r_done, r_flag, r_bus; wire [7:0] r_op; wire [5:0] r_a, r_b, r_c, r_d; wire [31:0] r_imm;
wire        rb_we; wire [1:0] rb_ws, rb_rs; wire [12:0] rb_wa, rb_ra; wire [7:0] rb_wd, br_data;
wire        be_start, be_done, be_lt; wire [3:0] be_op; wire [6:0] be_d, be_a, be_b; wire [1:0] be_msel;
wire [10:0] bx_ra, bx_wa; wire bx_we; wire [15:0] bx_wd, bx_rq;
generate if (HAS_PQ || HAS_ECC) begin : g_bn
    bn_engine u_bn (.clk(clk), .rst_n(rst_n), .start(r_start), .op(r_op), .ra(r_a), .rb(r_b), .rc(r_c), .rd(r_d), .imm0(r_imm),
        .done(r_done), .flag(r_flag), .bus_busy(r_bus),
        .bw_en(rb_we), .bw_space(rb_ws), .bw_addr(rb_wa), .bw_data(rb_wd), .br_space(rb_rs), .br_addr(rb_ra), .br_data(br_data),
        .trng_valid(trng_valid), .trng_data(trng_data),
        .e_start(be_start), .e_op(be_op), .e_d(be_d), .e_a(be_a), .e_b(be_b), .e_msel(be_msel), .e_done(be_done), .e_lt(be_lt),
        .x_ra(bx_ra), .x_we(bx_we), .x_wa(bx_wa), .x_wd(bx_wd), .x_rq(bx_rq), .busy_o());
end else begin : g_nobn
    assign r_done = 1'b0; assign r_flag = 1'b0; assign r_bus = 1'b0;
    assign rb_we = 1'b0; assign rb_ws = 2'd0; assign rb_wa = 13'd0; assign rb_wd = 8'd0; assign rb_rs = 2'd0; assign rb_ra = 13'd0;
    assign be_done = 1'b0; assign be_lt = 1'b0; assign bx_rq = 16'd0;
end endgenerate

generate if (HAS_PQ) begin : g_pq
    // ---- ML-DSA / RSA profile: polynomial engine, serial Keccak, bignum engine, microcoded key wrapping ----
    pq_core #(.MASTER(MASTER), .UID(UID_PARAM)) u_pq (.clk(clk), .rst_n(rst_n), .go(go_q), .cmd(f_cmd), .len(f_len),
        .done(q_done), .status(q_status), .tx_len(q_len), .busy(q_busy),
        .rx_ra(q_rx_ra), .rx_rq(mem_q), .rx_we(q_rx_we), .rx_wa(q_rx_wa), .rx_wd(q_rx_wd),
        .tx_we(q_tx_we), .tx_wa(q_tx_wa), .tx_wd(q_tx_wd), .tx_ra(q_tx_ra), .tx_rq(tx_rq),
        .trng_valid(trng_valid), .trng_data(trng_data),
        .r_start_o(r_start), .r_op_o(r_op), .r_a_o(r_a), .r_b_o(r_b), .r_c_o(r_c), .r_d_o(r_d), .r_imm_o(r_imm),
        .r_done_i(r_done), .r_flag_i(r_flag), .r_bus_i(r_bus),
        .rb_we_i(rb_we), .rb_ws_i(rb_ws), .rb_wa_i(rb_wa), .rb_wd_i(rb_wd), .rb_rs_i(rb_rs), .rb_ra_i(rb_ra), .br_data_o(br_data));
end else begin : g_nopq
    assign q_done = 1'b0; assign q_busy = 1'b0; assign q_status = 8'd0; assign q_len = 16'd0;
    assign q_rx_ra = 13'd0; assign q_rx_we = 1'b0; assign q_rx_wa = 13'd0; assign q_rx_wd = 8'd0;
    assign q_tx_we = 1'b0; assign q_tx_wa = 13'd0; assign q_tx_wd = 8'd0; assign q_tx_ra = 13'd0;
    assign r_start = 1'b0; assign r_op = 8'd0; assign r_a = 6'd0; assign r_b = 6'd0; assign r_c = 6'd0; assign r_d = 6'd0; assign r_imm = 32'd0; assign br_data = 8'd0;
end endgenerate

wire        h5_begin, h5_in_valid, h5_end; wire [7:0] h5_in_byte; wire [2:0] h5_ra;
wire        hu_in_ready, hu_done, hu_busy, hu_overrun; wire [63:0] hu_dgq;
generate if (HAS_ECC) begin : g_asym
    // ---- asymmetric profile: everything is microcode on the ECC engine ----
    ecc_cmd #(.UID(UID_PARAM)) u_ecc (.clk(clk), .rst_n(rst_n), .go(go_e), .cmd(f_cmd), .len(f_len),
        .mem_a(e_mem_a), .mem_q(mem_q), .tx_we(e_we), .tx_wa(e_wa), .tx_wd(e_wd),
        .done(e_done), .status(e_status), .tx_len(e_len), .busy(e_busy),
        .trng_valid(trng_valid), .trng_data(trng_data),
        .h5_begin(h5_begin), .h5_in_valid(h5_in_valid), .h5_in_byte(h5_in_byte), .h5_in_ready(hu_in_ready),
        .h5_end(h5_end), .h5_done(hu_done), .h5_ra(h5_ra), .h5_q(hu_dgq),
        .e_start(be_start), .e_op(be_op), .e_d(be_d), .e_a(be_a), .e_b(be_b), .e_msel(be_msel), .e_done(be_done), .e_lt(be_lt),
        .x_ra(bx_ra), .x_we(bx_we), .x_wa(bx_wa), .x_wd(bx_wd), .x_rq(bx_rq));
end else begin : g_noecc
    assign h5_begin = 1'b0; assign h5_in_valid = 1'b0; assign h5_in_byte = 8'd0; assign h5_end = 1'b0; assign h5_ra = 3'd0;
    assign be_start = 1'b0; assign be_op = 4'd0; assign be_d = 7'd0; assign be_a = 7'd0; assign be_b = 7'd0; assign be_msel = 2'd0;
    assign bx_ra = 11'd0; assign bx_we = 1'b0; assign bx_wa = 11'd0; assign bx_wd = 16'd0;
    assign e_mem_a = 13'd0; assign e_we = 1'b0; assign e_wa = 13'd0; assign e_wd = 8'd0;
    assign e_done = 1'b0; assign e_busy = 1'b0; assign e_status = 8'd0; assign e_len = 16'd0;
end endgenerate

// ---- shared SHA-2 unit: SHA-256 / HMAC / PBKDF2 for the symmetric commands, SHA-512 for the ECC microcode ----
wire        ah_begin, ah_keydone, ah_key_wr, ah_in_valid, ah_msg_end; wire [1:0] ah_mode; wire [31:0] ah_iters, ah_idx;
wire [7:0]  ah_key_byte, ah_in_byte; wire [2:0] ah_dgra;
generate if (SYM_ON || HAS_ECC) begin : g_sha2
    sha2_unit hu (.clk(clk), .rst_n(rst_n),
        .begin_op(ah_begin | h5_begin), .mode(h5_begin ? 2'd3 : ah_mode), .iters(ah_iters), .pb_idx(ah_idx),
        .key_wr(ah_key_wr), .key_byte(ah_key_byte), .key_done(ah_keydone),
        .in_valid(ah_in_valid | h5_in_valid), .in_byte(h5_in_valid ? h5_in_byte : ah_in_byte),
        .in_ready(hu_in_ready), .msg_end(ah_msg_end | h5_end), .busy(hu_busy), .done(hu_done), .overrun(hu_overrun),
        .dg_ra(act_e ? h5_ra : ah_dgra), .dg_q(hu_dgq));
end else begin : g_nosha2
    assign hu_in_ready = 1'b0; assign hu_done = 1'b0; assign hu_busy = 1'b0; assign hu_overrun = 1'b0; assign hu_dgq = 64'd0;
end endgenerate

generate if (SYM_ON) begin : g_sym
    // ---- symmetric commands: SHA-256 / HMAC / PBKDF2 (+ AES-256 CBC/GCM); answers the info commands only when alone ----
    accel_cmd #(.HAS_AES(HAS_AES != 0), .HAS_INFO(!HAS_PQ && !HAS_ECC), .CAPS(HAS_AES ? 32'h0000_000F : 32'h0000_0007)) u_cmd (
        .clk(clk), .rst_n(rst_n), .go(go_a), .cmd(f_cmd), .len(f_len),
        .mem_a(a_mem_a), .mem_q(mem_q), .tx_we(a_we), .tx_wa(a_wa), .tx_wd(a_wd),
        .done(a_done), .status(a_status), .tx_len(a_len), .busy(a_busy),
        .uid(uid), .trng_valid(trng_valid), .trng_data(trng_data),
        .h_begin_o(ah_begin), .h_keydone_o(ah_keydone), .h_key_wr_o(ah_key_wr), .h_in_valid_o(ah_in_valid),
        .h_msg_end_o(ah_msg_end), .h_mode_o(ah_mode), .h_iters_o(ah_iters), .h_idx_o(ah_idx),
        .h_key_byte_o(ah_key_byte), .h_in_byte_o(ah_in_byte),
        .h_in_ready(hu_in_ready), .h_done_i(hu_done), .h_dgra_o(ah_dgra), .h_dgq_i(hu_dgq));
end else begin : g_nosym
    assign a_mem_a = 13'd0; assign a_we = 1'b0; assign a_wa = 13'd0; assign a_wd = 8'd0;
    assign a_done = 1'b0; assign a_busy = 1'b0; assign a_status = 8'd0; assign a_len = 16'd0;
    assign ah_begin = 1'b0; assign ah_keydone = 1'b0; assign ah_key_wr = 1'b0; assign ah_in_valid = 1'b0; assign ah_msg_end = 1'b0;
    assign ah_mode = 2'd0; assign ah_iters = 32'd0; assign ah_idx = 32'd0; assign ah_key_byte = 8'd0; assign ah_in_byte = 8'd0; assign ah_dgra = 3'd0;
end endgenerate

// ---- user presence: button S2 + LED control (any profile) ----
`ifdef NO_PF
assign pf_we=1'b0; assign pf_wa=13'd0; assign pf_wd=8'd0; assign pf_done=1'b0; assign pf_status=8'd0; assign pf_len=16'd0; assign pf_busy=1'b0; assign pf_mem_a=13'd0;
assign fp_tx=1'b1; assign oled_sck=1'b0; assign oled_mosi=1'b0; assign oled_cs_n=1'b1; assign oled_dc=1'b0; assign oled_res_n=1'b1;
`else
pf_unit #(.FSYS_MHZ(FSYS_MHZ)) u_pf (.clk(clk), .rst_n(rst_n), .go(go_pf), .cmd(f_cmd), .len(f_len), .mem_a(pf_mem_a), .mem_q(mem_q),
              .tx_we(pf_we), .tx_wa(pf_wa), .tx_wd(pf_wd), .done(pf_done), .status(pf_status), .tx_len(pf_len), .busy(pf_busy),
              .fp_tx(fp_tx), .fp_rx(fp_rx), .oled_sck(oled_sck), .oled_mosi(oled_mosi), .oled_cs_n(oled_cs_n), .oled_dc(oled_dc), .oled_res_n(oled_res_n));
`endif
ux_unit #(.DEB_BITS(19), .LATCH_MAX(3000000 * FSYS_MHZ)) u_ux (.clk(clk), .rst_n(rst_n), .btn(btn_s2), .go(go_ux), .cmd(f_cmd), .len(f_len), .mem_a(), .mem_q(mem_q),
              .tx_we(ux_we), .tx_wa(ux_wa), .tx_wd(ux_wd), .done(ux_done), .status(ux_status), .tx_len(ux_len), .busy(ux_busy), .led_ctl(led_ctl), .diag(diag_in));

// frames of an absent class are rejected with BAD_CMD by the top level itself
reg none_done;
always @(posedge clk or negedge rst_n) begin if (!rst_n) none_done <= 1'b0; else none_done <= none_pend; end

assign cmd_busy = a_busy | e_busy | q_busy | ux_busy | pf_busy | none_pend | none_done;
assign mem_a = act_q ? q_rx_ra : act_e ? e_mem_a : act_ux ? 13'd0 : act_pf ? pf_mem_a : a_mem_a;
wire tx_we = a_we | e_we | q_tx_we | ux_we | pf_we;
wire [12:0] tx_wa = q_tx_we ? q_tx_wa : e_we ? e_wa : ux_we ? ux_wa : pf_we ? pf_wa : a_wa;
wire [7:0] tx_wd = q_tx_we ? q_tx_wd : e_we ? e_wd : ux_we ? ux_wd : pf_we ? pf_wd : a_wd;
wire c_done = a_done | e_done | q_done | ux_done | pf_done | none_done;
wire [7:0] c_status = q_done ? q_status : e_done ? e_status : ux_done ? ux_status : pf_done ? pf_status : a_done ? a_status : 8'd1;
wire [15:0] c_len = q_done ? q_len : e_done ? e_len : ux_done ? ux_len : pf_done ? pf_len : a_done ? a_len : 16'd0;
// the PQ engine decrypts its key in place in the payload RAM and re-reads the response RAM while it works
wire rx_we_m = act_q ? q_rx_we : rx_we;
wire [12:0] rx_wa_m = act_q ? q_rx_wa : rx_wa;
wire [7:0] rx_wd_m = act_q ? q_rx_wd : rx_wd;
wire [12:0] tx_ra_m = act_q ? q_tx_ra : tx_ra;

ram8k u_rxram (.clk(clk), .we(rx_we_m), .wa(rx_wa_m), .wd(rx_wd_m), .ra(mem_a), .rq(mem_q));
ram8k u_txram (.clk(clk), .we(tx_we), .wa(tx_wa), .wd(tx_wd), .ra(tx_ra_m), .rq(tx_rq));

reg req; reg [7:0] req_status; reg [15:0] req_len;
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) req <= 1'b0;
    else begin
        req <= 1'b0;
        if (c_done)         begin req <= 1'b1; req_status <= c_status; req_len <= c_len; end
        else if (frame_bad && !cmd_busy && !tx_busy)
                            begin req <= 1'b1; req_status <= 8'd3; req_len <= 16'd0; end
    end
end
wire f_start, f_busy, f_done; wire [7:0] f_data;
wire ut_busy, ut_done, st_busy, st_done;
frame_tx u_ftx (.clk(clk), .rst_n(rst_n), .req(req), .status(req_status), .len(req_len), .ra(tx_ra), .rd(tx_rq),
                .bstart(f_start), .bdata(f_data), .bbusy(via_spi ? st_busy : ut_busy), .bdone(via_spi ? st_done : ut_done),
                .busy(tx_busy));
uart_tx #(.CPB(CPB)) u_utx (.clk(clk), .rst_n(rst_n), .start(f_start && !via_spi), .data(f_data), .busy(ut_busy), .done(ut_done), .tx(uart_tx_pin));
spi_tx_if u_stx (.clk(clk), .rst_n(rst_n), .start(f_start && via_spi), .data(f_data), .busy(st_busy), .done(st_done),
                 .spi_data(s_data), .spi_have(s_have), .spi_take(s_take));

// LEDs (active low): led0 = busy, led1 = heartbeat, led2 = last status was an error
reg [24:0] hb = 0; reg err = 0;
always @(posedge clk) begin
    hb <= hb + 1;
    if (c_done) err <= (c_status != 8'd0);
end
// the host can force LEDs on (blinking with led_ctl[7]) to ask for a touch
wire ux_on = led_ctl[7] ? hb[22] : 1'b1;
assign led0 = ~((cmd_busy | tx_busy) | (led_ctl[0] & ux_on));
assign led1 = ~(hb[24] | (led_ctl[1] & ux_on));
assign led2 = ~(err | (led_ctl[2] & ux_on));
endmodule

// ---- top level: optional PLL in front of the core ----
// PLL_MUL = 1: the 27 MHz board clock drives the core directly (simulation, safe default).
// PLL_MUL / PLL_DIV: Gowin rPLL, f = 27 MHz * MUL / DIV (e.g. 3/1 = 81, 4/1 = 108, 8/3 = 72 MHz); the VCO is kept in 500..1250 MHz.
// CPB = f / 1.5 MHz must be an integer for the 1.5 Mbaud link (the build script computes it).
module accel_top #(parameter CPB = 18, parameter HAS_AES = 1, parameter HAS_SYM = 0, parameter HAS_ECC = 0, parameter HAS_PQ = 0, parameter PLL_MUL = 1, parameter PLL_DIV = 1)(
    input  wire clk,
    input  wire btn_s1,            // active high (reset)
    input  wire btn_s2,            // active high (user presence)
    output wire led0, led1, led2,  // active low
    output wire uart_tx_pin,
    input  wire uart_rx_pin,
    input  wire spi_sck, input wire spi_mosi, input wire spi_cs_n, output wire spi_miso,
    // peripherals bridged for the MCU: fingerprint sensor UART, SSD1306 OLED SPI
    output wire fp_tx, input wire fp_rx,
    output wire oled_sck, output wire oled_mosi, output wire oled_cs_n, output wire oled_dc, output wire oled_res_n
);
wire led1_core;
wire sclk, lock;
`ifdef BURN
wire burn_o; lut_burner #(.N(`BURN)) u_burn (.clk(sclk), .o(burn_o));
wire led_burn = burn_o;
`else
wire led_burn = 1'b0;
`endif
assign led1 = led1_core ^ led_burn;     // keeps the burner (BURN builds only)
wire [5:0] diag;
`ifdef PLL_PROBE
// Diagnostic build: the core stays on the 27 MHz board clock; a PLL runs beside it and its frequency is measured against
// the 27 MHz clock. {LOCK, ratio * 4 (PLL cycles per 27 MHz cycle)} is shown in bits 7..2 of the button status byte (UX_STATE / BENCH report it).
wire pclk, plock;
`ifdef PLL_DEV_ALT
localparam PDEV = "GW2A-18C";
`else
localparam PDEV = "GW2AR-18C";
`endif
`ifdef PLL_PRESET
reg [7:0] prc = 0; always @(posedge clk) if (!(&prc)) prc <= prc + 1'b1;
wire prst = !(&prc);
`else
wire prst = 1'b0;
`endif
localparam PF = 27 * PLL_MUL / PLL_DIV;
`ifdef PLL_IN_BUFG
wire clk_pi; BUFG u_pll_in (.I(clk), .O(clk_pi));
`endif
wire pclk_raw;
`ifdef PLL_FB_EXT
wire pfb = pclk_raw;
`else
wire pfb = 1'b0;
`endif
rPLL #(.FCLKIN("27"), .DEVICE(PDEV), .DYN_IDIV_SEL("false"), .IDIV_SEL(PLL_DIV - 1), .DYN_FBDIV_SEL("false"), .FBDIV_SEL(PLL_MUL - 1),
       .DYN_ODIV_SEL("false"), .ODIV_SEL(PF * 2 >= 500 ? 2 : PF * 4 >= 500 ? 4 : PF * 8 >= 500 ? 8 : 16), .PSDA_SEL("0000"), .DYN_DA_EN("false"), .DUTYDA_SEL("1000"),
       .CLKOUT_FT_DIR(1'b1), .CLKOUTP_FT_DIR(1'b1), .CLKOUT_DLY_STEP(0), .CLKOUTP_DLY_STEP(0), `ifdef PLL_FB_EXT
       .CLKFB_SEL("external"),
`else
       .CLKFB_SEL("internal"),
`endif
       .CLKOUT_BYPASS("false"), .CLKOUTP_BYPASS("false"), .CLKOUTD_BYPASS("false"), .DYN_SDIV_SEL(2),
       .CLKOUTD_SRC("CLKOUT"), .CLKOUTD3_SRC("CLKOUT")) u_probe_pll (
`ifdef PLL_IN_BUFG
    .CLKIN(clk_pi), .CLKFB(1'b0), .RESET(prst), .RESET_P(1'b0), .FBDSEL(6'd0), .IDSEL(6'd0), .ODSEL(6'd0),
`else
    .CLKIN(clk), .CLKFB(pfb), .RESET(prst), .RESET_P(1'b0), .FBDSEL(6'd0), .IDSEL(6'd0), .ODSEL(6'd0),
`endif
    .PSDA(4'd0), .DUTYDA(4'd0), .FDLY(4'd0), .CLKOUT(pclk_raw), .LOCK(plock), .CLKOUTP(), .CLKOUTD(), .CLKOUTD3());
`ifdef PLL_OUT_BUFG
BUFG u_pll_out (.I(pclk_raw), .O(pclk));
`elsif PLL_METER_SELFTEST
assign pclk = clk;                      // meter self test: measures the board clock itself (expect ratio 1.00)
`else
assign pclk = pclk_raw;
`endif
reg [15:0] gcnt = 0; always @(posedge clk) gcnt <= gcnt + 1'b1;               // gate: 2^15 cycles high, 2^15 low (27 MHz domain)
reg g1 = 0, g2 = 0, g3 = 0; always @(posedge pclk) begin g1 <= gcnt[15]; g2 <= g1; g3 <= g2; end
reg [17:0] pc = 0; reg [17:0] pres = 0;
always @(posedge pclk) begin
    if (g2 && !g3) pc <= 18'd1;                                                  // gate opens
    else if (g2) pc <= pc + 1'b1;
    if (!g2 && g3) pres <= pc;                                                   // gate closes: pc = ratio * 32768
end
reg [5:0] d1 = 0, d2 = 0; always @(posedge clk) begin d1 <= {plock, pres[17:13]}; d2 <= d1; end   // bit 5 = LOCK, bits 4..0 = ratio * 4 measured against 27 MHz
assign diag = d2;
assign sclk = clk;
assign lock = 1'b1;
`else
assign diag = 6'd0;
generate if (PLL_MUL == PLL_DIV) begin : g_nopll
    assign sclk = clk;
    assign lock = 1'b1;
end else begin : g_pll
    localparam FOUT = 27 * PLL_MUL / PLL_DIV;
    localparam ODIV = (FOUT * 2 >= 500) ? 2 : (FOUT * 4 >= 500) ? 4 : (FOUT * 8 >= 500) ? 8 : (FOUT * 16 >= 500) ? 16 : 32;   // VCO = f * ODIV within 500..1250 MHz
    rPLL #(.FCLKIN("27"), .DEVICE("GW2AR-18C"), .DYN_IDIV_SEL("false"), .IDIV_SEL(PLL_DIV - 1), .DYN_FBDIV_SEL("false"), .FBDIV_SEL(PLL_MUL - 1),
           .DYN_ODIV_SEL("false"), .ODIV_SEL(ODIV), .PSDA_SEL("0000"), .DYN_DA_EN("false"), .DUTYDA_SEL("1000"),
           .CLKOUT_FT_DIR(1'b1), .CLKOUTP_FT_DIR(1'b1), .CLKOUT_DLY_STEP(0), .CLKOUTP_DLY_STEP(0), .CLKFB_SEL("internal"),
           .CLKOUT_BYPASS("false"), .CLKOUTP_BYPASS("false"), .CLKOUTD_BYPASS("false"), .DYN_SDIV_SEL(2),
           .CLKOUTD_SRC("CLKOUT"), .CLKOUTD3_SRC("CLKOUT")) u_pll (
        .CLKIN(clk), .CLKFB(1'b0), .RESET(1'b0), .RESET_P(1'b0), .FBDSEL(6'd0), .IDSEL(6'd0), .ODSEL(6'd0),
        `ifdef PLL_FDLY15
        .PSDA(4'd0), .DUTYDA(4'd0), .FDLY(4'b1111), .CLKOUT(sclk)
`else
        .PSDA(4'd0), .DUTYDA(4'd0), .FDLY(4'd0), .CLKOUT(sclk)
`endif, .LOCK(lock), .CLKOUTP(), .CLKOUTD(), .CLKOUTD3());
end endgenerate
`endif

`ifdef PLL_NOLOCK
wire lock_use = 1'b1;                    // diagnostic build: do not wait for the PLL LOCK output
`else
wire lock_use = lock;
`endif
`ifdef PLL_PROBE
localparam CORE_CPB = 18, CORE_MHZ = 27;
`else
localparam CORE_CPB = CPB, CORE_MHZ = 27 * PLL_MUL / PLL_DIV;
`endif
accel_core #(.CPB(CORE_CPB), .HAS_AES(HAS_AES), .HAS_SYM(HAS_SYM), .HAS_ECC(HAS_ECC), .HAS_PQ(HAS_PQ), .FSYS_MHZ(CORE_MHZ)) u_core (
    .clk(sclk), .pll_lock(lock_use), .diag_in(diag), .btn_s1(btn_s1), .btn_s2(btn_s2), .led0(led0), .led1(led1_core), .led2(led2),
    .uart_tx_pin(uart_tx_pin), .uart_rx_pin(uart_rx_pin),
    .spi_sck(spi_sck), .spi_mosi(spi_mosi), .spi_cs_n(spi_cs_n), .spi_miso(spi_miso),
    .fp_tx(fp_tx), .fp_rx(fp_rx), .oled_sck(oled_sck), .oled_mosi(oled_mosi), .oled_cs_n(oled_cs_n), .oled_dc(oled_dc), .oled_res_n(oled_res_n));
endmodule
`default_nettype wire
