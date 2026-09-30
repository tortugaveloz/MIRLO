// The JTAG UART's PHY: LiteX's AlteraJTAG + JTAGPHY (litex/soc/cores/jtag.py), generated
// by Migen as a standalone module, so the host side is the same as Mirlo's:
// openocd -f litex/openocd_rpc.cfg + litex/jtag_uart_relay.py. A byte stream each way.
/* Machine-generated using Migen */
module jtag_uart_bridge(
	input clk,
	input rst,
	input altera_reserved_tms,
	input altera_reserved_tck,
	input altera_reserved_tdi,
	output altera_reserved_tdo,
	output source_valid,
	output [7:0] source_data,
	input source_ready,
	input sink_valid,
	input [7:0] sink_data,
	output sink_ready
);

wire sys_clk;
wire sys_rst;
wire sink_valid1;
wire sink_ready1;
reg sink_first = 1'd0;
reg sink_last = 1'd0;
wire [7:0] sink_payload_data;
wire source_valid1;
wire source_ready1;
wire source_first;
wire source_last;
wire [7:0] source_payload_data;
reg jtag_reset = 1'd0;
reg jtag_capture = 1'd0;
wire jtag_shift;
wire jtag_update;
wire jtag_runtest;
wire jtag_drck;
wire jtag_sel;
wire jtag_tck;
wire jtag_tms;
wire jtag_tdi;
reg jtag_tdo;
wire jtag_altera_reserved_tck;
wire jtag_altera_reserved_tms;
wire jtag_altera_reserved_tdi;
wire jtag_altera_reserved_tdo;
reg jtag_tdouser = 1'd0;
wire jtag_tmsutap;
wire jtag_tckutap;
wire jtag_tdiutap;
wire jtag_inv_clk;
wire jtag_inv_rst;
reg jtag_ongoing0;
reg jtag_ongoing1;
reg jtag_ongoing2;
reg jtag_ongoing3;
reg jtag_ongoing4;
reg jtag_ongoing5;
reg jtag_ongoing6;
reg jtag_ongoing7;
reg jtag_ongoing8;
reg jtag_ongoing9;
reg jtag_ongoing10;
reg jtag_ongoing11;
reg jtag_ongoing12;
reg jtag_ongoing13;
reg jtag_ongoing14;
reg jtag_ongoing15;
wire jtag_clk;
wire jtag_rst;
wire tx_cdc_sink_sink_valid;
wire tx_cdc_sink_sink_ready;
wire tx_cdc_sink_sink_first;
wire tx_cdc_sink_sink_last;
wire [7:0] tx_cdc_sink_sink_payload_data;
wire tx_cdc_source_source_valid;
reg tx_cdc_source_source_ready;
wire tx_cdc_source_source_first;
wire tx_cdc_source_source_last;
wire [7:0] tx_cdc_source_source_payload_data;
wire tx_cdc_cd_rst;
wire from190_clk;
wire from190_rst;
wire to190_clk;
wire to190_rst;
wire tx_cdc_cdc_sink_valid;
wire tx_cdc_cdc_sink_ready;
wire tx_cdc_cdc_sink_first;
wire tx_cdc_cdc_sink_last;
wire [7:0] tx_cdc_cdc_sink_payload_data;
wire tx_cdc_cdc_source_valid;
wire tx_cdc_cdc_source_ready;
wire tx_cdc_cdc_source_first;
wire tx_cdc_cdc_source_last;
wire [7:0] tx_cdc_cdc_source_payload_data;
wire tx_cdc_cdc_asyncfifo_we;
wire tx_cdc_cdc_asyncfifo_writable;
wire tx_cdc_cdc_asyncfifo_re;
wire tx_cdc_cdc_asyncfifo_readable;
wire [9:0] tx_cdc_cdc_asyncfifo_din;
wire [9:0] tx_cdc_cdc_asyncfifo_dout;
wire tx_cdc_cdc_graycounter0_ce;
(* no_retiming = "true" *) reg [2:0] tx_cdc_cdc_graycounter0_q = 3'd0;
wire [2:0] tx_cdc_cdc_graycounter0_q_next;
reg [2:0] tx_cdc_cdc_graycounter0_q_binary = 3'd0;
reg [2:0] tx_cdc_cdc_graycounter0_q_next_binary;
wire tx_cdc_cdc_graycounter1_ce;
(* no_retiming = "true" *) reg [2:0] tx_cdc_cdc_graycounter1_q = 3'd0;
wire [2:0] tx_cdc_cdc_graycounter1_q_next;
reg [2:0] tx_cdc_cdc_graycounter1_q_binary = 3'd0;
reg [2:0] tx_cdc_cdc_graycounter1_q_next_binary;
wire [2:0] tx_cdc_cdc_produce_rdomain;
wire [2:0] tx_cdc_cdc_consume_wdomain;
wire [1:0] tx_cdc_cdc_wrport_adr;
wire [9:0] tx_cdc_cdc_wrport_dat_r;
wire tx_cdc_cdc_wrport_we;
wire [9:0] tx_cdc_cdc_wrport_dat_w;
wire [1:0] tx_cdc_cdc_rdport_adr;
wire [9:0] tx_cdc_cdc_rdport_dat_r;
wire [7:0] tx_cdc_cdc_fifo_in_payload_data;
wire tx_cdc_cdc_fifo_in_first;
wire tx_cdc_cdc_fifo_in_last;
wire [7:0] tx_cdc_cdc_fifo_out_payload_data;
wire tx_cdc_cdc_fifo_out_first;
wire tx_cdc_cdc_fifo_out_last;
reg rx_cdc_sink_sink_valid;
wire rx_cdc_sink_sink_ready;
reg rx_cdc_sink_sink_first = 1'd0;
reg rx_cdc_sink_sink_last = 1'd0;
reg [7:0] rx_cdc_sink_sink_payload_data;
wire rx_cdc_source_source_valid;
wire rx_cdc_source_source_ready;
wire rx_cdc_source_source_first;
wire rx_cdc_source_source_last;
wire [7:0] rx_cdc_source_source_payload_data;
wire rx_cdc_cd_rst;
wire from341_clk;
wire from341_rst;
wire to341_clk;
wire to341_rst;
wire rx_cdc_cdc_sink_valid;
wire rx_cdc_cdc_sink_ready;
wire rx_cdc_cdc_sink_first;
wire rx_cdc_cdc_sink_last;
wire [7:0] rx_cdc_cdc_sink_payload_data;
wire rx_cdc_cdc_source_valid;
wire rx_cdc_cdc_source_ready;
wire rx_cdc_cdc_source_first;
wire rx_cdc_cdc_source_last;
wire [7:0] rx_cdc_cdc_source_payload_data;
wire rx_cdc_cdc_asyncfifo_we;
wire rx_cdc_cdc_asyncfifo_writable;
wire rx_cdc_cdc_asyncfifo_re;
wire rx_cdc_cdc_asyncfifo_readable;
wire [9:0] rx_cdc_cdc_asyncfifo_din;
wire [9:0] rx_cdc_cdc_asyncfifo_dout;
wire rx_cdc_cdc_graycounter0_ce;
(* no_retiming = "true" *) reg [2:0] rx_cdc_cdc_graycounter0_q = 3'd0;
wire [2:0] rx_cdc_cdc_graycounter0_q_next;
reg [2:0] rx_cdc_cdc_graycounter0_q_binary = 3'd0;
reg [2:0] rx_cdc_cdc_graycounter0_q_next_binary;
wire rx_cdc_cdc_graycounter1_ce;
(* no_retiming = "true" *) reg [2:0] rx_cdc_cdc_graycounter1_q = 3'd0;
wire [2:0] rx_cdc_cdc_graycounter1_q_next;
reg [2:0] rx_cdc_cdc_graycounter1_q_binary = 3'd0;
reg [2:0] rx_cdc_cdc_graycounter1_q_next_binary;
wire [2:0] rx_cdc_cdc_produce_rdomain;
wire [2:0] rx_cdc_cdc_consume_wdomain;
wire [1:0] rx_cdc_cdc_wrport_adr;
wire [9:0] rx_cdc_cdc_wrport_dat_r;
wire rx_cdc_cdc_wrport_we;
wire [9:0] rx_cdc_cdc_wrport_dat_w;
wire [1:0] rx_cdc_cdc_rdport_adr;
wire [9:0] rx_cdc_cdc_rdport_dat_r;
wire [7:0] rx_cdc_cdc_fifo_in_payload_data;
wire rx_cdc_cdc_fifo_in_first;
wire rx_cdc_cdc_fifo_in_last;
wire [7:0] rx_cdc_cdc_fifo_out_payload_data;
wire rx_cdc_cdc_fifo_out_first;
wire rx_cdc_cdc_fifo_out_last;
reg valid = 1'd0;
reg ready = 1'd0;
reg [7:0] data = 8'd0;
reg [2:0] count = 3'd0;
wire fsm_reset;
reg [3:0] alterajtag_state = 4'd0;
reg [3:0] alterajtag_next_state;
reg [1:0] resetinserter_state = 2'd0;
reg [1:0] resetinserter_next_state;
reg valid_next_value0;
reg valid_next_value_ce0;
reg [7:0] data_next_value1;
reg data_next_value_ce1;
reg [2:0] count_next_value2;
reg count_next_value_ce2;
reg ready_next_value3;
reg ready_next_value_ce3;
wire ars_cd_jtag_rst_meta;
wire ars_cd_from190_rst_meta;
wire ars_cd_to190_rst_meta;
(* no_retiming = "true" *) reg [2:0] multiregimpl00 = 3'd0;
(* no_retiming = "true" *) reg [2:0] multiregimpl01 = 3'd0;
(* no_retiming = "true" *) reg [2:0] multiregimpl10 = 3'd0;
(* no_retiming = "true" *) reg [2:0] multiregimpl11 = 3'd0;
wire ars_cd_from341_rst_meta;
wire ars_cd_to341_rst_meta;
(* no_retiming = "true" *) reg [2:0] multiregimpl20 = 3'd0;
(* no_retiming = "true" *) reg [2:0] multiregimpl21 = 3'd0;
(* no_retiming = "true" *) reg [2:0] multiregimpl30 = 3'd0;
(* no_retiming = "true" *) reg [2:0] multiregimpl31 = 3'd0;

