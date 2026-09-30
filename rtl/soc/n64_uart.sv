// Mirlo-N64's JTAG UART: LiteX's JTAG PHY (jtag_uart_bridge.v, the host side
// unchanged: litex/jtag_uart_relay.py) with a 512-byte TX FIFO and a 16-byte
// RX FIFO (RX not built: nothing reads it). Bytes go out from the registers (UART_RXTX: the geom and audio
// firmware), the boot ROM (the system window), and a dumper in hardware:
// every DUMP_CYCLES it sends the debug words (what the OSD shows) as one
// line, "D" and WORDS x " xxxxxxxx", so there is a log over time even with every
// core hung. A byte written to a full FIFO is dropped (nobody reading must
// not stall anyone); the dumper waits for room for a whole line.
`default_nettype none

module n64_uart #(
    parameter int DUMP_CYCLES = 62_832_000,     // ~1 s
    parameter int WORDS = 12                    // of dbg, from [0]
) (
    input  wire         clk,
    input  wire         rst,
    input  wire         tck, tdi, tms,
    output wire         tdo,
    // the writers (a strobe each; the registers' wins a same-cycle clash, then the window's)
    input  wire         r_we,
    input  wire  [7:0]  r_byte,
    input  wire         w_we,
    input  wire  [7:0]  w_byte,
    output logic        txfull,
    // reading
    input  wire         rx_pop,
    output logic [7:0]  rx_byte,
    output logic        rxempty,
    // the dumper
    input  wire         dump_en,
    input  wire  [15:0][31:0] dbg
);

// ---- TX FIFO
logic       tx_in_ready, tx_out_valid, tx_out_ready;
logic [7:0] tx_out;
logic [9:0] tx_level;
logic       d_valid;
logic [7:0] d_byte;                    // (the dumper's, registered)
wire        tx_push = r_we || w_we || d_valid;
wire  [7:0] tx_data = r_we ? r_byte : w_we ? w_byte : d_byte;
wire        d_taken = d_valid && !r_we && !w_we && tx_in_ready;
fifo_fwft #(.W(8), .AW(9)) txq (.clk, .rst, .in_valid(tx_push), .in_ready(tx_in_ready), .in_data(tx_data),
    .out_valid(tx_out_valid), .out_ready(tx_out_ready), .out_data(tx_out), .level(tx_level));
assign txfull = !tx_in_ready;

// ---- RX: nothing reads the host's bytes in this build (area): taken, dropped
logic       rx_in_valid;
logic [7:0] rx_in;
assign rxempty = 1'b1;
assign rx_byte = 8'd0;

// ---- the PHY (sink: to the host; source: from it)
jtag_uart_bridge phy (.clk, .rst,
    .altera_reserved_tms(tms), .altera_reserved_tck(tck), .altera_reserved_tdi(tdi), .altera_reserved_tdo(tdo),
    .source_valid(rx_in_valid), .source_data(rx_in), .source_ready(1'b1),
    .sink_valid(tx_out_valid), .sink_data(tx_out), .sink_ready(tx_out_ready));

// ---- the dumper: "D" + WORDS x (" " + 8 hex digits) + "\n".
// A word at a time into a register, its digits shifted out of it: every
// byte comes from registers (the first cut computed them from the line
// position through a divide by 9 and a 128:1 nibble mux: -4 ns).
logic [31:0] timer, wreg;
logic [3:0]  word, digit;               // digit 0: the space before the word
logic [1:0]  ph;                        // 0 idle, 1 "D", 2 the words, 3 "\n"
always_ff @(posedge clk) begin
    if (rst) begin timer <= 0; ph <= 0; d_valid <= 0; end
    else begin
        case (ph)
        2'd0: begin
            d_valid <= 0;
            timer <= timer + 32'd1;
            if (timer >= 32'(DUMP_CYCLES) && dump_en && tx_level < 10'd300) begin
                timer <= 0; ph <= 2'd1; d_valid <= 1; d_byte <= "D"; word <= 0; digit <= 0;
                wreg <= dbg[0];
            end
        end
        2'd1: if (d_taken) begin                        // "D" gone: the first word's space
            ph <= 2'd2; d_byte <= " "; digit <= 0;
        end
        2'd2: if (d_taken) begin
            if (digit == 4'd8) begin                    // the word done
                if (word == 4'(WORDS - 1)) begin ph <= 2'd3; d_byte <= 8'h0A; end
                else begin word <= word + 4'd1; digit <= 0; d_byte <= " "; wreg <= dbg[word + 4'd1]; end
            end else begin
                d_byte <= wreg[31:28] < 4'd10 ? 8'h30 + {4'd0, wreg[31:28]} : 8'h57 + {4'd0, wreg[31:28]};
                wreg <= {wreg[27:0], 4'd0};
                digit <= digit + 4'd1;
            end
        end
        2'd3: if (d_taken) begin ph <= 2'd0; d_valid <= 0; end
        endcase
    end
end

endmodule
`default_nettype wire
