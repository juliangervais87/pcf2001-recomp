"""Fase 1: traduce el exe de un juego y sus DLL (perfil config/juegos/<id>.json) a un solo programa C enlazable.

Cada imagen se traduce en su dirección base (no se reubica nada: las bases no se pisan), con el analizador
(analysis.py) y el traductor (translate.py + translator.py). Los imports entre imágenes traducidas se enlazan con un
"pegamento" imp_<DLL>__<nombre> -> f_<dirección del export>; los imports de Windows quedan para el runtime
(src/rt) y, si el runtime no los tiene, en un stub que para con rt_missing_import.

Uso:
  python tools/recomp/build.py --input generated/input --functions generated/funciones/ida.txt \
      --functions generated/funciones/ghidra.txt --out generated/build [--juego config/juegos/pcf2001.json]
Salida en <out>/gen/: fn_*.c, functions.h, link.c (tablas), images.c (para el cargador), build.json (medición).
"""
from __future__ import annotations

import argparse
import collections
import json
import re
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import analysis  # noqa: E402
import recomp  # noqa: E402
import translator  # noqa: E402
from capstone import x86_const as X  # noqa: E402
from pe_image import IMPORT_STUBS, Image, rebase_file  # noqa: E402

translator.install()
import translate  # noqa: E402

RUNTIME = REPO / "src" / "rt"
DEFAULT_GAME = REPO / "config" / "juegos" / "pcf2001.json"
STUB_SPACING = 0x2000
# Funciones de MSVCRT que no vuelven (para que el analizador no siga decodificando después de la llamada).
NORETURN_EXPORTS = {"exit", "_exit", "abort", "_cexit", "longjmp", "_CxxThrowException", "_amsg_exit"}
SETJMP_EXPORTS = {"_setjmp3", "_setjmp"}


def load_game(path: Path) -> dict:
    """El perfil del juego (config/juegos/<id>.json): imágenes en orden de inicialización (DllMain) con el exe
    último, dónde se reubica cada DLL (mapa compacto: todo lo que el juego toca debajo de 0x11000000, para que
    el mismo modelo plano entre en wasm32; docs/mapa-de-memoria.md), el heap y lo que deja el instalador."""
    game = json.loads(path.read_text(encoding="utf-8"))
    game["images"] = [(n, "functions" if n.lower().endswith(".exe") else None) for n in game["imagenes"]]
    game["rebase"] = {n: int(a, 16) for n, a in game.get("rebase", {}).items()}
    game["translated"] = tuple(n.upper() for n in game["imagenes"] if n.upper().endswith(".DLL"))
    return game


def c_str(s: str) -> str:
    """Literal de C en Latin-1 (como lo ve el juego); lo no imprimible va en octal de 3 cifras."""
    out = []
    for b in s.encode("latin-1", errors="replace"):
        c = chr(b)
        out.append(c if 0x20 <= b < 0x7F and c not in '"\\?' else "\\%03o" % b)
    return '"' + "".join(out) + '"'


def write_if_changed(path: Path, text: str) -> None:
    """Escribe sólo si cambia: compile.py recompila por fecha, así una retraducción recompila sólo lo afectado."""
    data = text.encode("ascii", errors="replace")
    if path.exists() and path.read_bytes() == data:
        return
    path.write_bytes(data)


def runtime_imports():
    have = set()
    for p in RUNTIME.rglob("*.c"):
        have |= set(re.findall(r"^(?:RT_IMPORT|NO_W)\((\w+)[,)]", p.read_text(encoding="utf-8", errors="replace"), re.M))
    return have


