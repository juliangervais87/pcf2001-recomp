/* Web: el CD en el browser. Los archivos del CD no se descargan: cada lectura del juego es un
 * fetch con Range de los bytes justos (bloques de 256 KB con caché en JS), igual que hace PSPRecomp con su
 * psprecomp_range_fetch. Un índice (cd_index.json: ruta relativa -> tamaño) contesta stat y los listados.
 * La URL base y el índice los pone la página (shell.html) en Module.cdBase / Module.cdIndex antes de arrancar.
 * Sólo en el objetivo web (RECOMP_WEB); el objetivo node lee el disco del host (NODERAWFS). */
#ifdef RECOMP_WEB
#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../host.h"

#define WEBCD_PREFIX "/cd"
#define WEBCD_FD_BASE 100000

/* Ruta del juego "/cd/X/Y" -> "X/Y" (sin regex: EM_JS pasa el cuerpo como texto y las barras invertidas se rompen). */
EM_JS(void, webcd_init_js, (void), {
    Module.cdRel = function (p) {
        if (p.slice(0, 3) === "/cd") p = p.slice(3);
        while (p.length && p[0] === "/") p = p.slice(1);
        while (p.length && p[p.length - 1] === "/") p = p.slice(0, -1);
        return p;
    };
});

EM_JS(int, webcd_size, (const char *path), {
    const p = Module.cdRel(UTF8ToString(path)).toUpperCase();
    if (p === "") return -2;                       /* la raíz del CD */
    const s = Module.cdIndex && Module.cdIndex[p];
    return s === undefined ? -1 : (s === "dir" ? -2 : s);
});

/* Lista los nombres de una carpeta del CD separados por '\n' en out; devuelve la cantidad o -1. */
EM_JS(int, webcd_list, (const char *dir, char *out, int room), {
    let d = Module.cdRel(UTF8ToString(dir)).toUpperCase();
    if (d && !d.endsWith("/")) d += "/";
    const seen = new Set();
    for (const k of Object.keys(Module.cdIndex || {})) {
        if (!k.startsWith(d) || k === d) continue;
        const rest = k.slice(d.length);
        const name = rest.split("/")[0];
        if (name) seen.add(name);
    }
    const text = [...seen].join("\n");
    if (lengthBytesUTF8(text) + 1 > room) return -1;
    stringToUTF8(text, out, room);
    return seen.size;
});

/* Lee n bytes desde off del archivo del CD (ruta del índice) a dst; devuelve los leídos. */
EM_ASYNC_JS(int, webcd_read, (const char *path, double off, char *dst, int n), {
    const rel = Module.cdRel(UTF8ToString(path));
    const size = Module.cdIndex[rel.toUpperCase()];
    if (typeof size !== "number" || off >= size) return 0;
    n = Math.min(n, size - off);
    const BLOCK = 262144;
    Module.cdCache = Module.cdCache || new Map();
    let done = 0;
    while (done < n) {
        const pos = off + done, b = Math.floor(pos / BLOCK), key = rel.toUpperCase() + "#" + b;
        let chunk = Module.cdCache.get(key);
        if (!chunk) {
            const start = b * BLOCK, end = Math.min(size, start + BLOCK) - 1;
            const url = Module.cdBase + "/" + (Module.cdNames[rel.toUpperCase()] || rel);
            const r = await fetch(url, {headers: {Range: "bytes=" + start + "-" + end}});
            chunk = new Uint8Array(await r.arrayBuffer());
            if (r.status === 200 && chunk.length > end - start + 1) chunk = chunk.subarray(start, end + 1);  /* sin Range */
            Module.cdCache.set(key, chunk);
            if (Module.cdCache.size > 512) Module.cdCache.delete(Module.cdCache.keys().next().value);
        }
        const inChunk = pos - b * BLOCK, take = Math.min(n - done, chunk.length - inChunk);
        if (take <= 0) break;
        HEAPU8.set(chunk.subarray(inChunk, inChunk + take), dst + done);
        done += take;
    }
    return done;
});

typedef struct { int used; char path[512]; int64_t pos, size; } WebFile;
static WebFile files[256];

int webcd_is(const char *path)
{
    static int ready;
    if (!ready) { webcd_init_js(); ready = 1; }
    return !strncmp(path, WEBCD_PREFIX, 3) && (path[3] == '/' || !path[3]);
}

int webcd_open(const char *path)
{
    int s = webcd_size(path);
    if (s < 0) return -1;
    for (int i = 0; i < 256; i++)
        if (!files[i].used) {
            files[i].used = 1;
            strncpy(files[i].path, path, sizeof files[i].path - 1);
            files[i].pos = 0;
            files[i].size = s;
            return WEBCD_FD_BASE + i;
        }
    return -1;
}
int webcd_fd(int fd) { return fd >= WEBCD_FD_BASE && fd < WEBCD_FD_BASE + 256 && files[fd - WEBCD_FD_BASE].used; }
long webcd_fd_read(int fd, void *buf, uint32_t n)
{
    WebFile *f = &files[fd - WEBCD_FD_BASE];
    int r = webcd_read(f->path, (double)f->pos, buf, (int)n);
    f->pos += r;
    return r;
}
int64_t webcd_fd_seek(int fd, int64_t off, int whence)
{
    WebFile *f = &files[fd - WEBCD_FD_BASE];
    int64_t p = whence == 0 ? off : whence == 1 ? f->pos + off : f->size + off;
    if (p < 0) return -1;
    return f->pos = p;
}
int webcd_fd_close(int fd) { files[fd - WEBCD_FD_BASE].used = 0; return 0; }
int webcd_stat(const char *path, HostStat *st)
{
    int s = webcd_size(path);
    if (s == -1) return -1;
    st->is_dir = s == -2;
    st->size = s < 0 ? 0 : (uint64_t)s;
    st->mtime = 877996800;          /* 1997-10-28, la fecha de la ISO */
    return 0;
}

/* Listado: el texto de webcd_list partido en nombres. */
typedef struct { char *buf, *next; int dots; } WebDir;
void *webcd_opendir(const char *path)
{
    char *buf = malloc(1 << 20);
    if (webcd_list(path, buf, 1 << 20) < 0) { free(buf); return NULL; }
    WebDir *d = calloc(1, sizeof *d);
    d->buf = d->next = buf;
    return d;
}
const char *webcd_readdir(void *dp)
{
    WebDir *d = dp;
    if (d->dots < 2) return d->dots++ ? ".." : ".";
    if (!d->next || !*d->next) return NULL;
    char *name = d->next, *nl = strchr(name, '\n');
    if (nl) { *nl = 0; d->next = nl + 1; } else d->next = NULL;
    return name;
}
void webcd_closedir(void *dp) { WebDir *d = dp; free(d->buf); free(d); }
#endif
