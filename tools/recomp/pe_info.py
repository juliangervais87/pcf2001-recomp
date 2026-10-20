"""Tamaño en memoria, base y relocaciones de cada imagen (para planear el mapa de memoria).

Uso: python tools/recomp/pe_info.py generated/pcf2001/input/manager.exe generated/pcf2001/input/MFC42.DLL ...
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pe_image import Image  # noqa: E402

for p in sys.argv[1:]:
    im = Image(p)
    rva, size = im._dir(5)
    print(f"{Path(p).name:14s} base {im.base:#010x} tamaño {im.size:#09x} fin {im.base + im.size:#010x} "
          f"relocs {size} B {'(sin .reloc)' if not rva else ''}")
