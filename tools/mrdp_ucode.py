#!/usr/bin/env python3
"""MRDP's TRI_R setup engine as microcode: assembler, cycle-accurate
simulator and differential test against the C spec.

  tools/mrdp_ucode.py gen            write rtl/mrdp/mrdp_ucode_rom.v
  tools/mrdp_ucode.py test [N]       N random TRI_R commands: simulator vs
                                     lang/c/mrdp/mrdp_setup.h (via ctypes)
  tools/mrdp_ucode.py list           the program, one instruction per line

Why microcode: the first TRI_R engine was a 70-step FSM in which every step
loaded its own registers from its own expressions -- 1,840 ALMs of operand
muxes (2026-09-27 standalone fit), more than the whole device had to spare.
Here the datapath is regular: 8 registers, one 32-bit adder-based ALU, the
MRDP's multiplier and 64-bit shifter, and the vertex RAM (vram) as storage.
The sequence lives in a ROM block.

The machine (rtl/mrdp/mrdp_top.v, S_RSET), one instruction per cycle:
  opA  R[ra] | VQ | SH | SHH | SHS | PLO | PHI
  opB  R[rb] | VQ | imm16 | imm16 << 16
  VQ   vram[rad] of the PREVIOUS instruction (its read address field)
  SH/SHH/SHS  the shifter's low word / high word / low word saturated to
       0x7FFFFFFF when bits 63..31 are not all 0, from a register behind the
       shifter: valid two instructions after a shl (and after an SHK), and
       consecutive shl's come out on consecutive instructions; SHS (the
       saturation) one instruction later still
  PLO/PHI/P16/P30  slices of the signed product of the MUL issued two
       instructions before (it stays until the next MUL); shl PU loads the
       unsigned one. No ALU operand comes from the unsigned product: its
       correction adder in front of the ALU's adder missed timing
  alu  -> R[rd] (we), vram[wad] (wv; written a cycle later: read it back
       two instructions on at the earliest), and the sp destination
  mul  mul_a <= opA, mul_b <= opB     shl  sh_in <= mul_pu | mul_p | {opA, opB}
  fl   flags from the result: Z, N (signed less-than for subtractions), C
       (borrow, for SBB/RSBB); a branch sees the flags of earlier instructions
  sp   TYH/TYM/TYL (t_y* <= opA[27:14]), LFT (t_lft/fneg from result[31]),
       TDXH.. TXL (edge registers), SHK (sh_k), SEEDA (the reciprocal ROM's
       address, result[30:22] -- the RSP's VRCP ROM -- SEED, 1 + rom / 2^16
       in Q30, is valid two instructions later),
       VEXP (vexp <= result clamped to 0..63, at DONE), GRW (t_grow <=
       result[2:0]: the crack-grow edges, mrdp_setup.h mrdp_grow_mask)
"""
import ctypes, os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ---- encoding ----------------------------------------------------------------
SEQ = dict(NEXT=0, JMP=1, BR=2, CALL=3, RET=4, DONE=5, EXIT=6)
COND = dict(Z=0, NZ=1, LT=2, LE=3, APOS=4, TEX=5, NTEX=6, GE=7)
SA = dict(R=0, VQ=1, SH=2, SHH=3, SHS=4, PLO=5, PHI=6)
SB = dict(R=0, VQ=1, IL=2, IH=3)
ALU = dict(MOV=0, MOVB=1, ADD=2, SUB=3, SBB=4, RSB=5, RSBB=6, ABS=7, NEGB=8, NEGD=9,
           AND=10, OR=11, CLZ=12, MIN=13, P16=14, P30=15, SEED=16)
SHL = dict(NONE=0, PU=1, P=2, AB=3)
SP = dict(NONE=0, TYH=1, TYM=2, TYL=3, LFT=4, TDXH=5, TDXM=6, TDXL=7, TXH=8, TXM=9,
          TXL=10, SHK=11, SEEDA=12, VEXP=13, GRW=14)
FIELDS = [  # (name, width), LSB first
    ('seq', 3), ('cond', 3), ('tgt', 8),
    ('sa', 3), ('ra', 3), ('sb', 2), ('rb', 3), ('imm', 16),
    ('alu', 5), ('we', 1), ('rd', 3),
    ('rad', 6), ('rrel', 1), ('wv', 1), ('wad', 6), ('wrel', 1),
    ('mul', 1), ('shl', 2), ('sp', 4), ('fl', 1),
]
WIDTH = sum(w for _, w in FIELDS)


