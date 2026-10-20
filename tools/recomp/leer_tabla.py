"""Lee una tabla de saltos (dwords) de una imagen, para ver por qué el analizador no la tomó.

Uso: python tools/recomp/leer_tabla.py <imagen> <dirección hex> [n=8]
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe_image import Image  # noqa: E402

im = Image(sys.argv[1])
a = int(sys.argv[2], 16)
for k in range(int(sys.argv[3]) if len(sys.argv) > 3 else 8):
    v = im.u32(a + 4 * k)
    print(f"{a + 4 * k:08x}: {v:08x} {'código' if im.is_code(v) else ''}")
