"""Translates the instructions of one function into C.

The generated function keeps the eight registers in locals and the flags in a local Flags value (the
operands and the result of the last instruction that set them; rt.h computes a flag when code reads it).
Before a call the locals go back into `cpu` and after it they come back. Memory is the flat block `M`.
"""
import re

from capstone import x86_const as X

# Tablas de parches por dirección (instrucción reemplazada, guardas, sitios de llamada). Vacías: el 2001 no usa.
SITES = {}
GUARDS = {}
INSNS = {}
PATCHES = {}
TRUE_WIDTH = set()

PATCHED = set()     # the addresses of PATCHES that a translation used
GUARDS_SEEN = set()  # the addresses of crashguards.py GUARDS that a translation used
SITES_SEEN = set()  # the addresses of callsites.py SITES that a translation checked

REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
LOW16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
LOW8 = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
HIGH8 = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
MASK = {1: "0xffu", 2: "0xffffu", 4: "0xffffffffu"}
MSB = {1: 0x80, 2: 0x8000, 4: 0x80000000}
STYPE = {1: "int8_t", 2: "int16_t", 4: "int32_t"}
SZ = {1: "SZ1", 2: "SZ2", 4: "SZ4"}

CONDS = {"o": "cc_o", "no": "!cc_o", "b": "cc_b", "ae": "!cc_b", "e": "cc_e", "ne": "!cc_e", "be": "cc_be",
         "a": "!cc_be", "s": "cc_s", "ns": "!cc_s", "p": "cc_p", "np": "!cc_p", "l": "cc_l", "ge": "!cc_l",
         "le": "cc_le", "g": "!cc_le"}
ALIASES = {"c": "b", "nae": "b", "nb": "ae", "nc": "ae", "z": "e", "nz": "ne", "na": "be", "nbe": "a",
           "pe": "p", "po": "np", "nge": "l", "nl": "ge", "ng": "le", "nle": "g"}


class Unhandled(Exception):
    pass


def cond(cc):
    cc = ALIASES.get(cc, cc)
    c = CONDS[cc]
    return f"!{c[1:]}(F)" if c.startswith("!") else f"{c}(F)"


def h(v):
    return "0x%xu" % (v & 0xFFFFFFFF)