// synthesis translate_off
reg dummy_s;
initial dummy_s <= 1'd0;
// synthesis translate_on

assign sys_clk = clk;
assign sys_rst = rst;
assign source_valid = source_valid1;
assign source_data = source_payload_data;
assign source_ready1 = source_ready;
assign sink_valid1 = sink_valid;
assign sink_payload_data = sink_data;
assign sink_ready = sink_ready1;
assign jtag_clk = jtag_tck;
assign tx_cdc_sink_sink_valid = sink_valid1;
assign sink_ready1 = tx_cdc_sink_sink_ready;
assign tx_cdc_sink_sink_first = sink_first;
assign tx_cdc_sink_sink_last = sink_last;
assign tx_cdc_sink_sink_payload_data = sink_payload_data;
assign source_valid1 = rx_cdc_source_source_valid;
assign rx_cdc_source_source_ready = source_ready1;
assign source_first = rx_cdc_source_source_first;
assign source_last = rx_cdc_source_source_last;
assign source_payload_data = rx_cdc_source_source_payload_data;
assign fsm_reset = (jtag_reset | jtag_capture);
assign jtag_inv_clk = (~jtag_clk);
assign jtag_inv_rst = jtag_rst;
assign jtag_altera_reserved_tms = altera_reserved_tms;
assign jtag_altera_reserved_tck = altera_reserved_tck;
assign jtag_altera_reserved_tdi = altera_reserved_tdi;
assign altera_reserved_tdo = jtag_altera_reserved_tdo;
assign jtag_tck = jtag_tckutap;
assign jtag_tms = jtag_tmsutap;
assign jtag_tdi = jtag_tdiutap;

