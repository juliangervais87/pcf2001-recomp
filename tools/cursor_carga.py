"""Saca los cuadros de un cursor animado .ANI del juego (RIFF ACON) a una tira PNG de cuadros iguales, para la pantalla
de carga de la versión web. Uso: python tools/cursor_carga.py <archivo.ani> <salida.png> [escala=1]"""
import struct
import sys
import zlib


def chunks(d, off, end):
    while off + 8 <= end:
        cid, size = d[off:off + 4], struct.unpack_from("<I", d, off + 4)[0]
        yield cid, off + 8, size
        off += 8 + size + (size & 1)


def frames_of(d):
    out, rate, seq, anih = [], None, None, None
    for cid, o, s in chunks(d, 12, len(d)):
        if cid == b"anih":
            anih = struct.unpack_from("<9I", d, o)
        elif cid == b"rate":
            rate = list(struct.unpack_from(f"<{s // 4}I", d, o))
        elif cid == b"seq ":
            seq = list(struct.unpack_from(f"<{s // 4}I", d, o))
        elif cid == b"LIST" and d[o:o + 4] == b"fram":
            for c2, o2, s2 in chunks(d, o + 4, o + s):
                if c2 == b"icon":
                    out.append(d[o2:o2 + s2])
    return out, anih, rate, seq


def decode_icon(ic):
    # ICONDIR + primera entrada; la imagen es un BITMAPINFOHEADER con alto doble (XOR + AND)
    off = struct.unpack_from("<I", ic, 6 + 12)[0]
    hs, w, h2, planes, bpp = struct.unpack_from("<IiiHH", ic, off)
    h = h2 // 2
    ncol = struct.unpack_from("<I", ic, off + 32)[0] or (1 << bpp if bpp <= 8 else 0)
    pal_off = off + hs
    pal = [struct.unpack_from("<BBBB", ic, pal_off + 4 * i) for i in range(ncol)]
    xor_off = pal_off + 4 * ncol
    xstride = ((w * bpp + 31) // 32) * 4
    and_off = xor_off + xstride * h
    astride = ((w + 31) // 32) * 4
    px = [[(0, 0, 0, 0)] * w for _ in range(h)]
    for y in range(h):
        row = h - 1 - y
        for x in range(w):
            if bpp == 32:
                b, g, r, a = struct.unpack_from("<BBBB", ic, xor_off + row * xstride + 4 * x)
            else:
                bitpos = x * bpp
                byte = ic[xor_off + row * xstride + bitpos // 8]
                idx = (byte >> (8 - bpp - bitpos % 8)) & ((1 << bpp) - 1)
                b, g, r, _ = pal[idx]
                mask = (ic[and_off + row * astride + x // 8] >> (7 - x % 8)) & 1
                a = 0 if mask else 255
            px[y][x] = (r, g, b, a)
    return w, h, px


def write_png(path, w, h, rows):
    raw = b"".join(b"\0" + bytes(c for p in r for c in p) for r in rows)
    def ch(t, data):
        return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b"")
    open(path, "wb").write(png)


d = open(sys.argv[1], "rb").read()
frames, anih, rate, seq = frames_of(d)
k = int(sys.argv[3]) if len(sys.argv) > 3 else 1
decoded = [decode_icon(f) for f in frames]
w, h = decoded[0][0], decoded[0][1]
rows = []
for y in range(h * k):
    row = []
    for fw, fh, px in decoded:
        for x in range(w * k):
            row.append(px[y // k][x // k])
    rows.append(row)
write_png(sys.argv[2], w * k * len(decoded), h * k, rows)
print(f"{len(frames)} cuadros de {w}x{h}, {anih[7] if anih else 10} jiffies (1/60 s) por cuadro")
