`default_nettype none
// User-presence unit: button S2 (debounced, plus a press latch that expires) and LED control.
//   0x04 [mode]  -> 1 byte {diag[5:0], latched, pressed}; mode bit0 = clear the latch after reading
//   0x05 [leds]  -> -    bits 0..2 force led0..led2 on (blink at ~3 Hz when bit 7 is set), 0 = LEDs back to status display
// The latch lets a short press between two polls of the host be seen, and expires after LATCH_MAX clocks (3 s at 27 MHz)
// so that an old press cannot authorize a later operation.
module ux_unit #(parameter DEB_BITS = 19, parameter LATCH_MAX = 81_000_000) (
    input  wire clk, input wire rst_n,
    input  wire btn,                       // raw, active high, asynchronous
    input  wire go, input wire [7:0] cmd, input wire [15:0] len,
    output wire [12:0] mem_a, input wire [7:0] mem_q,
    output reg  tx_we, output wire [12:0] tx_wa, output reg [7:0] tx_wd,
    output reg  done, output reg [7:0] status, output reg [15:0] tx_len, output wire busy,
    output reg  [7:0] led_ctl,
    input  wire [5:0] diag                  // reported in bits 7..2 of the 0x04 response (PLL probe etc.)
);
// ---- debounce ----
// The polarity is learned: the level seen right after reset (button not pressed) is "idle", the opposite level is a press.
// (S2 of the Tang Nano 20K is wired active-high; learning keeps the unit working on boards with the opposite wiring.)
reg s1, s2; reg [DEB_BITS-1:0] dc; reg pressed; reg idle; reg armed; reg [DEB_BITS:0] ac;
wire act = s2 ^ idle;                                       // 1 = button pressed (after debouncing: 'pressed')
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin s1 <= 0; s2 <= 0; dc <= 0; pressed <= 0; idle <= 0; armed <= 0; ac <= 0; end
    else begin
        s1 <= btn; s2 <= s1;
        if (!armed) begin                                   // wait for the input to settle, then sample the idle level
            ac <= ac + 1'b1;
            if (ac[DEB_BITS]) begin idle <= s2; armed <= 1; end
        end else begin
            if (act == pressed) dc <= 0;
            else begin dc <= dc + 1'b1; if (&dc) pressed <= act; end
        end
    end
end
// ---- latch ----
reg latched; reg [28:0] lt; reg clr; reg pressed_d;       // lt: 3 s at up to 170 MHz
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin latched <= 0; lt <= 0; pressed_d <= 0; end
    else begin
        pressed_d <= pressed;
        if (pressed && !pressed_d) begin latched <= 1; lt <= 0; end
        else if (clr) latched <= 0;
        else if (latched) begin lt <= lt + 1'b1; if (lt == LATCH_MAX) latched <= 0; end
    end
end
// ---- command ----
localparam S_IDLE = 0, S_W0 = 1, S_W1 = 2, S_EXE = 3, S_TX = 4, S_FIN = 5;
reg [2:0] st; reg [7:0] c, p; reg [15:0] l;
assign mem_a = 13'd0; assign tx_wa = 13'd0; assign busy = (st != S_IDLE);
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin st <= S_IDLE; done <= 0; tx_we <= 0; clr <= 0; led_ctl <= 0; status <= 0; tx_len <= 0; tx_wd <= 0; c <= 0; p <= 0; l <= 0; end
    else begin
        done <= 0; tx_we <= 0; clr <= 0;
        case (st)
        S_IDLE: if (go) begin c <= cmd; l <= len; st <= S_W0; end
        S_W0: st <= S_W1;                                   // payload byte 0 is being read from the request RAM
        S_W1: begin p <= mem_q; st <= S_EXE; end
        S_EXE: begin
            if (l != 16'd1) begin status <= 8'd2; tx_len <= 0; st <= S_FIN; end
            else if (c == 8'h04) begin
                tx_wd <= {diag, latched, pressed}; tx_we <= 1; clr <= p[0];
                status <= 8'd0; tx_len <= 16'd1; st <= S_TX;
            end else if (c == 8'h05) begin
                led_ctl <= p; status <= 8'd0; tx_len <= 0; st <= S_FIN;
            end else begin status <= 8'd1; tx_len <= 0; st <= S_FIN; end
        end
        S_TX: st <= S_FIN;                                  // let the response byte land in the RAM
        S_FIN: begin done <= 1; st <= S_IDLE; end
        default: st <= S_IDLE;
        endcase
    end
end
endmodule
`default_nettype wire
