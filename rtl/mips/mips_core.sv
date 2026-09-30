// Mirlo-N64's CPU: a MIPS that executes an N64 ROM's code as it is.
//
// The reference is sim/n64/cpu.c; sim/mips checks this against it
// instruction by instruction while both run a ROM.
//
//  - A 32-bit VR4300: MIPS II plus the MIPS III parts that are not 64-bit
//    (the TLB, ERET, ldc1/sdc1 of the FPU's register pairs). The VR4300's
//    64-bit instructions (ld/sd, dadd, dsll, dmult, lwu, ...) are Reserved
//    Instructions: a game's few uses of them -- libultra's thread context
//    save and its long long helpers -- are patched as the ROM is loaded
//    (tools/n64patch.py, lang/n64). Registers are 32 bits.
//  - Five stages: F (I-cache), D (decode, registers, branches -- a delay slot
//    costs nothing), E (ALU), M (D-cache, the slow path, COP0/COP1,
//    exceptions), W (write-back). Forwarding into E from M and W; a branch
//    in D waits for a result still in E, a load's for one cycle.
//  - Caches: 16 KiB I, 8 KiB D, direct mapped, 32-byte lines, physically
//    tagged; KSEG0 RDRAM only. The D-cache writes through an 8-entry write
//    buffer: memory is always current for the engines that read it (the RSP
//    firmware, MRDP, the audio core). A read goes out only with the buffer
//    empty. Uncached, device, TLB-mapped and 64-bit accesses take the slow
//    path (one single-word access; two for ldc1/sdc1).
//  - Memory is word-native (sim/n64/n64.h): big-endian byte k of a word is
//    lane 3 - k (BIG; little-endian: lane k).
//  - COP1 instructions run at M in fpu_n64 (single precision, doubles
//    through float); the operations it does not implement are Reserved
//    Instructions.
`default_nettype none

module mips_core #(
    parameter int TLB_N = 32,               // the VR4300's; in a RAM, so the count costs no logic
    // 0: no TLB -- as if it never held a valid entry: a mapped access takes
    // the refill exception, TLB writes are dropped, tlbp never finds one. For
    // games that map nothing -- NOT SM64: Goddard's title-screen head maps
    // 0x04000000 (n64sim: 200,897 mapped accesses in the first 3 G cycles).
    parameter bit TLB_EN = 1,
    // the cached span of KSEG0 from physical 0, as a power of two: the N64's
    // RDRAM (8 MiB)
    parameter int CACHE_LOG2 = 23,
    // native Mirlo (rtl/soc/mips_sys.sv): no TLB and no KSEG0/KUSEG/KSEG2 --
    // every address is physical but KSEG1's (the boot ROM, 0xBFC0_0000);
    // SDRAM at 0x4000_0000 (64 MiB) is cached, everything else uncached; the
    // exception vector with BEV 0 is EXC_BASE + 0x180. MIRLO's RISC-V map.
    parameter bit FLAT = 0,
    parameter logic [31:0] EXC_BASE = 32'h8000_0000,
    // byte order: 1 big-endian (the N64: byte k of a word is lane 3 - k),
    // 0 little-endian (native Mirlo, as its other cores: lane k)
    parameter bit BIG = 1,
    parameter bit SIM_COUNT_RETIRE = 0      // Count/Random advance per executed instruction (co-simulation)
) (
    input  wire         clk,
    input  wire         rst,
    input  wire  [31:0] reset_pc,
    // memory: physical N64 addresses, word-native data
    output logic        mem_req,
    output logic [31:0] mem_addr,
    output logic        mem_we,
    output logic [31:0] mem_wdata,
    output logic [3:0]  mem_wstrb,
    output logic        mem_line,           // an 8-word line read
    input  wire         mem_ack,            // the request is taken (a write is done)
    input  wire         mem_rvalid,
    input  wire  [31:0] mem_rdata,
    input  wire         irq_rcp,            // Cause.IP2 (the MI)
    // retire trace (co-simulation)
    output logic        tr_retire,
    output logic [31:0] tr_pc,
    output logic [31:0] tr_insn,
    output logic        tr_wr,
    output logic [4:0]  tr_rd,
    output logic [63:0] tr_val,
    output logic        tr_exc,
    output logic        tr_exc_int,
    output logic [31:0] tr_exc_pc,
    output logic [4:0]  tr_exc_code,
    output logic        tr_fwe_even, tr_fwe_odd,    // the FPU registers it wrote (pair tr_fwidx)
    output logic [3:0]  tr_fwidx,
    output logic [31:0] tr_fweven, tr_fwodd,
    output logic [31:0] tr_fcr31,
    // debug (Mirlo-N64's on-screen display)
    output logic [31:0] dbg_pc, dbg_cause, dbg_status, dbg_epc
);

// =============================================================== decode
typedef enum logic [4:0] {
    A_NONE, A_ADD, A_SUB, A_AND, A_OR, A_XOR, A_NOR, A_SLT, A_SLTU, A_LUI, A_SLL, A_SRL, A_SRA, A_LINK,
    A_MFHI, A_MFLO
} alu_t;
typedef enum logic [3:0] {
    M_NONE, M_LB, M_LBU, M_LH, M_LHU, M_LW, M_LWL, M_LWR, M_LL, M_FL
} load_t;
typedef enum logic [3:0] {
    S_NONE, S_SB, S_SH, S_SW, S_SWL, S_SWR, S_SC, S_FS
} store_t;
typedef enum logic [3:0] {
    B_NONE, B_EQ, B_NE, B_LEZ, B_GTZ, B_LTZ, B_GEZ, B_C1F, B_C1T, B_J, B_JR
} br_t;
typedef enum logic [3:0] {
    MD_NONE, MD_MULT, MD_MULTU, MD_DIV, MD_DIVU, MD_MTHI, MD_MTLO
} md_t;

typedef struct packed {
    logic        wr;
    logic [4:0]  rd;
    alu_t        alu;
    logic        b_imm, imm_zero;
    logic        use_rs, use_rt;
    load_t       ld;
    store_t      st;
    br_t         br;
    logic        likely;
    md_t         md;
    logic [2:0]  trap;           // 1 ge 2 geu 3 lt 4 ltu 5 eq 6 ne
    logic        trap_imm;
    logic        sys, brk, ri;
    logic        mfc0, mtc0, eret, tlbr, tlbwi, tlbwr, tlbp, cache;
    logic        cop1, cop1_gpr, cop1_cond;
    logic        late;           // its result comes out of M
} ctl_t;

// the COP1 operations fpu_n64 implements (moves, S/D arithmetic, conversions
// between S, D and W, compares)
function automatic logic fpu_implements(input logic [31:0] i);
    logic [4:0] rs; logic [5:0] fn;
    rs = i[25:21]; fn = i[5:0];
    case (rs)
    5'h00, 5'h02, 5'h04, 5'h06: return 1'b1;                          // mfc1 cfc1 mtc1 ctc1
    5'h10, 5'h11: return fn <= 6'h07 || (fn >= 6'h0C && fn <= 6'h0F) || fn == 6'h24 || fn >= 6'h30 ||
                         (fn == 6'h20 && rs == 5'h11) || (fn == 6'h21 && rs == 5'h10);
    5'h14: return fn == 6'h20 || fn == 6'h21;                         // cvt.s.w cvt.d.w
    default: return 1'b0;
    endcase
endfunction

function automatic ctl_t decode(input logic [31:0] i);
    ctl_t c;
    logic [5:0] op, fn;
    logic [4:0] rs, rt, rd;
    op = i[31:26]; fn = i[5:0]; rs = i[25:21]; rt = i[20:16]; rd = i[15:11];
    c = '0;
    case (op)
    6'h00: begin
        c.use_rs = 1; c.use_rt = 1; c.rd = rd; c.wr = 1;
        case (fn)
        6'h00, 6'h04: c.alu = A_SLL;
        6'h02, 6'h06: c.alu = A_SRL;
        6'h03, 6'h07: c.alu = A_SRA;
        6'h08: begin c.wr = 0; c.br = B_JR; end
        6'h09: begin c.br = B_JR; c.alu = A_LINK; end
        6'h0C: begin c.wr = 0; c.sys = 1; end
        6'h0D: begin c.wr = 0; c.brk = 1; end
        6'h0F: c.wr = 0;
        6'h10: c.alu = A_MFHI;
        6'h11: begin c.wr = 0; c.md = MD_MTHI; end
        6'h12: c.alu = A_MFLO;
        6'h13: begin c.wr = 0; c.md = MD_MTLO; end
        6'h18: begin c.wr = 0; c.md = MD_MULT; end
        6'h19: begin c.wr = 0; c.md = MD_MULTU; end
        6'h1A: begin c.wr = 0; c.md = MD_DIV; end
        6'h1B: begin c.wr = 0; c.md = MD_DIVU; end
        6'h20, 6'h21: c.alu = A_ADD;
        6'h22, 6'h23: c.alu = A_SUB;
        6'h24: begin c.alu = A_AND;  end
        6'h25: begin c.alu = A_OR;   end
        6'h26: begin c.alu = A_XOR;  end
        6'h27: begin c.alu = A_NOR;  end
        6'h2A: begin c.alu = A_SLT;  end
        6'h2B: begin c.alu = A_SLTU; end
        6'h30: begin c.wr = 0; c.trap = 3'd1; end
        6'h31: begin c.wr = 0; c.trap = 3'd2; end
        6'h32: begin c.wr = 0; c.trap = 3'd3; end
        6'h33: begin c.wr = 0; c.trap = 3'd4; end
        6'h34: begin c.wr = 0; c.trap = 3'd5; end
        6'h36: begin c.wr = 0; c.trap = 3'd6; end
        default: begin c.wr = 0; c.ri = 1; end
        endcase
        if (fn == 6'h00 || fn == 6'h02 || fn == 6'h03 || fn == 6'h38 || fn == 6'h3A || fn == 6'h3B ||
            fn == 6'h3C || fn == 6'h3E || fn == 6'h3F) c.use_rs = 0;
        if (fn == 6'h10 || fn == 6'h12) begin c.use_rs = 0; c.use_rt = 0; end
        if (fn == 6'h08 || fn == 6'h09 || fn == 6'h11 || fn == 6'h13) c.use_rt = 0;
    end
    6'h01: begin
        c.use_rs = 1;
        case (rt)
        5'h00, 5'h02, 5'h10, 5'h12: c.br = B_LTZ;
        5'h01, 5'h03, 5'h11, 5'h13: c.br = B_GEZ;
        5'h08: c.trap = 3'd1; 5'h09: c.trap = 3'd2; 5'h0A: c.trap = 3'd3; 5'h0B: c.trap = 3'd4;
        5'h0C: c.trap = 3'd5; 5'h0E: c.trap = 3'd6;
        default: c.ri = 1;
        endcase
        c.likely = rt[1] && !rt[3];
        c.trap_imm = rt[3];
        if (rt[4]) begin c.wr = 1; c.rd = 5'd31; c.alu = A_LINK; end
    end
    6'h02: c.br = B_J;
    6'h03: begin c.br = B_J; c.wr = 1; c.rd = 5'd31; c.alu = A_LINK; end
    6'h04, 6'h14: begin c.br = B_EQ;  c.use_rs = 1; c.use_rt = 1; c.likely = op[4]; end
    6'h05, 6'h15: begin c.br = B_NE;  c.use_rs = 1; c.use_rt = 1; c.likely = op[4]; end
    6'h06, 6'h16: begin c.br = B_LEZ; c.use_rs = 1; c.likely = op[4]; end
    6'h07, 6'h17: begin c.br = B_GTZ; c.use_rs = 1; c.likely = op[4]; end
    6'h08, 6'h09: begin c.alu = A_ADD;  c.b_imm = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
    6'h0A: begin c.alu = A_SLT;  c.b_imm = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
    6'h0B: begin c.alu = A_SLTU; c.b_imm = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
    6'h0C: begin c.alu = A_AND;  c.b_imm = 1; c.imm_zero = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
    6'h0D: begin c.alu = A_OR;   c.b_imm = 1; c.imm_zero = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
    6'h0E: begin c.alu = A_XOR;  c.b_imm = 1; c.imm_zero = 1; c.use_rs = 1; c.wr = 1; c.rd = rt; end
    6'h0F: begin c.alu = A_LUI;  c.b_imm = 1; c.wr = 1; c.rd = rt; end
    6'h10: begin
        if (rs == 5'h00) begin c.mfc0 = 1; c.wr = 1; c.rd = rt; c.late = 1; end
        else if (rs == 5'h04) begin c.mtc0 = 1; c.use_rt = 1; end
        else if (rs == 5'h10) begin
            case (fn)
            6'h01: c.tlbr = 1;  6'h02: c.tlbwi = 1; 6'h06: c.tlbwr = 1; 6'h08: c.tlbp = 1;
            6'h18: c.eret = 1;
            default: c.ri = 1;
            endcase
        end else c.ri = 1;
    end
    6'h11: begin
        if (rs == 5'h08) begin c.br = rt[0] ? B_C1T : B_C1F; c.likely = rt[1]; end
        else if (!fpu_implements(i)) c.ri = 1;                            // dmfc1, dmtc1, .l, ...
        else begin
            c.cop1 = 1;
            if (rs == 5'h00 || rs == 5'h02) begin c.cop1_gpr = 1; c.wr = 1; c.rd = rt; c.late = 1; end
            if (rs == 5'h04 || rs == 5'h06) c.use_rt = 1;
            if (rs == 5'h06 || rs >= 5'h10) c.cop1_cond = 1;
        end
    end
    6'h20: begin c.ld = M_LB;  c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h21: begin c.ld = M_LH;  c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h22: begin c.ld = M_LWL; c.use_rs = 1; c.use_rt = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h23: begin c.ld = M_LW;  c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h24: begin c.ld = M_LBU; c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h25: begin c.ld = M_LHU; c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h26: begin c.ld = M_LWR; c.use_rs = 1; c.use_rt = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h28: begin c.st = S_SB;  c.use_rs = 1; c.use_rt = 1; end
    6'h29: begin c.st = S_SH;  c.use_rs = 1; c.use_rt = 1; end
    6'h2A: begin c.st = S_SWL; c.use_rs = 1; c.use_rt = 1; end
    6'h2B: begin c.st = S_SW;  c.use_rs = 1; c.use_rt = 1; end
    6'h2E: begin c.st = S_SWR; c.use_rs = 1; c.use_rt = 1; end
    6'h2F: begin c.cache = 1; c.use_rs = 1; end
    6'h30: begin c.ld = M_LL;  c.use_rs = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h31, 6'h35: begin c.ld = M_FL; c.use_rs = 1; end
    6'h38: begin c.st = S_SC;  c.use_rs = 1; c.use_rt = 1; c.wr = 1; c.rd = rt; c.late = 1; end
    6'h39, 6'h3D: begin c.st = S_FS; c.use_rs = 1; end
    default: c.ri = 1;
    endcase
    if (c.rd == 5'd0) c.wr = 0;
    return c;
endfunction

// =============================================================== helpers
function automatic logic [31:0] sx8 (input logic [7:0]  v); return {{24{v[7]}},  v}; endfunction
function automatic logic [31:0] sx16(input logic [15:0] v); return {{16{v[15]}}, v}; endfunction
function automatic logic kseg01(input logic [31:0] va); return FLAT || va[31:30] == 2'b10; endfunction
function automatic logic [31:0] kphys(input logic [31:0] va);
    return (FLAT && va[31:29] != 3'b101) ? va : {3'b000, va[28:0]};
endfunction
// a physical address the D-cache may hold (FLAT: pa == va there)
function automatic logic pa_cached(input logic [31:0] pa);
    return FLAT ? pa[31:26] == 6'b010000 : (pa >> CACHE_LOG2) == 32'd0;
endfunction
function automatic logic cacheable(input logic [31:0] va);
    return FLAT ? va[31:26] == 6'b010000 : va[31:29] == 3'b100 && (va[28:0] >> CACHE_LOG2) == 29'd0;
endfunction

// =============================================================== state
logic [31:0] rf_lo [32];
// no reset: MLABs (the hardware's registers are undefined at power-up; the
// co-simulation starts both machines from zero)
initial for (int i = 0; i < 32; i++) rf_lo[i] = 0;
// read synchronously (a Cyclone V MLAB registers its read address): the
// address is that of the instruction entering D, or D's own while it waits;
// the write landing on the same edge is bypassed from rf_x*
logic [31:0] rfq_a, rfq_b;
logic        rf_xv; logic [4:0] rf_xrd; logic [31:0] rf_xlo;
logic [31:0] hi_r, lo_r;

logic [31:0] c0_index, c0_entrylo0, c0_entrylo1, c0_context, c0_pagemask, c0_wired, c0_badvaddr,
             c0_count, c0_entryhi, c0_compare, c0_status, c0_cause, c0_epc, c0_config, c0_lladdr,
             c0_watchlo, c0_watchhi, c0_taglo, c0_errorepc;
logic        count_frac;
logic [4:0]  c0_random;                     // decrements every step, from 31 down to Wired
wire  [4:0]  rnd_dec = (c0_random <= c0_wired[4:0]) ? 5'd31 : c0_random - 5'd1;
wire  [4:0]  rnd_now = SIM_COUNT_RETIRE ? rnd_dec : c0_random;   // (the model steps before executing)
logic        ll_bit;
logic        fcc;

// =============================================================== TLB
// The entries live in a RAM and are searched one per cycle (the scanner,
// shared by F and M; tlbp and tlbr use it too). Mapped accesses are rare
// (SM64: 200 k in 6,000 M cycles), and each of F and M keeps the last page it
// translated (a micro-TLB), so a search only follows a change of page.
typedef struct packed {
    logic [18:0] vpn;                        // EntryHi[31:13], the page-mask bits clear
    logic [7:0]  asid;
    logic [11:0] mask;                       // PageMask[24:13]
    logic        g;
    logic [30:0] lo0, lo1;                   // EntryLo[31:1]
} tlbe_t;
tlbe_t       tlb_ram [TLB_N];
logic        tlb_we; logic [4:0] tlb_waddr; tlbe_t tlb_wdata;
logic [4:0]  tlb_raddr; tlbe_t tlb_q;
localparam tlbe_t TLBE_RESET = '{vpn: 19'h40000, default: '0};                      // a kseg0 VPN: never matches
initial for (int i = 0; i < TLB_N; i++) tlb_ram[i] = TLBE_RESET;
always_ff @(posedge clk) begin
    if (tlb_we) tlb_ram[tlb_waddr] <= tlb_wdata;
    tlb_q <= tlb_ram[tlb_raddr];
end

// an entry's translation of va: {valid, dirty, pa}, and the page (for a micro-TLB)
typedef struct packed {
    logic        v, d;
    logic [19:0] vpage;                      // va[31:12] with the offset bits clear
    logic [12:0] om;                         // the page offset's bits above 11 (va[24:12])
    logic [19:0] pfn;                        // pa[31:12], the offset bits clear
    logic        g;
    logic [7:0]  asid;
} tlbx_t;
function automatic tlbx_t tlb_xlat(input tlbe_t e, input logic [31:0] va);
    tlbx_t x;
    logic [12:0] om, pb;
    logic [30:0] lo;
    om = {1'b0, e.mask};                                     // (page - 1) >> 12
    pb = {1'b0, e.mask} + 13'd1;                             // the bit choosing the odd page
    lo = (va[24:12] & pb) != 0 ? e.lo1 : e.lo0;
    x.v = lo[0]; x.d = lo[1];                                // EntryLo bits 1, 2
    x.om = om;
    x.vpage = va[31:12] & ~{7'd0, om};
    x.pfn = lo[24:5] & ~{7'd0, om};                          // EntryLo[25:6]
    x.g = e.g; x.asid = e.asid;
    return x;
endfunction
function automatic logic tlb_match(input tlbe_t e, input logic [31:0] va, input logic [7:0] asid);
    return ((va[31:13] ^ e.vpn) & {7'h7F, ~e.mask}) == 0 && (e.g || e.asid == asid);
endfunction
// a micro-TLB: the last page translated
typedef struct packed { logic valid; tlbx_t x; } utlb_t;
function automatic logic utlb_hit(input utlb_t u, input logic [31:0] va, input logic [7:0] asid);
    return u.valid && ((va[31:12] ^ u.x.vpage) & ~{7'd0, u.x.om}) == 0 && (u.x.g || u.x.asid == asid);
endfunction
// {hit, valid, dirty, pa}
function automatic logic [34:0] utlb_res(input tlbx_t x, input logic [31:0] va);
    return {1'b1, x.v, x.d, {x.pfn, 12'd0} | {7'd0, va[24:0] & {x.om, 12'hFFF}}};
endfunction

typedef enum logic [1:0] { SC_IDLE, SC_RUN, SC_DONE } sc_t;
typedef enum logic [1:0] { SCM_LOOKUP, SCM_PROBE, SCM_READ } scm_t;
sc_t         sc;
scm_t        sc_mode;
logic        sc_for_m;                       // M's request (else F's)
logic [4:0]  sc_k;                           // the entry in tlb_q
logic [31:0] sc_va;
logic [7:0]  sc_asid;
logic        sc_hit;
logic [4:0]  sc_idx;
tlbx_t       sc_x;
tlbe_t       sc_e;                           // tlbr's entry
utlb_t       utlb_f, utlb_m;

// =============================================================== caches
localparam int IC_LINES = 512, DC_LINES = 256;
logic [31:0] ic_data [IC_LINES * 8];
logic [15:0] ic_tag  [IC_LINES];            // {valid, pa[28:14]}
logic [7:0]  dc_b0 [DC_LINES * 8], dc_b1 [DC_LINES * 8], dc_b2 [DC_LINES * 8], dc_b3 [DC_LINES * 8];   // byte lanes
logic [16:0] dc_tag  [DC_LINES];            // {valid, pa[28:13]}
logic [11:0] ic_raddr; logic [31:0] ic_q; logic [15:0] ic_tq;
logic [10:0] dc_raddr; logic [31:0] dc_q; logic [16:0] dc_tq;
// writes: driven combinationally below (the write happens at the edge that ends
// the cycle, so a read issued in the next cycle sees it)
logic        ic_we;  logic [11:0] ic_waddr;  logic [31:0] ic_wdata;
logic        ict_we; logic [8:0]  ict_waddr; logic [15:0] ict_wdata;
logic        dc_we;  logic [10:0] dc_waddr;  logic [31:0] dc_wdata; logic [3:0] dc_wbe;
logic        dct_we; logic [7:0]  dct_waddr; logic [16:0] dct_wdata;
logic        slow_is_d;
logic        slow_d_own;                     // P_SLOW was granted to M's slow path (else to F's)

always_ff @(posedge clk) begin
    ic_q  <= ic_data[ic_raddr];
    ic_tq <= ic_tag[ic_raddr[11:3]];
    if (ic_we)  ic_data[ic_waddr] <= ic_wdata;
    if (ict_we) ic_tag[ict_waddr] <= ict_wdata;
end
always_ff @(posedge clk) begin
    dc_q  <= {dc_b3[dc_raddr], dc_b2[dc_raddr], dc_b1[dc_raddr], dc_b0[dc_raddr]};
    dc_tq <= dc_tag[dc_raddr[10:3]];
    if (dc_we) begin
        if (dc_wbe[0]) dc_b0[dc_waddr] <= dc_wdata[7:0];
        if (dc_wbe[1]) dc_b1[dc_waddr] <= dc_wdata[15:8];
        if (dc_wbe[2]) dc_b2[dc_waddr] <= dc_wdata[23:16];
        if (dc_wbe[3]) dc_b3[dc_waddr] <= dc_wdata[31:24];
    end
    if (dct_we) dc_tag[dct_waddr] <= dct_wdata;
end

// =============================================================== write buffer
localparam int WB_N = 8;
logic [31:0] wb_addr [WB_N];
logic [31:0] wb_data [WB_N];
logic [3:0]  wb_strb [WB_N];
logic [3:0]  wb_rp, wb_wp;
wire         wb_empty = wb_rp == wb_wp;
wire         wb_full  = (wb_wp - wb_rp) == 4'(WB_N);

// =============================================================== pipeline registers
logic [31:0] f_pc;
logic        f_ok;                           // the RAMs hold f_pc's line this cycle
logic        ds_next;                        // the next instruction into D is a delay slot
logic        rd_pend, rd_taken, rd_annul;    // its branch's decision, when it left D before it
logic [31:0] rd_target;

logic        d_valid, d_dly, d_fexc;
logic [31:0] d_pc, d_insn;
logic [4:0]  d_fexc_code;

logic        e_valid, e_dly, e_fexc, e_serial;
ctl_t        e_c;
logic [31:0] e_pc, e_insn, e_ra, e_rb;     // operands as captured in D
logic [4:0]  e_rs, e_rt, e_fexc_code;

logic        m_valid, m_dly, m_exc, m_serial;
logic [31:0] m_a_lo;                         // rs (mthi/mtlo, mult/div)
ctl_t        m_c;
logic [31:0] m_pc, m_insn, m_res, m_addr, m_st_lo, m_badv;
logic [4:0]  m_exc_code;

logic        w_valid, w_serial;
ctl_t        w_c;
logic [31:0] w_pc, w_insn, w_res;

// =============================================================== D
ctl_t d_c;
always_comb d_c = decode(d_insn);
wire [4:0] d_rs = d_insn[25:21], d_rt = d_insn[20:16];
wire w_wr = w_valid && w_c.wr;
wire m_wr = m_valid && m_c.wr;
wire e_wr = e_valid && e_c.wr;

// the regfile, with W's write bypassed
wire w_rs = w_wr && w_c.rd == d_rs, w_rt = w_wr && w_c.rd == d_rt;
wire x_rs = rf_xv && rf_xrd == d_rs, x_rt = rf_xv && rf_xrd == d_rt;
wire [31:0] rf_a  = d_rs == 0 ? 32'd0 : w_rs ? w_res  : x_rs ? rf_xlo : rfq_a;
wire [31:0] rf_b  = d_rt == 0 ? 32'd0 : w_rt ? w_res  : x_rt ? rf_xlo : rfq_b;
// a branch reads now: M's (not late) result forwarded
wire m_rs = m_wr && !m_c.late && m_c.rd == d_rs && d_rs != 0;
wire m_rt = m_wr && !m_c.late && m_c.rd == d_rt && d_rt != 0;
wire [31:0] br_a = m_rs ? m_res : rf_a, br_b = m_rt ? m_res : rf_b;

wire d_needs_rs = d_c.use_rs && d_rs != 0, d_needs_rt = d_c.use_rt && d_rt != 0;
wire e_rs_hit = e_wr && e_c.rd == d_rs, e_rt_hit = e_wr && e_c.rd == d_rt;
wire m_rs_late = m_wr && m_c.late && m_c.rd == d_rs, m_rt_late = m_wr && m_c.late && m_c.rd == d_rt;
wire d_reads_now = (d_c.br != B_NONE && d_c.br != B_J);
wire haz_br = d_reads_now && ((d_needs_rs && (e_rs_hit || m_rs_late)) || (d_needs_rt && (e_rt_hit || m_rt_late)));
wire haz_late = e_valid && e_c.late && ((d_needs_rs && e_rs_hit) || (d_needs_rt && e_rt_hit));
wire haz_fcc = (d_c.br == B_C1F || d_c.br == B_C1T) && ((e_valid && e_c.cop1_cond) || (m_valid && m_c.cop1_cond));
wire d_serial = d_c.mtc0 || d_c.eret || d_c.tlbwi || d_c.tlbwr || d_c.tlbr || d_c.tlbp || d_c.cache;
wire haz_serial = (d_serial && (e_valid || m_valid || w_valid)) || (e_valid && e_serial) || (m_valid && m_serial) || (w_valid && w_serial);
logic md_busy;
wire haz_md = (d_c.alu == A_MFHI || d_c.alu == A_MFLO || d_c.md != MD_NONE) &&
              (md_busy || (e_valid && e_c.md != MD_NONE) || (m_valid && m_c.md != MD_NONE));
wire d_stall = d_valid && (haz_br || haz_late || haz_fcc || haz_serial || haz_md);

logic d_taken;
always_comb begin
    case (d_c.br)
    B_EQ:  d_taken = br_a == br_b;
    B_NE:  d_taken = br_a != br_b;
    B_LEZ: d_taken = br_a[31] || br_a == 32'd0;
    B_GTZ: d_taken = !br_a[31] && br_a != 32'd0;
    B_LTZ: d_taken = br_a[31];
    B_GEZ: d_taken = !br_a[31];
    B_C1F: d_taken = !fcc;
    B_C1T: d_taken = fcc;
    B_J, B_JR: d_taken = 1'b1;
    default: d_taken = 1'b0;
    endcase
end
wire [31:0] d_target = d_c.br == B_J  ? {d_pc[31:28], d_insn[25:0], 2'b00}
                     : d_c.br == B_JR ? br_a
                     : d_pc + 32'd4 + {{14{d_insn[15]}}, d_insn[15:0], 2'b00};
wire d_cti = d_c.br != B_NONE && !d_fexc;

// =============================================================== E
// operands: E-stage forwarding from M (not late) and W
wire e_m_a = m_wr && !m_c.late && m_c.rd == e_rs && e_rs != 0;
wire e_m_b = m_wr && !m_c.late && m_c.rd == e_rt && e_rt != 0;
wire e_w_a = w_wr && w_c.rd == e_rs && e_rs != 0;
wire e_w_b = w_wr && w_c.rd == e_rt && e_rt != 0;
wire [31:0] e_a  = e_m_a ? m_res : e_w_a ? w_res : e_ra;
wire [31:0] e_b  = e_m_b ? m_res : e_w_b ? w_res : e_rb;

wire [31:0] e_bv  = e_c.b_imm ? (e_c.imm_zero ? {16'd0, e_insn[15:0]} : sx16(e_insn[15:0])) : e_b;
wire [5:0]  e_fn  = e_insn[5:0];
wire [4:0]  e_sh  = (e_fn == 6'h04 || e_fn == 6'h06 || e_fn == 6'h07) ? e_a[4:0] : e_insn[10:6];

// one shifter: SRL/SRA directly, SLL as a right shift of the bit-reversed value
function automatic logic [31:0] rev32(input logic [31:0] v);
    for (int i = 0; i < 32; i++) rev32[i] = v[31 - i];
endfunction
wire        e_sl = e_c.alu == A_SLL;
wire [32:0] e_shx = {e_c.alu == A_SRA && e_b[31], e_sl ? rev32(e_b) : e_b};
wire [32:0] e_shr = $signed(e_shx) >>> e_sh;
wire [31:0] e_shv = e_sl ? rev32(e_shr[31:0]) : e_shr[31:0];

logic [31:0] e_res;
always_comb begin
    e_res = 32'd0;
    case (e_c.alu)
    A_ADD:   e_res = e_a + e_bv;
    A_SUB:   e_res = e_a - e_bv;
    A_LUI:   e_res = {e_insn[15:0], 16'd0};
    A_SLL, A_SRL, A_SRA: e_res = e_shv;
    A_LINK:  e_res = e_pc + 32'd8;
    A_SLT:   e_res = {31'd0, $signed(e_a) < $signed(e_bv)};
    A_SLTU:  e_res = {31'd0, e_a < e_bv};
    A_AND:   e_res = e_a & e_bv;
    A_OR:    e_res = e_a | e_bv;
    A_XOR:   e_res = e_a ^ e_bv;
    A_NOR:   e_res = ~(e_a | e_bv);
    A_MFHI:  e_res = hi_r;
    A_MFLO:  e_res = lo_r;
    default: ;
    endcase
end

logic e_trap;
always_comb begin
    logic [31:0] tb;
    tb = e_c.trap_imm ? sx16(e_insn[15:0]) : e_b;
    case (e_c.trap)
    3'd1: e_trap = $signed(e_a) >= $signed(tb);
    3'd2: e_trap = e_a >= tb;
    3'd3: e_trap = $signed(e_a) < $signed(tb);
    3'd4: e_trap = e_a < tb;
    3'd5: e_trap = e_a == tb;
    3'd6: e_trap = e_a != tb;
    default: e_trap = 1'b0;
    endcase
end

wire [31:0] e_ea = e_a + sx16(e_insn[15:0]);
logic [3:0] e_align;       // the address's alignment requirement (1 2 4 8)
always_comb begin
    e_align = 4'd1;
    case (e_c.ld)
    M_LH, M_LHU: e_align = 4'd2;
    M_LW, M_LL: e_align = 4'd4;
    M_FL: e_align = e_insn[31:26] == 6'h35 ? 4'd8 : 4'd4;
    default: ;
    endcase
    case (e_c.st)
    S_SH: e_align = 4'd2;
    S_SW, S_SC: e_align = 4'd4;
    S_FS: e_align = e_insn[31:26] == 6'h3D ? 4'd8 : 4'd4;
    default: ;
    endcase
end
wire e_mem = e_c.ld != M_NONE || e_c.st != S_NONE;
wire e_misalign = e_mem && ((e_align == 4'd2 && e_ea[0]) || (e_align == 4'd4 && e_ea[1:0] != 0) ||
                            (e_align == 4'd8 && e_ea[2:0] != 0));

// E's synchronous exceptions (in order: fetch fault, coprocessor unusable,
// RI/sys/break, trap, address). CpU: an FPU instruction (bc1 included) or an
// FPU load/store while Status.CU1 is clear -- libultra starts every thread
// without it and, on this trap, gives the thread CU1 and marks it as using
// the FPU; only such threads get their FPU registers saved on a switch.
// Without it no thread ever was, and the sound thread's floats overwrote the
// game's mid-calculation (2026-09-29; sim/n64/cpu.c the same).
wire e_cpu1 = !c0_status[29] && (e_insn[31:26] == 6'h11 || e_c.ld == M_FL || e_c.st == S_FS);
logic e_exc; logic [4:0] e_exc_code; logic [31:0] e_badv;
always_comb begin
    e_exc = 1'b1; e_badv = e_ea; e_exc_code = 5'd0;
    if (e_fexc) begin e_exc_code = e_fexc_code; e_badv = e_pc; end
    else if (e_cpu1) e_exc_code = 5'd11;
    else if (e_c.ri) e_exc_code = 5'd10;
    else if (e_c.sys) e_exc_code = 5'd8;
    else if (e_c.brk) e_exc_code = 5'd9;
    else if (e_trap) e_exc_code = 5'd13;
    else if (e_misalign) e_exc_code = e_c.st != S_NONE ? 5'd5 : 5'd4;
    else e_exc = 1'b0;
end

// multiply / divide (runs in the background; HI/LO interlocked)
logic [5:0]   md_cnt;
logic [63:0]  md_acc;                        // {remainder, quotient}
logic [31:0]  md_dv;
logic         md_nq, md_nr;

// =============================================================== M
logic        fpu_valid, fpu_cond, fpu_busy;
logic [31:0] fpu_gpr_out;
logic [63:0] fpu_mem_out;
wire m_ld = m_c.ld != M_NONE, m_st = m_c.st != S_NONE, m_mem = m_ld || m_st;
wire m_kseg = kseg01(m_addr);
typedef enum logic [2:0] { MS_IDLE, MS_REFILL, MS_REREAD, MS_SLOW_A, MS_SLOW_B, MS_DONE, MS_XLAT, MS_WALK } ms_t;
ms_t ms;
// a mapped access spends a cycle in MS_XLAT with its translation registered
// (m_tlbq): from the micro-TLB, or after a search (MS_WALK)
logic [34:0] m_tlbq;
logic        m_wq;                           // MS_WALK: the scanner took the request
wire m_mapped = m_mem && !m_kseg;
wire [31:0] m_pa = m_kseg ? kphys(m_addr) : m_tlbq[31:0];
wire m_tlb_fault = m_mapped && ms == MS_XLAT && (!m_tlbq[34] || !m_tlbq[33] || (m_st && !m_tlbq[32]));
wire [4:0] m_tlb_code = (!m_tlbq[34] || !m_tlbq[33]) ? (m_st ? 5'd3 : 5'd2) : 5'd1;
wire m_wide = (m_c.ld == M_FL && m_insn[31:26] == 6'h35) || (m_c.st == S_FS && m_insn[31:26] == 6'h3D);   // ldc1, sdc1
// the store that completed last cycle: a load right behind it read the RAM in
// the edge the store wrote it; its bytes are merged in
logic        sb_valid; logic [10:0] sb_waddr; logic [31:0] sb_data; logic [3:0] sb_strb;
wire         sb_hit = sb_valid && sb_waddr == m_addr[12:2];
wire m_cached = cacheable(m_addr) && !m_wide;
wire m_dc_hit = dc_tq[16] && dc_tq[15:0] == m_addr[28:13];   // (cached = kseg0: pa is va[28:0])

logic [63:0] ms_data;                       // the slow path's read (high word = the lower address)
logic [2:0]  ms_cnt;

wire [31:0] dc_qm = {sb_hit && sb_strb[3] ? sb_data[31:24] : dc_q[31:24],
                     sb_hit && sb_strb[2] ? sb_data[23:16] : dc_q[23:16],
                     sb_hit && sb_strb[1] ? sb_data[15:8]  : dc_q[15:8],
                     sb_hit && sb_strb[0] ? sb_data[7:0]   : dc_q[7:0]};
wire [31:0] m_word = (ms == MS_DONE) ? (m_wide ? ms_data[63:32] : ms_data[31:0]) : dc_qm;

logic [31:0] m_ld_val;
always_comb begin
    logic [1:0] k, kl;
    logic [31:0] w, rt;
    // k: the byte's big-endian number in its word (little-endian lwl at byte
    // k is big-endian lwl at 3 - k, the same for lwr/swl/swr); kl its lane
    k = BIG ? m_addr[1:0] : ~m_addr[1:0]; kl = ~k; w = m_word; rt = m_st_lo;
    m_ld_val = 32'd0;
    case (m_c.ld)
    M_LB:  m_ld_val = sx8 (w[kl * 8 +: 8]);
    M_LBU: m_ld_val = {24'd0, w[kl * 8 +: 8]};
    M_LH:  m_ld_val = sx16(w[kl[1] * 16 +: 16]);
    M_LHU: m_ld_val = {16'd0, w[kl[1] * 16 +: 16]};
    M_LW, M_LL, M_FL: m_ld_val = w;
    M_LWL: case (k)
        2'd0: m_ld_val = w;
        2'd1: m_ld_val = {w[23:0], rt[7:0]};
        2'd2: m_ld_val = {w[15:0], rt[15:0]};
        default: m_ld_val = {w[7:0], rt[23:0]};
        endcase
    M_LWR: case (k)
        2'd0: m_ld_val = {rt[31:8], w[31:24]};
        2'd1: m_ld_val = {rt[31:16], w[31:16]};
        2'd2: m_ld_val = {rt[31:24], w[31:8]};
        default: m_ld_val = w;
        endcase
    default: ;
    endcase
end

// stores: data and strobes of the word at m_addr (lane 3 - k is N64 byte k)
logic [31:0] m_st_data; logic [3:0] m_st_strb;
always_comb begin
    logic [1:0] k;
    k = BIG ? m_addr[1:0] : ~m_addr[1:0]; m_st_data = 32'd0; m_st_strb = 4'd0;
    case (m_c.st)
    S_SB:  begin m_st_data = {4{m_st_lo[7:0]}};  m_st_strb = 4'b1000 >> k; end
    S_SH:  begin m_st_data = {2{m_st_lo[15:0]}}; m_st_strb = k[1] ? 4'b0011 : 4'b1100; end
    S_SW, S_SC: begin m_st_data = m_st_lo; m_st_strb = 4'b1111; end
    S_SWL: begin m_st_data = m_st_lo >> (8 * k); m_st_strb = 4'b1111 >> k; end
    S_SWR: begin m_st_data = m_st_lo << (8 * (3 - k)); m_st_strb = 4'b1111 << (3 - k); end
    S_FS:  begin m_st_data = fpu_mem_out[31:0]; m_st_strb = 4'b1111; end
    default: ;
    endcase
end

wire [31:0] cause_now = {c0_cause[31:11], irq_rcp, c0_cause[9:0]};
logic [31:0] c0_rd;
always_comb begin
    case (m_insn[15:11])
    5'd0: c0_rd = c0_index;
    5'd1: c0_rd = {27'd0, rnd_now};
    5'd2: c0_rd = c0_entrylo0; 5'd3: c0_rd = c0_entrylo1; 5'd4: c0_rd = c0_context;
    5'd5: c0_rd = c0_pagemask; 5'd6: c0_rd = c0_wired;    5'd8: c0_rd = c0_badvaddr;
    5'd9: c0_rd = (SIM_COUNT_RETIRE && count_frac) ? c0_count + 32'd1 : c0_count;   // (the model counts before executing)
    5'd10: c0_rd = c0_entryhi; 5'd11: c0_rd = c0_compare;
    5'd12: c0_rd = c0_status;  5'd13: c0_rd = cause_now;  5'd14: c0_rd = c0_epc;   // (Cause: IP2 live, as the model)
    5'd15: c0_rd = 32'h00000B22; 5'd16: c0_rd = c0_config; 5'd17: c0_rd = c0_lladdr;
    5'd18: c0_rd = c0_watchlo; 5'd19: c0_rd = c0_watchhi;
    5'd28: c0_rd = c0_taglo;   5'd30: c0_rd = c0_errorepc;
    default: c0_rd = 32'd0;
    endcase
end

// interrupts: taken before the instruction at M (not while a slow access is under way)
assign dbg_pc = f_pc; assign dbg_cause = cause_now; assign dbg_status = c0_status; assign dbg_epc = c0_epc;
wire int_pending = c0_status[0] && !c0_status[1] && !c0_status[2] && ((cause_now[15:8] & c0_status[15:8]) != 0);
wire m_take_int = m_valid && int_pending && ms == MS_IDLE;
wire m_take_exc = m_valid && !m_take_int && (m_exc || m_tlb_fault);
wire [4:0] m_code = m_exc ? m_exc_code : m_tlb_code;
wire m_refill = !m_exc && m_tlb_fault && !m_tlbq[34];
wire m_live = m_valid && !m_take_int && !m_take_exc;

wire m_sc_fail = m_c.st == S_SC && !ll_bit;
// a store to a device (not RDRAM, not the cartridge) completes when the device
// says so -- a DMA or a task it starts has finished by then, as in the model
wire m_devst = m_st && !m_wide && (FLAT ? !pa_cached(m_pa)
                                         : !(m_pa < 32'h03F0_0000) && !(m_pa >= 32'h1000_0000 && m_pa < 32'h1FC0_0000));
// the slow path: uncached / device / mapped loads, device stores, ldc1/sdc1;
// other stores (sc too) go through the write buffer whatever the region
wire m_needs_slow = m_mem && !m_sc_fail && (m_wide || (m_ld && !m_cached) || m_devst);
wire m_is_md = m_c.md != MD_NONE;
logic m_done;
always_comb begin
    m_done = 1'b1;
    if (m_live) begin
        if (m_ld && m_cached) m_done = ms == MS_DONE || (ms == MS_IDLE && m_dc_hit);
        else if (m_needs_slow) m_done = ms == MS_DONE;
        else if (m_st && !m_sc_fail) m_done = !wb_full;
        if (m_mapped && ms == MS_IDLE) m_done = 1'b0;               // translating first
        if (m_c.tlbp || m_c.tlbr) m_done = ms == MS_DONE;           // a search / a read of the TLB RAM
        if ((m_c.cop1 || m_c.ld == M_FL || m_c.st == S_FS) && fpu_busy) m_done = 1'b0;
        if (m_is_md && md_busy) m_done = 1'b0;
    end
end
wire m_stall = m_live && !m_done;
wire m_commit = m_live && m_done;           // (M always advances when done)

// =============================================================== pipeline advance
wire m_adv = !m_stall;
wire e_adv = m_adv;
wire d_adv = e_adv && !d_stall;
wire flush = m_take_int || m_take_exc || (m_commit && m_c.eret);
logic [31:0] flush_pc;
always_comb begin
    if (m_commit && m_c.eret) flush_pc = c0_status[2] ? c0_errorepc : c0_epc;
    else flush_pc = (c0_status[22] ? 32'hBFC00200 : EXC_BASE) + ((m_refill && !c0_status[1]) ? 32'h0 : 32'h180);
end

// =============================================================== F
typedef enum logic [2:0] { FS_RUN, FS_REFILL, FS_SLOW, FS_READY, FS_XLAT, FS_WALK } fs_t;
fs_t fs;
wire f_kseg = kseg01(f_pc);
wire f_cached = cacheable(f_pc);
// a mapped fetch: translated in FS_XLAT (registered), then fetched uncached
logic [34:0] f_tlbq;
logic        f_wq;                           // FS_WALK: the scanner took the request
logic        f_xfault;                       // FS_READY holds a translation fault, not an instruction
wire f_misal = f_pc[1:0] != 2'd0;
wire f_fault = f_misal || (fs == FS_READY && f_xfault);
wire [4:0] f_fault_code = f_misal ? 5'd4 : 5'd2;
logic [31:0] f_slow_insn;
logic [2:0]  f_cnt;
logic [31:0] f_fill_pa;
logic        f_discard;
wire f_hit = f_ok && f_cached && !f_misal && ic_tq[15] && ic_tq[14:0] == f_pc[28:14];
wire f_have = f_ok && (fs == FS_READY || (fs == FS_RUN && (f_hit || f_misal)));
wire f_take = !d_valid || d_adv;
wire f_deliver = f_have && f_take && !flush;
wire [31:0] f_insn = fs == FS_READY ? f_slow_insn : ic_q;

// the register file's read (see rfq_*)
wire [31:0] rf_rinsn = f_deliver ? (f_fault ? 32'd0 : f_insn) : d_insn;
always_ff @(posedge clk) begin
    rfq_a <= rf_lo[rf_rinsn[25:21]]; rfq_b <= rf_lo[rf_rinsn[20:16]];
    if (w_valid && w_c.wr) rf_lo[w_c.rd] <= w_res;
    rf_xv <= w_valid && w_c.wr; rf_xrd <= w_c.rd; rf_xlo <= w_res;
end

// the delay slot's branch: decided this cycle (leaving D) or earlier
wire cti_now = d_valid && d_adv && d_cti;
wire rd_valid = cti_now || rd_pend;
wire rd_t = cti_now ? d_taken : rd_taken;
wire rd_a = cti_now ? (d_c.likely && !d_taken) : rd_annul;
wire [31:0] rd_tg = cti_now ? d_target : rd_target;
// the instruction F delivers now is a delay slot: its branch left D earlier, or leaves now
wire ds_now = ds_next || cti_now;

logic [31:0] f_next;
always_comb begin
    if (flush) f_next = flush_pc;
    else if (f_deliver) f_next = (ds_now && rd_valid && rd_t) ? rd_tg : f_pc + 32'd4;
    else f_next = f_pc;
end
always_comb ic_raddr = f_next[13:2];

// =============================================================== the TLB scanner
wire sc_req_m = ms == MS_WALK && !m_wq;
wire sc_req_f = fs == FS_WALK && !f_wq && !f_discard && !flush;
wire sc_start = sc == SC_IDLE && (sc_req_m || sc_req_f);
wire sc_done_m = sc == SC_DONE && sc_for_m, sc_done_f = sc == SC_DONE && !sc_for_m;
wire sc_match = TLB_EN && tlb_match(tlb_q, sc_va, sc_asid);
always_comb begin
    tlb_raddr = sc_k + 5'd1;                                  // the next entry
    if (sc_start) tlb_raddr = (sc_req_m && m_c.tlbr) ? c0_index[4:0] : 5'd0;
    // writes: tlbwi / tlbwr, as they complete
    tlb_we = TLB_EN && m_commit && (m_c.tlbwi || m_c.tlbwr);
    tlb_waddr = m_c.tlbwi ? c0_index[4:0] : rnd_now;
    tlb_wdata.mask = c0_pagemask[24:13];
    tlb_wdata.vpn  = c0_entryhi[31:13] & ~{7'd0, c0_pagemask[24:13]};
    tlb_wdata.asid = c0_entryhi[7:0];
    tlb_wdata.g    = c0_entrylo0[0] & c0_entrylo1[0];
    tlb_wdata.lo0  = c0_entrylo0[31:1];
    tlb_wdata.lo1  = c0_entrylo1[31:1];
end

// =============================================================== memory port
typedef enum logic [2:0] { P_IDLE, P_WB, P_ILINE, P_DLINE, P_SLOW } port_t;
port_t port;
logic [2:0] port_cnt;
logic        slow_we; logic [31:0] slow_pa, slow_wdata; logic [3:0] slow_strb;
// whose the slow request is: fixed when the port is granted -- M may enter
// its slow path while F's uncached fetch is out (the address must not change
// under it, nor its word complete M's load)
always_comb slow_is_d = port == P_SLOW ? slow_d_own : (ms == MS_SLOW_A || ms == MS_SLOW_B);
always_comb begin
    slow_we = 1'b0; slow_pa = {f_fill_pa[31:2], 2'b00}; slow_wdata = 32'd0; slow_strb = 4'd0;
    if (slow_is_d) begin
        slow_pa = {m_pa[31:3], ms == MS_SLOW_B ? 1'b1 : (m_wide ? 1'b0 : m_pa[2]), 2'b00};
        slow_we = m_st;
        if (m_wide) begin                                   // sdc1: the pair, lower address first
            slow_wdata = (ms == MS_SLOW_B) == BIG ? fpu_mem_out[31:0] : fpu_mem_out[63:32];
            slow_strb  = 4'hF;
        end else begin                                      // a device store
            slow_wdata = m_st_data; slow_strb = m_st_strb;
        end
    end
end
always_comb begin
    mem_req = 1'b0; mem_addr = 32'd0; mem_we = 1'b0; mem_wdata = 32'd0; mem_wstrb = 4'd0; mem_line = 1'b0;
    case (port)
    P_WB:    begin mem_req = 1'b1; mem_we = 1'b1; mem_addr = wb_addr[wb_rp[2:0]]; mem_wdata = wb_data[wb_rp[2:0]]; mem_wstrb = wb_strb[wb_rp[2:0]]; end
    P_ILINE: begin mem_req = 1'b1; mem_line = 1'b1; mem_addr = {f_fill_pa[31:5], 5'd0}; end
    P_DLINE: begin mem_req = 1'b1; mem_line = 1'b1; mem_addr = {m_pa[31:5], 5'd0}; end
    P_SLOW:  begin mem_req = 1'b1; mem_we = slow_we; mem_addr = slow_pa; mem_wdata = slow_wdata; mem_wstrb = slow_strb; end
    default: ;
    endcase
end
wire port_done = (port == P_SLOW || port == P_WB) ? ((mem_we && mem_ack) || (!mem_we && mem_rvalid)) : 1'b0;

// =============================================================== cache RAM writes
always_comb begin
    ic_we = 0; ic_waddr = 0; ic_wdata = 0; ict_we = 0; ict_waddr = 0; ict_wdata = 0;
    dc_we = 0; dc_waddr = 0; dc_wdata = 0; dc_wbe = 0; dct_we = 0; dct_waddr = 0; dct_wdata = 0;
    if (port == P_ILINE && mem_rvalid) begin
        ic_we = 1; ic_waddr = {f_fill_pa[13:5], f_cnt}; ic_wdata = mem_rdata;
        if (f_cnt == 3'd7) begin ict_we = 1; ict_waddr = f_fill_pa[13:5]; ict_wdata = {1'b1, f_fill_pa[28:14]}; end
    end
    if (port == P_DLINE && mem_rvalid) begin
        dc_we = 1; dc_waddr = {m_pa[12:5], ms_cnt}; dc_wdata = mem_rdata; dc_wbe = 4'hF;
        if (ms_cnt == 3'd7) begin dct_we = 1; dct_waddr = m_pa[12:5]; dct_wdata = {1'b1, m_pa[28:13]}; end
    end
    // a completing store: into the D-cache (hit), or drop the line an uncached / sdc1 store may shadow
    if (m_commit && m_st && !m_sc_fail) begin
        if (!m_wide && m_cached && m_dc_hit) begin
            dc_we = 1; dc_waddr = m_addr[12:2]; dc_wdata = m_st_data; dc_wbe = m_st_strb;
        end else if ((m_wide || !m_cached) && pa_cached(m_pa)) begin
            dct_we = 1; dct_waddr = m_pa[12:5]; dct_wdata = 17'd0;
        end
    end
    // cache instructions: invalidate a line (every op but the fills; a write-through D-cache has nothing to write back)
    if (m_commit && m_c.cache) begin
        if (m_insn[17:16] == 2'd0) begin ict_we = 1; ict_waddr = m_addr[13:5]; ict_wdata = 16'd0; end
        else if (m_insn[17:16] == 2'd1) begin dct_we = 1; dct_waddr = m_addr[12:5]; dct_wdata = 17'd0; end
    end
end
// the D-cache read: the next M's address, or M's own while it waits
always_comb dc_raddr = m_stall ? m_addr[12:2] : e_ea[12:2];

// =============================================================== retire trace
always_comb begin
    tr_retire = w_valid;
    tr_pc = w_pc; tr_insn = w_insn;
    tr_wr = w_valid && w_c.wr; tr_rd = w_c.rd; tr_val = {{32{w_res[31]}}, w_res};
    tr_exc = m_take_int || m_take_exc;
    tr_exc_int = m_take_int;
    tr_exc_pc = m_pc;
    tr_exc_code = m_take_int ? 5'd0 : m_code;
end

// COP1: at M; its results are written as the instruction completes
wire m_fpu = m_c.cop1 || m_c.ld == M_FL || m_c.st == S_FS;
fpu_n64 fpu (
    .clk, .rst,
    .insn_next(m_adv ? e_insn : m_insn), .insn(m_insn),
    .live(m_live && m_fpu), .valid(fpu_valid),
    .gpr_in(m_st_lo), .mem_in(m_wide ? ms_data : {32'd0, m_word}),
    .gpr_out(fpu_gpr_out), .mem_out(fpu_mem_out), .cond(fpu_cond), .busy(fpu_busy),
    .tr_we_even(tr_fwe_even), .tr_we_odd(tr_fwe_odd), .tr_widx(tr_fwidx),
    .tr_weven(tr_fweven), .tr_wodd(tr_fwodd), .tr_fcr31(tr_fcr31)
);
assign fpu_valid = m_commit && m_fpu;

// =============================================================== sequential
integer k;
always_ff @(posedge clk) begin
    if (rst) begin
        f_pc <= reset_pc; f_ok <= 0; fs <= FS_RUN; ds_next <= 0; rd_pend <= 0; f_discard <= 0;
        d_valid <= 0; e_valid <= 0; m_valid <= 0; w_valid <= 0;
        c0_status <= 32'h34000000; c0_config <= 32'h7006E463; c0_cause <= 0; c0_count <= 0; c0_compare <= 0;
        c0_wired <= 0; c0_index <= 0; c0_entryhi <= 0; c0_entrylo0 <= 0; c0_entrylo1 <= 0; c0_pagemask <= 0;
        c0_context <= 0; c0_epc <= 0; c0_errorepc <= 0; c0_badvaddr <= 0; c0_lladdr <= 0; c0_watchlo <= 0;
        c0_watchhi <= 0; c0_taglo <= 0;
        count_frac <= 0; c0_random <= 5'd31; ll_bit <= 0; fcc <= 0;
        hi_r <= 0; lo_r <= 0;
        md_busy <= 0; md_cnt <= 0;
        wb_rp <= 0; wb_wp <= 0; port <= P_IDLE; port_cnt <= 0;
        ms <= MS_IDLE; ms_cnt <= 0; sb_valid <= 0;
        sc <= SC_IDLE; sc_k <= 0; utlb_f.valid <= 0; utlb_m.valid <= 0; m_wq <= 0; f_wq <= 0;
    end else begin
        // ---------------------------------------------------- Count / Compare
        begin
            logic step;
            step = SIM_COUNT_RETIRE ? (m_commit || m_take_exc) : 1'b1;
            if (step) begin
                c0_random <= rnd_dec;
                count_frac <= !count_frac;
                if (count_frac) begin
                    c0_count <= c0_count + 32'd1;
                    if (c0_count + 32'd1 == c0_compare) c0_cause[15] <= 1'b1;
                end
            end
        end
        c0_cause[10] <= irq_rcp;

        // ---------------------------------------------------- M -> W
        w_valid <= m_commit;
        if (m_commit) begin
            w_c <= m_c; w_pc <= m_pc; w_insn <= m_insn; w_serial <= m_serial;
            if (m_c.mfc0) w_res <= c0_rd;
            else if (m_c.cop1_gpr) w_res <= fpu_gpr_out[31:0];
            else if (m_c.st == S_SC) w_res <= {31'd0, !m_sc_fail};
            else if (m_ld) w_res <= m_ld_val;
            else w_res <= m_res;
        end

        // ---------------------------------------------------- M: exceptions
        if (m_take_int || m_take_exc) begin
            if (!c0_status[1]) begin
                c0_epc <= m_dly ? m_pc - 32'd4 : m_pc;
                c0_cause[31] <= m_dly;
            end
            c0_cause[6:2] <= m_take_int ? 5'd0 : m_code;
            c0_cause[29:28] <= (!m_take_int && m_exc && m_code == 5'd11) ? 2'd1 : 2'd0;   // CE: COP1
            c0_status[1] <= 1'b1;
            if (m_take_exc) begin
                if (m_exc && (m_code == 5'd4 || m_code == 5'd5)) c0_badvaddr <= m_badv;
                if (!m_exc) begin                                   // a TLB fault
                    c0_badvaddr <= m_addr;
                    c0_context <= (c0_context & 32'hFF800000) | ((m_addr >> 13) << 4);
                    c0_entryhi <= (m_addr & 32'hFFFFE000) | (c0_entryhi & 32'hFF);
                end
            end
        end

        // ---------------------------------------------------- M: completing
        sb_valid <= 0;
        if (m_commit) begin
            if (m_st && !m_wide && !m_sc_fail && m_cached && m_dc_hit) begin
                sb_valid <= 1; sb_waddr <= m_addr[12:2]; sb_data <= m_st_data; sb_strb <= m_st_strb;
            end
            if (m_c.ld == M_LL) begin ll_bit <= 1'b1; c0_lladdr <= m_pa >> 4; end
            if (m_c.st == S_SC) ll_bit <= 1'b0;
            if (m_c.cop1 && m_c.cop1_cond) fcc <= fpu_cond;
            if (m_c.md == MD_MTHI) hi_r <= m_a_lo;
            if (m_c.md == MD_MTLO) lo_r <= m_a_lo;
            if (m_c.mtc0) begin
                case (m_insn[15:11])
                5'd1, 5'd15: ;                                      // Random, PRId: read-only
                5'd9:  c0_count <= m_st_lo;
                5'd11: begin c0_compare <= m_st_lo; c0_cause[15] <= 1'b0; end
                5'd13: c0_cause[9:8] <= m_st_lo[9:8];
                // the VR4300's writable bits (the constant rest is not stored; BadVAddr,
                // XContext, TagHi and the reserved registers take no write)
                5'd0:  c0_index[5:0] <= m_st_lo[5:0];
                5'd2:  c0_entrylo0 <= m_st_lo & 32'h3FFFFFFF; 5'd3: c0_entrylo1 <= m_st_lo & 32'h3FFFFFFF;
                5'd4:  c0_context[31:23] <= m_st_lo[31:23];
                5'd5:  c0_pagemask <= m_st_lo & 32'h01FFE000;
                5'd6:  begin c0_wired <= m_st_lo & 32'h3F; c0_random <= 5'd31; end
                5'd10: c0_entryhi <= m_st_lo & 32'hFFFFE0FF;  5'd12: c0_status <= m_st_lo;
                5'd14: c0_epc <= m_st_lo;
                5'd16: c0_config <= (c0_config & ~32'h0F00800F) | (m_st_lo & 32'h0F00800F);
                5'd17: c0_lladdr <= m_st_lo;
                5'd18: c0_watchlo <= m_st_lo & 32'hFFFFFFFB;  5'd19: c0_watchhi <= m_st_lo & 32'hF;
                5'd28: c0_taglo <= m_st_lo & 32'h0FFFFFC0;    5'd30: c0_errorepc <= m_st_lo;
                default: ;
                endcase
            end
            if (m_c.eret) begin
                if (c0_status[2]) c0_status[2] <= 1'b0; else c0_status[1] <= 1'b0;
                ll_bit <= 1'b0;
            end
            if (m_c.tlbwi || m_c.tlbwr) begin utlb_f.valid <= 0; utlb_m.valid <= 0; end   // (the RAM: tlb_we)
            if (m_c.tlbr) begin
                c0_pagemask <= {7'd0, sc_e.mask, 13'd0};
                c0_entryhi  <= {sc_e.vpn, 5'd0, sc_e.asid};
                c0_entrylo0 <= {sc_e.lo0, sc_e.g};
                c0_entrylo1 <= {sc_e.lo1, sc_e.g};
            end
            if (m_c.tlbp) c0_index <= sc_hit ? {27'd0, sc_idx} : 32'h80000000;
        end

        // ---------------------------------------------------- the write buffer
        if (m_commit && m_st && !m_wide && !m_sc_fail && !m_devst) begin
            wb_addr[wb_wp[2:0]] <= {m_pa[31:2], 2'b00}; wb_data[wb_wp[2:0]] <= m_st_data; wb_strb[wb_wp[2:0]] <= m_st_strb;
            wb_wp <= wb_wp + 4'd1;
        end

        // ---------------------------------------------------- E -> M
        if (flush) m_valid <= 0;
        else if (m_adv) begin
            m_valid <= e_valid;
            m_c <= e_c; m_pc <= e_pc; m_insn <= e_insn; m_dly <= e_dly; m_serial <= e_serial;
            m_res <= e_res;
            m_addr <= e_ea;
            m_st_lo <= e_b;
            m_a_lo <= e_a;
            m_exc <= e_exc; m_exc_code <= e_exc_code; m_badv <= e_badv;
        end

        // ---------------------------------------------------- D -> E
        if (flush) e_valid <= 0;
        else if (e_adv) begin
            e_valid <= d_valid && !d_stall;
            e_c <= d_c; e_pc <= d_pc; e_insn <= d_insn; e_dly <= d_dly;
            e_rs <= d_rs; e_rt <= d_rt;
            e_ra <= rf_a; e_rb <= rf_b;
            e_fexc <= d_fexc; e_fexc_code <= d_fexc_code;
            e_serial <= d_serial;
        end else begin
            // E held behind a stalled M: W's result is only bypassed for one
            // cycle, so an operand it supplies is kept here
            if (e_w_a) e_ra <= w_res;
            if (e_w_b) e_rb <= w_res;
        end

        // ---------------------------------------------------- F -> D
        if (flush) begin
            d_valid <= 0; ds_next <= 0; rd_pend <= 0;
        end else begin
            if (d_adv) d_valid <= 0;
            if (cti_now && !f_deliver) begin
                rd_pend <= 1; rd_taken <= d_taken; rd_annul <= d_c.likely && !d_taken; rd_target <= d_target;
            end
            if (f_deliver) begin
                d_valid <= !(ds_now && rd_valid && rd_a);           // an annulled delay slot never enters
                d_pc <= f_pc; d_insn <= f_fault ? 32'd0 : f_insn;
                d_dly <= ds_now;
                d_fexc <= f_fault; d_fexc_code <= f_fault_code;
                ds_next <= 0;
                rd_pend <= 0;
            end else if (cti_now) ds_next <= 1;
        end

        // ---------------------------------------------------- F
        f_pc <= f_next;
        f_ok <= fs == FS_RUN || fs == FS_READY;
        case (fs)
        FS_RUN: if (f_ok && !f_have && !flush) begin
            f_cnt <= 0; f_discard <= 0; f_xfault <= 0;
            if (!f_kseg) begin
                if (TLB_EN && utlb_hit(utlb_f, f_pc, c0_entryhi[7:0])) begin fs <= FS_XLAT; f_tlbq <= utlb_res(utlb_f.x, f_pc); end
                else begin fs <= FS_WALK; f_wq <= 0; end
            end
            else begin f_fill_pa <= kphys(f_pc); fs <= f_cached ? FS_REFILL : FS_SLOW; end
        end
        FS_WALK: begin                                      // a search; a flush waits for its end
            if (flush) f_discard <= 1;
            if (sc_start && !sc_req_m) f_wq <= 1;
            if (sc_done_f) begin
                if (f_discard || flush) fs <= FS_RUN;
                else begin
                    fs <= FS_XLAT;
                    f_tlbq <= sc_hit ? utlb_res(sc_x, f_pc) : 35'd0;
                    if (sc_hit) begin utlb_f.valid <= 1; utlb_f.x <= sc_x; end
                end
            end else if (!f_wq && !sc_start && (f_discard || flush)) fs <= FS_RUN;   // not started: just go
        end
        FS_XLAT:
            if (flush) fs <= FS_RUN;
            else if (!f_tlbq[34] || !f_tlbq[33]) begin fs <= FS_READY; f_xfault <= 1; f_ok <= 1; end
            else begin f_fill_pa <= f_tlbq[31:0]; fs <= FS_SLOW; end
        FS_REFILL: if (port == P_ILINE && mem_rvalid) begin
            f_cnt <= f_cnt + 3'd1;
            if (f_cnt == 3'd7) begin fs <= FS_RUN; f_ok <= 0; end
        end
        FS_SLOW: begin
            if (flush) f_discard <= 1;
            if (port == P_SLOW && !slow_is_d && mem_rvalid) begin
                f_slow_insn <= mem_rdata;
                fs <= (f_discard || flush) ? FS_RUN : FS_READY;
                f_ok <= !(f_discard || flush);
            end
        end
        FS_READY: if (f_deliver || flush) fs <= FS_RUN;
        default: fs <= FS_RUN;
        endcase
        if (fs == FS_REFILL || (fs == FS_SLOW && !(port == P_SLOW && !slow_is_d && mem_rvalid))) f_ok <= 0;

        // ---------------------------------------------------- M's slow path / refill
        case (ms)
        MS_IDLE: if (m_live && (m_c.tlbp || m_c.tlbr)) begin ms <= MS_WALK; m_wq <= 0; end
        else if (m_live && m_mem) begin
            if (m_mapped) begin
                if (TLB_EN && utlb_hit(utlb_m, m_addr, c0_entryhi[7:0])) begin ms <= MS_XLAT; m_tlbq <= utlb_res(utlb_m.x, m_addr); end
                else begin ms <= MS_WALK; m_wq <= 0; end
            end
            else if (m_ld && m_cached && !m_dc_hit) begin ms <= MS_REFILL; ms_cnt <= 0; end
            else if (m_needs_slow) ms <= MS_SLOW_A;
        end
        MS_WALK: begin                               // a search (a mapped access, tlbp) or tlbr's read
            if (sc_start && sc_req_m) m_wq <= 1;
            if (sc_done_m) begin
                if (m_c.tlbp || m_c.tlbr) ms <= MS_DONE;
                else begin
                    ms <= MS_XLAT;
                    m_tlbq <= sc_hit ? utlb_res(sc_x, m_addr) : 35'd0;
                    if (sc_hit) begin utlb_m.valid <= 1; utlb_m.x <= sc_x; end
                end
            end
        end
        MS_XLAT:                                     // a fault, a store done, or on to the slow path
            if (!m_live || m_commit) ms <= MS_IDLE;
            else if (m_needs_slow) ms <= MS_SLOW_A;
        MS_REFILL: if (port == P_DLINE && mem_rvalid) begin
            ms_cnt <= ms_cnt + 3'd1;
            if (ms_cnt == 3'd7) ms <= MS_REREAD;
        end
        MS_REREAD: ms <= MS_IDLE;
        MS_SLOW_A: if (port == P_SLOW && slow_d_own && port_done) begin
            if (m_wide) begin if (BIG) ms_data[63:32] <= mem_rdata; else ms_data[31:0] <= mem_rdata; ms <= MS_SLOW_B; end
            else begin ms_data[31:0] <= mem_rdata; ms <= MS_DONE; end
        end
        MS_SLOW_B: if (port == P_SLOW && slow_d_own && port_done) begin
            if (BIG) ms_data[31:0] <= mem_rdata; else ms_data[63:32] <= mem_rdata; ms <= MS_DONE;
        end
        MS_DONE: if (m_commit) ms <= MS_IDLE;
        default: ms <= MS_IDLE;
        endcase

        // ---------------------------------------------------- the TLB scanner
        case (sc)
        SC_IDLE: if (sc_start) begin
            sc <= SC_RUN; sc_k <= tlb_raddr; sc_for_m <= sc_req_m;
            sc_mode <= !sc_req_m ? SCM_LOOKUP : m_c.tlbp ? SCM_PROBE : m_c.tlbr ? SCM_READ : SCM_LOOKUP;
            sc_va <= !sc_req_m ? f_pc : m_c.tlbp ? c0_entryhi : m_addr;
            sc_asid <= c0_entryhi[7:0];
        end
        SC_RUN: begin                                 // tlb_q holds entry sc_k
            sc_e <= TLB_EN ? tlb_q : TLBE_RESET;
            sc_idx <= sc_k; sc_x <= tlb_xlat(tlb_q, sc_va);
            if (sc_mode == SCM_READ) sc <= SC_DONE;
            else if (sc_match) begin sc_hit <= 1; sc <= SC_DONE; end
            else if (!TLB_EN || sc_k == 5'(TLB_N - 1)) begin sc_hit <= 0; sc <= SC_DONE; end
            else sc_k <= sc_k + 5'd1;
        end
        default: sc <= SC_IDLE;                       // SC_DONE: the result, for one cycle
        endcase

        // ---------------------------------------------------- the memory port
        case (port)
        P_IDLE: begin
            port_cnt <= 0;
            if (!wb_empty) port <= P_WB;
            else if (ms == MS_REFILL) port <= P_DLINE;
            else if (ms == MS_SLOW_A || ms == MS_SLOW_B) begin port <= P_SLOW; slow_d_own <= 1; end
            else if (fs == FS_REFILL) port <= P_ILINE;
            else if (fs == FS_SLOW) begin port <= P_SLOW; slow_d_own <= 0; end
        end
        P_WB: if (mem_ack) begin wb_rp <= wb_rp + 4'd1; port <= P_IDLE; end
        P_ILINE, P_DLINE: if (mem_rvalid) begin
            port_cnt <= port_cnt + 3'd1;
            if (port_cnt == 3'd7) port <= P_IDLE;
        end
        P_SLOW: if (port_done) port <= P_IDLE;
        default: port <= P_IDLE;
        endcase

        // ---------------------------------------------------- multiply / divide (started as M completes)
        if (m_commit && m_is_md && m_c.md != MD_MTHI && m_c.md != MD_MTLO) begin
            logic sa, sb, sgn;
            sgn = m_c.md == MD_DIV || m_c.md == MD_MULT;
            sa = sgn && m_a_lo[31]; sb = sgn && m_st_lo[31];
            if (m_c.md == MD_MULT || m_c.md == MD_MULTU) begin
                logic [63:0] p;
                p = m_c.md == MD_MULT ? 64'($signed(m_a_lo) * $signed(m_st_lo)) : 64'(m_a_lo) * 64'(m_st_lo);
                lo_r <= p[31:0]; hi_r <= p[63:32];
            end else if (m_st_lo == 32'd0) begin                  // divide by zero: what the VR4300 leaves
                lo_r <= (m_c.md == MD_DIV && m_a_lo[31]) ? 32'd1 : 32'hFFFFFFFF;
                hi_r <= m_a_lo;
            end else begin                                         // restoring division of the magnitudes
                md_acc <= {32'd0, sa ? -m_a_lo : m_a_lo};
                md_dv <= sb ? -m_st_lo : m_st_lo;
                md_nq <= sa ^ sb; md_nr <= sa;
                md_cnt <= 6'd32; md_busy <= 1'b1;
            end
        end else if (md_busy) begin
            if (md_cnt != 0) begin
                logic [32:0] r, t;
                r = {md_acc[63:32], md_acc[31]};
                t = r - {1'b0, md_dv};
                md_acc <= t[32] ? {r[31:0], md_acc[30:0], 1'b0} : {t[31:0], md_acc[30:0], 1'b1};
                md_cnt <= md_cnt - 6'd1;
            end else begin
                md_busy <= 1'b0;
                lo_r <= md_nq ? -md_acc[31:0] : md_acc[31:0];
                hi_r <= md_nr ? -md_acc[63:32] : md_acc[63:32];
            end
        end
    end
end

endmodule
`default_nettype wire