// synthesis translate_off
reg dummy_d;
// synthesis translate_on
always @(*) begin
	jtag_ongoing0 <= 1'd0;
	jtag_ongoing1 <= 1'd0;
	jtag_ongoing2 <= 1'd0;
	jtag_ongoing3 <= 1'd0;
	jtag_ongoing4 <= 1'd0;
	jtag_ongoing5 <= 1'd0;
	jtag_ongoing6 <= 1'd0;
	jtag_ongoing7 <= 1'd0;
	jtag_ongoing8 <= 1'd0;
	jtag_ongoing9 <= 1'd0;
	jtag_ongoing10 <= 1'd0;
	jtag_ongoing11 <= 1'd0;
	jtag_ongoing12 <= 1'd0;
	jtag_ongoing13 <= 1'd0;
	jtag_ongoing14 <= 1'd0;
	jtag_ongoing15 <= 1'd0;
	alterajtag_next_state <= 4'd0;
	alterajtag_next_state <= alterajtag_state;
	case (alterajtag_state)
		1'd1: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 1'd1;
				end
				1'd1: begin
					alterajtag_next_state <= 2'd2;
				end
			endcase
			jtag_ongoing1 <= 1'd1;
		end
		2'd2: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 2'd3;
				end
				1'd1: begin
					alterajtag_next_state <= 4'd9;
				end
			endcase
			jtag_ongoing2 <= 1'd1;
		end
		2'd3: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 3'd4;
				end
				1'd1: begin
					alterajtag_next_state <= 3'd5;
				end
			endcase
			jtag_ongoing3 <= 1'd1;
		end
		3'd4: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 3'd4;
				end
				1'd1: begin
					alterajtag_next_state <= 3'd5;
				end
			endcase
			jtag_ongoing4 <= 1'd1;
		end
		3'd5: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 3'd6;
				end
				1'd1: begin
					alterajtag_next_state <= 4'd8;
				end
			endcase
			jtag_ongoing5 <= 1'd1;
		end
		3'd6: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 3'd6;
				end
				1'd1: begin
					alterajtag_next_state <= 3'd7;
				end
			endcase
			jtag_ongoing6 <= 1'd1;
		end
		3'd7: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 3'd4;
				end
				1'd1: begin
					alterajtag_next_state <= 4'd8;
				end
			endcase
			jtag_ongoing7 <= 1'd1;
		end
		4'd8: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 1'd1;
				end
				1'd1: begin
					alterajtag_next_state <= 2'd2;
				end
			endcase
			jtag_ongoing8 <= 1'd1;
		end
		4'd9: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 4'd10;
				end
				1'd1: begin
					alterajtag_next_state <= 1'd0;
				end
			endcase
			jtag_ongoing9 <= 1'd1;
		end
		4'd10: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 4'd11;
				end
				1'd1: begin
					alterajtag_next_state <= 4'd12;
				end
			endcase
			jtag_ongoing10 <= 1'd1;
		end
		4'd11: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 4'd11;
				end
				1'd1: begin
					alterajtag_next_state <= 4'd12;
				end
			endcase
			jtag_ongoing11 <= 1'd1;
		end
		4'd12: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 4'd13;
				end
				1'd1: begin
					alterajtag_next_state <= 4'd15;
				end
			endcase
			jtag_ongoing12 <= 1'd1;
		end
		4'd13: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 4'd13;
				end
				1'd1: begin
					alterajtag_next_state <= 4'd14;
				end
			endcase
			jtag_ongoing13 <= 1'd1;
		end
		4'd14: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 4'd11;
				end
				1'd1: begin
					alterajtag_next_state <= 4'd15;
				end
			endcase
			jtag_ongoing14 <= 1'd1;
		end
		4'd15: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 1'd1;
				end
				1'd1: begin
					alterajtag_next_state <= 2'd2;
				end
			endcase
			jtag_ongoing15 <= 1'd1;
		end
		default: begin
			case (jtag_tms)
				1'd0: begin
					alterajtag_next_state <= 1'd1;
				end
				1'd1: begin
					alterajtag_next_state <= 1'd0;
				end
			endcase
			jtag_ongoing0 <= 1'd1;
		end
	endcase
