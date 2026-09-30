// The SDRAM ports of Mirlo-N64's masters (sdr_arb's request side: valid,
// ready, we, word address, data, byte enables; read data back in order).
`default_nettype none

// ---- n64_sys's system memory: RDRAM, the cartridge, the save (litex/n64.py's
// adapter). N64 physical -> SDRAM word:
//   RDRAM 0x0000_0000-0x007F_FFFF -> SDRAM 0
//   cart  0x1000_0000-0x1FBF_FFFF -> SDRAM CART_OFF (the Pocket loaded the ROM there)
//   save  SAVE_BASE..             -> SDRAM SAVE_OFF (the EEPROM: data slot 1)
// A read is taken at once (s_ack) and its words (8 for a line) come back as
// s_rvalid; a write is acknowledged once the arbiter has it.
module port_n64sys #(
    parameter bit          FLAT      = 0,               // native Mirlo: SDRAM's word = address[25:2]
    parameter logic [31:0] CART_OFF  = 32'h0100_0000,
    parameter logic [31:0] SAVE_OFF  = 32'h03FF_0000,
    parameter logic [31:0] SAVE_BASE = 32'h1FE0_0000
) (
    input  wire         clk, rst,
    input  wire         s_req, s_we, s_line,
    input  wire  [31:0] s_addr, s_wdata,
    input  wire  [3:0]  s_strb,
    output logic        s_ack, s_rvalid,
    output logic [31:0] s_rdata,
    output logic        p_valid, p_we,
    output logic [23:0] p_addr,
    output logic [31:0] p_wdata,
    output logic [3:0]  p_be,
    input  wire         p_ready, p_rvalid,
    input  wire  [31:0] p_rdata
);
logic [31:0] a;
always_comb
    if (FLAT) a = s_addr;
    else if (s_addr < 32'h0080_0000) a = s_addr;
    else if (s_addr >= 32'h1000_0000 && s_addr < 32'h1FC0_0000) a = s_addr - 32'h1000_0000 + CART_OFF;
    else if (s_addr >= SAVE_BASE) a = s_addr - SAVE_BASE + SAVE_OFF;
    else a = s_addr;
typedef enum logic [1:0] { P_IDLE, P_WRITE, P_WACK, P_READ } pst_t;
pst_t st;
logic [23:0] base;
logic [3:0]  n, issued, got;
assign p_we    = st == P_WRITE;
assign p_valid = st == P_WRITE || (st == P_READ && issued != n);
assign p_addr  = base + {20'd0, st == P_READ ? issued : 4'd0};
assign s_ack   = (st == P_IDLE && s_req && !s_we) || st == P_WACK;
assign s_rvalid = st == P_READ && p_rvalid;
assign s_rdata  = p_rdata;
always_ff @(posedge clk) begin
    if (rst) st <= P_IDLE;
    else case (st)
    P_IDLE: if (s_req) begin
        base <= a[25:2]; p_wdata <= s_wdata; p_be <= s_strb;
        n <= s_line ? 4'd8 : 4'd1; issued <= 0; got <= 0;
        st <= s_we ? P_WRITE : P_READ;
    end
    P_WRITE: if (p_ready) st <= P_WACK;
    P_WACK:  st <= P_IDLE;
    P_READ: begin
        if (p_valid && p_ready) issued <= issued + 4'd1;
        if (p_rvalid) begin got <= got + 4'd1; if (got + 4'd1 == n) st <= P_IDLE; end
    end
    endcase
end
endmodule

// ---- MRDP's memory port (LiteDRAM-native-like: a command, and a write's data
// on its own channel after it). Commands queue here (4) so MRDP is not held
// to one in flight; a write goes to the arbiter with its data.
module port_mrdp (
    input  wire         clk, rst,
    input  wire         m_cmd_valid, m_cmd_we,
    input  wire  [31:0] m_cmd_addr,         // byte address on MRDP's bus (SDRAM at 0x4000_0000)
    output logic        m_cmd_ready,
    input  wire         m_wdata_valid,
    input  wire  [31:0] m_wdata,
    input  wire  [3:0]  m_wdata_we,
    output logic        m_wdata_ready,
    output logic        m_rdata_valid,
    output logic [31:0] m_rdata,
    output logic        p_valid, p_we,
    output logic [23:0] p_addr,
    output logic [31:0] p_wdata,
    output logic [3:0]  p_be,
    input  wire         p_ready, p_rvalid,
    input  wire  [31:0] p_rdata
);
logic [24:0] q [4];                         // {we, word address}
logic [2:0]  qw, qr;
wire  [2:0]  qn = qw - qr;
assign m_cmd_ready = qn != 3'd4;
wire  [24:0] head = q[qr[1:0]];
assign p_we    = head[24];
assign p_addr  = head[23:0];
assign p_wdata = m_wdata;
assign p_be    = m_wdata_we;
assign p_valid = qn != 0 && (!head[24] || m_wdata_valid);
assign m_wdata_ready = p_valid && p_we && p_ready;
assign m_rdata_valid = p_rvalid;
assign m_rdata = p_rdata;
always_ff @(posedge clk) begin
    if (rst) begin qw <= 0; qr <= 0; end
    else begin
        if (m_cmd_valid && m_cmd_ready) begin q[qw[1:0]] <= {m_cmd_we, m_cmd_addr[25:2]}; qw <= qw + 3'd1; end
        if (p_valid && p_ready) qr <= qr + 3'd1;
    end
end
endmodule

// ---- the Pocket's bridge (apf_wishbone_master: word addresses from 0x1000_0000)
module port_apf (
    input  wire         clk, rst,
    input  wire         cyc, stb, we,
    input  wire  [29:0] adr,
    input  wire  [31:0] dat_w,
    input  wire  [3:0]  sel,
    output logic        ack,
    output logic [31:0] dat_r,
    output logic        p_valid, p_we,
    output logic [23:0] p_addr,
    output logic [31:0] p_wdata,
    output logic [3:0]  p_be,
    input  wire         p_ready, p_rvalid,
    input  wire  [31:0] p_rdata
);
typedef enum logic [1:0] { A_IDLE, A_REQ, A_WAIT, A_ACK } ast_t;
ast_t st;
assign p_valid = st == A_REQ;
assign p_we = we;
assign p_addr = adr[23:0];
assign p_wdata = dat_w;
assign p_be = sel;
always_ff @(posedge clk) begin
    ack <= 0;
    if (rst) st <= A_IDLE;
    else case (st)
    A_IDLE: if (cyc && stb && !ack) st <= A_REQ;
    A_REQ:  if (p_ready) begin if (we) begin ack <= 1; st <= A_ACK; end else st <= A_WAIT; end
    A_WAIT: if (p_rvalid) begin dat_r <= p_rdata; ack <= 1; st <= A_ACK; end
    A_ACK:  st <= A_IDLE;                   // (the master drops its request after the ack)
    endcase
end
endmodule
`default_nettype wire
