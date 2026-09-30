// A single-clock FIFO in a RAM (M10K), show-ahead: out_data is the head
// whenever out_valid. The RAM is read every cycle at the next head's
// address; a word written into an empty FIFO shows the cycle after (the
// RAM's registered read gives old data for an address written at the same
// edge).
`default_nettype none
module fifo_fwft #(parameter int W = 32, parameter int AW = 11) (
    input  wire          clk, rst,
    input  wire          in_valid,
    output logic         in_ready,
    input  wire  [W-1:0] in_data,
    output logic         out_valid,
    input  wire          out_ready,
    output logic [W-1:0] out_data,
    output logic [AW:0]  level
);
logic [W-1:0] mem [1 << AW];
logic [AW:0]  wp, rp;
logic         stale;                        // out_data was read at the address written at the same edge
wire          push = in_valid && in_ready;
wire          pop  = out_valid && out_ready;
wire  [AW:0]  rp_n = rp + pop;
assign level     = wp - rp;
assign in_ready  = level != (AW+1)'(1 << AW);
assign out_valid = level != 0 && !stale;
always_ff @(posedge clk) begin
    if (rst) begin wp <= 0; rp <= 0; stale <= 0; end
    else begin
        wp <= wp + push;
        rp <= rp_n;
        stale <= push && wp[AW-1:0] == rp_n[AW-1:0];
    end
end
always_ff @(posedge clk) begin
    if (push) mem[wp[AW-1:0]] <= in_data;
    out_data <= mem[rp_n[AW-1:0]];
end
endmodule
`default_nettype wire