// synthesis translate_off
	dummy_d <= dummy_s;
// synthesis translate_on
end
assign from190_clk = sys_clk;
assign to190_clk = jtag_clk;
assign tx_cdc_cd_rst = (sys_rst | jtag_rst);
assign tx_cdc_cdc_sink_valid = tx_cdc_sink_sink_valid;
assign tx_cdc_sink_sink_ready = tx_cdc_cdc_sink_ready;
assign tx_cdc_cdc_sink_first = tx_cdc_sink_sink_first;
assign tx_cdc_cdc_sink_last = tx_cdc_sink_sink_last;
assign tx_cdc_cdc_sink_payload_data = tx_cdc_sink_sink_payload_data;
assign tx_cdc_source_source_valid = tx_cdc_cdc_source_valid;
assign tx_cdc_cdc_source_ready = tx_cdc_source_source_ready;
assign tx_cdc_source_source_first = tx_cdc_cdc_source_first;
assign tx_cdc_source_source_last = tx_cdc_cdc_source_last;
assign tx_cdc_source_source_payload_data = tx_cdc_cdc_source_payload_data;
assign tx_cdc_cdc_asyncfifo_din = {tx_cdc_cdc_fifo_in_last, tx_cdc_cdc_fifo_in_first, tx_cdc_cdc_fifo_in_payload_data};
assign {tx_cdc_cdc_fifo_out_last, tx_cdc_cdc_fifo_out_first, tx_cdc_cdc_fifo_out_payload_data} = tx_cdc_cdc_asyncfifo_dout;
assign tx_cdc_cdc_sink_ready = tx_cdc_cdc_asyncfifo_writable;
assign tx_cdc_cdc_asyncfifo_we = tx_cdc_cdc_sink_valid;
assign tx_cdc_cdc_fifo_in_first = tx_cdc_cdc_sink_first;
assign tx_cdc_cdc_fifo_in_last = tx_cdc_cdc_sink_last;
assign tx_cdc_cdc_fifo_in_payload_data = tx_cdc_cdc_sink_payload_data;
assign tx_cdc_cdc_source_valid = tx_cdc_cdc_asyncfifo_readable;
assign tx_cdc_cdc_source_first = tx_cdc_cdc_fifo_out_first;
assign tx_cdc_cdc_source_last = tx_cdc_cdc_fifo_out_last;
assign tx_cdc_cdc_source_payload_data = tx_cdc_cdc_fifo_out_payload_data;
assign tx_cdc_cdc_asyncfifo_re = tx_cdc_cdc_source_ready;
assign tx_cdc_cdc_graycounter0_ce = (tx_cdc_cdc_asyncfifo_writable & tx_cdc_cdc_asyncfifo_we);
assign tx_cdc_cdc_graycounter1_ce = (tx_cdc_cdc_asyncfifo_readable & tx_cdc_cdc_asyncfifo_re);
assign tx_cdc_cdc_asyncfifo_writable = (((tx_cdc_cdc_graycounter0_q[2] == tx_cdc_cdc_consume_wdomain[2]) | (tx_cdc_cdc_graycounter0_q[1] == tx_cdc_cdc_consume_wdomain[1])) | (tx_cdc_cdc_graycounter0_q[0] != tx_cdc_cdc_consume_wdomain[0]));
assign tx_cdc_cdc_asyncfifo_readable = (tx_cdc_cdc_graycounter1_q != tx_cdc_cdc_produce_rdomain);
assign tx_cdc_cdc_wrport_adr = tx_cdc_cdc_graycounter0_q_binary[1:0];
assign tx_cdc_cdc_wrport_dat_w = tx_cdc_cdc_asyncfifo_din;
assign tx_cdc_cdc_wrport_we = tx_cdc_cdc_graycounter0_ce;
assign tx_cdc_cdc_rdport_adr = tx_cdc_cdc_graycounter1_q_next_binary[1:0];
assign tx_cdc_cdc_asyncfifo_dout = tx_cdc_cdc_rdport_dat_r;

