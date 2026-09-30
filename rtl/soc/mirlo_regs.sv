// Mirlo's registers on the MIPS SoC (tools/mirlo_regs.py: the map, and the
// firmware's csr.h), in place of LiteX's CSR peripherals (litex/csr.py,
// litex/mailbox.py, litex/mrdp.py, LiteX's timer/UART/video DMA) and with
// their behaviour:
//   APF_AUDIO     the Pocket's audio FIFO (core_top's): samples, playback, flush
//   APF_BRIDGE    the Pocket's target commands (core_top's core_bridge_cmd);
//                 STATUS is set by a command's completion, cleared by its read
//   APF_INPUT     the controllers
//   APF_INTERACT  interact.json: two words the Pocket writes (whole, or a
//                 field at a time at addresses 2-15); INTERACT_CHANGEDn is set
//                 by a whole-word write and cleared by the CPU reading INTERACTn
//   APF_VIDEO     vblank: its level, a flag set at its start (cleared by the
//                 read) and a frame counter
//   MAILBOX       game <-> geom message words and doorbells
//   MRDP          its command FIFO, fed by these registers or by the geom
//                 CFU's PUSH port (a register write wins a same-cycle clash),
//                 and its counters
//   TIMER0        a free-running 64-bit cycle counter, latched by a write
//   UART          the JTAG UART
//   VIDEO_FRAMEBUFFER  the scan-out's base (latched by it at the frame
//                 boundary), its enables and its mode
// One access port: an access in the cycle of r_req, its read data the next.
`default_nettype none

module mirlo_regs (
    input  wire         clk,
    input  wire         rst,
    input  wire         r_req,
    input  wire         r_we,
    input  wire  [15:0] r_addr,
    input  wire  [31:0] r_wdata,
    output logic [31:0] r_rdata,
    // APF audio
    output logic [31:0] aud_out,
    output logic        aud_wr,
    output logic        aud_playback_en,
    output logic        aud_flush,
    input  wire  [11:0] aud_fill,
    // APF bridge
    output logic        br_request_read, br_request_write, br_request_getfile, br_request_openfile,
    output logic [15:0] br_slot_id,
    output logic [31:0] br_data_offset, br_length, br_ram_data_address,
    output logic        br_file_size_wr,
    output logic [31:0] br_new_file_size,
    input  wire  [31:0] br_file_size,
    input  wire         br_complete_trigger,
    input  wire  [31:0] br_current_address,
    input  wire  [2:0]  br_command_result_code,
    input  wire         br_host_reset_n, br_host_loaded,
    // APF input
    input  wire  [3:0][31:0] cont_key, cont_joy, cont_trig,
    // APF interact
    input  wire  [3:0]  ia_address,
    input  wire  [31:0] ia_data,
    input  wire         ia_wr,
    output logic [31:0] ia_q,
    // video
    input  wire         vblank,             // sys: the scan-out is in vblank
    output logic [31:0] fb_base,
    output logic        fb_dma_enable, fb_vtg_enable, fb_mode,
    // CTRL
    input  wire  [15:0] bus_errors,
    // the mailbox
    output logic        geom_irq, game_irq,
    output logic [31:0] dbg_geom_msg,
    // MRDP's command stream and counters
    input  wire         su_valid,           // the geom CFU's PUSH
    output logic        su_ready,
    input  wire  [31:0] su_data,
    output logic        cmd_valid,
    input  wire         cmd_ready,
    output logic [31:0] cmd_data,
    input  wire  [31:0] mrdp_sync_count,
    input  wire  [31:0] mrdp_load_count,
    input  wire  [15:0] mrdp_unknown_ops,
    input  wire         mrdp_idle,
    // the JTAG UART
    output logic        uart_we,
    output logic [7:0]  uart_byte,
    input  wire         uart_txfull,
    output logic        uart_rx_pop,
    input  wire  [7:0]  uart_rx_byte,
    input  wire         uart_rxempty
);
`include "mirlo_regs.svh"

