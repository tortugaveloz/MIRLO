// Mirlo's SoC on MIPS, without LiteX (docs/mips.md): what core_top.sv
// instantiates in place of the LiteX `litex` module under MIRLO_MIPS -- the
// same ports. Its fabric is Mirlo-N64's (MIRLO64: rtl/soc/sdr_*, n64_bus,
// n64_ports, n64_video); its three cores are MIPS:
//
//   the game CPU (mips_sys: mips_core, little-endian) --\
//   MRDP -----------------------------------------------+-- sdr_arb -- sdr_ctrl -- sdr_phy -- SDRAM
//   the scan-out ---------------------------------------|
//   n64_bus (the geom core + the audio core) -----------|
//   the Pocket's bridge --------------------------------/
//
// The map is MIRLO's (lang/c keeps its addresses):
//   0x2000_0000  the geom core's ROM (its own), 0x2000_8000 its RAM (the TCM;
//                the game CPU reads and writes it through the TCM's port B)
//   0x4000_0000  SDRAM, 64 MiB
//   0x8000_0000  the audio core (IMEM, DMEM, CTRL: the game CPU loads it)
//   0xF000_0000  the registers (mirlo_regs, tools/mirlo_regs.py)
//   0xBFC0_0000  the game CPU's boot ROM (lang/c/boot: waits for the Pocket
//                to load the game into SDRAM, then jumps to 0x4000_0000)
// The SDRAM initialises itself; boot_ready tells the Pocket it may load.
`default_nettype none

`include "mirlo_mips_init.svh"

