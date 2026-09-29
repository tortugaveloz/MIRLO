// MRDP pixel pipeline: lang/c/mrdp/mrdp.c draw_pixel(), one pixel per clock,
// no stalls (every operand is on chip: TMEM and the span's line buffers).
// Stage by stage the arithmetic is the model's; the model is the contract.
//
// Per-primitive state must stay stable while pixels are in flight (the
// sequencer drains the pipe before a state command). Latency LAT cycles
// from in_valid to the line-buffer write.
module mrdp_pix (
    input  wire        clk,
    input  wire        rst,

    // ---- per-primitive state
    input  wire [23:0] other_h,
    input  wire [31:0] other_l,
    input  wire [23:0] cc_hi,
    input  wire [31:0] cc_lo,
    input  wire [31:0] prim, env, fog, blend,   // RGBA, R in [31:24]
    input  wire [7:0]  prim_lod_frac,
    input  wire [15:0] prim_z,
    input  wire [31:0] fill,
    input  wire        p_shade, p_tex, p_zbuf, p_direct,   // p_direct: S,T are s10.5 already
    input  wire [2:0]  t_fmt,
    input  wire [8:0]  t_line,
    input  wire [9:0]  t_tmem,
    input  wire        t_cs, t_ms, t_ct, t_mt,
    input  wire [3:0]  t_masks, t_shifts, t_maskt, t_shiftt,
    input  wire [11:0] t_uls, t_ult, t_lrs, t_lrt,

    // ---- pixels in
    input  wire        in_valid,
    input  wire [9:0]  in_x,
    input  wire [31:0] in_r, in_g, in_b, in_a, in_s, in_t, in_w, in_z,
    // AA_EN coverage (mrdp_top): a pixel with no covered sample goes down the
    // pipe but writes nothing; Z moves to its first covered sample
    input  wire        in_kill,
    input  wire        in_part, in_minor,      // < 8 / < 4 of the 8 samples covered
    input  wire signed [2:0] in_sx8, in_sy8,   // that sample from the centre, 1/8 px
    input  wire [31:0] dzdx, dzdy,             // the triangle's, quasi-static

    // ---- TMEM, 4 banks, registered read (data one cycle after the address)
    output reg  [9:0]  tm_a0, tm_a1, tm_a2, tm_a3,
    input  wire [15:0] tm_d0, tm_d1, tm_d2, tm_d3,

    // ---- span line buffers (512 x 32: two pixels a word), registered read
    output reg  [8:0]  lb_raddr,
    input  wire [31:0] zlb_rdata,
    input  wire [31:0] clb_rdata,
    output reg         out_valid,        // one pixel result
    output reg  [9:0]  out_x,
    output reg  [15:0] out_color,
    output reg  [15:0] out_depth,
    output reg         out_cpass,        // write the color
    output reg         out_zpass,        // write the depth

    output wire        busy
);
    // ======================================================================
    // helpers
    // ======================================================================
    function automatic [7:0] clamp8;
        input signed [19:0] v;
        clamp8 = v < 0 ? 8'd0 : (v > 255 ? 8'd255 : v[7:0]);
    endfunction

    function automatic [8:0] wgt;             // 255 means 1.0: c + (c >> 7)
        input [7:0] c;
        wgt = {1'b0, c} + {8'd0, c[7]};
    endfunction

    // (a - b) * c + d, c = 255 meaning 1
    function automatic [7:0] cc_eval;
        input [7:0] a, b, c, d;
        reg signed [9:0]  diff;
        reg signed [19:0] prod;
        begin
            diff = $signed({2'b00, a}) - $signed({2'b00, b});
            prod = diff * $signed({1'b0, wgt(c)});
            cc_eval = clamp8((prod >>> 8) + $signed({12'd0, d}));
        end
    endfunction

    // 565 <-> RGBA8888
    function automatic [31:0] un565;
        input [15:0] v;
        un565 = { v[15:11], v[15:13], v[10:5], v[10:9], v[4:0], v[4:2], 8'hFF };
    endfunction

    // texel expand: 0 RGBA5551, 1 IA88, 2 RGBA4444
    function automatic [31:0] texel;
        input [2:0]  fmt;
        input [15:0] v;
        begin
            case (fmt)
                3'd1: texel = { v[15:8], v[15:8], v[15:8], v[7:0] };
                3'd2: texel = { v[15:12], v[15:12], v[11:8], v[11:8], v[7:4], v[7:4], v[3:0], v[3:0] };
                default: texel = { v[15:11], v[15:13], v[10:6], v[10:8], v[5:1], v[5:3], {8{v[0]}} };
            endcase
        end
    endfunction

    // ======================================================================
    // decoded modes
    // ======================================================================
    wire [1:0] cyc      = other_h[21:20];
    wire       persp    = other_h[19];
    wire       point    = other_h[13:12] == 2'd0;
    wire       a_cmp    = other_l[0];
    wire       z_src_p  = other_l[2];
    wire       z_cmp    = other_l[4];
    wire       z_upd    = other_l[5];
    wire       decal    = other_l[11:10] == 2'd3;
    wire       cvg_x_a  = other_l[12];
    wire       force_bl = other_l[14];

    // ======================================================================
    // P0: input registers
    // ======================================================================
    reg        v0;
    reg k0, k1, k2, k3, k4, k5, k6, k7, k8, k8b, k9, k10a, k10, k11a, k11, k12a, k12, k13a;   // AA_EN: no covered sample
    reg [1:0] c0, c1, c2, c3, c4, c5, c6, c7, c8, c8b, c9, c10a, c10, c11a, c11;          // AA_EN: {partial, minority}
    reg [9:0]  x0;
    reg [31:0] s0r, t0r, w0r;
    reg [31:0] shade0;
    reg [15:0] depth0;
    wire [7:0] sr8 = clamp8($signed(in_r[31:16])), sg8 = clamp8($signed(in_g[31:16]));
    wire [7:0] sb8 = clamp8($signed(in_b[31:16])), sa8 = clamp8($signed(in_a[31:16]));
    // Z at the first covered sample: ((dzdx >> 5) sx + (dzdy >> 5) sy) << 2,
    // sx, sy in 1/8 px (lang/c/mrdp/mrdp.c draw_triangle_aa), on two DSPs
`ifdef MRDP_COVERAGE
    (* multstyle = "dsp" *) wire signed [29:0] zcx = $signed(dzdx[31:5]) * in_sx8;
    (* multstyle = "dsp" *) wire signed [29:0] zcy = $signed(dzdy[31:5]) * in_sy8;
    wire [31:0] zcorr = ({{2{zcx[29]}}, zcx} + {{2{zcy[29]}}, zcy}) << 2;
    wire [31:0] zin_c = in_z + zcorr;
`else
    wire [31:0] zin_c = in_z;            // (the crack grow: Z at the pixel centre)
`endif
    wire signed [31:0] zq = $signed(zin_c) >>> 15;
    wire [15:0] dz = zq < 0 ? 16'd0 : (zq > 32'sd65535 ? 16'hFFFF : zq[15:0]);
    always @(posedge clk) begin
        v0 <= in_valid & ~rst;
        x0 <= in_x; k0 <= in_kill; c0 <= {in_part, in_minor};
        s0r <= in_s; t0r <= in_t; w0r <= in_w;
        shade0 <= p_shade ? {sr8, sg8, sb8, sa8} : 32'd0;
        depth0 <= (z_src_p || !p_zbuf) ? prim_z : dz;
    end

    // ======================================================================
    // P1: normalise W, table lookup
    // ======================================================================
    wire [31:0] wpos = ($signed(w0r) <= 0) ? 32'd1 : w0r;
    reg  [4:0]  sh_c;
    integer k;
    always @(*) begin
        sh_c = 5'd30;
        for (k = 0; k <= 30; k = k + 1)
            if (wpos[k]) sh_c = 5'd30 - k[4:0];
    end
    wire [31:0] wn = wpos << sh_c;
    wire [16:0] rom_p;
    wire [10:0] rom_d;
    mrdp_rcp_rom rom (.idx(wn[29:24]), .p(rom_p), .d(rom_d));

    reg        v1;
    reg [9:0]  x1;
    reg [16:0] p1;
    reg [10:0] d1;
    reg [8:0]  f1;
    reg [4:0]  sh1;
    reg [31:0] s1r, t1r;
    reg [31:0] shade1;
    reg [15:0] depth1;
    always @(posedge clk) begin
        v1 <= v0 & ~rst; x1 <= x0; k1 <= k0; c1 <= c0;
        p1 <= rom_p; d1 <= rom_d; f1 <= wn[23:15]; sh1 <= sh_c;
        s1r <= s0r; t1r <= t0r; shade1 <= shade0; depth1 <= depth0;
    end

    // ======================================================================
    // P2: r = P - (D * frac >> 9); S, T clamped to the 27-bit multiplier input
    // ======================================================================
    wire [19:0] dfrac = d1 * f1;
    function automatic [26:0] sat27;
        input [31:0] v;
        begin
            if ($signed(v) > 32'sd67108863) sat27 = 27'h3FFFFFF;
            else if ($signed(v) < -32'sd67108864) sat27 = 27'h4000000;
            else sat27 = v[26:0];
        end
    endfunction
    reg        v2;
    reg [9:0]  x2;
    reg [16:0] r2;
    reg [4:0]  sh2;
    reg [26:0] sc2, tc2;
    reg [31:0] s2r, t2r;
    reg [31:0] shade2;
    reg [15:0] depth2;
    always @(posedge clk) begin
        v2 <= v1 & ~rst; x2 <= x1; k2 <= k1; c2 <= c1;
        r2 <= p1 - {6'd0, dfrac[19:9]};
        sh2 <= sh1;
        sc2 <= sat27(s1r); tc2 <= sat27(t1r);
        s2r <= s1r; t2r <= t1r; shade2 <= shade1; depth2 <= depth1;
    end

    // ======================================================================
    // P3: S * r, T * r
    // ======================================================================
    reg        v3;
    reg [9:0]  x3;
    reg signed [44:0] ps3, pt3;
    reg [4:0]  sh3;
    reg [31:0] s3r, t3r;
    reg [31:0] shade3;
    reg [15:0] depth3;
    always @(posedge clk) begin
        v3 <= v2 & ~rst; x3 <= x2; k3 <= k2; c3 <= c2;
        ps3 <= $signed(sc2) * $signed({1'b0, r2});
        pt3 <= $signed(tc2) * $signed({1'b0, r2});
        sh3 <= sh2;
        s3r <= s2r; t3r <= t2r; shade3 <= shade2; depth3 <= depth2;
    end

    // ======================================================================
    // P4: s10.5 coordinates (perspective, affine, or direct)
    // ======================================================================
    function automatic [15:0] sat16_45;
        input signed [44:0] v;
        sat16_45 = (v > 45'sd32767) ? 16'h7FFF : ((v < -45'sd32768) ? 16'h8000 : v[15:0]);
    endfunction
    function automatic [15:0] sat16_32;
        input signed [31:0] v;
        sat16_32 = (v > 32'sd32767) ? 16'h7FFF : ((v < -32'sd32768) ? 16'h8000 : v[15:0]);
    endfunction
    // W is 2.30 (lang/c/mrdp/mrdp_setup.h mrdp_slope30): 41 - 14, never below 0
    wire [5:0] shamt = (sh3 > 5'd27) ? 6'd0 : 6'd27 - {1'b0, sh3};
    wire signed [44:0] ps_sh = ps3 >>> shamt;
    wire signed [44:0] pt_sh = pt3 >>> shamt;
    reg        v4;
    reg [9:0]  x4;
    reg [15:0] s4, t4;
    reg [31:0] shade4;
    reg [15:0] depth4;
    always @(posedge clk) begin
        v4 <= v3 & ~rst; x4 <= x3; k4 <= k3; c4 <= c3;
        if (p_direct) begin
            s4 <= s3r[15:0]; t4 <= t3r[15:0];
        end else if (persp) begin
            s4 <= sat16_45(ps_sh); t4 <= sat16_45(pt_sh);
        end else begin
            s4 <= sat16_32($signed(s3r) >>> 11); t4 <= sat16_32($signed(t3r) >>> 11);
        end
        shade4 <= shade3; depth4 <= depth3;
    end

    // ======================================================================
    // P5: tile shift, origin (P5a); clamp, mirror, mask (P5b)
    // ======================================================================
    function automatic signed [23:0] tc_shift;
        input [15:0] v;
        input [3:0]  shift;
        reg signed [23:0] e;
        begin
            e = {{8{v[15]}}, v};
            if (shift == 4'd0) tc_shift = e;
            else if (shift <= 4'd10) tc_shift = e >>> shift;
            else tc_shift = e <<< (5'd16 - {1'b0, shift});
        end
    endfunction
    wire signed [23:0] vs5 = tc_shift(s4, t_shifts) - $signed({9'd0, t_uls, 3'd0});
    wire signed [23:0] vt5 = tc_shift(t4, t_shiftt) - $signed({9'd0, t_ult, 3'd0});
    reg        v5;
    reg [9:0]  x5;
    reg signed [23:0] vs5r, vt5r;
    reg [31:0] shade5;
    reg [15:0] depth5;
    always @(posedge clk) begin
        v5 <= v4 & ~rst; x5 <= x4; k5 <= k4; c5 <= c4;
        vs5r <= vs5; vt5r <= vt5;
        shade5 <= shade4; depth5 <= depth4;
    end

    // one axis: v -> c0, c1 (wrapped) and the 5-bit fraction
    task automatic tc_axis;
        input  signed [23:0] v;
        input  [11:0] lo, hi;
        input  clampbit, mirror;
        input  [3:0] mask;
        output [10:0] c0, c1;
        output [4:0]  fr;
        reg signed [18:0] i, j, mx, m;
        reg [4:0] f;
        begin
            i = v >>> 5;
            f = v[4:0];
            j = i + 19'sd1;
            if (clampbit || mask == 4'd0) begin
                mx = ($signed({7'd0, hi}) - $signed({7'd0, lo})) >>> 2;
                if (i < 0) begin i = 0; j = 0; f = 0; end
                else if (i >= mx) begin i = mx; j = mx; f = 0; end
            end
            if (mask != 4'd0) begin
                m = (19'sd1 <<< mask) - 19'sd1;
                if (mirror && ((i >>> mask) & 19'sd1)) i = ~i;
                if (mirror && ((j >>> mask) & 19'sd1)) j = ~j;
                i = i & m;
                j = j & m;
            end
            c0 = i[10:0];
            c1 = j[10:0];
            fr = f;
        end
    endtask

    reg [10:0] s0c, s1c, t0c, t1c;
    reg [4:0]  fsc, ftc;
    always @(*) begin
        tc_axis(vs5r, t_uls, t_lrs, t_cs, t_ms, t_masks, s0c, s1c, fsc);
        tc_axis(vt5r, t_ult, t_lrt, t_ct, t_mt, t_maskt, t0c, t1c, ftc);
    end
    reg        v6;
    reg [9:0]  x6;
    reg [10:0] s06, s16, t06, t16;
    reg [4:0]  fs6, ft6;
    reg [31:0] shade6;
    reg [15:0] depth6;
    always @(posedge clk) begin
        v6 <= v5 & ~rst; x6 <= x5; k6 <= k5; c6 <= c5;
        s06 <= s0c; s16 <= s1c; t06 <= t0c; t16 <= t1c; fs6 <= fsc; ft6 <= ftc;
        shade6 <= shade5; depth6 <= depth5;
    end

    // ======================================================================
    // P7: TMEM addresses: bank (t&1)*2 + (s&1), tmem + (t>>1)*line*2 + (s>>1)
    // ======================================================================
    wire [19:0] row0 = t06[10:1] * t_line;
    wire [19:0] row1 = t16[10:1] * t_line;
    wire [9:0]  aA = t_tmem + {row0[8:0], 1'b0} + s06[10:1];   // (s0, t0)
    wire [9:0]  aB = t_tmem + {row0[8:0], 1'b0} + s16[10:1];   // (s1, t0)
    wire [9:0]  aC = t_tmem + {row1[8:0], 1'b0} + s06[10:1];   // (s0, t1)
    wire [9:0]  aD = t_tmem + {row1[8:0], 1'b0} + s16[10:1];   // (s1, t1)
    wire [1:0]  bA = {t06[0], s06[0]}, bB = {t06[0], s16[0]};
    wire [1:0]  bC = {t16[0], s06[0]}, bD = {t16[0], s16[0]};
    // each bank reads for whichever texel lands on it (two that share a
    // bank always share the address: equal coordinates)
    function automatic [9:0] bank_addr;
        input [1:0] b;
        bank_addr = (bA == b) ? aA : (bB == b) ? aB : (bC == b) ? aC : aD;
    endfunction
    reg        v7;
    reg [9:0]  x7;
    reg [1:0]  bA7, bB7, bC7, bD7;
    reg [4:0]  fs7, ft7;
    reg [31:0] shade7;
    reg [15:0] depth7;
    always @(posedge clk) begin
        v7 <= v6 & ~rst; x7 <= x6; k7 <= k6; c7 <= c6;
        tm_a0 <= bank_addr(2'd0); tm_a1 <= bank_addr(2'd1);
        tm_a2 <= bank_addr(2'd2); tm_a3 <= bank_addr(2'd3);
        bA7 <= bA; bB7 <= bB; bC7 <= bC; bD7 <= bD;
        fs7 <= fs6; ft7 <= ft6;
        shade7 <= shade6; depth7 <= depth6;
    end

    // ======================================================================
    // P8: texel data (TMEM read latency), expand
    // ======================================================================
    function automatic [15:0] bank_data;
        input [1:0] b;
        case (b)
            2'd0: bank_data = tm_d0;
            2'd1: bank_data = tm_d1;
            2'd2: bank_data = tm_d2;
            default: bank_data = tm_d3;
        endcase
    endfunction
    // (P8 is the TMEM read: the data arrive with the stage after it)
    reg        v8;
    reg [9:0]  x8;
    reg [1:0]  bA8, bB8, bC8, bD8;
    reg [4:0]  fs8, ft8;
    reg [31:0] shade8;
    reg [15:0] depth8;
    always @(posedge clk) begin
        v8 <= v7 & ~rst; x8 <= x7; k8 <= k7; c8 <= c7;
        bA8 <= bA7; bB8 <= bB7; bC8 <= bC7; bD8 <= bD7;
        fs8 <= fs7; ft8 <= ft7;
        shade8 <= shade7; depth8 <= depth7;
    end
    wire [31:0] ta8 = texel(t_fmt, bank_data(bA8)), tb8 = texel(t_fmt, bank_data(bB8));
    wire [31:0] tc8 = texel(t_fmt, bank_data(bC8)), td8 = texel(t_fmt, bank_data(bD8));
    wire upper8 = ({1'b0, fs8} + {1'b0, ft8}) < 6'd32;

    // P8b: the expanded texels, registered -- straight off the M10Ks through
    // the bank mux and the format expand into the filter's multipliers was
    // 4 ns over the clock (first MRDP fit)
    reg        v8b;
    reg [9:0]  x8b;
    reg [31:0] ta8b, tb8b, tc8b, td8b;
    reg [4:0]  fs8b, ft8b;
    reg        upper8b;
    reg [31:0] shade8b;
    reg [15:0] depth8b;
    always @(posedge clk) begin
        v8b <= v8 & ~rst; x8b <= x8; k8b <= k8; c8b <= c8;
        ta8b <= ta8; tb8b <= tb8; tc8b <= tc8; td8b <= td8;
        fs8b <= fs8; ft8b <= ft8; upper8b <= upper8;
        shade8b <= shade8; depth8b <= depth8;
    end

    // ======================================================================
    // P9: N64 3-point filter
    // ======================================================================
    function automatic [7:0] tri3;
        input [7:0] a, b, c, d;
        input [4:0] fs, ft;
        input upper;
        reg signed [9:0]  e1, e2;
        reg signed [16:0] acc;
        reg [5:0] ws, wt;
        begin
            if (upper) begin
                e1 = $signed({2'b0, b}) - $signed({2'b0, a});
                e2 = $signed({2'b0, c}) - $signed({2'b0, a});
                ws = {1'b0, fs}; wt = {1'b0, ft};
            end else begin
                e1 = $signed({2'b0, c}) - $signed({2'b0, d});
                e2 = $signed({2'b0, b}) - $signed({2'b0, d});
                ws = 6'd32 - {1'b0, fs}; wt = 6'd32 - {1'b0, ft};
            end
            acc = e1 * $signed({1'b0, ws}) + e2 * $signed({1'b0, wt}) + 17'sd16;
            tri3 = clamp8($signed({12'd0, upper ? a : d}) + (acc >>> 5));
        end
    endfunction
    wire [31:0] filt9 = { tri3(ta8b[31:24], tb8b[31:24], tc8b[31:24], td8b[31:24], fs8b, ft8b, upper8b),
                          tri3(ta8b[23:16], tb8b[23:16], tc8b[23:16], td8b[23:16], fs8b, ft8b, upper8b),
                          tri3(ta8b[15:8],  tb8b[15:8],  tc8b[15:8],  td8b[15:8],  fs8b, ft8b, upper8b),
                          tri3(ta8b[7:0],   tb8b[7:0],   tc8b[7:0],   td8b[7:0],   fs8b, ft8b, upper8b) };
    reg        v9;
    reg [9:0]  x9;
    // a flip-flop, not the head of a RAM delay line: Quartus folded it into
    // one (altshift_taps) and the filter then had to meet the M10K's input
    // setup -- -0.26 ns in the 1-cycle-only build (2026-09-27)
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *) reg [31:0] tex9;
    reg [31:0] shade9;
    reg [15:0] depth9;
    always @(posedge clk) begin
        v9 <= v8b & ~rst; x9 <= x8b; k9 <= k8b; c9 <= c8b;
        lb_raddr <= x8b[9:1];           // line buffers: data after the next edge, captured at P10b
        if (!p_tex)
            tex9 <= 32'd0;
        else if (point)
            tex9 <= ta8b;
        else
            tex9 <= filt9;
        shade9 <= shade8b; depth9 <= depth8b;
    end

    // ======================================================================
    // combiner selections (N64 SetCombine)
    // ======================================================================
    function automatic [7:0] ch;           // byte ch of an RGBA word: 0 r, 1 g, 2 b, 3 a
        input [31:0] c;
        input [1:0]  i;
        ch = c[31 - 8*i -: 8];
    endfunction
    function automatic [7:0] rgb_a;        // sub A (and the RGB core of B, C, D)
        input [3:0]  sel;
        input [1:0]  i;
        input [31:0] tex, sh, cm;
        case (sel)
            4'd0: rgb_a = ch(cm, i);
            4'd1, 4'd2: rgb_a = ch(tex, i);
            4'd3: rgb_a = ch(prim, i);
            4'd4: rgb_a = ch(sh, i);
            4'd5: rgb_a = ch(env, i);
            4'd6: rgb_a = 8'd255;
            default: rgb_a = 8'd0;
        endcase
    endfunction
    function automatic [7:0] rgb_b;
        input [3:0]  sel;
        input [1:0]  i;
        input [31:0] tex, sh, cm;
        rgb_b = (sel >= 4'd6) ? 8'd0 : rgb_a(sel, i, tex, sh, cm);
    endfunction
    function automatic [7:0] rgb_c;
        input [4:0]  sel;
        input [1:0]  i;
        input [31:0] tex, sh, cm;
        case (sel)
            5'd0, 5'd1, 5'd2, 5'd3, 5'd4, 5'd5: rgb_c = rgb_a(sel[3:0], i, tex, sh, cm);
            5'd7:  rgb_c = cm[7:0];
            5'd8, 5'd9: rgb_c = tex[7:0];
            5'd10: rgb_c = prim[7:0];
            5'd11: rgb_c = sh[7:0];
            5'd12: rgb_c = env[7:0];
            5'd14: rgb_c = prim_lod_frac;
            default: rgb_c = 8'd0;
        endcase
    endfunction
    function automatic [7:0] rgb_d;
        input [2:0]  sel;
        input [1:0]  i;
        input [31:0] tex, sh, cm;
        rgb_d = (sel == 3'd7) ? 8'd0 : rgb_a({1'b0, sel}, i, tex, sh, cm);
    endfunction
    function automatic [7:0] al_abd;
        input [2:0]  sel;
        input [31:0] tex, sh, cm;
        case (sel)
            3'd0: al_abd = cm[7:0];
            3'd1, 3'd2: al_abd = tex[7:0];
            3'd3: al_abd = prim[7:0];
            3'd4: al_abd = sh[7:0];
            3'd5: al_abd = env[7:0];
            3'd6: al_abd = 8'd255;
            default: al_abd = 8'd0;
        endcase
    endfunction
    function automatic [7:0] al_c;
        input [2:0]  sel;
        input [31:0] tex, sh;
        case (sel)
            3'd1, 3'd2: al_c = tex[7:0];
            3'd3: al_c = prim[7:0];
            3'd4: al_c = sh[7:0];
            3'd5: al_c = env[7:0];
            3'd6: al_c = prim_lod_frac;
            default: al_c = 8'd0;
        endcase
    endfunction

    // (functions are evaluated in continuous assignments and only their
    // results registered: Verilator 5.020 mis-evaluated the nested automatic
    // functions inside the clocked blocks -- comb11 came out with RGB 0)
    //
    // Each combiner and blender cycle is two stages: the operand muxes, then
    // the arithmetic. As one stage the path from the mode registers through
    // the muxes, the multiplier and the clamp missed the clock by ~1 ns.

    // one combiner cycle's operands {A, B, C, D}, each RGBA (R in [31:24])
    function automatic [127:0] cc_sel;
        input        cy;              // 0: cycle 0 settings, 1: cycle 1
        input [31:0] tex, sh, cm;
        reg [3:0] sa_r, sb_r;
        reg [4:0] mul_r;
        reg [2:0] sa_a, mul_a, sb_a, add_a, add_r;
        begin
            if (!cy) begin
                sa_r = cc_hi[23:20]; mul_r = cc_hi[19:15]; sa_a = cc_hi[14:12]; mul_a = cc_hi[11:9];
                sb_r = cc_lo[31:28]; add_r = cc_lo[17:15]; sb_a = cc_lo[14:12]; add_a = cc_lo[11:9];
            end else begin
                sa_r = cc_hi[8:5];   mul_r = cc_hi[4:0];   sa_a = cc_lo[23:21]; mul_a = cc_lo[20:18];
                sb_r = cc_lo[27:24]; add_r = cc_lo[8:6];   sb_a = cc_lo[5:3];   add_a = cc_lo[2:0];
            end
            cc_sel = {
                rgb_a(sa_r, 2'd0, tex, sh, cm), rgb_a(sa_r, 2'd1, tex, sh, cm), rgb_a(sa_r, 2'd2, tex, sh, cm), al_abd(sa_a, tex, sh, cm),
                rgb_b(sb_r, 2'd0, tex, sh, cm), rgb_b(sb_r, 2'd1, tex, sh, cm), rgb_b(sb_r, 2'd2, tex, sh, cm), al_abd(sb_a, tex, sh, cm),
                rgb_c(mul_r, 2'd0, tex, sh, cm), rgb_c(mul_r, 2'd1, tex, sh, cm), rgb_c(mul_r, 2'd2, tex, sh, cm), al_c(mul_a, tex, sh),
                rgb_d(add_r, 2'd0, tex, sh, cm), rgb_d(add_r, 2'd1, tex, sh, cm), rgb_d(add_r, 2'd2, tex, sh, cm), al_abd(add_a, tex, sh, cm) };
        end
    endfunction
    function automatic [31:0] cc_arith;
        input [127:0] o;
        cc_arith = { cc_eval(o[127:120], o[95:88], o[63:56], o[31:24]),
                     cc_eval(o[119:112], o[87:80], o[55:48], o[23:16]),
                     cc_eval(o[111:104], o[79:72], o[47:40], o[15:8]),
                     cc_eval(o[103:96],  o[71:64], o[39:32], o[7:0]) };
    endfunction

    // ======================================================================
    // P10a/b: combiner cycle 0; P11a/b: cycle 1 (COMBINED = cycle 0 in
    // 2-cycle, 0 in 1-cycle); copy mode takes the texel. The line-buffer
    // words addressed at P9 are captured at P10b.
    // ======================================================================
    wire [127:0] o0_w = cc_sel(1'b0, tex9, shade9, 32'd0);
    reg         v10a;
    reg [9:0]   x10a;
    reg [127:0] o10a;
    reg [31:0]  tex10a, shade10a;
    reg [15:0]  depth10a;
    always @(posedge clk) begin
        v10a <= v9 & ~rst; x10a <= x9; k10a <= k9; c10a <= c9;
        o10a <= o0_w;
        tex10a <= tex9; shade10a <= shade9; depth10a <= depth9;
    end
    wire [31:0] c0_w = cc_arith(o10a);
    reg        v10;
    reg [9:0]  x10;
    reg [31:0] c010, tex10, shade10, mem10;
    reg [15:0] depth10, zold10;
    always @(posedge clk) begin
        v10 <= v10a & ~rst; x10 <= x10a; k10 <= k10a; c10 <= c10a;
        c010 <= c0_w;
        tex10 <= tex10a; shade10 <= shade10a; depth10 <= depth10a;
        zold10 <= x10a[0] ? zlb_rdata[31:16] : zlb_rdata[15:0];
        mem10 <= un565(x10a[0] ? clb_rdata[31:16] : clb_rdata[15:0]);
    end
`ifdef MRDP_NO_2CYCLE
    // (1-cycle only: 2-cycle mode draws as 1-cycle; cycle 0's combiner and
    // cycle 1's blender, the hardware only 2-cycle uses, are pruned)
    wire [31:0]  cm10 = 32'd0;
`else
    wire [31:0]  cm10 = (cyc == 2'd1) ? c010 : 32'd0;   // COMBINED for cycle 1
`endif
    wire [127:0] o1_w = cc_sel(1'b1, tex10, shade10, cm10);
    reg         v11a;
    reg [9:0]   x11a;
    reg [127:0] o11a;
    reg [31:0]  tex11a, shade11a, mem11a;
    reg [15:0]  depth11a, zold11a;
    always @(posedge clk) begin
        v11a <= v10 & ~rst; x11a <= x10; k11a <= k10; c11a <= c10;
        o11a <= o1_w;
        tex11a <= tex10; shade11a <= shade10; mem11a <= mem10;
        depth11a <= depth10; zold11a <= zold10;
    end
    wire [31:0] c1_w = cc_arith(o11a);
    reg        v11;
    reg [9:0]  x11;
    reg [31:0] comb11, shade11;
    reg [15:0] depth11, zold11;
    reg [31:0] mem11;
    always @(posedge clk) begin
        v11 <= v11a & ~rst; x11 <= x11a; k11 <= k11a; c11 <= c11a;
        comb11 <= (cyc == 2'd2) ? tex11a : c1_w;
        shade11 <= shade11a; depth11 <= depth11a;
        zold11 <= zold11a; mem11 <= mem11a;
    end

    // ======================================================================
    // blender: (P * a + M * b) >> 8
    // ======================================================================
    function automatic [31:0] bl_pm;
        input [1:0]  sel;
        input [31:0] pix, mem;
        case (sel)
            2'd0: bl_pm = pix;
            2'd1: bl_pm = mem;
            2'd2: bl_pm = blend;
            default: bl_pm = fog;
        endcase
    endfunction
    function automatic [7:0] bl_ch;
        input [7:0] p, m;
        input [8:0] aw, bw;
        reg [17:0] s;
        begin
            s = p * aw + m * bw;
            bl_ch = (s[17:8] > 10'd255) ? 8'd255 : s[15:8];
        end
    endfunction
    // one blender cycle's operands {P rgb, M rgb, a weight, b weight}
    function automatic [65:0] bl_sel;
        input        cy;
        input [31:0] pix, mem;
        input [7:0]  comb_a, shade_a;
        reg [1:0] p, a, m, b;
        reg [7:0] av;
        reg [8:0] aw, bw;
        reg [31:0] P, M;
        begin
            p = cy ? other_l[29:28] : other_l[31:30];
            a = cy ? other_l[25:24] : other_l[27:26];
            m = cy ? other_l[21:20] : other_l[23:22];
            b = cy ? other_l[17:16] : other_l[19:18];
            case (a)
                2'd0: av = comb_a;
                2'd1: av = fog[7:0];
                2'd2: av = shade_a;
                default: av = 8'd0;
            endcase
            aw = wgt(av);
            case (b)
                2'd0: bw = 9'd256 - aw;
                2'd1, 2'd2: bw = 9'd256;
                default: bw = 9'd0;
            endcase
            P = bl_pm(p, pix, mem);
            M = bl_pm(m, pix, mem);
            bl_sel = { P[31:8], M[31:8], aw, bw };
        end
    endfunction
    function automatic [23:0] bl_arith;
        input [65:0] o;
        bl_arith = { bl_ch(o[65:58], o[41:34], o[17:9], o[8:0]),
                     bl_ch(o[57:50], o[33:26], o[17:9], o[8:0]),
                     bl_ch(o[49:42], o[25:18], o[17:9], o[8:0]) };
    endfunction

    // ======================================================================
    // P12a: tests, blender cycle 0 operands; P12b: its result
    // ======================================================================
    // AA_EN, a partly covered pixel within dz of the stored depth (an edge
    // two surfaces share): the one covering more of the pixel keeps it,
    // whichever is nearer (lang/c/mrdp/mrdp.c draw_pixel; the N64 blends by
    // stored coverage there). dz = |dZ/dx >> 15| + |dZ/dy >> 15| + 1, the
    // triangle's, registered from the quasi-static gradients.
`ifdef MRDP_COVERAGE
    wire signed [16:0] dzx15 = dzdx[31:15], dzy15 = dzdy[31:15];
    wire [17:0] dz_sum = {1'b0, dzx15[16] ? -dzx15 : dzx15} + {1'b0, dzy15[16] ? -dzy15 : dzy15} + 18'd1;
    reg  [15:0] dz_r;
    always @(posedge clk) dz_r <= (dz_sum > 18'hFFFF) ? 16'hFFFF : dz_sum[15:0];
    wire signed [16:0] zdiff11 = {1'b0, depth11} - {1'b0, zold11};
    wire [16:0] zdabs11 = zdiff11[16] ? -zdiff11 : zdiff11;
    wire close11 = c11[1] && (zdabs11 <= {1'b0, dz_r});
    wire zpass11 = !z_cmp || (decal ? (depth11 <= zold11) : close11 ? !c11[0] : (depth11 < zold11));
`else
    wire zpass11 = !z_cmp || (decal ? (depth11 <= zold11) : (depth11 < zold11));
`endif
    wire arej11  = (a_cmp && comb11[7:0] < blend[7:0]) || (cvg_x_a && comb11[7:0] < 8'd32);
    wire [65:0] bo0_w = bl_sel(1'b0, comb11, mem11, comb11[7:0], shade11[7:0]);
    wire [31:0] pm0_w = bl_pm(other_l[31:30], comb11, mem11);
    reg        v12a;
    reg [9:0]  x12a;
    reg [65:0] bo12a;
    reg [31:0] comb12a, mem12a, pm012a;
    reg [7:0]  shade_a12a;
    reg [15:0] depth12a;
    reg        pass12a;
    always @(posedge clk) begin
        v12a <= v11 & ~rst; x12a <= x11; k12a <= k11;
        bo12a <= bo0_w; pm012a <= pm0_w;
        comb12a <= comb11; mem12a <= mem11; shade_a12a <= shade11[7:0];
        depth12a <= depth11;
        pass12a <= (cyc == 2'd3) ? 1'b1 : (zpass11 && !arej11);
    end
    wire [23:0] b0_w = bl_arith(bo12a);
    reg        v12;
    reg [9:0]  x12;
    reg [31:0] b012, comb12, mem12, pm012;
    reg [7:0]  shade_a12;
    reg [15:0] depth12;
    reg        pass12;
    always @(posedge clk) begin
        v12 <= v12a & ~rst; x12 <= x12a; k12 <= k12a;
        b012 <= { b0_w, comb12a[7:0] };
        comb12 <= comb12a; mem12 <= mem12a; pm012 <= pm012a; shade_a12 <= shade_a12a;
        depth12 <= depth12a; pass12 <= pass12a;
    end

    // ======================================================================
    // P13a: blender cycle 1 operands; P13b: final color
    // ======================================================================
    wire [65:0] bo1_w = bl_sel(1'b1, b012, mem12, comb12[7:0], shade_a12);
    wire [31:0] pm1_w = bl_pm(other_l[29:28], b012, mem12);
    reg        v13a;
    reg [9:0]  x13a;
    reg [65:0] bo13a;
    reg [31:0] b013, comb13, pm013, pm113;
    reg [15:0] depth13;
    reg        pass13;
    always @(posedge clk) begin
        v13a <= v12 & ~rst; x13a <= x12; k13a <= k12;
        bo13a <= bo1_w; pm113 <= pm1_w;
        b013 <= b012; comb13 <= comb12; pm013 <= pm012;
        depth13 <= depth12; pass13 <= pass12;
    end
    wire [23:0] b1_w = bl_arith(bo13a);
`ifdef MRDP_NO_2CYCLE
    wire [31:0] fin = (cyc == 2'd3) ? 32'd0 : (cyc == 2'd2) ? comb13
                    : (force_bl ? b013 : pm013);
`else
    wire [31:0] fin = (cyc == 2'd3) ? 32'd0 : (cyc == 2'd2) ? comb13
                    : (cyc == 2'd1) ? (force_bl ? {b1_w, 8'd0} : pm113)
                    : (force_bl ? b013 : pm013);
`endif
    always @(posedge clk) begin
        out_valid <= v13a & ~rst;
        out_x <= x13a;
        out_color <= (cyc == 2'd3) ? (x13a[0] ? fill[15:0] : fill[31:16])
                                   : { fin[31:27], fin[23:18], fin[15:11] };
        out_depth <= depth13;
        out_cpass <= pass13 && !k13a;
        out_zpass <= pass13 && !k13a && z_upd && (cyc != 2'd3);
    end

`ifdef MRDP_DEBUG
    integer dbg_p = 0;
    always @(posedge clk) begin
        if (v11 && dbg_p < 4) begin
            $display("PIX11 x=%0d shade11=%08x comb11=%08x cyc=%0d", x11, shade11, comb11, cyc);
            dbg_p = dbg_p + 1;
        end
    end
`endif
    assign busy = v0 | v1 | v2 | v3 | v4 | v5 | v6 | v7 | v8 | v8b | v9 | v10a | v10 | v11a | v11 | v12a | v12 | v13a | out_valid;
endmodule
