// MRDP memory engine: every SDRAM access, one row at a time, over a
// LiteDRAM-native-style port (32-bit words = two 16-bit pixels; commands and
// read data in order; write data after its command is accepted).
//
//   OP_RD_Z / OP_RD_C  span [x0, x1) of a row into the Z / color line buffer
//   OP_WR_Z / OP_WR_C  the same span back, only pixels whose mask bit is set;
//                      words with no such pixel are not written at all
//   OP_FILL            span [x0, x1) := the fill color (even pixel [31:16])
//   OP_LOAD            texels [x0, x1) of a texture row into TMEM row `trow`
//
// A row's base address (image + y * width * 2) must be 4-byte aligned for
// the line-buffer ops (images 4-aligned, even widths); OP_LOAD takes any.
module mrdp_mem (
    input  wire        clk,
    input  wire        rst,

    input  wire        req_valid,
    output wire        req_ready,
    input  wire [2:0]  req_op,
    input  wire [31:0] req_base,
    input  wire [9:0]  req_x0,
    input  wire [10:0] req_x1,          // exclusive, up to 1024
    input  wire [31:0] req_fill,
    input  wire [9:0]  req_trow,
    input  wire [9:0]  req_tmem,
    input  wire [8:0]  req_line,

    // line buffers: fill port (reads from SDRAM) ...
    output reg         lbw_z_we,
    output reg         lbw_c_we,
    output reg  [8:0]  lbw_addr,
    output reg  [31:0] lbw_data,
    // ... and read port (write-back); masks: bit 0 even pixel, bit 1 odd
    output reg  [8:0]  lbr_addr,
    input  wire [31:0] lbr_zdata,
    input  wire [31:0] lbr_cdata,
    input  wire [1:0]  lbr_zmask,
    input  wire [1:0]  lbr_cmask,

    // TMEM writes (up to two banks a cycle)
    output reg  [3:0]  tm_we,
    output reg  [9:0]  tm_waddr0, tm_waddr1, tm_waddr2, tm_waddr3,
    output reg  [15:0] tm_wdata0, tm_wdata1, tm_wdata2, tm_wdata3,

    // SDRAM
    output reg         m_cmd_valid,
    input  wire        m_cmd_ready,
    output reg         m_cmd_we,
    output reg  [31:0] m_cmd_addr,      // byte address, 4-aligned
    output wire        m_wdata_valid,
    input  wire        m_wdata_ready,
    output wire [31:0] m_wdata,
    output wire [3:0]  m_wdata_we,
    input  wire        m_rdata_valid,
    input  wire [31:0] m_rdata,

    output wire        busy,            // a request in progress
    output wire        writes_pending   // write data not yet accepted
);
    localparam OP_RD_Z = 3'd0, OP_RD_C = 3'd1, OP_WR_Z = 3'd2, OP_WR_C = 3'd3,
               OP_FILL = 3'd4, OP_LOAD = 3'd5;

    // ---- request registers
    reg        active;
    reg [2:0]  op;
    reg [31:0] base;
    reg [9:0]  x0;
    reg [10:0] x1;
    reg [31:0] fill;
    reg [9:0]  trow, tm_row;              // tm_row: the TMEM row's first address
    reg [29:0] w_first, w_last, w_next;   // word addresses
    reg [29:0] w_recv;                    // next read word to arrive
    reg        issued_all;
    wire is_read  = op == OP_RD_Z || op == OP_RD_C || op == OP_LOAD;
    wire is_write = op == OP_WR_Z || op == OP_WR_C || op == OP_FILL;

    wire [31:0] first_byte = req_base + {21'd0, req_x0, 1'b0};
    wire [31:0] last_byte  = req_base + {20'd0, req_x1 - 11'd1, 1'b0};

    assign req_ready = !active;
    assign busy = active;

    // ---- write data FIFO (data follows its accepted command)
    // A RAM with a registered read and the head in an output register (show
    // ahead): read combinationally it was 576 flip-flops and a 16-way mux,
    // and asking Quartus for LUT RAM instead crashed its Verilog front end.
    localparam WF = 4;                    // 16 entries
    reg [35:0] wf [0:(1<<WF)-1];
    reg [WF:0] wf_wr, wf_rd;
    reg [35:0] wq;                        // the head
    reg        wq_v;
    wire       wf_mem_empty = wf_wr == wf_rd;
    wire       wf_full  = (wf_wr - wf_rd) >= ((1 << WF) - 3);
    wire       wf_pop   = wq_v && m_wdata_ready;
    assign m_wdata_valid = wq_v;
    assign m_wdata    = wq[31:0];
    assign m_wdata_we = wq[35:32];
    assign writes_pending = wq_v || !wf_mem_empty || (m_cmd_valid && m_cmd_we);

    // ---- write path: line-buffer addresses go out one a cycle; the RAM's
    // data is valid two edges later (p1 -> p2) and lands in a 4-entry FIFO;
    // its head becomes a command (or is dropped when no pixel passed)
    reg        p1, p2;
    reg [29:0] p1_word, p2_word;
    reg [97:0] rf [0:3];                  // {word, zdata, cdata, zmask, cmask}
    reg [2:0]  rf_wr, rf_rd;
    wire [2:0] rf_cnt = rf_wr - rf_rd;
    wire       rf_room = ({1'b0, rf_cnt} + {3'd0, p1} + {3'd0, p2}) < 4'd4;
    wire [97:0] rf_head = rf[rf_rd[1:0]];

    // pixel range of a word (line-buffer ops: base is 4-aligned)
    wire [29:0] base_w = base[31:2];
    function automatic [3:0] range_sel;
        input [29:0] w;
        reg [10:0] xe;   // even pixel of the word
        begin
            xe = {w - base_w, 1'b0};
            range_sel = { {2{(xe + 11'd1) >= {1'b0, x0} && (xe + 11'd1) < x1}},
                          {2{xe >= {1'b0, x0} && xe < x1}} };
        end
    endfunction
    wire [29:0] h_word  = rf_head[97:68];
    wire [31:0] h_zdata = rf_head[67:36];
    wire [31:0] h_cdata = rf_head[35:4];
    wire [1:0]  h_zmask = rf_head[3:2];
    wire [1:0]  h_cmask = rf_head[1:0];
    wire [3:0]  h_range = range_sel(h_word);
    wire [31:0] h_data = (op == OP_FILL) ? {fill[15:0], fill[31:16]} : (op == OP_WR_Z) ? h_zdata : h_cdata;
    wire [3:0]  h_sel  = (op == OP_FILL) ? h_range
                       : (op == OP_WR_Z) ? (h_range & { {2{h_zmask[1]}}, {2{h_zmask[0]}} })
                       : (h_range & { {2{h_cmask[1]}}, {2{h_cmask[0]}} });
    // ---- TMEM destination of a load texel (s relative to x0)
    // (the row part once per request, not per texel)
    function automatic [9:0] tm_addr;
        input [9:0] s;
        tm_addr = tm_row + {1'b0, s[9:1]};
    endfunction
    wire [18:0] req_row = req_trow[9:1] * req_line;

    integer i;
    always @(posedge clk) begin
        lbw_z_we <= 1'b0;
        lbw_c_we <= 1'b0;
        tm_we <= 4'd0;
        if (rst) begin
            active <= 1'b0;
            m_cmd_valid <= 1'b0;
            wf_wr <= 0;
            wf_rd <= 0;
            wq_v <= 1'b0;
            p1 <= 1'b0; p2 <= 1'b0;
            rf_wr <= 0; rf_rd <= 0;
        end else begin
            // write data out: refill the head from the RAM
            if (!wq_v || wf_pop) begin
                if (!wf_mem_empty) begin
                    wq <= wf[wf_rd[WF-1:0]];
                    wq_v <= 1'b1;
                    wf_rd <= wf_rd + 1'b1;
                end else
                    wq_v <= 1'b0;
            end

            // command accepted
            if (m_cmd_valid && m_cmd_ready) m_cmd_valid <= 1'b0;

            if (!active) begin
                if (req_valid) begin
                    active <= 1'b1;
                    op <= req_op; base <= req_base; x0 <= req_x0; x1 <= req_x1;
                    fill <= req_fill; trow <= req_trow;
                    tm_row <= req_tmem + {req_row[8:0], 1'b0};
                    w_first <= first_byte[31:2];
                    w_last <= last_byte[31:2];
                    w_next <= first_byte[31:2];
                    w_recv <= first_byte[31:2];
                    issued_all <= 1'b0;
                    p1 <= 1'b0; p2 <= 1'b0;
                end
            end else if (is_read) begin
                // issue read commands back to back
                if (!issued_all && (!m_cmd_valid || m_cmd_ready)) begin
                    m_cmd_valid <= 1'b1;
                    m_cmd_we <= 1'b0;
                    m_cmd_addr <= {w_next, 2'b00};
                    if (w_next == w_last) issued_all <= 1'b1;
                    w_next <= w_next + 30'd1;
                end
                // data back, in order
                if (m_rdata_valid) begin
                    if (op == OP_LOAD) begin
                        // two halfwords: bytes w*4 and w*4+2 -> texels relative to x0
                        for (i = 0; i < 2; i = i + 1) begin : ld
                            reg [31:0] byte_off;
                            reg [31:0] s_abs;
                            reg [9:0]  s_rel;
                            byte_off = {w_recv, 2'b00} + (i * 2) - base;
                            s_abs = byte_off >> 1;
                            if (s_abs >= {22'd0, x0} && s_abs < {21'd0, x1}) begin
                                s_rel = s_abs[9:0] - x0;
                                case ({trow[0], s_rel[0]})
                                    2'd0: begin tm_we[0] <= 1'b1; tm_waddr0 <= tm_addr(s_rel); tm_wdata0 <= i ? m_rdata[31:16] : m_rdata[15:0]; end
                                    2'd1: begin tm_we[1] <= 1'b1; tm_waddr1 <= tm_addr(s_rel); tm_wdata1 <= i ? m_rdata[31:16] : m_rdata[15:0]; end
                                    2'd2: begin tm_we[2] <= 1'b1; tm_waddr2 <= tm_addr(s_rel); tm_wdata2 <= i ? m_rdata[31:16] : m_rdata[15:0]; end
                                    default: begin tm_we[3] <= 1'b1; tm_waddr3 <= tm_addr(s_rel); tm_wdata3 <= i ? m_rdata[31:16] : m_rdata[15:0]; end
                                endcase
                            end
                        end
                    end else begin
                        lbw_addr <= w_recv - base_w;
                        lbw_data <= m_rdata;
                        if (op == OP_RD_Z) lbw_z_we <= 1'b1; else lbw_c_we <= 1'b1;
                    end
                    if (w_recv == w_last) active <= 1'b0;
                    w_recv <= w_recv + 30'd1;
                end
            end else begin
                // ---- writes
                // FIFO head -> command + data (or dropped: no pixel passed)
                if (rf_wr != rf_rd && !m_cmd_valid && !wf_full) begin
                    if (h_sel != 4'd0) begin
                        m_cmd_valid <= 1'b1;
                        m_cmd_we <= 1'b1;
                        m_cmd_addr <= {h_word, 2'b00};
                        wf[wf_wr[WF-1:0]] <= {h_sel, h_data};
                        wf_wr <= wf_wr + 1'b1;
                    end
                    rf_rd <= rf_rd + 3'd1;
                end else if (rf_wr != rf_rd && h_sel == 4'd0) begin
                    rf_rd <= rf_rd + 3'd1;
                end
                // the RAM's data for the word addressed two edges ago
                p2 <= p1;
                p2_word <= p1_word;
                if (p2) begin
                    rf[rf_wr[1:0]] <= {p2_word, lbr_zdata, lbr_cdata, lbr_zmask, lbr_cmask};
                    rf_wr <= rf_wr + 3'd1;
                end
                // next address
                if (!issued_all && rf_room) begin
                    lbr_addr <= w_next - base_w;
                    p1 <= 1'b1;
                    p1_word <= w_next;
                    if (w_next == w_last) issued_all <= 1'b1;
                    w_next <= w_next + 30'd1;
                end else begin
                    p1 <= 1'b0;
                end
                if (issued_all && !p1 && !p2 && rf_wr == rf_rd)
                    active <= 1'b0;
            end
        end
    end
endmodule
