/* GDI32: contextos de dispositivo (DC) sobre memoria de 8 bits (el framebuffer de la pantalla, una superficie de
 * DirectDraw o un bitmap de memoria), paletas, pinceles y lápices, StretchDIBits/BitBlt, rectángulos y líneas.
 * Lo exacto que el juego necesita (docs/investigacion/fase-2.md § 1): copias de índices de 8 bits y la paleta del
 * sistema. Texto: el juego dibuja el suyo con sus .FNT; TextOutA de GDI se ANOTA (medición pedida en § 2) y dibuja
 * nada. Impresión: todo vuelve como si el usuario hubiera cancelado. */
#include "user32.h"
#include "screen.h"

#include <ctype.h>

/* ---- objetos ---- */
enum { GO_FREE, GO_PEN, GO_BRUSH, GO_FONT, GO_PALETTE, GO_BITMAP, GO_REGION };
typedef struct {
    int kind, stock;
    uint32_t color;                  /* COLORREF de lápiz/pincel */
    int null_style;                  /* PS_NULL / BS_NULL */
    uint8_t pal[256][4];             /* paleta lógica */
    uint32_t pal_n;
    uint32_t bits, w, h, pitch, bpp; /* bitmap */
    int32_t rl, rt, rr, rb;          /* región rectangular */
    int32_t font_h;
} GObj;
#define MAX_GOBJ 2048
#define GOBJ_BASE 0x00040000u
static GObj gobjs[MAX_GOBJ];

static uint32_t gobj_new(int kind)
{
    for (uint32_t i = 32; i < MAX_GOBJ; i++)
        if (gobjs[i].kind == GO_FREE) { memset(&gobjs[i], 0, sizeof gobjs[i]); gobjs[i].kind = kind; return GOBJ_BASE + 4 * i; }
    rt_fatal("demasiados objetos GDI");
}
static GObj *gobj(uint32_t h)
{
    if (h < GOBJ_BASE || (h & 3)) return NULL;
    uint32_t i = (h - GOBJ_BASE) / 4;
    return i < MAX_GOBJ && gobjs[i].kind ? &gobjs[i] : NULL;
}

/* Objetos de stock (GetStockObject): WHITE_BRUSH 0 .. GRAY 2, BLACK_BRUSH 4, NULL_BRUSH 5, WHITE_PEN 6,
 * BLACK_PEN 7, NULL_PEN 8, fuentes 10..17, DEFAULT_PALETTE 15. */
static void stock_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    static const uint32_t brush[6] = {0xffffff, 0xc0c0c0, 0x808080, 0x404040, 0x000000, 0};
    for (int i = 0; i <= 5; i++) { gobjs[i].kind = GO_BRUSH; gobjs[i].stock = 1; gobjs[i].color = brush[i]; gobjs[i].null_style = i == 5; }
    gobjs[6].kind = GO_PEN; gobjs[6].color = 0xffffff; gobjs[6].stock = 1;
    gobjs[7].kind = GO_PEN; gobjs[7].color = 0; gobjs[7].stock = 1;
    gobjs[8].kind = GO_PEN; gobjs[8].null_style = 1; gobjs[8].stock = 1;
    for (int i = 10; i <= 17; i++) { gobjs[i].kind = GO_FONT; gobjs[i].stock = 1; gobjs[i].font_h = 13; }
    gobjs[15].kind = GO_PALETTE;
    gobjs[15].pal_n = 20;
    for (int i = 18; i < 32; i++) { gobjs[i].kind = GO_BITMAP; gobjs[i].stock = 1; gobjs[i].w = gobjs[i].h = 1; }
}
RT_IMPORT(GetStockObject) { stock_init(); uint32_t i = rt_arg(0); rt_ret_stdcall(i <= 17 ? GOBJ_BASE + 4 * i : 0, 1); }

/* ---- DC ---- */
typedef struct {
    int used;
    uint32_t hwnd;                   /* DC de ventana: 0 si es de memoria o de superficie */
    uint32_t bits, w, h, pitch;      /* destino (8 bits) */
    int32_t ox, oy;                  /* origen del DC en el destino (área cliente de la ventana) */
    int32_t vx, vy;                  /* origen del viewport (OffsetViewportOrgEx) */
    int32_t cl, ct, cr, cb;          /* recorte en coordenadas del DC */
    uint32_t pen, brush, font, palette, bitmap;
    uint32_t text_color, bk_color, bk_mode, text_align, map_mode;
    int32_t cur_x, cur_y;
    int is_screen;
} DC;
#define MAX_DC 256
#define DC_BASE 0x00050000u
static DC dcs[MAX_DC];

static DC *dc_get(uint32_t h)
{
    if (h < DC_BASE || (h & 3)) return NULL;
    uint32_t i = (h - DC_BASE) / 4;
    return i && i < MAX_DC && dcs[i].used ? &dcs[i] : NULL;
}
static uint32_t dc_new(void)
{
    stock_init();
    for (uint32_t i = 1; i < MAX_DC; i++)
        if (!dcs[i].used) {
            DC *d = &dcs[i];
            memset(d, 0, sizeof *d);
            d->used = 1;
            d->pen = GOBJ_BASE + 4 * 7;
            d->brush = GOBJ_BASE + 4 * 0;
            d->font = GOBJ_BASE + 4 * 13;
            d->palette = GOBJ_BASE + 4 * 15;
            d->bk_color = 0xffffff;
            d->bk_mode = 2;                       /* OPAQUE */
            d->map_mode = 1;
            return DC_BASE + 4 * i;
        }
    rt_fatal("demasiados DC");
}

