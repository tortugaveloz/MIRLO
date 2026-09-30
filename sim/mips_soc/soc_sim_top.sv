// mirlo_mips for tb_soc.cpp: the SDRAM's DQ split for the chip model, the
// Pocket's side as inputs, a few internals out for the bench.
`default_nettype none
module soc_sim_top (
    input  wire         clk_sys, clk_sys2x, clk_vid, reset,
    input  wire         host_reset_n, host_loaded,
    input  wire  [31:0] file_size,
    input  wire         br_complete,
    output wire         br_req_read,
    output wire  [15:0] br_slot,
    output wire  [31:0] br_offset, br_length, br_addr,
    input  wire  [31:0] cont1_key,
    output wire  [12:0] sdram_a,
    output wire  [1:0]  sdram_ba, sdram_dm,
    output wire         sdram_ras_n, sdram_cas_n, sdram_we_n, sdram_cke,
    output wire  [15:0] dq_out,
    output wire         dq_oe,
    input  wire  [15:0] dq_in,
    output wire  [4:0]  vga_r, vga_b,
    output wire  [5:0]  vga_g,
    output wire         vga_de, vga_hsync, vga_vsync,
    output wire         boot_ready,
    output wire         uart_we,
    output wire  [7:0]  uart_byte,
    output wire  [31:0] cpu_pc, cpu_cause, cpu_epc, geom_pc, geom_msg, fb_base, mrdp_sync, audio_pc, audio_state,
    output wire         fb_mode, vblank, audio_wr,
    output wire  [31:0] audio_out,
    output wire         x_req, x_we, x_ack,
    output wire  [31:0] x_addr, x_rdata, x_wdata
);
wire [15:0] dq;
// the APF audio FIFO (core_top's): its fill, flushed, drained at 48 kHz while playing
logic [11:0] aud_fill = 0;
logic [10:0] aud_div = 0;
wire         aud_flush, aud_play;
always_ff @(posedge clk_sys) begin
    aud_div <= aud_div == 11'd1308 ? 11'd0 : aud_div + 11'd1;          // 62.832 MHz / 48 kHz
    if (aud_flush) aud_fill <= 0;
    else aud_fill <= aud_fill + {11'd0, audio_wr && aud_fill != 12'hFFF} - {11'd0, aud_play && aud_div == 0 && aud_fill != 0};
end
mirlo_mips #(.SDR_T_INIT(100)) soc (
    .altera_reserved_tck(1'b0), .altera_reserved_tdi(1'b0), .altera_reserved_tdo(), .altera_reserved_tms(1'b0),
    .apf_audio_buffer_fill(aud_fill), .apf_audio_bus_out(audio_out), .apf_audio_bus_wr(audio_wr), .apf_audio_flush(aud_flush),
    .apf_audio_playback_en(aud_play),
    .apf_bridge_boot_ready(boot_ready), .apf_bridge_command_result_code(3'd0), .apf_bridge_complete_trigger(br_complete),
    .apf_bridge_current_address(32'd0), .apf_bridge_data_offset(br_offset), .apf_bridge_file_size(file_size),
    .apf_bridge_file_size_wr(), .apf_bridge_host_loaded(host_loaded), .apf_bridge_host_reset_n(host_reset_n),
    .apf_bridge_length(br_length), .apf_bridge_new_file_size_data(), .apf_bridge_ram_data_address(br_addr),
    .apf_bridge_request_getfile(), .apf_bridge_request_openfile(), .apf_bridge_request_read(br_req_read), .apf_bridge_request_write(),
    .apf_bridge_slot_id(br_slot), .apf_id_chip_id(64'd0),
    .apf_input_cont1_joy(32'd0), .apf_input_cont1_key(cont1_key), .apf_input_cont1_trig(32'd0),
    .apf_input_cont2_joy(32'd0), .apf_input_cont2_key(32'd0), .apf_input_cont2_trig(32'd0),
    .apf_input_cont3_joy(32'd0), .apf_input_cont3_key(32'd0), .apf_input_cont3_trig(32'd0),
    .apf_input_cont4_joy(32'd0), .apf_input_cont4_key(32'd0), .apf_input_cont4_trig(32'd0),
    .apf_interact_address(4'd0), .apf_interact_data(32'd0), .apf_interact_q(), .apf_interact_wr(1'b0),
    .apf_rtc_date_bcd(32'd0), .apf_rtc_time_bcd(32'd0), .apf_rtc_unix_seconds(32'd0),
    .clk_sys, .clk_sys2x, .clk_sys2x_90deg(clk_sys2x), .clk_vid, .reset,
    .sdram_a, .sdram_ba, .sdram_cas_n, .sdram_cke, .sdram_clock(), .sdram_dm, .sdram_dq(dq), .sdram_ras_n, .sdram_we_n,
    .serial_rx(1'b1), .serial_tx(), .use_jtag(1'b0),
    .vga_b, .vga_de, .vga_g, .vga_hsync, .vga_r, .vga_vsync,
    .wishbone_ack(1'b0), .wishbone_adr(), .wishbone_bte(), .wishbone_cti(), .wishbone_cyc(), .wishbone_dat_r(32'd0),
    .wishbone_dat_w(), .wishbone_err(1'b0),
    .wishbone_master_ack(), .wishbone_master_adr(30'd0), .wishbone_master_bte(2'd0), .wishbone_master_cti(3'd0),
    .wishbone_master_cyc(1'b0), .wishbone_master_dat_r(), .wishbone_master_dat_w(32'd0), .wishbone_master_err(),
    .wishbone_master_sel(4'd0), .wishbone_master_stb(1'b0), .wishbone_master_we(1'b0),
    .wishbone_sel(), .wishbone_stb(), .wishbone_we()
);
assign dq_oe  = soc.phy.oe_reg;
assign dq_out = soc.phy.out_reg;
assign dq = dq_oe ? 16'bz : dq_in;
assign uart_we   = soc.r_uart_we;
assign uart_byte = soc.r_uart_byte;
assign cpu_pc    = soc.sys.cpu.f_pc;
assign cpu_cause = soc.dbg_cause;
assign cpu_epc   = soc.dbg_epc;
assign geom_pc   = soc.dbg_geom_pc;
assign geom_msg  = soc.dbg_geom_msg;
assign fb_base   = soc.fb_base;
assign fb_mode   = soc.fb_mode;
assign vblank    = soc.vblank;
assign mrdp_sync = soc.mrdp_sync;
assign audio_pc  = soc.dbg_aud_pc;
assign x_req = soc.x_req; assign x_we = soc.x_we; assign x_ack = soc.x_ack;
assign x_addr = soc.x_addr; assign x_rdata = soc.x_rdata; assign x_wdata = soc.x_wdata;
assign audio_state = soc.dbg_aud_state;
endmodule
`default_nettype wire
