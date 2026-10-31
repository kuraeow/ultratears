"""Generate C for a whole NSO module.

From LASRecomp tools/gen.py @da35b83 (MIT, LICENSE-LASRecomp); TOTK changes: no randomizer variants,
TOTK patch table (patches.py).

Usage: gen.py <name> <nso> <base> <outdir> [--hle hle_symbols.txt] [--chunk N] [--simd asm|c]

--simd: FP/SIMD/CRC32 as AArch64 inline asm (default on arm64 hosts) or portable C (default elsewhere).
"""
import argparse
import array
import bisect
import os
import platform
import re
import struct
import sys
import time
from collections import Counter

import unicorn as uc
from unicorn import arm64_const as A

from analyze import eh_frame_functions
from lifter import Insn, Lifter, Unsupported, bits, emit_asm_batch, hx, sext
from nso import Nso, R_AARCH64_RELATIVE

SCRATCH_MEM = 0x7300000000
PROBE_DELTA = 0x12345674   # Module.lift: changes the page and the low bits of an address


def same_insn(x, y):
    if (x.kind, x.code, x.target, x.cond) != (y.kind, y.code, y.target, y.cond):
        return False
    if x.asm is None or y.asm is None:
        return x.asm is y.asm
    p, q = x.asm, y.asm
    return (p.word, p.vregs, p.gprs, p.flags, p.consts) == (q.word, q.vregs, q.gprs, q.flags, q.consts)


def sext26(w):
    v = w & 0x03FFFFFF
    return v - 0x04000000 if v & 0x02000000 else v


def fname(a):
    return f'f_{a:x}'


def sanitize(sym):
    return 'hle_' + re.sub(r'[^A-Za-z0-9_]', '_', sym)