uint32_t gdi_window_dc(uint32_t hwnd)
{
    uint32_t h = dc_new();
    DC *d = dc_get(h);
    d->hwnd = hwnd;
    d->bits = screen_fb(); d->w = screen.w; d->h = screen.h; d->pitch = screen.w;
    d->is_screen = 1;
    Wnd *w = wnd_get(hwnd);
    wnd_screen_origin(hwnd, &d->ox, &d->oy);
    d->cl = 0; d->ct = 0;
    d->cr = w ? w->w : (int32_t)screen.w;
    d->cb = w ? w->h : (int32_t)screen.h;
    return h;
}
/* Un DC sobre una superficie de DirectDraw (IDirectDrawSurface::GetDC). */
uint32_t gdi_surface_dc(uint32_t bits, uint32_t w, uint32_t h, uint32_t pitch, int is_primary)
{
    uint32_t hd = dc_new();
    DC *d = dc_get(hd);
    d->bits = bits; d->w = w; d->h = h; d->pitch = pitch;
    d->cr = (int32_t)w; d->cb = (int32_t)h;
    d->is_screen = is_primary;
    return hd;
}
void gdi_release_dc(uint32_t hdc) { DC *d = dc_get(hdc); if (d) d->used = 0; }

RT_IMPORT(CreateCompatibleDC)
{
    uint32_t h = dc_new();
    DC *d = dc_get(h);
    d->bitmap = GOBJ_BASE + 4 * 18;                /* un bitmap de 1x1 */
    d->cr = d->cb = 1;
    rt_ret_stdcall(h, 1);
}
RT_IMPORT(CreateDCA) { rt_ret_stdcall(gdi_window_dc(0), 4); }
RT_IMPORT(CreateICA) { rt_ret_stdcall(gdi_window_dc(0), 4); }
RT_IMPORT(DeleteDC) { gdi_release_dc(rt_arg(0)); rt_ret_stdcall(1, 1); }
RT_IMPORT(SaveDC) { rt_ret_stdcall(1, 1); }
RT_IMPORT(RestoreDC) { rt_ret_stdcall(1, 2); }

/* ---- color -> índice de la paleta de la pantalla ---- */
static uint8_t nearest(uint32_t rgb)
{
    int r = rgb & 0xff, g = (rgb >> 8) & 0xff, b = (rgb >> 16) & 0xff, best = 0, bd = 1 << 30;
    for (int i = 0; i < 256; i++) {
        int dr = r - screen.palette[i][0], dg = g - screen.palette[i][1], db = b - screen.palette[i][2];
        int dd = dr * dr + dg * dg + db * db;
        if (dd < bd) { bd = dd; best = i; if (!dd) break; }
    }
    return (uint8_t)best;
}
static uint8_t color_index(DC *d, uint32_t c)
{
    if ((c >> 24) == 1) return (uint8_t)(c & 0xff);                     /* PALETTEINDEX */
    if ((c >> 24) == 2) {                                               /* PALETTERGB: el más cercano */
        c &= 0xffffff;
    }
    (void)d;
    return nearest(c & 0xffffff);
}

static void fill(DC *d, int32_t l, int32_t t, int32_t r, int32_t b, uint8_t idx)
{
    l += d->vx; r += d->vx; t += d->vy; b += d->vy;
    if (l < d->cl) l = d->cl;
    if (t < d->ct) t = d->ct;
    if (r > d->cr) r = d->cr;
    if (b > d->cb) b = d->cb;
    for (int32_t y = t; y < b; y++) {
        int32_t sy = y + d->oy;
        if (sy < 0 || sy >= (int32_t)d->h) continue;
        int32_t x0 = l + d->ox, x1 = r + d->ox;
        if (x0 < 0) x0 = 0;
        if (x1 > (int32_t)d->w) x1 = (int32_t)d->w;
        if (x1 > x0) memset(g_mem + d->bits + (uint32_t)sy * d->pitch + (uint32_t)x0, idx, (size_t)(x1 - x0));
    }
    if (d->is_screen) screen_dirty();
}
static void plot(DC *d, int32_t x, int32_t y, uint8_t idx)
{
    x += d->vx; y += d->vy;
    if (x < d->cl || x >= d->cr || y < d->ct || y >= d->cb) return;
    int32_t sx = x + d->ox, sy = y + d->oy;
    if (sx < 0 || sy < 0 || sx >= (int32_t)d->w || sy >= (int32_t)d->h) return;
    g_mem[d->bits + (uint32_t)sy * d->pitch + (uint32_t)sx] = idx;
}

RT_IMPORT(FillRect)
{
    DC *d = dc_get(rt_arg(0));
    uint32_t r = rt_arg(1), br = rt_arg(2);
    GObj *b = gobj(br);
    uint32_t color = b ? b->color : 0;
    if (br && br < 0x00080100u && br >= 0x00080000u) color = 0xc0c0c0;   /* GetSysColorBrush */
    if (d && !(b && b->null_style))
        fill(d, (int32_t)rd32(g_mem, r), (int32_t)rd32(g_mem, r + 4), (int32_t)rd32(g_mem, r + 8), (int32_t)rd32(g_mem, r + 12), color_index(d, color));
    rt_ret_stdcall(1, 3);
}

RT_IMPORT(Rectangle)
{
    DC *d = dc_get(rt_arg(0));
    int32_t l = (int32_t)rt_arg(1), t = (int32_t)rt_arg(2), r = (int32_t)rt_arg(3), b = (int32_t)rt_arg(4);
    if (d) {
        GObj *br = gobj(d->brush), *pen = gobj(d->pen);
        if (br && !br->null_style) fill(d, l, t, r, b, color_index(d, br->color));
        if (pen && !pen->null_style) {
            uint8_t c = color_index(d, pen->color);
            fill(d, l, t, r, t + 1, c); fill(d, l, b - 1, r, b, c);
            fill(d, l, t, l + 1, b, c); fill(d, r - 1, t, r, b, c);
        }
    }
    rt_ret_stdcall(1, 5);
}
RT_IMPORT(PatBlt)
{
    DC *d = dc_get(rt_arg(0));
    if (d) {
        GObj *br = gobj(d->brush);
        uint32_t rop = rt_arg(5);
        uint8_t c = rop == 0x00000042u ? 0 : rop == 0x00FF0062u ? 255 : color_index(d, br ? br->color : 0);   /* BLACKNESS/WHITENESS */
        int32_t x = (int32_t)rt_arg(1), y = (int32_t)rt_arg(2);
        fill(d, x, y, x + (int32_t)rt_arg(3), y + (int32_t)rt_arg(4), c);
    }
    rt_ret_stdcall(1, 6);
}

