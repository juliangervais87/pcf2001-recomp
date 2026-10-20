"""Compila el C generado por build.py + el runtime (src/rt) y enlaza el programa con clang.

Uso: python tools/recomp/compile.py --build generated/build [--jobs 4] [--opt -O1]
Incremental: un .c sólo se recompila si cambió después de su .o. Salida: <build>/<programa>(.exe), con el nombre del perfil del juego.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
RT = REPO / "src" / "rt"
SDL_WIN = REPO / "third_party" / "sdl2" / "SDL2-2.32.10"
CLANG_CANDIDATES = ("clang", r"C:\Program Files\LLVM\bin\clang.exe")


def find_clang() -> str:
    for c in CLANG_CANDIDATES:
        p = shutil.which(c) or (c if Path(c).exists() else None)
        if p:
            return p
    sys.exit("no encuentro clang")


def runtime_sources() -> list[Path]:
    core = [RT / "core" / "rt.c", RT / "core" / "heap.c",
            RT / "core" / "host" / ("win32.c" if os.name == "nt" else "posix.c")]
    own = sorted(p for p in RT.glob("*.c"))
    win = sorted((RT / "win").glob("*.c")) if (RT / "win").exists() else []
    return core + own + win


def sdl_cflags() -> list[str]:
    if os.name == "nt":
        return ["-I", str(SDL_WIN / "include")]
    out = subprocess.run(["sdl2-config", "--cflags"], capture_output=True, text=True)
    if out.returncode:
        sys.exit("falta SDL2 (sdl2-config): apt install libsdl2-dev")
    return out.stdout.split()


def sdl_libs() -> list[str]:
    if os.name == "nt":
        return [str(SDL_WIN / "lib" / "x64" / "SDL2.lib")]
    return subprocess.run(["sdl2-config", "--libs"], capture_output=True, text=True).stdout.split()


def write_stubs(gen: Path) -> None:
    """gen/stubs.c: un imp_<nombre> que para con rt_missing_import por cada import de Windows que el runtime de ahora
    no implementa (RT_IMPORT en src/rt). Sólo se reescribe si cambia, para no recompilar de gusto."""
    have = set()
    for p in RT.rglob("*.c"):
        have |= set(re.findall(r"^(?:RT_IMPORT|NO_W)\((\w+)[,)]", p.read_text(encoding="utf-8", errors="replace"), re.M))
    wanted = (gen / "windows_imports.txt").read_text(encoding="ascii").split()
    missing = [n for n in wanted if n not in have]
    text = '#include "rt.h"\n#include "rt_host.h"\n\n' + "".join(
        f'void imp_{n}(void) {{ rt_missing_import("{n}"); }}\n' for n in missing)
    out = gen / "stubs.c"
    if not out.exists() or out.read_text(encoding="ascii") != text:
        out.write_text(text, encoding="ascii")
    print(f"imports de Windows: {len(wanted)}, en el runtime {len(wanted) - len(missing)}, stubs {len(missing)}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", required=True)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--opt", default="-O1")
    args = ap.parse_args()
    clang = find_clang()
    build = Path(args.build)
    gen, obj = build / "gen", build / "obj"
    obj.mkdir(parents=True, exist_ok=True)
    flags = [args.opt, "-std=c11", "-fno-strict-aliasing", "-fwrapv", "-w", "-D_USE_MATH_DEFINES",
             "-D_CRT_SECURE_NO_WARNINGS", "-D_DEFAULT_SOURCE",   # M_PI/M_LN2: Windows y glibc los esconden en C estricto
"-I", str(RT / "core"), "-I", str(RT), "-I", str(RT / "win"), "-I", str(gen)] + sdl_cflags()
    write_stubs(gen)
    rt_headers = max(p.stat().st_mtime for p in RT.rglob("*.h"))
    sources = sorted(gen.glob("*.c")) + runtime_sources()

    def one(src: Path):
        tag = "rt_" if RT in src.parents else ""
        o = obj / (tag + src.stem + ".o")
        dep = rt_headers if tag else 0.0   # un .c generado que no cambió referencia lo mismo que antes
        if o.exists() and o.stat().st_mtime >= src.stat().st_mtime and o.stat().st_mtime >= dep:
            return o, None
        r = subprocess.run([clang, "-c", str(src), "-o", str(o)] + flags, capture_output=True, text=True)
        return o, (r.stderr[-3000:] if r.returncode else None)

    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        results = list(ex.map(one, sources))
    errors = [(o, e) for o, e in results if e]
    t_compile = time.time() - t0
    for o, e in errors[:5]:
        print(f"== error en {o.name} ==\n{e}")
    if errors:
        print(f"{len(errors)} archivos con error")
        return 1
    bj = build / "build.json"                    # el nombre del programa sale del perfil del juego
    prog = json.loads(bj.read_text(encoding="utf-8")).get("programa", "pcf2001_recomp") if bj.exists() else "pcf2001_recomp"
    exe = build / (prog + ".exe" if os.name == "nt" else prog)
    libs = sdl_libs() + (["-lshell32", "-lole32", "-luser32", "-ladvapi32"] if os.name == "nt" else ["-lm", "-lpthread"])
    t1 = time.time()
    # Los objetos van en un archivo de respuesta: con 880 (PC Fútbol 2001) la línea pasa el límite de Windows.
    rsp = build / "link.rsp"
    rsp.write_text("\n".join('"' + str(o).replace("\\", "/") + '"' for o, _ in results) + "\n", encoding="utf-8")
    r = subprocess.run([clang, "-o", str(exe), f"@{rsp}"] + libs, capture_output=True, text=True)
    if r.returncode:
        print("== enlace ==\n" + r.stderr[-6000:])
        return 1
    if os.name == "nt":
        shutil.copyfile(SDL_WIN / "lib" / "x64" / "SDL2.dll", build / "SDL2.dll")
    rep = {"sources": len(sources), "seconds_compile": round(t_compile, 1), "seconds_link": round(time.time() - t1, 1),
           "jobs": args.jobs, "opt": args.opt, "program": str(exe), "bytes": exe.stat().st_size}
    (build / "compile.json").write_text(json.dumps(rep, indent=1), encoding="utf-8")
    print(json.dumps(rep, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