module mirlo_mips #(
    parameter BOOTROM = `MIPS_INIT_BOOTROM,
    parameter GEOM_ROM_A = `MIPS_INIT_GEOM_A, parameter GEOM_ROM_B = `MIPS_INIT_GEOM_B,
    parameter int SDR_T_INIT = 12600
) (
    input  wire          altera_reserved_tck,
    input  wire          altera_reserved_tdi,
    output wire          altera_reserved_tdo,
    input  wire          altera_reserved_tms,
    input  wire   [11:0] apf_audio_buffer_fill,
    output wire   [31:0] apf_audio_bus_out,
    output wire          apf_audio_bus_wr,
    output wire          apf_audio_flush,
    output wire          apf_audio_playback_en,
    output wire          apf_bridge_boot_ready,
    input  wire    [2:0] apf_bridge_command_result_code,
    input  wire          apf_bridge_complete_trigger,
    input  wire   [31:0] apf_bridge_current_address,
    output wire   [31:0] apf_bridge_data_offset,
    input  wire   [31:0] apf_bridge_file_size,
    output wire          apf_bridge_file_size_wr,
    input  wire          apf_bridge_host_loaded,
    input  wire          apf_bridge_host_reset_n,
    output wire   [31:0] apf_bridge_length,
    output wire   [31:0] apf_bridge_new_file_size_data,
    output wire   [31:0] apf_bridge_ram_data_address,
    output wire          apf_bridge_request_getfile,
    output wire          apf_bridge_request_openfile,
    output wire          apf_bridge_request_read,
    output wire          apf_bridge_request_write,
    output wire   [15:0] apf_bridge_slot_id,
    input  wire   [63:0] apf_id_chip_id,
    input  wire   [31:0] apf_input_cont1_joy,
    input  wire   [31:0] apf_input_cont1_key,
    input  wire   [31:0] apf_input_cont1_trig,
    input  wire   [31:0] apf_input_cont2_joy,
    input  wire   [31:0] apf_input_cont2_key,
    input  wire   [31:0] apf_input_cont2_trig,
    input  wire   [31:0] apf_input_cont3_joy,
    input  wire   [31:0] apf_input_cont3_key,
    input  wire   [31:0] apf_input_cont3_trig,
    input  wire   [31:0] apf_input_cont4_joy,
    input  wire   [31:0] apf_input_cont4_key,
    input  wire   [31:0] apf_input_cont4_trig,
    input  wire    [3:0] apf_interact_address,
    input  wire   [31:0] apf_interact_data,
    output logic  [31:0] apf_interact_q,
    input  wire          apf_interact_wr,
    input  wire   [31:0] apf_rtc_date_bcd,
    input  wire   [31:0] apf_rtc_time_bcd,
    input  wire   [31:0] apf_rtc_unix_seconds,
    input  wire          clk_sys,
    input  wire          clk_sys2x,
    input  wire          clk_sys2x_90deg,
    input  wire          clk_vid,
    input  wire          reset,
    output logic  [12:0] sdram_a,
    output logic   [1:0] sdram_ba,
    output logic         sdram_cas_n,
    output logic         sdram_cke,
    output wire          sdram_clock,
    output logic   [1:0] sdram_dm,
    inout  wire   [15:0] sdram_dq,
    output logic         sdram_ras_n,
    output logic         sdram_we_n,
    input  wire          serial_rx,
    output wire          serial_tx,
    input  wire          use_jtag,
    output logic   [4:0] vga_b,
    output logic         vga_de,
    output logic   [5:0] vga_g,
    output logic         vga_hsync,
    output logic   [4:0] vga_r,
    output logic         vga_vsync,
    input  wire          wishbone_ack,
    output wire   [29:0] wishbone_adr,
    output wire    [1:0] wishbone_bte,
    output wire    [2:0] wishbone_cti,
    output wire          wishbone_cyc,
    input  wire   [31:0] wishbone_dat_r,
    output wire   [31:0] wishbone_dat_w,
    input  wire          wishbone_err,
    output wire          wishbone_master_ack,
    input  wire   [29:0] wishbone_master_adr,
    input  wire    [1:0] wishbone_master_bte,
    input  wire    [2:0] wishbone_master_cti,
    input  wire          wishbone_master_cyc,
    output wire   [31:0] wishbone_master_dat_r,
    input  wire   [31:0] wishbone_master_dat_w,
    output wire          wishbone_master_err,
    input  wire    [3:0] wishbone_master_sel,
    input  wire          wishbone_master_stb,
    input  wire          wishbone_master_we,
    output wire    [3:0] wishbone_sel,
    output wire          wishbone_stb,
    output wire          wishbone_we
);

// ---------------------------------------------------------------- clocks, resets
wire clk = clk_sys;
logic rst, rst2x, rst_vid, rv1;
always_ff @(posedge clk) rst <= reset;
always_ff @(posedge clk_sys2x) rst2x <= reset;
always_ff @(posedge clk_vid) begin rv1 <= reset; rst_vid <= rv1; end

// ---------------------------------------------------------------- unused pads
assign serial_tx = 1'b1;
assign wishbone_adr = 0; assign wishbone_bte = 0; assign wishbone_cti = 0; assign wishbone_cyc = 0;
assign wishbone_dat_w = 0; assign wishbone_sel = 0; assign wishbone_stb = 0; assign wishbone_we = 0;

// ---------------------------------------------------------------- SDRAM
localparam int NP = 5;              // 0 scan-out, 1 the game CPU, 2 MRDP, 3 geom + audio, 4 the bridge
logic [NP-1:0]       p_valid, p_we, p_urgent, p_ready, p_rvalid;
logic [NP-1:0][23:0] p_addr;
logic [NP-1:0][31:0] p_wdata;
logic [NP-1:0][3:0]  p_be;
logic [31:0]         p_rdata;
logic        rq_valid, rq_we, rq_ready, rd_valid, sdr_ready;
logic [23:0] rq_addr;
logic [31:0] rq_wdata, rd_data;
logic [3:0]  rq_be;
logic [2:0]  rq_tag, rd_tag;
sdr_arb #(.N(NP)) arb (.clk, .rst, .p_valid, .p_we, .p_addr, .p_wdata, .p_be, .p_urgent, .p_ready, .p_rvalid, .p_rdata,
    .req_valid(rq_valid), .req_we(rq_we), .req_addr(rq_addr), .req_wdata(rq_wdata), .req_be(rq_be), .req_tag(rq_tag),
    .req_ready(rq_ready), .rd_valid, .rd_data, .rd_tag);
logic [1:0]  dfi_ras_n, dfi_cas_n, dfi_we_n;
logic [25:0] dfi_addr;
logic [3:0]  dfi_ba, dfi_wm;
logic        dfi_cke, dfi_wren, dfi_rden, dfi_rv;
logic [31:0] dfi_wd, dfi_rd;
sdr_ctrl #(.T_INIT(SDR_T_INIT)) sdram (.clk, .rst, .ready(sdr_ready),
    .req_valid(rq_valid), .req_we(rq_we), .req_addr(rq_addr), .req_wdata(rq_wdata), .req_be(rq_be), .req_tag(rq_tag),
    .req_ready(rq_ready), .rd_valid, .rd_data, .rd_tag,
    .dfi_ras_n, .dfi_cas_n, .dfi_we_n, .dfi_addr, .dfi_ba, .dfi_cke, .dfi_wrdata_en(dfi_wren), .dfi_wrdata(dfi_wd),
    .dfi_wrdata_mask(dfi_wm), .dfi_rddata_en(dfi_rden), .dfi_rddata(dfi_rd), .dfi_rddata_valid(dfi_rv));
sdr_phy phy (.clk, .rst, .clk2x(clk_sys2x), .rst2x, .clk2x_90(clk_sys2x_90deg),
    .ras_n(dfi_ras_n), .cas_n(dfi_cas_n), .we_n(dfi_we_n), .addr(dfi_addr), .ba(dfi_ba), .cke(dfi_cke),
    .wrdata_en(dfi_wren), .wrdata(dfi_wd), .wrdata_mask(dfi_wm), .rddata_en(dfi_rden), .rddata(dfi_rd), .rddata_valid(dfi_rv),
    .sdram_a, .sdram_ba, .sdram_ras_n, .sdram_cas_n, .sdram_we_n, .sdram_cke, .sdram_dm, .sdram_dq, .sdram_clock);
assign apf_bridge_boot_ready = sdr_ready;           // the Pocket may write the slots

// ---------------------------------------------------------------- the game CPU
logic        s_req, s_we, s_line, s_ack, s_rvalid;
logic [31:0] s_addr, s_wdata, s_rdata;
logic [3:0]  s_strb;
logic        x_req, x_we, x_ack;
logic [31:0] x_addr, x_wdata, x_rdata;
logic [3:0]  x_strb;
logic [31:0] dbg_cpu_pc, dbg_cause, dbg_epc;
logic        game_irq, geom_irq;
mips_sys #(.BOOTROM(BOOTROM)) sys (.clk, .rst,
    .s_req, .s_we, .s_addr, .s_wdata, .s_strb, .s_line, .s_ack, .s_rvalid, .s_rdata,
    .x_req, .x_we, .x_addr, .x_wdata, .x_strb, .x_ack, .x_rdata,
    .irq(game_irq), .dbg_pc(dbg_cpu_pc), .dbg_cause, .dbg_epc);
port_n64sys #(.FLAT(1)) pc (.clk, .rst, .s_req, .s_we, .s_line, .s_addr, .s_wdata, .s_strb, .s_ack, .s_rvalid, .s_rdata,
    .p_valid(p_valid[1]), .p_we(p_we[1]), .p_addr(p_addr[1]), .p_wdata(p_wdata[1]), .p_be(p_be[1]),
    .p_ready(p_ready[1]), .p_rvalid(p_rvalid[1]), .p_rdata);
assign p_urgent[1] = 0;

// the CPU's device port: one access at a time, acknowledged for a cycle (a
// request still up in the cycle after its ack is the same one: ignored)
wire x_regs  = x_addr[31:16] == 16'hF000;
wire x_audio = x_addr[31:15] == 17'h1_0000;          // 0x8000_0000 - 0x8000_7FFF
wire x_tcm   = x_addr[31:14] == 18'h0_8002;          // 0x2000_8000 - 0x2000_BFFF
typedef enum logic [2:0] { X_IDLE, X_REGS, X_REGD, X_AUDIO, X_TCM, X_TCMD, X_ACK } xst_t;
xst_t xs;
logic        xr_go;                                  // the CPU's register access, this cycle
logic        bus_r_req;
logic [15:0] bus_errors;
logic [31:0] r_rdata, a_s_dat_r, t_dat_r;
logic        a_s_ack;
always_ff @(posedge clk) begin
    x_ack <= 0;
    if (rst) begin xs <= X_IDLE; bus_errors <= 0; end
    else case (xs)
    X_IDLE: if (x_req && !x_ack) begin
        if (x_regs) xs <= X_REGS;
        else if (x_audio) xs <= X_AUDIO;
        else if (x_tcm) xs <= X_TCM;
        else begin x_rdata <= 0; x_ack <= 1; if (bus_errors != 16'hFFFF) bus_errors <= bus_errors + 16'd1; end
    end
    X_REGS: if (!bus_r_req) xs <= X_REGD;            // (the geom and audio cores' accesses go first)
    X_REGD: begin x_rdata <= r_rdata; x_ack <= 1; xs <= X_IDLE; end
    X_AUDIO: if (a_s_ack) begin x_rdata <= a_s_dat_r; x_ack <= 1; xs <= X_IDLE; end
    X_TCM:  xs <= X_TCMD;
    X_TCMD: begin x_rdata <= t_dat_r; x_ack <= 1; xs <= X_IDLE; end
    default: xs <= X_IDLE;
    endcase
end
assign xr_go = xs == X_REGS && !bus_r_req;

// ---------------------------------------------------------------- registers, bus
logic        br_we;
logic [15:0] br_addr, stray;
logic [31:0] br_wdata;
logic        su_valid, su_ready;
logic [31:0] su_data;
logic        cmd_valid, cmd_ready;
logic [31:0] cmd_data, mrdp_sync, mrdp_load;
logic [15:0] mrdp_unknown;
logic        mrdp_idle;
logic        r_uart_we, uart_rx_pop, uart_rxempty, uart_txfull;
logic [7:0]  r_uart_byte, uart_rx_byte;
logic [31:0] fb_base, dbg_geom_msg;
logic        fb_dma_enable, fb_vtg_enable, fb_mode, vblank;
mirlo_regs regs (.clk, .rst,
    .r_req(bus_r_req || xr_go), .r_we(bus_r_req ? br_we : x_we), .r_addr(bus_r_req ? br_addr : x_addr[15:0]),
    .r_wdata(bus_r_req ? br_wdata : x_wdata), .r_rdata,
    .aud_out(apf_audio_bus_out), .aud_wr(apf_audio_bus_wr), .aud_playback_en(apf_audio_playback_en),
    .aud_flush(apf_audio_flush), .aud_fill(apf_audio_buffer_fill),
    .br_request_read(apf_bridge_request_read), .br_request_write(apf_bridge_request_write),
    .br_request_getfile(apf_bridge_request_getfile), .br_request_openfile(apf_bridge_request_openfile),
    .br_slot_id(apf_bridge_slot_id), .br_data_offset(apf_bridge_data_offset), .br_length(apf_bridge_length),
    .br_ram_data_address(apf_bridge_ram_data_address), .br_file_size_wr(apf_bridge_file_size_wr),
    .br_new_file_size(apf_bridge_new_file_size_data), .br_file_size(apf_bridge_file_size),
    .br_complete_trigger(apf_bridge_complete_trigger), .br_current_address(apf_bridge_current_address),
    .br_command_result_code(apf_bridge_command_result_code),
    .br_host_reset_n(apf_bridge_host_reset_n), .br_host_loaded(apf_bridge_host_loaded),
    .cont_key('{apf_input_cont4_key, apf_input_cont3_key, apf_input_cont2_key, apf_input_cont1_key}),
    .cont_joy('{apf_input_cont4_joy, apf_input_cont3_joy, apf_input_cont2_joy, apf_input_cont1_joy}),
    .cont_trig('{apf_input_cont4_trig, apf_input_cont3_trig, apf_input_cont2_trig, apf_input_cont1_trig}),
    .ia_address(apf_interact_address), .ia_data(apf_interact_data), .ia_wr(apf_interact_wr), .ia_q(apf_interact_q),
    .vblank, .fb_base, .fb_dma_enable, .fb_vtg_enable, .fb_mode,
    .bus_errors, .geom_irq, .game_irq, .dbg_geom_msg,
    .su_valid, .su_ready, .su_data, .cmd_valid, .cmd_ready, .cmd_data,
    .mrdp_sync_count(mrdp_sync), .mrdp_load_count(mrdp_load), .mrdp_unknown_ops(mrdp_unknown), .mrdp_idle,
    .uart_we(r_uart_we), .uart_byte(r_uart_byte), .uart_txfull, .uart_rx_pop, .uart_rx_byte, .uart_rxempty);

// ---------------------------------------------------------------- the JTAG UART
`ifdef MIRLO_NO_JTAG_UART
assign uart_txfull = 1'b0;
assign uart_rxempty = 1'b1;
assign uart_rx_byte = 8'd0;
// the JTAG pins still end in the hard JTAG block, idle: without an atom on
// them Quartus places altera_reserved_* as four more user I/O (228 > 224)
`ifndef VERILATOR
cyclonev_jtag jtag_idle (.tck(altera_reserved_tck), .tdi(altera_reserved_tdi), .tms(altera_reserved_tms),
    .tdo(altera_reserved_tdo), .tdouser(1'b0), .clkdruser(), .runidleuser(), .shiftuser(), .tckutap(),
    .tdiutap(), .tmsutap(), .updateuser(), .usr1user());
`endif
`else
n64_uart uart (.clk, .rst,
    .tck(altera_reserved_tck), .tdi(altera_reserved_tdi), .tms(altera_reserved_tms), .tdo(altera_reserved_tdo),
    .r_we(r_uart_we), .r_byte(r_uart_byte), .w_we(1'b0), .w_byte(8'd0), .txfull(uart_txfull),
    .rx_pop(uart_rx_pop), .rx_byte(uart_rx_byte), .rxempty(uart_rxempty), .dump_en(1'b0), .dbg('0));
`endif

