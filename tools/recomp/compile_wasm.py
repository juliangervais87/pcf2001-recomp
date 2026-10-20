"""Compila el C generado + el runtime a WebAssembly con Emscripten (third_party/emsdk, o la variable EMSDK).

Objetivos:
  --target node     Node.js con los archivos del host (NODERAWFS): prueba sin pantalla (RECOMP_HEADLESS=1).
  --target web      página: SDL2 en un canvas; el CD se lee por rangos (webcd.c) y la instalación va empaquetada.
Sin pthreads (las fibras de threads.c van sobre Asyncify): el servidor no necesita cabeceras COOP/COEP.

Uso: python tools/recomp/compile_wasm.py --build generated/pcf2001/build --target web [--jobs 1] [--opt -O1]
Salida: <build>/wasm-<target>/pcf2001.{js,wasm,data}
"""
from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
RT = REPO / "src" / "rt"
EMSDK = Path(os.environ.get("EMSDK") or REPO / "third_party" / "emsdk")
sys.path.insert(0, str(HERE))
from compile import write_stubs  # noqa: E402


def emcc() -> list[str]:
    py = EMSDK / "python"
    tool = EMSDK / "upstream" / "emscripten" / "emcc.py"
    pys = sorted(py.glob("*/python.exe")) if py.exists() else []
    return [str(pys[-1]) if pys else sys.executable, str(tool)]


def env() -> dict:
    e = dict(os.environ)
    e["EM_CONFIG"] = str(EMSDK / ".emscripten")
    e["EMSDK"] = str(EMSDK)
    nodes = sorted((EMSDK / "node").glob("*/bin")) if (EMSDK / "node").exists() else []
    if nodes:
        e["EMSDK_NODE"] = str(sorted(nodes[-1].glob("node*"))[0])
    return e


def runtime_sources() -> list[Path]:
    core = [RT / "core" / "rt.c", RT / "core" / "heap.c", RT / "core" / "host" / "posix.c"]
    return core + sorted(RT.glob("*.c")) + sorted((RT / "win").glob("*.c"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", required=True)
    ap.add_argument("--target", choices=("node", "web"), default="node")
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--opt", default="-O1")
    ap.add_argument("--link-opt", default="-O2", help="el enlace corre wasm-opt: -O2 fusiona locales (V8 limita a 50.000 por función)")
    args = ap.parse_args()
    build = Path(args.build)
    gen = build / "gen"
    out = build / f"wasm-{args.target}"
    obj = out / "obj"
    obj.mkdir(parents=True, exist_ok=True)
    write_stubs(gen)
    cc, e = emcc(), env()
    flags = [args.opt, "-std=gnu11", "-fno-strict-aliasing", "-fwrapv", "-w", "-D_USE_MATH_DEFINES", "-D_DEFAULT_SOURCE",
             "-sUSE_SDL=2", "-I", str(RT / "core"), "-I", str(RT), "-I", str(RT / "win"), "-I", str(gen)]
    sources = sorted(gen.glob("*.c")) + runtime_sources()
    if args.target == "web":
        flags.append("-DRECOMP_WEB")
        sources.append(RT / "core" / "host" / "webcd.c")
    rt_headers = max(p.stat().st_mtime for p in RT.rglob("*.h"))

    def one(src: Path):
        tag = "rt_" if RT in src.parents else ""
        o = obj / (tag + src.stem + ".o")
        dep = rt_headers if tag else 0.0
        if o.exists() and o.stat().st_mtime >= src.stat().st_mtime and o.stat().st_mtime >= dep:
            return o, None
        r = subprocess.run(cc + ["-c", str(src), "-o", str(o)] + flags, capture_output=True, text=True, env=e)
        return o, (r.stderr[-3000:] if r.returncode else None)

    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        results = list(ex.map(one, sources))
    errors = [(o, err) for o, err in results if err]
    for o, err in errors[:5]:
        print(f"== error en {o.name} ==\n{err}")
    if errors:
        print(f"{len(errors)} archivos con error")
        return 1
    t_compile = time.time() - t0
    link = ["-sUSE_SDL=2", "-sASYNCIFY=1", "-sASYNCIFY_STACK_SIZE=262144", "-sALLOW_MEMORY_GROWTH=1",
            "-sINITIAL_MEMORY=640MB", "-sMAXIMUM_MEMORY=2GB", "-sSTACK_SIZE=4MB", "-sEXIT_RUNTIME=1", args.link_opt]
    if args.target == "node":
        link += ["-sNODERAWFS=1", "-sENVIRONMENT=node"]
    else:
        link += ["-sENVIRONMENT=web", "-sFORCE_FILESYSTEM=1", "-sEXPORTED_RUNTIME_METHODS=FS,callMain,ENV",
                 "-sINVOKE_RUN=0", "-lidbfs.js",
                 "--preload-file", f"{(build / 'images').as_posix()}@/images",
                 # la instalación (C:\PCF2001 recién instalado): la página la copia a /c la primera vez
                 "--preload-file", f"{(build.parent / 'c_base').as_posix()}@/c_base"]
    target = out / "pcf2001.js"
    t1 = time.time()
    rsp = out / "link.rsp"         # ~1.300 objetos no entran en la línea de comandos de Windows
    rsp.write_text("\n".join('"' + str(o).replace("\\", "/") + '"' for o, _ in results) + "\n", encoding="utf-8")
    r = subprocess.run(cc + [f"@{rsp}", "-o", str(target)] + link, capture_output=True, text=True, env=e)
    if r.returncode:
        print("== enlace ==\n" + r.stderr[-6000:])
        return 1
    wasm = out / "pcf2001.wasm"
    rep = {"target": args.target, "sources": len(sources), "seconds_compile": round(t_compile, 1),
           "seconds_link": round(time.time() - t1, 1), "wasm_bytes": wasm.stat().st_size, "opt": args.opt}
    (out / "compile.json").write_text(json.dumps(rep, indent=1), encoding="utf-8")
    print(json.dumps(rep, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
