// mips_lite: a small MIPS32 integer core for Mirlo's helper cores (the audio
// core, the geometry core), in place of their VexRiscv. Drop-in: the same
// ports as VexRiscvAudio (iBus simple, dBus simple, CfuPlugin bus).
//
// Instruction set: MIPS I integer (no traps: add/addi/sub act as their
// unsigned forms), MIPS32's mul, movn/movz, lwl/lwr, mult/multu with
// HI/LO, and the CFU as one major opcode (0x1F):
//     [31:26]=0x1F  rs  rt  rd  fid[10:0]      rd = cfu(fid, rs, rt)
// With WITH_DIV, div/divu (a restoring divider, 32 cycles in E; a zero
// divisor gives what the iteration gives: see sim/mips_lite/iss.h), with
// WITH_CLZ, MIPS32's clz/clo (the geom core's firmware has both; the audio
// core's neither). Nothing else: no COP0, no exceptions or interrupts, no
// madd/msub (gcc -mno-imadd), no swl/swr (the dBus has no 3-byte store; the
// firmware has none), no traps (teq: gcc -mno-check-zero-division).
// Little-endian, like the RISC-V cores it replaces, so the firmware sees
// memory the same way.
//
// Pipeline: F D E M W, one instruction a cycle.
//   F  the PC goes out on the iBus (a synchronous memory: the word comes back
//      the next cycle)
//   D  decode; the register file (two RAM copies, registered read address)
//      is read with the arriving instruction's rs/rt
//   E  forwarding from M, W and the last write, the ALU, branches and jumps
//      (resolved here: the delay slot is in D and runs, the instruction
//      being fetched is squashed if taken), load/store address and the dBus
//      command, the CFU (E waits for its response), the multiplier's inputs
//   M  load data (M waits for the dBus response), the multiplier's product
//   W  register write
// A load or mul followed by a user stalls one cycle. div/divu hold E until
// the quotient is there and write HI/LO as they leave it.
`default_nettype none

module mips_lite #(
    parameter logic [31:0] RESET_PC = 32'h8000_0000,
    parameter bit          WITH_DIV = 1'b0,
    parameter bit          WITH_CLZ = 1'b0
) (
    input  wire         clk,
    input  wire         reset,

    output logic        iBus_cmd_valid,
    input  wire         iBus_cmd_ready,
    output logic [31:0] iBus_cmd_payload_pc,
    input  wire         iBus_rsp_valid,
    input  wire         iBus_rsp_payload_error,
    input  wire  [31:0] iBus_rsp_payload_inst,

    input  wire         timerInterrupt,
    input  wire         externalInterrupt,
    input  wire         softwareInterrupt,

    output logic        CfuPlugin_bus_cmd_valid,
    input  wire         CfuPlugin_bus_cmd_ready,
    output logic [9:0]  CfuPlugin_bus_cmd_payload_function_id,
    output logic [31:0] CfuPlugin_bus_cmd_payload_inputs_0,
    output logic [31:0] CfuPlugin_bus_cmd_payload_inputs_1,
    input  wire         CfuPlugin_bus_rsp_valid,
    output logic        CfuPlugin_bus_rsp_ready,
    input  wire  [31:0] CfuPlugin_bus_rsp_payload_outputs_0,

    output logic        dBus_cmd_valid,
    input  wire         dBus_cmd_ready,
    output logic        dBus_cmd_payload_wr,
    output logic [31:0] dBus_cmd_payload_address,
    output logic [31:0] dBus_cmd_payload_data,
    output logic [1:0]  dBus_cmd_payload_size,
    input  wire         dBus_rsp_ready,
    input  wire         dBus_rsp_error,
    input  wire  [31:0] dBus_rsp_data,

    // retire trace, at W (simulation only: unconnected in the SoC)
    output logic        tr_retire,
    output logic [31:0] tr_pc,
    output logic [31:0] tr_insn,
    output logic        tr_we,
    output logic [4:0]  tr_wr,
    output logic [31:0] tr_val
);

// ---------------------------------------------------------------- decode --
typedef enum logic [3:0] {
    A_ADD, A_SUB, A_AND, A_OR, A_XOR, A_NOR, A_SLT, A_SLTU, A_SLL, A_SRL, A_SRA, A_LUI, A_PASS_A,
    A_CLZ, A_CLO
} alu_t;

typedef struct packed {
    logic       we;          // writes a register
    logic [4:0] wr;          // which
    logic       b_imm;       // second operand: the immediate
    logic       imm_zx;      // ... zero-extended (andi/ori/xori)
    logic       sh_imm;      // shift amount from shamt
    alu_t       alu;
    logic       load, store;
    logic [1:0] size;        // 0 byte, 1 half, 2 word
    logic       lu;          // load unsigned
    logic       lwl, lwr;
    logic       br;          // conditional branch
    logic [2:0] bcond;       // 0 beq 1 bne 2 blez 3 bgtz 4 bltz 5 bgez
    logic       j, jr;       // jump (target in the instruction / a register)
    logic       link;        // writes pc + 8
    logic       mul;         // mul rd (low 32 bits, ready in M)
    logic       mult, multu; // HI/LO
    logic       div, divu;   // HI/LO, from E
    logic       mfhi, mflo, mthi, mtlo;
    logic       movn, movz;
    logic       cfu;
} ctl_t;

function automatic ctl_t decode(input logic [31:0] i);
    ctl_t c = '0;
    logic [5:0] op = i[31:26], fn = i[5:0];
    logic [4:0] rs = i[25:21], rt = i[20:16], rd = i[15:11];
    c.alu = A_ADD;
    unique case (op)
    6'h00: begin
        c.wr = rd; c.we = 1'b1;
        unique case (fn)
        6'h00: begin c.alu = A_SLL; c.sh_imm = 1'b1; end
        6'h02: begin c.alu = A_SRL; c.sh_imm = 1'b1; end
        6'h03: begin c.alu = A_SRA; c.sh_imm = 1'b1; end
        6'h04: c.alu = A_SLL;
        6'h06: c.alu = A_SRL;
        6'h07: c.alu = A_SRA;
        6'h08: begin c.we = 1'b0; c.jr = 1'b1; end
        6'h09: begin c.jr = 1'b1; c.link = 1'b1; end
        6'h0A: begin c.movz = 1'b1; c.alu = A_PASS_A; end
        6'h0B: begin c.movn = 1'b1; c.alu = A_PASS_A; end
        6'h10: c.mfhi = 1'b1;
        6'h11: begin c.we = 1'b0; c.mthi = 1'b1; end
        6'h12: c.mflo = 1'b1;
        6'h13: begin c.we = 1'b0; c.mtlo = 1'b1; end
        6'h18: begin c.we = 1'b0; c.mult = 1'b1; end
        6'h19: begin c.we = 1'b0; c.multu = 1'b1; end
        6'h1A: begin c.we = 1'b0; c.div = WITH_DIV; end
        6'h1B: begin c.we = 1'b0; c.div = WITH_DIV; c.divu = WITH_DIV; end
        6'h20, 6'h21: c.alu = A_ADD;
        6'h22, 6'h23: c.alu = A_SUB;
        6'h24: c.alu = A_AND;
        6'h25: c.alu = A_OR;
        6'h26: c.alu = A_XOR;
        6'h27: c.alu = A_NOR;
        6'h2A: c.alu = A_SLT;
        6'h2B: c.alu = A_SLTU;
        default: c.we = 1'b0;            // sync, break, ...: nothing
        endcase
    end
    6'h01: if (rt[3:1] == 3'd0) begin    // REGIMM: bltz bgez bltzal bgezal (not the traps)
        c.br = 1'b1; c.bcond = rt[0] ? 3'd5 : 3'd4;
        if (rt[4]) begin c.link = 1'b1; c.we = 1'b1; c.wr = 5'd31; end
    end
    6'h02: c.j = 1'b1;
    6'h03: begin c.j = 1'b1; c.link = 1'b1; c.we = 1'b1; c.wr = 5'd31; end
    6'h04: begin c.br = 1'b1; c.bcond = 3'd0; end
    6'h05: begin c.br = 1'b1; c.bcond = 3'd1; end
    6'h06: begin c.br = 1'b1; c.bcond = 3'd2; end
    6'h07: begin c.br = 1'b1; c.bcond = 3'd3; end
    6'h08, 6'h09: begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.alu = A_ADD;  end
    6'h0A:        begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.alu = A_SLT;  end
    6'h0B:        begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.alu = A_SLTU; end
    6'h0C: begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.imm_zx = 1'b1; c.alu = A_AND; end
    6'h0D: begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.imm_zx = 1'b1; c.alu = A_OR;  end
    6'h0E: begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.imm_zx = 1'b1; c.alu = A_XOR; end
    6'h0F: begin c.we = 1'b1; c.wr = rt; c.b_imm = 1'b1; c.alu = A_LUI; end
    6'h1C: if (fn == 6'h02) begin c.we = 1'b1; c.wr = rd; c.mul = 1'b1; end   // mul
           else if (WITH_CLZ && fn == 6'h20) begin c.we = 1'b1; c.wr = rd; c.alu = A_CLZ; end
           else if (WITH_CLZ && fn == 6'h21) begin c.we = 1'b1; c.wr = rd; c.alu = A_CLO; end
    6'h1F: begin c.we = 1'b1; c.wr = rd; c.cfu = 1'b1; end
    6'h20: begin c.we = 1'b1; c.wr = rt; c.load = 1'b1; c.size = 2'd0; end               // lb
    6'h21: begin c.we = 1'b1; c.wr = rt; c.load = 1'b1; c.size = 2'd1; end               // lh
    6'h22: begin c.we = 1'b1; c.wr = rt; c.load = 1'b1; c.size = 2'd2; c.lwl = 1'b1; end // lwl
    6'h23: begin c.we = 1'b1; c.wr = rt; c.load = 1'b1; c.size = 2'd2; end               // lw
    6'h24: begin c.we = 1'b1; c.wr = rt; c.load = 1'b1; c.size = 2'd0; c.lu = 1'b1; end  // lbu
    6'h25: begin c.we = 1'b1; c.wr = rt; c.load = 1'b1; c.size = 2'd1; c.lu = 1'b1; end  // lhu
    6'h26: begin c.we = 1'b1; c.wr = rt; c.load = 1'b1; c.size = 2'd2; c.lwr = 1'b1; end // lwr
    6'h28: begin c.store = 1'b1; c.size = 2'd0; end
    6'h29: begin c.store = 1'b1; c.size = 2'd1; end
    6'h2B: begin c.store = 1'b1; c.size = 2'd2; end
    default: ;
    endcase
    if (c.wr == 5'd0) c.we = 1'b0;
    return c;
endfunction

// ------------------------------------------------------------ pipeline regs --
logic [31:0] pc_f;                       // the PC on the iBus this cycle
logic        d_valid;  logic [31:0] d_pc;          // D: its instruction is iBus_rsp_payload_inst
logic        e_valid;  logic [31:0] e_pc, e_ins;  ctl_t e_c;
logic        m_valid;  logic [31:0] m_res;        ctl_t m_c;  logic [1:0] m_ba;
logic [31:0] m_pc, m_ins;                         // the trace
logic        w_valid;  logic [31:0] w_pc, w_ins;
logic [31:0] m_rt;                                // lwl/lwr: the old rt
logic        m_we;
logic        w_we;     logic [4:0] w_wr;   logic [31:0] w_res;
logic        x_we;     logic [4:0] x_wr;   logic [31:0] x_res;   // the write before W (RAM read latency)
logic [31:0] hi, lo;

// ---------------------------------------------------------- register file --
// Two copies (one per read port), written together at W. The read address is
// the instruction arriving in D, registered by the RAM: the data is there in E.
logic [31:0] rf_a [0:31];
logic [31:0] rf_b [0:31];
logic [31:0] rf_qa, rf_qb;
logic [4:0]  ra_d, rb_d;
wire  [31:0] ins_d = iBus_rsp_payload_inst;
logic        stall_e;                    // E cannot move this cycle
logic        stall_m;                    // M cannot move this cycle
wire         stall_d = stall_e;          // D holds with E
// while E is held, its own operands are read again every cycle: a long stall
// (the CFU, the dBus) outlasts the forwarding paths
wire  [31:0] e_ins_w;
assign ra_d = stall_d ? e_ins_w[25:21] : ins_d[25:21];
assign rb_d = stall_d ? e_ins_w[20:16] : ins_d[20:16];

always_ff @(posedge clk) begin
    if (w_we) begin rf_a[w_wr] <= w_res; rf_b[w_wr] <= w_res; end
    rf_qa <= rf_a[ra_d]; rf_qb <= rf_b[rb_d];
end

// ------------------------------------------------------------------ E ------
assign e_ins_w = e_ins;
wire  [4:0]  e_rs = e_ins[25:21], e_rt = e_ins[20:16];
logic [31:0] ea, eb;                     // forwarded rs, rt
// forwarding: M (if its result is ready: not a load / mul), W, the write before
logic        m_ready;                    // M's result is final this cycle
always_comb begin
    ea = rf_qa;
    if (x_we && x_wr == e_rs) ea = x_res;
    if (w_we && w_wr == e_rs) ea = w_res;
    if (m_we && m_c.wr == e_rs) ea = m_res;
    eb = rf_qb;
    if (x_we && x_wr == e_rt) eb = x_res;
    if (w_we && w_wr == e_rt) eb = w_res;
    if (m_we && m_c.wr == e_rt) eb = m_res;
end
// a load or mul in M with a dependent in E: wait a cycle
wire m_late = m_valid && m_we && (m_c.load || m_c.mul);
wire hazard = m_late && ((m_c.wr == e_rs) || (m_c.wr == e_rt));
// mfhi/mflo right after a mult: HI/LO are written at the end of M
wire hilo_hazard = m_valid && (m_c.mult || m_c.multu) && (e_c.mfhi || e_c.mflo);

wire [31:0] imm_sx = {{16{e_ins[15]}}, e_ins[15:0]};
wire [31:0] imm_zx = {16'h0, e_ins[15:0]};
wire [31:0] op_b   = e_c.b_imm ? (e_c.imm_zx ? imm_zx : imm_sx) : eb;
wire [4:0]  shamt  = e_c.sh_imm ? e_ins[10:6] : ea[4:0];
wire [31:0] sh_in  = eb;

// clz/clo: the leading zeros of rs (clo: of ~rs)
wire  [31:0] clz_in = e_c.alu == A_CLO ? ~ea : ea;
logic [5:0]  clz_n;
always_comb begin
    clz_n = 6'd32;
    for (int k = 0; k < 32; k++) if (clz_in[k]) clz_n = 6'(31 - k);
end

logic [31:0] alu;
always_comb begin
    unique case (e_c.alu)
    A_ADD:    alu = ea + op_b;
    A_SUB:    alu = ea - op_b;
    A_AND:    alu = ea & op_b;
    A_OR:     alu = ea | op_b;
    A_XOR:    alu = ea ^ op_b;
    A_NOR:    alu = ~(ea | op_b);
    A_SLT:    alu = {31'b0, $signed(ea) < $signed(op_b)};
    A_SLTU:   alu = {31'b0, ea < op_b};
    A_SLL:    alu = sh_in << shamt;
    A_SRL:    alu = sh_in >> shamt;
    A_SRA:    alu = $unsigned($signed(sh_in) >>> shamt);
    A_LUI:    alu = {e_ins[15:0], 16'h0};
    A_PASS_A: alu = ea;
    A_CLZ, A_CLO: alu = WITH_CLZ ? {26'h0, clz_n} : 32'h0;
    default:  alu = 'x;
    endcase
end

// branches and jumps
logic taken;
always_comb begin
    unique case (e_c.bcond)
    3'd0: taken = ea == eb;
    3'd1: taken = ea != eb;
    3'd2: taken = $signed(ea) <= 0;
    3'd3: taken = $signed(ea) > 0;
    3'd4: taken = ea[31];
    3'd5: taken = !ea[31];
    default: taken = 1'b0;
    endcase
end
wire [31:0] br_tgt = e_pc + 32'd4 + {imm_sx[29:0], 2'b00};
wire [31:0] j_tgt  = {e_pc[31:28], e_ins[25:0], 2'b00};
wire        redirect = e_valid && !stall_e && ((e_c.br && taken) || e_c.j || e_c.jr);
wire [31:0] redirect_pc = e_c.jr ? ea : e_c.j ? j_tgt : br_tgt;

// the dBus command, in E
wire [31:0] addr = ea + imm_sx;
wire        mem_op = e_valid && (e_c.load || e_c.store);
logic [31:0] st_data;
always_comb begin
    unique case (e_c.size)
    2'd0:    st_data = {4{eb[7:0]}};
    2'd1:    st_data = {2{eb[15:0]}};
    default: st_data = eb;
    endcase
end

// the multiplier: operands registered at the end of E, product in M (DSPs)
logic [63:0] prod;
logic [32:0] mul_a, mul_b;
always_ff @(posedge clk) if (!stall_m) begin
    mul_a <= {e_c.mult & ea[31], ea};
    mul_b <= {e_c.mult & eb[31], eb};
end
assign prod = 64'($signed(mul_a) * $signed(mul_b));

// the CFU: issued from E, E waits for the response (kept if E is held)
logic cfu_sent, cfu_got; logic [31:0] cfu_val;
wire  cfu_op   = e_valid && e_c.cfu;
wire  cfu_fire = CfuPlugin_bus_cmd_valid && CfuPlugin_bus_cmd_ready;
wire  cfu_rsp  = CfuPlugin_bus_rsp_valid && (cfu_sent || cfu_fire);
wire  cfu_have = cfu_got || cfu_rsp;
assign CfuPlugin_bus_cmd_valid = cfu_op && !cfu_sent && !cfu_got && !hazard;
assign CfuPlugin_bus_cmd_payload_function_id = e_ins[9:0];
assign CfuPlugin_bus_cmd_payload_inputs_0 = ea;
assign CfuPlugin_bus_cmd_payload_inputs_1 = eb;
assign CfuPlugin_bus_rsp_ready = 1'b1;
always_ff @(posedge clk) begin
    if (reset || (e_valid && !stall_e)) begin cfu_sent <= 1'b0; cfu_got <= 1'b0; end
    else begin
        if (cfu_rsp) begin cfu_got <= 1'b1; cfu_val <= CfuPlugin_bus_rsp_payload_outputs_0; end
        if (cfu_fire) cfu_sent <= 1'b1;
    end
end

// the divider: started from E (operands as magnitudes), one quotient bit a
// cycle, E held until done; the signs go back on as the result leaves E
logic        div_busy, div_done, dv_sq, dv_sr;
logic [4:0]  div_cnt;
logic [31:0] dv_b, dv_q, dv_r;
wire         div_op = WITH_DIV && e_valid && e_c.div;
wire  [32:0] dv_sh  = {dv_r, dv_q[31]};                 // the remainder, the next bit in
wire  [33:0] dv_df  = {1'b0, dv_sh} - {2'b0, dv_b};
wire         dv_ge  = !dv_df[33];
always_ff @(posedge clk) begin
    if (reset || (e_valid && !stall_e)) begin div_busy <= 1'b0; div_done <= 1'b0; end
    else if (div_op && !div_busy && !div_done && !hazard) begin
        div_busy <= 1'b1; div_cnt <= 5'd0;
        dv_b <= (!e_c.divu && eb[31]) ? -eb : eb;
        dv_q <= (!e_c.divu && ea[31]) ? -ea : ea;
        dv_r <= 32'd0;
        dv_sq <= !e_c.divu && (ea[31] ^ eb[31]);
        dv_sr <= !e_c.divu && ea[31];
    end else if (div_busy) begin
        dv_r <= dv_ge ? dv_df[31:0] : dv_sh[31:0];
        dv_q <= {dv_q[30:0], dv_ge};
        div_cnt <= div_cnt + 5'd1;
        if (div_cnt == 5'd31) begin div_busy <= 1'b0; div_done <= 1'b1; end
    end
end
wire [31:0] div_q = dv_sq ? -dv_q : dv_q;
wire [31:0] div_r = dv_sr ? -dv_r : dv_r;

assign dBus_cmd_valid = mem_op && !hazard && !stall_m;
assign dBus_cmd_payload_wr = e_c.store;
assign dBus_cmd_payload_address = addr;
assign dBus_cmd_payload_data = st_data;
assign dBus_cmd_payload_size = e_c.size;

// E's result
logic [31:0] e_res;
always_comb begin
    if (e_c.link)      e_res = e_pc + 32'd8;
    else if (e_c.mfhi) e_res = hi;
    else if (e_c.mflo) e_res = lo;
    else if (e_c.cfu)  e_res = cfu_got ? cfu_val : CfuPlugin_bus_rsp_payload_outputs_0;
    else               e_res = alu;
end
wire e_we = e_c.we && !(e_c.movz && eb != 0) && !(e_c.movn && eb == 0);

assign stall_e = e_valid && (hazard || hilo_hazard || stall_m
                 || (mem_op && !dBus_cmd_ready)
                 || (cfu_op && !cfu_have)
                 || (div_op && !div_done));

// ------------------------------------------------------------------ M ------
wire m_wait = m_valid && m_c.load && !dBus_rsp_ready;
assign stall_m = m_wait;
logic [31:0] ld;
wire  [31:0] ldw = dBus_rsp_data;
wire  [7:0]  ldb = ldw[8*m_ba +: 8];
wire  [15:0] ldh = m_ba[1] ? ldw[31:16] : ldw[15:0];
always_comb begin
    unique case (m_c.size)
    2'd0: ld = m_c.lu ? {24'h0, ldb} : {{24{ldb[7]}}, ldb};
    2'd1: ld = m_c.lu ? {16'h0, ldh} : {{16{ldh[15]}}, ldh};
    default: begin
        ld = ldw;
        // little-endian lwr: the word's bytes from addr up, into rt's low end
        if (m_c.lwr) unique case (m_ba)
            2'd0: ld = ldw;
            2'd1: ld = {m_rt[31:24], ldw[31:8]};
            2'd2: ld = {m_rt[31:16], ldw[31:16]};
            2'd3: ld = {m_rt[31:8],  ldw[31:24]};
        endcase
        // lwl: the word's bytes up to addr, into rt's high end
        if (m_c.lwl) unique case (m_ba)
            2'd0: ld = {ldw[7:0],  m_rt[23:0]};
            2'd1: ld = {ldw[15:0], m_rt[15:0]};
            2'd2: ld = {ldw[23:0], m_rt[7:0]};
            2'd3: ld = ldw;
        endcase
    end
    endcase
end
wire [31:0] m_out = m_c.load ? ld : m_c.mul ? prod[31:0] : m_res;

// ----------------------------------------------------------- sequencing ----
assign iBus_cmd_valid = 1'b1;
assign iBus_cmd_payload_pc = stall_d ? d_pc : pc_f;   // held: D's word again

always_ff @(posedge clk) begin
    if (reset) begin
        pc_f <= RESET_PC;
        d_valid <= 1'b0; e_valid <= 1'b0; m_valid <= 1'b0;
        m_we <= 1'b0; w_we <= 1'b0; x_we <= 1'b0; w_valid <= 1'b0;
    end else begin
        // W <- M, and the write before
        x_we <= w_we; x_wr <= w_wr; x_res <= w_res;
        if (!stall_m) begin
            w_valid <= m_valid; w_pc <= m_pc; w_ins <= m_ins;
            w_we <= m_valid && m_we;
            w_wr <= m_c.wr;
            w_res <= m_out;
            if (m_valid && m_c.mult | m_c.multu) begin hi <= prod[63:32]; lo <= prod[31:0]; end
        end else begin w_we <= 1'b0; w_valid <= 1'b0; end
        // M <- E
        if (!stall_m) begin
            m_valid <= e_valid && !stall_e;
            m_c <= e_c; m_res <= e_res; m_ba <= addr[1:0]; m_rt <= eb;
            m_pc <= e_pc; m_ins <= e_ins;
            m_we <= e_valid && !stall_e && e_we;
            if (e_valid && !stall_e && e_c.mthi) hi <= ea;
            if (e_valid && !stall_e && e_c.mtlo) lo <= ea;
            if (WITH_DIV && e_valid && !stall_e && e_c.div) begin hi <= div_r; lo <= div_q; end
        end
        // E <- D
        if (!stall_e) begin
            e_valid <= d_valid && iBus_rsp_valid;
            e_pc <= d_pc; e_ins <= ins_d; e_c <= decode(ins_d);
        end
        // D <- F: squash what is being fetched after a taken branch's delay slot
        if (!stall_d) begin
            d_valid <= !redirect;
            d_pc <= pc_f;
            pc_f <= redirect ? redirect_pc : pc_f + 32'd4;
        end
    end
end

assign tr_retire = w_valid;
assign tr_pc = w_pc;
assign tr_insn = w_ins;
assign tr_we = w_we;
assign tr_wr = w_wr;
assign tr_val = w_res;

endmodule
`default_nettype wire
