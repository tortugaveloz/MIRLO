// Mirlo's game CPU on the MIPS SoC: mips_core (rtl/mips/mips_core.sv) with
// MIRLO's memory map -- no TLB, little-endian, every address physical:
//   0x4000_0000-0x43FF_FFFF  SDRAM, cached (the s_* port: lines and words)
//   0x1FC0_0000-0x1FC0_0FFF  the boot ROM (KSEG1 0xBFC0_0000: the reset
//                            vector, and the exception vectors while BEV)
//   anything else            the device port (x_*): single words, uncached
//                            -- the registers at 0xF000_0000, the audio
//                            core at 0x8000_0000, the geom RAM at 0x2000_8000
// The exception vector with BEV 0 is 0x4000_0180 (the program's crt0).
`default_nettype none

module mips_sys #(
    parameter              BOOTROM = "",         // $readmemh file, 32-bit words
    parameter int          BOOT_WORDS = 1024
) (
    input  wire         clk,
    input  wire         rst,
    // SDRAM: byte addresses (0x4xxx_xxxx); line reads (8 words) and single words
    output logic        s_req,
    output logic        s_we,
    output logic [31:0] s_addr,
    output logic [31:0] s_wdata,
    output logic [3:0]  s_strb,
    output logic        s_line,
    input  wire         s_ack,
    input  wire         s_rvalid,
    input  wire  [31:0] s_rdata,
    // devices: single words, held until x_ack (reads: data with the ack)
    output logic        x_req,
    output logic        x_we,
    output logic [31:0] x_addr,
    output logic [31:0] x_wdata,
    output logic [3:0]  x_strb,
    input  wire         x_ack,
    input  wire  [31:0] x_rdata,
    input  wire         irq,                // Cause.IP2
    output logic [31:0] dbg_pc, dbg_cause, dbg_epc
);

logic        c_req, c_we, c_line, c_ack, c_rvalid;
logic [31:0] c_addr, c_wdata, c_rdata, dbg_status;
logic [3:0]  c_strb;

mips_core #(.TLB_EN(0), .FLAT(1), .BIG(0), .EXC_BASE(32'h4000_0000)) cpu (
    .clk, .rst, .reset_pc(32'hBFC0_0000),
    .mem_req(c_req), .mem_addr(c_addr), .mem_we(c_we), .mem_wdata(c_wdata), .mem_wstrb(c_strb), .mem_line(c_line),
    .mem_ack(c_ack), .mem_rvalid(c_rvalid), .mem_rdata(c_rdata),
    .irq_rcp(irq),
    .tr_retire(), .tr_pc(), .tr_insn(), .tr_wr(), .tr_rd(), .tr_val(), .tr_exc(), .tr_exc_int(), .tr_exc_pc(),
    .tr_exc_code(), .tr_fwe_even(), .tr_fwe_odd(), .tr_fwidx(), .tr_fweven(), .tr_fwodd(), .tr_fcr31(),
    .dbg_pc, .dbg_cause, .dbg_status, .dbg_epc
);

wire is_sd   = c_addr[31:26] == 6'b010000;
wire is_boot = c_addr[31:12] == 20'h1FC00;

// the boot ROM: a read a cycle later
logic [31:0] boot [BOOT_WORDS];
initial if (BOOTROM != "") $readmemh(BOOTROM, boot);
logic [31:0] boot_q; logic boot_ack;
always_ff @(posedge clk) begin
    boot_q <= boot[c_addr[$clog2(BOOT_WORDS)+1:2]];
    boot_ack <= c_req && is_boot && !boot_ack && !rst;
end

assign s_req = c_req && is_sd; assign s_we = c_we; assign s_addr = c_addr; assign s_wdata = c_wdata;
assign s_strb = c_strb; assign s_line = c_line;
assign x_req = c_req && !is_sd && !is_boot; assign x_we = c_we; assign x_addr = c_addr;
assign x_wdata = c_wdata; assign x_strb = c_strb;

always_comb begin
    if (is_boot) begin                                       // (writes: ignored, acknowledged)
        c_ack = boot_ack && c_we; c_rvalid = boot_ack && !c_we; c_rdata = boot_q;
    end else if (is_sd) begin
        c_ack = s_ack; c_rvalid = s_rvalid; c_rdata = s_rdata;
    end else begin
        c_ack = x_ack && c_we; c_rvalid = x_ack && !c_we; c_rdata = x_rdata;
    end
end

endmodule
`default_nettype wire
