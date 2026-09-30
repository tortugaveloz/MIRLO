// The Cyclone V JTAG atom (jtag_uart_bridge.v) for Verilator: nobody at the
// other end -- the user chain idle.
module cyclonev_jtag (
    input  wire tms, tck, tdi, tdouser,
    output wire tdo, tmsutap, tckutap, tdiutap, shiftuser, clkdruser, updateuser, runidleuser, usr1user
);
    assign tdo = 1'b0; assign tmsutap = 1'b0; assign tckutap = 1'b0; assign tdiutap = 1'b0;
    assign shiftuser = 1'b0; assign clkdruser = 1'b0; assign updateuser = 1'b0; assign runidleuser = 1'b0;
    assign usr1user = 1'b0;
endmodule

// Intel's DFF primitive (jtag_uart_bridge.v's reset synchronisers)
module DFF (input wire d, clk, clrn, prn, output reg q);
    always @(posedge clk or negedge clrn or negedge prn)
        if (!clrn) q <= 1'b0; else if (!prn) q <= 1'b1; else q <= d;
endmodule