// ---------------------------------------------------------------- geom + audio -> SDRAM, registers
logic        g_cyc, g_stb, g_we, g_ack, a_cyc, a_stb, a_we, a_ack;
logic [29:0] g_adr, a_adr;
logic [31:0] g_dw, g_dr, a_dw, a_dr;
logic [3:0]  g_sel, a_sel;
logic [2:0]  g_cti;
n64_bus bus (.clk, .rst,
    .m0_cyc(g_cyc), .m0_stb(g_stb), .m0_we(g_we), .m0_adr(g_adr), .m0_dat_w(g_dw), .m0_sel(g_sel), .m0_cti(g_cti),
    .m0_ack(g_ack), .m0_dat_r(g_dr),
    .m1_cyc(a_cyc), .m1_stb(a_stb), .m1_we(a_we), .m1_adr(a_adr), .m1_dat_w(a_dw), .m1_sel(a_sel),
    .m1_ack(a_ack), .m1_dat_r(a_dr),
    .s_valid(p_valid[3]), .s_we(p_we[3]), .s_addr(p_addr[3]), .s_wdata(p_wdata[3]), .s_be(p_be[3]),
    .s_ready(p_ready[3]), .s_rvalid(p_rvalid[3]), .s_rdata(p_rdata),
    .r_req(bus_r_req), .r_we(br_we), .r_addr(br_addr), .r_wdata(br_wdata), .r_rdata, .stray);