class Module:
    def __init__(self, name, path, base, simd='asm'):
        self.name = name
        self.nso = Nso(path)
        self.base = base
        n = self.nso
        self.lo = base + n.text[0]
        self.hi = base + n.text[0] + n.text[1]
        # [TOTK] 4 bytes a word (a tuple of ints took ~36 bytes a word: ~400 MiB for main)
        self.words = array.array('I', bytes(n.image[n.text[0]:n.text[0] + n.text[1]]))
        assert self.words.itemsize == 4 and sys.byteorder == 'little'
        self.lifter = Lifter(base, simd)
        # [TOTK] Lifted instructions are cached by word when the result does not depend on the address (most of
        # them: the same word recurs), else by address. One entry per address for all of main's 11.5 M
        # instructions needed ~4 GiB and failed with MemoryError where the commit limit was tight.
        self.cache = {}          # address -> Insn, for words whose lifting depends on the address
        self.by_word = {}        # word -> Insn, for words whose lifting does not
        self.pc_words = set()    # words known to depend on the address
        self.stats = Counter()
        self.mu = None

    def in_text(self, a):
        return self.lo <= a < self.hi and a % 4 == 0

    def word(self, a):
        return self.words[(a - self.lo) // 4]

    def _lift(self, w, a):
        try:
            return self.lifter.lift(w, a), False
        except Unsupported as e:
            return Insn('trap', f'rt_udf(c, {hx(a)}); /* {e} */'), True

    def c_of(self, w, a):
        """C for one non-branching instruction word as if at a (tools/lifter/patches.py: a replacement or the original)."""
        ins = self.lifter.lift(w, a)
        if ins.kind == 'asm':
            return emit_asm_batch([ins.asm])
        if ins.kind == 'op':
            return ins.code
        raise SystemExit(f'patch at {a:#x}: {w:#010x} is a {ins.kind}, not a plain instruction')

    def lift(self, a):
        ins = self.cache.get(a)
        if ins is not None:
            return ins
        w = self.word(a)
        ins = self.by_word.get(w)
        if ins is not None:
            return ins
        ins, unsupported = self._lift(w, a)
        if unsupported:
            self.stats['unsupported'] += 1
            self.cache[a] = ins
        elif w in self.pc_words:
            self.cache[a] = ins
        else:
            # the same word lifted at another address (another page, other low bits): equal means address-free
            try:
                other, unsupported = self._lift(w, a + PROBE_DELTA)
            except Exception:
                other, unsupported = None, True
            if not unsupported and same_insn(ins, other):
                self.by_word[w] = ins
            else:
                self.pc_words.add(w)
                self.cache[a] = ins
        return ins

    def boundaries(self):
        n, b = self.nso, self.base
        starts = set()
        for a in eh_frame_functions(n):
            starts.add(b + a)
        for name, info, shndx, value, size in n.syms:
            if shndx and (info & 0xF) == 2:
                starts.add(b + value)
        for off, typ, sym, add in n.relocs:
            if typ == R_AARCH64_RELATIVE:
                starts.add(b + add)
        starts = sorted(a for a in starts if self.in_text(a))
        return starts

    # ------------------------------------------------------------ jump tables

    def emu(self):
        if self.mu is None:
            mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
            n = self.nso
            mu.mem_map(self.base, len(n.image))
            mu.mem_write(self.base, bytes(n.image))
            mu.mem_map(SCRATCH_MEM, 0x100000)
            mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)

            # [TOTK] Loads unrelated to the table between the bounds check and the br (e.g. `ldr q29, [x8, x13]`
            # with both registers holding the scratch address) used to fault and drop the whole table (798
            # functions in main fell back to an all-instruction switch). Unmapped pages now read as zeros.
            def unmapped(uc_, access, address, size, value, data):
                page = address & ~0xFFF
                if page >= 1 << 48:
                    return False
                try:
                    uc_.mem_map(page, 0x1000)
                except uc.UcError:
                    return False
                return True
            mu.hook_add(uc.UC_HOOK_MEM_READ_UNMAPPED | uc.UC_HOOK_MEM_WRITE_UNMAPPED, unmapped)
            self.mu = mu
        return self.mu

    def jump_table(self, br_pc, entry, end):
        """Resolve `br xN` targets from a bounds-checked jump table, or None."""
        self._table_count = 0   # the bounds-checked number of entries, set by the emulations below
        r = self._jump_table_fallthrough(br_pc, entry, end)
        if r is None:
            r = self._jump_table_taken(br_pc, entry, end)
        # [TOTK] The emulation sets the register the bounds check compares; when the load indexes the table with
        # another register holding the same value (main 0x80209aa08: `cmp w22, #0xb` ... `ldrh w10, [x8, x28, lsl #1]`)
        # every index gives the same target (a table read at a fixed scratch address). Such a single-target result is
        # replaced by the entries read from the image. (Adding the scanned entries to every table instead put
        # trailing data that looks like entries into 1619 of main's 2367 tables.)
        if r is not None and len(set(r)) > 1:
            more = self._jump_table_other_bases(br_pc, entry, end) - set(r)
            if more:
                self.stats['jumptables_other_bases'] += 1
                return sorted(set(r) | more)
            return r
        s = self._jump_table_scan(br_pc, entry, end)
        if r is not None and s is not None:
            self.stats['jumptables_single_target_rescanned'] += 1
            return sorted(set(r) | set(s))
        # A single target the image scan cannot confirm is a guess (main 0x8006eb84c: a byte table of 7 targets whose
        # register is reused before the br, so the scan misses its adrp/add and the emulation reads one entry at the
        # scratch address; loading a save then ran into "no function at 8006eb850", GitHub issue #18). No table: the
        # br takes the computed-target fallback over all of the function's labels.
        if s is None and r is not None and len(set(r)) <= 1:
            self.stats['jumptables_single_unconfirmed'] += 1
            return None
        return r if r is not None else s

    def _jump_table_scan(self, br_pc, entry, end):
        """[TOTK] A table whose bounds check the compiler hoisted far from the br (main: functions with thousands of
        cases): `adr xB, base; adrp/add xT, table; ldr{b,h,sw} wO, [xT, xI, ...]; add xD, xB, wO, {lsl,sxt*} #k; br xD`.
        The entries are read from the image until one points outside the function or is misaligned; trailing bytes
        that happen to look like entries only add labels (an index the table never had is never taken)."""
        import capstone
        if not hasattr(self, '_cs'):
            self._cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
            self._cs.detail = True
        cs = self._cs
        window = []
        a = br_pc
        while a >= entry and br_pc - a <= 4 * 24:
            window.append(a)
            a -= 4
        insns = {}
        for x in window:
            got = list(cs.disasm(struct.pack('<I', self.word(x)), x))
            if not got:
                return None
            insns[x] = got[0]
        rn = bits(self.word(br_pc), 5, 5)
        # add xD(=rn), xB, wO, <ext|lsl> #k
        add = load = None
        for x in window[1:]:
            i = insns[x]
            if i.mnemonic == 'add' and len(i.operands) == 3 and i.reg_name(i.operands[0].reg) == f'x{rn}':
                add = i
                break
            if self.lift(x).kind not in ('op', 'asm'):
                return None
        if add is None or add.operands[2].type != capstone.arm64.ARM64_OP_REG:
            return None
        base_reg = add.reg_name(add.operands[1].reg)
        off_reg = add.reg_name(add.operands[2].reg)
        shift = add.operands[2].shift.value if add.operands[2].shift.type else 0
        values = {}   # register -> constant from adr / adrp+add before the add

        def scan_back(reg, before):
            for x in window:
                if x >= before:
                    continue
                i = insns[x]
                if not i.operands or i.operands[0].type != capstone.arm64.ARM64_OP_REG:
                    continue
                if i.reg_name(i.operands[0].reg) != reg:
                    continue
                return x, i
            return None, None
        x, i = scan_back(base_reg, add.address)
        if i is None or i.mnemonic != 'adr':
            return None
        base = i.operands[1].imm
        # the load of the offset
        x, i = scan_back(off_reg.replace('x', 'w'), add.address)
        if i is None:
            x, i = scan_back(off_reg.replace('w', 'x'), add.address)
        if i is None or i.mnemonic not in ('ldrb', 'ldrh', 'ldrsb', 'ldrsh', 'ldrsw', 'ldr'):
            return None
        mem = i.operands[1]
        if mem.type != capstone.arm64.ARM64_OP_MEM or not mem.mem.index:
            return None
        size = {'ldrb': 1, 'ldrsb': 1, 'ldrh': 2, 'ldrsh': 2, 'ldrsw': 4, 'ldr': 4}[i.mnemonic]
        signed = i.mnemonic.startswith('ldrs')
        table_reg = i.reg_name(mem.mem.base)
        # table address: adrp + add on table_reg, near the br or (hoisted out of a loop) anywhere earlier in the
        # function: the closest earlier write of table_reg must be that add, and the write before it the adrp.
        def writer_before(reg, before):
            for y in range(before - 4, entry - 4, -4):
                got = list(cs.disasm(struct.pack('<I', self.word(y)), y))
                if not got:
                    return None
                j = got[0]
                if j.operands and j.operands[0].type == capstone.arm64.ARM64_OP_REG and j.reg_name(j.operands[0].reg) == reg \
                        and j.mnemonic not in ('str', 'stp', 'stur', 'strb', 'strh', 'cmp', 'cmn', 'tst'):
                    return j
            return None
        ti = writer_before(table_reg, x)
        if ti is None or ti.mnemonic != 'add' or len(ti.operands) != 3 or ti.operands[2].type != capstone.arm64.ARM64_OP_IMM \
                or ti.reg_name(ti.operands[1].reg) != table_reg:
            return None
        pi = writer_before(table_reg, ti.address)
        if pi is None or pi.mnemonic != 'adrp':
            return None
        table = pi.operands[1].imm + ti.operands[2].imm
        lo, hi = self.base, self.base + len(self.nso.image)
        targets = set()
        for k in range(4096):
            at = table + k * size
            if at + size > hi or at < lo:
                break
            raw = int.from_bytes(self.nso.image[at - self.base:at - self.base + size], 'little', signed=signed)
            t = base + (raw << shift)
            if not (entry <= t < end) or t % 4:
                break
            targets.add(t)
        if len(targets) < 2:
            return None
        self.stats['jumptables_scanned'] += 1
        return sorted(targets)

    def _jump_table_other_bases(self, br_pc, entry, end):
        """[TOTK] Targets of the other tables the table register can hold. A function may choose its table before a
        loop, on one of several paths (main 0x8015b8d30: x20 is 0x802cfe74a or 0x802cfe6d2, and 0x60 further on, for
        its brs at 0x8015b9400 and 0x8015be6bc); the emulation replays every adrp/add from the entry in order and so
        reads only the last table. The game took another one at the bats of the prologue: "no function at 8015b9fb0"
        (GitHub issue #4). Every constant the register gets in the function (adrp, adrp+add, then adds on itself) is
        a candidate; one is kept only if all its entries for the bounds-checked count land in the function, aligned."""
        count = getattr(self, '_table_count', 0)
        if not count:
            return set()
        import capstone
        if not hasattr(self, '_cs'):
            self._cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
            self._cs.detail = True
        cs = self._cs
        # br xD <- add xD, xB, wO, lsl #k <- ldr{b,h,sw} wO, [xT, xI, ...]; xB from adr: all near the br
        rn = bits(self.word(br_pc), 5, 5)
        insns = []
        for x in range(br_pc - 4, max(entry, br_pc - 4 * 24) - 4, -4):
            got = list(cs.disasm(struct.pack('<I', self.word(x)), x))
            if not got:
                return set()
            insns.append(got[0])
        add = next((i for i in insns if i.mnemonic == 'add' and len(i.operands) == 3
                    and i.operands[0].type == capstone.arm64.ARM64_OP_REG and i.reg_name(i.operands[0].reg) == f'x{rn}'
                    and i.operands[2].type == capstone.arm64.ARM64_OP_REG), None)
        if add is None:
            return set()
        base_reg, off_reg = add.reg_name(add.operands[1].reg), add.reg_name(add.operands[2].reg)
        shift = add.operands[2].shift.value if add.operands[2].shift.type else 0
        before = [i for i in insns if i.address < add.address]
        adr = next((i for i in before if i.operands and i.operands[0].type == capstone.arm64.ARM64_OP_REG
                    and i.reg_name(i.operands[0].reg) == base_reg), None)
        load = next((i for i in before if i.operands and i.operands[0].type == capstone.arm64.ARM64_OP_REG
                     and i.reg_name(i.operands[0].reg)[1:] == off_reg[1:]), None)
        if adr is None or adr.mnemonic != 'adr' or load is None \
                or load.mnemonic not in ('ldrb', 'ldrh', 'ldrsb', 'ldrsh', 'ldrsw', 'ldr'):
            return set()
        mem = load.operands[1]
        if mem.type != capstone.arm64.ARM64_OP_MEM or not mem.mem.index:
            return set()
        size = {'ldrb': 1, 'ldrsb': 1, 'ldrh': 2, 'ldrsh': 2, 'ldrsw': 4, 'ldr': 4}[load.mnemonic]
        signed = load.mnemonic.startswith('ldrs')
        treg = int(load.reg_name(mem.mem.base)[1:]) if load.reg_name(mem.mem.base)[1:].isdigit() else -1
        if treg < 0:
            return set()
        consts, adds, paired = set(), [], set()
        for a in range(entry, load.address, 4):
            w = self.word(a)
            if (w & 31) != treg:
                continue
            if (w & 0x9F000000) == 0x90000000:   # adrp xT (+ add xT, xT, #lo12 right after it)
                imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
                if imm & (1 << 20):
                    imm -= 1 << 21
                page = (a & ~0xFFF) + (imm << 12)
                nxt = self.word(a + 4) if a + 4 < load.address else 0
                if (nxt & 0xFFC00000) == 0x91000000 and (nxt & 31) == treg and bits(nxt, 5, 5) == treg:
                    consts.add(page + bits(nxt, 10, 12))
                    paired.add(a + 4)
            elif (w & 0xFFC00000) == 0x91000000 and bits(w, 5, 5) == treg and a not in paired:   # add xT, xT, #imm
                adds.append(bits(w, 10, 12))
        cands = set(consts) | {c + k for c in consts for k in adds}
        lo, hi = self.base, self.base + len(self.nso.image)
        out = set()
        for table in cands:
            got = []
            for k in range(count):
                at = table + k * size
                if at < lo or at + size > hi:
                    break
                raw = int.from_bytes(self.nso.image[at - self.base:at - self.base + size], 'little', signed=signed)
                t = adr.operands[1].imm + (raw << shift)
                if not (entry <= t < end) or t % 4:
                    break
                got.append(t)
            if len(got) == count:
                out |= set(got)
        return out

    def _index_regs(self, start, at, idx):
        """The index register plus registers holding the same value in start..at: sources it was
        copied/masked from, and copies of those (the table lookup sometimes recomputes the index
        from another copy after the bounds check)."""
        def copy_src(w):
            if (w & 0x7F800000) == 0x12000000:  # AND (imm)
                return bits(w, 5, 5)
            if (w & 0x7FE0FFE0) == 0x2A0003E0:  # MOV (ORR with zr)
                return bits(w, 16, 5)
            if (w & 0x7F800000) in (0x53000000, 0x13000000):  # UBFM/SBFM (uxtb, uxth, sxtw...)
                return bits(w, 5, 5)
            return None
        regs = {idx}
        changed = True
        while changed:
            changed = False
            for a in range(start, at, 4):
                w = self.word(a)
                rd, src = w & 31, copy_src(w)
                if src is None or src >= 31 or rd >= 31:
                    continue
                for x, y in ((rd, src), (src, rd)):
                    if x in regs and y not in regs:
                        regs.add(y)
                        changed = True
        return sorted(regs)

    def _set_index(self, mu, regs, i):
        for r in regs:
            mu.reg_write(A.UC_ARM64_REG_X0 + r if r < 29 else A.UC_ARM64_REG_X29, i)

    def _emulate_table(self, br_pc, entry, end, start, idx, count, set_at):
        """Run start..br for each index value (set at address set_at), collect targets."""
        mu = self.emu()
        rn = bits(self.word(br_pc), 5, 5)

        def step_to(frm, to):
            pc = frm
            for _ in range(64):
                if pc == to:
                    return True
                mu.emu_start(pc, 0, count=1)
                pc = mu.reg_read(A.UC_ARM64_REG_PC)
            return False

        targets = []
        try:
            for i in range(count):
                for r in range(29):
                    mu.reg_write(A.UC_ARM64_REG_X0 + r, SCRATCH_MEM + 0x80000)
                mu.reg_write(A.UC_ARM64_REG_SP, SCRATCH_MEM + 0x80000)
                a2 = entry
                while a2 < start:
                    w2 = self.word(a2)
                    if (w2 & 0x1F000000) == 0x10000000 or (w2 & 0xFF800000) == 0x91000000:
                        mu.emu_start(a2, 0, count=1)
                    a2 += 4
                if set_at != start and not step_to(start, set_at):
                    return None
                self._set_index(mu, [idx], i)
                if not step_to(set_at, br_pc):
                    return None
                t = mu.reg_read(A.UC_ARM64_REG_X0 + rn if rn < 29 else
                                (A.UC_ARM64_REG_X29 if rn == 29 else A.UC_ARM64_REG_X30))
                if not (entry <= t < end) or t % 4:
                    return None
                targets.append(t)
        except uc.UcError:
            return None
        return sorted(set(targets))

    def _jump_table_taken(self, br_pc, entry, end):
        """Form: cmp idx, #N ; b.ls/b.lo table ... table: <address math> ; br"""
        # start of the block holding the br
        s = br_pc
        while s - 4 >= entry and br_pc - (s - 4) <= 4 * 12 and self.lift(s - 4).kind in ('op', 'asm'):
            s -= 4
        for g in range(entry, end, 4):
            w = self.word(g)
            if (w & 0xFF000010) != 0x54000000 or bits(w, 0, 4) not in (9, 3):
                continue
            t = g + sext(bits(w, 5, 19), 19) * 4
            if t != s:
                continue
            cw = self.word(g - 4) if g - 4 >= entry else 0
            if (cw & 0x7F80001F) != 0x7100001F or bits(cw, 22, 1):
                continue
            limit = bits(cw, 10, 12)
            count = limit + 1 if bits(w, 0, 4) == 9 else limit
            if count <= 0 or count > 4096:
                continue
            self._table_count = count
            return self._emulate_table(br_pc, entry, end, s, bits(cw, 5, 5), count, s)
        return None

    def _jump_table_fallthrough(self, br_pc, entry, end):
        rn = bits(self.word(br_pc), 5, 5)
        # find the guarding b.hi / b.hs and its cmp, scanning backwards
        a = br_pc - 4
        guard = cmp = None
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if (w & 0xFF000010) == 0x54000000 and bits(w, 0, 4) in (8, 2):
                guard = a
                break
            if self.lift(a).kind in ('b', 'bl', 'blr', 'br', 'ret', 'trap'):
                return None
            a -= 4
        if guard is None:
            return None
        a = guard - 4
        while a >= entry and guard - a <= 4 * 8:
            w = self.word(a)
            # SUBS (imm) with Rd = zr, i.e. CMP #imm
            if (w & 0x7F80001F) == 0x7100001F:
                cmp = a
                break
            if self.lift(a).kind != 'op':
                return None
            a -= 4
        if cmp is None:
            return None
        w = self.word(cmp)
        if bits(w, 22, 1):
            return None
        limit = bits(w, 10, 12)
        count = limit + 1 if bits(self.word(guard), 0, 4) == 8 else limit
        if count <= 0 or count > 4096:
            return None
        idx = bits(w, 5, 5)
        # window start: back from cmp to the first control-flow instruction
        start = cmp
        while start - 4 >= entry and cmp - (start - 4) <= 4 * 12 and self.lift(start - 4).kind in ('op', 'asm'):
            start -= 4
        idx_regs = self._index_regs(start, cmp, idx)
        self._table_count = count
        mu = self.emu()
        targets = []

        def step_to(frm, to):
            # single-step: Unicorn's `until` is unreliable mid translation-block
            pc = frm
            for _ in range(64):
                if pc == to:
                    return True
                mu.emu_start(pc, 0, count=1)
                pc = mu.reg_read(A.UC_ARM64_REG_PC)
            return False

        try:
            for i in range(count):
                for r in range(29):
                    mu.reg_write(A.UC_ARM64_REG_X0 + r, SCRATCH_MEM + 0x80000)
                mu.reg_write(A.UC_ARM64_REG_SP, SCRATCH_MEM + 0x80000)
                # replay address materialization (ADRP/ADR/ADD-imm) from the function entry, so table
                # bases computed before loops or branches are known
                a2 = entry
                while a2 < start:
                    w2 = self.word(a2)
                    if (w2 & 0x1F000000) == 0x10000000 or (w2 & 0xFF800000) == 0x91000000:
                        mu.emu_start(a2, 0, count=1)
                    a2 += 4
                if not step_to(start, cmp):
                    return None
                self._set_index(mu, idx_regs, i)
                if not step_to(cmp, br_pc):
                    return None
                t = mu.reg_read(A.UC_ARM64_REG_X0 + rn if rn < 29 else
                                (A.UC_ARM64_REG_X29 if rn == 29 else A.UC_ARM64_REG_X30))
                if not (entry <= t < end) or t % 4:
                    return None
                targets.append(t)
        except uc.UcError:
            return None
        return sorted(set(targets))

    # ------------------------------------------------------------ functions

    def computed_target(self, br_pc, entry):
        """True if the br register comes from address arithmetic (a table) rather than a load."""
        rn = bits(self.word(br_pc), 5, 5)
        a = br_pc - 4
        while a >= entry and br_pc - a <= 4 * 16:
            w = self.word(a)
            if self.lift(a).kind not in ('op', 'asm'):
                return False
            if (w & 31) == rn:
                if (w & 0x1F000000) == 0x0B000000:  # ADD/SUB (shifted or extended register)
                    return True
                if (w & 0x9F000000) == 0x10000000:  # ADR
                    return True
                return False
            a -= 4
        return False

    def scan(self, entry, end):
        """Collect labels and external entries referenced by a function."""
        labels, ext, tables = {entry}, set(), {}
        terminated = False
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if ins.kind in ('b', 'bcond'):
                t = ins.target
                if entry <= t < end:
                    labels.add(t)
                elif self.in_text(t):
                    ext.add(t)
            elif ins.kind == 'bl':
                if self.in_text(ins.target):
                    ext.add(ins.target)
            elif ins.kind == 'br':
                jt = self.jump_table(a, entry, end)
                if jt:
                    tables[a] = jt
                    labels.update(jt)
                    self.stats['jumptables'] += 1
                elif self.computed_target(a, entry):
                    # unresolved table jump: allow jumping to any instruction of the function
                    tables[a] = 'all'
                    self.stats['br_all_labels'] += 1
                else:
                    self.stats['br_dispatch'] += 1
            terminated = ins.kind in ('b', 'br', 'ret', 'trap')
        if not terminated and end < self.hi:
            ext.add(end)
        return labels, ext, tables

    def render_single(self, ins, a, jump, refs):
        """C for one lifted instruction (no batching); used for patch variants."""
        k = ins.kind
        if k == 'asm':
            return [emit_asm_batch([ins.asm])]
        if k in ('op', 'trap'):
            return [ins.code]
        if k == 'b':
            return [jump(ins.target)]
        if k == 'bcond':
            return [f'if ({ins.cond}) {{ {jump(ins.target)} }}']
        if k == 'bl':
            t = ins.target
            if self.in_text(t):
                refs.add(t)
                return [f'c->x[30] = {hx(a + 4)}; {fname(t)}(c);']
            return [f'c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);']
        if k == 'blr':
            return [f'c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch(c);']
        if k == 'br':
            return [f'c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);']
        if k == 'ret':
            return ['return;']
        raise Unsupported(k)

    def emit_function(self, entry, end, labels, tables, hle):
        out = [f'void {fname(entry)}(Cpu *c) {{']
        if entry in hle:
            out.append(f'    {sanitize(hle[entry])}(c);')
            out.append('}')
            return out
        out.append(f'    FN_ENTER({hx(entry)});')
        batch = []
        refs = set()

        def flush():
            if batch:
                out.append('    ' + emit_asm_batch(batch))
                batch.clear()

        def jump(t):
            if entry <= t < end:
                return f'goto L_{t:x};'
            if not self.in_text(t):
                return f'c->pc = {hx(t)}; __attribute__((musttail)) return rt_dispatch(c);'
            refs.add(t)
            return f'__attribute__((musttail)) return {fname(t)}(c);'

        # [TOTK] Only functions with an unresolved computed branch ('all') get the all-instruction fallback. LASRecomp
        # gave it to every function with a jump table: thousands of labels made clang take hours on single functions
        # (main_127.c: 15 880 labels, >2.5 h at -O2) and kept it from optimizing them. A resolved table's targets are
        # complete (each index was emulated); an index outside them reaches rt_dispatch and fails loudly.
        all_labels = any(v == 'all' for v in tables.values())
        if all_labels:
            labels = set(range(entry, end, 4))
        last = None
        for a in range(entry, end, 4):
            ins = self.lift(a)
            if a in labels:
                flush()
                out.append(f'L_{a:x}:;')
            if a in PRE_PATCHES:  # code run before the instruction, which is lifted as usual (tools/patches.py)
                flush()
                out.append('    ' + PRE_PATCHES[a](self, a))
            if a in PATCHES:  # hand-written replacement for one instruction (tools/patches.py)
                flush()
                out.append('    ' + PATCHES[a](self, a))
                last = 'op'
                continue
            k = ins.kind
            if k == 'asm':
                if len(batch) >= 24:
                    flush()
                batch.append(ins.asm)
                try:
                    emit_asm_batch(batch)
                except Unsupported:
                    batch.pop()
                    flush()
                    batch.append(ins.asm)
                last = k
                continue
            flush()
            if k == 'op' or k == 'trap':
                out.append('    ' + ins.code)
            elif k == 'b':
                out.append('    ' + jump(ins.target))
            elif k == 'bcond':
                out.append(f'    if ({ins.cond}) {{ {jump(ins.target)} }}')
            elif k == 'bl':
                t = ins.target
                if self.in_text(t):
                    refs.add(t)
                    out.append(f'    c->x[30] = {hx(a + 4)}; {fname(t)}(c);')
                else:
                    out.append(f'    c->x[30] = {hx(a + 4)}; c->pc = {hx(t)}; rt_dispatch(c);')
            elif k == 'blr':
                out.append(f'    c->pc = {ins.target}; c->x[30] = {hx(a + 4)}; rt_dispatch(c);')
            elif k == 'br':
                jt = tables.get(a)
                if jt == 'all':
                    out.append(f'    c->pc = {ins.target}; goto L_dispatch;')
                    continue
                if jt:
                    cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in jt)
                    out.append(f'    switch ({ins.target}) {{ {cases} default: break; }}')
                    if all_labels:
                        out.append(f'    c->pc = {ins.target}; goto L_dispatch;')
                    else:
                        out.append(f'    c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);')
                    continue
                out.append(f'    c->pc = {ins.target}; __attribute__((musttail)) return rt_dispatch(c);')
            elif k == 'ret':
                out.append('    return;')
            last = k
        flush()
        if last not in ('b', 'br', 'ret', 'trap'):
            out.append('    ' + jump(end) if end < self.hi else f'    rt_udf(c, {hx(end)});')
        if all_labels:
            out.append('L_dispatch:')
            cases = ' '.join(f'case {hx(t)}: goto L_{t:x};' for t in range(entry, end, 4))
            out.append(f'    switch (c->pc) {{ {cases} default: break; }}')
            out.append('    __attribute__((musttail)) return rt_dispatch(c);')
        out.append('}')
        self._refs = refs
        return out


