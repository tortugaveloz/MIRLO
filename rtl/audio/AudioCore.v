// Audio core: VexRiscvAudio (rv32i + Zmmul, no FPU, no divider, no caches)
// with its own IMEM and DMEM, and a CFU of audio DSP instructions
// (AudioDspCfu.v: the ucode's envelope mixer and resampler). It plays the N64 RSP's audio role (runs the
// ABI2 command list src/audio/synthesis.c builds) and the AI's (streams PCM
// into the APF audio FIFO).
//
// Address map -- identical from the core and from the SoC, so pointers can
// be shared. 0x8000_0000 is in the game CPU's uncached I/O region, so the
// game CPU never sees stale mailbox/status words.
//
//   0x8000_0000  IMEM  8 KiB  code. Core: fetch only. SoC: write; read only
//                             while the core is held in reset (verify).
//   0x8000_2000  DMEM  8 KiB  .rodata/.data/.bss/stack/work buffers.
//                             Core and SoC: read/write (true dual port).
//   0x8000_4000  CTRL          see below. Core and SoC.
//   anything else (core dBus)  -> Wishbone master to the SoC bus (SDRAM,
//                                  CSRs such as the APF audio FIFO).
//
// CTRL registers (word offsets from 0x8000_4000):
//   0x00 RUN     SoC RW, core RO. bit0 = 1 releases the core's reset. Resets
//                to 0: the core stays in reset until the game CPU has
//                loaded IMEM/DMEM and sets it.
//   0x04 CYCLES  RO. Free-running sys-clock counter (profiling; the core has
//                no mcycle CSR).
//   0x08..0x24   MBOX[0..7], RW from both sides (message words; if both
//                write in the same cycle the SoC wins).
//
// Timing of the core's buses:
//   iBus: cmd.ready is always 1, the response arrives the next cycle.
//   dBus local: cmd.ready is 1, a read response arrives the next cycle and
//     is HELD until the next command, so a memory stage that is stalled for
//     another reason still sees it.
//   dBus external: cmd.ready is held low until the Wishbone ack; the read
//     data is registered and presented the cycle after.
`default_nettype none

module AudioCore_tdp8 #(parameter AW = 11) (
    input  wire          clk,
    input  wire          en_a,
    input  wire          we_a,
    input  wire [AW-1:0] addr_a,
    input  wire [7:0]    d_a,
    output reg  [7:0]    q_a,
    input  wire          en_b,
    input  wire          we_b,
    input  wire [AW-1:0] addr_b,
    input  wire [7:0]    d_b,
    output reg  [7:0]    q_b
);
    reg [7:0] ram [0:(1<<AW)-1];
    always @(posedge clk) begin
        if (en_a) begin
            if (we_a) begin ram[addr_a] <= d_a; q_a <= d_a; end
            else q_a <= ram[addr_a];
        end
    end
    always @(posedge clk) begin
        if (en_b) begin
            if (we_b) begin ram[addr_b] <= d_b; q_b <= d_b; end
            else q_b <= ram[addr_b];
        end
    end
endmodule

// Simple dual port: one write port, one read port (the canonical M10K
// template). IMEM uses this, not the true-dual-port one: with its core-side
// port read-only, Quartus 25.1 declined to infer the TDP template ("uninferred
// due to asynchronous read logic") and built 64 Kbit of IMEM out of registers.
module AudioCore_sdp8 #(parameter AW = 11) (
    input  wire          clk,
    input  wire          we,
    input  wire [AW-1:0] waddr,
    input  wire [7:0]    d,
    input  wire          re,
    input  wire [AW-1:0] raddr,
    output reg  [7:0]    q
);
    reg [7:0] ram [0:(1<<AW)-1];
    always @(posedge clk) begin
        if (we) ram[waddr] <= d;
        if (re) q <= ram[raddr];
    end
endmodule

module AudioCore #(
    parameter IMEM_AW = 11,   // words: 2^11 * 4 = 8 KiB
    parameter DMEM_AW = 11
) (
    input  wire        clk,
    input  wire        rst,

    // SoC -> audio core (Wishbone slave, word addressed, classic)
    input  wire [29:0] s_adr,
    input  wire [31:0] s_dat_w,
    output reg  [31:0] s_dat_r,
    input  wire [3:0]  s_sel,
    input  wire        s_cyc,
    input  wire        s_stb,
    input  wire        s_we,
    output reg         s_ack,

    // audio core -> SoC (Wishbone master, word addressed, classic)
    output reg  [29:0] m_adr,
    output reg  [31:0] m_dat_w,
    input  wire [31:0] m_dat_r,
    output reg  [3:0]  m_sel,
    output reg         m_cyc,
    output reg         m_stb,
    output reg         m_we,
    input  wire        m_ack,
    input  wire        m_err,

    output wire        running
);
    // ---------------------------------------------------------------- CPU
    wire        ib_cmd_valid;
    wire [31:0] ib_cmd_pc;
    reg         ib_rsp_valid;
    wire [31:0] ib_rsp_inst;

    wire        db_cmd_valid;
    wire        db_cmd_ready;
    wire        db_cmd_wr;
    wire [31:0] db_cmd_addr;
    wire [31:0] db_cmd_data;
    wire [1:0]  db_cmd_size;
    reg         db_rsp_ready;
    wire [31:0] db_rsp_data;

    reg run;
    assign running = run;
    wire core_rst = rst | ~run;

    VexRiscvAudio cpu (
        .clk                      (clk),
        .reset                    (core_rst),
        .timerInterrupt           (1'b0),
        .externalInterrupt        (1'b0),
        .softwareInterrupt        (1'b0),
        .iBus_cmd_valid           (ib_cmd_valid),
        .iBus_cmd_ready           (1'b1),
        .iBus_cmd_payload_pc      (ib_cmd_pc),
        .iBus_rsp_valid           (ib_rsp_valid),
        .iBus_rsp_payload_error   (1'b0),
        .iBus_rsp_payload_inst    (ib_rsp_inst),
        .dBus_cmd_valid           (db_cmd_valid),
        .dBus_cmd_ready           (db_cmd_ready),
        .dBus_cmd_payload_wr      (db_cmd_wr),
        .dBus_cmd_payload_address (db_cmd_addr),
        .dBus_cmd_payload_data    (db_cmd_data),
        .dBus_cmd_payload_size    (db_cmd_size),
        .dBus_rsp_ready           (db_rsp_ready),
        .dBus_rsp_error           (1'b0),
        .dBus_rsp_data            (db_rsp_data),
        .CfuPlugin_bus_cmd_valid               (cfu_cmd_valid),
        .CfuPlugin_bus_cmd_ready               (cfu_cmd_ready),
        .CfuPlugin_bus_cmd_payload_function_id (cfu_fid),
        .CfuPlugin_bus_cmd_payload_inputs_0    (cfu_in0),
        .CfuPlugin_bus_cmd_payload_inputs_1    (cfu_in1),
        .CfuPlugin_bus_rsp_valid               (cfu_rsp_valid),
        .CfuPlugin_bus_rsp_ready               (cfu_rsp_ready),
        .CfuPlugin_bus_rsp_payload_outputs_0   (cfu_out)
    );

    // Audio DSP instructions (envelope mixer, resampler): AudioDspCfu.v
    wire        cfu_cmd_valid, cfu_cmd_ready, cfu_rsp_valid, cfu_rsp_ready;
    wire [9:0]  cfu_fid;
    wire [31:0] cfu_in0, cfu_in1, cfu_out;
    AudioDspCfu dsp (
        .clk             (clk),
        .rst             (core_rst),
        .cmd_valid       (cfu_cmd_valid),
        .cmd_ready       (cfu_cmd_ready),
        .cmd_function_id (cfu_fid),
        .cmd_inputs_0    (cfu_in0),
        .cmd_inputs_1    (cfu_in1),
        .rsp_valid       (cfu_rsp_valid),
        .rsp_ready       (cfu_rsp_ready),
        .rsp_outputs_0   (cfu_out)
    );

    // ------------------------------------------------------- SoC decode
    // Word address bits [12:11] of the 0x8000 window: 0 IMEM, 1 DMEM, 2 CTRL.
    wire [1:0] s_blk   = s_adr[12:11];
    wire       s_req   = s_cyc & s_stb & ~s_ack;
    wire       s_imem  = s_req & (s_blk == 2'd0);
    wire       s_dmem  = s_req & (s_blk == 2'd1);
    wire       s_ctrl  = s_req & (s_blk == 2'd2);
    reg  [1:0] s_blk_q;

    // ----------------------------------------------------- core dBus decode
    // Local window 0x8000_0000..0x8000_7FFF.
    wire       d_local = (db_cmd_addr[31:15] == 17'h10000);
    wire [1:0] d_blk   = db_cmd_addr[14:13];
    wire       d_dmem  = d_local & (d_blk == 2'd1);
    wire       d_ctrl  = d_local & (d_blk == 2'd2);
    wire       d_ext   = ~d_local;
    reg  [3:0] d_mask;
    always @(*) begin
        case (db_cmd_size)
            2'd0:    d_mask = 4'b0001 << db_cmd_addr[1:0];
            2'd1:    d_mask = 4'b0011 << {db_cmd_addr[1], 1'b0};
            default: d_mask = 4'b1111;
        endcase
    end

    // External: one Wishbone transaction at a time; the command is accepted
    // on the ack cycle.
    wire ext_done  = m_cyc & (m_ack | m_err);
    assign db_cmd_ready = d_ext ? ext_done : 1'b1;
    wire d_fire    = db_cmd_valid & db_cmd_ready;

    // ------------------------------------------------------------- IMEM
    // Written by the SoC. The read port belongs to the iBus while the core
    // runs; while it is held in reset the SoC reads through it (the loader's
    // verify pass). A SoC read of IMEM while the core runs returns junk.
    wire [31:0] imem_q;
    wire        imem_soc_rd = s_imem & ~s_we & ~run;
    wire [IMEM_AW-1:0] imem_raddr = run ? ib_cmd_pc[IMEM_AW+1:2] : s_adr[IMEM_AW-1:0];
    genvar gi;
    generate for (gi = 0; gi < 4; gi = gi + 1) begin : g_imem
        AudioCore_sdp8 #(.AW(IMEM_AW)) ram (
            .clk   (clk),
            .we    (s_imem & s_we & s_sel[gi]),
            .waddr (s_adr[IMEM_AW-1:0]),
            .d     (s_dat_w[8*gi +: 8]),
            .re    (run ? ib_cmd_valid : imem_soc_rd),
            .raddr (imem_raddr),
            .q     (imem_q[8*gi +: 8])
        );
    end endgenerate
    assign ib_rsp_inst = imem_q;
    wire [31:0] imem_q_b = imem_q;

    always @(posedge clk) begin
        if (core_rst) ib_rsp_valid <= 1'b0;
        else          ib_rsp_valid <= ib_cmd_valid;
    end

    // ------------------------------------------------------------- DMEM
    wire [31:0] dmem_q_a, dmem_q_b;
    wire        d_dmem_fire = d_fire & d_dmem;
    generate for (gi = 0; gi < 4; gi = gi + 1) begin : g_dmem
        AudioCore_tdp8 #(.AW(DMEM_AW)) ram (
            .clk    (clk),
            .en_a   (d_dmem_fire),
            .we_a   (d_dmem_fire & db_cmd_wr & d_mask[gi]),
            .addr_a (db_cmd_addr[DMEM_AW+1:2]),
            .d_a    (db_cmd_data[8*gi +: 8]),
            .q_a    (dmem_q_a[8*gi +: 8]),
            .en_b   (s_dmem),
            .we_b   (s_dmem & s_we & s_sel[gi]),
            .addr_b (s_adr[DMEM_AW-1:0]),
            .d_b    (s_dat_w[8*gi +: 8]),
            .q_b    (dmem_q_b[8*gi +: 8])
        );
    end endgenerate

    // ------------------------------------------------------------- CTRL
    reg  [31:0] cycles;
    reg  [31:0] mbox [0:7];
    // Word index inside CTRL: 0 RUN, 1 CYCLES, 2..9 MBOX[0..7].
    wire [3:0]  s_ci = s_adr[3:0];
    wire [3:0]  d_ci = db_cmd_addr[5:2];
    function [31:0] ctrl_read(input [3:0] i);
        begin
            if (i == 4'd0)      ctrl_read = {31'd0, run};
            else if (i == 4'd1) ctrl_read = cycles;
            else if (i >= 4'd2 && i <= 4'd9) ctrl_read = mbox[i - 4'd2];
            else                ctrl_read = 32'd0;
        end
    endfunction

    reg [31:0] s_ctrl_q;
    integer k;
    always @(posedge clk) begin
        cycles <= cycles + 32'd1;
        if (rst) begin
            run <= 1'b0;
            for (k = 0; k < 8; k = k + 1) mbox[k] <= 32'd0;
        end else begin
            // core side first, SoC second: the SoC wins a same-cycle clash
            if (d_fire & d_ctrl & db_cmd_wr & d_ci >= 4'd2 & d_ci <= 4'd9)
                mbox[d_ci - 4'd2] <= db_cmd_data;
            if (s_ctrl & s_we) begin
                if (s_ci == 4'd0) run <= s_dat_w[0];
                else if (s_ci >= 4'd2 && s_ci <= 4'd9) mbox[s_ci - 4'd2] <= s_dat_w;
            end
        end
        if (s_ctrl) s_ctrl_q <= ctrl_read(s_ci);
    end

    // -------------------------------------------------- SoC slave response
    always @(posedge clk) begin
        if (rst) s_ack <= 1'b0;
        else     s_ack <= s_req;
        if (s_req) s_blk_q <= s_blk;
    end
    always @(*) begin
        case (s_blk_q)
            2'd0:    s_dat_r = imem_q_b;
            2'd1:    s_dat_r = dmem_q_b;
            default: s_dat_r = s_ctrl_q;
        endcase
    end

    // --------------------------------------------- core dBus response path
    // rsp_sel: 0 DMEM (the RAM's own output register holds it), 1 held reg.
    reg        rsp_from_dmem;
    reg [31:0] rsp_hold;
    always @(posedge clk) begin
        if (core_rst) begin
            db_rsp_ready <= 1'b0;
        end else if (d_fire) begin
            db_rsp_ready  <= ~db_cmd_wr;
            rsp_from_dmem <= d_dmem;
            if (d_ctrl)     rsp_hold <= ctrl_read(d_ci);
            else if (d_ext) rsp_hold <= m_dat_r;
            else if (~d_dmem) rsp_hold <= 32'd0;   // IMEM is not readable from the dBus
        end
    end
    assign db_rsp_data = rsp_from_dmem ? dmem_q_a : rsp_hold;

    // ------------------------------------------------- Wishbone master out
    always @(posedge clk) begin
        if (core_rst) begin
            m_cyc <= 1'b0;
            m_stb <= 1'b0;
        end else if (m_cyc) begin
            if (m_ack | m_err) begin
                m_cyc <= 1'b0;
                m_stb <= 1'b0;
            end
        end else if (db_cmd_valid & d_ext) begin
            m_cyc   <= 1'b1;
            m_stb   <= 1'b1;
            m_we    <= db_cmd_wr;
            m_adr   <= db_cmd_addr[31:2];
            m_dat_w <= db_cmd_data;
            m_sel   <= d_mask;
        end
    end
endmodule

`default_nettype wire
