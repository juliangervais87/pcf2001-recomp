"""Etiqueta y fecha del volumen de una ISO 9660 (descriptor primario en el sector 16), para el GetVolumeInformationA del
CD virtual. Escribe la etiqueta en <salida> (por defecto generated/cd.label).

Uso: python tools/iso_label.py RUTA.iso [salida]
"""
from __future__ import annotations

import sys
from pathlib import Path

iso = Path(sys.argv[1])
out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[1] / "generated" / "cd.label"
with iso.open("rb") as f:
    f.seek(16 * 2048)
    pvd = f.read(2048)
if pvd[1:6] != b"CD001":
    sys.exit("no es ISO 9660")
label = pvd[40:72].decode("ascii", "replace").rstrip()
created = pvd[813:830].decode("ascii", "replace")
print(f"etiqueta: {label!r}  creado: {created}")
out.write_text(label, encoding="ascii")