class FunctionWriter:
    def __init__(self, func, image, names, own_name=None):
        """names(addr) gives the C name of the function at a call target. own_name is the name of the
        C function to write, when it is not names(entry): the body of a wrapped function."""
        self.f = func
        self.own_name = own_name
        self.image = image
        self.names = names
        self.out = []
        self.handled = 0
        self.unhandled = []
        self.features = set()

    # Output
    def e(self, line):
        self.out.append("    " + line)

    # Registers
    def reg_read(self, name):
        if name in REG32:
            return name
        if name in LOW16:
            return f"({LOW16[name]} & 0xffffu)"
        if name in LOW8:
            return f"({LOW8[name]} & 0xffu)"
        if name in HIGH8:
            return f"(({HIGH8[name]} >> 8) & 0xffu)"
        raise Unhandled("register " + name)

    def reg_write(self, name, v):
        if name in REG32:
            return f"{name} = {v};"
        if name in LOW16:
            r = LOW16[name]
            return f"{r} = ({r} & 0xffff0000u) | (({v}) & 0xffffu);"
        if name in LOW8:
            r = LOW8[name]
            return f"{r} = ({r} & 0xffffff00u) | (({v}) & 0xffu);"
        if name in HIGH8:
            r = HIGH8[name]
            return f"{r} = ({r} & 0xffff00ffu) | ((({v}) & 0xffu) << 8);"
        raise Unhandled("register " + name)

    # Memory operands
    def addr(self, insn, op, segment=True):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg_read(insn.reg_name(m.base)))
        if m.index:
            idx = self.reg_read(insn.reg_name(m.index))
            parts.append(idx if m.scale == 1 else f"{idx} * {m.scale}u")
        if m.disp or not parts:
            parts.append(h(m.disp))
        expr = " + ".join(parts)
        if segment and m.segment == X.X86_REG_FS:
            self.features.add("fs segment (SEH chain)")
            expr = f"RT_FS_BASE + {expr}"
        elif segment and m.segment not in (0, X.X86_REG_DS, X.X86_REG_ES, X.X86_REG_SS):
            raise Unhandled("segment " + insn.reg_name(m.segment))
        return f"(uint32_t)({expr})"

    def mem_read(self, size, a="ad"):
        return {1: f"R8({a})", 2: f"R16({a})", 4: f"R32({a})"}[size]

    def mem_write(self, size, v, a="ad"):
        return {1: f"W8({a}, {v});", 2: f"W16({a}, {v});", 4: f"W32({a}, {v});"}[size]

    def rd(self, insn, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_read(insn.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return h(op.imm & (0xFFFFFFFF if size == 4 else (1 << (8 * size)) - 1))
        if op.type == X.X86_OP_MEM:
            return self.mem_read(size)
        raise Unhandled("operand")

    def wr(self, insn, op, v):
        if op.type == X.X86_OP_REG:
            return self.reg_write(insn.reg_name(op.reg), v)
        if op.type == X.X86_OP_MEM:
            return self.mem_write(op.size, v)
        raise Unhandled("write operand")

    def setup_mem(self, insn):
        mems = [o for o in insn.operands if o.type == X.X86_OP_MEM]
        if len(mems) == 1:
            self.e(f"ad = {self.addr(insn, mems[0])};")

    def mask(self, expr, size):
        return expr if size == 4 else f"(({expr}) & {MASK[size]})"

    def sx(self, expr, size):
        return expr if size == 4 else f"(uint32_t)(int32_t)({STYPE[size]})({expr})"

    def setf(self, kind, size, a, b, r, c):
        self.e(f"SETF(K_{kind} | {SZ[size]}, {a}, {b}, {r}, {c});")

    # Calls
    def call_direct(self, target, ret):
        self.e(f"esp -= 4; W32(esp, {h(ret)}); SYNC(); {self.names(target)}(); LOAD(); RT_AFTER_CALL();")

    # Main loop
    def write(self):
        f = self.f
        addrs = sorted(f.insns)
        body = []
        self.out = body
        need_label = set(f.labels) | {f.entry}
        for a in GUARDS:
            if a in f.insns:   # the exits of the crash guards
                need_label |= {int(x, 16) for x in re.findall(r"L_([0-9a-f]+)", GUARDS[a][2])}
        for i, a in enumerate(addrs):
            nxt = a + f.insns[a].size
            if (addrs[i + 1] if i + 1 < len(addrs) else None) != nxt:
                need_label.add(nxt)
        gotos = set()
        for i, a in enumerate(addrs):
            insn = f.insns[a]
            if a in need_label or a in gotos:
                body.append(f"L_{a:x}:")
            text = (insn.mnemonic + " " + insn.op_str).strip().replace("*/", "* /")
            self.e(f"/* {a:x}: {text} */")
            mark = len(body)
            try:
                if a in GUARDS:
                    self.guard(insn)
                if a in INSNS:
                    falls = self.replace_insn(insn)
                else:
                    falls = self.insn(insn)
                self.handled += 1
                if a in GUARDS:
                    GUARDS_SEEN.add(a)      # only when the guarded instruction is translated too
                if a in PATCHES:
                    self.patch(body, mark, a)
                if a in TRUE_WIDTH:
                    if text != "call 0x5e5ad0":
                        raise SystemExit(f"widescreen.py: the instruction at {a:#x} is not call 0x5e5ad0")
                    PATCHED.add(a)
                if a in SITES:
                    name, target = SITES[a]
                    if text != f"call {target:#x}" or insn.size != 5:
                        raise SystemExit(f"callsites.py: the instruction at {a:#x} ({name}) is not call {target:#x}")
                    SITES_SEEN.add(a)
            except Unhandled as exc:
                del body[mark:]
                self.unhandled.append((a, text, str(exc)))
                self.e(f'rt_unimpl({h(a)}, "{text}");')
                falls = False
            if falls:
                nxt = a + insn.size
                following = addrs[i + 1] if i + 1 < len(addrs) else None
                if following != nxt:
                    self.e(f"goto L_{nxt:x};")
                    gotos.add(nxt)
        # Branch targets where the decoder found no instruction.
        lines = list(body)
        for x in sorted(need_label - set(f.insns)):
            if any(f"L_{x:x};" in ln for ln in body):
                lines.append(f"L_{x:x}:")
                if x in f.tails:
                    lines.append(f"    SYNC(); {self.names(x)}(); return;")
                else:
                    lines.append(f"    rt_fell_off({h(x)});")
        name = self.own_name or self.names(f.entry)
        head = [
            f"void {name}(void)",
            "{",
            "    uint8_t *const M = g_mem;",
            "    uint32_t eax = cpu.eax, ecx = cpu.ecx, edx = cpu.edx, ebx = cpu.ebx;",
            "    uint32_t esp = cpu.esp, ebp = cpu.ebp, esi = cpu.esi, edi = cpu.edi;",
            "    const uint32_t esp0 = esp;   /* for RT_AFTER_CALL: the frame is gone once esp passes it */",
            "    uint32_t ad = 0, t0 = 0, t1 = 0, t2 = 0, t3 = 0; uint64_t q0 = 0; double d0 = 0;",
            "    Flags F = {K_RAW | SZ4, 0, 0, 2, 0};",
            "    (void)esp0; (void)ad; (void)t0; (void)t1; (void)t2; (void)t3; (void)q0; (void)d0; (void)M;",
        ]
        if f.entry != addrs[0]:
            head.append(f"    goto L_{f.entry:x};")
        return "\n".join(head + lines + ["}"]) + "\n"

    def replace_insn(self, insn):
        """Writes the C code of widescreen.py INSNS in place of the instruction."""
        text, code = INSNS[insn.address]
        if (insn.mnemonic + " " + insn.op_str).strip() != text:
            raise SystemExit(f"widescreen.py: the instruction at {insn.address:#x} is not {text}")
        self.e(code)
        PATCHED.add(insn.address)
        return True

    def guard(self, insn):
        """Writes the NULL test of crashguards.py GUARDS before the instruction."""
        text, test, code = GUARDS[insn.address]
        if (insn.mnemonic + " " + insn.op_str).strip() != text:
            raise SystemExit(f"crashguards.py: the instruction at {insn.address:#x} is not {text}")
        self.e(f"if ({test}) {{ {code} }}")

    def patch(self, body, mark, a):
        """Replaces the constant of the instruction at a with its run-time value (widescreen.py)."""
        value, expr = PATCHES[a]
        text = h(value)
        if not any(text in line for line in body[mark:]):
            raise SystemExit(f"widescreen.py: the instruction at {a:#x} has no constant {value}")
        body[mark:] = [line.replace(text, expr) for line in body[mark:]]
        PATCHED.add(a)

    # One instruction. Returns True when execution can continue at the next address.
    def insn(self, insn):
        m = insn.mnemonic
        ops = insn.operands
        prefix = None
        if " " in m:
            prefix, m = m.split(" ", 1)
            if prefix not in ("rep", "repe", "repne"):
                raise Unhandled("prefix " + prefix)
        if prefix or m in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "lodsb", "lodsw", "lodsd",
                           "scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd"):
            return self.string(insn, prefix, m)
        if m.startswith("f") or m == "wait":
            return self.x87(insn, m)
        handler = getattr(self, "i_" + m, None)
        if handler is None:
            if m.startswith("j") and (m[1:] in CONDS or m[1:] in ALIASES):
                return self.jcc(insn, m[1:])
            if m.startswith("set"):
                return self.setcc(insn, m[3:])
            if m.startswith("cmov"):
                return self.cmovcc(insn, m[4:])
            raise Unhandled(m)
        return handler(insn, ops)

    # Data movement
    def i_mov(self, insn, ops):
        self.setup_mem(insn)
        self.e(self.wr(insn, ops[0], self.rd(insn, ops[1], ops[0].size)))
        return True

    def i_movzx(self, insn, ops):
        self.setup_mem(insn)
        self.e(self.wr(insn, ops[0], self.rd(insn, ops[1])))
        return True

    def i_movsx(self, insn, ops):
        self.setup_mem(insn)
        v = self.sx(self.rd(insn, ops[1]), ops[1].size)
        self.e(self.wr(insn, ops[0], v))
        return True

    def i_lea(self, insn, ops):
        self.e(self.wr(insn, ops[0], self.addr(insn, ops[1], segment=False)))
        return True

    def i_xchg(self, insn, ops):
        self.setup_mem(insn)
        self.e(f"t0 = {self.rd(insn, ops[0])}; t1 = {self.rd(insn, ops[1])};")
        self.e(self.wr(insn, ops[0], "t1"))
        self.e(self.wr(insn, ops[1], "t0"))
        return True

    def i_bswap(self, insn, ops):
        r = insn.reg_name(ops[0].reg)
        self.e(f"{r} = __builtin_bswap32({r});")
        return True

    def i_push(self, insn, ops):
        op = ops[0]
        size = op.size or 4
        if op.type == X.X86_OP_IMM:
            v = h(op.imm)
        else:
            self.setup_mem(insn)
            v = self.rd(insn, op)
        self.e(f"t0 = {v}; esp -= {size}; " + self.mem_write(size, "t0", "esp"))
        return True

    def i_pop(self, insn, ops):
        op = ops[0]
        size = op.size or 4
        self.e(f"t0 = {self.mem_read(size, 'esp')}; esp += {size};")
        if op.type == X.X86_OP_MEM:
            self.e(f"ad = {self.addr(insn, op)};")
        self.e(self.wr(insn, op, "t0"))
        return True

    def i_pushal(self, insn, ops):
        self.e("t0 = esp; esp -= 32; W32(esp + 28, eax); W32(esp + 24, ecx); W32(esp + 20, edx); W32(esp + 16, ebx);")
        self.e("W32(esp + 12, t0); W32(esp + 8, ebp); W32(esp + 4, esi); W32(esp, edi);")
        return True

    def i_popal(self, insn, ops):
        self.e("edi = R32(esp); esi = R32(esp + 4); ebp = R32(esp + 8); ebx = R32(esp + 16);")
        self.e("edx = R32(esp + 20); ecx = R32(esp + 24); eax = R32(esp + 28); esp += 32;")
        return True

    def i_pushfd(self, insn, ops):
        self.e("esp -= 4; W32(esp, f_flags(F) | (cpu.df << 10));")
        return True

    def i_popfd(self, insn, ops):
        self.e("t0 = R32(esp); esp += 4; cpu.df = (t0 >> 10) & 1; SETF(K_RAW | SZ4, 0, 0, t0, 0);")
        return True

    def i_leave(self, insn, ops):
        self.e("esp = ebp; ebp = R32(esp); esp += 4;")
        return True

    def i_cdq(self, insn, ops):
        self.e("edx = (uint32_t)((int32_t)eax >> 31);")
        return True

    def i_cwde(self, insn, ops):
        self.e("eax = (uint32_t)(int32_t)(int16_t)eax;")
        return True

    def i_cbw(self, insn, ops):
        self.e("eax = (eax & 0xffff0000u) | ((uint32_t)(int16_t)(int8_t)eax & 0xffffu);")
        return True

    def i_cwd(self, insn, ops):
        self.e("edx = (edx & 0xffff0000u) | (((int16_t)eax < 0) ? 0xffffu : 0);")
        return True

    def i_lahf(self, insn, ops):
        self.e("eax = (eax & 0xffff00ffu) | ((f_flags(F) & 0xffu) << 8);")
        return True

    def i_sahf(self, insn, ops):
        self.e("t0 = (f_flags(F) & ~0xd5u) | ((eax >> 8) & 0xd5u); SETF(K_RAW | SZ4, 0, 0, t0, 0);")
        return True

    def i_nop(self, insn, ops):
        return True

    def i_cld(self, insn, ops):
        self.e("cpu.df = 0;")
        return True

    def i_std(self, insn, ops):
        self.features.add("direction flag (std)")
        self.e("cpu.df = 1;")
        return True

    def setcc(self, insn, cc):
        self.setup_mem(insn)
        self.e(self.wr(insn, insn.operands[0], f"(uint32_t){cond(cc)}"))
        return True

    def cmovcc(self, insn, cc):
        self.setup_mem(insn)
        ops = insn.operands
        self.e(f"if ({cond(cc)}) {{ {self.wr(insn, ops[0], self.rd(insn, ops[1]))} }}")
        return True

    # Arithmetic
    def _binop(self, insn, ops, kind):
        self.setup_mem(insn)
        size = ops[0].size
        a = self.rd(insn, ops[0])
        b = self.rd(insn, ops[1], size)
        if ops[1].type == X.X86_OP_IMM:
            b = h(ops[1].imm & int(MASK[size][:-1], 16))
        self.e(f"t0 = {a}; t1 = {b};")
        return size

    def _arith(self, insn, ops, op, carry, store=True):
        size = self._binop(insn, ops, None)
        c = "0"
        if carry:
            self.e("t3 = f_cf(F);")
            c = "t3"
        if op == "+":
            self.e(f"t2 = {self.mask(f't0 + t1 + {c}', size)};")
            self.setf("ADD", size, "t0", "t1", "t2", c)
        else:
            self.e(f"t2 = {self.mask(f't0 - t1 - {c}', size)};")
            self.setf("SUB", size, "t0", "t1", "t2", c)
        if store:
            self.e(self.wr(insn, ops[0], "t2"))
        return True

    def i_add(self, insn, ops):
        return self._arith(insn, ops, "+", False)

    def i_adc(self, insn, ops):
        return self._arith(insn, ops, "+", True)

    def i_sub(self, insn, ops):
        return self._arith(insn, ops, "-", False)

    def i_sbb(self, insn, ops):
        return self._arith(insn, ops, "-", True)

    def i_cmp(self, insn, ops):
        return self._arith(insn, ops, "-", False, store=False)

    def _logic(self, insn, ops, op, store=True):
        size = self._binop(insn, ops, None)
        self.e(f"t2 = t0 {op} t1;")
        self.setf("RES", size, "0", "0", "t2", "0")
        if store:
            self.e(self.wr(insn, ops[0], "t2"))
        return True

    def i_and(self, insn, ops):
        return self._logic(insn, ops, "&")

    def i_or(self, insn, ops):
        return self._logic(insn, ops, "|")

    def i_xor(self, insn, ops):
        return self._logic(insn, ops, "^")

    def i_test(self, insn, ops):
        return self._logic(insn, ops, "&", store=False)

    def i_not(self, insn, ops):
        self.setup_mem(insn)
        size = ops[0].size
        self.e(self.wr(insn, ops[0], self.mask(f"~{self.rd(insn, ops[0])}", size)))
        return True

    def i_neg(self, insn, ops):
        self.setup_mem(insn)
        size = ops[0].size
        self.e(f"t1 = {self.rd(insn, ops[0])}; t2 = {self.mask('0u - t1', size)};")
        self.setf("SUB", size, "0", "t1", "t2", "0")
        self.e(self.wr(insn, ops[0], "t2"))
        return True

    def _incdec(self, insn, ops, kind, op):
        self.setup_mem(insn)
        size = ops[0].size
        self.e(f"t3 = f_cf(F); t0 = {self.rd(insn, ops[0])}; t2 = {self.mask(f't0 {op} 1u', size)};")
        self.setf(kind, size, "t0", "1u", "t2", "t3")
        self.e(self.wr(insn, ops[0], "t2"))
        return True

    def i_inc(self, insn, ops):
        return self._incdec(insn, ops, "INC", "+")

    def i_dec(self, insn, ops):
        return self._incdec(insn, ops, "DEC", "-")

    def i_imul(self, insn, ops):
        self.setup_mem(insn)
        if len(ops) == 1:
            size = ops[0].size
            src = self.rd(insn, ops[0])
            if size == 4:
                self.e(f"q0 = (uint64_t)((int64_t)(int32_t)eax * (int64_t)(int32_t){src});")
                self.e("eax = (uint32_t)q0; edx = (uint32_t)(q0 >> 32);")
                self.setf("RES", 4, "0", "0", "eax", "((int64_t)q0 != (int64_t)(int32_t)q0) ? 3u : 0u")
            elif size == 2:
                self.e(f"t2 = (uint32_t)((int32_t)(int16_t)eax * (int32_t)(int16_t){src});")
                self.e("eax = (eax & 0xffff0000u) | (t2 & 0xffffu); edx = (edx & 0xffff0000u) | (t2 >> 16);")
                self.setf("RES", 2, "0", "0", "t2", "((int32_t)t2 != (int32_t)(int16_t)t2) ? 3u : 0u")
            else:
                self.e(f"t2 = (uint32_t)((int32_t)(int8_t)eax * (int32_t)(int8_t){src});")
                self.e("eax = (eax & 0xffff0000u) | (t2 & 0xffffu);")
                self.setf("RES", 1, "0", "0", "t2", "((int32_t)(int16_t)t2 != (int32_t)(int8_t)t2) ? 3u : 0u")
            return True
        size = ops[0].size
        if len(ops) == 2:
            a, b = self.rd(insn, ops[0]), self.rd(insn, ops[1])
        else:
            a, b = self.rd(insn, ops[1]), h(ops[2].imm)
        st = STYPE[size]
        self.e(f"q0 = (uint64_t)((int64_t)({st})({a}) * (int64_t)({st})({b}));")
        self.e(f"t2 = {self.mask('(uint32_t)q0', size)};")
        self.setf("RES", size, "0", "0", "t2", f"((int64_t)q0 != (int64_t)({st})q0) ? 3u : 0u")
        self.e(self.wr(insn, ops[0], "t2"))
        return True

    def i_mul(self, insn, ops):
        self.setup_mem(insn)
        size = ops[0].size
        src = self.rd(insn, ops[0])
        if size == 4:
            self.e(f"q0 = (uint64_t)eax * (uint64_t){src}; eax = (uint32_t)q0; edx = (uint32_t)(q0 >> 32);")
            self.setf("RES", 4, "0", "0", "eax", "edx ? 3u : 0u")
        elif size == 2:
            self.e(f"t2 = (eax & 0xffffu) * {src};")
            self.e("eax = (eax & 0xffff0000u) | (t2 & 0xffffu); edx = (edx & 0xffff0000u) | (t2 >> 16);")
            self.setf("RES", 2, "0", "0", "t2", "(t2 >> 16) ? 3u : 0u")
        else:
            self.e(f"t2 = (eax & 0xffu) * {src}; eax = (eax & 0xffff0000u) | t2;")
            self.setf("RES", 1, "0", "0", "t2", "(t2 >> 8) ? 3u : 0u")
        return True

    def _div(self, insn, ops, signed):
        self.setup_mem(insn)
        size = ops[0].size
        src = self.rd(insn, ops[0])
        a = h(insn.address)
        if size == 4:
            if signed:
                self.e(f"t1 = {src}; rt_idiv32({a}, &eax, &edx, t1);")
            else:
                self.e(f"t1 = {src}; rt_div32({a}, &eax, &edx, t1);")
        elif size == 2:
            fn = "rt_idiv16" if signed else "rt_div16"
            self.e(f"t1 = {src}; {fn}({a}, &eax, &edx, t1);")
        else:
            fn = "rt_idiv8" if signed else "rt_div8"
            self.e(f"t1 = {src}; {fn}({a}, &eax, t1);")
        return True

    def i_div(self, insn, ops):
        return self._div(insn, ops, False)

    def i_idiv(self, insn, ops):
        return self._div(insn, ops, True)

    def _shift(self, insn, ops, kind):
        self.setup_mem(insn)
        size = ops[0].size
        bits = size * 8
        if len(ops) == 1:
            count = "1u"
        elif ops[1].type == X.X86_OP_IMM:
            n = ops[1].imm & 31
            if n == 0:
                return True
            count = f"{n}u"
        else:
            count = "(ecx & 31u)"
        self.e(f"t1 = {count};")
        self.e("if (t1) {")
        self.e(f"    t0 = {self.rd(insn, ops[0])};")
        if kind == "shl":
            self.e(f"    t2 = {self.mask('t0 << t1', size)};")
            self.e(f"    t3 = (t1 <= {bits}u) ? ((uint32_t)((uint64_t)t0 >> ({bits}u - t1)) & 1u) : 0u;")
            self.e(f"    t3 |= ((((t2 >> {bits - 1}) & 1u) ^ t3) << 1);")
        elif kind == "shr":
            self.e("    t2 = t0 >> t1;")
            self.e(f"    t3 = ((t0 >> (t1 - 1u)) & 1u) | (((t0 >> {bits - 1}) & 1u) << 1);")
        elif kind == "sar":
            st = STYPE[size]
            self.e(f"    t2 = {self.mask(f'(uint32_t)((int32_t)({st})t0 >> t1)', size)};")
            self.e(f"    t3 = (uint32_t)((int32_t)({st})t0 >> (t1 - 1u)) & 1u;")
        elif kind in ("rol", "ror"):
            self.features.add("rotates")
            self.e(f"    t1 &= {bits - 1}u;")
            if kind == "rol":
                self.e(f"    t2 = t1 ? {self.mask(f't0 << t1 | t0 >> ({bits}u - t1)', size)} : t0;")
                self.e(f"    t3 = t2 & 1u; t3 |= ((((t2 >> {bits - 1}) & 1u) ^ t3) << 1);")
            else:
                self.e(f"    t2 = t1 ? {self.mask(f't0 >> t1 | t0 << ({bits}u - t1)', size)} : t0;")
                self.e(f"    t3 = (t2 >> {bits - 1}) & 1u; t3 |= ((t3 ^ ((t2 >> {bits - 2}) & 1u)) << 1);")
            self.e("    t3 = (f_flags(F) & ~0x801u) | (t3 & 1u) | ((t3 >> 1) << 11);")
            self.e("    SETF(K_RAW | SZ4, 0, 0, t3, 0);")
            self.e("    " + self.wr(insn, ops[0], "t2"))
            self.e("}")
            return True
        elif kind in ("rcl", "rcr"):
            self.features.add("rotates through carry")
            self.e(f"    t1 %= {bits + 1}u; t3 = f_cf(F); t2 = t0;")
            self.e("    for (uint32_t n_ = 0; n_ < t1; n_++) {")
            if kind == "rcr":
                self.e(f"        uint32_t c_ = t2 & 1u; t2 = (t2 >> 1) | (t3 << {bits - 1}); t3 = c_;")
            else:
                self.e(f"        uint32_t c_ = (t2 >> {bits - 1}) & 1u; t2 = {self.mask('(t2 << 1) | t3', size)}; t3 = c_;")
            self.e("    }")
            if kind == "rcr":
                self.e(f"    t0 = ((t2 >> {bits - 1}) ^ (t2 >> {bits - 2})) & 1u;")
            else:
                self.e(f"    t0 = ((t2 >> {bits - 1}) & 1u) ^ t3;")
            self.e("    t3 = (f_flags(F) & ~0x801u) | t3 | (t0 << 11);")
            self.e("    SETF(K_RAW | SZ4, 0, 0, t3, 0);")
            self.e("    " + self.wr(insn, ops[0], "t2"))
            self.e("}")
            return True
        self.e(f"    SETF(K_RES | {SZ[size]}, 0, 0, t2, t3);")
        self.e("    " + self.wr(insn, ops[0], "t2"))
        self.e("}")
        return True

    def i_shl(self, insn, ops):
        return self._shift(insn, ops, "shl")

    i_sal = i_shl

    def i_shr(self, insn, ops):
        return self._shift(insn, ops, "shr")

    def i_sar(self, insn, ops):
        return self._shift(insn, ops, "sar")

    def i_rol(self, insn, ops):
        return self._shift(insn, ops, "rol")

    def i_ror(self, insn, ops):
        return self._shift(insn, ops, "ror")

    def i_rcl(self, insn, ops):
        return self._shift(insn, ops, "rcl")

    def i_rcr(self, insn, ops):
        return self._shift(insn, ops, "rcr")

    def _double_shift(self, insn, ops, left):
        if ops[0].size != 4:
            raise Unhandled("16-bit double shift")
        self.setup_mem(insn)
        count = f"{ops[2].imm & 31}u" if ops[2].type == X.X86_OP_IMM else "(ecx & 31u)"
        self.e(f"t1 = {count};")
        self.e("if (t1) {")
        self.e(f"    t0 = {self.rd(insn, ops[0])}; t3 = {self.rd(insn, ops[1])};")
        if left:
            self.e("    t2 = (t0 << t1) | (t3 >> (32u - t1)); t3 = (t0 >> (32u - t1)) & 1u;")
        else:
            self.e("    t2 = (t0 >> t1) | (t3 << (32u - t1)); t3 = (t0 >> (t1 - 1u)) & 1u;")
        self.e("    t3 |= (((t0 ^ t2) >> 31) & 1u) << 1;")
        self.e("    SETF(K_RES | SZ4, 0, 0, t2, t3);")
        self.e("    " + self.wr(insn, ops[0], "t2"))
        self.e("}")
        return True

    def i_shld(self, insn, ops):
        return self._double_shift(insn, ops, True)

    def i_shrd(self, insn, ops):
        return self._double_shift(insn, ops, False)

    def _bt(self, insn, ops, action):
        self.setup_mem(insn)
        if ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG:
            # The bit offset of a register can reach past the addressed dword.
            self.e(f"t1 = {self.rd(insn, ops[1])}; ad += (uint32_t)(((int32_t)t1 >> 5) * 4); t1 &= 31u;")
        else:
            self.e(f"t1 = {self.rd(insn, ops[1])} & 31u;")
        self.e(f"t0 = {self.rd(insn, ops[0])}; t3 = (t0 >> t1) & 1u;")
        self.e("SETF(K_RAW | SZ4, 0, 0, (f_flags(F) & ~1u) | t3, 0);")
        if action:
            self.e(self.wr(insn, ops[0], {"s": "t0 | (1u << t1)", "r": "t0 & ~(1u << t1)", "c": "t0 ^ (1u << t1)"}[action]))
        return True

    def i_bt(self, insn, ops):
        return self._bt(insn, ops, None)

    def i_bts(self, insn, ops):
        return self._bt(insn, ops, "s")

    def i_btr(self, insn, ops):
        return self._bt(insn, ops, "r")

    def i_btc(self, insn, ops):
        return self._bt(insn, ops, "c")

    # Control flow
    def jcc(self, insn, cc):
        target = insn.operands[0].imm & 0xFFFFFFFF
        self.e(f"if ({cond(cc)}) goto L_{target:x};")
        return True

    def i_jecxz(self, insn, ops):
        self.e(f"if (!ecx) goto L_{ops[0].imm & 0xFFFFFFFF:x};")
        return True

    def i_loop(self, insn, ops):
        self.e(f"if (--ecx) goto L_{ops[0].imm & 0xFFFFFFFF:x};")
        return True

    def import_name(self, op):
        if op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
            imp = self.image.imports.get(op.mem.disp & 0xFFFFFFFF)
            if imp:
                return "imp_" + imp[1]
        return None

    def i_call(self, insn, ops):
        op = ops[0]
        ret = insn.address + insn.size
        if op.type == X.X86_OP_IMM:
            self.call_direct(op.imm & 0xFFFFFFFF, ret)
            return True
        imp = self.import_name(op)
        if imp:
            self.e(f"esp -= 4; W32(esp, {h(ret)}); SYNC(); {imp}(); LOAD(); RT_AFTER_CALL();")
            return True
        self.features.add("indirect calls")
        self.setup_mem(insn)
        self.e(f"t0 = {self.rd(insn, op)}; esp -= 4; W32(esp, {h(ret)}); SYNC(); rt_call(t0); LOAD(); RT_AFTER_CALL();")
        return True

    def i_lcall(self, insn, ops):
        raise Unhandled("far call")

    def i_jmp(self, insn, ops):
        op = ops[0]
        f = self.f
        if op.type == X.X86_OP_IMM:
            target = op.imm & 0xFFFFFFFF
            if target in f.tails:
                self.features.add("tail jumps to other functions")
                self.e(f"SYNC(); {self.names(target)}(); return;")
            else:
                self.e(f"goto L_{target:x};")
            return False
        imp = self.import_name(op)
        if imp:
            self.features.add("import thunks (jmp [iat])")
            self.e(f"SYNC(); {imp}(); return;")
            return False
        self.setup_mem(insn)
        if insn.address in f.tables:
            self.features.add("jump tables")
            self.e(f"switch ({self.rd(insn, op)}) {{")
            for t in sorted(set(f.tables[insn.address])):
                self.e(f"case {h(t)}: goto L_{t:x};")
            self.e(f"default: rt_bad_jump({h(insn.address)}, {self.rd(insn, op)});")
            self.e("}")
            return False
        self.features.add("indirect jumps")
        self.e(f"t0 = {self.rd(insn, op)}; SYNC(); rt_jump(t0); return;")
        return False

    def i_ret(self, insn, ops):
        n = ops[0].imm if ops else 0
        if n:
            self.features.add("stdcall cleanup (ret n)")
        self.e(f"esp += {4 + n}; SYNC(); return;")
        return False

    def i_int3(self, insn, ops):
        self.e(f'rt_unimpl({h(insn.address)}, "int3");')
        return False

    i_hlt = i_int3

    # String instructions
    def string(self, insn, prefix, m):
        kind, size = m[:4], {"b": 1, "w": 2, "d": 4}[m[4]]
        self.features.add("string instructions")
        step = f"(cpu.df ? -{size}u : {size}u)"
        acc = {1: "(eax & 0xffu)", 2: "(eax & 0xffffu)", 4: "eax"}[size]
        if kind == "movs":
            if prefix:
                self.e(f"rt_rep_movs(M, {size}, &ecx, &esi, &edi);")
            else:
                self.e(f"{self.mem_write(size, self.mem_read(size, 'esi'), 'edi')} esi += {step}; edi += {step};")
            return True
        if kind == "stos":
            if prefix:
                self.e(f"rt_rep_stos(M, {size}, &ecx, &edi, eax);")
            else:
                self.e(f"{self.mem_write(size, acc, 'edi')} edi += {step};")
            return True
        if kind == "lods":
            if prefix:
                raise Unhandled("rep lods")
            self.e(self.reg_write({1: "al", 2: "ax", 4: "eax"}[size], self.mem_read(size, "esi")) + f" esi += {step};")
            return True
        if kind == "scas":
            body = f"t0 = {acc}; t1 = {self.mem_read(size, 'edi')}; edi += {step}; " \
                   f"SETF(K_SUB | {SZ[size]}, t0, t1, {self.mask('t0 - t1', size)}, 0);"
        elif kind == "cmps":
            body = f"t0 = {self.mem_read(size, 'esi')}; t1 = {self.mem_read(size, 'edi')}; esi += {step}; edi += {step}; " \
                   f"SETF(K_SUB | {SZ[size]}, t0, t1, {self.mask('t0 - t1', size)}, 0);"
        else:
            raise Unhandled(m)
        if not prefix:
            self.e(body)
            return True
        if prefix == "rep":
            prefix = "repe"
        stop = "t0 != t1" if prefix == "repe" else "t0 == t1"
        self.e(f"while (ecx) {{ ecx--; {body} if ({stop}) break; }}")
        return True

    # x87
    def x87(self, insn, m):
        self.features.add("x87")
        ops = insn.operands
        b = insn.bytes
        # Skip a wait or segment prefix to reach the opcode.
        if m in ("wait", "fwait"):
            return True
        k = 0
        while k < len(b) - 1 and b[k] in (0x9B, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66):
            k += 1
        if m == "wait":
            return True
        opc, modrm = b[k], b[k + 1] if k + 1 < len(b) else 0
        if modrm >= 0xC0:
            return self.x87_reg(insn, opc, modrm)
        self.setup_mem(insn)
        size = ops[0].size if ops else 0
        load = {4: "rdf32(M, ad)", 8: "rdf64(M, ad)", 10: "rdf80(M, ad)"}
        iload = {2: "(double)(int16_t)R16(ad)", 4: "(double)(int32_t)R32(ad)", 8: "(double)(int64_t)R64(ad)"}
        arith = {"fadd": "ST(0) = ST(0) + {v};", "fmul": "ST(0) = ST(0) * {v};", "fsub": "ST(0) = ST(0) - {v};",
                 "fsubr": "ST(0) = {v} - ST(0);", "fdiv": "ST(0) = ST(0) / {v};", "fdivr": "ST(0) = {v} / ST(0);"}
        if m == "fld":
            if size == 10:
                self.features.add("x87 80-bit loads and stores")
            self.e(f"fpush({load[size]});")
        elif m in ("fst", "fstp"):
            if size == 10:
                self.features.add("x87 80-bit loads and stores")
            self.e({4: "wrf32(M, ad, ST(0));", 8: "wrf64(M, ad, ST(0));", 10: "wrf80(M, ad, ST(0));"}[size])
            if m == "fstp":
                self.e("fpop();")
        elif m == "fild":
            self.e(f"fpush({iload[size]});")
        elif m in ("fist", "fistp"):
            self.e({2: "W16(ad, (uint16_t)fist16(ST(0)));", 4: "W32(ad, (uint32_t)fist32(ST(0)));",
                    8: "W64(ad, (uint64_t)fist64(ST(0)));"}[size])
            if m == "fistp":
                self.e("fpop();")
        elif m in arith:
            self.e(arith[m].format(v=load[size]))
        elif m[:2] == "fi" and "f" + m[2:] in arith:
            self.e(arith["f" + m[2:]].format(v=iload[size]))
        elif m in ("fcom", "fcomp"):
            self.e(f"fcompare(ST(0), {load[size]});")
            if m == "fcomp":
                self.e("fpop();")
        elif m in ("ficom", "ficomp"):
            self.e(f"fcompare(ST(0), {iload[size]});")
            if m == "ficomp":
                self.e("fpop();")
        elif m == "fldcw":
            self.features.add("x87 control word (rounding)")
            self.e("fpu.cw = R16(ad);")
        elif m == "fnstcw":
            self.e("W16(ad, fpu.cw);")
        elif m == "fnstsw":
            self.e("W16(ad, fstatus());")
        elif m == "fnsave":
            self.e("rt_fnsave(M, ad);")
        elif m == "frstor":
            self.e("rt_frstor(M, ad);")
        else:
            raise Unhandled(m)
        return True

    def x87_reg(self, insn, opc, modrm):
        i = modrm & 7
        r = (modrm >> 3) & 7
        if opc in (0xD8, 0xDC, 0xDE):
            if r in (2, 3):
                if opc == 0xDE and modrm == 0xD9:
                    self.e("fcompare(ST(0), ST(1)); fpop(); fpop();")
                    return True
                self.e(f"fcompare(ST(0), ST({i}));")
                if r == 3:
                    self.e("fpop();")
                return True
            if opc == 0xD8:
                dst, a, b_ = "ST(0)", "ST(0)", f"ST({i})"
                ops = {0: "{a} + {b}", 1: "{a} * {b}", 4: "{a} - {b}", 5: "{b} - {a}", 6: "{a} / {b}", 7: "{b} / {a}"}
            else:
                dst, a, b_ = f"ST({i})", f"ST({i})", "ST(0)"
                ops = {0: "{a} + {b}", 1: "{a} * {b}", 4: "{b} - {a}", 5: "{a} - {b}", 6: "{b} / {a}", 7: "{a} / {b}"}
            self.e(f"{dst} = {ops[r].format(a=a, b=b_)};")
            if opc == 0xDE:
                self.e("fpop();")
            return True
        if opc == 0xD9:
            if r == 0:
                self.e(f"d0 = ST({i}); fpush(d0);")
                return True
            if r == 1:
                self.e(f"d0 = ST(0); ST(0) = ST({i}); ST({i}) = d0;")
                return True
            simple = {0xE0: "ST(0) = -ST(0);", 0xE1: "ST(0) = fabs(ST(0));", 0xE4: "fcompare(ST(0), 0.0);",
                      0xE8: "fpush(1.0);", 0xEB: "fpush(M_PI);", 0xEE: "fpush(0.0);", 0xE9: "fpush(log2(10.0));",
                      0xEA: "fpush(M_LOG2E);", 0xEC: "fpush(log10(2.0));", 0xED: "fpush(M_LN2);",
                      0xF0: "ST(0) = exp2(ST(0)) - 1.0;", 0xF1: "ST(1) = ST(1) * log2(ST(0)); fpop();",
                      0xF3: "ST(1) = atan2(ST(1), ST(0)); fpop();", 0xFA: "ST(0) = sqrt(ST(0));",
                      0xFC: "ST(0) = fround(ST(0));", 0xFD: "ST(0) = ldexp(ST(0), (int)trunc(ST(1)));",
                      0xFE: "ST(0) = sin(ST(0));", 0xFF: "ST(0) = cos(ST(0));"}
            if modrm in simple:
                if modrm in (0xF0, 0xF1, 0xF3, 0xFE, 0xFF, 0xFD):
                    self.features.add("x87 transcendental (libm, may differ in the last bit)")
                self.e(simple[modrm])
                return True
            raise Unhandled("x87 D9 %02x" % modrm)
        if opc == 0xDD:
            if r == 2:
                self.e(f"ST({i}) = ST(0);")
                return True
            if r == 3:
                self.e(f"ST({i}) = ST(0); fpop();")
                return True
            raise Unhandled("x87 DD %02x" % modrm)
        if opc == 0xDF and modrm == 0xE0:
            self.e("eax = (eax & 0xffff0000u) | fstatus();")
            return True
        if opc == 0xDB and modrm in (0xE2, 0xE3):
            self.e("fpu.sw = 0;" if modrm == 0xE2 else "fpu_init();")
            return True
        raise Unhandled("x87 %02x %02x" % (opc, modrm))