class U:
    """one instruction; keyword arguments as in the program below"""
    def __init__(self, alu=None, a='R', b='R', ra=0, rb=0, imm=0, d=None, rad=None, rrel=0,
                 wv=None, wrel=0, mul=0, shl='NONE', sp='NONE', fl=0, seq='NEXT', cond='Z',
                 tgt=None, note=''):
        self.alu, self.a, self.b, self.ra, self.rb, self.imm = alu, a, b, ra, rb, imm
        self.d, self.rad, self.rrel, self.wv, self.wrel = d, rad, rrel, wv, wrel
        self.mul, self.shl, self.sp, self.fl = mul, shl, sp, fl
        self.seq, self.cond, self.tgt, self.note = seq, cond, tgt, note

    def fields(self, labels):
        t = self.tgt
        if isinstance(t, str):
            t = labels[t]
        return dict(seq=SEQ[self.seq], cond=COND[self.cond], tgt=t or 0,
                    sa=SA[self.a], ra=self.ra, sb=SB[self.b], rb=self.rb, imm=self.imm & 0xFFFF,
                    alu=ALU[self.alu or 'MOV'], we=int(self.d is not None), rd=self.d or 0,
                    rad=self.rad or 0, rrel=self.rrel, wv=int(self.wv is not None),
                    wad=self.wv or 0, wrel=self.wrel, mul=self.mul, shl=SHL[self.shl],
                    sp=SP[self.sp], fl=self.fl)


def encode(f):
    v, sh = 0, 0
    for name, w in FIELDS:
        assert 0 <= f[name] < (1 << w), (name, f[name])
        v |= f[name] << sh
        sh += w
    return v


