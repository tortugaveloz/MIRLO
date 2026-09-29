// Audio DSP custom function unit for the audio core (VexRiscvAudio's
// CfuPlugin): the two hot loops of the N64 audio ucode, as instructions.
// lang/c/audio/audio_cfu.h has the C model and the intrinsics;
// sim/audio (build_cfu.sh, tb_cfu) checks this RTL against it op by op.
//
// function_id (custom-0, funct3 = 0, funct7 = id):
//  envelope mixer -- the lane volumes live in here for a whole aEnvMixer:
//   0x01 ENV_CFG_A  in0 = rate (0..0x1FFFF) | channel << 31, in1 = target (0..32767)
//   0x02 ENV_CFG_B  in0 = vol_dry           in1 = vol_wet   (int16)
//   0x03 ENV_VSET   in0 = lane (channel * 8 + i), in1 = its volume (0..2^31-1)
//   0x04 ENV_VGET   in0 = lane                   -> its volume
//   0x05 ENV_START  in0 = aux (1: dry and wet sends, 0: dry only); cursor to
//                   channel 0, lanes 0-1, dry
//   0x06 ENV_MIX2   in0 = two out samples {o1, o0}, in1 = two in samples
//                   {x1, x0} for the cursor's channel, lane pair and send:
//                     v_c = the lane's volume clamped to the channel's target
//                           (up if rate >= 1.0, else down)
//                     g   = ((v_c >> 16) * vol_send + 0x4000) >> 15
//                     o'  = clamp16((o * 0x7fff + x * g + 0x4000) >> 15)
//                   after the last send of a pair each lane's volume becomes
//                   min(v_c * rate >> 16, 0x7fffffff); the cursor then moves
//                   on (send, pair, channel -- the audio ucode's loop order)  -> {o1', o0'}
//  resampler:
//   0x08 RS_CFG     in0 = step (pitch << 1, 0..0x1FFFF)  in1 = pitch accumulator
//   0x09 RS_ACC     -> the pitch accumulator
//   0x0A RS_OUT     in0 = {s1, s0}, in1 = {s3, s2}   (int16 pairs)
//                   t = resample_table[acc >> 10]
//                   y = clamp16(sum_j (s_j * t_j + 0x4000) >> 15)
//                   acc += step; adv = acc >> 16; acc &= 0xffff  -> {adv, y}
//  VADPCM decoder -- the codebook lives in here, on RS_OUT's multipliers:
//   0x07 ADP_TLOAD  in0 = {c1, c0}, in1 = {c3, c2}: four codebook entries
//                   (int16), word tptr of the flat [8][2][8] table; tptr++
//   0x0E ADP_TRST   tptr = in0[4:0]
//   0x0B ADP_START  in0 = {out[-1], out[-2]}: the decoder's history
//   0x0C ADP_GRP    in0 = the frame header byte (shift << 4 | table index,
//                   index 0..7), in1 = the 4 data bytes of 8 samples, first
//                   byte in [31:24] -> {out1, out0} of those 8
//   0x0D ADP_NXT    -> {out3, out2}, then {out5, out4}, then {out7, out6};
//                   out7/out6 become the history of the next ADP_GRP
//                   out_j = clamp16((c0[j] * out[-2] + (x_j << 11)
//                              + sum_{m<=j} c1[m] * x_{j-1-m}) >> 11)
//                   with x_-1 = out[-1] and 32-bit wrapping sums, as the C model
//   0x0F ID         -> 0x41445333 ("ADS3")
// An op answers four cycles after it is accepted (operands, products, second
// products, result); ADP_GRP/ADP_NXT take six (outputs 0-3) or eight
// (4-7). Multipliers are separate (DSP blocks), not shared through muxes:
// the device is short of ALMs, not DSPs. The ADPCM steps reuse RS_OUT's four
// by loading their operands into a1/b1 and their coefficients through the
// same RAM read port, so they add no muxes in front of them.
`default_nettype none

module AudioDspCfu (
    input  wire        clk,
    input  wire        rst,
    input  wire        cmd_valid,
    output wire        cmd_ready,
    input  wire [9:0]  cmd_function_id,
    input  wire [31:0] cmd_inputs_0,
    input  wire [31:0] cmd_inputs_1,
    output reg         rsp_valid,
    input  wire        rsp_ready,
    output reg  [31:0] rsp_outputs_0
);
    // ---- envelope-mixer state
    reg [16:0] rate0, rate1;
    reg [15:0] tgt0, tgt1;
    reg signed [15:0] vol_d, vol_w;
    reg        aux;
    reg        cc, ss;                // cursor: channel, send
    reg [1:0]  pp;                    //         lane pair
    // lane volumes: even lanes and odd lanes, indexed {channel, pair}
    (* ramstyle = "MLAB, no_rw_check" *) reg [30:0] v_even [0:7];
    (* ramstyle = "MLAB, no_rw_check" *) reg [30:0] v_odd  [0:7];
    reg [30:0] rd_even, rd_odd;

    // ---- resampler state
    reg [16:0] step;
    reg [15:0] acc;

    // ---- pipeline
    reg        s1, s2, s3;
    reg [3:0]  op1, op2, op3;
    reg [31:0] a1, b1;                // held until the next accept (one op in flight)
    reg        c1, snd1, last1;       // the MIX2 op's cursor snapshot
    reg [2:0]  wa1, wa2;              // its volume RAM address
    reg        last2, lane1_odd;
    reg [30:0] vget_q;                // ENV_VGET's value, captured in stage 1
    // ---- ADPCM state
    reg signed [15:0] prev1, prev2;   // out[-1], out[-2] of the next group
    reg [31:0] nibs;
    reg [3:0]  ashift;
    reg [2:0]  tidx;
    reg [15:0] h0, h1, h2, h3, h4, h5, h6, h7;   // x_{j-1}, x_{j-2}, ...
    reg [2:0]  aj;                    // next output to issue
    reg        aph;                   // its step: 0 = c1[0..3] (+ c0, x << 11), 1 = c1[4..7]
    reg        aissue, abusy;
    reg [4:0]  tptr;

    assign cmd_ready = !s1 && !s2 && !s3 && !rsp_valid && !abusy;
    wire fire = cmd_valid && cmd_ready;
    wire [3:0] fid = cmd_function_id[3:0];
    wire adp_run = (fid == 4'hC) || (fid == 4'hD);

    // volume RAMs: one write port each (VSET at accept, or MIX2's ramp in
    // stage 2 -- never in the same cycle), so they map to MLABs
    wire       vset   = fire && (fid == 4'h3);
    wire       vramp  = s2 && (op2 == 4'h6) && last2;
    wire [2:0] waddr  = vset ? {cmd_inputs_0[3], cmd_inputs_0[2:1]} : wa2;
    wire       we_even = vramp || (vset && !cmd_inputs_0[0]);
    wire       we_odd  = vramp || (vset &&  cmd_inputs_0[0]);
    wire [30:0] wd_even = vset ? cmd_inputs_1[30:0] : vn0;
    wire [30:0] wd_odd  = vset ? cmd_inputs_1[30:0] : vn1;
    always @(posedge clk) begin
        if (we_even) v_even[waddr] <= wd_even;
        if (we_odd)  v_odd[waddr]  <= wd_odd;
    end

    // volume RAM: read every cycle (the MIX2 cursor, or VGET's lane)
    wire [2:0] raddr = (fid == 4'h4) ? {cmd_inputs_0[3], cmd_inputs_0[2:1]} : {cc, pp};
    always @(posedge clk) begin
        rd_even <= v_even[raddr];
        rd_odd  <= v_odd[raddr];
    end

    // ---- stage 1: clamp to the target, first products
    wire [16:0] rate_c = c1 ? rate1 : rate0;
    wire [15:0] tgt_c  = c1 ? tgt1 : tgt0;
    wire        inc    = rate_c[16];
    function [30:0] vclamp(input [30:0] v, input [15:0] t, input up);
        reg [15:0] hi;
        begin
            hi = {1'b0, v[30:16]};
            vclamp = (up ? (hi > t) : (hi < t)) ? {t[14:0], 16'd0} : v;
        end
    endfunction
    wire [30:0] vc0 = vclamp(rd_even, tgt_c, inc);
    wire [30:0] vc1 = vclamp(rd_odd,  tgt_c, inc);
    wire signed [15:0] vol_s = snd1 ? vol_w : vol_d;

    // Coefficients for the four RS_OUT multipliers, in block RAM (2 M10K):
    // 0..63 the resampler table (constant), 64..79 the ADPCM codebook's
    // second rows ({table index, half}: c1[4h..4h+3]), written by ADP_TLOAD.
    // Read every cycle: at the pitch accumulator, or at the ADPCM step being
    // issued. acc only changes on RS_CFG or at the end of an RS_OUT, and the
    // next op is always accepted cycles later, so the registered read is
    // never stale.
    (* ramstyle = "M10K, no_rw_check" *) reg [63:0] rs_rom [0:127];
    initial begin
        rs_rom[0] = 64'hffdf0d4666ad0c39;
        rs_rom[1] = 64'hffd80e5f66960b39;
        rs_rom[2] = 64'hffd00f8366690a44;
        rs_rom[3] = 64'hffc810b46626095a;
        rs_rom[4] = 64'hffbf11f065cd087d;
        rs_rom[5] = 64'hffb61338655e07ab;
        rs_rom[6] = 64'hffac148c64d906e4;
        rs_rom[7] = 64'hffa115eb643f0628;
        rs_rom[8] = 64'hff961756638f0577;
        rs_rom[9] = 64'hff8a18cb62cb04d1;
        rs_rom[10] = 64'hff7e1a4c61f30435;
        rs_rom[11] = 64'hff711bd7610603a4;
        rs_rom[12] = 64'hff641d6c6007031c;
        rs_rom[13] = 64'hff561f0b5ef5029f;
        rs_rom[14] = 64'hff4820b35dd0022a;
        rs_rom[15] = 64'hff3a22645c9a01be;
        rs_rom[16] = 64'hff2c241e5b53015b;
        rs_rom[17] = 64'hff1e25e059fc0101;
        rs_rom[18] = 64'hff1027a9589600ae;
        rs_rom[19] = 64'hff02297a57200063;
        rs_rom[20] = 64'hfef42b50559d001f;
        rs_rom[21] = 64'hfee82d2c540dffe2;
        rs_rom[22] = 64'hfedb2f0d5270ffac;
        rs_rom[23] = 64'hfed030f350c7ff7c;
        rs_rom[24] = 64'hfec632dc4f14ff53;
        rs_rom[25] = 64'hfebd34c84d57ff2e;
        rs_rom[26] = 64'hfeb636b64b91ff0f;
        rs_rom[27] = 64'hfeb038a549c2fef5;
        rs_rom[28] = 64'hfeac3a9547edfedf;
        rs_rom[29] = 64'hfeab3c854611fece;
        rs_rom[30] = 64'hfeac3e744430fec0;
        rs_rom[31] = 64'hfeaf4060424afeb6;
        rs_rom[32] = 64'hfeb6424a4060feaf;
        rs_rom[33] = 64'hfec044303e74feac;
        rs_rom[34] = 64'hfece46113c85feab;
        rs_rom[35] = 64'hfedf47ed3a95feac;
        rs_rom[36] = 64'hfef549c238a5feb0;
        rs_rom[37] = 64'hff0f4b9136b6feb6;
        rs_rom[38] = 64'hff2e4d5734c8febd;
        rs_rom[39] = 64'hff534f1432dcfec6;
        rs_rom[40] = 64'hff7c50c730f3fed0;
        rs_rom[41] = 64'hffac52702f0dfedb;
        rs_rom[42] = 64'hffe2540d2d2cfee8;
        rs_rom[43] = 64'h001f559d2b50fef4;
        rs_rom[44] = 64'h00635720297aff02;
        rs_rom[45] = 64'h00ae589627a9ff10;
        rs_rom[46] = 64'h010159fc25e0ff1e;
        rs_rom[47] = 64'h015b5b53241eff2c;
        rs_rom[48] = 64'h01be5c9a2264ff3a;
        rs_rom[49] = 64'h022a5dd020b3ff48;
        rs_rom[50] = 64'h029f5ef51f0bff56;
        rs_rom[51] = 64'h031c60071d6cff64;
        rs_rom[52] = 64'h03a461061bd7ff71;
        rs_rom[53] = 64'h043561f31a4cff7e;
        rs_rom[54] = 64'h04d162cb18cbff8a;
        rs_rom[55] = 64'h0577638f1756ff96;
        rs_rom[56] = 64'h0628643f15ebffa1;
        rs_rom[57] = 64'h06e464d9148cffac;
        rs_rom[58] = 64'h07ab655e1338ffb6;
        rs_rom[59] = 64'h087d65cd11f0ffbf;
        rs_rom[60] = 64'h095a662610b4ffc8;
        rs_rom[61] = 64'h0a4466690f83ffd0;
        rs_rom[62] = 64'h0b3966960e5fffd8;
        rs_rom[63] = 64'h0c3966ad0d46ffdf;
    end
    reg [63:0] rs_coef;
    wire tload = fire && (fid == 4'h7);
    always @(posedge clk) begin
        if (tload && tptr[1]) rs_rom[{3'b100, tptr[4:2], tptr[0]}] <= {cmd_inputs_1, cmd_inputs_0};
        rs_coef <= rs_rom[aissue ? {3'b100, tidx, aph} : {1'b0, acc[15:10]}];
    end

    // the codebook's first rows: {table index, half} -> c0[4h..4h+3]
    (* ramstyle = "M10K, no_rw_check" *) reg [63:0] c0_ram [0:15];
    reg [63:0] c0_rd;
    always @(posedge clk) begin
        if (tload && !tptr[1]) c0_ram[{tptr[4:2], tptr[0]}] <= {cmd_inputs_1, cmd_inputs_0};
        c0_rd <= c0_ram[{tidx, aj[2]}];
    end

    // ---- ADPCM issue: one step per cycle, operands into a1/b1 (below)
    wire [3:0]  nib  = nibs[{~aj, 2'b00} +: 4];
    wire [30:0] xw   = {{27{nib[3]}}, nib} << ashift;
    wire [15:0] xj   = xw[15:0];
    wire        alast = aph || !aj[2];            // outputs 0-3 need one step
    reg         q_v, q_first, q_last;             // stage 1 of a step
    reg  [2:0]  q_j;
    reg  signed [15:0] q_m5;
    reg  signed [31:0] q_xt;
    reg         r_v, r_first, r_last;             // stage 2
    reg  [2:0]  r_j;
    reg  signed [31:0] r_xt;
    reg  signed [31:0] p5;                        // c0[j] * out[-2]
    reg  signed [31:0] aacc;
    reg  [15:0] alo;
    wire [15:0] c0sel = c0_rd[{q_j[1:0], 4'b0000} +: 16];
    always @(posedge clk) p5 <= q_m5 * $signed(c0sel);

    reg  signed [31:0] pg0, pg1;          // (v_c >> 16) * vol
    reg  [31:0] ph0, ph1;                 // v_c[30:16] * rate
    reg  [32:0] pl0, pl1;                 // v_c[15:0]  * rate
    reg  signed [31:0] p_rs0, p_rs1, p_rs2, p_rs3;
    always @(posedge clk) begin
        pg0 <= $signed({1'b0, vc0[30:16]}) * vol_s;
        pg1 <= $signed({1'b0, vc1[30:16]}) * vol_s;
        ph0 <= vc0[30:16] * rate_c;
        ph1 <= vc1[30:16] * rate_c;
        pl0 <= vc0[15:0] * rate_c;
        pl1 <= vc1[15:0] * rate_c;
        p_rs0 <= $signed(a1[15:0])  * $signed(rs_coef[15:0]);
        p_rs1 <= $signed(a1[31:16]) * $signed(rs_coef[31:16]);
        p_rs2 <= $signed(b1[15:0])  * $signed(rs_coef[47:32]);
        p_rs3 <= $signed(b1[31:16]) * $signed(rs_coef[63:48]);
    end

    // ---- stage 2: gains, ramp, second products
    wire signed [17:0] g0 = (pg0 + 32'sh4000) >>> 15;
    wire signed [17:0] g1 = (pg1 + 32'sh4000) >>> 15;
    wire [33:0] ramp0 = {2'b0, ph0} + {17'd0, pl0[32:16]};
    wire [33:0] ramp1 = {2'b0, ph1} + {17'd0, pl1[32:16]};
    wire [30:0] vn0 = (ramp0 > 34'h7fffffff) ? 31'h7fffffff : ramp0[30:0];
    wire [30:0] vn1 = (ramp1 > 34'h7fffffff) ? 31'h7fffffff : ramp1[30:0];
    reg  signed [33:0] pm0, pm1;          // x * g
    always @(posedge clk) begin
        pm0 <= $signed(b1[15:0]) * g0;
        pm1 <= $signed(b1[31:16]) * g1;
    end
    wire signed [19:0] r0 = (p_rs0 + 32'sh4000) >>> 15;
    wire signed [19:0] r1 = (p_rs1 + 32'sh4000) >>> 15;
    wire signed [19:0] r2 = (p_rs2 + 32'sh4000) >>> 15;
    wire signed [19:0] r3 = (p_rs3 + 32'sh4000) >>> 15;
    wire signed [19:0] rsum = r0 + r1 + r2 + r3;
    wire [15:0] rs16 = (rsum > 20'sh7fff) ? 16'h7fff : (rsum < -20'sh8000) ? 16'h8000 : rsum[15:0];
    wire [17:0] acc_next = {2'b0, acc} + {1'b0, step};

    // ADPCM stage 2: the step's sum (32-bit, wrapping like the C model's int32)
    // registered, then accumulated and saturated in the next stage: all of it
    // in one cycle did not close timing once the texture filter filled the chip
    wire signed [31:0] asum = p_rs0 + p_rs1 + p_rs2 + p_rs3 + p5 + r_xt;
    reg                t_v, t_first, t_last;
    reg  [2:0]         t_j;
    reg  signed [31:0] t_sum;
    wire signed [31:0] atot = (t_first ? 32'sd0 : aacc) + t_sum;
    wire signed [20:0] ash  = atot >>> 11;
    wire [15:0] ay = (ash > 21'sh7fff) ? 16'h7fff : (ash < -21'sh8000) ? 16'h8000 : ash[15:0];

    // ---- stage 3: the mix
    function [15:0] mix(input signed [15:0] o, input signed [33:0] p);
        reg signed [35:0] t;
        begin
            // the sum first, into the signed t: concatenations are unsigned,
            // and >>> on an unsigned expression is a logical shift
            t = ({{20{o[15]}}, o} <<< 15) - {{20{o[15]}}, o} + {{2{p[33]}}, p} + 36'sh4000;
            t = t >>> 15;
            mix = (t > 36'sh7fff) ? 16'h7fff : (t < -36'sh8000) ? 16'h8000 : t[15:0];
        end
    endfunction

    always @(posedge clk) begin
        if (rst) begin
            s1 <= 1'b0; s2 <= 1'b0; s3 <= 1'b0; rsp_valid <= 1'b0;
            abusy <= 1'b0; aissue <= 1'b0; q_v <= 1'b0; r_v <= 1'b0; t_v <= 1'b0; tptr <= 5'd0;
        end else begin
            if (rsp_valid && rsp_ready) rsp_valid <= 1'b0;
            s1 <= fire && !adp_run; s2 <= s1; s3 <= s2;
            if (fire) begin
                op1 <= fid; a1 <= cmd_inputs_0; b1 <= cmd_inputs_1;
                c1 <= cc; snd1 <= ss; wa1 <= {cc, pp};
                last1 <= !aux || ss;
                case (fid)
                4'h1: if (cmd_inputs_0[31]) begin rate1 <= cmd_inputs_0[16:0]; tgt1 <= cmd_inputs_1[15:0]; end
                      else                  begin rate0 <= cmd_inputs_0[16:0]; tgt0 <= cmd_inputs_1[15:0]; end
                4'h2: begin vol_d <= cmd_inputs_0[15:0]; vol_w <= cmd_inputs_1[15:0]; end
                4'h5: begin aux <= cmd_inputs_0[0]; cc <= 1'b0; pp <= 2'd0; ss <= 1'b0; end
                4'h6: begin                      // cursor: send, then pair, then channel
                    if (aux && !ss) ss <= 1'b1;
                    else begin ss <= 1'b0; pp <= pp + 2'd1; if (pp == 2'd3) cc <= ~cc; end
                end
                4'h8: begin step <= cmd_inputs_0[16:0]; acc <= cmd_inputs_1[15:0]; end
                4'h7: tptr <= tptr + 5'd1;
                4'hE: tptr <= cmd_inputs_0[4:0];
                4'hB: begin prev2 <= cmd_inputs_0[15:0]; prev1 <= cmd_inputs_0[31:16]; end
                4'hC: begin
                    ashift <= cmd_inputs_0[7:4]; tidx <= cmd_inputs_0[2:0]; nibs <= cmd_inputs_1;
                    h0 <= prev1; {h1, h2, h3, h4, h5, h6, h7} <= 112'd0;
                    aj <= 3'd0; aph <= 1'b0; aissue <= 1'b1; abusy <= 1'b1;
                end
                4'hD: begin aph <= 1'b0; aissue <= 1'b1; abusy <= 1'b1; end
                default: ;
                endcase
                lane1_odd <= cmd_inputs_0[0];
            end
            if (s1) begin
                op2 <= op1; wa2 <= wa1; last2 <= last1;
                vget_q <= lane1_odd ? rd_odd : rd_even;   // rd_* is re-read at the cursor after this
            end
            if (s2) begin
                op3 <= op2;
                if (op2 == 4'hA) acc <= acc_next[15:0];
                case (op2)                       // results that are ready in stage 2
                4'h4: rsp_outputs_0 <= {1'b0, vget_q};
                4'h9: rsp_outputs_0 <= {16'd0, acc};
                4'hA: rsp_outputs_0 <= {14'd0, acc_next[17:16], rs16};
                4'hF: rsp_outputs_0 <= 32'h41445333;
                default: rsp_outputs_0 <= 32'd0;
                endcase
            end
            if (s3) begin
                rsp_valid <= 1'b1;
                if (op3 == 4'h6) rsp_outputs_0 <= {mix(a1[31:16], pm1), mix(a1[15:0], pm0)};
            end

            // ---- ADPCM: issue (operands into a1/b1 for RS_OUT's multipliers)
            q_v <= aissue;
            if (aissue) begin
                a1 <= aph ? {h5, h4} : {h1, h0};
                b1 <= aph ? {h7, h6} : {h3, h2};
                q_m5 <= aph ? 16'sd0 : prev2;
                q_xt <= aph ? 32'sd0 : {{5{xj[15]}}, xj, 11'd0};
                q_first <= !aph; q_last <= alast; q_j <= aj;
                if (alast) begin
                    {h7, h6, h5, h4, h3, h2, h1, h0} <= {h6, h5, h4, h3, h2, h1, h0, xj};
                    aj <= aj + 3'd1; aph <= 1'b0;
                    if (aj[0]) aissue <= 1'b0;
                end else
                    aph <= 1'b1;
            end
            // stage 1: products (p_rs*, p5)
            r_v <= q_v; r_first <= q_first; r_last <= q_last; r_j <= q_j; r_xt <= q_xt;
            t_v <= r_v; t_first <= r_first; t_last <= r_last; t_j <= r_j; t_sum <= asum;
            // stage 2: accumulate; an output per last step, a pair per answer
            if (t_v) begin
                aacc <= atot;
                if (t_last) begin
                    if (!t_j[0]) alo <= ay;
                    else begin
                        rsp_outputs_0 <= {ay, alo};
                        rsp_valid <= 1'b1;
                        abusy <= 1'b0;
                        if (t_j == 3'd7) begin prev1 <= ay; prev2 <= alo; end
                    end
                end
            end
        end
    end
endmodule

`default_nettype wire