// synthesis translate_off
reg dummy_d_1;
// synthesis translate_on
always @(*) begin
	tx_cdc_cdc_graycounter0_q_next_binary <= 3'd0;
	if (tx_cdc_cdc_graycounter0_ce) begin
		tx_cdc_cdc_graycounter0_q_next_binary <= (tx_cdc_cdc_graycounter0_q_binary + 1'd1);
	end else begin
		tx_cdc_cdc_graycounter0_q_next_binary <= tx_cdc_cdc_graycounter0_q_binary;
	end
// synthesis translate_off
	dummy_d_1 <= dummy_s;
// synthesis translate_on
end
assign tx_cdc_cdc_graycounter0_q_next = (tx_cdc_cdc_graycounter0_q_next_binary ^ tx_cdc_cdc_graycounter0_q_next_binary[2:1]);

// synthesis translate_off
reg dummy_d_2;
// synthesis translate_on
always @(*) begin
	tx_cdc_cdc_graycounter1_q_next_binary <= 3'd0;
	if (tx_cdc_cdc_graycounter1_ce) begin
		tx_cdc_cdc_graycounter1_q_next_binary <= (tx_cdc_cdc_graycounter1_q_binary + 1'd1);
	end else begin
		tx_cdc_cdc_graycounter1_q_next_binary <= tx_cdc_cdc_graycounter1_q_binary;
	end
// synthesis translate_off
	dummy_d_2 <= dummy_s;
// synthesis translate_on
end
assign tx_cdc_cdc_graycounter1_q_next = (tx_cdc_cdc_graycounter1_q_next_binary ^ tx_cdc_cdc_graycounter1_q_next_binary[2:1]);
assign from341_clk = jtag_clk;
assign to341_clk = sys_clk;
assign rx_cdc_cd_rst = (jtag_rst | sys_rst);
assign rx_cdc_cdc_sink_valid = rx_cdc_sink_sink_valid;
assign rx_cdc_sink_sink_ready = rx_cdc_cdc_sink_ready;
assign rx_cdc_cdc_sink_first = rx_cdc_sink_sink_first;
assign rx_cdc_cdc_sink_last = rx_cdc_sink_sink_last;
assign rx_cdc_cdc_sink_payload_data = rx_cdc_sink_sink_payload_data;
assign rx_cdc_source_source_valid = rx_cdc_cdc_source_valid;
assign rx_cdc_cdc_source_ready = rx_cdc_source_source_ready;
assign rx_cdc_source_source_first = rx_cdc_cdc_source_first;
assign rx_cdc_source_source_last = rx_cdc_cdc_source_last;
assign rx_cdc_source_source_payload_data = rx_cdc_cdc_source_payload_data;
assign rx_cdc_cdc_asyncfifo_din = {rx_cdc_cdc_fifo_in_last, rx_cdc_cdc_fifo_in_first, rx_cdc_cdc_fifo_in_payload_data};
assign {rx_cdc_cdc_fifo_out_last, rx_cdc_cdc_fifo_out_first, rx_cdc_cdc_fifo_out_payload_data} = rx_cdc_cdc_asyncfifo_dout;
assign rx_cdc_cdc_sink_ready = rx_cdc_cdc_asyncfifo_writable;
assign rx_cdc_cdc_asyncfifo_we = rx_cdc_cdc_sink_valid;
assign rx_cdc_cdc_fifo_in_first = rx_cdc_cdc_sink_first;
assign rx_cdc_cdc_fifo_in_last = rx_cdc_cdc_sink_last;
assign rx_cdc_cdc_fifo_in_payload_data = rx_cdc_cdc_sink_payload_data;
assign rx_cdc_cdc_source_valid = rx_cdc_cdc_asyncfifo_readable;
assign rx_cdc_cdc_source_first = rx_cdc_cdc_fifo_out_first;
assign rx_cdc_cdc_source_last = rx_cdc_cdc_fifo_out_last;
assign rx_cdc_cdc_source_payload_data = rx_cdc_cdc_fifo_out_payload_data;
assign rx_cdc_cdc_asyncfifo_re = rx_cdc_cdc_source_ready;
assign rx_cdc_cdc_graycounter0_ce = (rx_cdc_cdc_asyncfifo_writable & rx_cdc_cdc_asyncfifo_we);
assign rx_cdc_cdc_graycounter1_ce = (rx_cdc_cdc_asyncfifo_readable & rx_cdc_cdc_asyncfifo_re);
assign rx_cdc_cdc_asyncfifo_writable = (((rx_cdc_cdc_graycounter0_q[2] == rx_cdc_cdc_consume_wdomain[2]) | (rx_cdc_cdc_graycounter0_q[1] == rx_cdc_cdc_consume_wdomain[1])) | (rx_cdc_cdc_graycounter0_q[0] != rx_cdc_cdc_consume_wdomain[0]));
assign rx_cdc_cdc_asyncfifo_readable = (rx_cdc_cdc_graycounter1_q != rx_cdc_cdc_produce_rdomain);
assign rx_cdc_cdc_wrport_adr = rx_cdc_cdc_graycounter0_q_binary[1:0];
assign rx_cdc_cdc_wrport_dat_w = rx_cdc_cdc_asyncfifo_din;
assign rx_cdc_cdc_wrport_we = rx_cdc_cdc_graycounter0_ce;
assign rx_cdc_cdc_rdport_adr = rx_cdc_cdc_graycounter1_q_next_binary[1:0];
assign rx_cdc_cdc_asyncfifo_dout = rx_cdc_cdc_rdport_dat_r;

// synthesis translate_off
reg dummy_d_3;
// synthesis translate_on
always @(*) begin
	rx_cdc_cdc_graycounter0_q_next_binary <= 3'd0;
	if (rx_cdc_cdc_graycounter0_ce) begin
		rx_cdc_cdc_graycounter0_q_next_binary <= (rx_cdc_cdc_graycounter0_q_binary + 1'd1);
	end else begin
		rx_cdc_cdc_graycounter0_q_next_binary <= rx_cdc_cdc_graycounter0_q_binary;
	end
// synthesis translate_off
	dummy_d_3 <= dummy_s;
// synthesis translate_on
end
assign rx_cdc_cdc_graycounter0_q_next = (rx_cdc_cdc_graycounter0_q_next_binary ^ rx_cdc_cdc_graycounter0_q_next_binary[2:1]);

// synthesis translate_off
reg dummy_d_4;
// synthesis translate_on
always @(*) begin
	rx_cdc_cdc_graycounter1_q_next_binary <= 3'd0;
	if (rx_cdc_cdc_graycounter1_ce) begin
		rx_cdc_cdc_graycounter1_q_next_binary <= (rx_cdc_cdc_graycounter1_q_binary + 1'd1);
	end else begin
		rx_cdc_cdc_graycounter1_q_next_binary <= rx_cdc_cdc_graycounter1_q_binary;
	end
// synthesis translate_off
	dummy_d_4 <= dummy_s;
// synthesis translate_on
end
assign rx_cdc_cdc_graycounter1_q_next = (rx_cdc_cdc_graycounter1_q_next_binary ^ rx_cdc_cdc_graycounter1_q_next_binary[2:1]);

// synthesis translate_off
reg dummy_d_5;
// synthesis translate_on
always @(*) begin
	jtag_tdo <= 1'd0;
	tx_cdc_source_source_ready <= 1'd0;
	rx_cdc_sink_sink_valid <= 1'd0;
	rx_cdc_sink_sink_payload_data <= 8'd0;
	resetinserter_next_state <= 2'd0;
	valid_next_value0 <= 1'd0;
	valid_next_value_ce0 <= 1'd0;
	data_next_value1 <= 8'd0;
	data_next_value_ce1 <= 1'd0;
	count_next_value2 <= 3'd0;
	count_next_value_ce2 <= 1'd0;
	ready_next_value3 <= 1'd0;
	ready_next_value_ce3 <= 1'd0;
	resetinserter_next_state <= resetinserter_state;
	case (resetinserter_state)
		1'd1: begin
			jtag_tdo <= data;
			if (jtag_shift) begin
				count_next_value2 <= (count + 1'd1);
				count_next_value_ce2 <= 1'd1;
				data_next_value1 <= {jtag_tdi, data[7:1]};
				data_next_value_ce1 <= 1'd1;
				if ((count == 3'd7)) begin
					resetinserter_next_state <= 2'd2;
				end
			end
		end
		2'd2: begin
			jtag_tdo <= valid;
			if (jtag_shift) begin
				rx_cdc_sink_sink_valid <= jtag_tdi;
				rx_cdc_sink_sink_payload_data <= data;
				ready_next_value3 <= rx_cdc_sink_sink_ready;
				ready_next_value_ce3 <= 1'd1;
				resetinserter_next_state <= 1'd0;
			end
		end
		default: begin
			jtag_tdo <= ready;
			if (jtag_shift) begin
				tx_cdc_source_source_ready <= jtag_tdi;
				valid_next_value0 <= tx_cdc_source_source_valid;
				valid_next_value_ce0 <= 1'd1;
				data_next_value1 <= tx_cdc_source_source_payload_data;
				data_next_value_ce1 <= 1'd1;
				count_next_value2 <= 1'd0;
				count_next_value_ce2 <= 1'd1;
				resetinserter_next_state <= 1'd1;
			end
		end
	endcase
// synthesis translate_off
	dummy_d_5 <= dummy_s;
// synthesis translate_on
end
assign tx_cdc_cdc_produce_rdomain = multiregimpl01;
assign tx_cdc_cdc_consume_wdomain = multiregimpl11;
assign rx_cdc_cdc_produce_rdomain = multiregimpl21;
assign rx_cdc_cdc_consume_wdomain = multiregimpl31;

always @(posedge from190_clk) begin
	tx_cdc_cdc_graycounter0_q_binary <= tx_cdc_cdc_graycounter0_q_next_binary;
	tx_cdc_cdc_graycounter0_q <= tx_cdc_cdc_graycounter0_q_next;
	if (from190_rst) begin
		tx_cdc_cdc_graycounter0_q <= 3'd0;
		tx_cdc_cdc_graycounter0_q_binary <= 3'd0;
	end
	multiregimpl10 <= tx_cdc_cdc_graycounter1_q;
	multiregimpl11 <= multiregimpl10;
end

always @(posedge from341_clk) begin
	rx_cdc_cdc_graycounter0_q_binary <= rx_cdc_cdc_graycounter0_q_next_binary;
	rx_cdc_cdc_graycounter0_q <= rx_cdc_cdc_graycounter0_q_next;
	if (from341_rst) begin
		rx_cdc_cdc_graycounter0_q <= 3'd0;
		rx_cdc_cdc_graycounter0_q_binary <= 3'd0;
	end
	multiregimpl30 <= rx_cdc_cdc_graycounter1_q;
	multiregimpl31 <= multiregimpl30;
end

always @(posedge jtag_clk) begin
	alterajtag_state <= alterajtag_next_state;
	resetinserter_state <= resetinserter_next_state;
	if (valid_next_value_ce0) begin
		valid <= valid_next_value0;
	end
	if (data_next_value_ce1) begin
		data <= data_next_value1;
	end
	if (count_next_value_ce2) begin
		count <= count_next_value2;
	end
	if (ready_next_value_ce3) begin
		ready <= ready_next_value3;
	end
	if (fsm_reset) begin
		valid <= 1'd0;
		ready <= 1'd0;
		data <= 8'd0;
		count <= 3'd0;
		resetinserter_state <= 2'd0;
	end
	if (jtag_rst) begin
		valid <= 1'd0;
		ready <= 1'd0;
		data <= 8'd0;
		count <= 3'd0;
		alterajtag_state <= 4'd0;
		resetinserter_state <= 2'd0;
	end
end

always @(posedge jtag_inv_clk) begin
	jtag_reset <= jtag_ongoing0;
	jtag_capture <= jtag_ongoing3;
	jtag_tdouser <= jtag_tdo;
	if (jtag_inv_rst) begin
		jtag_reset <= 1'd0;
		jtag_capture <= 1'd0;
		jtag_tdouser <= 1'd0;
	end
end

always @(posedge to190_clk) begin
	tx_cdc_cdc_graycounter1_q_binary <= tx_cdc_cdc_graycounter1_q_next_binary;
	tx_cdc_cdc_graycounter1_q <= tx_cdc_cdc_graycounter1_q_next;
	if (to190_rst) begin
		tx_cdc_cdc_graycounter1_q <= 3'd0;
		tx_cdc_cdc_graycounter1_q_binary <= 3'd0;
	end
	multiregimpl00 <= tx_cdc_cdc_graycounter0_q;
	multiregimpl01 <= multiregimpl00;
end

always @(posedge to341_clk) begin
	rx_cdc_cdc_graycounter1_q_binary <= rx_cdc_cdc_graycounter1_q_next_binary;
	rx_cdc_cdc_graycounter1_q <= rx_cdc_cdc_graycounter1_q_next;
	if (to341_rst) begin
		rx_cdc_cdc_graycounter1_q <= 3'd0;
		rx_cdc_cdc_graycounter1_q_binary <= 3'd0;
	end
	multiregimpl20 <= rx_cdc_cdc_graycounter0_q;
	multiregimpl21 <= multiregimpl20;
end

cyclonev_jtag cyclonev_jtag(
	.tck(jtag_altera_reserved_tck),
	.tdi(jtag_altera_reserved_tdi),
	.tdouser(jtag_tdouser),
	.tms(jtag_altera_reserved_tms),
	.clkdruser(jtag_drck),
	.runidleuser(jtag_runtest),
	.shiftuser(jtag_shift),
	.tckutap(jtag_tckutap),
	.tdiutap(jtag_tdiutap),
	.tdo(jtag_altera_reserved_tdo),
	.tmsutap(jtag_tmsutap),
	.updateuser(jtag_update),
	.usr1user(jtag_sel)
);

reg [9:0] storage[0:3];
reg [1:0] memadr;
reg [1:0] memadr_1;
always @(posedge from190_clk) begin
	if (tx_cdc_cdc_wrport_we)
		storage[tx_cdc_cdc_wrport_adr] <= tx_cdc_cdc_wrport_dat_w;
	memadr <= tx_cdc_cdc_wrport_adr;
end

always @(posedge to190_clk) begin
	memadr_1 <= tx_cdc_cdc_rdport_adr;
end

assign tx_cdc_cdc_wrport_dat_r = storage[memadr];
assign tx_cdc_cdc_rdport_dat_r = storage[memadr_1];

reg [9:0] storage_1[0:3];
reg [1:0] memadr_2;
reg [1:0] memadr_3;
always @(posedge from341_clk) begin
	if (rx_cdc_cdc_wrport_we)
		storage_1[rx_cdc_cdc_wrport_adr] <= rx_cdc_cdc_wrport_dat_w;
	memadr_2 <= rx_cdc_cdc_wrport_adr;
end

always @(posedge to341_clk) begin
	memadr_3 <= rx_cdc_cdc_rdport_adr;
end

assign rx_cdc_cdc_wrport_dat_r = storage_1[memadr_2];
assign rx_cdc_cdc_rdport_dat_r = storage_1[memadr_3];

DFF ars_cd_jtag_ff0(
	.clk(jtag_clk),
	.clrn(1'd1),
	.d(1'd0),
	.prn((~sys_rst)),
	.q(ars_cd_jtag_rst_meta)
);

DFF ars_cd_jtag_ff1(
	.clk(jtag_clk),
	.clrn(1'd1),
	.d(ars_cd_jtag_rst_meta),
	.prn((~sys_rst)),
	.q(jtag_rst)
);

DFF ars_cd_from190_ff0(
	.clk(from190_clk),
	.clrn(1'd1),
	.d(1'd0),
	.prn((~tx_cdc_cd_rst)),
	.q(ars_cd_from190_rst_meta)
);

DFF ars_cd_from190_ff1(
	.clk(from190_clk),
	.clrn(1'd1),
	.d(ars_cd_from190_rst_meta),
	.prn((~tx_cdc_cd_rst)),
	.q(from190_rst)
);

DFF ars_cd_to190_ff0(
	.clk(to190_clk),
	.clrn(1'd1),
	.d(1'd0),
	.prn((~tx_cdc_cd_rst)),
	.q(ars_cd_to190_rst_meta)
);

DFF ars_cd_to190_ff1(
	.clk(to190_clk),
	.clrn(1'd1),
	.d(ars_cd_to190_rst_meta),
	.prn((~tx_cdc_cd_rst)),
	.q(to190_rst)
);

DFF ars_cd_from341_ff0(
	.clk(from341_clk),
	.clrn(1'd1),
	.d(1'd0),
	.prn((~rx_cdc_cd_rst)),
	.q(ars_cd_from341_rst_meta)
);

DFF ars_cd_from341_ff1(
	.clk(from341_clk),
	.clrn(1'd1),
	.d(ars_cd_from341_rst_meta),
	.prn((~rx_cdc_cd_rst)),
	.q(from341_rst)
);

DFF ars_cd_to341_ff0(
	.clk(to341_clk),
	.clrn(1'd1),
	.d(1'd0),
	.prn((~rx_cdc_cd_rst)),
	.q(ars_cd_to341_rst_meta)
);

DFF ars_cd_to341_ff1(
	.clk(to341_clk),
	.clrn(1'd1),
	.d(ars_cd_to341_rst_meta),
	.prn((~rx_cdc_cd_rst)),
	.q(to341_rst)
);

endmodule