# ---- the program ---------------------------------------------------------------
# vram: 0..23 attribute triplets (texture S T W at tbase = shade ? 12 : 0),
# 24..27 F0..F3, 28 kx, 29 ky (outputs), 32..37 x0 y0 x1 y1 x2 y2 (input),
# 38..41 DX1 DY1 DX2 DY2, 42..43 the area (lo, hi)
def program():
    P, L = [], {}

    def I(**kw):
        P.append(U(**kw))

    def label(n):
        L[n] = len(P)

    # differences, t_y*, the area (64 bits, lo - lo then hi - hi - borrow)
    I(alu='MOVB', b='IL', imm=0, sp='SHK', rad=32, note='sh_k = 0; X0')
    I(a='VQ', d=0, rad=33, note='R0 = X0')
    I(a='VQ', d=1, sp='TYH', rad=34, note='R1 = Y0')
    I(alu='SUB', a='VQ', rb=0, d=2, wv=38, rad=35, note='R2 = DX1')
    I(alu='SUB', a='VQ', rb=1, d=3, wv=39, sp='TYM', rad=36, note='R3 = DY1')
    I(alu='SUB', a='VQ', rb=0, d=4, wv=40, rad=37, note='R4 = DX2')
    I(alu='SUB', a='VQ', rb=1, d=5, wv=41, sp='TYL', fl=1, note='R5 = DY2')
    I(mul=1, ra=2, rb=5, seq='BR', cond='Z', tgt='exit', note='DX1 * DY2; DY2 == 0: nothing')
    I(mul=1, ra=4, rb=3, note='DX2 * DY1')
    I(shl='P', note='sh_in = DX1 * DY2')
    I(a='PLO', d=6)
    I(a='PHI', d=7)
    I(alu='SUB', a='SH', rb=6, d=6, wv=42, fl=1, note='area lo')
    I(alu='SBB', a='SHH', rb=7, d=7, wv=43, sp='LFT', note='area hi')
    I(alu='OR', ra=6, rb=7, fl=1)
    I(alu='AND', ra=1, b='IH', imm=0xFFFF, d=7, seq='BR', cond='Z', tgt='exit', note='area 0: nothing; y0i')
    I(alu='SUB', ra=7, rb=1, d=7, wv=29, note='ky = y0i - Y0')

    # the edges: dxh = slope(DX2, DY2), xh = X0 + kx, kx = mul16(dxh, ky)
    I(ra=4, d=2)
    I(ra=5, d=3, fl=1, seq='CALL', tgt='slope1')
    I(ra=2, sp='TDXH', rad=29)
    I(mul=1, ra=2, b='VQ')
    I()
    I(alu='P16', d=6, wv=28, note='kx')
    I(alu='ADD', ra=6, rb=0, sp='TXH', rad=38, note='xh')
    I(a='VQ', d=2, rad=39)
    I(a='VQ', d=3, fl=1, seq='CALL', tgt='slope1', note='dxm = slope(DX1, DY1)')
    I(ra=2, sp='TDXM', rad=29)
    I(mul=1, ra=2, b='VQ')
    I()
    I(alu='P16', d=6)
    I(alu='ADD', ra=6, rb=0, sp='TXM', rad=34, note='xm')
    I(a='VQ', d=0, rad=36, note='R0 = X1')
    I(alu='SUB', a='VQ', rb=0, d=2, rad=35, note='X2 - X1')
    I(a='VQ', d=1, rad=37, note='R1 = Y1')
    I(alu='SUB', a='VQ', rb=1, d=3, seq='CALL', tgt='slope', note='dxl = slope(X2 - X1, Y2 - Y1); the flags of the 32-bit difference')
    I(alu='AND', ra=1, b='IH', imm=0xFFFF, d=7, note='ymi')
    I(alu='SUB', ra=7, rb=1, d=7)
    I(ra=2, sp='TDXL', mul=1, rb=7, note='dxl * (ymi - Y1)')
    I()
    I(alu='P16', d=6)
    I(alu='ADD', ra=6, rb=0, sp='TXL', rad=42, note='xl')

    # |area|, normalised to [2^31, 2^32) (mrdp_norm64): ea = msb, d = the mantissa
    I(a='VQ', d=6, rad=43, note='area lo')
    I(a='VQ', d=7, seq='BR', cond='APOS', tgt='apos', note='area hi')
    I(alu='RSB', ra=6, b='IL', imm=0, d=6, fl=1, note='-area')
    I(alu='RSBB', ra=7, b='IL', imm=0, d=7)
    label('apos')
    # the crack-grow edges (mrdp_grow_mask): edge k grows iff not
    # |area| < (|dx| + |dy|) << 16, 64-bit signed; R2 the mask
    I(alu='MOVB', b='IL', imm=16, sp='SHK', rad=40, note='sh_k = 16; DX2')
    I(alu='ABS', a='VQ', d=3, rad=41, note='|DX2|')
    I(alu='ABS', a='VQ', d=4, note='|DY2|')
    I(alu='MOVB', b='IL', imm=0, d=2, note='mask = 0')
    for k, name in ((0, 'H'), (1, 'M'), (2, 'L')):
        if k == 1:
            I(rad=38)
            I(alu='ABS', a='VQ', d=3, rad=39, note='|DX1|')
            I(alu='ABS', a='VQ', d=4, note='|DY1|')
        elif k == 2:
            I(rad=40)
            I(a='VQ', d=3, rad=38, note='DX2')
            I(alu='SUB', ra=3, b='VQ', d=3, rad=41, note='DX2 - DX1')
            I(a='VQ', d=4, rad=39, note='DY2')
            I(alu='SUB', ra=4, b='VQ', d=4, note='DY2 - DY1')
            I(alu='ABS', ra=3, d=3)
            I(alu='ABS', ra=4, d=4)
        I(alu='ADD', ra=3, rb=4, d=3, note='%s: |dx| + |dy|' % name)
        I(shl='AB', ra=3, b='IL', imm=0, note='sh_in = {l, 0}: l << 16 after sh_k')
        I()
        I(alu='RSB', a='SH', rb=6, fl=1, note='|area| - (l << 16), lo')
        I(alu='RSBB', a='SHH', rb=7, fl=1, note='... hi: N = |area| < l << 16')
        I(seq='BR', cond='LT', tgt='nogrow' + name)
        I(alu='OR', ra=2, b='IL', imm=1 << k, d=2)
        label('nogrow' + name)
    I(ra=2, sp='GRW')
    I(ra=7, fl=1)
    I(alu='CLZ', ra=7, d=4, seq='BR', cond='Z', tgt='alo')
    I(alu='RSB', ra=4, b='IL', imm=32, sp='SHK')
    I(shl='AB', ra=7, rb=6, note='sh_in = |area|')
    I(alu='RSB', ra=4, b='IL', imm=63, d=4, note='ea = 63 - clz hi')
    I(a='SH', d=3, sp='SEEDA', seq='JMP', tgt='arec', note='d = |area| >> (32 - clz hi)')
    label('alo')
    I(alu='CLZ', ra=6, d=4)
    I(alu='RSB', ra=4, b='IL', imm=32, sp='SHK')
    I(shl='AB', ra=6, b='IL', imm=0, note='sh_in = lo << 32')
    I(alu='RSB', ra=4, b='IL', imm=31, d=4, note='ea = 31 - clz lo')
    I(a='SH', d=3, sp='SEEDA', note='d = lo << clz lo')
    label('arec')
    I(note='the seed ROM')
    recip(I)                                  # R5 = 2^62 / R3

    # the factors (mrdp_factors): s = 33 - clz(|dy2| | |dy1| | |dx1| | |dx2|),
    # F = +-(|d| r) >> s, e = 15 + ea - s
    P[-1].rad = 41
    I(alu='ABS', a='VQ', d=0, rad=39, note='|DY2|')
    I(alu='ABS', a='VQ', d=1, rad=38, note='|DY1|')
    I(alu='ABS', a='VQ', d=2, rad=40, note='|DX1|')
    I(alu='ABS', a='VQ', d=3, note='|DX2|')
    I(alu='OR', ra=0, rb=1, d=7)
    I(alu='OR', ra=7, rb=2, d=7)
    I(alu='OR', ra=7, rb=3, d=7)
    I(alu='CLZ', ra=7, d=7)
    I(alu='RSB', ra=7, b='IL', imm=33, d=7, sp='SHK', note='s')
    I(alu='ADD', ra=4, b='IL', imm=15, d=4)
    I(alu='SUB', ra=4, rb=7, sp='VEXP', note='e = 15 + ea - s')
    I(mul=1, ra=0, rb=5)
    I(mul=1, ra=1, rb=5)
    I(mul=1, ra=2, rb=5, shl='PU')
    I(mul=1, ra=3, rb=5, shl='PU', rad=41)
    I(alu='NEGD', a='SH', b='VQ', wv=24, shl='PU', rad=39, note='F0')
    I(alu='NEGD', a='SH', b='VQ', wv=25, shl='PU', rad=38, note='F1')
    I(alu='NEGD', a='SH', b='VQ', wv=26, rad=40, note='F2')
    I(alu='NEGD', a='SH', b='VQ', wv=27, seq='BR', cond='NTEX', tgt='done', rad=6, rrel=1, note='F3')

    # texture: W' = wref / w in 2.30 (a slope with 30 fraction bits) when
    # w != wref and w >= 2^17, else 1.0 = 2^30; S' = s W' >> 30, T' likewise
    I(a='VQ', d=0, rad=7, rrel=1)
    I(alu='MIN', ra=0, b='VQ', d=0, rad=8, rrel=1)
    I(alu='MIN', ra=0, b='VQ', d=0, rad=6, rrel=1, note='R0 = wref')
    for k in range(3):
        I(a='VQ', d=3, note='w%d' % k)
        I(alu='SUB', ra=3, rb=0, fl=1)
        I(alu='SUB', ra=3, b='IH', imm=2, fl=1, seq='BR', cond='Z', tgt='one%d' % k)
        I(ra=0, d=2, seq='BR', cond='LT', tgt='one%d' % k)
        I(seq='CALL', tgt='slopew', note='W%d = slope30(wref, w%d)' % (k, k))
        I(seq='JMP', tgt='apply%d' % k, rad=k, rrel=1)
        label('one%d' % k)
        I(alu='MOVB', b='IH', imm=0x4000, d=2, rad=k, rrel=1, note='W = 1.0 (2.30)')
        label('apply%d' % k)
        I(mul=1, a='VQ', rb=2, rad=3 + k, rrel=1)
        I(mul=1, a='VQ', rb=2)
        I(alu='P30', wv=k, wrel=1, note='S W >> 30')
        I(alu='P30', wv=3 + k, wrel=1)
        I(ra=2, wv=6 + k, wrel=1, rad=7 + k, rrel=1)
    label('done')
    I(seq='DONE')
    label('exit')
    I(seq='EXIT')

    # mrdp_slope(R2, R3) -> R2 (S15.16), mrdp_slope30 (2.30): the same but the
    # final shift; entered at slope1 the flags already hold R3's
    for name, base in (('slope', 46), ('slopew', 32)):
        label(name)
        I(ra=3, fl=1)
        label(name + '1')
        I(ra=2, fl=1, seq='BR', cond='LE', tgt=name + 'z', note='dy <= 0: 0')
        I(alu='CLZ', ra=3, d=4, seq='BR', cond='Z', tgt=name + 'z', note='dx == 0: 0; s')
        I(alu='RSB', ra=4, b='IL', imm=32, sp='SHK')
        I(shl='AB', ra=3, b='IL', imm=0)
        I(alu='RSB', ra=4, b='IL', imm=base, d=4, note='%d - s' % base)
        I(a='SH', d=3, sp='SEEDA', note='d = dy << s')
        I(alu='ABS', ra=2, d=7, note='|dx|; the seed ROM')
        recip(I, fill=[dict(ra=4, sp='SHK')])
        I(mul=1, ra=7, rb=5, note='|dx| r')
        I()
        I(shl='PU')
        I()
        I()
        I(alu='NEGB', a='SHS', rb=2, d=2, seq='RET')
        label(name + 'z')
        I(alu='MOVB', b='IL', imm=0, d=2, seq='RET')
    return P, L


