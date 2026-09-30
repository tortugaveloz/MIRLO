// The geom and audio cores' way out (Wishbone classic, word addresses):
//   0x4000_0000-0x43FF_FFFF  SDRAM (a port of sdr_arb)
//   0xF000_0000-0xF000_FFFF  the registers (n64_regs), a cycle to answer
//   anything else            read 0, writes dropped (counted in `stray`)
// One transaction at a time; when both masters ask, the one that did not have
// the last one goes. A read burst (CTI 010: the geom core's line refills,
// line aligned, linear, 8 beats) is asked of SDRAM whole at its first beat and
// handed out a word a cycle as the words come back (the next beat's word is
// taken in the cycle that acknowledges the last). Writes are posted:
// acknowledged once the arbiter has taken them.
`default_nettype none

module n64_bus (
    input  wire         clk,
    input  wire         rst,
    // master 0: the geom core's dBus (its ROM / TCM are its own)
    input  wire         m0_cyc, m0_stb, m0_we,
    input  wire  [29:0] m0_adr,
    input  wire  [31:0] m0_dat_w,
    input  wire  [3:0]  m0_sel,
    input  wire  [2:0]  m0_cti,
    output logic        m0_ack,
    output logic [31:0] m0_dat_r,
    // master 1: the audio core
    input  wire         m1_cyc, m1_stb, m1_we,
    input  wire  [29:0] m1_adr,
    input  wire  [31:0] m1_dat_w,
    input  wire  [3:0]  m1_sel,
    output logic        m1_ack,
    output logic [31:0] m1_dat_r,
    // SDRAM
    output logic        s_valid,
    output logic        s_we,
    output logic [23:0] s_addr,
    output logic [31:0] s_wdata,
    output logic [3:0]  s_be,
    input  wire         s_ready,
    input  wire         s_rvalid,
    input  wire  [31:0] s_rdata,
    // the registers
    output logic        r_req,          // a cycle: the access
    output logic        r_we,
    output logic [15:0] r_addr,         // byte offset
    output logic [31:0] r_wdata,
    input  wire  [31:0] r_rdata,        // the cycle after r_req
    output logic [15:0] stray
);

logic        own;                   // 0: geom, 1: audio
wire         cyc  = own ? m1_cyc : m0_cyc;
wire         stb  = own ? m1_stb : m0_stb;
wire         we   = own ? m1_we  : m0_we;
wire  [29:0] adr  = own ? m1_adr : m0_adr;
wire  [31:0] dw   = own ? m1_dat_w : m0_dat_w;
wire  [3:0]  sel  = own ? m1_sel : m0_sel;
wire  [2:0]  cti  = own ? 3'd0 : m0_cti;
wire         is_sd  = adr[29:24] == 6'h10;          // 0x4000_0000 - 0x43FF_FFFF
wire         is_reg = adr[29:14] == 16'hF000;       // 0xF000_0000 - 0xF000_FFFF (word 0x3C00_0000..)

typedef enum logic [2:0] { B_IDLE, B_ROUTE, B_WR, B_RD, B_DRAIN, B_REG, B_REGD } bst_t;
bst_t st;
logic        ack;
logic [31:0] dat;
assign m0_ack = ack && !own;
assign m1_ack = ack && own;
assign m0_dat_r = dat;
assign m1_dat_r = dat;

// a read transaction: words asked of SDRAM, received, handed out; the base
// word and how many
logic [23:0] rbase;
logic [3:0]  want, asked, recv, handed;
logic [31:0] rq [8];
wire  [3:0]  queued = recv - handed;

assign s_we    = st == B_WR;
assign s_valid = st == B_WR || (st == B_RD && asked != want);
assign s_addr  = st == B_RD ? rbase + {20'd0, asked} : adr[23:0];
assign s_wdata = dw;
assign s_be    = sel;

wire got  = s_rvalid;                           // (a read transaction drains before the next starts)
wire more = cti == 3'b010 && handed != want;    // (the beat being acknowledged is not the last)
wire pop  = st == B_RD && queued != 0 && cyc && stb && (!ack || more);
wire last = pop && (cti != 3'b010 && !ack || handed + 4'd1 == want);

always_ff @(posedge clk) begin
    if (rst) begin
        st <= B_IDLE; own <= 0; ack <= 0; r_req <= 0; stray <= 0;
        want <= 0; asked <= 0; recv <= 0; handed <= 0; dat <= 0;
    end else begin
        ack <= 0; r_req <= 0;
        if (got) begin rq[recv[2:0]] <= s_rdata; recv <= recv + 4'd1; end
        case (st)
        B_IDLE: if (!ack) begin                                 // (never during an acknowledge)
            if (m0_cyc && m0_stb && m1_cyc && m1_stb) begin own <= !own; st <= B_ROUTE; end
            else if (m0_cyc && m0_stb) begin own <= 0; st <= B_ROUTE; end
            else if (m1_cyc && m1_stb) begin own <= 1; st <= B_ROUTE; end
        end
        B_ROUTE: begin
            if (!(cyc && stb)) st <= B_IDLE;
            else if (is_sd && we) st <= B_WR;
            else if (is_sd) begin
                st <= B_RD; asked <= 0; recv <= 0; handed <= 0;
                if (cti == 3'b010) begin rbase <= adr[23:0]; want <= 4'd8 - {1'b0, adr[2:0]}; end
                else begin rbase <= adr[23:0]; want <= 4'd1; end
            end else if (is_reg) begin st <= B_REG; r_req <= 1; end
            else begin
                if (stray != 16'hFFFF) stray <= stray + 16'd1;
                dat <= 0; ack <= 1; st <= B_IDLE;
            end
        end
        B_WR: if (s_ready) begin ack <= 1; st <= B_IDLE; end
        B_REG: st <= B_REGD;                                   // (the registers answer a cycle later)
        B_REGD: begin dat <= r_rdata; ack <= 1; st <= B_IDLE; end
        B_RD: begin
            if (s_valid && s_ready) asked <= asked + 4'd1;
            if (pop) begin dat <= rq[handed[2:0]]; ack <= 1; handed <= handed + 4'd1; end
            // the transaction is over: wait for what it asked (a burst ended
            // early, or given up) -- nothing is asked any more
            if (last || !cyc) st <= B_DRAIN;
        end
        B_DRAIN: if (asked == recv) st <= B_IDLE;
        default: st <= B_IDLE;
        endcase
    end
end

// the register access (registered with r_req)
always_ff @(posedge clk) begin
    r_we <= we && cyc && stb; r_addr <= {adr[13:0], 2'b00}; r_wdata <= dw;
end

endmodule
`default_nettype wire
