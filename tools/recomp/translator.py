"""Nuestro traductor: el FunctionWriter de translate.py más las instrucciones que el juego y sus DLL
necesitan:

  cpuid            detección de CPU del CRT del exe        -> rt_cpuid() (respuesta fija de un Pentium)
  stc              exe                                     -> CF = 1 sobre las banderas crudas
  xlatb            MSVCRT                                  -> al = [ebx + al]
  lock <op>        MSVCRT (lock inc/dec)                   -> la misma op sin atomicidad (el runtime corre
                                                              los hilos de a uno)
  fprem, fprem1, fptan, fxam, fnstenv, fldenv   x87 de MSVCRT -> helpers rt_* de src/rt/cpu_ext.c

Los helpers se declaran en EXTRA_DECLS, que build.py agrega a functions.h.
"""
from __future__ import annotations

import translate
from capstone import x86_const as X
from translate import FunctionWriter, Unhandled

EXTRA_DECLS = """\
extern int32_t rt_poll_budget;
void rt_poll(void);
#define RT_POLL() do { if (__builtin_expect(--rt_poll_budget <= 0, 0)) { SYNC(); rt_poll(); LOAD(); } } while (0)
void rt_cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
void rt_fprem(int ieee);
void rt_fptan(void);
void rt_fxam(void);
void rt_fnstenv(uint8_t *m, uint32_t a);
void rt_fldenv(uint8_t *m, uint32_t a);
void rt_fsincos(void);
/* MMX: canales de 8, 16 y 32 bits sobre un uint64_t (sin saturación: las que el juego usa son las de vuelta). */
static inline uint64_t mmx_lanes(uint64_t a, uint64_t b, int bits, int op)
{
    uint64_t r = 0, mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    for (int i = 0; i < 64; i += bits) {
        uint64_t x = (a >> i) & mask, y = (b >> i) & mask, v;
        switch (op) {
        case 0: v = x + y; break;                                                        /* padd */
        case 1: v = (uint64_t)(uint32_t)((int32_t)(int16_t)x * (int32_t)(int16_t)y); break;  /* pmullw */
        case 2: v = x == y ? mask : 0; break;                                            /* pcmpeq */
        default: v = x - y; break;                                                       /* psub */
        }
        r |= (v & mask) << i;
    }
    return r;
}
static inline uint64_t mmx_shift(uint64_t a, uint64_t n, int bits, int left)
{
    if (n >= (uint64_t)bits) return 0;
    uint64_t r = 0, mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    for (int i = 0; i < 64; i += bits) {
        uint64_t x = (a >> i) & mask;
        r |= ((left ? x << n : x >> n) & mask) << i;
    }
    return r;
}
static inline uint64_t mmx_unpack(uint64_t a, uint64_t b, int bits, int high)
{
    uint64_t r = 0, mask = (1ull << bits) - 1;
    int base = high ? 32 : 0;
    for (int i = 0, o = 0; i < 32; i += bits, o += 2 * bits) {
        r |= ((a >> (base + i)) & mask) << o;
        r |= ((b >> (base + i)) & mask) << (o + bits);
    }
    return r;
}
#define R64(a) ((uint64_t)R32(a) | (uint64_t)R32((a) + 4u) << 32)
#define W64(a, v) do { uint64_t w64_ = (v); W32((a), (uint32_t)w64_); W32((a) + 4u, (uint32_t)(w64_ >> 32)); } while (0)
"""

LOCKABLE = {"inc", "dec", "add", "sub", "and", "or", "xor", "not", "neg", "xchg", "adc", "sbb", "bts", "btr", "btc"}
X87_REG_EXTRA = {0xF8: "rt_fprem(0);", 0xF5: "rt_fprem(1);", 0xF2: "rt_fptan();", 0xE5: "rt_fxam();",
                 0xFB: "rt_fsincos();"}
# MMX: mnemónico -> (tipo, bits de canal, operación). PC Fútbol 2001 (dcsfx «Software MMX», utils.dll).
MMX_LANES = {"paddb": (8, 0), "paddw": (16, 0), "paddd": (32, 0), "psubb": (8, 3), "psubw": (16, 3), "psubd": (32, 3),
             "pmullw": (16, 1), "pcmpeqb": (8, 2), "pcmpeqw": (16, 2), "pcmpeqd": (32, 2)}
MMX_LOGIC = {"pand": "{a} & {b}", "pandn": "~{a} & {b}", "por": "{a} | {b}", "pxor": "{a} ^ {b}"}
MMX_SHIFT = {"psllw": (16, 1), "pslld": (32, 1), "psllq": (64, 1), "psrlw": (16, 0), "psrld": (32, 0), "psrlq": (64, 0)}
MMX_UNPACK = {"punpcklbw": (8, 0), "punpcklwd": (16, 0), "punpckldq": (32, 0),
              "punpckhbw": (8, 1), "punpckhwd": (16, 1), "punpckhdq": (32, 1)}