def recip(I, fill=()):
    """R5 = mrdp_recip_norm(R3): the RSP ROM, one Newton step. SEEDA was set
    two instructions before. The multiplier's idle slots take `fill` (no
    operand use)"""
    fill = list(fill)
    slot = lambda: I(**(fill.pop(0) if fill else {}))
    I(alu='SEED', d=5)
    for _ in range(1):
        I(mul=1, ra=3, rb=5, note='d y')
        slot()
        I(alu='RSB', a='PHI', b='IH', imm=0x8000, d=6, note='2 - d y: d >= 2^31, so unsigned d y = signed + 2^32 y')
        I(alu='SUB', ra=6, rb=5, d=6)
        I(mul=1, ra=5, rb=6, note='y (2 - d y): both below 2^31')
        slot()
        I(alu='P30', d=5)
    assert not fill


# ---- simulator -------------------------------------------------------------------
M32, M64 = (1 << 32) - 1, (1 << 64) - 1
s32 = lambda v: v - (1 << 32) if v & 0x80000000 else v
s64 = lambda v: v - (1 << 64) if v >> 63 else v


def clz32(v):
    return 32 - v.bit_length() if v else 32


def seed_table():
    src = open(os.path.join(ROOT, 'lang/c/mrdp/mrdp_setup.h')).read()
    body = src[src.index('mrdp_rcp_rom[512] = {') + 21:]
    tab = [int(x, 0) for x in body[:body.index('}')].replace('\n', ' ').split(',') if x.strip()]
    assert tab == [min(0xFFFF, (((1 << 34) // (512 + i) + 1) >> 8) - 0x10000) for i in range(512)], \
        'mrdp_rcp_rom[] no longer matches min(0xFFFF, ((2^34 / (512 + i) + 1) >> 8) - 2^16)'
    return tab


SEED_ROM = None


def simulate(prog, labels, vram, tflags, maxcyc=5000, trace=False):
    """run from 0; returns (outputs dict or None for EXIT, cycles)"""
    global SEED_ROM
    if SEED_ROM is None:
        SEED_ROM = seed_table()
    R = [0] * 8
    mem = list(vram)
    q = 0                                     # vram read data (VQ)
    vw = None                                 # pending write (addr, data)
    mul_a = mul_b = mul_ad = mul_bd = 0
    mul_p = 0
    sh_in, sh_k = 0, 0
    sh_r = 0                                  # the registered shifter output
    sh_rs = 0                                 # ... and its saturation, a cycle later
    sa, seed_q = 0, 0
    freg, fN, fC, fneg = 0, 0, 0, 0
    out = {}
    tbase = 12 if tflags & 4 else 0
    pc, ret = 0, 0
    for cyc in range(maxcyc):
        u = prog[pc]
        mul_pu = ((((mul_p >> 32) + (mul_bd if mul_ad >> 31 else 0) + (mul_ad if mul_bd >> 31 else 0)) & M32) << 32) | (mul_p & M32)
        sh_out = (s64(sh_in) >> sh_k) & M64
        srcA = [R[u.ra], q, sh_r & M32, sh_r >> 32,
                sh_rs, mul_p & M32, mul_p >> 32]
        opA = srcA[SA[u.a]]
        opB = [R[u.rb], q, u.imm & 0xFFFF, (u.imm & 0xFFFF) << 16][SB[u.b]]
        alu = u.alu or 'MOV'
        # the adder: X + (Y ^ inv) + cin, 33 bits signed for N
        neg = lambda c, v: ((-v) & M32) if c else v
        lt = None
        if alu in ('SUB', 'SBB', 'RSB', 'RSBB', 'MIN'):
            x, y = (opB, opA) if alu in ('RSB', 'RSBB') else (opA, opB)
            cin = 0 if (alu in ('SBB', 'RSBB') and fC) else 1
            t = x + (y ^ M32) + cin
            res = t & M32
            borrow = 1 - (t >> 32)
            lt = int(s32(x) - s32(y) - (1 - cin) < 0)
            if alu == 'MIN':
                res = opA if lt else opB
        elif alu == 'MOV': res = opA
        elif alu == 'MOVB': res = opB
        elif alu == 'ADD': res = (opA + opB) & M32
        elif alu == 'ABS': res = neg(opA >> 31, opA)
        elif alu == 'NEGB': res = neg(opB >> 31, opA)
        elif alu == 'NEGD': res = neg(fneg ^ (opB >> 31), opA)
        elif alu == 'AND': res = opA & opB
        elif alu == 'OR': res = opA | opB
        elif alu == 'CLZ': res = clz32(opA)
        elif alu == 'P16': res = (mul_p >> 16) & M32
        elif alu == 'P30': res = (mul_p >> 30) & M32
        elif alu == 'SEED': res = 0x40000000 | (seed_q << 14)
        else: raise ValueError(alu)
        if trace:
            print('%4d pc %3d %-5s A=%08x B=%08x res=%08x fl(z=%d n=%d c=%d) %s' % (cyc, pc, alu, opA, opB, res, freg == 0, fN, fC, u.note))
        # the branch condition, from the flags as they are before this edge
        z = freg == 0
        c = {'Z': z, 'NZ': not z, 'LT': fN, 'LE': fN or z, 'APOS': not fneg,
             'TEX': bool(tflags & 2), 'NTEX': not (tflags & 2), 'GE': not fN}[u.cond]
        # ---- the edge: every register takes its next value
        n_q = mem[(u.rad or 0) + (tbase if u.rrel else 0)]
        sh_rs = 0x7FFFFFFF if (sh_r >> 31) else sh_r & M32
        sh_r = sh_out
        if vw:
            mem[vw[0]] = vw[1]
        vw = (u.wv + (tbase if u.wrel else 0), res) if u.wv is not None else None
        if u.d is not None:
            R[u.d] = res
        if u.fl:
            freg = res
            fN = lt if lt is not None else res >> 31
            if lt is not None and alu != 'MIN':
                fC = borrow
        n_seed = SEED_ROM[sa]
        sp = u.sp
        if sp in ('TYH', 'TYM', 'TYL'): out[sp] = (opA >> 14) & 0x3FFF
        elif sp == 'LFT': out['LFT'] = 1 - (res >> 31); fneg = res >> 31
        elif sp in ('TDXH', 'TDXM', 'TDXL', 'TXH', 'TXM', 'TXL'): out[sp] = res
        elif sp == 'SHK': sh_k = res & 63
        elif sp == 'SEEDA': sa = (res >> 22) & 0x1FF
        elif sp == 'VEXP': out['VEXP'] = 0 if res >> 31 else 63 if res > 63 else res
        elif sp == 'GRW': out['GRW'] = res & 7
        if u.shl == 'PU': sh_in = mul_pu
        elif u.shl == 'P': sh_in = mul_p
        elif u.shl == 'AB': sh_in = (opA << 32) | opB
        n_mul_p = (s32(mul_a) * s32(mul_b)) & M64
        mul_ad, mul_bd = mul_a, mul_b
        if u.mul:
            mul_a, mul_b = opA, opB
        mul_p = n_mul_p
        q, seed_q = n_q, n_seed
        # ---- sequencing
        if u.seq == 'NEXT': pc += 1
        elif u.seq == 'JMP': pc = labels[u.tgt]
        elif u.seq == 'BR': pc = labels[u.tgt] if c else pc + 1
        elif u.seq == 'CALL': ret, pc = pc + 1, labels[u.tgt]
        elif u.seq == 'RET': pc = ret
        elif u.seq in ('DONE', 'EXIT'):
            if vw:
                mem[vw[0]] = vw[1]
            if u.seq == 'EXIT':
                return None, cyc + 1
            out['mem'] = mem
            return out, cyc + 1
    raise RuntimeError('no DONE')


def ref_lib():
    so = os.path.join(tempfile.gettempdir(), 'mrdp_ucode_ref_%d.so' % os.getuid())
    src = os.path.join(HERE, 'mrdp_ucode_ref.c')
    hdr = os.path.join(ROOT, 'lang/c/mrdp/mrdp_setup.h')
    if not os.path.exists(so) or os.path.getmtime(so) < max(os.path.getmtime(src), os.path.getmtime(hdr)):
        subprocess.check_call(['cc', '-O1', '-shared', '-fPIC', '-I', os.path.join(ROOT, 'lang/c/mrdp'), src, '-o', so])
    lib = ctypes.CDLL(so)
    lib.ref_tri_r.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_int32)]
    return lib


def rand_cmd(rng):
    """a TRI_R command: mostly plausible triangles, some extreme, some garbage"""
    flags = rng.randrange(8)
    kind = rng.randrange(10)
    fx = lambda lo, hi: int(rng.uniform(lo, hi) * 65536) & M32
    if kind < 6:
        vs = []
        for _ in range(3):
            x, y = fx(-300, 600), fx(-300, 600)
            y = (y + 0x2000) & ~0x3FFF & M32
            vs.append((x, y))
        vs.sort(key=lambda v: s32(v[1]))
        if kind == 5:                         # slivers / tiny
            vs[1] = ((vs[0][0] + rng.randrange(-3, 4)) & M32, vs[1][1])
    else:
        vs = [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(3)]
        if kind == 9:
            vs.sort(key=lambda v: s32(v[1]))
    w = [0x20000000 | (flags << 19)] + [c for v in vs for c in v]
    for i in range(8):
        if not ((i < 4 and flags & 4) or (4 <= i < 7 and flags & 2) or (i == 7 and flags & 1)):
            continue
        for k in range(3):
            if kind >= 7:
                w.append(rng.getrandbits(32))
            elif i < 4 or i == 7:
                w.append(rng.randrange(0x10001))
            elif i < 6:
                w.append(fx(-2048, 2048))
            else:
                w.append(rng.choice([0x10000, rng.randrange(1, 1 << 24), rng.randrange(1 << 17), 0]))
        if i == 6 and kind < 7 and rng.random() < 0.3:
            w[-2] = w[-3]                     # equal w's
    return w


def run_sim(P, L, w):
    flags = (w[0] >> 19) & 7
    vram = [0] * 64
    for k in range(6):
        vram[32 + k] = w[1 + k]
    for i, v in enumerate(w[7:]):
        vram[i] = v
    out, cyc = simulate(P, L, vram, flags)
    if out is None:
        return [0] * 28, cyc
    m = out['mem']
    tb = 12 if flags & 4 else 0
    r = [1, out['LFT'], out['TYH'], out['TYM'], out['TYL'], out['TDXH'], out['TDXM'], out['TDXL'],
         out['TXH'], out['TXM'], out['TXL']] + m[24:28] + [m[28], m[29], out['VEXP']]
    r += [m[tb + k] for k in range(9)] if flags & 2 else [0] * 9
    r += [out['GRW']]
    return [s32(v & M32) for v in r], cyc


NAMES = ['ok', 'lft', 'yh', 'ym', 'yl', 'dxh', 'dxm', 'dxl', 'xh', 'xm', 'xl', 'F0', 'F1', 'F2', 'F3',
         'kx', 'ky', 'vexp'] + ['S0', 'S1', 'S2', 'T0', 'T1', 'T2', 'W0', 'W1', 'W2', 'grow']


def test(n, seed=1):
    P, L = program()
    lib = ref_lib()
    rng = random.Random(seed)
    bad, cycs = 0, []
    for it in range(n):
        w = rand_cmd(rng)
        wa = (ctypes.c_uint32 * len(w))(*w)
        ref = (ctypes.c_int32 * 28)()
        lib.ref_tri_r(wa, ref)
        ref = list(ref)
        if not (w[0] >> 19) & 2:
            ref[18:27] = [0] * 9
        got, cyc = run_sim(P, L, w)
        cycs.append(cyc)
        if got != ref:
            bad += 1
            if bad <= 5:
                print('MISMATCH #%d flags %d: %s' % (it, (w[0] >> 19) & 7, ' '.join('%08x' % x for x in w)))
                for nm, a, b in zip(NAMES, ref, got):
                    if a != b:
                        print('   %-5s ref %08x sim %08x' % (nm, a & M32, b & M32))
    print('%d / %d match; cycles min %d mean %.1f max %d' % (n - bad, n, min(cycs), sum(cycs) / n, max(cycs)))
    return bad == 0


def gen():
    P, L = program()
    assert len(P) <= 256
    words = [encode(u.fields(L)) for u in P]
    fill = encode(U(seq='EXIT').fields(L))
    o = []
    o.append('// GENERATED by tools/mrdp_ucode.py gen -- do not edit. MRDP\'s TRI_R setup')
    o.append('// microcode (%d instructions of %d bits); registered address, one ROM block pair.' % (len(P), WIDTH))
    ports = ',\n'.join('    output wire [%d:0] u_%s' % (w - 1, n) for n, w in FIELDS)
    o.append('module mrdp_ucode_rom (\n    input  wire       clk,\n    input  wire [7:0] a,\n%s\n);' % ports)
    o.append('    reg [%d:0] rom [0:255];' % (WIDTH - 1))
    o.append('    reg [%d:0] q;' % (WIDTH - 1))
    o.append('    integer i;')
    o.append('    initial begin')
    o.append("        for (i = 0; i < 256; i = i + 1) rom[i] = %d'h%x;" % (WIDTH, fill))
    for i, (u, v) in enumerate(zip(P, words)):
        tag = ','.join(k for k, x in L.items() if x == i)
        o.append("        rom[%d] = %d'h%019x;%s" % (i, WIDTH, v, ('   // ' + tag) if tag else ''))
    o.append('    end')
    o.append('    always @(posedge clk) q <= rom[a];')
    sh = 0
    for n, w in FIELDS:
        o.append('    assign u_%s = q[%d:%d];' % (n, sh + w - 1, sh))
        sh += w
    o.append('endmodule')
    path = os.path.join(ROOT, 'rtl/mrdp/mrdp_ucode_rom.v')
    open(path, 'w').write('\n'.join(o) + '\n')
    t = seed_table()
    r = ['// GENERATED by tools/mrdp_ucode.py gen from lang/c/mrdp/mrdp_setup.h mrdp_rcp_rom[]:',
         "// the N64 RSP's VRCP reciprocal ROM (entries 0..511), registered read",
         'module mrdp_seed_rom (', '    input  wire       clk,', '    input  wire [8:0] a,',
         '    output reg [15:0] q', ');', '    reg [15:0] rom [0:511];', '    initial begin']
    r += ["        rom[%d] = 16'h%04x;" % (i, v) for i, v in enumerate(t)]
    r += ['    end', '    always @(posedge clk) q <= rom[a];', 'endmodule']
    assert len(t) == 512
    open(os.path.join(ROOT, 'rtl/mrdp/mrdp_seed_rom.v'), 'w').write('\n'.join(r) + '\n')
    print('wrote %s: %d instructions' % (path, len(P)))
    # the encodings the RTL decodes, for rtl/mrdp/mrdp_top.v to check against
    for name, d in (('SEQ', SEQ), ('COND', COND), ('SA', SA), ('SB', SB), ('ALU', ALU), ('SHL', SHL), ('SP', SP)):
        print('  %-4s %s' % (name, ' '.join('%s=%d' % kv for kv in d.items())))


def main():
    P, L = program()
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'list'
    if cmd == 'list':
        for i, u in enumerate(P):
            tag = [k for k, v in L.items() if v == i]
            print('%3d %-8s %s' % (i, ','.join(tag), vars(u)))
        print('%d instructions, %d bits' % (len(P), WIDTH))
    elif cmd == 'gen':
        gen()
    elif cmd == 'test':
        sys.exit(0 if test(int(sys.argv[2]) if len(sys.argv) > 2 else 2000) else 1)


if __name__ == '__main__':
    main()
