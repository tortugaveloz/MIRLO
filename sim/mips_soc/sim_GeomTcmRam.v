// rtl/geom/GeomTcmRam.v (an altsyncram) for Verilator: port A's address is
// registered while a_en (addressstall_a = ~a_en), its data read from the
// registered address (unregistered output: a write there shows at once).
// Port B: the same, never stalled.
module GeomTcmRam (
    input  wire        clk,
    input  wire        a_en,
    input  wire [11:0] a_adr,
    input  wire [3:0]  a_we,
    input  wire [31:0] a_dat_w,
    output wire [31:0] a_dat_r,
    input  wire [11:0] b_adr,
    input  wire [3:0]  b_we,
    input  wire [31:0] b_dat_w,
    output wire [31:0] b_dat_r
);
    reg [31:0] mem [0:4095];
    reg [11:0] a_q, b_q;
    integer i;
    always @(posedge clk) begin
        if (a_en) begin
            a_q <= a_adr;
            for (i = 0; i < 4; i = i + 1) if (a_we[i]) mem[a_adr][8*i +: 8] <= a_dat_w[8*i +: 8];
        end
        b_q <= b_adr;
        for (i = 0; i < 4; i = i + 1) if (b_we[i]) mem[b_adr][8*i +: 8] <= b_dat_w[8*i +: 8];
    end
    // +tcmw=<byte addr>,... (up to 4, e.g. from geom.map): printed every 2^26 cycles
    reg [31:0] w0, w1, w2, w3;
    reg [25:0] tick;
    initial begin
        w0 = 0; w1 = 0; w2 = 0; w3 = 0; tick = 0;
        if (!$value$plusargs("tcmw0=%h", w0)) w0 = 0;
        if (!$value$plusargs("tcmw1=%h", w1)) w1 = 0;
        if (!$value$plusargs("tcmw2=%h", w2)) w2 = 0;
        if (!$value$plusargs("tcmw3=%h", w3)) w3 = 0;
    end
    always @(posedge clk) begin
        tick <= tick + 26'd1;
        if (tick == 26'd0 && w0 != 0)
            $display("TCM %08x=%08x %08x=%08x %08x=%08x %08x=%08x", w0, mem[w0[13:2]], w1, mem[w1[13:2]],
                     w2, mem[w2[13:2]], w3, mem[w3[13:2]]);
    end
    assign a_dat_r = mem[a_q];
    assign b_dat_r = mem[b_q];
endmodule
