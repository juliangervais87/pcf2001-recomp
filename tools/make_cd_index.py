"""Índice del CD para la versión web (webcd.c): {RUTA EN MAYÚSCULAS: tamaño | "dir"} y {RUTA EN MAYÚSCULAS: ruta real}.

Uso: python tools/make_cd_index.py generated/pcf2001/cd generated/pcf2001/cd_index.json
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
sizes, names = {}, {}
for p in sorted(root.rglob("*")):
    rel = p.relative_to(root).as_posix()
    sizes[rel.upper()] = "dir" if p.is_dir() else p.stat().st_size
    names[rel.upper()] = rel
Path(sys.argv[2]).write_text(json.dumps({"sizes": sizes, "names": names}), encoding="utf-8")
print(f"{len(sizes)} entradas, {sum(v for v in sizes.values() if v != 'dir') / 1e6:.1f} MB")
