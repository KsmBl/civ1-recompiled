"""Thin normalisation layer over capstone for 8086/80286 real-mode code."""
import capstone
from capstone import x86

_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
_md.detail = True

REG8 = {'al', 'ah', 'bl', 'bh', 'cl', 'ch', 'dl', 'dh'}
REG16 = {'ax', 'bx', 'cx', 'dx', 'si', 'di', 'bp', 'sp'}
SREG = {'es', 'cs', 'ss', 'ds'}


class Op:
    __slots__ = ('kind', 'size', 'reg', 'imm', 'seg', 'base', 'index',
                 'disp', 'override', 'reloc')

    def __init__(self, kind, size):
        self.kind = kind          # 'reg', 'imm', 'mem'
        self.size = size
        self.reloc = False

    def __repr__(self):
        if self.kind == 'reg':
            return self.reg
        if self.kind == 'imm':
            return hex(self.imm)
        return '%s:[%s+%s+%x]' % (self.seg, self.base, self.index, self.disp)


class Insn:
    __slots__ = ('off', 'size', 'bytes', 'mn', 'ops', 'rep', 'text',
                 'segov', 'target', 'far_target', 'ovl_target', 'disp_loc',
                 'disp_size', 'imm_loc', 'imm_size', 'patched')

    def __repr__(self):
        return '%04x %s' % (self.off, self.text)


# Mnemonics whose capstone spelling is the 32-bit variant
_RENAME = {'cdq': 'cwd', 'cwde': 'cbw', 'pushal': 'pusha', 'popal': 'popa',
           'pushfd': 'pushf', 'popfd': 'popf', 'iretd': 'iret',
           'retf': 'retf', 'ljmp': 'jmpf', 'lcall': 'callf'}

_JCC = {'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns',
        'jp', 'jnp', 'jl', 'jge', 'jle', 'jg'}


def decode(code, off, relocs_at=None):
    """Decode one instruction from `code` (bytes starting at `off`).

    relocs_at: set of offsets (in the same coordinate space as `off`) that
    hold relocated segment words.  Returns Insn or None if invalid.
    """
    try:
        ci = next(_md.disasm(code, off, 1))
    except StopIteration:
        return None
    raw = bytes(ci.bytes)
    i = Insn()
    i.target = i.far_target = i.ovl_target = None
    i.disp_loc = i.imm_loc = None
    i.disp_size = i.imm_size = 0
    i.patched = None
    i.segov = None
    i.off = off
    i.size = ci.size
    i.bytes = raw
    mn = ci.mnemonic
    pfx = ci.prefix
    i.rep = None
    if pfx[0] == x86.X86_PREFIX_REP:
        i.rep = 'rep'
    elif pfx[0] == x86.X86_PREFIX_REPNE:
        i.rep = 'repne'
    if mn.startswith('rep '):
        mn = mn[4:]
    elif mn.startswith('repe ') or mn.startswith('repz '):
        mn = mn[5:]
    elif mn.startswith('repne ') or mn.startswith('repnz '):
        mn = mn[6:]
    stripped = raw.lstrip(bytes([0x26, 0x2e, 0x36, 0x3e, 0xf2, 0xf3, 0xf0]))
    if pfx[2] == x86.X86_PREFIX_OPSIZE or pfx[3] == x86.X86_PREFIX_ADDRSIZE or \
            pfx[1] in (x86.X86_PREFIX_FS, x86.X86_PREFIX_GS) or \
            (stripped[:1] == b'\x0f') or (stripped[:1] in (b'\x64', b'\x65')) \
            or (0xd8 <= stripped[0] <= 0xdf) or stripped[:1] == b'\x63':
        i.mn = 'bad32'
        i.ops = []
        i.text = ci.mnemonic + ' ' + ci.op_str
        return i
    mn = _RENAME.get(mn, mn)
    op0 = raw.lstrip(bytes([0x26, 0x2e, 0x36, 0x3e, 0xf2, 0xf3, 0xf0]))[:1]
    # capstone spells some 16-bit forms oddly; fix by opcode
    if op0 == b'\x98':
        mn = 'cbw'
    elif op0 == b'\x99':
        mn = 'cwd'
    elif op0 == b'\xcf':
        mn = 'iret'
    elif op0 == b'\x60':
        mn = 'pusha'
    elif op0 == b'\x61':
        mn = 'popa'
    elif op0 == b'\x9c':
        mn = 'pushf'
    elif op0 == b'\x9d':
        mn = 'popf'
    i.mn = mn
    i.text = (ci.mnemonic + ' ' + ci.op_str).strip()
    i.segov = None
    for b in raw:
        if b in (0x26, 0x2e, 0x36, 0x3e):
            i.segov = {0x26: 'es', 0x2e: 'cs', 0x36: 'ss', 0x3e: 'ds'}[b]
        elif b in (0xf2, 0xf3, 0xf0):
            continue
        else:
            break
    i.disp_loc = off + ci.disp_offset if ci.disp_size else None
    i.disp_size = ci.disp_size
    i.imm_loc = off + ci.imm_offset if ci.imm_size else None
    i.imm_size = ci.imm_size
    i.patched = None
    i.target = None
    i.far_target = None
    i.ovl_target = None
    ops = []
    for o in ci.operands:
        if o.type == x86.X86_OP_REG:
            op = Op('reg', o.size)
            op.reg = ci.reg_name(o.reg)
        elif o.type == x86.X86_OP_IMM:
            op = Op('imm', o.size)
            op.imm = o.imm & 0xFFFF if o.size != 1 else o.imm & 0xFF
            if o.size == 0:
                op.size = 2
        elif o.type == x86.X86_OP_MEM:
            op = Op('mem', o.size)
            m = o.mem
            op.base = ci.reg_name(m.base) if m.base else None
            op.index = ci.reg_name(m.index) if m.index else None
            op.disp = m.disp & 0xFFFF
            op.override = i.segov
            if i.segov:
                op.seg = i.segov
            elif op.base == 'bp' or (op.base is None and op.index == 'bp'):
                op.seg = 'ss'
            else:
                op.seg = 'ds'
        else:
            return None
        ops.append(op)
    i.ops = ops

    # relocation annotation for immediates
    if relocs_at is not None:
        if op0 in (b'\x9a', b'\xea'):
            pass
        elif ci.imm_size == 2 and (off + ci.imm_offset) in relocs_at:
            for op in ops:
                if op.kind == 'imm':
                    op.reloc = True
        elif ci.disp_size == 2 and (off + ci.disp_offset) in relocs_at:
            i.mn = 'reloc_disp'      # flag as unsupported for review

    if mn in _JCC or mn in ('jmp', 'call', 'loop', 'loope', 'loopne',
                            'jcxz'):
        if ops and ops[0].kind == 'imm' and op0 not in (b'\xff',):
            i.target = ops[0].imm
    if op0 in (b'\x9a', b'\xea'):
        p = raw.index(op0[0])
        o_ = int.from_bytes(raw[p + 1:p + 3], 'little')
        s_ = int.from_bytes(raw[p + 3:p + 5], 'little')
        reloc = relocs_at is not None and (off + p + 3) in relocs_at
        i.far_target = (s_, o_, reloc)
        i.mn = 'callf' if op0 == b'\x9a' else 'jmpf'
    return i


def is_jcc(mn):
    return mn in _JCC