assign p_urgent[3] = 0;

// ---------------------------------------------------------------- the geom core
logic [31:0] dbg_geom_pc;
n64_geom #(.ROM_A(GEOM_ROM_A), .ROM_B(GEOM_ROM_B)) geom (.clk, .rst,
    .o_cyc(g_cyc), .o_stb(g_stb), .o_we(g_we), .o_adr(g_adr), .o_dat_w(g_dw), .o_sel(g_sel), .o_cti(g_cti),
    .o_ack(g_ack), .o_dat_r(g_dr), .su_valid, .su_ready, .su_data,
    .t_adr(x_addr[13:2]), .t_we({4{xs == X_TCM && x_we}} & x_strb), .t_dat_w(x_wdata), .t_dat_r,
    .irq(geom_irq), .dbg_pc(dbg_geom_pc));

// ---------------------------------------------------------------- the audio core
// Loaded by the game CPU through its window (lang/c/game/audio_load.c), then
// released (CTRL RUN).
logic [31:0] dbg_aud_state, dbg_aud_pc;
AudioCore #(.RUN_RESET(1'b0), .SOC_PORT(1'b1)) audio (.clk, .rst,
    .s_adr(x_addr[31:2]), .s_dat_w(x_wdata), .s_dat_r(a_s_dat_r), .s_sel(x_strb),
    .s_cyc(xs == X_AUDIO), .s_stb(xs == X_AUDIO), .s_we(x_we), .s_ack(a_s_ack),
    .m_adr(a_adr), .m_dat_w(a_dw), .m_dat_r(a_dr), .m_sel(a_sel), .m_cyc(a_cyc), .m_stb(a_stb), .m_we(a_we),
    .m_ack(a_ack), .m_err(1'b0), .running(), .dbg_state(dbg_aud_state), .dbg_pc(dbg_aud_pc)
);