RT_IMPORT(MoveToEx)
{
    DC *d = dc_get(rt_arg(0));
    if (d) {
        if (rt_arg(3)) { wr32(g_mem, rt_arg(3), (uint32_t)d->cur_x); wr32(g_mem, rt_arg(3) + 4, (uint32_t)d->cur_y); }
        d->cur_x = (int32_t)rt_arg(1); d->cur_y = (int32_t)rt_arg(2);
    }
    rt_ret_stdcall(d != NULL, 4);
}
RT_IMPORT(LineTo)
{
    DC *d = dc_get(rt_arg(0));
    if (d) {
        GObj *pen = gobj(d->pen);
        int32_t x0 = d->cur_x, y0 = d->cur_y, x1 = (int32_t)rt_arg(1), y1 = (int32_t)rt_arg(2);
        if (pen && !pen->null_style) {
            uint8_t c = color_index(d, pen->color);
            int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
            int32_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
            for (int32_t x = x0, y = y0; !(x == x1 && y == y1);) {   /* Bresenham, sin el punto final (como GDI) */
                plot(d, x, y, c);
                int32_t e2 = 2 * err;
                if (e2 >= dy) { err += dy; x += sx; }
                if (e2 <= dx) { err += dx; y += sy; }
            }
            if (d->is_screen) screen_dirty();
        }
        d->cur_x = x1; d->cur_y = y1;
    }
    rt_ret_stdcall(1, 3);
}
RT_IMPORT(SetPixel) { DC *d = dc_get(rt_arg(0)); if (d) { plot(d, (int32_t)rt_arg(1), (int32_t)rt_arg(2), color_index(d, rt_arg(3))); if (d->is_screen) screen_dirty(); } rt_ret_stdcall(rt_arg(3), 4); }
RT_IMPORT(GetPixel)
{
    DC *d = dc_get(rt_arg(0));
    uint32_t r = 0;
    if (d) {
        int32_t x = (int32_t)rt_arg(1) + d->ox, y = (int32_t)rt_arg(2) + d->oy;
        if (x >= 0 && y >= 0 && x < (int32_t)d->w && y < (int32_t)d->h) {
            const uint8_t *c = screen.palette[g_mem[d->bits + (uint32_t)y * d->pitch + (uint32_t)x]];
            r = (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16;
        }
    }
    rt_ret_stdcall(r, 3);
}

/* ---- DIB y blits ---- */
/* Mapa de los 256 índices de una DIB de 8 bits a la pantalla: identidad con DIB_PAL_COLORS o si la tabla de colores
 * es la paleta actual; si no, el color más cercano. */
static void dib_map(uint32_t bmi, uint32_t usage, uint8_t map[256])
{
    uint32_t hdr = rd32(g_mem, bmi), used = rd32(g_mem, bmi + 32);
    if (!used) used = 256;
    if (usage == 1) {                                         /* DIB_PAL_COLORS: índices de 16 bits */
        for (int i = 0; i < 256; i++) map[i] = (uint8_t)(i < (int)used ? rd16(g_mem, bmi + hdr + 2u * (uint32_t)i) : (uint32_t)i);
        return;
    }
    int same = 1;
    for (uint32_t i = 0; i < used && i < 256 && same; i++) {
        uint32_t q = bmi + hdr + 4 * i;
        same = g_mem[q + 2] == screen.palette[i][0] && g_mem[q + 1] == screen.palette[i][1] && g_mem[q] == screen.palette[i][2];
    }
    for (uint32_t i = 0; i < 256; i++) {
        if (same || i >= used) { map[i] = (uint8_t)i; continue; }
        uint32_t q = bmi + hdr + 4 * i;
        map[i] = nearest((uint32_t)g_mem[q + 2] | (uint32_t)g_mem[q + 1] << 8 | (uint32_t)g_mem[q] << 16);
    }
}

static void blit8(DC *d, int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                  uint32_t src, uint32_t spitch, int bottom_up, int32_t sh, int32_t sx, int32_t sy, int32_t sw, int32_t shh,
                  const uint8_t *map)
{
    if (dw <= 0 || dh <= 0 || sw <= 0 || shh <= 0) return;
    dx += d->vx; dy += d->vy;
    for (int32_t y = 0; y < dh; y++) {
        int32_t ty = dy + y;
        if (ty < d->ct || ty >= d->cb) continue;
        int32_t py = ty + d->oy;
        if (py < 0 || py >= (int32_t)d->h) continue;
        int32_t srow = sy + (int32_t)((int64_t)y * shh / dh);
        if (srow < 0 || srow >= sh) continue;
        int32_t mrow = bottom_up ? sh - 1 - srow : srow;
        const uint8_t *s = g_mem + src + (uint32_t)mrow * spitch;
        uint8_t *o = g_mem + d->bits + (uint32_t)py * d->pitch;
        for (int32_t x = 0; x < dw; x++) {
            int32_t tx = dx + x;
            if (tx < d->cl || tx >= d->cr) continue;
            int32_t px = tx + d->ox;
            if (px < 0 || px >= (int32_t)d->w) continue;
            int32_t scol = sx + (int32_t)((int64_t)x * sw / dw);
            o[px] = map ? map[s[scol]] : s[scol];
        }
    }
    if (d->is_screen) screen_dirty();
}

/* StretchDIBits(hdc, xD, yD, wD, hD, xS, yS, wS, hS, bits, bmi, usage, rop) */
RT_IMPORT(StretchDIBits)
{
    DC *d = dc_get(rt_arg(0));
    WIN_TRACE("StretchDIBits(%#x%s, %d,%d %dx%d)\n", rt_arg(0), d && d->is_screen ? " pantalla" : "", (int)rt_arg(1), (int)rt_arg(2), (int)rt_arg(3), (int)rt_arg(4));
    uint32_t bits = rt_arg(9), bmi = rt_arg(10), usage = rt_arg(11);
    int32_t bw = (int32_t)rd32(g_mem, bmi + 4), bh = (int32_t)rd32(g_mem, bmi + 8);
    uint32_t bpp = rd16(g_mem, bmi + 14);
    static int warned;
    if (d && bpp == 8) {
        uint8_t map[256];
        dib_map(bmi, usage, map);
        int up = bh > 0;
        int32_t ah = up ? bh : -bh;
        int32_t ys = (int32_t)rt_arg(6), hs = (int32_t)rt_arg(8);
        /* en una DIB de abajo hacia arriba, yS cuenta desde abajo */
        int32_t top = up ? ah - ys - hs : ys;
        blit8(d, (int32_t)rt_arg(1), (int32_t)rt_arg(2), (int32_t)rt_arg(3), (int32_t)rt_arg(4),
              bits, ((uint32_t)bw + 3u) & ~3u, up, ah, (int32_t)rt_arg(5), top, (int32_t)rt_arg(7), hs, map);
    } else if (!warned++) fprintf(stderr, "recomp: StretchDIBits de %u bits sin implementar\n", bpp);
    rt_ret_stdcall((uint32_t)rt_arg(8), 13);
}
RT_IMPORT(SetDIBitsToDevice)
{
    DC *d = dc_get(rt_arg(0));
    uint32_t bits = rt_arg(9), bmi = rt_arg(10), usage = rt_arg(11);
    int32_t bw = (int32_t)rd32(g_mem, bmi + 4), bh = (int32_t)rd32(g_mem, bmi + 8);
    if (d && rd16(g_mem, bmi + 14) == 8) {
        uint8_t map[256];
        dib_map(bmi, usage, map);
        int up = bh > 0;
        int32_t ah = up ? bh : -bh, w = (int32_t)rt_arg(3), h = (int32_t)rt_arg(4), ys = (int32_t)rt_arg(6);
        blit8(d, (int32_t)rt_arg(1), (int32_t)rt_arg(2), w, h, bits, ((uint32_t)bw + 3u) & ~3u, up, ah, (int32_t)rt_arg(5), up ? ah - ys - h : ys, w, h, map);
    }
    rt_ret_stdcall(rt_arg(4), 12);
}

static DC *bitmap_source(uint32_t hdc, uint32_t *bits, uint32_t *pitch, int32_t *h)
{
    DC *s = dc_get(hdc);
    if (!s) return NULL;
    *bits = s->bits; *pitch = s->pitch; *h = (int32_t)s->h;
    return s;
}
/* BitBlt(dst, x, y, w, h, src, xs, ys, rop): copia SRCCOPY entre DC de 8 bits; otras ROP como relleno. */
RT_IMPORT(BitBlt)
{
    DC *d = dc_get(rt_arg(0));
    uint32_t rop = rt_arg(8), bits, pitch;
    WIN_TRACE("BitBlt(%#x%s, %d,%d %dx%d, src %#x, rop %#x)\n", rt_arg(0), d && d->is_screen ? " pantalla" : "",
              (int)rt_arg(1), (int)rt_arg(2), (int)rt_arg(3), (int)rt_arg(4), rt_arg(5), rop);
    int32_t sh;
    DC *s = bitmap_source(rt_arg(5), &bits, &pitch, &sh);
    if (d && s && rop == 0x00CC0020u)
        blit8(d, (int32_t)rt_arg(1), (int32_t)rt_arg(2), (int32_t)rt_arg(3), (int32_t)rt_arg(4),
              bits + (uint32_t)(s->oy * (int32_t)pitch + s->ox), pitch, 0, sh, (int32_t)rt_arg(6), (int32_t)rt_arg(7), (int32_t)rt_arg(3), (int32_t)rt_arg(4), NULL);
    else if (d && (rop == 0x00000042u || rop == 0x00FF0062u)) {
        int32_t x = (int32_t)rt_arg(1), y = (int32_t)rt_arg(2);
        fill(d, x, y, x + (int32_t)rt_arg(3), y + (int32_t)rt_arg(4), rop == 0x00000042u ? 0 : 255);
    }
    rt_ret_stdcall(1, 9);
}
RT_IMPORT(StretchBlt)
{
    DC *d = dc_get(rt_arg(0));
    uint32_t bits, pitch;
    int32_t sh;
    DC *s = bitmap_source(rt_arg(5), &bits, &pitch, &sh);
    if (d && s)
        blit8(d, (int32_t)rt_arg(1), (int32_t)rt_arg(2), (int32_t)rt_arg(3), (int32_t)rt_arg(4),
              bits, pitch, 0, sh, (int32_t)rt_arg(6), (int32_t)rt_arg(7), (int32_t)rt_arg(8), (int32_t)rt_arg(9), NULL);
    rt_ret_stdcall(1, 11);
}
RT_IMPORT(SetStretchBltMode) { rt_ret_stdcall(1, 2); }

/* Bitmaps de memoria: 8 bits en el heap. */
static uint32_t make_bitmap(uint32_t w, uint32_t h, uint32_t *bits_out)
{
    uint32_t hb = gobj_new(GO_BITMAP);
    GObj *b = gobj(hb);
    b->w = w; b->h = h; b->bpp = 8; b->pitch = (w + 3u) & ~3u;
    b->bits = rt_alloc_zero(b->pitch * (h ? h : 1));
    if (bits_out) *bits_out = b->bits;
    return hb;
}
RT_IMPORT(CreateCompatibleBitmap) { rt_ret_stdcall(make_bitmap(rt_arg(1), rt_arg(2), NULL), 3); }
RT_IMPORT(CreateBitmap) { uint32_t h = make_bitmap(rt_arg(0), rt_arg(1), NULL); GObj *b = gobj(h); if (rt_arg(4) && rt_arg(3) == 8) memcpy(g_mem + b->bits, g_mem + rt_arg(4), b->pitch * b->h); rt_ret_stdcall(h, 5); }
RT_IMPORT(CreateDIBSection)
{
    uint32_t bmi = rt_arg(1), out = rt_arg(3), bits;
    int32_t w = (int32_t)rd32(g_mem, bmi + 4), h = (int32_t)rd32(g_mem, bmi + 8);
    uint32_t hb = make_bitmap((uint32_t)w, (uint32_t)(h < 0 ? -h : h), &bits);
    if (out) wr32(g_mem, out, bits);
    rt_ret_stdcall(hb, 6);
}

RT_IMPORT(SelectObject)
{
    DC *d = dc_get(rt_arg(0));
    uint32_t h = rt_arg(1), old = 0;
    GObj *o = gobj(h);
    if (d && o) {
        switch (o->kind) {
        case GO_PEN: old = d->pen; d->pen = h; break;
        case GO_BRUSH: old = d->brush; d->brush = h; break;
        case GO_FONT: old = d->font; d->font = h; break;
        case GO_BITMAP:
            old = d->bitmap; d->bitmap = h;
            d->bits = o->bits; d->w = o->w; d->h = o->h; d->pitch = o->pitch; d->ox = d->oy = 0;
            d->cl = d->ct = 0; d->cr = (int32_t)o->w; d->cb = (int32_t)o->h; d->is_screen = 0;
            break;
        case GO_REGION: old = 1; break;
        }
    }
    rt_ret_stdcall(old, 2);
}
RT_IMPORT(DeleteObject)
{
    GObj *o = gobj(rt_arg(0));
    if (o && !o->stock) { if (o->kind == GO_BITMAP && o->bits) rt_free(o->bits); o->kind = GO_FREE; }
    rt_ret_stdcall(1, 1);
}
RT_IMPORT(GetObjectA)
{
    GObj *o = gobj(rt_arg(0));
    uint32_t n = rt_arg(1), p = rt_arg(2), r = 0;
    if (o && o->kind == GO_BITMAP && p && n >= 24) {
        memset(g_mem + p, 0, 24);
        wr32(g_mem, p + 4, o->w); wr32(g_mem, p + 8, o->h); wr32(g_mem, p + 12, o->pitch);
        wr16(g_mem, p + 16, 1); wr16(g_mem, p + 18, 8); wr32(g_mem, p + 20, o->bits);
        r = 24;
    } else if (o && o->kind == GO_PALETTE && p) { wr16(g_mem, p, o->pal_n); r = 2; }
    else if (o && p && n) { memset(g_mem + p, 0, n); r = n; }
    rt_ret_stdcall(r, 3);
}
RT_IMPORT(GetCurrentObject)
{
    DC *d = dc_get(rt_arg(0));
    uint32_t k = rt_arg(1), r = 0;
    if (d) r = k == 1 ? d->pen : k == 2 ? d->brush : k == 5 ? d->palette : k == 6 ? d->font : k == 7 ? d->bitmap : 0;
    rt_ret_stdcall(r, 2);
}

RT_IMPORT(CreatePen) { uint32_t h = gobj_new(GO_PEN); GObj *o = gobj(h); o->color = rt_arg(2); o->null_style = rt_arg(0) == 5; rt_ret_stdcall(h, 3); }
RT_IMPORT(CreatePenIndirect) { uint32_t p = rt_arg(0), h = gobj_new(GO_PEN); GObj *o = gobj(h); o->null_style = rd32(g_mem, p) == 5; o->color = rd32(g_mem, p + 12); rt_ret_stdcall(h, 1); }
RT_IMPORT(CreateSolidBrush) { uint32_t h = gobj_new(GO_BRUSH); gobj(h)->color = rt_arg(0); rt_ret_stdcall(h, 1); }
RT_IMPORT(CreateBrushIndirect)
{
    uint32_t p = rt_arg(0), h = gobj_new(GO_BRUSH);
    GObj *o = gobj(h);
    o->null_style = rd32(g_mem, p) == 1;            /* BS_NULL */
    o->color = rd32(g_mem, p + 4);
    rt_ret_stdcall(h, 1);
}
RT_IMPORT(CreatePatternBrush) { uint32_t h = gobj_new(GO_BRUSH); rt_ret_stdcall(h, 1); }
RT_IMPORT(CreateHatchBrush) { uint32_t h = gobj_new(GO_BRUSH); gobj(h)->color = rt_arg(1); rt_ret_stdcall(h, 2); }

/* ---- fuentes y texto ---- */
RT_IMPORT(CreateFontIndirectA)
{
    uint32_t lf = rt_arg(0), h = gobj_new(GO_FONT);
    int32_t height = (int32_t)rd32(g_mem, lf);
    gobj(h)->font_h = height < 0 ? -height : height ? height : 13;
    WIN_TRACE("CreateFontIndirectA(%s, %d)\n", win_str(lf + 28), height);
    rt_ret_stdcall(h, 1);
}
RT_IMPORT(CreateFontA) { uint32_t h = gobj_new(GO_FONT); int32_t height = (int32_t)rt_arg(0); gobj(h)->font_h = height < 0 ? -height : height ? height : 13; rt_ret_stdcall(h, 14); }
static int32_t font_h(DC *d) { GObj *f = d ? gobj(d->font) : NULL; return f && f->font_h ? f->font_h : 13; }
static int32_t char_w(DC *d) { return font_h(d) * 7 / 13 > 0 ? font_h(d) * 7 / 13 : 1; }

static uint32_t textout_count;
static void note_text(const char *fn, int32_t x, int32_t y, const char *s, uint32_t n)
{
    textout_count++;
    if (textout_count <= 200) fprintf(stderr, "recomp: %s(%d,%d) \"%.*s\" (llamada %u desde %#x)\n", fn, x, y, (int)n, s, textout_count, rd32(g_mem, cpu.esp));
}
RT_IMPORT(TextOutA)
{
    note_text("TextOutA", (int32_t)rt_arg(1), (int32_t)rt_arg(2), win_str(rt_arg(3)), rt_arg(4));
    rt_ret_stdcall(1, 5);
}
RT_IMPORT(ExtTextOutA)
{
    note_text("ExtTextOutA", (int32_t)rt_arg(1), (int32_t)rt_arg(2), rt_arg(5) ? win_str(rt_arg(5)) : "", rt_arg(6));
    rt_ret_stdcall(1, 8);
}
RT_IMPORT(GetTextExtentPoint32A)
{
    DC *d = dc_get(rt_arg(0));
    uint32_t sz = rt_arg(3);
    wr32(g_mem, sz, rt_arg(2) * (uint32_t)char_w(d));
    wr32(g_mem, sz + 4, (uint32_t)font_h(d));
    rt_ret_stdcall(1, 4);
}
RT_IMPORT(GetTextExtentPointA)
{
    DC *d = dc_get(rt_arg(0));
    uint32_t sz = rt_arg(3);
    wr32(g_mem, sz, rt_arg(2) * (uint32_t)char_w(d));
    wr32(g_mem, sz + 4, (uint32_t)font_h(d));
    rt_ret_stdcall(1, 4);
}
RT_IMPORT(GetTextMetricsA)
{
    DC *d = dc_get(rt_arg(0));
    uint32_t p = rt_arg(1);
    int32_t h = font_h(d);
    memset(g_mem + p, 0, 56);
    wr32(g_mem, p, (uint32_t)h);
    wr32(g_mem, p + 4, (uint32_t)(h * 10 / 13));
    wr32(g_mem, p + 8, (uint32_t)(h - h * 10 / 13));
    wr32(g_mem, p + 20, (uint32_t)char_w(d));
    wr32(g_mem, p + 24, (uint32_t)(char_w(d) * 2));
    wr32(g_mem, p + 28, 400);
    g_mem[p + 46] = 0; g_mem[p + 47] = 255; g_mem[p + 48] = '?'; g_mem[p + 49] = ' ';
    g_mem[p + 51] = 0x26;                            /* TMPF_VECTOR | TMPF_TRUETYPE | variable */
    rt_ret_stdcall(1, 2);
}
RT_IMPORT(GetCharWidthA)
{
    DC *d = dc_get(rt_arg(0));
    for (uint32_t c = rt_arg(1), i = 0; c <= rt_arg(2); c++, i++) wr32(g_mem, rt_arg(3) + 4 * i, (uint32_t)char_w(d));
    rt_ret_stdcall(1, 4);
}
RT_IMPORT(GetTextFaceA) { win_put_str(rt_arg(2), "Arial", rt_arg(1)); rt_ret_stdcall(5, 3); }
RT_IMPORT(SetTextColor) { DC *d = dc_get(rt_arg(0)); uint32_t o = d ? d->text_color : 0; if (d) d->text_color = rt_arg(1); rt_ret_stdcall(o, 2); }
RT_IMPORT(SetBkColor) { DC *d = dc_get(rt_arg(0)); uint32_t o = d ? d->bk_color : 0; if (d) d->bk_color = rt_arg(1); rt_ret_stdcall(o, 2); }
RT_IMPORT(GetBkColor) { DC *d = dc_get(rt_arg(0)); rt_ret_stdcall(d ? d->bk_color : 0, 1); }
RT_IMPORT(GetTextColor) { DC *d = dc_get(rt_arg(0)); rt_ret_stdcall(d ? d->text_color : 0, 1); }
RT_IMPORT(SetBkMode) { DC *d = dc_get(rt_arg(0)); uint32_t o = d ? d->bk_mode : 0; if (d) d->bk_mode = rt_arg(1); rt_ret_stdcall(o, 2); }
RT_IMPORT(SetTextAlign) { DC *d = dc_get(rt_arg(0)); uint32_t o = d ? d->text_align : 0; if (d) d->text_align = rt_arg(1); rt_ret_stdcall(o, 2); }
RT_IMPORT(SetTextJustification) { rt_ret_stdcall(1, 3); }
RT_IMPORT(SetTextCharacterExtra) { rt_ret_stdcall(0, 2); }
RT_IMPORT(SetMapMode) { DC *d = dc_get(rt_arg(0)); uint32_t o = d ? d->map_mode : 1; if (d) d->map_mode = rt_arg(1); rt_ret_stdcall(o, 2); }
RT_IMPORT(GetMapMode) { DC *d = dc_get(rt_arg(0)); rt_ret_stdcall(d ? d->map_mode : 1, 1); }
RT_IMPORT(SetViewportExtEx) { rt_ret_stdcall(1, 4); }
RT_IMPORT(SetWindowExtEx) { rt_ret_stdcall(1, 4); }
RT_IMPORT(ScaleViewportExtEx) { rt_ret_stdcall(1, 6); }
RT_IMPORT(ScaleWindowExtEx) { rt_ret_stdcall(1, 6); }
RT_IMPORT(SetWindowOrgEx) { rt_ret_stdcall(1, 4); }
RT_IMPORT(OffsetViewportOrgEx)
{
    DC *d = dc_get(rt_arg(0));
    if (d) {
        if (rt_arg(3)) { wr32(g_mem, rt_arg(3), (uint32_t)d->vx); wr32(g_mem, rt_arg(3) + 4, (uint32_t)d->vy); }
        d->vx += (int32_t)rt_arg(1); d->vy += (int32_t)rt_arg(2);
    }
    rt_ret_stdcall(1, 4);
}
RT_IMPORT(SetViewportOrgEx)
{
    DC *d = dc_get(rt_arg(0));
    if (d) {
        if (rt_arg(3)) { wr32(g_mem, rt_arg(3), (uint32_t)d->vx); wr32(g_mem, rt_arg(3) + 4, (uint32_t)d->vy); }
        d->vx = (int32_t)rt_arg(1); d->vy = (int32_t)rt_arg(2);
    }
    rt_ret_stdcall(1, 4);
}
RT_IMPORT(GetViewportOrgEx) { DC *d = dc_get(rt_arg(0)); wr32(g_mem, rt_arg(1), d ? (uint32_t)d->vx : 0); wr32(g_mem, rt_arg(1) + 4, d ? (uint32_t)d->vy : 0); rt_ret_stdcall(1, 2); }

/* ---- recorte ---- */
RT_IMPORT(IntersectClipRect)
{
    DC *d = dc_get(rt_arg(0));
    if (d) {
        int32_t l = (int32_t)rt_arg(1), t = (int32_t)rt_arg(2), r = (int32_t)rt_arg(3), b = (int32_t)rt_arg(4);
        if (l > d->cl) d->cl = l;
        if (t > d->ct) d->ct = t;
        if (r < d->cr) d->cr = r;
        if (b < d->cb) d->cb = b;
    }
    rt_ret_stdcall(2, 5);                              /* SIMPLEREGION */
}
RT_IMPORT(GetClipBox)
{
    DC *d = dc_get(rt_arg(0));
    uint32_t p = rt_arg(1);
    wr32(g_mem, p, d ? (uint32_t)d->cl : 0); wr32(g_mem, p + 4, d ? (uint32_t)d->ct : 0);
    wr32(g_mem, p + 8, d ? (uint32_t)d->cr : screen.w); wr32(g_mem, p + 12, d ? (uint32_t)d->cb : screen.h);
    rt_ret_stdcall(2, 2);
}
RT_IMPORT(CreateRectRgn)
{
    uint32_t h = gobj_new(GO_REGION);
    GObj *o = gobj(h);
    o->rl = (int32_t)rt_arg(0); o->rt = (int32_t)rt_arg(1); o->rr = (int32_t)rt_arg(2); o->rb = (int32_t)rt_arg(3);
    rt_ret_stdcall(h, 4);
}
RT_IMPORT(CreateRectRgnIndirect)
{
    uint32_t r = rt_arg(0), h = gobj_new(GO_REGION);
    GObj *o = gobj(h);
    o->rl = (int32_t)rd32(g_mem, r); o->rt = (int32_t)rd32(g_mem, r + 4); o->rr = (int32_t)rd32(g_mem, r + 8); o->rb = (int32_t)rd32(g_mem, r + 12);
    rt_ret_stdcall(h, 1);
}
RT_IMPORT(SelectClipRgn)
{
    DC *d = dc_get(rt_arg(0));
    GObj *o = gobj(rt_arg(1));
    if (d) {
        if (o) { d->cl = o->rl; d->ct = o->rt; d->cr = o->rr; d->cb = o->rb; }
        else { d->cl = d->ct = 0; Wnd *w = wnd_get(d->hwnd); d->cr = w ? w->w : (int32_t)d->w; d->cb = w ? w->h : (int32_t)d->h; }
    }
    rt_ret_stdcall(2, 2);
}
RT_IMPORT(ExcludeClipRect) { rt_ret_stdcall(2, 5); }
RT_IMPORT(CombineRgn) { rt_ret_stdcall(2, 4); }
RT_IMPORT(PtVisible) { rt_ret_stdcall(1, 3); }
RT_IMPORT(RectVisible) { rt_ret_stdcall(1, 2); }

/* ---- paletas ---- */
RT_IMPORT(CreatePalette)
{
    uint32_t lp = rt_arg(0), h = gobj_new(GO_PALETTE);
    WIN_TRACE("GDI CreatePalette(%u entradas) desde %#x\n", rd16(g_mem, lp + 2), rd32(g_mem, cpu.esp));
    GObj *o = gobj(h);
    o->pal_n = rd16(g_mem, lp + 2);
    if (o->pal_n > 256) o->pal_n = 256;
    memcpy(o->pal, g_mem + lp + 4, 4 * o->pal_n);
    rt_ret_stdcall(h, 1);
}
RT_IMPORT(SelectPalette) { DC *d = dc_get(rt_arg(0)); uint32_t o = d ? d->palette : 0; if (d) d->palette = rt_arg(1); rt_ret_stdcall(o, 3); }
/* RealizePalette: en un DC de la pantalla de 8 bits, la paleta lógica pasa a ser la del sistema (identidad). */
RT_IMPORT(RealizePalette)
{
    DC *d = dc_get(rt_arg(0));
    WIN_TRACE("GDI RealizePalette(%#x%s) desde %#x\n", rt_arg(0), d && d->is_screen ? " pantalla" : "", rd32(g_mem, cpu.esp));
    GObj *p = d ? gobj(d->palette) : NULL;
    uint32_t n = 0;
    if (d && p && p->pal_n && d->is_screen) { screen_set_palette(0, p->pal_n, &p->pal[0][0]); screen_dirty(); n = p->pal_n; }
    rt_ret_stdcall(n, 1);
}
RT_IMPORT(SetPaletteEntries)
{
    WIN_TRACE("GDI SetPaletteEntries(%#x, %u, %u)\n", rt_arg(0), rt_arg(1), rt_arg(2));
    GObj *p = gobj(rt_arg(0));
    uint32_t first = rt_arg(1), n = rt_arg(2), src = rt_arg(3);
    for (uint32_t i = 0; p && i < n && first + i < 256; i++) memcpy(p->pal[first + i], g_mem + src + 4 * i, 4);
    if (p && first + n > p->pal_n) p->pal_n = first + n > 256 ? 256 : first + n;
    rt_ret_stdcall(n, 4);
}
RT_IMPORT(GetPaletteEntries)
{
    GObj *p = gobj(rt_arg(0));
    uint32_t first = rt_arg(1), n = rt_arg(2), dst = rt_arg(3);
    for (uint32_t i = 0; p && i < n && first + i < 256; i++) memcpy(g_mem + dst + 4 * i, p->pal[first + i], 4);
    rt_ret_stdcall(p ? n : 0, 4);
}
RT_IMPORT(AnimatePalette)
{
    WIN_TRACE("GDI AnimatePalette(%#x, %u, %u)\n", rt_arg(0), rt_arg(1), rt_arg(2));
    GObj *p = gobj(rt_arg(0));
    uint32_t first = rt_arg(1), n = rt_arg(2), src = rt_arg(3);
    for (uint32_t i = 0; p && i < n && first + i < 256; i++) memcpy(p->pal[first + i], g_mem + src + 4 * i, 4);
    if (p) { screen_set_palette(first, n, g_mem + src); screen_dirty(); }
    rt_ret_stdcall(1, 4);
}
RT_IMPORT(GetSystemPaletteEntries)
{
    WIN_TRACE("GDI GetSystemPaletteEntries -> %#x, [128]=%02x%02x%02x desde %#x\n", rt_arg(3), screen.palette[128][0], screen.palette[128][1], screen.palette[128][2], rd32(g_mem, cpu.esp));
    uint32_t first = rt_arg(1), n = rt_arg(2), dst = rt_arg(3);
    for (uint32_t i = 0; dst && i < n && first + i < 256; i++) {
        g_mem[dst + 4 * i] = screen.palette[first + i][0];
        g_mem[dst + 4 * i + 1] = screen.palette[first + i][1];
        g_mem[dst + 4 * i + 2] = screen.palette[first + i][2];
        g_mem[dst + 4 * i + 3] = 0;
    }
    rt_ret_stdcall(256, 4);
}
RT_IMPORT(SetSystemPaletteUse) { static uint32_t use = 1; uint32_t o = use; use = rt_arg(1); rt_ret_stdcall(o, 2); }
RT_IMPORT(GetSystemPaletteUse) { rt_ret_stdcall(1, 1); }
RT_IMPORT(GetNearestPaletteIndex) { rt_ret_stdcall(nearest(rt_arg(1)), 2); }
RT_IMPORT(GetNearestColor) { rt_ret_stdcall(rt_arg(1), 2); }
RT_IMPORT(UnrealizeObject) { rt_ret_stdcall(1, 1); }
RT_IMPORT(UpdateColors) { rt_ret_stdcall(1, 1); }

RT_IMPORT(GetDeviceCaps)
{
    uint32_t i = rt_arg(1), r = 0;
    switch (i) {
    case 2: r = 1; break;              /* TECHNOLOGY: DT_RASDISPLAY */
    case 4: r = 240; break;            /* HORZSIZE mm */
    case 6: r = 180; break;            /* VERTSIZE */
    case 8: r = screen.w; break;       /* HORZRES */
    case 10: r = screen.h; break;      /* VERTRES */
    case 12: r = 8; break;             /* BITSPIXEL */
    case 14: r = 1; break;             /* PLANES */
    case 24: r = 20; break;            /* NUMCOLORS */
    case 38: r = 0x7f99; break;        /* RASTERCAPS de una pantalla de 8 bits: incluye RC_PALETTE (0x100), que el gestor de paleta del juego (sub_5A9810) exige */
    case 88: case 90: r = 96; break;   /* LOGPIXELSX/Y */
    case 104: r = 256; break;          /* SIZEPALETTE */
    case 106: r = 20; break;           /* NUMRESERVED */
    case 108: r = 18; break;           /* COLORRES */
    case 116: r = 60; break;           /* VREFRESH */
    }
    rt_ret_stdcall(r, 2);
}
RT_IMPORT(GdiFlush) { rt_ret_stdcall(1, 0); }

/* ---- impresión: el usuario "cancela" ---- */
RT_IMPORT(StartDocA) { fprintf(stderr, "recomp: StartDocA (impresión no soportada)\n"); rt_ret_stdcall(0xffffffffu, 2); }
RT_IMPORT(EndDoc) { rt_ret_stdcall(1, 1); }
RT_IMPORT(StartPage) { rt_ret_stdcall(1, 1); }
RT_IMPORT(EndPage) { rt_ret_stdcall(1, 1); }
RT_IMPORT(AbortDoc) { rt_ret_stdcall(1, 1); }
RT_IMPORT(SetAbortProc) { rt_ret_stdcall(1, 2); }
RT_IMPORT(Escape) { rt_ret_stdcall(0, 5); }
RT_IMPORT(PrintDlgA) { rt_ret_stdcall(0, 1); }                      /* COMDLG32: cancelado */
RT_IMPORT(CommDlgExtendedError) { rt_ret_stdcall(0, 0); }