def analyze(image, known, noreturn_addrs, setjmp_addrs, setjmp_imports):
    """Como recomp.main --all, más: noreturn de MSVCRT, entradas de reanudación tras _setjmp3 y descarte de
    inicios falsos (entradas sacadas de datos que caen en el medio de una instrucción de otra función)."""
    known_set = set(known)
    analysis.NORETURN = dict(noreturn_addrs)
    an = analysis.Analyzer(image, known_set)
    from_data = set(recomp.data_pointers(image, known_set))
    # DLL: los punteros a código de las secciones de datos según .reloc son entradas seguras (no adivinadas).
    sure = (image.data_code_pointers() | image.import_thunks()) - known_set
    known_set |= sure
    known = sorted(known_set)
    an.known = known_set
    entries = list(dict.fromkeys(list(known) + sorted(from_data)))
    # Memoria acotada (el 2001 llegaba a ~10 GB): de cada función quedan sólo números (cuántas instrucciones; los
    # bytes de las que son entradas conocidas, en dos mapas de bits), y los cachés del analizador (instrucciones y
    # funciones decodificadas) se vacían cada FLUSH funciones. Lo que el análisis necesita recordar sí queda (qué
    # funciones no vuelven, dónde hay tablas). main() vuelve a decodificar cada entrada al escribirla.
    funcs, resume, called, jump_targets = {}, set(), set(), set()
    t0_, t1_ = image.text
    starts, inside = bytearray(t1_ - t0_), bytearray(t1_ - t0_)
    guessed = set(from_data)
    work = list(entries)
    done = 0
    while work:
        e = work.pop()
        if e in funcs or not image.is_code(e):
            continue
        f = an.function(e)
        funcs[e] = len(f.insns)
        if e in known_set:
            for a, insn in f.insns.items():
                if t0_ <= a < t1_:
                    starts[a - t0_] = 1
                    for b in range(a + 1, min(a + insn.size, t1_)):
                        inside[b - t0_] = 1
        done += 1
        if done % FLUSH == 0:
            flush_analyzer(an)
        for a, insn in f.insns.items():
            if insn.mnemonic in ("push", "mov"):
                for op in insn.operands:
                    v = op.imm & 0xFFFFFFFF if op.type == X.X86_OP_IMM else None
                    if v is not None and image.is_code(v) and v not in funcs and recomp.function_start(image, v, known_set):
                        guessed.add(v)
                        work.append(v)
                # mov [memoria], dirección de código: una tabla de saltos llenada en ejecución (el escalador del partido
                # llena 0x64e030.. con 120 etiquetas, mov [tabla+k], offset loc_59F3F3): cada una es destino seguro.
                ops = insn.operands
                if insn.mnemonic == "mov" and len(ops) == 2 and ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_IMM:
                    v = ops[1].imm & 0xFFFFFFFF
                    if image.is_code(v) and v not in funcs and v != e:
                        jump_targets.add(v)
                        work.append(v)
            if insn.mnemonic == "call":
                op = insn.operands[0]
                tgt = op.imm & 0xFFFFFFFF if op.type == X.X86_OP_IMM else None
                imp = an.import_of(op)
                if (tgt is not None and tgt in setjmp_addrs) or (imp in setjmp_imports):
                    resume.add(a + insn.size)
                    work.append(a + insn.size)
        # La misma tabla llenada con lea reg, [dirección de código] + stosd / mov [memoria], reg (el escalador del
        # partido: lea eax, [0x59f3f3]; stosd): cada dirección guardada es destino seguro.
        order = sorted(f.insns)
        for pos, a in enumerate(order[:-1]):
            insn, nxt = f.insns[a], f.insns[order[pos + 1]]
            ops = insn.operands
            if insn.mnemonic != "lea" or len(ops) != 2 or ops[1].mem.base or ops[1].mem.index:
                continue
            v = ops[1].mem.disp & 0xFFFFFFFF
            stores = nxt.mnemonic == "stosd" or (nxt.mnemonic == "mov" and len(nxt.operands) == 2
                                                  and nxt.operands[0].type == X.X86_OP_MEM
                                                  and nxt.operands[1].type == X.X86_OP_REG
                                                  and nxt.operands[1].reg == ops[0].reg)
            if stores and image.is_code(v) and v not in funcs and v != e:
                jump_targets.add(v)
                work.append(v)
        # Manejadores de excepción del prólogo SEH de MSVC (push -1; push manejador; mov eax, fs:[0]): el valor empujado
        # justo antes de leer fs:[0] lo llama el despachador de excepciones (MFC del 2001, 0xd46e46: mov eax, funcinfo;
        # jmp __CxxFrameHandler). Seguro, no adivinado: el patrón es el del compilador.
        # MFC (compilada por tamaño) usa la otra forma: mov eax, manejador; call __EH_prolog.
        for pos, a in enumerate(order[:-1]):
            insn, nxt = f.insns[a], f.insns[order[pos + 1]]
            ops = insn.operands
            seh = insn.mnemonic == "push" and ops[0].type == X.X86_OP_IMM and nxt.mnemonic == "mov" \
                and len(nxt.operands) == 2 and nxt.operands[1].type == X.X86_OP_MEM \
                and nxt.operands[1].mem.segment == X.X86_REG_FS and nxt.operands[1].mem.disp == 0
            eh_prolog = insn.mnemonic == "mov" and len(ops) == 2 and ops[0].type == X.X86_OP_REG \
                and ops[0].reg == X.X86_REG_EAX and ops[1].type == X.X86_OP_IMM and nxt.mnemonic == "call"
            # Tercera forma (manager.exe del 2001, 0x49b500): mov eax, fs:[0]; push -1; push manejador; push eax.
            seh_first = False
            if pos >= 2 and insn.mnemonic == "push" and ops[0].type == X.X86_OP_IMM:
                p1, p2 = f.insns[order[pos - 1]], f.insns[order[pos - 2]]
                seh_first = p1.mnemonic == "push" and p1.operands[0].type == X.X86_OP_IMM \
                    and (p1.operands[0].imm & 0xFFFFFFFF) == 0xFFFFFFFF and p2.mnemonic == "mov" \
                    and len(p2.operands) == 2 and p2.operands[1].type == X.X86_OP_MEM \
                    and p2.operands[1].mem.segment == X.X86_REG_FS and p2.operands[1].mem.disp == 0
            if not (seh or eh_prolog or seh_first):
                continue
            v = ops[-1].imm & 0xFFFFFFFF
            if image.is_code(v) and v not in funcs and v != e:
                jump_targets.add(v)
                work.append(v)
        called |= f.calls | f.tails
        work.extend(t for t in (f.calls | f.tails) if t not in funcs)
        # Saltos por tabla que el analizador no acotó (memcpy de MSVCRT: jmp [ecx*4 + tabla] sin cmp antes): se leen
        # las entradas que apuntan cerca de la función y cada destino se traduce como entrada propia; rt_jump la
        # encuentra y su ret vuelve como habría vuelto la función entera.
        order = sorted(f.insns)
        for pos, a in enumerate(order):
            insn = f.insns[a]
            if insn.mnemonic != "jmp":
                continue
            op = insn.operands[0]
            if op.type == X.X86_OP_MEM:
                m = op.mem                               # jmp [idx*4 + tabla]
                if an.import_of(op):
                    continue
            elif op.type == X.X86_OP_REG and pos:
                prev = f.insns[order[pos - 1]]           # mov reg, [idx*4 + tabla] ; jmp reg (escalador del partido)
                if prev.mnemonic != "mov" or len(prev.operands) != 2 or prev.operands[0].type != X.X86_OP_REG \
                        or prev.operands[1].type != X.X86_OP_MEM or prev.operands[0].reg != op.reg:
                    continue
                m = prev.operands[1].mem
            else:
                continue
            table = m.disp & 0xFFFFFFFF
            if m.base or not m.index or m.scale != 4:
                continue
            # el índice 0 puede no usarse (memcpy: jmp [eax*4 + tabla] con eax 1..3) y puede ser negativo
            # (memcpy: neg ecx; jmp [ecx*4 + tabla]): se lee hacia adelante y hacia atrás
            for ks in (range(256), range(-1, -65, -1)):
                found = misses = 0
                for n, k in enumerate(ks):
                    t = image.u32(table + 4 * k)
                    # cerca de la función, o ya instrucción suya, o la tabla está pegada al salto (el memcpy del MSVCRT de
                    # PCF 2001 tiene sus bloques a 60 KB del inicio y la tabla justo después de cada jmp)
                    near = abs(t - e) <= 0x4000 or t in f.insns or abs(table - a) < 0x400
                    if not image.is_code(t) or not near:
                        misses += 1
                        if (found and misses >= 2) or (not found and n >= 3):
                            break
                        continue
                    found, misses = found + 1, 0
                    if t not in funcs:
                        jump_targets.add(t)
                        work.append(t)
    flush_analyzer(an)
    # Descartar inicios falsos.
    false = {e for e in funcs if e in guessed and e not in known_set and e not in called and e not in resume
             and t0_ <= e < t1_ and inside[e - t0_] and not starts[e - t0_] and e not in jump_targets}
    empty = {e for e, n in funcs.items() if not n}           # la entrada cae en datos o en una tabla de saltos
    for e in false | empty:
        del funcs[e]
    return sorted(funcs), resume, false | empty, an