class Writer(FunctionWriter):
    def write(self):
        # El analizador marca etiquetas en los saltos del grupo JUMP de capstone, que no incluye loop/jecxz: su
        # destino dentro de la función queda sin etiqueta (PC Fútbol 2001, utils.dll: lodsd; stosd; stosd; loop).
        for insn in self.f.insns.values():
            if insn.mnemonic in ("loop", "loope", "loopne", "jecxz", "jcxz") and insn.operands:
                t = insn.operands[0].imm & 0xFFFFFFFF
                if t in self.f.insns:
                    self.f.labels.add(t)
        return super().write()

    def insn(self, insn):
        m = insn.mnemonic
        if m.startswith("lock "):
            inner = m.split(" ", 1)[1]
            if inner not in LOCKABLE:
                raise Unhandled("prefix lock " + inner)
            self.features.add("lock prefix (sin atomicidad)")
            return getattr(self, "i_" + inner)(insn, insn.operands)
        if m == "cpuid":
            self.features.add("cpuid")
            self.e("rt_cpuid(&eax, &ebx, &ecx, &edx);")
            return True
        if m == "stc":
            self.e("SETF(K_RAW | SZ4, 0, 0, f_flags(F) | 1u, 0);")
            return True
        if m == "clc":
            self.e("SETF(K_RAW | SZ4, 0, 0, f_flags(F) & ~1u, 0);")
            return True
        if m == "cmc":
            self.e("SETF(K_RAW | SZ4, 0, 0, f_flags(F) ^ 1u, 0);")
            return True
        if m in MMX_LANES or m in MMX_LOGIC or m in MMX_SHIFT or m in MMX_UNPACK or m in ("movd", "movq", "emms"):
            return self.mmx(insn, m, insn.operands)
        if m == "xlatb":
            self.e("eax = (eax & 0xffffff00u) | R8((uint32_t)(ebx + (eax & 0xffu)));")
            return True
        return super().insn(insn)

    # Punto de cesión en los saltos hacia atrás (las vueltas de bucle): el juego espera a otros hilos en bucles de
    # espera activa sin llamar a Windows (sub_4BE730 espera el fundido de volumen del hilo de la música); con hilos
    # cooperativos, sin esto nadie más corre. RT_POLL descuenta un presupuesto y al agotarse cede (threads.c).
    def backward(self, insn, target):
        return target <= insn.address and target not in self.f.tails

    # MMX: los registros mm0-7 viven en cpu.mm (por hilo); memoria de 64 bits con R64/W64.
    def mmreg(self, op):
        return f"cpu.mm[{op.reg - X.X86_REG_MM0}]" if op.type == X.X86_OP_REG and X.X86_REG_MM0 <= op.reg <= X.X86_REG_MM7 else None

    def mm64(self, insn, op):
        r = self.mmreg(op)
        if r:
            return r
        if op.type == X.X86_OP_MEM:
            return "R64(ad)"
        if op.type == X.X86_OP_IMM:
            return f"{op.imm & 0xFF}ull"
        raise Unhandled("operando mmx")

    def mmx(self, insn, m, ops):
        self.features.add("mmx")
        if m == "emms":
            return True                                       # el x87 no se modela con etiquetas
        self.setup_mem(insn)
        d, s = ops[0], ops[1]
        if m == "movd":
            if self.mmreg(d):
                self.e(f"{self.mmreg(d)} = (uint64_t)(uint32_t)({self.rd(insn, s, 4)});")
            else:
                self.e(self.wr(insn, d, f"(uint32_t){self.mmreg(s)}"))
            return True
        if m == "movq":
            if self.mmreg(d):
                self.e(f"{self.mmreg(d)} = {self.mm64(insn, s)};")
            elif d.type == X.X86_OP_MEM:
                self.e(f"W64(ad, {self.mmreg(s)});")
            else:
                raise Unhandled("movq xmm")
            return True
        a, b = self.mmreg(d), self.mm64(insn, s)
        if not a:
            raise Unhandled("destino mmx")
        if m in MMX_LANES:
            bits, op = MMX_LANES[m]
            self.e(f"{a} = mmx_lanes({a}, {b}, {bits}, {op});")
        elif m in MMX_LOGIC:
            self.e(f"{a} = " + MMX_LOGIC[m].format(a=a, b=b) + ";")
        elif m in MMX_SHIFT:
            bits, left = MMX_SHIFT[m]
            self.e(f"{a} = mmx_shift({a}, {b}, {bits}, {left});")
        else:
            bits, high = MMX_UNPACK[m]
            self.e(f"{a} = mmx_unpack({a}, {b}, {bits}, {high});")
        return True

    def outside(self, target):
        """Un salto condicional a algo que no es instrucción de esta función (al medio de otra instrucción o a
        otra entrada; PC Fútbol 2001, utils.dll): no hay etiqueta, va por rt_jump, que para si no hay entrada."""
        return f"{{ SYNC(); rt_jump(0x{target:x}u); return; }}" if target not in self.f.insns else None

    def jcc(self, insn, cc):
        target = insn.operands[0].imm & 0xFFFFFFFF
        far = self.outside(target)
        if far:
            self.e(f"if ({translate.cond(cc)}) {far}")
            return True
        if self.backward(insn, target):
            self.e(f"if ({translate.cond(cc)}) {{ RT_POLL(); goto L_{target:x}; }}")
            return True
        return super().jcc(insn, cc)

    def i_jecxz(self, insn, ops):
        far = self.outside(ops[0].imm & 0xFFFFFFFF)
        if far:
            self.e(f"if (!ecx) {far}")
            return True
        return super().i_jecxz(insn, ops)

    def i_loop(self, insn, ops):
        target = ops[0].imm & 0xFFFFFFFF
        far = self.outside(target)
        if far:
            self.e(f"if (--ecx) {far}")
            return True
        if self.backward(insn, target):
            self.e(f"if (--ecx) {{ RT_POLL(); goto L_{target:x}; }}")
            return True
        return super().i_loop(insn, ops)

    # loope / loopne: ecx-- y salta si no llegó a 0 y ZF es 1 (loope) o 0 (loopne). ecx-- no toca las banderas.
    def loop_cc(self, insn, ops, cc):
        target = ops[0].imm & 0xFFFFFFFF
        cond = f"--ecx && ({translate.cond(cc)})"
        far = self.outside(target)
        if far:
            self.e(f"if ({cond}) {far}")
        elif self.backward(insn, target):
            self.e(f"if ({cond}) {{ RT_POLL(); goto L_{target:x}; }}")
        else:
            self.e(f"if ({cond}) goto L_{target:x};")
        return True

    def i_loope(self, insn, ops):
        return self.loop_cc(insn, ops, "e")

    def i_loopne(self, insn, ops):
        return self.loop_cc(insn, ops, "ne")

    # pushfd / popfd: como los de translate.py, pero guardan los bits AC (18) e ID (21) de EFLAGS. La detección de CPU del
    # exe invierte ID con popfd y lo relee con pushfd para saber si hay CPUID; sin esos bits veía un 486 y elegía las
    # opciones de una CPU lenta.
    def i_pushfd(self, insn, ops):
        self.e("esp -= 4; W32(esp, f_flags(F) | (cpu.df << 10) | cpu.eflags_ia);")
        return True

    def i_popfd(self, insn, ops):
        self.e("t0 = R32(esp); esp += 4; cpu.df = (t0 >> 10) & 1; cpu.eflags_ia = t0 & 0x240000u; "
               "SETF(K_RAW | SZ4, 0, 0, t0, 0);")
        return True

    def i_call(self, insn, ops):
        if ops[0].type == X.X86_OP_MEM and ops[0].size == 6:
            raise Unhandled("call far")  # m16:32: datos decodificados como código
        return super().i_call(insn, ops)

    def i_jmp(self, insn, ops):
        """Como el de translate.py, pero el default de un switch de tabla salta por rt_jump (con los registros sincronizados)
        en vez de frenar: el analizador a veces cuenta de menos las entradas (memcpy de MSVCRT), y build.py traduce
        cada destino de la tabla como entrada propia. Un jmp hacia atrás pasa por RT_POLL."""
        op = ops[0]
        if op.type == X.X86_OP_MEM and op.size == 6:
            raise Unhandled("jmp far")   # m16:32: en Win32 sólo aparece al decodificar datos como código
        if op.type == X.X86_OP_IMM and self.backward(insn, op.imm & 0xFFFFFFFF):
            self.e("RT_POLL();")
        if insn.address not in self.f.tables:
            return super().i_jmp(insn, ops)
        op = ops[0]
        self.setup_mem(insn)
        self.features.add("jump tables")
        self.e(f"t0 = {self.rd(insn, op)};")
        self.e("switch (t0) {")
        for t in sorted(set(self.f.tables[insn.address])):
            self.e(f"case {translate.h(t)}: goto L_{t:x};")
        self.e("default: SYNC(); rt_jump(t0); return;")
        self.e("}")
        return False

    def x87(self, insn, m):
        if m in ("fnstenv", "fldenv"):
            self.features.add("x87")
            self.setup_mem(insn)
            self.e(f"rt_{m}(M, ad);")
            return True
        return super().x87(insn, m)

    def x87_reg(self, insn, opc, modrm):
        if opc == 0xD9 and modrm in X87_REG_EXTRA:
            self.features.add("x87 transcendental (libm, may differ in the last bit)")
            self.e(X87_REG_EXTRA[modrm])
            return True
        return super().x87_reg(insn, opc, modrm)


def install():
    """Hace que todo lo que use translate.FunctionWriter use el nuestro."""
    translate.FunctionWriter = Writer
