// Mirlo-N64's scan-out: the N64's VI_ORIGIN framebuffer (RGB565 as MRDP
// writes it: two pixels a word, the left one in the low half) to the
// Pocket's video pads, 320 x 240 at 60.1 Hz (5.712 MHz, 360 x 264 a frame).
//
//  - vid: the timing generator of litex/replaced_components.py
//    FixedVideoTimingGenerator (the 320 x 240 set), which the Pocket's scaler
//    has been fed with; it never stalls -- a word the FIFO does not have in
//    time shows black.
//  - sys: at the frame boundary (the first line of vblank, from vid) the base
//    is latched from VI_ORIGIN (as the LiteX DMA latched its base: a new
//    origin never tears the frame being sent) and the frame's 38,400 words
//    are read in order into the FIFO: 64 KiB (16,384 words, as the LiteX
//    VideoFrameBuffer's), which is what rides out the DRAM contention -- the
//    arbiter gives the scan-out no priority (a priority scheme was tried on
//    the Pocket and went badly; docs/n64.md).
//  - a hi-res framebuffer (VI_WIDTH 512 or more: 640 x 480, interlaced --
//    Pokemon Stadium's menus) is shown 2:1 both ways: each even row's 640
//    pixels are read (the odd row skipped) and every two are averaged, two
//    read words making one FIFO word; the frame is then 76,800 reads.
//  - the FIFO crosses sys -> vid; each word carries the frame's epoch bit, so
//    what is left of a frame is dropped by the vid side instead of resetting
//    a dual-clock FIFO.
//  - native Mirlo (MIRLO's SoC, rtl/soc/mirlo_mips.sv): `mode` picks the
//    timing -- 0 the 268 x 240 set (340 x 280 a frame, 60.0 Hz; scaler slot
//    0), 1 the 320 x 240 one (slot 1), as LiteX's FixedVideoTimingGenerator
//    -- taken at the frame boundary, like the base; the slot goes out on
//    g[3] while DE is low (the Pocket's "Set Scaler Slot"). `vblank` is the
//    timing's vblank as a sys level (APF_VIDEO).
//  - vi_line / vi_frame: each line's start and the frame's (hcount 0, vcount
//    0), as sys pulses, for the RCP's VI_CURRENT and VI interrupt.
//  - OSD (a debug build: osd_en): 16 words (osd_val, sys) as hex in a box at
//    the top left, 8 lines of two -- the only window into the running SoC
//    without a UART, read through the HDMI capture. Read live across the
//    clock domains (no snapshot: 512 flip-flops the device does not have):
//    a value that changes while it is drawn may show torn for a frame.
`default_nettype none

module n64_video #(
    parameter logic [23:0] ORIGIN_MASK = 24'h1FFFFF    // word address: the N64's 8 MiB RDRAM
) (
    input  wire         clk,            // sys
    input  wire         rst,
    input  wire         clk_vid,
    input  wire         rst_vid,
    input  wire         enable,         // sys: scan out (else black, no DMA)
    input  wire  [25:0] vi_origin,      // sys: the framebuffer's byte address (ORIGIN_MASK: its span)
    input  wire         mode,           // sys: 0 268 x 240, 1 320 x 240
    output logic        vblank,         // sys
    input  wire  [11:0] vi_width,       // sys: VI_WIDTH (512 or more: hi-res, shown 2:1)
    input  wire         vi_black,       // sys: H_START 0 (osViBlack): black
    input  wire  [8:0]  vi_lines,       // sys: the lines the VI's window shows (the rest black)
    input  wire         osd_en,
    input  wire         fps_en,         // the frame counter in the corner (when the OSD is off)
    input  wire  [15:0][31:0] osd_val,  // sys
    // the DMA's SDRAM port (sys): word addresses
    output logic        m_valid,
    output logic [23:0] m_addr,
    output logic        m_urgent,       // (always 0: no priority)
    input  wire         m_ready,
    input  wire         m_rvalid,
    input  wire  [31:0] m_rdata,
    // VI clocks (sys pulses)
    output logic        vi_line,
    output logic        vi_frame,
    // pads (vid)
    output logic [4:0]  vga_r,
    output logic [5:0]  vga_g,
    output logic [4:0]  vga_b,
    output logic        vga_hsync,
    output logic        vga_vsync,
    output logic        vga_de
);

localparam int VRES = 240, VSYNC_S = 241, VSYNC_E = 249;
// the mode (vid): taken at the end of a frame
logic [1:0] mode_s;
logic       m320;
always_ff @(posedge clk_vid) mode_s <= {mode_s[0], mode};
wire [8:0] HRES    = m320 ? 9'd320 : 9'd268;
wire [8:0] HSYNC_S = m320 ? 9'd328 : 9'd276;
wire [8:0] HSYNC_E = m320 ? 9'd352 : 9'd308;
wire [8:0] HSCAN   = m320 ? 9'd359 : 9'd339;
wire [8:0] VSCAN   = m320 ? 9'd263 : 9'd279;
localparam int WORDS320 = 320 * VRES / 2, WORDS268 = 268 * VRES / 2;

// ---------------------------------------------------------------- vid: timing
logic [8:0] hcount, vcount;
logic       hactive, vactive, hsync, vsync;
always_ff @(posedge clk_vid) begin
    if (rst_vid) begin
        hcount <= 0; vcount <= 0; hactive <= 0; vactive <= 0; hsync <= 0; vsync <= 0; m320 <= 1;
    end else begin
        if (hcount == HSCAN && vcount == VSCAN) m320 <= mode_s[1];
        hcount <= hcount + 9'd1;
        if (hcount == 0) hactive <= 1;
        if (hcount == HRES) hactive <= 0;
        if (hcount == HSYNC_S) hsync <= 1;
        if (hcount == HSYNC_E) hsync <= 0;
        if (hcount == HSCAN) hcount <= 0;
        if (hcount == HSYNC_S) begin
            vcount <= vcount + 9'd1;
            if (vcount == 0) vactive <= 1;
            if (vcount == VRES) vactive <= 0;
            if (vcount == VSYNC_S) vsync <= 1;
            if (vcount == VSYNC_E) vsync <= 0;
            if (vcount == VSCAN) vcount <= 0;
        end
    end
end
wire de = hactive & vactive;

// the frame boundary: vblank's first line (vid), and the vid side's epoch
wire  fb_vid = hcount == 0 && vcount == VRES + 1;
logic epoch_vid;
always_ff @(posedge clk_vid) if (rst_vid) epoch_vid <= 0; else if (fb_vid) epoch_vid <= ~epoch_vid;

// ------------------------------------------------------------ vid -> sys pulses
logic t_line, t_frame, t_fb;
always_ff @(posedge clk_vid)
    if (rst_vid) begin t_line <= 0; t_frame <= 0; t_fb <= 0; end
    else begin
        if (hcount == 0) t_line <= ~t_line;
        if (hcount == 0 && vcount == 0) t_frame <= ~t_frame;
        if (fb_vid) t_fb <= ~t_fb;
    end
logic [2:0] s_line, s_frame, s_fb;
always_ff @(posedge clk)
    if (rst) begin s_line <= 0; s_frame <= 0; s_fb <= 0; end
    else begin s_line <= {s_line[1:0], t_line}; s_frame <= {s_frame[1:0], t_frame}; s_fb <= {s_fb[1:0], t_fb}; end
wire fb_sys = s_fb[2] ^ s_fb[1];
logic [2:0] s_vb;
always_ff @(posedge clk) s_vb <= {s_vb[1:0], !vactive};
assign vblank = s_vb[2];
always_ff @(posedge clk) begin vi_line <= s_line[2] ^ s_line[1]; vi_frame <= s_frame[2] ^ s_frame[1]; end

// sys: the frame counter's count -- VI origin changes (a frame the game
// showed) in BCD, taken every 60 VI frames (the OSD's corner shows it)
logic [25:0] org_d;
logic [7:0]  fps_cnt, fps_bcd;
logic [5:0]  fps_v;
always_ff @(posedge clk) begin
    org_d <= vi_origin;
    if (rst) begin fps_cnt <= 0; fps_bcd <= 0; fps_v <= 0; end
    else if (vi_frame && fps_v == 6'd59) begin fps_v <= 0; fps_bcd <= fps_cnt; fps_cnt <= 0; end
    else begin
        if (vi_frame) fps_v <= fps_v + 6'd1;
        if (org_d != vi_origin)
            fps_cnt <= fps_cnt[3:0] == 4'd9 ? {fps_cnt[7:4] + 4'd1, 4'd0} : fps_cnt + 8'd1;
    end
end

// ------------------------------------------------------------------ sys: DMA
logic        epoch_sys, running;
logic [16:0] issued, got;
logic        hires, pair;          // the frame is hi-res; a read word waits for its pair
logic        f320;                 // the frame's mode (its word count)
logic [8:0]  col;                  // the read's word in its row
logic [23:0] raddr;                // the next read's word address
logic [31:0] held;
localparam int FAW = 14;           // the FIFO: 2^14 words
logic [9:0]  drop;                 // the last frame's reads still to come back (not written)
logic [FAW:0] wlevel;              // the FIFO's fill as sys sees it (words)
wire         keep = m_rvalid && !fb_sys && drop == 0;
always_ff @(posedge clk) begin
    if (rst) begin epoch_sys <= 0; running <= 0; issued <= 0; got <= 0; drop <= 0;
                   hires <= 0; pair <= 0; col <= 0; raddr <= 0; end
    else begin
        if (fb_sys) begin
            epoch_sys <= ~epoch_sys;
            running <= enable;
            raddr <= vi_origin[25:2] & ORIGIN_MASK;   // SDRAM word address
            f320 <= mode;
            hires <= |vi_width[11:9];
            issued <= 0; got <= 0; pair <= 0; col <= 0;
            drop <= 10'(issued - got - {16'd0, m_rvalid});
        end else begin
            if (m_valid && m_ready) begin
                issued <= issued + 17'd1;
                // hi-res: a row is 320 words, and the odd row after it is skipped
                if (hires && col == 9'd319) begin col <= 0; raddr <= raddr + 24'd321; end
                else begin col <= col + 9'd1; raddr <= raddr + 24'd1; end
            end
            if (keep) begin got <= got + 17'd1; pair <= hires & ~pair; held <= m_rdata; end
            if (m_rvalid && drop != 0) drop <= drop - 10'd1;
        end
    end
end
// two RGB565 pixels (a word) averaged into one
function automatic logic [15:0] avg2(input logic [31:0] w);
    logic [5:0] r, b; logic [6:0] g;
    r = 6'(w[15:11]) + 6'(w[31:27]);
    g = 7'(w[10:5])  + 7'(w[26:21]);
    b = 6'(w[4:0])   + 6'(w[20:16]);
    return {r[5:1], g[6:1], b[5:1]};
endfunction
wire        f_we = keep && (!hires || pair);
wire [31:0] f_wd = hires ? {avg2(m_rdata), avg2(held)} : m_rdata;
// ask while the frame is not all asked for and the FIFO has room for what is in flight
// (room and urgency registered: the FIFO's level must not reach the
// arbiter's grant combinationally; the level is a few cycles late anyway, and
// the margin to 512 covers it)
wire [15:0] inflight = 16'(issued - got);
logic       room;
always_ff @(posedge clk) room <= (16'(wlevel) + inflight) < 16'((1 << FAW) - 64);
assign m_urgent = 1'b0;
assign m_valid  = running && !fb_sys && drop == 0 && issued != (hires ? 17'(2 * WORDS320) : f320 ? 17'(WORDS320) : 17'(WORDS268)) && room;
assign m_addr   = raddr;

// ------------------------------------------------------------------ the FIFO
logic        f_rd, f_empty;
logic [32:0] f_q;
afifo #(.W(33), .AW(FAW)) fifo (
    .wclk(clk), .wrst(rst), .w_en(f_we), .w_data({epoch_sys, f_wd}), .w_level(wlevel),
    .rclk(clk_vid), .rrst(rst_vid), .r_en(f_rd), .r_data(f_q), .r_empty(f_empty)
);

// --------------------------------------------------------------- vid: pixels
// a word: two pixels, low half first. Stale words (the last frame's epoch)
// are dropped whenever they come up. A word not there by its second pixel is
// black and owed (debt): it is dropped when it arrives, so the rest of the
// frame stays where it belongs.
logic        half;                 // the next pixel is the word's high half
logic [15:0] pix;
logic [15:0] debt;
wire         stale = !f_empty && f_q[32] != epoch_vid;
wire         owed  = !f_empty && !stale && debt != 0;
always_comb f_rd = stale || owed || (de && half && !f_empty);
always_ff @(posedge clk_vid) begin
    if (rst_vid) begin half <= 0; pix <= 0; debt <= 0; end
    else begin
        if (fb_vid) begin half <= 0; debt <= 0; end
        else if (owed && !(de && half && f_empty)) debt <= debt - 16'd1;
        else if (de && half && f_empty && !owed) debt <= debt + 16'd1;
        if (de) begin
            if (!f_empty && !stale && !owed) pix <= half ? f_q[31:16] : f_q[15:0];
            else pix <= 16'd0;
            half <= ~half;
        end
    end
end
// ---- the OSD
function automatic logic [7:0] font(input logic [3:0] d, input logic [2:0] r);  // 5x7 hex digits
    logic [4:0] f;
    case ({d, r})
    7'h00: f = 5'h0E;
    7'h01: f = 5'h11;
    7'h02: f = 5'h13;
    7'h03: f = 5'h15;
    7'h04: f = 5'h19;
    7'h05: f = 5'h11;
    7'h06: f = 5'h0E;
    7'h08: f = 5'h04;
    7'h09: f = 5'h0C;
    7'h0A: f = 5'h04;
    7'h0B: f = 5'h04;
    7'h0C: f = 5'h04;
    7'h0D: f = 5'h04;
    7'h0E: f = 5'h0E;
    7'h10: f = 5'h0E;
    7'h11: f = 5'h11;
    7'h12: f = 5'h01;
    7'h13: f = 5'h02;
    7'h14: f = 5'h04;
    7'h15: f = 5'h08;
    7'h16: f = 5'h1F;
    7'h18: f = 5'h1F;
    7'h19: f = 5'h02;
    7'h1A: f = 5'h04;
    7'h1B: f = 5'h02;
    7'h1C: f = 5'h01;
    7'h1D: f = 5'h11;
    7'h1E: f = 5'h0E;
    7'h20: f = 5'h02;
    7'h21: f = 5'h06;
    7'h22: f = 5'h0A;
    7'h23: f = 5'h12;
    7'h24: f = 5'h1F;
    7'h25: f = 5'h02;
    7'h26: f = 5'h02;
    7'h28: f = 5'h1F;
    7'h29: f = 5'h10;
    7'h2A: f = 5'h1E;
    7'h2B: f = 5'h01;
    7'h2C: f = 5'h01;
    7'h2D: f = 5'h11;
    7'h2E: f = 5'h0E;
    7'h30: f = 5'h06;
    7'h31: f = 5'h08;
    7'h32: f = 5'h10;
    7'h33: f = 5'h1E;
    7'h34: f = 5'h11;
    7'h35: f = 5'h11;
    7'h36: f = 5'h0E;
    7'h38: f = 5'h1F;
    7'h39: f = 5'h01;
    7'h3A: f = 5'h02;
    7'h3B: f = 5'h04;
    7'h3C: f = 5'h08;
    7'h3D: f = 5'h08;
    7'h3E: f = 5'h08;
    7'h40: f = 5'h0E;
    7'h41: f = 5'h11;
    7'h42: f = 5'h11;
    7'h43: f = 5'h0E;
    7'h44: f = 5'h11;
    7'h45: f = 5'h11;
    7'h46: f = 5'h0E;
    7'h48: f = 5'h0E;
    7'h49: f = 5'h11;
    7'h4A: f = 5'h11;
    7'h4B: f = 5'h0F;
    7'h4C: f = 5'h01;
    7'h4D: f = 5'h02;
    7'h4E: f = 5'h0C;
    7'h50: f = 5'h0E;
    7'h51: f = 5'h11;
    7'h52: f = 5'h11;
    7'h53: f = 5'h1F;
    7'h54: f = 5'h11;
    7'h55: f = 5'h11;
    7'h56: f = 5'h11;
    7'h58: f = 5'h1E;
    7'h59: f = 5'h11;
    7'h5A: f = 5'h11;
    7'h5B: f = 5'h1E;
    7'h5C: f = 5'h11;
    7'h5D: f = 5'h11;
    7'h5E: f = 5'h1E;
    7'h60: f = 5'h0E;
    7'h61: f = 5'h11;
    7'h62: f = 5'h10;
    7'h63: f = 5'h10;
    7'h64: f = 5'h10;
    7'h65: f = 5'h11;
    7'h66: f = 5'h0E;
    7'h68: f = 5'h1C;
    7'h69: f = 5'h12;
    7'h6A: f = 5'h11;
    7'h6B: f = 5'h11;
    7'h6C: f = 5'h11;
    7'h6D: f = 5'h12;
    7'h6E: f = 5'h1C;
    7'h70: f = 5'h1F;
    7'h71: f = 5'h10;
    7'h72: f = 5'h10;
    7'h73: f = 5'h1E;
    7'h74: f = 5'h10;
    7'h75: f = 5'h10;
    7'h76: f = 5'h1F;
    7'h78: f = 5'h1F;
    7'h79: f = 5'h10;
    7'h7A: f = 5'h10;
    7'h7B: f = 5'h1E;
    7'h7C: f = 5'h10;
    7'h7D: f = 5'h10;
    7'h7E: f = 5'h10;
    default: f = 5'h00;
    endcase
    return {1'b0, f, 2'b00};
endfunction
localparam int OX = 4, OY = 4;             // the box's top left (active pixels)
wire  [8:0] ax = hcount - 9'd1 - 9'(OX), ay = vcount - 9'd1 - 9'(OY);
wire        in_box = osd_en && de && ax < 9'(17 * 8) && ay < 9'(8 * 8);
// the frame counter (fps_en, without the OSD): the frames the game showed --
// VI origin changes -- over the last 60 VI frames, two decimal digits in the
// top left corner
logic [7:0] fps_s1, fps_s2;
always_ff @(posedge clk_vid) begin fps_s1 <= fps_bcd; fps_s2 <= fps_s1; end
wire        in_fps = fps_en && !osd_en && de && ax < 9'd16 && ay < 9'd8;
logic       s1_in, s1_sp;
logic [2:0] s1_px, s1_py;
logic [3:0] s1_nib;
logic       osd_in, osd_px;
wire  [7:0] glyph = font(s1_nib, s1_py);
always_ff @(posedge clk_vid) begin
    // stage 1: the character and its digit
    s1_in <= in_box || in_fps;
    s1_px <= ax[2:0]; s1_py <= ay[2:0];
    s1_sp <= in_box && ax[7:3] == 5'd8;                 // the column between the two words
    s1_nib <= in_fps ? (ax[3] ? fps_s2[3:0] : fps_s2[7:4])
                     : osd_val[{ay[5:3], ax[7:3] > 5'd8}][4 * (7 - (ax[7:3] > 5'd8 ? ax[7:3] - 5'd9 : ax[7:3])) +: 4];
    // stage 2: the pixel
    osd_in <= s1_in;
    osd_px <= !s1_sp && glyph[3'd7 - s1_px];
end

// the VI's window (slow-changing, from sys): black while H_START is 0 --
// libultra's osViBlack, the game's level changes, when the framebuffer is
// being rewritten -- and below its last line (V_START..V_END: SM64 shows
// 237 lines from its origin, one line into the framebuffer: the 240th
// would be the next framebuffer's first, or the memory past the last one)
logic [1:0] blk_s; logic [8:0] lin_s1, lin_s2;
always_ff @(posedge clk_vid) begin blk_s <= {blk_s[0], vi_black}; lin_s1 <= vi_lines; lin_s2 <= lin_s1; end
wire  crop = blk_s[1] || vcount > lin_s2;           // (active lines are vcount 1..240)

// outputs, behind the timing by the pixel's register and the OSD's two stages
logic de_d, hs_d, vs_d, de_d2, hs_d2, vs_d2, cr_d, cr_d2;
logic [15:0] pix_d;
always_ff @(posedge clk_vid) begin
    de_d <= de; hs_d <= hsync; vs_d <= vsync; cr_d <= crop;
    de_d2 <= de_d; hs_d2 <= hs_d; vs_d2 <= vs_d; cr_d2 <= cr_d; pix_d <= pix;
    vga_de <= de_d2; vga_hsync <= hs_d2; vga_vsync <= vs_d2;
    vga_r <= !de_d2 ? 5'd0 : osd_in ? {5{osd_px}} : cr_d2 ? 5'd0 : pix_d[15:11];
    vga_g <= !de_d2 ? {2'd0, m320, 3'd0} : osd_in ? {6{osd_px}} : cr_d2 ? 6'd0 : pix_d[10:5];
    vga_b <= !de_d2 ? 5'd0 : osd_in ? {5{osd_px}} : cr_d2 ? 5'd0 : pix_d[4:0];
end

endmodule
`default_nettype wire
