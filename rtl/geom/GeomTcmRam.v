// The geometry core's RAM (16 KiB): port A is the core's tightly coupled
// data port (rtl/soc/n64_geom.sv), port B the game CPU's window on it
// (rtl/soc/mirlo_mips.sv). A true dual-port M10K array, 4K x 2 per block, so neither port
// needs a read mux in logic (a shared single port cost ~100 ALMs of muxes
// and a hold register, more than the full device had left).
//
// Port A's read lands the cycle after `a_en` and holds while `a_en` is low
// (addressstall_a): the core's memory stage may stall with a load in it.
// Quartus rejects two byte-enabled write processes on one inferred array, so
// this is an altsyncram instance. Not used in Verilator (sim/geom_full models
// the port in C++).
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
    altsyncram #(
        .operation_mode                     ("BIDIR_DUAL_PORT"),
        .width_a                            (32),
        .widthad_a                          (12),
        .numwords_a                         (4096),
        .width_b                            (32),
        .widthad_b                          (12),
        .numwords_b                         (4096),
        .width_byteena_a                    (4),
        .width_byteena_b                    (4),
        .byte_size                          (8),
        .outdata_reg_a                      ("UNREGISTERED"),
        .outdata_reg_b                      ("UNREGISTERED"),
        .address_reg_b                      ("CLOCK0"),
        .indata_reg_b                       ("CLOCK0"),
        .wrcontrol_wraddress_reg_b          ("CLOCK0"),
        .byteena_reg_b                      ("CLOCK0"),
        .outdata_aclr_a                     ("NONE"),
        .outdata_aclr_b                     ("NONE"),
        .clock_enable_input_a               ("BYPASS"),
        .clock_enable_input_b               ("BYPASS"),
        .clock_enable_output_a              ("BYPASS"),
        .clock_enable_output_b              ("BYPASS"),
        .read_during_write_mode_mixed_ports ("DONT_CARE"),
        .read_during_write_mode_port_a      ("NEW_DATA_NO_NBE_READ"),
        .read_during_write_mode_port_b      ("NEW_DATA_NO_NBE_READ"),
        .power_up_uninitialized             ("FALSE"),
        .ram_block_type                     ("M10K"),
        .intended_device_family             ("Cyclone V"),
        .lpm_type                           ("altsyncram")
    ) ram (
        .clock0         (clk),
        .address_a      (a_adr),
        .addressstall_a (~a_en),
        .data_a         (a_dat_w),
        .wren_a         (a_en & (|a_we)),
        .byteena_a      (a_we | {4{~(|a_we)}}),
        .q_a            (a_dat_r),
        .address_b      (b_adr),
        .data_b         (b_dat_w),
        .wren_b         (|b_we),
        .byteena_b      (b_we | {4{~(|b_we)}}),
        .q_b            (b_dat_r),
        .aclr0          (1'b0),
        .aclr1          (1'b0),
        .addressstall_b (1'b0),
        .clock1         (1'b1),
        .clocken0       (1'b1),
        .clocken1       (1'b1),
        .clocken2       (1'b1),
        .clocken3       (1'b1),
        .eccstatus      (),
        .rden_a         (1'b1),
        .rden_b         (1'b1)
    );
endmodule
