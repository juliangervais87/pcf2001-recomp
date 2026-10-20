/* Cursores: el juego usa cursores de Windows (los .CUR/.ANI de la carpeta Cursores, LoadCursorFromFileA + SetCursor),
 * no dibuja el suyo. Se leen del VFS, se decodifica la primera imagen (formato ICO/CUR; un .ANI es un RIFF 'ACON' con
 * cuadros CUR: se toma el primero) y se muestran como cursor de color de SDL (en el browser, SDL lo pasa a CSS).
 * LoadCursorA(NULL, IDC_*) da los cursores del sistema. Sin ventana (sin pantalla) todo es inerte. */
#include "user32.h"
#include "screen.h"
#include "core/host.h"

#include <SDL.h>

#define MAX_CURSORS 64
#define CURSOR_BASE 0x00070000u
static SDL_Cursor *cursors[MAX_CURSORS];
static uint32_t ncursors = 1;
static uint32_t current_cursor;

static uint32_t rd16le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32le(const uint8_t *p) { return rd16le(p) | rd16le(p + 2) << 16; }

/* Decodifica un archivo ICO/CUR en memoria (la primera imagen). */
static SDL_Cursor *from_cur(const uint8_t *d, size_t n)
{
    if (n < 22 || rd16le(d + 2) < 1 || rd16le(d + 2) > 2) return NULL;
    const uint8_t *e = d + 6;
    uint32_t hx = rd16le(e + 4), hy = rd16le(e + 6), off = rd32le(e + 12);
    if (off + 40 > n) return NULL;
    const uint8_t *bi = d + off;
    int32_t w = (int32_t)rd32le(bi + 4), h2 = (int32_t)rd32le(bi + 8);
    uint32_t bpp = rd16le(bi + 14), hdr = rd32le(bi), used = rd32le(bi + 32);
    int32_t h = h2 / 2;
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return NULL;
    if (rd16le(e + 0) == 0) {}                    /* ancho 0 = 256 en el directorio; se usa el del BITMAPINFOHEADER */
    uint32_t ncolors = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    const uint8_t *pal = bi + hdr;
    const uint8_t *xor_bits = pal + 4 * ncolors;
    uint32_t xor_pitch = (((uint32_t)w * bpp + 31) / 32) * 4, and_pitch = (((uint32_t)w + 31) / 32) * 4;
    const uint8_t *and_bits = xor_bits + xor_pitch * (uint32_t)h;
    if ((size_t)(and_bits - d) + and_pitch * (uint32_t)h > n) return NULL;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return NULL;
    for (int32_t y = 0; y < h; y++) {
        const uint8_t *xr = xor_bits + (uint32_t)(h - 1 - y) * xor_pitch;      /* de abajo hacia arriba */
        const uint8_t *ar = and_bits + (uint32_t)(h - 1 - y) * and_pitch;
        uint32_t *out = (uint32_t *)((uint8_t *)s->pixels + y * s->pitch);
        for (int32_t x = 0; x < w; x++) {
            uint32_t r = 0, g = 0, b = 0, a = 255;
            if (bpp == 32) { b = xr[4 * x]; g = xr[4 * x + 1]; r = xr[4 * x + 2]; a = xr[4 * x + 3]; }
            else if (bpp == 24) { b = xr[3 * x]; g = xr[3 * x + 1]; r = xr[3 * x + 2]; }
            else {
                uint32_t idx = bpp == 8 ? xr[x] : bpp == 4 ? (xr[x / 2] >> (x & 1 ? 0 : 4)) & 15 : (xr[x / 8] >> (7 - (x & 7))) & 1;
                if (idx < ncolors) { b = pal[4 * idx]; g = pal[4 * idx + 1]; r = pal[4 * idx + 2]; }
            }
            int transparent = (ar[x / 8] >> (7 - (x & 7))) & 1;
            if (bpp != 32 && transparent) a = 0;             /* AND = 1: transparente (o invertido, sin soporte) */
            out[x] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    SDL_Cursor *c = SDL_CreateColorCursor(s, (int)hx, (int)hy);
    SDL_FreeSurface(s);
    return c;
}

/* Un .ANI: RIFF 'ACON' ... LIST 'fram' { 'icon' <CUR> ... }: el primer cuadro. */
static SDL_Cursor *from_ani(const uint8_t *d, size_t n)
{
    for (size_t i = 12; i + 8 <= n;) {
        uint32_t id = rd32le(d + i), size = rd32le(d + i + 4);
        if (id == 0x5453494Cu) { i += 12; continue; }        /* "LIST": entrar */
        if (id == 0x6E6F6369u && i + 8 + size <= n) return from_cur(d + i + 8, size);   /* "icon" */
        i += 8 + size + (size & 1);
    }
    return NULL;
}

static uint32_t add(SDL_Cursor *c)
{
    if (!c || ncursors == MAX_CURSORS) return 0;
    cursors[ncursors] = c;
    return CURSOR_BASE + ncursors++;
}

RT_IMPORT(LoadCursorFromFileA)
{
    const char *name = win_str(rt_arg(0));
    uint32_t h = 0;
    char host[1024];
    if (!screen_headless()) {
        screen_video_init();
        FILE *f = win_read_path(name, host, sizeof host) ? host_fopen(host, "rb") : NULL;
        if (f) {
            static uint8_t buf[1 << 18];
            size_t n = fread(buf, 1, sizeof buf, f);
            fclose(f);
            h = add(n >= 12 && !memcmp(buf, "RIFF", 4) ? from_ani(buf, n) : from_cur(buf, n));
        }
    }
    WIN_TRACE("LoadCursorFromFile(%s) = %#x\n", name, h);
    rt_ret_stdcall(h ? h : CURSOR_BASE, 1);                  /* sin ventana: un handle válido igual */
}

/* LoadCursorA(NULL, IDC_*): los del sistema. Con instancia: el exe no trae cursores, se usa la flecha. */
RT_IMPORT(LoadCursorA)
{
    uint32_t id = rt_arg(1) & 0xffff;
    SDL_SystemCursor sc = SDL_SYSTEM_CURSOR_ARROW;
    switch (id) {
    case 32513: sc = SDL_SYSTEM_CURSOR_IBEAM; break;
    case 32514: sc = SDL_SYSTEM_CURSOR_WAIT; break;
    case 32515: sc = SDL_SYSTEM_CURSOR_CROSSHAIR; break;
    case 32649: sc = SDL_SYSTEM_CURSOR_HAND; break;
    case 32650: sc = SDL_SYSTEM_CURSOR_WAITARROW; break;
    }
    if (!screen_headless()) screen_video_init();
    uint32_t h = screen_headless() ? 0 : add(SDL_CreateSystemCursor(sc));
    rt_ret_stdcall(h ? h : CURSOR_BASE, 2);
}

RT_IMPORT(SetCursor)
{
    uint32_t old = current_cursor, h = rt_arg(0);
    current_cursor = h;
    uint32_t i = h - CURSOR_BASE;
    if (h && i > 0 && i < ncursors && cursors[i]) SDL_SetCursor(cursors[i]);
    rt_ret_stdcall(old, 1);
}
RT_IMPORT(GetCursor) { rt_ret_stdcall(current_cursor, 0); }
RT_IMPORT(DestroyCursor) { rt_ret_stdcall(1, 1); }