wire wr = r_req && r_we;
wire rd = r_req && !r_we;
wire [15:0] a = {r_addr[15:2], 2'b00};

// ---- APF audio
always_ff @(posedge clk) begin
    aud_wr <= wr && a == R_APF_AUDIO_OUT;
    aud_out <= r_wdata;
    aud_flush <= wr && a == R_APF_AUDIO_BUFFER_FLUSH && r_wdata[0];      // a register: core_top's FIFO clear
    if (rst) aud_playback_en <= 0;
    else if (wr && a == R_APF_AUDIO_PLAYBACK_EN) aud_playback_en <= r_wdata[0];
end

// ---- APF bridge
logic br_status, br_ct_d, br_boot_ready;
always_comb begin
    br_request_read     = wr && a == R_APF_BRIDGE_REQUEST_READ;
    br_request_write    = wr && a == R_APF_BRIDGE_REQUEST_WRITE;
    br_request_getfile  = wr && a == R_APF_BRIDGE_REQUEST_GETFILE;
    br_request_openfile = wr && a == R_APF_BRIDGE_REQUEST_OPENFILE;
    br_file_size_wr     = wr && a == R_APF_BRIDGE_FILE_SIZE;
    br_new_file_size    = r_wdata;
end
always_ff @(posedge clk) begin
    br_ct_d <= br_complete_trigger;
    if (rst) begin
        br_slot_id <= 0; br_data_offset <= 0; br_length <= 0; br_ram_data_address <= 0; br_status <= 0; br_boot_ready <= 0;
    end else begin
        if (wr && a == R_APF_BRIDGE_SLOT_ID) br_slot_id <= r_wdata[15:0];
        if (wr && a == R_APF_BRIDGE_DATA_OFFSET) br_data_offset <= r_wdata;
        if (wr && a == R_APF_BRIDGE_TRANSFER_LENGTH) br_length <= r_wdata;
        if (wr && a == R_APF_BRIDGE_RAM_DATA_ADDRESS) br_ram_data_address <= r_wdata;
        if (wr && a == R_APF_BRIDGE_BOOT_READY) br_boot_ready <= r_wdata[0];
        if (rd && a == R_APF_BRIDGE_STATUS) br_status <= 0;
        if (br_complete_trigger && !br_ct_d) br_status <= 1;
    end
end

// ---- APF interact (slot 0: A, B, Z, Start, L, R at 5 bits each; slot 1:
// C-up/down/left/right at 5 bits, stick [21:20], D-pad [23:22], Show FPS
// [24], Start = Select+Start [25]) -- as analogue_pocket.py's fields
logic [31:0] ia0, ia1;
logic        ia_ch0, ia_ch1;
always_ff @(posedge clk) begin
    if (rst) begin ia0 <= 0; ia1 <= 0; ia_ch0 <= 0; ia_ch1 <= 0; end
    else begin
        if (wr && a == R_APF_INTERACT_INTERACT0) ia0 <= r_wdata;
        if (wr && a == R_APF_INTERACT_INTERACT1) ia1 <= r_wdata;
        if (rd && a == R_APF_INTERACT_INTERACT0) ia_ch0 <= 0;
        if (rd && a == R_APF_INTERACT_INTERACT1) ia_ch1 <= 0;
        if (ia_wr) case (ia_address)
            4'd0:  begin ia0 <= ia_data; ia_ch0 <= 1; end
            4'd1:  begin ia1 <= ia_data; ia_ch1 <= 1; end
            4'd2:  ia0[4:0]   <= ia_data[4:0];
            4'd3:  ia0[9:5]   <= ia_data[4:0];
            4'd4:  ia0[14:10] <= ia_data[4:0];
            4'd5:  ia0[19:15] <= ia_data[4:0];
            4'd6:  ia0[24:20] <= ia_data[4:0];
            4'd7:  ia0[29:25] <= ia_data[4:0];
            4'd8:  ia1[4:0]   <= ia_data[4:0];
            4'd9:  ia1[9:5]   <= ia_data[4:0];
            4'd10: ia1[14:10] <= ia_data[4:0];
            4'd11: ia1[19:15] <= ia_data[4:0];
            4'd12: ia1[21:20] <= ia_data[1:0];
            4'd13: ia1[23:22] <= ia_data[1:0];
            4'd14: ia1[24]    <= ia_data[0];
            default: ia1[25]  <= ia_data[0];
        endcase
    end
    case (ia_address)
    4'd0:  ia_q <= ia0;
    4'd1:  ia_q <= ia1;
    4'd2:  ia_q <= {27'd0, ia0[4:0]};
    4'd3:  ia_q <= {27'd0, ia0[9:5]};
    4'd4:  ia_q <= {27'd0, ia0[14:10]};
    4'd5:  ia_q <= {27'd0, ia0[19:15]};
    4'd6:  ia_q <= {27'd0, ia0[24:20]};
    4'd7:  ia_q <= {27'd0, ia0[29:25]};
    4'd8:  ia_q <= {27'd0, ia1[4:0]};
    4'd9:  ia_q <= {27'd0, ia1[9:5]};
    4'd10: ia_q <= {27'd0, ia1[14:10]};
    4'd11: ia_q <= {27'd0, ia1[19:15]};
    4'd12: ia_q <= {30'd0, ia1[21:20]};
    4'd13: ia_q <= {30'd0, ia1[23:22]};
    4'd14: ia_q <= {31'd0, ia1[24]};
    default: ia_q <= {31'd0, ia1[25]};
    endcase
end

// ---- APF video
logic        vb_d, vb_trig;
logic [29:0] vb_count;
always_ff @(posedge clk) begin
    vb_d <= vblank;
    if (rst) begin vb_trig <= 0; vb_count <= 0; end
    else begin
        if (rd && a == R_APF_VIDEO_VIDEO) vb_trig <= 0;
        if (vblank && !vb_d) begin vb_trig <= 1; vb_count <= vb_count + 30'd1; end
    end
end

// ---- the scan-out's registers
logic [31:0] fb_length;
always_ff @(posedge clk)
    if (rst) begin fb_base <= 32'h40C0_0000; fb_length <= 0; fb_dma_enable <= 0; fb_vtg_enable <= 0; fb_mode <= 0; end
    else if (wr) case (a)
        R_VIDEO_FRAMEBUFFER_DMA_BASE:   fb_base <= r_wdata;
        R_VIDEO_FRAMEBUFFER_DMA_LENGTH: fb_length <= r_wdata;
        R_VIDEO_FRAMEBUFFER_DMA_ENABLE: fb_dma_enable <= r_wdata[0];
        R_VIDEO_FRAMEBUFFER_VTG_ENABLE: fb_vtg_enable <= r_wdata[0];
        R_VIDEO_FRAMEBUFFER_VTG_MODE:   fb_mode <= r_wdata[0];
        default: ;
    endcase

// ---- CTRL
logic [31:0] scratch;
always_ff @(posedge clk) if (rst) scratch <= 32'h1234_5678; else if (wr && a == R_CTRL_SCRATCH) scratch <= r_wdata;

// ---- the mailbox
logic [31:0] game_msg, geom_msg;
always_ff @(posedge clk)
    if (rst) begin game_msg <= 0; geom_msg <= 0; geom_irq <= 0; game_irq <= 0; end
    else begin
        if (wr && a == R_MAILBOX_GAME_MSG) game_msg <= r_wdata;
        if (wr && a == R_MAILBOX_GEOM_MSG) geom_msg <= r_wdata;
        if (wr && a == R_MAILBOX_GAME_KICK) geom_irq <= 1; else if (wr && a == R_MAILBOX_GEOM_ACK) geom_irq <= 0;
        if (wr && a == R_MAILBOX_GEOM_KICK) game_irq <= 1; else if (wr && a == R_MAILBOX_GAME_ACK) game_irq <= 0;
    end
assign dbg_geom_msg = geom_msg;

// ---- TIMER0
logic [63:0] uptime, uptime_l;
always_ff @(posedge clk) begin
    if (rst) uptime <= 0; else uptime <= uptime + 64'd1;
    if (wr && a == R_TIMER0_UPTIME_LATCH) uptime_l <= uptime;
end

// ---- UART
assign uart_we = wr && a == R_UART_RXTX;
assign uart_byte = r_wdata[7:0];
assign uart_rx_pop = rd && a == R_UART_RXTX && !uart_rxempty;

// ---- MRDP's command FIFO
wire         csr_push = wr && a == R_MRDP_CMD_DATA;
logic        f_in_ready;
logic [11:0] f_level;
logic [15:0] dropped;
fifo_fwft #(.W(32), .AW(11)) cmdq (
    .clk, .rst, .in_valid(csr_push || su_valid), .in_ready(f_in_ready), .in_data(csr_push ? r_wdata : su_data),
    .out_valid(cmd_valid), .out_ready(cmd_ready), .out_data(cmd_data), .level(f_level)
);
assign su_ready = !csr_push && f_in_ready;
always_ff @(posedge clk)
    if (rst) dropped <= 0;
    else if (csr_push && !f_in_ready && dropped != 16'hFFFF) dropped <= dropped + 16'd1;

// ---- reads
always_ff @(posedge clk) begin
    case (a)
    R_APF_AUDIO_PLAYBACK_EN:            r_rdata <= {31'd0, aud_playback_en};
    R_APF_AUDIO_BUFFER_FILL:            r_rdata <= {20'd0, aud_fill};
    R_APF_BRIDGE_SLOT_ID:               r_rdata <= {16'd0, br_slot_id};
    R_APF_BRIDGE_DATA_OFFSET:           r_rdata <= br_data_offset;
    R_APF_BRIDGE_TRANSFER_LENGTH:       r_rdata <= br_length;
    R_APF_BRIDGE_RAM_DATA_ADDRESS:      r_rdata <= br_ram_data_address;
    R_APF_BRIDGE_FILE_SIZE:             r_rdata <= br_file_size;
    R_APF_BRIDGE_STATUS:                r_rdata <= {31'd0, br_status};
    R_APF_BRIDGE_CURRENT_ADDRESS:       r_rdata <= br_current_address;
    R_APF_BRIDGE_COMMAND_RESULT_CODE:   r_rdata <= {29'd0, br_command_result_code};
    R_APF_BRIDGE_BOOT_READY:            r_rdata <= {31'd0, br_boot_ready};
    R_APF_BRIDGE_HOST_STATE:            r_rdata <= {30'd0, br_host_loaded, br_host_reset_n};
    R_APF_INPUT_CONT1_KEY:              r_rdata <= cont_key[0];
    R_APF_INPUT_CONT2_KEY:              r_rdata <= cont_key[1];
    R_APF_INPUT_CONT3_KEY:              r_rdata <= cont_key[2];
    R_APF_INPUT_CONT4_KEY:              r_rdata <= cont_key[3];
    R_APF_INPUT_CONT1_JOY:              r_rdata <= cont_joy[0];
    R_APF_INPUT_CONT2_JOY:              r_rdata <= cont_joy[1];
    R_APF_INPUT_CONT3_JOY:              r_rdata <= cont_joy[2];
    R_APF_INPUT_CONT4_JOY:              r_rdata <= cont_joy[3];
    R_APF_INPUT_CONT1_TRIG:             r_rdata <= cont_trig[0];
    R_APF_INPUT_CONT2_TRIG:             r_rdata <= cont_trig[1];
    R_APF_INPUT_CONT3_TRIG:             r_rdata <= cont_trig[2];
    R_APF_INPUT_CONT4_TRIG:             r_rdata <= cont_trig[3];
    R_APF_INTERACT_INTERACT0:           r_rdata <= ia0;
    R_APF_INTERACT_INTERACT_CHANGED0:   r_rdata <= {31'd0, ia_ch0};
    R_APF_INTERACT_INTERACT1:           r_rdata <= ia1;
    R_APF_INTERACT_INTERACT_CHANGED1:   r_rdata <= {31'd0, ia_ch1};
    R_APF_VIDEO_VIDEO:                  r_rdata <= {vb_count, vb_trig, vb_d};
    R_CTRL_SCRATCH:                     r_rdata <= scratch;
    R_CTRL_BUS_ERRORS:                  r_rdata <= {16'd0, bus_errors};
    R_MAILBOX_GAME_MSG:                 r_rdata <= game_msg;
    R_MAILBOX_GEOM_MSG:                 r_rdata <= geom_msg;
    R_MAILBOX_STATUS:                   r_rdata <= {30'd0, game_irq, geom_irq};
    R_MRDP_CMD_STATUS:                  r_rdata <= {19'd0, !f_in_ready, f_level};
    R_MRDP_CMD_DROPPED:                 r_rdata <= {16'd0, dropped};
    R_MRDP_SYNC_COUNT:                  r_rdata <= mrdp_sync_count;
    R_MRDP_LOAD_COUNT:                  r_rdata <= mrdp_load_count;
    R_MRDP_STATUS:                      r_rdata <= {mrdp_unknown_ops, 15'd0, mrdp_idle && f_level == 0 && !cmd_valid};
    R_TIMER0_UPTIME_CYCLES:             r_rdata <= uptime_l[63:32];
    R_TIMER0_UPTIME_CYCLES + 16'd4:     r_rdata <= uptime_l[31:0];
    R_UART_RXTX:                        r_rdata <= {24'd0, uart_rx_byte};
    R_UART_TXFULL:                      r_rdata <= {31'd0, uart_txfull};
    R_UART_RXEMPTY:                     r_rdata <= {31'd0, uart_rxempty};
    R_VIDEO_FRAMEBUFFER_DMA_BASE:       r_rdata <= fb_base;
    R_VIDEO_FRAMEBUFFER_DMA_LENGTH:     r_rdata <= fb_length;
    R_VIDEO_FRAMEBUFFER_DMA_ENABLE:     r_rdata <= {31'd0, fb_dma_enable};
    R_VIDEO_FRAMEBUFFER_VTG_ENABLE:     r_rdata <= {31'd0, fb_vtg_enable};
    R_VIDEO_FRAMEBUFFER_VTG_MODE:       r_rdata <= {31'd0, fb_mode};
    default:                            r_rdata <= 32'd0;
    endcase
end

endmodule
`default_nettype wire