// ---------------------------------------------------------------- MRDP
logic        m_cmd_valid, m_cmd_ready, m_cmd_we, m_wdata_valid, m_wdata_ready, m_rdata_valid;
logic [31:0] m_cmd_addr, m_wdata, m_rdata;
logic [3:0]  m_wdata_we;
mrdp_top mrdp (.clk, .rst,
    .cmd_valid, .cmd_ready, .cmd_data,
    .m_cmd_valid, .m_cmd_ready, .m_cmd_we, .m_cmd_addr,
    .m_wdata_valid, .m_wdata_ready, .m_wdata, .m_wdata_we,
    .m_rdata_valid, .m_rdata,
    .sync_count(mrdp_sync), .load_count(mrdp_load), .unknown_ops(mrdp_unknown), .idle(mrdp_idle));
port_mrdp pm (.clk, .rst, .m_cmd_valid, .m_cmd_we, .m_cmd_addr, .m_cmd_ready, .m_wdata_valid, .m_wdata, .m_wdata_we,
    .m_wdata_ready, .m_rdata_valid, .m_rdata,
    .p_valid(p_valid[2]), .p_we(p_we[2]), .p_addr(p_addr[2]), .p_wdata(p_wdata[2]), .p_be(p_be[2]),
    .p_ready(p_ready[2]), .p_rvalid(p_rvalid[2]), .p_rdata);
