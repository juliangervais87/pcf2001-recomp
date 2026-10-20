"""Sonda de depuración: inserta en el C generado (generated/build/gen, local) un fprintf con los registros justo antes
de cada instrucción pedida. compile.py recompila sólo esos archivos. Se deshace retraduciendo (build.py reescribe el
archivo) o con --quitar.

Uso: python tools/recomp/sonda.py 5b5370 5b5512 ...      (direcciones hex de instrucciones)
     python tools/recomp/sonda.py --quitar
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

GEN = Path(__file__).resolve().parents[2] / "generated" / "build" / "gen"
MARK = "/* SONDA */"


def main() -> int:
    args = sys.argv[1:]
    files = sorted(GEN.glob("fn_*.c"))
    if args == ["--quitar"]:
        for f in files:
            text = f.read_text(encoding="ascii")
            if MARK in text:
                f.write_text("".join(l for l in text.splitlines(True) if MARK not in l), encoding="ascii")
                print("sin sonda:", f.name)
        return 0
    want = {int(a, 16) for a in args}
    for f in files:
        text = f.read_text(encoding="ascii")
        if not any(f"/* {a:x}: " in text for a in want):
            continue
        out = []
        for line in text.splitlines(True):
            m = re.match(r"\s*/\* ([0-9a-f]+): ", line)
            if m and int(m.group(1), 16) in want:
                a = m.group(1)
                out.append(f'    {MARK} fprintf(stderr, "SONDA {a} eax=%#x ebx=%#x ecx=%#x edx=%#x esi=%#x edi=%#x ebp=%#x esp=%#x [esp]=%#x\\n", '
                           f"eax, ebx, ecx, edx, esi, edi, ebp, esp, R32(esp));\n")
            out.append(line)
        new = "".join(out)
        if "#include <stdio.h>" not in new:
            new = "#include <stdio.h>\n" + new
        f.write_text(new, encoding="ascii")
        print("sonda en", f.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
