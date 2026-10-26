`default_nettype none
// Peripheral bridge for the MCU (the BL616 has no free GPIOs): UART to the fingerprint sensor and SPI to the SSD1306 OLED.
//   0x06 FP_XFER  [gap_ms][timeout_ms LE16][max_rx LE16][tx bytes...] -> received bytes
//        sends the tx bytes (57600 8N1), then collects bytes. One countdown timer: loaded with timeout_ms after the transmission
//        and reloaded with gap_ms after every received byte; the command ends when it reaches 0 or max_rx (<= 255) bytes arrived.
//        Bytes go straight into the response RAM while the command runs (no FIFO). Timeout is limited to 4095 ms.
//   0x07 OLED     [ctl][data...] -> -    ctl bit0 = DC level, bit1 = RES level, bit2 = apply RES (else RES unchanged).
//        data bytes are shifted out MSB first, SPI mode 0, CS low from the first to the last byte. Payload <= 1023 bytes.
// Written for area: request RAM address = byte index, down-counters, no async reset on data registers.
module pf_unit #(parameter FSYS_MHZ = 27, parameter FP_BAUD = 57600, parameter SPI_HALF = (FSYS_MHZ + 12) / 13)(
    input  wire clk, input wire rst_n,
    input  wire go, input wire [7:0] cmd, input wire [15:0] len,
    output reg  [12:0] mem_a = 0, input wire [7:0] mem_q,
    output reg  tx_we, output wire [12:0] tx_wa, output reg [7:0] tx_wd = 0,
    output reg  done, output reg [7:0] status = 0, output reg [15:0] tx_len = 0, output wire busy,
    output wire fp_tx, input wire fp_rx,
    output reg  oled_sck, output reg oled_mosi = 0, output reg oled_cs_n, output reg oled_dc = 0, output reg oled_res_n
);
localparam FP_CPB = (FSYS_MHZ * 1000000) / FP_BAUD;
localparam MS_CLKS = FSYS_MHZ * 1000;

reg u_start; reg [7:0] u_data = 0; wire u_busy, u_done;
uart_tx #(.CPB(FP_CPB)) u_utx (.clk(clk), .rst_n(rst_n), .start(u_start), .data(u_data), .busy(u_busy), .done(u_done), .tx(fp_tx));
wire r_valid; wire [7:0] r_data;
uart_rx #(.CPB(FP_CPB)) u_urx (.clk(clk), .rst_n(rst_n), .rx(fp_rx), .valid(r_valid), .data(r_data));

localparam S_IDLE = 0, S_FA = 1, S_FB = 2, S_FC = 3, S_HAVE = 4, S_UTX = 5, S_RX = 6, S_SP1 = 7, S_SP2 = 8, S_SPE = 9, S_FIN = 10, S_TXW = 11;
reg [3:0] st; reg is_fp = 0, is_oled = 0;
reg [7:0] gap_ms = 0, max_rx = 0, rxn = 0; reg [9:0] rem = 0; reg [11:0] tc = 0;
reg [7:0] sh = 0; reg [2:0] bitn = 0; reg [3:0] hc = 0; reg [16:0] msc = 0; reg ms_tick = 0;
assign busy = (st != S_IDLE);
reg [7:0] wa = 0; assign tx_wa = {5'd0, wa};
wire hdr = (mem_a[12:3] == 10'd0) && (mem_a[2:0] < 3'd5);     // request bytes 0..4 are the FP header

always @(posedge clk) begin
    ms_tick <= 0;
    if (st != S_RX) msc <= 0;
    else if (msc == MS_CLKS - 1) begin msc <= 0; ms_tick <= 1; end
    else msc <= msc + 1'b1;
end

always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        st <= S_IDLE; done <= 0; tx_we <= 0; u_start <= 0;
        oled_sck <= 0; oled_cs_n <= 1; oled_res_n <= 1;
    end else begin
        done <= 0; tx_we <= 0; u_start <= 0;
        case (st)
        S_IDLE: if (go) begin
            is_fp <= (cmd == 8'h06); is_oled <= (cmd == 8'h07); mem_a <= 0; rem <= len[9:0]; rxn <= 0; st <= S_FA;
        end
        S_FA: begin                                             // request byte at mem_a is on its way from the RAM
            if (rem == 0) begin                                 // payload exhausted
                if (is_fp && !hdr) begin st <= S_RX; end           // header complete (>= 5 bytes seen)
                else if (is_oled && mem_a != 0) st <= S_SPE;
                else begin status <= (is_fp || is_oled) ? 8'd2 : 8'd1; tx_len <= 0; st <= S_FIN; end
            end else st <= S_FB;
        end
        S_FB: st <= S_FC;
        S_FC: begin sh <= mem_q; u_data <= mem_q; st <= S_HAVE; end
        S_HAVE: begin
            mem_a <= mem_a + 1'b1; rem <= rem - 1'b1; st <= S_FA;
            if (is_fp) begin
                if (hdr) begin
                    case (mem_a[2:0])
                    3'd0: gap_ms <= sh;
                    3'd1: tc[7:0] <= sh;
                    3'd2: tc[11:8] <= sh[3:0];
                    3'd3: max_rx <= sh;
                    default: ;
                    endcase
                end else begin u_start <= 1; st <= S_UTX; end
            end else if (is_oled) begin
                if (mem_a == 0) begin oled_dc <= sh[0]; if (sh[2]) oled_res_n <= sh[1]; end
                else begin bitn <= 0; hc <= 0; oled_cs_n <= 0; oled_mosi <= sh[7]; st <= S_SP1; end
            end
        end
        S_UTX: if (u_done) st <= S_FA;
        S_RX: begin
            if (r_valid && rxn != max_rx) begin
                tx_wd <= r_data; tx_we <= 1; wa <= rxn; rxn <= rxn + 1'b1; tc <= {4'd0, gap_ms};
            end else if (ms_tick && tc != 0) tc <= tc - 1'b1;
            if (rxn == max_rx || tc == 0) begin status <= 8'd0; tx_len <= {8'd0, rxn}; st <= S_TXW; end
        end
        S_SP1: begin                                            // clock low half
            if (hc == SPI_HALF - 1) begin hc <= 0; oled_sck <= 1; st <= S_SP2; end else hc <= hc + 1'b1;
        end
        S_SP2: begin                                            // clock high half
            if (hc == SPI_HALF - 1) begin
                hc <= 0; oled_sck <= 0; sh <= {sh[6:0], 1'b0}; bitn <= bitn + 1'b1;
                if (bitn == 3'd7) st <= S_FA;
                else begin oled_mosi <= sh[6]; st <= S_SP1; end
            end else hc <= hc + 1'b1;
        end
        S_SPE: begin                                            // hold CS for one half period after the last byte
            if (oled_cs_n || hc == SPI_HALF - 1) begin oled_cs_n <= 1; status <= 8'd0; tx_len <= 0; st <= S_FIN; end
            else hc <= hc + 1'b1;
        end
        S_TXW: st <= S_FIN;                                     // let the last response byte land in the RAM
        S_FIN: begin done <= 1; st <= S_IDLE; end
        default: st <= S_IDLE;
        endcase
    end
end
endmodule
`default_nettype wire