assign p_urgent[2] = 0;

// ---------------------------------------------------------------- the Pocket's bridge
port_apf pa (.clk, .rst, .cyc(wishbone_master_cyc), .stb(wishbone_master_stb), .we(wishbone_master_we),
    .adr(wishbone_master_adr), .dat_w(wishbone_master_dat_w), .sel(wishbone_master_sel),
    .ack(wishbone_master_ack), .dat_r(wishbone_master_dat_r),
    .p_valid(p_valid[4]), .p_we(p_we[4]), .p_addr(p_addr[4]), .p_wdata(p_wdata[4]), .p_be(p_be[4]),
    .p_ready(p_ready[4]), .p_rvalid(p_rvalid[4]), .p_rdata);
assign wishbone_master_err = 1'b0;
assign p_urgent[4] = 0;

// ---------------------------------------------------------------- the scan-out
n64_video #(.ORIGIN_MASK(24'hFFFFFF)) video (.clk, .rst, .clk_vid, .rst_vid,
    .enable(fb_dma_enable && fb_vtg_enable), .vi_origin(fb_base[25:0]), .mode(fb_mode), .vblank,
    .vi_width(12'd0), .vi_black(1'b0), .vi_lines(9'd240), .osd_en(1'b0), .fps_en(1'b0), .osd_val('0),
    .m_valid(p_valid[0]), .m_addr(p_addr[0]), .m_urgent(p_urgent[0]), .m_ready(p_ready[0]),
    .m_rvalid(p_rvalid[0]), .m_rdata(p_rdata),
    .vi_line(), .vi_frame(),
    .vga_r, .vga_g, .vga_b, .vga_hsync, .vga_vsync, .vga_de);
assign p_we[0] = 0; assign p_wdata[0] = 0; assign p_be[0] = 4'hF;

endmodule
`default_nettype wire
