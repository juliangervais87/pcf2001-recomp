"""Desensambla una zona de una imagen (en su base) para investigar un freno del runtime.

Uso: python tools/recomp/desensamblar.py <imagen> <dirección hex> [antes=48] [después=64]
"""
from __future__ import annotations

import sys
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe_image import Image  # noqa: E402

im = Image(sys.argv[1])
addr = int(sys.argv[2], 16)
before = int(sys.argv[3]) if len(sys.argv) > 3 else 48
after = int(sys.argv[4]) if len(sys.argv) > 4 else 64
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
start = addr - before
for insn in md.disasm(im.read(start, before + after), start):
    mark = ">>" if insn.address == addr else "  "
    exp = ",".join(im.exports.get(insn.address, []))
    print(f"{mark} {insn.address:08x}: {insn.mnemonic:8s} {insn.op_str}" + (f"   ; export {exp}" if exp else ""))