FLUSH = 1500


def flush_analyzer(an):
    """Vacía lo que el analizador acumula sin límite (cada instrucción decodificada y cada función)."""
    an._cache.clear()
    an._functions.clear()


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--input", required=True)
    ap.add_argument("--functions", action="append", default=[], help="listas de entradas del exe (se unen)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--juego", default=str(DEFAULT_GAME), help="perfil del juego (config/juegos/<id>.json)")
    args = ap.parse_args(argv)
    t0 = time.time()
    game = load_game(Path(args.juego))
    IMAGES, REBASE, TRANSLATED = game["images"], game["rebase"], game["translated"]
    inp, out = Path(args.input), Path(args.out)
    gen = out / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    previous = set(gen.glob("fn_*.c"))   # lo que no se vuelva a escribir en esta corrida se borra al final

    imgdir = out / "images"          # las imágenes tal como las carga el runtime (DLL reubicadas): obra derivada
    imgdir.mkdir(parents=True, exist_ok=True)
    images, relocs = {}, {}
    for i, (name, _) in enumerate(IMAGES):
        if name in REBASE:
            relocs[name] = rebase_file(str(inp / name), str(imgdir / name), REBASE[name])
        else:
            shutil.copyfile(inp / name, imgdir / name)
        images[name] = Image(str(imgdir / name), stub_base=IMPORT_STUBS + i * STUB_SPACING, translated=TRANSLATED)
    spans = sorted((im.base, im.base + im.size, n) for n, im in images.items())
    for (a0, a1, n0), (b0, b1, n1) in zip(spans, spans[1:]):
        if a1 > b0:
            raise SystemExit(f"{n0} y {n1} se pisan en memoria")

    def export_addr(dll, fn):
        """Dirección del export que nombra el import fn (ya con prefijo) de la DLL traducida dll."""
        im = images[next(n for n in images if n.upper() == dll.upper())]
        raw = fn.split("__", 1)[1] if "__" in fn else None
        want = ("ord" + fn.rsplit("_ord", 1)[1]) if raw is None and "_ord" in fn else None
        for addr, labels in im.exports.items():
            for lb in labels:
                if (raw is not None and translate_name(lb) == raw) or (want is not None and lb == want):
                    return addr
        return None

    from pe_image import c_name as translate_name
    msvcrt = images["MSVCRT.DLL"]
    noreturn = {a: lb for a, lbs in msvcrt.exports.items() for lb in lbs if lb in NORETURN_EXPORTS}
    setjmp = {a for a, lbs in msvcrt.exports.items() if set(lbs) & SETJMP_EXPORTS}
    setjmp_imports = {"MSVCRT__" + s for s in SETJMP_EXPORTS}
    analysis.NORETURN_IMPORTS |= {"MSVCRT__" + s for s in NORETURN_EXPORTS}

    exe_known = sorted({a for p in args.functions for a in recomp.read_functions(p)})
    all_funcs, all_resume, report = {}, set(), {"images": {}}
    referenced = set()
    empty_entries = []
    stats_total = collections.Counter()
    unhandled_total = collections.Counter()
    files = []

    def names(addr):
        referenced.add(addr)
        return recomp.fname(addr)

    for name, kind in IMAGES:
        im = images[name]
        roots = set(exe_known) if kind == "functions" else set(im.exports)
        known = [a for a in sorted(roots | {im.entry}) if im.is_code(a)]   # sin listas de IDA/Ghidra: desde la entrada
        nr = noreturn if name == "MSVCRT.DLL" else {}
        funcs, resume, false, an = analyze(im, known, nr, setjmp if name == "MSVCRT.DLL" else set(), setjmp_imports)
        st = collections.Counter()
        unh = collections.Counter()
        chunk, first = [], None
        stem = name.split(".")[0].lower()

        def flush():
            nonlocal chunk, first
            if chunk:
                p = gen / f"fn_{stem}_{first:08x}.c"
                write_if_changed(p, '#include "rt.h"\n#include "functions.h"\n\n' + "\n".join(chunk))
                previous.discard(p)
                files.append(p.name)
            chunk, first = [], None

        for i, e in enumerate(funcs):
            fn = an.function(e)                  # segunda pasada: se decodifica de nuevo y se suelta (memoria acotada)
            first = e if first is None else first
            if not fn.insns:
                # Una entrada de las listas donde no se decodifica ninguna instrucción (una lista de funciones externa
                # puede traer alguna): el escritor no la admite. Queda un stub que para si alguna vez se llama.
                chunk.append(f'void {names(e)}(void) {{ rt_fatal("function %#x decodes to nothing", 0x{e:x}u); }}\n')
                empty_entries.append(e)
                continue
            w = translate.FunctionWriter(fn, im, names)
            chunk.append(w.write())
            if len(chunk) >= recomp.GROUP:
                flush()
            if (i + 1) % FLUSH == 0:
                flush_analyzer(an)
            st["functions"] += 1
            st["instructions"] += len(fn.insns)
            st["handled"] += w.handled
            for _, text, _ in w.unhandled:
                unh[text.split()[0] if not text.startswith("lock") else text.split()[0] + " " + text.split()[1]] += 1
        flush()
        flush_analyzer(an)
        del an
        all_funcs.update({e: name for e in funcs})
        all_resume |= resume
        stats_total.update(st)
        unhandled_total.update(unh)
        report["images"][name] = {
            "base": "%#x" % im.base, "entry": "%#x" % im.entry, "functions": st["functions"],
            "instructions": st["instructions"], "handled_share": round(st["handled"] / max(1, st["instructions"]), 6),
            "unhandled": dict(unh.most_common()), "false_starts_dropped": len(false), "resume_entries": len(resume),
            "imports": len(im.imports), "exports": len(im.exports)}

    # Imports: pegamento hacia DLL traducidas, runtime o stub.
    have = runtime_imports()
    glue, stubs, iat_rows, rt_rows = {}, [], [], {}
    unresolved = []
    for name, im in images.items():
        for iat, (dll, fn, stub) in sorted(im.imports.items()):
            if dll.upper() in TRANSLATED:
                addr = export_addr(dll, fn)
                if addr is None:
                    unresolved.append(f"{name}: {dll}!{fn}")
                    stubs.append(fn)
                    iat_rows.append((iat, stub))
                    rt_rows[stub] = (f"imp_{fn}", fn)
                    continue
                glue[fn] = addr
                iat_rows.append((iat, addr))        # en la IAT, la dirección real del export (como Windows)
                rt_rows[stub] = (f"imp_{fn}", fn)
            else:
                if fn not in have:
                    stubs.append(fn)
                iat_rows.append((iat, stub))
                rt_rows[stub] = (f"imp_{fn}", fn)
    missing = sorted(a for a in referenced | set(glue.values()) if a not in all_funcs)
    for a in set(all_funcs) | set(missing):
        rt_rows[a] = (recomp.fname(a), recomp.fname(a))

    imp_names = sorted({fn for im in images.values() for _, fn, _ in im.imports.values()})
    decls = ["/* Generado por tools/recomp/build.py. */", translator.EXTRA_DECLS]
    decls += [f"void {recomp.fname(a)}(void);" for a in sorted(set(all_funcs) | set(missing) | referenced)]
    decls += [f"void imp_{n}(void);" for n in imp_names]
    write_if_changed(gen / "functions.h", "\n".join(decls) + "\n")
    for old in previous:
        old.unlink()

    lines = ['#include "rt.h"', '#include "rt_host.h"', '#include "functions.h"', ""]
    for a in missing:
        lines.append(f'void {recomp.fname(a)}(void) {{ rt_fatal("function %#x was not recompiled", 0x{a:x}u); }}')
    for fn, addr in sorted(glue.items()):
        lines.append(f"void imp_{fn}(void) {{ {recomp.fname(addr)}(); }}")
    # Los stubs de imports de Windows sin reemplazo NO van acá: los escribe compile.py en cada compilación
    # (gen/stubs.c) contra el runtime de ese momento, así una función nueva del runtime no pide retraducir.
    (gen / "windows_imports.txt").write_text("".join(f"{fn}\n" for fn in sorted(set(stubs))), encoding="ascii")
    lines.append("\nconst RtEntry rt_table[] = {")
    for a in sorted(rt_rows):
        fn, n = rt_rows[a]
        lines.append(f'    {{0x{a:x}u, {fn}, "{n}", {"RT_RESUME" if a in all_resume else "0"}}},')
    lines.append("};")
    lines.append(f"const uint32_t rt_table_size = {len(rt_rows)};\n")
    lines.append("const RtIat rt_iat[] = {")
    lines += [f"    {{0x{i:x}u, 0x{s:x}u}}," for i, s in sorted(iat_rows)]
    lines.append("};")
    lines.append(f"const uint32_t rt_iat_size = {len(iat_rows)};")
    write_if_changed(gen / "link.c", "\n".join(lines) + "\n")

    img = ['#include "../../../src/rt/loader.h"', "", "const RtImage rt_images[] = {"]
    for name, _ in IMAGES:
        im = images[name]
        img.append(f'    {{"{name}", 0x{im.base:x}u, 0x{im.size:x}u, 0x{im.entry:x}u, {1 if im.is_dll else 0}}},')
    img.append("};")
    img.append(f"const uint32_t rt_image_count = {len(IMAGES)};")
    # El perfil del juego para el runtime: dónde cree que está instalado y lo que dejó el instalador.
    img.append(f"\nconst char rt_game_dir[] = {c_str(game['carpeta'])};")
    img.append(f"const char rt_exe_path[] = {c_str(game['exe_ruta'])};")
    img.append(f"const char rt_command_line[] = {c_str(game.get('linea_comandos', '\"' + game['exe_ruta'] + '\"'))};")
    img.append(f"const char rt_cd_label[] = {c_str(game['etiqueta_cd'])};")
    img.append(f"const uint32_t rt_heap_base = {game['heap_base']}u;")
    dirs = game.get("carpetas", [])
    img.append("const char *const rt_game_dirs[] = {" + "".join(f"{c_str(d)}, " for d in dirs) + "0};")
    img.append(f"const uint32_t rt_game_dir_count = {len(dirs)};")
    files_ = game.get("archivos", {})
    img.append("const RtGameFile rt_game_files[] = {" + "".join(f"{{{c_str(p)}, {c_str(t)}}}, " for p, t in files_.items())
               + "{0, 0}};")
    img.append(f"const uint32_t rt_game_file_count = {len(files_)};")
    write_if_changed(gen / "images.c", "\n".join(img) + "\n")

    if empty_entries:
        report["entradas_vacias"] = [f"0x{a:08x}" for a in sorted(empty_entries)]
    report.update({
        "juego": game["id"], "programa": game["programa"],
        "functions": stats_total["functions"], "instructions": stats_total["instructions"],
        "handled_share": round(stats_total["handled"] / max(1, stats_total["instructions"]), 6),
        "unhandled": dict(unhandled_total.most_common()),
        "glue_imports": len(glue), "windows_imports_in_runtime": len({fn for fn in imp_names if fn in have}),
        "windows_imports_stubbed": len(set(stubs)), "unresolved_dll_imports": unresolved,
        "relocations_applied": relocs, "missing_functions": len(missing), "resume_entries": len(all_resume), "files": len(files) + 2,
        "seconds": round(time.time() - t0, 1)})
    (out / "build.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
