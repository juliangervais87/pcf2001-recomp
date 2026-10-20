"""Utilidades del traductor que usa build.py: punteros a código en los datos, inicio de función, lista de entradas
y nombre C de una función."""
import json

GROUP = 64          # funciones por archivo C


def data_pointers(image, known):
    """Addresses of code that the data sections hold (vtables, initializer tables, callbacks) and
    that are not in the known list. Only values where a function can start count: aligned to 16 or
    after padding or a ret. Other values are text that happens to look like an address."""
    found = set()
    for name, va, vsize, rptr, rsize, flags in image.sections:
        if image.is_code(va) or not rsize:
            continue
        data = image.read(va, min(vsize or rsize, rsize))
        for off in range(0, len(data) - 3, 4):
            v = int.from_bytes(data[off:off + 4], "little")
            if not image.is_code(v) or v in known:
                continue
            if function_start(image, v, ()):
                found.add(v)
    return found


def function_start(image, v, known):
    """Whether a function can start at v: a known entry, or aligned to 16, or after padding or a ret."""
    return v in known or v % 16 == 0 or image.u8(v - 1) in (0x90, 0xCC, 0xC3) \
        or (image.u8(v - 3) == 0xC2 and image.u8(v - 1) == 0)


def read_functions(path):
    text = open(path).read()
    if text.lstrip().startswith("{"):
        rows = json.loads(text)["normalized_result"]
        return sorted({int(r["address"], 16) for r in rows if r["address"].startswith("0x")})
    return [int(x, 16) for x in text.split()]


def fname(addr):
    return "f_%08x" % addr
