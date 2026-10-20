"""Finds the code of each function: its instructions, branch targets, calls and jump tables.

A function is every instruction that its entry reaches without a call. A jump to the entry of another
known function is a tail call. A jump through a table of addresses is a switch; the table size comes
from the bound check before it (cmp index, n; ja default), with a byte table in between when there is one.
"""
import capstone
from capstone import x86_const as X

from pe_image import Image

TERMINATORS = {"ret", "retf", "iretd", "hlt", "ud2", "int3"}
# CRT functions that never return but whose code has a ret path for other callers (_doexit's flag).
NORETURN = {}          # dirección -> nombre; build.py la llena
NORETURN_IMPORTS = {"ExitProcess", "ExitThread", "FatalAppExitA", "RaiseException", "TerminateProcess"}


class Function:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}          # address -> capstone instruction
        self.labels = set()      # addresses that are jump targets inside the function
        self.calls = set()       # direct call targets
        self.tails = set()       # direct jump targets that are other functions
        self.tables = {}         # address of the jmp -> list of targets
        self.bad = []            # (address, reason) where the decoder stopped
        self.returns = False     # a ret or a jump out of the function is reachable
        self.noreturn_calls = set()   # calls after which execution does not continue


class Analyzer:
    def __init__(self, image: Image, known_entries=()):
        self.image = image
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.known = set(known_entries)
        self._cache = {}
        self._noreturn = {}
        self._functions = {}
        self._building = set()
        self._table_bytes = set()     # bytes of the jump tables and their byte tables in .text

    def noreturn(self, addr):
        """True when the function at addr never returns: no path of it reaches a ret or a jump out."""
        if addr in NORETURN:
            return True
        if addr in self._noreturn:
            return self._noreturn[addr]
        if addr in self._building:
            return False                        # a call back into a function being decoded
        self._noreturn[addr] = False            # while in progress: assume it returns
        f = self.function(addr)
        exits = f.returns or any(not self.noreturn(t) for t in f.tails)
        self._noreturn[addr] = not exits and not f.bad
        return self._noreturn[addr]

    def decode(self, addr):
        if addr in self._cache:
            return self._cache[addr]
        code = self.image.read(addr, 16)
        insn = next(self.md.disasm(code, addr, 1), None)
        self._cache[addr] = insn
        return insn

    def function(self, entry):
        if entry in self._functions:
            return self._functions[entry]
        f = Function(entry)
        self._building.add(entry)
        self._decode(f)
        if any(self._in_table(a, f.insns[a].size) for a in f.insns):
            # Code decoded over a jump table found later: decode again, now stopping at the tables.
            f = Function(entry)
            self._decode(f)
        self._building.discard(entry)
        self._functions[entry] = f
        return f

    def _decode(self, f):
        entry = f.entry
        work = [entry]
        while work:
            addr = work.pop()
            while addr not in f.insns:
                if addr != entry and addr in self.known:
                    f.tails.add(addr)                   # falls or jumps into the next function
                    break
                if self._in_table(addr, 1):
                    f.bad.append((addr, "jump table data"))
                    break
                if not self.image.is_code(addr):
                    f.bad.append((addr, "outside .text"))
                    break
                insn = self.decode(addr)
                if insn is None:
                    f.bad.append((addr, "cannot decode"))
                    break
                f.insns[addr] = insn
                nxt = addr + insn.size
                m = insn.mnemonic
                groups = set(insn.groups)
                if m in TERMINATORS:
                    if m.startswith("ret"):
                        f.returns = True
                    break
                if m == "call":
                    op = insn.operands[0]
                    if op.type == X.X86_OP_IMM:
                        target = op.imm & 0xFFFFFFFF
                        f.calls.add(target)
                        if target != entry and self.image.is_code(target) and self.noreturn(target):
                            f.noreturn_calls.add(addr)
                            break
                    elif self.import_of(op) in NORETURN_IMPORTS:
                        f.noreturn_calls.add(addr)
                        break
                    addr = nxt
                    continue
                if m == "jmp":
                    op = insn.operands[0]
                    imp = self.import_of(op)
                    if imp:
                        if imp not in NORETURN_IMPORTS:
                            f.returns = True
                        break
                    if op.type == X.X86_OP_IMM:
                        target = op.imm & 0xFFFFFFFF
                        if target != entry and target in self.known:
                            f.tails.add(target)
                        else:
                            f.labels.add(target)
                            work.append(target)
                    else:
                        table = self.jump_table(f, insn)
                        if not table:
                            f.returns = True            # an indirect jump: assume it leaves
                        if table:
                            f.tables[addr] = table
                            for t in table:
                                f.labels.add(t)
                                work.append(t)
                    break
                if capstone.CS_GRP_JUMP in groups:          # jcc, jecxz, loop
                    target = insn.operands[0].imm & 0xFFFFFFFF
                    f.labels.add(target)
                    work.append(target)
                    f.labels.add(nxt)
                addr = nxt
        return f

    def _in_table(self, addr, size):
        return any(a in self._table_bytes for a in range(addr, addr + size))

    def import_of(self, op):
        if op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
            imp = self.image.imports.get(op.mem.disp & 0xFFFFFFFF)
            return imp[1] if imp else None
        return None

    def _previous(self, f, addr, count):
        """The instructions before addr in address order, nearest first."""
        before = sorted(a for a in f.insns if a < addr)[-count:]
        return [f.insns[a] for a in reversed(before)]

    def jump_table(self, f, insn):
        op = insn.operands[0]
        if op.type != X.X86_OP_MEM:
            return None
        mem = op.mem
        if mem.base != 0 or mem.index == 0 or mem.scale != 4 or not self.image.is_code(mem.disp & 0xFFFFFFFF):
            return None
        table = mem.disp & 0xFFFFFFFF
        index = insn.reg_name(mem.index)
        count = None
        prev = self._previous(f, insn.address, 8)
        byte_table = None
        for p in prev:
            if p.mnemonic in ("mov", "movzx") and len(p.operands) == 2 and p.operands[0].type == X.X86_OP_REG \
                    and _same_reg(p.reg_name(p.operands[0].reg), index) and p.operands[1].type == X.X86_OP_MEM \
                    and p.operands[1].size == 1:
                m2 = p.operands[1].mem
                byte_table = (m2.disp & 0xFFFFFFFF, p.reg_name(m2.base) if m2.base else p.reg_name(m2.index))
                index = byte_table[1]
                continue
            if p.mnemonic == "cmp" and p.operands[0].type == X.X86_OP_REG and p.operands[1].type == X.X86_OP_IMM \
                    and _same_reg(p.reg_name(p.operands[0].reg), index):
                count = (p.operands[1].imm & 0xFFFFFFFF) + 1
                break
        if count is not None and count > 4096:
            count = None
        if byte_table and count is not None:
            self._table_bytes.update(range(byte_table[0], byte_table[0] + count))
            count = max(self.image.read(byte_table[0], count)) + 1
        if count is None:
            # No bound check found: read entries while they point into this function's area.
            count = 0
            while count < 512:
                t = self.image.u32(table + 4 * count)
                if not self.image.is_code(t) or abs(t - f.entry) > 0x20000:
                    break
                count += 1
        targets = [self.image.u32(table + 4 * i) for i in range(count)]
        if not targets or not all(self.image.is_code(t) for t in targets):
            return None
        self._table_bytes.update(range(table, table + 4 * count))
        return targets


def _same_reg(a, b):
    groups = [("eax", "ax", "al", "ah"), ("ebx", "bx", "bl", "bh"), ("ecx", "cx", "cl", "ch"), ("edx", "dx", "dl", "dh"),
              ("esi", "si"), ("edi", "di"), ("ebp", "bp"), ("esp", "sp")]
    return any(a in g and b in g for g in groups)