from patches import PATCHES, PRE_PATCHES


def table_base(m, i, reg, window=40):
    """True if the ADR at word i loads the base of a jump table: within a few instructions an ADD (register) uses
    `reg` and a BR jumps to the sum. [TOTK] The compiler schedules other address setup between them (main
    0x800d2aab0: the br is 17 instructions after the adr), so the window ends at an unconditional branch or return
    rather than after 16 instructions."""
    regs, added = {reg}, False
    for j in range(i + 1, min(i + 1 + window, len(m.words))):
        w = m.words[j]
        if (w & 0xFC000000) == 0x14000000 or w == 0xD65F03C0:  # B / RET: the block ends without the br
            return False
        if (w & 0xFFFFFC1F) == 0xD61F0000:  # BR Xn
            return added and ((w >> 5) & 31) in regs
        if (w & 0x7F200000) in (0x0B000000, 0x0B200000) and (((w >> 5) & 31) in regs or ((w >> 16) & 31) in regs):
            regs.add(w & 31)  # ADD (shifted/extended register) of the base
            added = True
    return False


def write_if_changed(path, text):
    """Leaves unchanged chunks untouched so make only recompiles what a patch affected."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, 'w') as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('nso')
    ap.add_argument('base', type=lambda s: int(s, 0))
    ap.add_argument('outdir')
    ap.add_argument('--hle')
    ap.add_argument('--chunk', type=int, default=1500)  # functions per file at most
    ap.add_argument('--chunk-bytes', type=int, default=1500000)  # and about this much C (even compile times)
    ap.add_argument('--big-fn', type=int, default=100000)  # bytes of C from which a function gets its own file
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--simd', choices=('asm', 'c'),
                    default='asm' if platform.machine().lower() in ('arm64', 'aarch64') else 'c')
    args = ap.parse_args()

    t0 = time.time()
    m = Module(args.name, args.nso, args.base, args.simd)
    starts = m.boundaries()
    hle = {}
    if args.hle and os.path.exists(args.hle):
        want = {l.strip() for l in open(args.hle) if l.strip() and not l.startswith('#')}
        for name, info, shndx, value, size in m.nso.syms:
            if shndx and name in want:
                hle[args.base + value] = name

    # Partition text into functions; any branch/call target outside its function becomes a new
    # boundary (splitting the containing function) until a fixpoint is reached.
    start_set = set(starts)
    for i, w in enumerate(m.words):
        if (w & 0xFC000000) == 0x94000000:
            t = m.lo + i * 4 + sext26(w) * 4
            if m.in_text(t):
                start_set.add(t)
    # Code addresses materialized in registers (callbacks, vtables built at runtime): ADR, or
    # ADRP followed shortly by ADD on the same register.
    adrp_regs = {}
    for i, w in enumerate(m.words):
        pc = m.lo + i * 4
        if (w & 0x9F000000) == 0x10000000:  # ADR
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            # [TOTK] an ADR that is the base of a jump table (adr xT; ...; add xD, xT, wI, ...; br xD) is not a
            # function: as a start it split the switch function there and made the table's targets unreachable.
            if m.in_text(pc + imm) and not table_base(m, i, w & 31):
                start_set.add(pc + imm)
        elif (w & 0x9F000000) == 0x90000000:  # ADRP
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            adrp_regs[w & 31] = ((pc & ~0xFFF) + (imm << 12), i)
        elif (w & 0xFFC00000) == 0x91000000:  # ADD (imm, 64-bit, no shift)
            rn, rd = (w >> 5) & 31, w & 31
            if rn in adrp_regs and i - adrp_regs[rn][1] <= 8:
                t = adrp_regs[rn][0] + ((w >> 10) & 0xFFF)
                if m.in_text(t):
                    start_set.add(t)
    scanned = {}
    rounds = 0
    while True:
        rounds += 1
        order = sorted(start_set)
        new = set()
        for i, a in enumerate(order):
            end = order[i + 1] if i + 1 < len(order) else m.hi
            key = (a, end)
            if key not in scanned:
                scanned[key] = m.scan(a, end)
            new |= scanned[key][1] - start_set
        if not new:
            break
        start_set |= new
    entries = {}
    for i, a in enumerate(order):
        end = order[i + 1] if i + 1 < len(order) else m.hi
        labels, ext, tables = scanned[(a, end)]
        entries[a] = (end, labels, tables)
    print(f'{args.name}: {len(starts)} boundaries, {len(entries)} functions, {rounds} rounds, {dict(m.stats)}, '
          f'{time.time() - t0:.0f}s', file=sys.stderr)

    os.makedirs(args.outdir, exist_ok=True)
    order = sorted(entries)
    if args.limit:
        order = order[:args.limit]
    nchunks = 0
    ci = 0
    pending = None  # (address, lines, refs) of a big function that starts the next file
    while ci < len(order) or pending:
        body, refs, size, fns = [], set(), 0, []
        while (pending or ci < len(order)) and len(fns) < args.chunk and size < args.chunk_bytes:
            if pending:
                a, lines, r = pending
                pending = None
            else:
                a = order[ci]
                ci += 1
                end, labels, tables = entries[a]
                lines = m.emit_function(a, end, labels, tables, hle)
                r = getattr(m, '_refs', set())
                m._refs = set()
            n = sum(len(l) + 1 for l in lines)
            if n > args.big_fn and fns:  # (a big function gets a file of its own: they compile in parallel)
                pending = (a, lines, r)
                break
            body += lines
            size += n
            fns.append(a)
            refs |= r
            if n > args.big_fn:
                break
        path = os.path.join(args.outdir, f'{args.name}_{nchunks:03d}.c')
        text = ['#include "cpu.h"\n' + ('#include "simd.h"\n' if m.lifter.sc else '')]
        text += [f'void {fname(r)}(Cpu *c);\n' for r in sorted(refs)]
        text += [f'void {sanitize(hle[a])}(Cpu *c);\n' for a in sorted(set(hle) & set(fns))]
        text += ['\n'.join(body), '\n']
        write_if_changed(path, ''.join(text))
        nchunks += 1
        # (the start screen's setup reads these for its progress bar: runtime/gamebuild.c)
        print(f'{args.name}: progress {ci}/{len(order)} files {nchunks}', file=sys.stderr, flush=True)
    for fn in os.listdir(args.outdir):  # an earlier run's extra chunks
        mm = re.fullmatch(re.escape(args.name) + r'_(\d+)\.c', fn)
        if mm and int(mm.group(1)) >= nchunks:
            os.remove(os.path.join(args.outdir, fn))
    import io
    with io.StringIO() as f:
        f.write('#include "cpu.h"\n')
        for a in order:
            f.write(f'void {fname(a)}(Cpu *c);\n')
        f.write(f'const uint64_t {args.name}_fn_addrs[] = {{\n')
        f.write(',\n'.join(hx(a) for a in order))
        f.write('};\n')
        f.write(f'const GuestFn {args.name}_fn_ptrs[] = {{\n')
        f.write(',\n'.join(fname(a) for a in order))
        f.write('};\n')
        f.write(f'const uint64_t {args.name}_fn_count = {len(order)};\n')
        write_if_changed(os.path.join(args.outdir, f'{args.name}_table.c'), f.getvalue())
    print(f'{args.name}: wrote {nchunks} chunks, {time.time() - t0:.0f}s total', file=sys.stderr)


if __name__ == '__main__':
    main()
