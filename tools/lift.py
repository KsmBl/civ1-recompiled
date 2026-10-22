#!/usr/bin/env python3
"""Static recompiler: lifts the x86-16 code of Civilization to C.

usage: lift.py GAMEDIR OUTDIR

Every function found by recursive descent (plus seeds from seeds.txt) is
translated into a C function operating on the emulated CPU state declared in
runtime/cpu.h.  The result is semantically equivalent to the original code
and is compiled together with the runtime into a native program.
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from modules import load_all, OVL_SEG, DGROUP          # noqa: E402
from x86dec import decode, is_jcc                     # noqa: E402

F_CF, F_PF, F_AF, F_ZF, F_SF, F_OF = 0x1, 0x4, 0x10, 0x40, 0x80, 0x800
F_ALL = F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF
F_ARITH = F_ALL

JCC_USE = {
    'jo': F_OF, 'jno': F_OF, 'jb': F_CF, 'jae': F_CF, 'je': F_ZF,
    'jne': F_ZF, 'jbe': F_CF | F_ZF, 'ja': F_CF | F_ZF, 'js': F_SF,
    'jns': F_SF, 'jp': F_PF, 'jnp': F_PF, 'jl': F_SF | F_OF,
    'jge': F_SF | F_OF, 'jle': F_ZF | F_SF | F_OF, 'jg': F_ZF | F_SF | F_OF,
}
JCC_FLAGCOND = {
    'jo': 'OF', 'jno': '!OF', 'jb': 'CF', 'jae': '!CF', 'je': 'ZF',
    'jne': '!ZF', 'jbe': '(CF || ZF)', 'ja': '(!CF && !ZF)', 'js': 'SF',
    'jns': '!SF', 'jp': 'PF', 'jnp': '!PF', 'jl': '(SF != OF)',
    'jge': '(SF == OF)', 'jle': '(ZF || SF != OF)',
    'jg': '(!ZF && SF == OF)',
}

TERMINATORS = {'jmp', 'ret', 'retf', 'iret', 'jmpf', 'hlt'}

STRING_OPS = {'movsb', 'movsw', 'stosb', 'stosw', 'lodsb', 'lodsw',
              'cmpsb', 'cmpsw', 'scasb', 'scasw', 'insb', 'insw', 'outsb',
              'outsw'}


def flag_effects(i):
    """Return (use, def) flag masks for an instruction."""
    mn = i.mn
    if mn in ('add', 'sub', 'cmp', 'neg'):
        return 0, F_ALL
    if mn in ('adc', 'sbb'):
        return F_CF, F_ALL
    if mn in ('inc', 'dec'):
        return 0, F_ALL & ~F_CF
    if mn in ('and', 'or', 'xor', 'test'):
        return 0, F_ALL
    if mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
        cnt = i.ops[1] if len(i.ops) > 1 else None
        use = F_CF if mn in ('rcl', 'rcr') else 0
        if mn in ('rol', 'ror', 'rcl', 'rcr'):
            d = F_CF | F_OF
        else:
            d = F_ALL
        if cnt is not None and cnt.kind == 'reg':
            # count may be zero -> flags unchanged: treat as use+def
            return use | d, d
        if cnt is not None and cnt.kind == 'imm' and (cnt.imm & 0x1f) == 0:
            return use, 0
        return use, d
    if mn in ('mul', 'imul'):
        return 0, F_ALL
    if mn in ('div', 'idiv'):
        return 0, F_ALL
    if is_jcc(mn):
        return JCC_USE[mn], 0
    if mn == 'pushf':
        return F_ALL, 0
    if mn == 'popf':
        return 0, F_ALL
    if mn == 'sahf':
        return 0, F_CF | F_PF | F_AF | F_ZF | F_SF
    if mn == 'lahf':
        return F_CF | F_PF | F_AF | F_ZF | F_SF, 0
    if mn in ('clc', 'stc'):
        return 0, F_CF
    if mn == 'cmc':
        return F_CF, F_CF
    if mn in ('cmpsb', 'cmpsw', 'scasb', 'scasw'):
        return 0, F_ALL
    if mn in ('daa', 'das'):
        return F_CF | F_AF, F_ALL
    if mn in ('aaa', 'aas'):
        return F_AF, F_ALL
    if mn in ('aam', 'aad'):
        return 0, F_ALL
    if mn == 'salc':
        return F_CF, 0
    if mn in ('call', 'callf', 'ovlcall'):
        # flags pass through calls: the callee may leave any of them
        # unchanged.  What it reads on entry is added by the liveness pass.
        return 0, 0
    if mn in ('int', 'into', 'int3'):
        # INT pushes the flags and IRET restores them
        return F_ALL, 0
    if mn == 'iret':
        return 0, F_ALL
    if mn == 'bad':
        return F_ALL, 0
    return 0, 0


class Func:
    def __init__(self, mod, seg, off):
        self.mod = mod
        self.seg = seg
        self.off = off
        self.insns = {}         # off -> Insn
        self.leaders = set()
        self.blocks = []        # list of lists of insns
        self.tables = {}        # insn off -> list of target offsets
        self.calls = []         # (insn, target key)
        self.name = func_name(mod, seg, off)
        self.indirect = False   # may be called indirectly
        self.retlive = 0

    @property
    def key(self):
        return (self.mod.cname, self.seg, self.off)


def func_name(mod, seg, off):
    if mod.kind == 'root':
        return 'f_%04x_%04x' % (seg, off)
    if mod.kind == 'ovl':
        return '%s_%04x' % (mod.cname, off)
    return '%s_%04x_%04x' % (mod.cname, seg, off)


class Program:
    def __init__(self, mods):
        self.mods = {m.cname: m for m in mods}
        self.root = self.mods['root']
        self.funcs = {}
        self.queue = []
        self.warnings = []
        self.indirect_seeds = set()

    def warn(self, s):
        self.warnings.append(s)

    def add_entry(self, mod, seg, off, indirect=False):
        key = (mod.cname, seg, off)
        if not mod.valid(seg, off):
            self.warn('entry outside module %s %04x:%04x' % key)
            return None
        if key not in self.funcs:
            f = Func(mod, seg, off)
            self.funcs[key] = f
            self.queue.append(f)
        f = self.funcs[key]
        if indirect:
            f.indirect = True
        return f

    def decode_at(self, mod, seg, off):
        o = mod.ofs(seg, off)
        if o < 0 or o >= len(mod.image):
            return None
        code = mod.image[o:o + 16]
        rel = {r - o + off for r in mod.relocs if o <= r < o + 16}
        return decode(bytes(code), off, rel)

    # ------------------------------------------------------------------
    def explore(self, f):
        mod, seg = f.mod, f.seg
        work = [f.off]
        f.leaders.add(f.off)
        while work:
            off = work.pop()
            while True:
                if off in f.insns:
                    f.leaders.add(off)
                    break
                i = self.decode_at(mod, seg, off)
                if i is None or i.mn in ('bad32', 'reloc_disp'):
                    if i is not None:
                        self.warn('unsupported insn in %s at %04x: %s' %
                                  (f.name, off, i.text))
                    f.insns[off] = None   # undecodable
                    break
                # INT 3Fh overlay call: CD 3F nn oooo
                if i.bytes[:2] == b'\xcd\x3f':
                    o = mod.ofs(seg, off)
                    ovl = mod.image[o + 2]
                    tgt = struct.unpack('<H', mod.image[o + 3:o + 5])[0]
                    i.mn = 'ovlcall'
                    i.size = 5
                    i.ovl_target = (ovl, tgt)
                    i.text = 'call overlay %d:%04x' % (ovl, tgt)
                    om = self.mods.get('ovl%02d' % ovl)
                    if om is None:
                        self.warn('bad overlay %d at %s' % (ovl, f.name))
                    else:
                        self.add_entry(om, OVL_SEG, tgt)
                f.insns[off] = i
                nxt = (off + i.size) & 0xFFFF
                mn = i.mn
                if mn == 'call' and i.target is not None:
                    self.add_entry(mod, seg, i.target)
                elif mn == 'callf' and i.far_target is not None:
                    s, o_, rel = i.far_target
                    tm = self.far_module(mod, s, rel)
                    if tm is not None:
                        self.add_entry(tm, s, o_)
                    else:
                        self.warn('far call to unknown %04x:%04x in %s' %
                                  (s, o_, f.name))
                elif mn == 'jmpf' and i.far_target is not None:
                    s, o_, rel = i.far_target
                    tm = self.far_module(mod, s, rel)
                    if tm is not None:
                        self.add_entry(tm, s, o_)
                if is_jcc(mn) or mn in ('loop', 'loope', 'loopne', 'jcxz'):
                    work.append(i.target)
                    f.leaders.add(i.target)
                    f.leaders.add(nxt)
                    off = nxt
                    continue
                if mn == 'jmp':
                    if i.target is not None:
                        work.append(i.target)
                        f.leaders.add(i.target)
                    else:
                        tbl = self.jump_table(f, i)
                        if tbl:
                            f.tables[off] = tbl
                            for t in tbl:
                                work.append(t)
                                f.leaders.add(t)
                    break
                if mn in TERMINATORS:
                    break
                if mn == 'int' and i.ops and i.ops[0].imm == 0x20:
                    break
                off = nxt

    def far_module(self, mod, seg, reloc):
        """Which module does a far reference with link-time segment `seg`
        made from module `mod` point to?"""
        if mod.kind == 'drv':
            return mod if reloc else None
        if not reloc:
            return None
        if seg == OVL_SEG:
            return mod if mod.kind == 'ovl' else None
        return self.root

    def jump_table(self, f, i):
        """Resolve `jmp word ptr cs:[reg+disp]` switch tables."""
        if not i.ops or i.ops[0].kind != 'mem':
            self.warn('unresolved indirect jmp at %s %04x: %s' %
                      (f.name, i.off, i.text))
            return None
        op = i.ops[0]
        if op.seg != 'cs' or op.index is not None or op.base is None:
            self.warn('unresolved indirect jmp at %s %04x: %s' %
                      (f.name, i.off, i.text))
            return None
        reg = op.base
        # look backwards for "cmp reg, N ; ja" (bound) and "shl reg,1"/add
        bound = None
        prev = sorted(o for o in f.insns if o < i.off and f.insns[o])
        chain = [f.insns[o] for o in prev[-12:]]
        for p in reversed(chain):
            if (p.mn == 'cmp' and p.ops[0].kind == 'reg' and
                    p.ops[1].kind == 'imm'):
                bound = p.ops[1].imm
                break
        if bound is None or bound > 512:
            self.warn('jump table without bound at %s %04x: %s' %
                      (f.name, i.off, i.text))
            n = 64
        else:
            n = bound + 1
        tbl = []
        for k in range(n):
            o = op.disp + 2 * k
            if not f.mod.valid(f.seg, o + 1):
                break
            t = f.mod.word(f.seg, o)
            if bound is None:
                # stop at implausible entries
                if t < f.off or t > op.disp + 2 * n or t == 0:
                    break
            tbl.append(t)
        return tbl

    def run(self):
        while self.queue:
            f = self.queue.pop()
            self.explore(f)
        for f in self.funcs.values():
            self.build_blocks(f)

    def build_blocks(self, f):
        entries = {k[2] for k in self.funcs if k[0] == f.mod.cname and
                   k[1] == f.seg}
        f.entries_other = entries - {f.off}
        offs = sorted(f.insns)
        blocks = []
        cur = None
        for o in offs:
            i = f.insns[o]
            if cur is None or o in f.leaders or o != cur_next:
                cur = []
                blocks.append(cur)
            cur.append((o, i))
            cur_next = o + (i.size if i else 1)
            if i is None or i.mn in TERMINATORS or is_jcc(i.mn) or \
                    i.mn in ('loop', 'loope', 'loopne', 'jcxz'):
                cur = None
        f.blocks = blocks


# ----------------------------------------------------------------------
# Liveness

def successors(f, blk):
    """Successor offsets of a block (fallthrough last)."""
    o, i = blk[-1]
    if i is None:
        return []
    nxt = (o + i.size) & 0xFFFF
    mn = i.mn
    if is_jcc(mn) or mn in ('loop', 'loope', 'loopne', 'jcxz'):
        return [i.target, nxt]
    if mn == 'jmp':
        if i.target is not None:
            return [i.target]
        return list(f.tables.get(o, []))
    if mn in TERMINATORS:
        return []
    if mn == 'int' and i.ops and i.ops[0].imm == 0x20:
        return []
    return [nxt]


def is_exit(f, blk):
    """Does the block leave the function (ret / tail call)?"""
    o, i = blk[-1]
    if i is None:
        return False
    return i.mn in ('ret', 'retf', 'iret', 'jmpf')


class Analysis:
    def __init__(self, prog):
        self.prog = prog
        self.entry_live = {}     # function key -> flags read before set

    def fusion_source(self, f, bidx_of, blk_index, idx):
        """Find the instruction whose flags a jcc at blk[idx] consumes, if
        that instruction directly precedes it (possibly through earlier
        jccs on the only path)."""
        blk = f.blocks[blk_index]
        k = idx - 1
        while True:
            if k >= 0:
                o, i = blk[k]
                if i is None:
                    return None
                if is_jcc(i.mn):
                    k -= 1
                    continue
                if i.mn in ('cmp', 'test', 'and', 'or', 'xor', 'sub',
                            'add', 'inc', 'dec'):
                    return i
                return None
            # previous block: single predecessor via fallthrough
            start = blk[0][0]
            preds = f.preds.get(start, [])
            if len(preds) != 1:
                return None
            pb = preds[0]
            pblk = f.blocks[pb]
            last = pblk[-1][1]
            if last is None or not is_jcc(last.mn):
                return None
            blk = pblk
            blk_index = pb
            k = len(blk) - 1

    def analyze(self, f):
        bidx = {blk[0][0]: n for n, blk in enumerate(f.blocks)}
        f.bidx = bidx
        preds = collections.defaultdict(list)
        for n, blk in enumerate(f.blocks):
            for s in successors(f, blk):
                if s in bidx:
                    preds[s].append(n)
        f.preds = preds
        # fusion decisions
        f.fused = {}
        for n, blk in enumerate(f.blocks):
            for k, (o, i) in enumerate(blk):
                if i is not None and is_jcc(i.mn):
                    src = self.fusion_source(f, bidx, n, k)
                    if src is not None and fusable(src, i.mn):
                        f.fused[o] = src

    def tail_targets(self, f, blk):
        o, i = blk[-1]
        if i is None:
            return []
        out = []
        mod, seg = f.mod, f.seg
        if i.mn == 'jmpf' and i.far_target is not None:
            s_, o_, rel = i.far_target
            tm = self.prog.far_module(mod, s_, rel)
            if tm is not None:
                out.append((tm.cname, s_, o_))
            return out
        for s_ in successors(f, blk):
            if s_ not in f.bidx or s_ in f.entries_other:
                out.append((mod.cname, seg, s_))
        return out

    def liveness(self, f, retlive_of):
        """Compute flags live-out for every instruction."""
        nb = len(f.blocks)
        live_in = [0] * nb
        succs = []
        for blk in f.blocks:
            ss = []
            for s in successors(f, blk):
                if s in f.bidx:
                    ss.append(f.bidx[s])
                else:
                    ss.append(None)    # leaves function / tail call
            succs.append(ss)
        changed = True
        f.live_after = {}
        while changed:
            changed = False
            for n in range(nb - 1, -1, -1):
                blk = f.blocks[n]
                out = 0
                for s in succs[n]:
                    out |= live_in[s] if s is not None else F_ALL
                o, i = blk[-1]
                if i is not None and i.mn in ('ret', 'retf'):
                    out |= f.retlive
                if i is not None and i.mn == 'jmpf':
                    out |= F_ALL
                # tail transfers: the target returns to our caller, so it
                # must preserve the flags our callers read
                for tk in self.tail_targets(f, blk):
                    retlive_of.setdefault(tk, 0)
                    retlive_of[tk] |= f.retlive
                live = out
                for o, i in reversed(blk):
                    f.live_after[o] = live
                    if i is None:
                        live = F_ALL
                        continue
                    use, d = flag_effects(i)
                    if o in f.fused:
                        use = 0
                    if i.mn in ('call', 'callf', 'ovlcall'):
                        tk = call_target_key(self.prog, f, i)
                        retlive_of.setdefault(tk, 0)
                        retlive_of[tk] |= live
                        use |= self.entry_live.get(tk, F_ALL)
                    live = (live & ~d) | use
                if live != live_in[n]:
                    live_in[n] = live
                    changed = True
        if f.off in f.bidx:
            self.entry_live[f.key] = live_in[f.bidx[f.off]]
        else:
            self.entry_live[f.key] = F_ALL


def call_target_key(prog, f, i):
    mod, seg = f.mod, f.seg
    if i.mn == 'call':
        if i.target is None:
            return None
        return (mod.cname, seg, i.target)
    if i.mn == 'callf':
        if i.far_target is None:
            return None
        s, o, rel = i.far_target
        tm = prog.far_module(mod, s, rel)
        return (tm.cname, s, o) if tm else None
    if i.mn == 'ovlcall':
        ovl, o = i.ovl_target
        return ('ovl%02d' % ovl, OVL_SEG, o)
    return None


def fusable(src, jcc):
    mn = src.mn
    if src.patched:
        return False
    if jcc in ('jo', 'jno', 'jp', 'jnp'):
        return False
    if mn == 'cmp':
        return True
    if mn in ('test', 'and', 'or', 'xor'):
        return True
    if mn in ('sub', 'add', 'inc', 'dec'):
        return jcc in ('je', 'jne', 'js', 'jns')
    return False


# ----------------------------------------------------------------------
# C emission

REG_C = {'ax': 'AX', 'bx': 'BX', 'cx': 'CX', 'dx': 'DX', 'si': 'SI',
         'di': 'DI', 'bp': 'BP', 'sp': 'SP', 'al': 'AL', 'ah': 'AH',
         'bl': 'BL', 'bh': 'BH', 'cl': 'CL', 'ch': 'CH', 'dl': 'DL',
         'dh': 'DH', 'cs': 'CS', 'ds': 'DS', 'es': 'ES', 'ss': 'SS'}


class Emitter:
    def __init__(self, prog, f):
        self.prog = prog
        self.f = f
        self.lines = []
        self.mod = f.mod
        self.cur = None

    def segconst(self, v, reloc):
        if reloc:
            return 'SEGR(0x%04x)' % v
        return '0x%04x' % v

    def memseg(self, op):
        i = self.cur
        if i is not None:
            k = ('seg', self.mod.cname, self.f.seg, i.off)
            e = getattr(self.prog, 'patches', {}).get(k)
            if e:
                return '((uint16_t)(%s))' % e
        return REG_C[op.seg]

    def ea(self, op):
        i = self.cur
        if i is not None and i.patched and i.disp_loc is not None and \
                i.disp_loc in i.patched:
            dyn = 'RW(CS, 0x%04x)' % i.disp_loc
            if i.disp_size == 1:
                dyn = '(int8_t)RB(CS, 0x%04x)' % i.disp_loc
            parts = []
            if op.base:
                parts.append(REG_C[op.base])
            if op.index:
                parts.append(REG_C[op.index])
            parts.append(dyn)
            return ' + '.join(parts)
        parts = []
        if op.base:
            parts.append(REG_C[op.base])
        if op.index:
            parts.append(REG_C[op.index])
        dk = ('disp', self.mod.cname, self.f.seg, i.off) if i is not None else None
        de = getattr(self.prog, 'patches', {}).get(dk)
        if de:
            parts.append('(uint16_t)(%s)' % de)
            return ' + '.join(parts)
        if op.disp or not parts:
            if parts and op.disp >= 0x8000:
                parts.append('- 0x%x' % (0x10000 - op.disp))
            else:
                parts.append('0x%x' % op.disp)
        s = ' + '.join(parts).replace('+ -', '-')
        return s

    def rd(self, op, size=None):
        size = size or op.size
        if op.kind == 'reg':
            return REG_C[op.reg]
        if op.kind == 'imm':
            i = self.cur
            if i is not None and i.patched and i.imm_loc is not None and \
                    i.imm_loc in i.patched:
                if i.imm_size == 1:
                    return 'RB(CS, 0x%04x)' % i.imm_loc
                return 'RW(CS, 0x%04x)' % i.imm_loc
            if op.reloc:
                return self.segconst(op.imm, True)
            pk = (self.mod.cname, self.f.seg, i.off) if i is not None else None
            if pk in getattr(self.prog, 'patches', {}):
                t = 'uint8_t' if size == 1 else 'uint16_t'
                return '((%s)(%s))' % (t, self.prog.patches[pk])
            if size == 1:
                return '0x%02x' % (op.imm & 0xFF)
            return '0x%04x' % (op.imm & 0xFFFF)
        seg = self.memseg(op)
        if size == 1:
            return 'RB(%s, %s)' % (seg, self.ea(op))
        return 'RW(%s, %s)' % (seg, self.ea(op))

    def wr(self, op, val, size=None):
        size = size or op.size
        if op.kind == 'reg':
            return '%s = %s;' % (REG_C[op.reg], val)
        seg = self.memseg(op)
        if size == 1:
            return 'WB(%s, %s, %s);' % (seg, self.ea(op), val)
        return 'WW(%s, %s, %s);' % (seg, self.ea(op), val)

    def emit(self, s):
        self.lines.append(s)

    # --------------------------------------------------------------
    def cond_expr(self, src, jcc):
        """C condition for a jcc fused with its flag source."""
        saved = self.cur
        self.cur = src          # operands (and patches) belong to the source
        try:
            return self._cond_expr(src, jcc)
        finally:
            self.cur = saved

    def _cond_expr(self, src, jcc):
        sz = src.ops[0].size if src.ops else 2
        if src.mn in ('inc', 'dec') or len(src.ops) < 2:
            a = self.rd(src.ops[0])
            res = a
            kind = 'result'
        else:
            a = self.rd(src.ops[0], sz)
            b = self.rd(src.ops[1], sz)
            if src.mn == 'cmp':
                kind = 'cmp'
            elif src.mn == 'test':
                res = '(%s & %s)' % (a, b)
                kind = 'result'
            else:
                res = a
                kind = 'result'
        u = 'uint8_t' if sz == 1 else 'uint16_t'
        s = 'int8_t' if sz == 1 else 'int16_t'
        if kind == 'cmp':
            ca, cb = '(%s)%s' % (u, a), '(%s)%s' % (u, b)
            sa, sb = '(%s)%s' % (s, a), '(%s)%s' % (s, b)
            m = {'je': '%s == %s' % (ca, cb), 'jne': '%s != %s' % (ca, cb),
                 'jb': '%s < %s' % (ca, cb), 'jae': '%s >= %s' % (ca, cb),
                 'jbe': '%s <= %s' % (ca, cb), 'ja': '%s > %s' % (ca, cb),
                 'jl': '%s < %s' % (sa, sb), 'jge': '%s >= %s' % (sa, sb),
                 'jle': '%s <= %s' % (sa, sb), 'jg': '%s > %s' % (sa, sb),
                 'js': '(%s)(%s - %s) < 0' % (s, ca, cb),
                 'jns': '(%s)(%s - %s) >= 0' % (s, ca, cb)}
            return m[jcc]
        r = '(%s)%s' % (u, res)
        sr = '(%s)%s' % (s, res)
        if src.mn in ('inc', 'dec', 'add', 'sub'):
            m = {'je': '%s == 0' % r, 'jne': '%s != 0' % r,
                 'js': '%s < 0' % sr, 'jns': '%s >= 0' % sr}
            return m[jcc]
        # logic ops: CF = OF = 0
        m = {'je': '%s == 0' % r, 'jne': '%s != 0' % r,
             'js': '%s < 0' % sr, 'jns': '%s >= 0' % sr,
             'jl': '%s < 0' % sr, 'jge': '%s >= 0' % sr,
             'jle': '%s <= 0' % sr, 'jg': '%s > 0' % sr,
             'jb': '0', 'jae': '1', 'jbe': '%s == 0' % r,
             'ja': '%s != 0' % r}
        return m[jcc]

    def goto(self, tgt):
        f = self.f
        if tgt in f.bidx:
            return 'goto L_%04x;' % tgt
        return self.tail(tgt)

    def tail(self, tgt):
        key = (self.mod.cname, self.f.seg, tgt)
        fn = self.prog.funcs.get(key)
        if fn is None:
            return 'JUMP_NEAR(0x%04x); return;' % tgt
        return '%s(); return;' % fn.name

    def lift(self):
        f = self.f
        used_labels = set()
        for blk in f.blocks:
            for o, i in blk:
                if i is None:
                    continue
                if i.target is not None and i.mn != 'call':
                    used_labels.add(i.target)
                for t in f.tables.get(o, []):
                    used_labels.add(t)
        name = f.name
        if name in getattr(self.prog, 'overrides', set()):
            name = 'lifted_' + name    # replaced by src/native code
        self.emit('void %s(void)' % name)
        self.emit('{')
        if f.off not in f.bidx:
            self.emit('    BAD_CODE(0x%04x);' % f.off)
        # entry block first
        order = sorted(range(len(f.blocks)),
                       key=lambda n: (f.blocks[n][0][0] != f.off,
                                      f.blocks[n][0][0]))
        prev_fall = None
        for n in order:
            blk = f.blocks[n]
            start = blk[0][0]
            if prev_fall is not None and prev_fall != start:
                self.emit('    ' + self.goto(prev_fall))
            prev_fall = None
            if start in used_labels or start != f.off:
                self.emit('L_%04x:' % start)
            for k, (o, i) in enumerate(blk):
                if i is None:
                    self.emit('    BAD_CODE(0x%04x);' % o)
                    break
                if o != start and o in f.entries_other and k == 0:
                    pass
                live = f.live_after.get(o, F_ALL)
                self.cur = i
                self.emit('    /* %04x: %s */' % (o, i.text))
                hk = self.prog.hooks.get((f.mod.cname, f.seg, o))
                if hk:
                    self.emit('    %s();' % hk)
                try:
                    self.insn(i, live)
                except (NotImplementedError, KeyError, TypeError,
                        ValueError, AttributeError) as e:
                    self.emit('    UNIMPL(0x%04x); /* %s */' % (o, e))
                    self.prog.warn('unimplemented %s in %s at %04x' %
                                   (i.text, f.name, o))
            # fallthrough
            o, i = blk[-1]
            if i is None:
                continue
            ss = successors(f, blk)
            mn = i.mn
            nxt = (o + i.size) & 0xFFFF
            fall = None
            if is_jcc(mn) or mn in ('loop', 'loope', 'loopne', 'jcxz'):
                fall = nxt
            elif mn not in TERMINATORS and not (
                    mn == 'int' and i.ops and i.ops[0].imm == 0x20):
                fall = nxt
            if fall is not None:
                if fall in f.entries_other:
                    self.emit('    ' + self.tail(fall))
                elif fall in f.bidx:
                    prev_fall = fall
                else:
                    self.emit('    ' + self.tail(fall))
        if prev_fall is not None:
            self.emit('    ' + self.goto(prev_fall))
        self.emit('}')
        return self.lines

    # --------------------------------------------------------------
    def insn(self, i, live):
        mn = i.mn
        ops = i.ops
        E = lambda s: self.emit('    ' + s)      # noqa: E731
        if i.patched and mn not in ('jmpf', 'callf', 'jmp', 'call', 'ret',
                                    'retf', 'iret', 'int', 'ovlcall') and \
                not is_jcc(mn) and mn not in ('loop', 'loope', 'loopne',
                                              'jcxz'):
            # the program rewrites this instruction at run time
            E('INTERP_STEP(0x%04x);' % i.off)
            return
        use, d = flag_effects(i)
        m = d & live
        nxt = (i.off + i.size) & 0xFFFF
        sz = ops[0].size if ops else 2
        W = 'B' if sz == 1 else 'W'
        mask = flagmask(m)

        if mn == 'mov':
            E(self.wr(ops[0], self.rd(ops[1], ops[0].size)))
        elif mn in ('add', 'sub', 'adc', 'sbb', 'and', 'or', 'xor'):
            a = self.rd(ops[0])
            b = self.rd(ops[1], ops[0].size)
            if mn == 'xor' and ops[0].kind == 'reg' and \
                    ops[1].kind == 'reg' and ops[0].reg == ops[1].reg:
                E(self.wr(ops[0], '0'))
                if m:
                    E('SET_FLAGS_LOGIC%s(0, %s);' % (W, mask))
                return
            if m == 0 and mn in ('add', 'sub', 'and', 'or', 'xor'):
                cop = {'add': '+', 'sub': '-', 'and': '&', 'or': '|',
                       'xor': '^'}[mn]
                if ops[0].kind == 'reg':
                    E('%s %s= %s;' % (a, cop, b))
                else:
                    E(self.wr(ops[0], '%s %s %s' % (a, cop, b)))
            else:
                E(self.wr(ops[0], '%s%s(%s, %s, %s)' %
                          (mn.upper(), sz * 8, a, b, mask)))
        elif mn == 'cmp':
            if m:
                E('CMP%s(%s, %s, %s);' % (sz * 8, self.rd(ops[0]),
                                          self.rd(ops[1], ops[0].size), mask))
        elif mn == 'test':
            if m:
                E('SET_FLAGS_LOGIC%s(%s & %s, %s);' % (
                    W, self.rd(ops[0]), self.rd(ops[1], ops[0].size), mask))
        elif mn in ('inc', 'dec'):
            a = self.rd(ops[0])
            if m == 0:
                if ops[0].kind == 'reg':
                    E('%s%s;' % (a, '++' if mn == 'inc' else '--'))
                else:
                    E(self.wr(ops[0], '%s %s 1' % (a, '+' if mn == 'inc'
                                                   else '-')))
            else:
                E(self.wr(ops[0], '%s%s(%s, %s)' % (mn.upper(), sz * 8, a,
                                                    mask)))
        elif mn == 'neg':
            a = self.rd(ops[0])
            if m == 0:
                E(self.wr(ops[0], '-%s' % a))
            else:
                E(self.wr(ops[0], 'NEG%s(%s, %s)' % (sz * 8, a, mask)))
        elif mn == 'not':
            E(self.wr(ops[0], '~%s' % self.rd(ops[0])))
        elif mn in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
            a = self.rd(ops[0])
            if len(ops) > 1:
                c = self.rd(ops[1], 1)
            else:
                c = '1'
            op = 'shl' if mn == 'sal' else mn
            if m == 0 and op in ('shl', 'shr', 'sar') and \
                    (len(ops) == 1 or ops[1].kind == 'imm'):
                cnt = int(c, 0) & 0x1f
                if op == 'shl':
                    E(self.wr(ops[0], '%s << %d' % (a, cnt)))
                elif op == 'shr':
                    E(self.wr(ops[0], '%s >> %d' % (a, cnt)))
                else:
                    t = 'int8_t' if sz == 1 else 'int16_t'
                    E(self.wr(ops[0], '(%s)%s >> %d' % (t, a, cnt)))
            else:
                E(self.wr(ops[0], '%s%s(%s, %s, %s)' % (op.upper(), sz * 8,
                                                        a, c, mask)))
        elif mn in ('mul', 'imul'):
            if len(ops) == 1:
                E('%s%s(%s, %s);' % (mn.upper(), sz * 8, self.rd(ops[0]),
                                     mask))
            elif len(ops) == 2:
                E(self.wr(ops[0], 'IMUL16X(%s, %s, %s)' % (
                    self.rd(ops[0]), self.rd(ops[1], 2), mask)))
            else:
                E(self.wr(ops[0], 'IMUL16X(%s, %s, %s)' % (
                    self.rd(ops[1]), self.rd(ops[2], 2), mask)))
        elif mn in ('div', 'idiv'):
            E('%s%s(%s);' % (mn.upper(), sz * 8, self.rd(ops[0])))
        elif mn == 'cbw':
            E('AX = (int16_t)(int8_t)AL;')
        elif mn == 'cwd':
            E('DX = (AX & 0x8000) ? 0xffff : 0;')
        elif mn == 'lea':
            E(self.wr(ops[0], '(uint16_t)(%s)' % self.ea(ops[1])))
        elif mn in ('les', 'lds'):
            op = ops[1]
            seg = self.memseg(op)
            ea = self.ea(op)
            sr = 'ES' if mn == 'les' else 'DS'
            # read both before writing (dest may be part of address)
            E('{ uint16_t o_ = RW(%s, %s), s_ = RW(%s, (uint16_t)(%s) + 2); '
              '%s = o_; %s = s_; }' % (seg, ea, seg, ea, REG_C[ops[0].reg],
                                       sr))
        elif mn == 'xchg':
            a, b = ops
            E('{ uint%d_t t_ = %s; %s %s }' % (
                sz * 8, self.rd(a), self.wr(a, self.rd(b)), self.wr(b, 't_')))
        elif mn == 'push':
            if ops[0].kind == 'reg' and ops[0].reg == 'sp':
                E('PUSH(SP);')     # 8086 pushes new SP; 286 old: use 286
            else:
                E('PUSH(%s);' % self.rd(ops[0], 2))
        elif mn == 'pop':
            if ops[0].kind == 'mem':
                E('{ uint16_t t_ = POP(); %s }' % self.wr(ops[0], 't_'))
            else:
                E(self.wr(ops[0], 'POP()'))
        elif mn == 'pusha':
            E('PUSHA();')
        elif mn == 'popa':
            E('POPA();')
        elif mn == 'pushf':
            E('PUSH(GET_FLAGS());')
        elif mn == 'popf':
            E('SET_FLAGS(POP());')
        elif mn == 'lahf':
            E('AH = (uint8_t)GET_FLAGS();')
        elif mn == 'sahf':
            E('SET_FLAGS_LO(AH);')
        elif mn == 'clc':
            E('CF = 0;')
        elif mn == 'stc':
            E('CF = 1;')
        elif mn == 'cmc':
            E('CF = !CF;')
        elif mn == 'cld':
            E('DF = 0;')
        elif mn == 'std':
            E('DF = 1;')
        elif mn == 'cli':
            E('CLI();')
        elif mn == 'sti':
            E('STI();')
        elif mn in ('nop', 'wait', 'fwait', 'lock'):
            E(';')
        elif mn == 'hlt':
            E('HLT(0x%04x);' % i.off)
        elif mn == 'xlatb' or mn == 'xlat':
            seg = REG_C[i.segov] if i.segov else 'DS'
            E('AL = RB(%s, BX + AL);' % seg)
        elif mn == 'enter':
            E('ENTER(0x%x, %d);' % (ops[0].imm, ops[1].imm))
        elif mn == 'leave':
            E('SP = BP; BP = POP();')
        elif mn in STRING_OPS:
            seg = REG_C[i.segov] if i.segov else 'DS'
            rep = ''
            if i.rep == 'rep':
                rep = 'REP_' if mn not in ('cmpsb', 'cmpsw', 'scasb',
                                           'scasw') else 'REPE_'
            elif i.rep == 'repne':
                rep = 'REPNE_'
                if mn not in ('cmpsb', 'cmpsw', 'scasb', 'scasw'):
                    rep = 'REP_'
            E('%s%s(%s);' % (rep, mn.upper(), seg))
        elif mn in ('in',):
            port = 'DX' if ops[1].kind == 'reg' else '0x%02x' % ops[1].imm
            if ops[0].size == 1:
                E('AL = port_in8(%s);' % port)
            else:
                E('AX = port_in16(%s);' % port)
            E('IRQ_POLL();')
        elif mn == 'out':
            port = 'DX' if ops[0].kind == 'reg' else '0x%02x' % ops[0].imm
            if ops[1].size == 1:
                E('port_out8(%s, AL);' % port)
            else:
                E('port_out16(%s, AX);' % port)
        elif mn in ('daa', 'das', 'aaa', 'aas'):
            E('%s();' % mn.upper())
        elif mn in ('aam', 'aad'):
            E('%s(0x%02x);' % (mn.upper(), ops[0].imm if ops else 10))
        elif mn == 'salc':
            E('AL = CF ? 0xff : 0;')
        elif is_jcc(mn):
            src = self.f.fused.get(i.off)
            if src is not None:
                cond = self.cond_expr(src, mn)
            else:
                cond = JCC_FLAGCOND[mn]
            back = '' if i.target > i.off else ' IRQ_POLL();'
            if back:
                E('if (%s) {%s %s }' % (cond, back, self.goto(i.target)))
            else:
                E('if (%s) %s' % (cond, self.goto(i.target)))
        elif mn == 'jcxz':
            E('if (CX == 0) %s' % self.goto(i.target))
        elif mn in ('loop', 'loope', 'loopne'):
            c = {'loop': '--CX != 0', 'loope': '--CX != 0 && ZF',
                 'loopne': '--CX != 0 && !ZF'}[mn]
            if i.target <= i.off:
                E('if (%s) { IRQ_POLL(); %s }' % (c, self.goto(i.target)))
            else:
                E('if (%s) %s' % (c, self.goto(i.target)))
        elif mn == 'jmp':
            if i.target is not None:
                if i.target <= i.off:
                    E('IRQ_POLL();')
                E(self.goto(i.target))
            elif i.off in self.f.tables:
                tgt = self.rd(ops[0])
                E('switch (%s) {' % tgt)
                for t in sorted(set(self.f.tables[i.off])):
                    E('case 0x%04x: %s' % (t, self.goto(t)))
                E('default: JUMP_NEAR(%s); return;' % tgt)
                E('}')
            else:
                E('JUMP_NEAR(%s); return;' % self.rd(ops[0], 2))
        elif mn == 'call':
            if i.target is not None:
                fn = self.prog.funcs.get((self.mod.cname, self.f.seg,
                                          i.target))
                if fn is None:
                    E('CALL_NEAR(0x%04x, 0x%04x);' % (i.target, nxt))
                else:
                    E('CALLN(0x%04x, %s);' % (nxt, fn.name))
            else:
                E('CALL_NEAR(%s, 0x%04x);' % (self.rd(ops[0], 2), nxt))
        elif mn == 'callf':
            if i.far_target is not None:
                s, o, rel = i.far_target
                tm = self.prog.far_module(self.mod, s, rel)
                fn = self.prog.funcs.get((tm.cname, s, o)) if tm else None
                p = i.off + i.size - 4
                if i.patched or not rel:
                    E('CALL_FAR(RW(CS, 0x%04x), RW(CS, 0x%04x), 0x%04x);' % (
                        p + 2, p, nxt))
                elif fn is None:
                    E('CALL_FAR(%s, 0x%04x, 0x%04x);' % (
                        self.segconst(s, rel), o, nxt))
                else:
                    E('CALLF(%s, 0x%04x, %s);' % (
                        self.segconst(s, rel), nxt, fn.name))
            else:
                op = ops[0]
                seg = self.memseg(op)
                ea = self.ea(op)
                E('{ uint16_t o_ = RW(%s, %s), s_ = RW(%s, (uint16_t)(%s) + 2);'
                  ' CALL_FAR(s_, o_, 0x%04x); }' % (seg, ea, seg, ea, nxt))
        elif mn == 'jmpf':
            if i.far_target is not None:
                s, o, rel = i.far_target
                tm = self.prog.far_module(self.mod, s, rel)
                fn = self.prog.funcs.get((tm.cname, s, o)) if tm else None
                p = i.off + i.size - 4
                if i.patched or not rel:
                    E('JUMP_FAR(RW(CS, 0x%04x), RW(CS, 0x%04x)); return;' % (
                        p + 2, p))
                elif fn is None:
                    E('JUMP_FAR(%s, 0x%04x); return;' % (
                        self.segconst(s, rel), o))
                else:
                    E('JMPF(%s, %s); return;' % (self.segconst(s, rel),
                                                 fn.name))
            else:
                op = ops[0]
                seg = self.memseg(op)
                ea = self.ea(op)
                E('{ uint16_t o_ = RW(%s, %s), s_ = RW(%s, (uint16_t)(%s) + 2);'
                  ' JUMP_FAR(s_, o_); } return;' % (seg, ea, seg, ea))
        elif mn == 'ovlcall':
            ovl, o = i.ovl_target
            key = ('ovl%02d' % ovl, OVL_SEG, o)
            fn = self.prog.funcs.get(key)
            if fn is None:
                E('OVL_CALL_ID(%d, 0x%04x, 0x%04x);' % (ovl, o, nxt))
            else:
                E('OVL_CALL(%d, %s, 0x%04x);' % (ovl, fn.name, nxt))
        elif mn == 'ret':
            n = ops[0].imm if ops else 0
            E('RET_NEAR(%d); return;' % n)
        elif mn == 'retf':
            n = ops[0].imm if ops else 0
            E('RET_FAR(%d); return;' % n)
        elif mn == 'iret':
            E('IRET(); return;')
        elif mn == 'int':
            n = ops[0].imm
            E('INT(0x%02x, 0x%04x);' % (n, nxt))
        elif mn == 'int3':
            E('INT(0x03, 0x%04x);' % nxt)
        elif mn == 'into':
            E('if (OF) INT(0x04, 0x%04x);' % nxt)
        elif mn == 'bound':
            E('BOUND(%s, %s, %s, 0x%04x);' % (
                self.rd(ops[0]), REG_C[ops[1].seg], self.ea(ops[1]), i.off))
        else:
            raise NotImplementedError(mn)


def flagmask(m):
    if m == 0:
        return '0'
    if m == F_ALL:
        return 'F_ALL'
    names = []
    for bit, n in ((F_CF, 'F_CF'), (F_PF, 'F_PF'), (F_AF, 'F_AF'),
                   (F_ZF, 'F_ZF'), (F_SF, 'F_SF'), (F_OF, 'F_OF')):
        if m & bit:
            names.append(n)
    return '|'.join(names)


# ----------------------------------------------------------------------

def load_seeds(prog, path):
    if not os.path.exists(path):
        return
    for line in open(path):
        line = line.split('#')[0].strip()
        if not line:
            continue
        mod, addr = line.split()
        seg, off = (int(x, 16) for x in addr.split(':'))
        m = prog.mods.get(mod)
        if m is None:
            print('seed: unknown module', mod)
            continue
        prog.add_entry(m, seg, off, indirect=True)


def seed_data_pointers(prog):
    """Far code pointers stored in data (relocated words outside code)."""
    root = prog.root
    img = root.image
    for r in sorted(root.relocs):
        if r < DGROUP * 16:
            continue
        seg = struct.unpack('<H', img[r:r + 2])[0]
        off = struct.unpack('<H', img[r - 2:r])[0]
        if seg >= DGROUP or seg == OVL_SEG:
            continue
        o = seg * 16 + off
        if o + 3 < len(img) and img[o:o + 3] in (b'\x55\x8b\xec',):
            prog.add_entry(root, seg, off, indirect=True)


def seed_drivers(prog):
    """Driver modules start with a name, then a table of entry points."""
    for m in prog.mods.values():
        if m.kind != 'drv':
            continue
        n = struct.unpack('<H', m.image[0x30:0x32])[0]
        for k in range(n):
            off = struct.unpack('<H', m.image[0x32 + 2 * k:0x34 + 2 * k])[0]
            prog.add_entry(m, 0, off, indirect=True)


def find_patched_code(prog):
    """Mark instructions whose bytes are overwritten through CS: stores
    with a constant address (self-modifying thunks)."""
    writes = collections.defaultdict(set)
    for f in prog.funcs.values():
        # does the function point DS at its own code segment?
        ds_is_cs = False
        seq = [f.insns[o] for o in sorted(f.insns) if f.insns[o]]
        for a, b in zip(seq, seq[1:]):
            if a.mn == 'mov' and a.ops[1].kind == 'reg' and \
                    a.ops[1].reg == 'cs' and b.mn == 'mov' and \
                    b.ops[0].kind == 'reg' and b.ops[0].reg == 'ds' and \
                    b.ops[1].kind == 'reg' and b.ops[1].reg == a.ops[0].reg:
                ds_is_cs = True
            if a.mn == 'push' and a.ops[0].kind == 'reg' and \
                    a.ops[0].reg == 'cs' and b.mn == 'pop' and \
                    b.ops[0].kind == 'reg' and b.ops[0].reg == 'ds':
                ds_is_cs = True
        for o, i in f.insns.items():
            if i is None or not i.ops:
                continue
            op = i.ops[0]
            if op.kind != 'mem' or op.base or op.index:
                continue
            if not (op.seg == 'cs' or (ds_is_cs and op.seg == 'ds')):
                continue
            if i.mn in ('cmp', 'test', 'push', 'jmp', 'call', 'callf',
                        'jmpf'):
                continue
            for k in range(op.size or 2):
                writes[(f.mod.cname, f.seg)].add((op.disp + k) & 0xFFFF)
    count = 0
    for f in prog.funcs.values():
        w = writes.get((f.mod.cname, f.seg))
        if not w:
            continue
        for o, i in f.insns.items():
            if i is None:
                continue
            hit = {a for a in range(o, o + i.size) if a in w}
            if hit:
                i.patched = hit
                count += 1
                if i.mn not in ('jmpf', 'callf', 'jmp', 'call', 'mov', 'cmp',
                                'add', 'sub', 'push', 'and', 'or') or (
                        i.mn in ('jmp', 'call') and i.target is not None):
                    prog.warn('patched insn %s %04x: %s' % (f.name, o,
                                                            i.text))
    return count


def code_segments(prog):
    """(module, seg, start_off, end_off) ranges that contain code."""
    root = prog.root
    segs = set()
    img = root.image
    for r in root.relocs:
        v = struct.unpack('<H', img[r:r + 2])[0]
        if v < DGROUP and v != OVL_SEG:
            segs.add(v)
    segs.add(prog.root.entry[0])
    segs = sorted(segs)
    bounds = segs + [DGROUP]
    out = []
    for a, b in zip(bounds, bounds[1:]):
        if a == OVL_SEG:
            continue
        if a < OVL_SEG < b:
            b = OVL_SEG
        out.append((root, a, 0, (b - a) * 16))
    for m in prog.mods.values():
        if m.kind == 'ovl':
            out.append((m, OVL_SEG, 0, len(m.image)))
        elif m.kind == 'drv':
            out.append((m, 0, 0, len(m.image)))
    return out


def seed_gaps(prog):
    """Seed code that follows an unconditional transfer but was not reached
    (functions only called through pointers).  Returns number of seeds."""
    covered = collections.defaultdict(set)
    term_end = collections.defaultdict(set)
    for f in prog.funcs.values():
        k = (f.mod.cname, f.seg)
        for o, i in f.insns.items():
            if i is None:
                continue
            covered[k].update(range(o, o + i.size))
            if i.mn in ('ret', 'retf', 'iret', 'jmp', 'jmpf'):
                term_end[k].add(o + i.size)
    n = 0
    for mod, seg, lo, hi in code_segments(prog):
        k = (mod.cname, seg)
        cov = covered[k]
        if mod.kind == 'drv':
            lo = 0x32 + 2 * struct.unpack('<H', mod.image[0x30:0x32])[0]
        starts = set(term_end[k])
        # prologues anywhere in uncovered space
        base = mod.ofs(seg, 0)
        for off in range(lo, hi - 3):
            if off not in cov and mod.image[base + off:base + off + 3] == \
                    b'\x55\x8b\xec':
                starts.add(off)
        for off in sorted(starts):
            if off < lo or off >= hi:
                continue
            # skip alignment padding
            p = off
            while p < hi and p - off < 16 and \
                    mod.image[base + p] in (0x90, 0x00, 0xCC) and \
                    p not in cov:
                p += 1
            if p >= hi or p in cov:
                continue
            if mod.image[base + p:base + p + 2] == b'\x00\x00':
                continue
            i = prog.decode_at(mod, seg, p)
            if i is None or i.mn in ('bad32', 'reloc_disp'):
                continue
            if (mod.cname, seg, p) not in prog.funcs:
                prog.add_entry(mod, seg, p, indirect=True)
                n += 1
    return n


def load_patches(path):
    """tools/patches.txt: MODULE SEG:OFF C-EXPRESSION  (replaces the
    immediate operand of that instruction)"""
    out = {}
    if not os.path.exists(path):
        return out
    for line in open(path):
        line = line.split('#')[0].strip()
        if not line:
            continue
        mod, addr, expr = line.split(None, 2)
        seg, off = (int(x, 16) for x in addr.split(':'))
        if expr.startswith('seg '):
            # replace the segment of the instruction's memory operand
            out[('seg', mod, seg, off)] = expr[4:].strip()
        elif expr.startswith('disp '):
            # replace the displacement of the instruction's memory operand
            out[('disp', mod, seg, off)] = expr[5:].strip()
        else:
            out[(mod, seg, off)] = expr.strip()
    return out


def load_hooks(path):
    """tools/hooks.txt: MODULE SEG:OFF FUNCTION -- call a native C function
    right before that instruction (it may change registers)"""
    out = {}
    if os.path.exists(path):
        for line in open(path):
            line = line.split('#')[0].strip()
            if not line:
                continue
            mod, addr, fn = line.split()
            seg, off = (int(x, 16) for x in addr.split(':'))
            out[(mod, seg, off)] = fn
    return out


def load_overrides(path):
    out = set()
    if os.path.exists(path):
        for line in open(path):
            line = line.split('#')[0].strip()
            if line:
                out.add(line)
    return out


def build(gamedir):
    """Discover, analyse and compute liveness for all code."""
    mods = load_all(gamedir)
    prog = Program(mods)
    here = os.path.dirname(os.path.abspath(__file__))
    prog.patches = load_patches(os.path.join(here, 'patches.txt'))
    prog.overrides = load_overrides(os.path.join(here, 'overrides.txt'))
    prog.hooks = load_hooks(os.path.join(here, 'hooks.txt'))
    cs, ip = prog.root.entry
    prog.add_entry(prog.root, cs, ip, indirect=True)
    seed_data_pointers(prog)
    seed_drivers(prog)
    load_seeds(prog, os.path.join(os.path.dirname(__file__), 'seeds.txt'))
    prog.run()
    for it in range(30):
        n = seed_gaps(prog)
        if not n:
            break
        prog.run()
    prog.patched_count = find_patched_code(prog)
    an = Analysis(prog)
    funcs = list(prog.funcs.values())
    for f in funcs:
        an.analyze(f)
    # interprocedural flag liveness at returns
    for f in funcs:
        f.retlive = F_ALL if f.indirect else 0
    for it in range(60):
        retlive_of = {}
        before = dict(an.entry_live)
        for f in funcs:
            an.liveness(f, retlive_of)
        changed = before != an.entry_live
        for f in funcs:
            nl = (F_ALL if f.indirect else 0) | retlive_of.get(f.key, 0)
            if nl != f.retlive:
                f.retlive = nl
                changed = True
        if not changed:
            break
    return prog, funcs


def main():
    gamedir, outdir = sys.argv[1], sys.argv[2]
    prog, funcs = build(gamedir)
    print('patched instructions:', prog.patched_count)
    write_output(prog, funcs, outdir)
    with open(os.path.join(outdir, 'lift_warnings.txt'), 'w') as w:
        for s in prog.warnings:
            w.write(s + '\n')
    print('%d functions, %d warnings' % (len(funcs), len(prog.warnings)))


def write_output(prog, funcs, outdir):
    os.makedirs(outdir, exist_ok=True)
    groups = collections.defaultdict(list)
    for f in funcs:
        if f.mod.kind == 'root':
            g = 'root_%04x' % f.seg
        else:
            g = f.mod.cname
        groups[g].append(f)
    with open(os.path.join(outdir, 'funcs.h'), 'w') as h:
        h.write('/* generated by tools/lift.py -- do not edit */\n')
        h.write('#pragma once\n')
        for hk in sorted(set(prog.hooks.values())):
            h.write('void %s(void);\n' % hk)
        for f in sorted(funcs, key=lambda f: f.name):
            h.write('void %s(void);\n' % f.name)
            if f.name in prog.overrides:
                h.write('void lifted_%s(void);\n' % f.name)
    for g, fl in groups.items():
        with open(os.path.join(outdir, g + '.c'), 'w') as c:
            c.write('/* generated by tools/lift.py -- do not edit */\n')
            c.write('#include "cpu.h"\n#include "funcs.h"\n#include "wide.h"\n')
            mod = fl[0].mod
            if mod.kind == 'drv':
                c.write('#define SEGR(v) ((uint16_t)(modbase[MOD_%s] + (v)))\n'
                        % mod.cname.upper())
            else:
                c.write('#define SEGR(v) ((uint16_t)(LOADSEG + (v)))\n')
            if mod.kind == 'ovl':
                c.write('#undef JUMP_NEAR\n#define JUMP_NEAR(o) '
                        'jump_near_ovl(%d, (o))\n' % mod.ovl)
            c.write('\n')
            for f in sorted(fl, key=lambda f: (f.seg, f.off)):
                em = Emitter(prog, f)
                c.write('\n'.join(em.lift()))
                c.write('\n\n')
    # dispatch table
    with open(os.path.join(outdir, 'dispatch.c'), 'w') as c:
        c.write('/* generated by tools/lift.py -- do not edit */\n')
        c.write('#include "cpu.h"\n#include "funcs.h"\n\n')
        c.write('const struct func_entry func_table[] = {\n')
        for f in sorted(funcs, key=lambda f: (f.mod.cname, f.seg, f.off)):
            c.write('    { MOD_%s, 0x%04x, 0x%04x, %s },\n' % (
                f.mod.cname.upper(), f.seg, f.off, f.name))
        c.write('};\nconst int func_table_len = %d;\n' % len(funcs))
    # module list
    with open(os.path.join(outdir, 'modules.h'), 'w') as h:
        h.write('/* generated by tools/lift.py -- do not edit */\n')
        h.write('#pragma once\nenum {\n')
        for m in prog.mods.values():
            h.write('    MOD_%s,\n' % m.cname.upper())
        h.write('    MOD_COUNT\n};\n')
        h.write('#define MOD_NAMES { %s }\n' % ', '.join(
            '"%s"' % m.name for m in prog.mods.values()))


if __name__ == '__main__':
    main()
