"""Lector de PE (Image) para cualquier PE x86.

Toma como código la primera sección con IMAGE_SCN_CNT_CODE (no la primera a
secas) y lee la tabla de exports, que las DLL necesitan para enlazar los `imp_` por nombre u ordinal.
"""
from __future__ import annotations

import hashlib
import re
import struct

# El stub del k-ésimo import vive en IMPORT_STUBS + 4*k.
IMPORT_STUBS = 0x62000000
SCN_CNT_CODE = 0x20
SCN_MEM_EXECUTE = 0x20000000


def rebase_file(src: str, dst: str, new_base: int) -> int:
    """Escribe en dst una copia de la DLL src reubicada a new_base: aplica las relocaciones HIGHLOW de .reloc y
    cambia ImageBase. Devuelve cuántas relocaciones aplicó. (El runtime carga la copia tal cual: así el C traducido,
    los punteros de .data y las vtables quedan todos en la misma base.)"""
    data = bytearray(open(src, "rb").read())
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    opt = pe + 24
    old_base = struct.unpack_from("<I", data, opt + 28)[0]
    n_sec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    secs = [struct.unpack_from("<IIII", data, opt + opt_size + i * 40 + 8) for i in range(n_sec)]   # vsize, va, rsize, rptr

    def off(rva):
        for vsize, va, rsize, rptr in secs:
            if va <= rva < va + max(vsize, rsize):
                return rptr + rva - va if rva - va < rsize else None
        return None

    rva, size = struct.unpack_from("<II", data, opt + 96 + 8 * 5)
    if not rva:
        raise SystemExit(f"{src}: sin .reloc, no se puede reubicar")
    delta = (new_base - old_base) & 0xFFFFFFFF
    p, end, count = off(rva), off(rva) + size, 0
    while p < end:
        page, block = struct.unpack_from("<II", data, p)
        if block < 8:
            break
        for i in range((block - 8) // 2):
            e = struct.unpack_from("<H", data, p + 8 + 2 * i)[0]
            kind, ofs = e >> 12, e & 0xFFF
            if kind == 0:
                continue
            if kind != 3:
                raise SystemExit(f"{src}: relocación de tipo {kind} no soportada")
            fo = off(page + ofs)
            if fo is None:
                continue
            v = struct.unpack_from("<I", data, fo)[0]
            struct.pack_into("<I", data, fo, (v + delta) & 0xFFFFFFFF)
            count += 1
        p += block
    struct.pack_into("<I", data, opt + 28, new_base)
    open(dst, "wb").write(bytes(data))
    return count


def c_name(name: str) -> str:
    """Un nombre de import como identificador de C. Los decorados de C++ de MSVCIRT (??0ifstream@@QAE@XZ)
    no lo son: se reemplaza lo inválido por _ y se agrega un hash corto del original para no colisionar."""
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        return name
    return "cxx_%s_%s" % (re.sub(r"[^A-Za-z0-9_]", "_", name).strip("_")[:60],
                          hashlib.sha1(name.encode()).hexdigest()[:8])


class Image:
    def __init__(self, path: str, stub_base: int = IMPORT_STUBS, translated: tuple[str, ...] = ()):
        """stub_base: dónde empiezan los stubs de import de ESTA imagen (con varias imágenes, uno distinto
        por imagen). translated: DLL (en mayúsculas) que también se traducen; sus imports llevan el prefijo
        de la DLL (MSVCRT__free) para no confundirse con un reemplazo de Windows del runtime."""
        self.stub_base = stub_base
        self.translated = {t.upper() for t in translated}
        data = open(path, "rb").read()
        self.path = path
        self.data = data
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise SystemExit(f"{path}: no es un PE")
        machine, count = struct.unpack_from("<HH", data, pe + 4)
        if machine != 0x14C:
            raise SystemExit(f"{path}: no es x86 de 32 bits (machine {machine:#x})")
        optional = struct.unpack_from("<H", data, pe + 20)[0]
        self.characteristics = struct.unpack_from("<H", data, pe + 22)[0]
        self.is_dll = bool(self.characteristics & 0x2000)
        opt = pe + 24
        self.linker = "%d.%d" % (data[opt + 2], data[opt + 3])
        self.entry = struct.unpack_from("<I", data, opt + 16)[0]
        self.base = struct.unpack_from("<I", data, opt + 28)[0]
        self.size = struct.unpack_from("<I", data, opt + 56)[0]
        self.entry += self.base
        self._dirs = opt + 96
        self.sections = []
        for i in range(count):
            name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", data, opt + optional + i * 40)
            flags = struct.unpack_from("<I", data, opt + optional + i * 40 + 36)[0]
            self.sections.append((name.rstrip(b"\0").decode(errors="replace"), self.base + va, vsize, rptr, rsize, flags))
        code = [s for s in self.sections if s[5] & (SCN_CNT_CODE | SCN_MEM_EXECUTE)]
        if not code:
            raise SystemExit(f"{path}: sin sección de código")
        self.code_sections = code
        text = code[0]
        self.text = (text[1], text[1] + max(text[2], text[4]))
        self.raw_import_names = {}           # nombre C -> nombre original (decorado de C++ incluido)
        self.imports = self._imports()       # dirección IAT -> (dll, nombre C, dirección del stub)
        self.exports = self._exports()       # dirección -> [nombres u "ord<N>"]

    def reloc_slots(self):
        """Direcciones de los dwords que la tabla .reloc corrige (HIGHLOW). En una DLL, un slot en una sección de
        DATOS cuyo valor cae en el código es un puntero a función con certeza (vtables, _pRawDllMain, tablas de
        callbacks): mejor señal que la heurística de alineación."""
        rva, size = self._dir(5)
        slots = []
        if not rva:
            return slots
        p, end = self.base + rva, self.base + rva + size
        while p < end:
            page, block = self.u32(p), self.u32(p + 4)
            if block < 8:
                break
            for i in range((block - 8) // 2):
                e = self.u16(p + 8 + 2 * i)
                if e >> 12 == 3:
                    slots.append(self.base + page + (e & 0xFFF))
            p += block
        return slots

    def data_code_pointers(self):
        """Valores de slots relocados fuera del código que apuntan al código."""
        out = set()
        for s in self.reloc_slots():
            if self.is_code(s):
                continue
            v = self.u32(s)
            if self.is_code(v):
                out.add(v)
        return out

    def import_thunks(self):
        """Los saltos `jmp dword ptr [IAT]` (FF 25) del código: el enlazador los pone por cada import que se toma por
        dirección, y las vtables del exe los apuntan (MFC los llama por puntero). En un exe sin .reloc ni listas de
        IDA/Ghidra (PC Fútbol 2001) no aparecen de otro modo. Exacto: la dirección tiene que ser una entrada de la IAT."""
        out = set()
        for _, va, vsize, rptr, rsize, _ in self.code_sections:
            raw = self.data[rptr:rptr + rsize]
            i = raw.find(b"\xff\x25")
            while i >= 0:
                if i + 6 <= len(raw) and struct.unpack_from("<I", raw, i + 2)[0] in self.imports:
                    out.add(va + i)
                i = raw.find(b"\xff\x25", i + 1)
        return out

    def _dir(self, i):
        return struct.unpack_from("<II", self.data, self._dirs + 8 * i)

    def _offset(self, addr):
        for _, va, vsize, rptr, rsize, _ in self.sections:
            if va <= addr < va + max(vsize, rsize):
                if addr - va >= rsize:
                    return None
                return rptr + addr - va
        return None

    def read(self, addr, size):
        off = self._offset(addr)
        if off is None:
            return bytes(size)
        return self.data[off:off + size]

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def u16(self, addr):
        return struct.unpack("<H", self.read(addr, 2))[0]

    def u8(self, addr):
        return self.read(addr, 1)[0]

    def cstr(self, addr, limit=256):
        return self.read(addr, limit).split(b"\0")[0].decode("latin-1")

    def is_code(self, addr):
        return self.text[0] <= addr < self.text[1]

    def _imports(self):
        rva, _ = self._dir(1)
        result = {}
        if not rva:
            return result
        d = self.base + rva
        k = 0
        while True:
            ilt, _, _, name, iat = struct.unpack("<5I", self.read(d, 20))
            d += 20
            if name == 0:
                break
            dll = self.cstr(self.base + name, 64)
            i = 0
            while True:
                entry = self.u32(self.base + (ilt or iat) + 4 * i)
                if entry == 0:
                    break
                stem = dll.split(".")[0].upper()
                if entry & 0x80000000:
                    fn = "%s_ord%d" % (stem, entry & 0xFFFF)   # p. ej. MFC42_ord1176
                    raw = "ord%d" % (entry & 0xFFFF)
                else:
                    raw = self.cstr(self.base + entry + 2, 256)
                    fn = c_name(raw)
                    if dll.upper() in self.translated:
                        fn = "%s__%s" % (stem, fn)
                self.raw_import_names.setdefault(fn, raw)
                result[self.base + iat + 4 * i] = (dll, fn, self.stub_base + 4 * k)
                i += 1
                k += 1
        return result

    def _exports(self):
        rva, size = self._dir(0)
        result: dict[int, list[str]] = {}
        if not rva:
            return result
        d = self.base + rva
        ordinal_base, n_funcs, n_names, a_funcs, a_names, a_ords = struct.unpack("<6I", self.read(d + 16, 24))
        names = {}
        for i in range(n_names):
            ordinal = self.u16(self.base + a_ords + 2 * i)
            names[ordinal] = self.cstr(self.base + self.u32(self.base + a_names + 4 * i))
        for i in range(n_funcs):
            frva = self.u32(self.base + a_funcs + 4 * i)
            if not frva or rva <= frva < rva + size:     # vacío o forwarder
                continue
            label = names.get(i) or "ord%d" % (ordinal_base + i)
            result.setdefault(self.base + frva, []).append(label)
        return result
