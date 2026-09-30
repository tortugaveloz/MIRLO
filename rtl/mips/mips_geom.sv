// The geometry core on MIPS: mips_lite (with clz/clo) and a
// data side equivalent to VexRiscvGeom's, behind VexRiscvGeom's own ports,
// so that it drops into rtl/n64/soc/n64_geom.sv and sim/geom_full as is.
//
//   iBus     straight through (the ROMs' fetch port, a word the cycle after)
//   0x2000_8000..0x2000_BFFF  the TCM (dTcm_*): a word the cycle after
//   0x2000_C000 (a store)     invalidate the data cache (geom_dcache.h); the
//                             next access outside the TCM waits for it
//   0x8000_0000..             uncached (the registers): one Wishbone access
//   anything else             the data cache: 4 KiB, direct mapped, 32-byte
//                             lines, write-through without allocation, like
//                             VexRiscvGeom's (GenCoreGeomCfu.scala); a miss
//                             refills the line with one 8-beat burst (CTI 010)
//
// A load's answer comes in the core's M stage: from the TCM, or on a cache
// hit, in the first cycle; otherwise when the bus has served it. Stores wait
// in a one-entry write buffer; the bus serves it before any read, so a read
// always sees the writes before it. A store that hits also writes the line
// (the cycle after it was taken; a load taken in that cycle gets the new
// bytes forwarded). Reset invalidates the whole cache (128 cycles).
`default_nettype none

module mips_geom #(
    parameter logic [31:0] RESET_PC = 32'h2000_0000
) (
    input  wire         clk,
    input  wire         reset,
    input  wire  [31:0] externalResetVector,        // (RESET_PC)
    output logic        iBus_cmd_valid,
    input  wire         iBus_cmd_ready,
    output logic [31:0] iBus_cmd_payload_pc,
    input  wire         iBus_rsp_valid,
    input  wire         iBus_rsp_payload_error,
    input  wire  [31:0] iBus_rsp_payload_inst,
    input  wire         timerInterrupt,
    input  wire         softwareInterrupt,
    input  wire  [31:0] externalInterruptArray,
    output logic        CfuPlugin_bus_cmd_valid,
    input  wire         CfuPlugin_bus_cmd_ready,
    output logic [9:0]  CfuPlugin_bus_cmd_payload_function_id,
    output logic [31:0] CfuPlugin_bus_cmd_payload_inputs_0,
    output logic [31:0] CfuPlugin_bus_cmd_payload_inputs_1,
    input  wire         CfuPlugin_bus_rsp_valid,
    output logic        CfuPlugin_bus_rsp_ready,
    input  wire  [31:0] CfuPlugin_bus_rsp_payload_outputs_0,
    output logic        dBusWishbone_CYC,
    output logic        dBusWishbone_STB,
    input  wire         dBusWishbone_ACK,
    output logic        dBusWishbone_WE,
    output logic [29:0] dBusWishbone_ADR,
    input  wire  [31:0] dBusWishbone_DAT_MISO,
    output logic [31:0] dBusWishbone_DAT_MOSI,
    output logic [3:0]  dBusWishbone_SEL,
    input  wire         dBusWishbone_ERR,
    output logic [2:0]  dBusWishbone_CTI,
    output logic [1:0]  dBusWishbone_BTE,
    output logic        dTcm_enable,
    output logic [31:0] dTcm_address,
    output logic        dTcm_write_enable,
    output logic [31:0] dTcm_write_data,
    output logic [3:0]  dTcm_write_mask,
    input  wire  [31:0] dTcm_read_data
);

// ---- the core
logic        c_valid, c_ready, c_wr;
logic [31:0] c_adr, c_dat;
logic [1:0]  c_size;
logic        r_ready;
logic [31:0] r_dat;

// (no divider: the firmware has no division -- lang/c/mips/check_isa.py
// fails its build if one ever appears)
mips_lite #(.RESET_PC(RESET_PC), .WITH_DIV(1'b0), .WITH_CLZ(1'b1)) cpu (
    .clk, .reset,
    .iBus_cmd_valid, .iBus_cmd_ready, .iBus_cmd_payload_pc,
    .iBus_rsp_valid, .iBus_rsp_payload_error, .iBus_rsp_payload_inst,
    .timerInterrupt, .externalInterrupt(1'b0), .softwareInterrupt,
    .CfuPlugin_bus_cmd_valid, .CfuPlugin_bus_cmd_ready, .CfuPlugin_bus_cmd_payload_function_id,
    .CfuPlugin_bus_cmd_payload_inputs_0, .CfuPlugin_bus_cmd_payload_inputs_1,
    .CfuPlugin_bus_rsp_valid, .CfuPlugin_bus_rsp_ready, .CfuPlugin_bus_rsp_payload_outputs_0,
    .dBus_cmd_valid(c_valid), .dBus_cmd_ready(c_ready), .dBus_cmd_payload_wr(c_wr),
    .dBus_cmd_payload_address(c_adr), .dBus_cmd_payload_data(c_dat), .dBus_cmd_payload_size(c_size),
    .dBus_rsp_ready(r_ready), .dBus_rsp_error(1'b0), .dBus_rsp_data(r_dat),
    .tr_retire(), .tr_pc(), .tr_insn(), .tr_we(), .tr_wr(), .tr_val()
);

// ---- the command (the core's E stage)
wire       k_tcm   = c_adr[31:14] == 18'h08002;                 // 0x2000_8000
wire       k_flush = c_wr && c_adr[31:2] == 30'h0800_3000;      // 0x2000_C000
wire       k_io    = c_adr[31];
wire       k_mem   = !k_tcm && !k_flush && !k_io;
wire [3:0] c_sel   = c_size == 2'd0 ? 4'b0001 << c_adr[1:0]
                   : c_size == 2'd1 ? (c_adr[1] ? 4'b1100 : 4'b0011) : 4'b1111;

logic flushing;
logic wb_v;                                 // the write buffer holds a store
assign c_ready = k_tcm || (!flushing && (!c_wr || k_flush || !wb_v));
wire   fire = c_valid && c_ready;

assign dTcm_enable       = c_valid && k_tcm;
assign dTcm_address      = c_adr;
assign dTcm_write_enable = c_wr;
assign dTcm_write_data   = c_dat;
assign dTcm_write_mask   = c_sel;

// ---- the cache's memories: tags {valid, adr[31:12]} by line, data by word
logic [20:0] tag_ram [128];
logic [3:0][7:0] dat_ram [1024];
logic        t_we;  logic [6:0] t_wa;  logic [20:0] t_wd;
logic [20:0] tq;
logic        d_re, d_we;  logic [9:0] d_ra, d_wa;  logic [31:0] d_wd;  logic [3:0] d_be;
logic [31:0] dq;
always_ff @(posedge clk) begin
    if (t_we) tag_ram[t_wa] <= t_wd;
    if (fire && k_mem) tq <= tag_ram[c_adr[11:5]];
    for (int i = 0; i < 4; i++) if (d_we && d_be[i]) dat_ram[d_wa][i] <= d_wd[8*i +: 8];
    if (d_re) dq <= dat_ram[d_ra];
end
assign d_re = fire && k_mem && !c_wr;
assign d_ra = c_adr[11:2];

// a store's line write in the cycle a load reads the same word: forwarded
logic        fw_v;  logic [31:0] fw_d;  logic [3:0] fw_be;
always_ff @(posedge clk) begin
    fw_v <= d_we && d_re && d_wa == d_ra; fw_d <= d_wd; fw_be <= d_be;
end
logic [31:0] dq_f;
always_comb for (int i = 0; i < 4; i++) dq_f[8*i +: 8] = fw_v && fw_be[i] ? fw_d[8*i +: 8] : dq[8*i +: 8];

// ---- the load in the core's M stage
typedef enum logic [1:0] { K_NONE, K_TCM, K_MEM, K_IO } kind_t;
kind_t       m_k;
logic [31:0] m_adr;
logic        m_first;                       // its first cycle: the hit check
logic        m_miss;                        // waiting for the bus
logic        m_done;  logic [31:0] m_rdat;  // the bus's answer
wire  hit = tq[20] && tq[19:0] == m_adr[31:12];
assign r_ready = m_k == K_TCM || (m_k == K_MEM && m_first && hit) || m_done;
assign r_dat   = m_k == K_TCM ? dTcm_read_data : m_done ? m_rdat : dq_f;

// the store the cycle after it was taken: the line write if it hits
logic        s1_v;  logic [31:0] s1_adr, s1_dat;  logic [3:0] s1_sel;
wire  s1_hit = s1_v && tq[20] && tq[19:0] == s1_adr[31:12];

// ---- the bus
typedef enum logic [1:0] { B_IDLE, B_WR, B_RD, B_FILL } bst_t;
bst_t        bst;
logic [31:0] wb_adr, wb_dat;  logic [3:0] wb_sel;
logic [2:0]  beat;
logic [6:0]  fl_idx;
wire         ack = dBusWishbone_ACK;

always_comb begin
    dBusWishbone_CYC = bst != B_IDLE;
    dBusWishbone_STB = bst != B_IDLE;
    dBusWishbone_WE  = bst == B_WR;
    dBusWishbone_ADR = bst == B_WR ? wb_adr[31:2] : bst == B_FILL ? {m_adr[31:5], beat} : m_adr[31:2];
    dBusWishbone_DAT_MOSI = wb_dat;
    dBusWishbone_SEL = bst == B_WR ? wb_sel : 4'hF;
    dBusWishbone_CTI = bst == B_FILL ? (beat == 3'd7 ? 3'b111 : 3'b010) : 3'b000;
    dBusWishbone_BTE = 2'b00;
    // the memories' writes: a refill beat, a store hit, the flush
    d_we = 1'b0; d_wa = s1_adr[11:2]; d_wd = s1_dat; d_be = s1_sel;
    if (bst == B_FILL && ack) begin d_we = 1'b1; d_wa = {m_adr[11:5], beat}; d_wd = dBusWishbone_DAT_MISO; d_be = 4'hF; end
    else if (s1_hit) d_we = 1'b1;
    t_we = 1'b0; t_wa = m_adr[11:5]; t_wd = {1'b1, m_adr[31:12]};
    if (flushing) begin t_we = 1'b1; t_wa = fl_idx; t_wd = 21'd0; end
    else if (bst == B_FILL && ack && beat == 3'd7) t_we = 1'b1;
end

always_ff @(posedge clk) begin
    if (reset) begin
        m_k <= K_NONE; m_first <= 1'b0; m_miss <= 1'b0; m_done <= 1'b0;
        s1_v <= 1'b0; wb_v <= 1'b0; bst <= B_IDLE;
        flushing <= 1'b1; fl_idx <= 7'd0;
    end else begin
        // the load in M: taken, answered
        m_first <= fire && k_mem && !c_wr;
        if (fire) begin
            m_k <= c_wr ? K_NONE : k_tcm ? K_TCM : k_io ? K_IO : K_MEM;
            m_adr <= c_adr;
        end else if (r_ready) m_k <= K_NONE;
        if (r_ready) m_done <= 1'b0;
        if (m_k == K_MEM && m_first && !hit) m_miss <= 1'b1;
        // stores: to the write buffer (and the line, if it hits)
        s1_v <= fire && k_mem && c_wr;
        s1_adr <= c_adr; s1_dat <= c_dat; s1_sel <= c_sel;
        if (fire && c_wr && (k_mem || k_io)) begin
            wb_v <= 1'b1; wb_adr <= c_adr; wb_dat <= c_dat; wb_sel <= c_sel;
        end
        // the flush
        if (fire && k_flush) begin flushing <= 1'b1; fl_idx <= 7'd0; end
        else if (flushing) begin fl_idx <= fl_idx + 7'd1; if (fl_idx == 7'd127) flushing <= 1'b0; end
        // the bus: the write buffer first, then the load in M
        unique case (bst)
        B_IDLE:
            if (wb_v) bst <= B_WR;
            else if (m_miss) begin bst <= B_FILL; beat <= 3'd0; end
            else if (m_k == K_IO && !m_done) bst <= B_RD;
        B_WR: if (ack) begin wb_v <= 1'b0; bst <= B_IDLE; end
        B_RD: if (ack) begin m_done <= 1'b1; m_rdat <= dBusWishbone_DAT_MISO; bst <= B_IDLE; end
        B_FILL: if (ack) begin
            if (beat == m_adr[4:2]) m_rdat <= dBusWishbone_DAT_MISO;
            beat <= beat + 3'd1;
            if (beat == 3'd7) begin m_miss <= 1'b0; m_done <= 1'b1; bst <= B_IDLE; end
        end
        endcase
    end
end

endmodule
`default_nettype wire
