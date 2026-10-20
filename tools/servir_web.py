"""Servidor local de la versión web, con soporte de Range (el http.server de Python no lo tiene y webcd.c lee el CD
por rangos). Sólo para probar en esta máquina: escucha en 127.0.0.1.

  /            web/index.html y generated/pcf2001/build/wasm-web/pcf2001.{js,wasm,data}
  /cd/...      generated/pcf2001/cd (el contenido de la ISO; otra carpeta con SERVIR_CD)
  /cd/cd_index.json   generated/pcf2001/cd_index.json

Uso: python tools/servir_web.py [puerto=8097]
"""
from __future__ import annotations

import http.server
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ROOTS = {"/cd/": Path(os.environ.get("SERVIR_CD") or REPO / "generated" / "pcf2001" / "cd")}
SITE = [REPO / "web", REPO / "generated" / "pcf2001" / "build" / "wasm-web"]
TYPES = {".wasm": "application/wasm", ".js": "text/javascript", ".html": "text/html; charset=utf-8", ".json": "application/json", ".data": "application/octet-stream"}


class Handler(http.server.BaseHTTPRequestHandler):
    def resolve(self):
        path = self.path.split("?")[0]
        if path == "/cd/cd_index.json":           # el índice va aparte del CD (make_cd_index.py)
            return REPO / "generated" / "pcf2001" / "cd_index.json"
        for prefix, root in ROOTS.items():
            if path.startswith(prefix):
                p = (root / path[len(prefix):]).resolve()
                return p if root.resolve() in p.parents or p == root.resolve() else None
        name = path.lstrip("/") or "index.html"
        for d in SITE:
            p = (d / name).resolve()
            if d.resolve() in p.parents and p.is_file():
                return p
        return None

    def do_GET(self):
        p = self.resolve()
        if not p or not p.is_file():
            self.send_error(404)
            return
        size = p.stat().st_size
        start, end, status = 0, size - 1, 200
        m = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        if m:
            start = int(m.group(1))
            end = min(int(m.group(2)) if m.group(2) else size - 1, size - 1)
            status = 206
        self.send_response(status)
        self.send_header("Content-Type", TYPES.get(p.suffix.lower(), "application/octet-stream"))
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        with p.open("rb") as f:
            f.seek(start)
            left = end - start + 1
            while left > 0:
                chunk = f.read(min(left, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)

    def log_message(self, fmt, *args):
        if os.environ.get("SERVIR_LOG"):
            super().log_message(fmt, *args)


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8097
    print(f"http://127.0.0.1:{port}/")
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
