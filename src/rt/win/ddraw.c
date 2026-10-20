/* DirectDraw (ddraw.dll, cargada por el juego con LoadLibraryA + GetProcAddress): IDirectDraw / IDirectDraw2,
 * IDirectDrawSurface hasta la 3, IDirectDrawPalette e IDirectDrawClipper, a 8 bits con paleta. La superficie primaria
 * ES el framebuffer de screen.c; Flip copia la trasera a la primaria y presenta. Los métodos que el
 * juego no llama paran con su nombre. */
#include "ddraw_int.h"
#include "screen.h"

#define DD_OK 0u
#define DDERR_GENERIC 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_NOTFOUND 0x887600FFu
#define DDERR_UNSUPPORTED 0x80004001u

#define DDSD_CAPS 0x1u
#define DDSD_HEIGHT 0x2u
#define DDSD_WIDTH 0x4u
#define DDSD_PITCH 0x8u
#define DDSD_BACKBUFFERCOUNT 0x20u
#define DDSD_PIXELFORMAT 0x1000u
#define DDSCAPS_BACKBUFFER 0x4u
#define DDSCAPS_COMPLEX 0x8u
#define DDSCAPS_FLIP 0x10u
#define DDSCAPS_FRONTBUFFER 0x20u
#define DDSCAPS_OFFSCREENPLAIN 0x40u
#define DDSCAPS_PRIMARYSURFACE 0x200u
#define DDSCAPS_SYSTEMMEMORY 0x800u
#define DDSCAPS_VIDEOMEMORY 0x4000u
#define DDSCAPS_ZBUFFER 0x20000u


typedef struct { uint32_t obj, refs, hwnd; } Clipper;
typedef struct { uint32_t obj1, obj2, obj4, obj7, refs, hwnd, coop; } DDraw;

static uint32_t vt_dd1, vt_dd2, vt_dd4, vt_dd7, vt_surf, vt_surf7, vt_pal, vt_clip;
static DDraw *the_dd;
/* Contadores de diagnóstico (se imprimen con el volcado de ddraw_dump). */
static uint32_t n_blt, n_bltfast, n_flip, n_setentries, n_blt_to_primary, n_blt_to_back, n_vblank;


/* ---- IUnknown genéricos ---- */
static void ret(uint32_t hr, int nargs) { rt_ret_stdcall(hr, nargs + 1); }

static void m_dd_addref(void) { the_dd->refs++; ret(the_dd->refs, 0); }
static void m_dd_release(void) { if (the_dd->refs) the_dd->refs--; ret(the_dd->refs, 0); }

static const char *IID_IDirectDraw = "6c14db80-a733-11ce-a521-0020af0be560";
static const char *IID_IDirectDraw2 = "b3a6f3e0-2b43-11cf-a2de-00aa00b93356";
static const char *IID_IDirectDrawSurface = "6c14db81-a733-11ce-a521-0020af0be560";
static const char *IID_IDirectDrawSurface2 = "57805885-6eec-11cf-9441-a82303c10e27";
static const char *IID_IDirectDrawSurface3 = "da044e00-69b2-11d0-a1d5-00aa00b8dfbb";
static const char *IID_IDirectDraw7 = "15e65ec0-3b9c-11d2-b92f-00609797ea5b";
static const char *IID_IDirectDraw4 = "9c59509a-39bd-11d1-8c4a-00c04fd930c5";
static const char *IID_IDirectDrawSurface4 = "0b2b8630-ad35-11d0-8ea6-00609797ea5b";
static const char *IID_IDirectDrawSurface7 = "06675a80-3b9c-11d2-b92f-00609797ea5b";

/* IDirect3D7 (QueryInterface de IDirectDraw7): el dispositivo RGB por software, en d3d.c. */
static const char *IID_IDirect3D7 = "f5049e77-4861-11d2-a407-00a0c90629a8";
uint32_t d3d7_object(void);

/* Las caras 4 y 7 del objeto DirectDraw se crean al pedirlas: un juego que no las pide no mueve su memoria estática. */
static uint32_t dd_obj4(void) { if (!the_dd->obj4) the_dd->obj4 = com_new(vt_dd4, the_dd, 0); return the_dd->obj4; }
static uint32_t dd_obj7(void) { if (!the_dd->obj7) the_dd->obj7 = com_new(vt_dd7, the_dd, 0); return the_dd->obj7; }

static void m_dd_qi(void)
{
    uint32_t iid = rt_arg(1), out = rt_arg(2);
    char t[40];
    com_guid_text(iid, t);
    uint32_t r = 0;
    if (com_guid_is(iid, IID_IDirectDraw)) r = the_dd->obj1;
    else if (com_guid_is(iid, IID_IDirectDraw2)) r = the_dd->obj2;
    else if (com_guid_is(iid, IID_IDirectDraw4)) r = dd_obj4();
    else if (com_guid_is(iid, IID_IDirectDraw7)) r = dd_obj7();
    else if (com_guid_is(iid, IID_IDirect3D7)) r = d3d7_object();
    fprintf(stderr, "recomp: IDirectDraw::QueryInterface(%s) = %#x\n", t, r);
    if (out) wr32(g_mem, out, r);
    if (r) the_dd->refs++;
    ret(r ? S_OK : E_NOINTERFACE, 2);
}

static void m_dd_coop(void)
{
    the_dd->hwnd = rt_arg(1);
    the_dd->coop = rt_arg(2);
    WIN_TRACE("SetCooperativeLevel(%#x, %#x)\n", rt_arg(1), rt_arg(2));
    ret(DD_OK, 2);
}
/* Cambiar de modo en exclusivo: Windows lleva la ventana a pantalla completa y la invalida (WM_SIZE + WM_PAINT). */
void wnd_fullscreen(uint32_t hwnd, uint32_t w, uint32_t h);
static void set_mode(int nargs)
{
    uint32_t w = rt_arg(1), h = rt_arg(2), bpp = rt_arg(3);
    screen_set_mode(w, h, bpp);
    if (the_dd->coop & 1u) wnd_fullscreen(the_dd->hwnd, w, h);     /* DDSCL_FULLSCREEN */
    ret(DD_OK, nargs);
}
static void m_dd1_setmode(void) { set_mode(3); }
static void m_dd2_setmode(void) { set_mode(5); }
static void m_dd_restoremode(void) { ret(DD_OK, 0); }

/* DDPIXELFORMAT: 8 bits con paleta; 16 en 5-6-5 y 32 en X8R8G8B8 (los de una placa de 2000). */
static void put_pixel_format_bpp(uint32_t pf, uint32_t bpp)
{
    memset(g_mem + pf, 0, 32);
    wr32(g_mem, pf, 32);
    if (bpp == 16 || bpp == 32) {
        wr32(g_mem, pf + 4, 0x40u);                   /* DDPF_RGB */
        wr32(g_mem, pf + 12, bpp);
        wr32(g_mem, pf + 16, bpp == 16 ? 0xF800u : 0xFF0000u);
        wr32(g_mem, pf + 20, bpp == 16 ? 0x07E0u : 0x00FF00u);
        wr32(g_mem, pf + 24, bpp == 16 ? 0x001Fu : 0x0000FFu);
        return;
    }
    wr32(g_mem, pf + 4, 0x60u);                       /* DDPF_RGB | DDPF_PALETTEINDEXED8 */
    wr32(g_mem, pf + 12, 8);                          /* dwRGBBitCount */
}

/* DDSURFACEDESC (108 bytes) o DDSURFACEDESC2 (124, DirectDraw 7: mismos campos hasta ddsCaps, que pasa a 16 bytes);
 * el tamaño lo pone quien llama. */
static void put_desc(uint32_t d, uint32_t w, uint32_t h, uint32_t pitch, uint32_t caps, uint32_t bits, uint32_t bpp)
{
    uint32_t size = rd32(g_mem, d) == 124 ? 124 : 108;
    memset(g_mem + d + 4, 0, size - 4);
    wr32(g_mem, d, size);
    wr32(g_mem, d + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT);
    wr32(g_mem, d + 8, h);
    wr32(g_mem, d + 12, w);
    wr32(g_mem, d + 16, pitch);
    wr32(g_mem, d + 36, bits);
    put_pixel_format_bpp(d + 72, bpp);
    wr32(g_mem, d + 104, caps);
}

static void m_dd_getmode(void) { put_desc(rt_arg(1), screen.w, screen.h, screen.w * (screen.bpp > 8 ? screen.bpp / 8 : 1), 0, 0, screen.bpp); wr32(g_mem, rt_arg(1) + 24, 60); ret(DD_OK, 1); }

/* EnumDisplayModes(this, flags, desc, ctx, callback): las resoluciones de una placa de 1998 a 8 bits (lo que se ve
 * por IDirectDraw/2) y, por IDirectDraw7, también a 16 y 32 (una placa de 2000: PC Fútbol 2001 descarta
 * los de 8 bits en su motor dcsfx). */
static void enum_modes(uint32_t desc_size, int hicolor)
{
    uint32_t ctx = rt_arg(3), cb = rt_arg(4);
    static const uint32_t modes[][2] = {{320, 200}, {640, 400}, {640, 480}, {800, 600}, {1024, 768}};
    static const uint32_t depths[] = {8, 16, 32};
    Cpu s = cpu;
    cpu.esp -= 0x100;
    uint32_t d = cpu.esp + 0x10;
    for (unsigned k = 0; k < (hicolor ? 3u : 1u); k++)
        for (unsigned i = 0; i < sizeof modes / sizeof *modes; i++) {
            uint32_t bpp = depths[k];
            wr32(g_mem, d, desc_size);
            put_desc(d, modes[i][0], modes[i][1], modes[i][0] * (bpp / 8), 0, 0, bpp);
            wr32(g_mem, d + 24, 60);
            uint32_t a[2] = {d, ctx};
            if (!win_call(cb, 2, a)) goto done;       /* DDENUMRET_CANCEL */
        }
done:
    cpu = s;
    ret(DD_OK, 4);
}
static void m_dd_enummodes(void) { enum_modes(108, 0); }
static void m_dd7_enummodes(void) { enum_modes(124, 1); }

static void m_dd_getcaps(void)
{
    for (int i = 1; i <= 2; i++) {
        uint32_t p = rt_arg(i);
        if (!p) continue;
        uint32_t size = rd32(g_mem, p);
        if (size < 8 || size > 0x200) size = 316;
        memset(g_mem + p + 4, 0, size - 4);
        wr32(g_mem, p + 4, 0x00000040u | 0x00000200u | 0x00004000u);   /* DDCAPS_BLT | BLTSTRETCH | COLORKEY */
        wr32(g_mem, p + 12, 0x00000001u);                              /* DDCKEYCAPS_SRCBLT en dwCKeyCaps (aprox.) */
        wr32(g_mem, p + 60, 4u << 20);                                  /* dwVidMemTotal: 4 MB */
        wr32(g_mem, p + 64, 4u << 20);
    }
    ret(DD_OK, 2);
}
static void m_dd_vblank(void)
{
    n_vblank++;
    uint32_t t0 = win_ticks_ms() / 16;
    while (win_ticks_ms() / 16 == t0) rt_host_idle();
    ret(DD_OK, 2);
}
static void m_dd_monitor_freq(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 60); ret(DD_OK, 1); }
static void m_dd_vidmem(void) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), 4u << 20); if (rt_arg(3)) wr32(g_mem, rt_arg(3), 4u << 20); ret(DD_OK, 3); }
static void m_dd_fliptogdi(void) { ret(DD_OK, 0); }
static void m_dd_scanline(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), (win_ticks_ms() % 16) * 30); ret(DD_OK, 1); }
static void m_dd_vbstatus(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 0); ret(DD_OK, 1); }

/* ---- superficies ---- */
#define MAX_SURF 512
static Surface *all_surfaces[MAX_SURF];
static int nsurf;

/* Diagnóstico: vuelca cada superficie viva a PNG (RECOMP_DUMP_AFTER_MS en input.c lo pide una vez). */
void ddraw_dump(const char *dir)
{
    for (int i = 0; i < nsurf; i++) {
        Surface *s = all_surfaces[i];
        if (!s || !s->bits || s->w * s->h < 64) continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/surf_%02d_%ux%u%s.png", dir, i, s->w, s->h, s->primary ? "_primaria" : "");
        screen_write_png(path, s->bits, s->w, s->h, s->pitch);
    }
    {   /* la paleta base del fundido del juego (sub_590360 lee 0x690378..0x690778) */
        int nz = 0;
        for (uint32_t i = 0; i < 256; i++) nz += (rd32(g_mem, 0x690378u + 4 * i) & 0xffffffu) != 0;
        int nz2 = 0;
        for (uint32_t i = 0; i < 236; i++) nz2 += (rd32(g_mem, 0x6326C0u + 4 * i) & 0xffffffu) != 0;
        fprintf(stderr, "recomp: colores de MENU.PAL en 0x6326C0: %d de 236 no negros; [0]=%06x [100]=%06x\n", nz2,
                rd32(g_mem, 0x6326C0u) & 0xffffffu, rd32(g_mem, 0x6326C0u + 400) & 0xffffffu);
        fprintf(stderr, "recomp: paleta base 0x690378: %d entradas no negras; [10..13]: %06x %06x %06x %06x [128]: %06x\n", nz,
                rd32(g_mem, 0x690378u + 40) & 0xffffffu, rd32(g_mem, 0x690378u + 44) & 0xffffffu, rd32(g_mem, 0x690378u + 48) & 0xffffffu,
                rd32(g_mem, 0x690378u + 52) & 0xffffffu, rd32(g_mem, 0x690378u + 512) & 0xffffffu);
    }
    int grises = 0;
    for (int i = 0; i < 256; i++) grises += screen.palette[i][0] == screen.palette[i][1] && screen.palette[i][1] == screen.palette[i][2];
    fprintf(stderr, "recomp: paleta: %d de 256 entradas grises; 16..31:", grises);
    for (int i = 16; i < 32; i++) fprintf(stderr, " %02x%02x%02x", screen.palette[i][0], screen.palette[i][1], screen.palette[i][2]);
    fprintf(stderr, "\n");
    for (int i = 0; i < nsurf; i++) {
        Surface *s = all_surfaces[i];
        if (s && s->palette) {
            Palette *p = com_ptr(s->palette);
            int g2 = 0;
            for (int k = 0; k < 256; k++) g2 += p->entries[k][0] == p->entries[k][1] && p->entries[k][1] == p->entries[k][2];
            fprintf(stderr, "recomp: superficie %d tiene paleta propia (%d grises; 16..19: %02x%02x%02x %02x%02x%02x)\n", i, g2,
                    p->entries[16][0], p->entries[16][1], p->entries[16][2], p->entries[17][0], p->entries[17][1], p->entries[17][2]);
        }
    }
    fprintf(stderr, "recomp: %d superficies volcadas en %s; blt %u bltfast %u (a primaria %u, a trasera %u) flip %u setentries %u vblank %u\n",
            nsurf, dir, n_blt, n_bltfast, n_blt_to_primary, n_blt_to_back, n_flip, n_setentries, n_vblank);
}

static int creating_v7;          /* la superficie que se crea es de IDirectDraw7 (m_dd7_createsurface) */
static uint32_t creating_bpp;    /* el formato que pidió CreateSurface (DDSD_PIXELFORMAT); 0 = el de la pantalla */

static uint32_t new_surface(uint32_t caps, uint32_t w, uint32_t h, int primary)
{
    Surface *s = calloc(1, sizeof *s);
    s->v7 = creating_v7;
    if (nsurf < MAX_SURF) all_surfaces[nsurf++] = s;
    s->refs = 1;
    s->caps = caps;
    s->w = w; s->h = h;
    s->primary = primary;
    s->bpp = !primary && creating_bpp ? creating_bpp : screen.bpp;
    uint32_t bytes = s->bpp > 8 ? s->bpp / 8 : 1;
    s->pitch = primary ? screen.w * bytes : ((w * bytes + 3u) & ~3u);
    s->bits = primary ? screen_fb() : rt_alloc_zero(s->pitch * (h ? h : 1));
    s->obj = com_new(s->v7 ? vt_surf7 : vt_surf, s, 0);
    /* formato por defecto (el de la pantalla); m_dd_createsurface lo pisa si se pidió uno */
    s->pf_flags = s->bpp == 8 ? 0x60u : 0x40u;
    s->rmask = s->bpp == 16 ? 0xF800u : s->bpp == 8 ? 0 : 0xFF0000u;
    s->gmask = s->bpp == 16 ? 0x07E0u : s->bpp == 8 ? 0 : 0x00FF00u;
    s->bmask = s->bpp == 16 ? 0x001Fu : s->bpp == 8 ? 0 : 0x0000FFu;
    return s->obj;
}

/* El DDPIXELFORMAT de una superficie (texturas con alfa, z-buffers): el que se pidió al crearla. */
static void put_surface_pf(uint32_t pf, const Surface *s)
{
    memset(g_mem + pf, 0, 32);
    wr32(g_mem, pf, 32);
    wr32(g_mem, pf + 4, s->pf_flags);
    wr32(g_mem, pf + 12, s->bpp);
    wr32(g_mem, pf + 16, s->rmask);
    wr32(g_mem, pf + 20, s->gmask);
    wr32(g_mem, pf + 24, s->bmask);
    wr32(g_mem, pf + 28, s->amask);
}

static void m_dd_createsurface(void)
{
    uint32_t d = rt_arg(1), out = rt_arg(2);
    uint32_t flags = rd32(g_mem, d + 4), caps = rd32(g_mem, d + 104);
    uint32_t w = flags & DDSD_WIDTH ? rd32(g_mem, d + 12) : screen.w;
    uint32_t h = flags & DDSD_HEIGHT ? rd32(g_mem, d + 8) : screen.h;
    uint32_t r;
    creating_bpp = flags & DDSD_PIXELFORMAT ? rd32(g_mem, d + 72 + 12) : 0;
    /* z-buffer: sólo para Direct3D 7 (d3d.c); DirectDraw 1/2 sigue viendo el no de siempre */
    if ((caps & DDSCAPS_ZBUFFER) && !creating_v7) { fprintf(stderr, "recomp: CreateSurface de z-buffer: no soportado\n"); ret(DDERR_UNSUPPORTED, 3); return; }
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        r = new_surface(caps | DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY, screen.w, screen.h, 1);
        uint32_t nback = flags & DDSD_BACKBUFFERCOUNT ? rd32(g_mem, d + 20) : 0;
        if (nback) surf(r)->back = new_surface(DDSCAPS_BACKBUFFER | DDSCAPS_VIDEOMEMORY | (caps & (DDSCAPS_FLIP | DDSCAPS_COMPLEX)), screen.w, screen.h, 0);
    } else r = new_surface(caps ? caps : DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, w, h, 0);
    Surface *s = surf(r);
    creating_bpp = 0;
    if ((flags & DDSD_PIXELFORMAT) && !s->primary) {
        s->pf_flags = rd32(g_mem, d + 76);
        s->rmask = rd32(g_mem, d + 88); s->gmask = rd32(g_mem, d + 92);
        s->bmask = rd32(g_mem, d + 96); s->amask = rd32(g_mem, d + 100);
    }
    if (flags & 0x10000u) { s->has_ck = 1; s->ck_lo = rd32(g_mem, d + 64) & px_mask(s); s->ck_hi = rd32(g_mem, d + 68) & px_mask(s); }   /* DDSD_CKSRCBLT */
    WIN_TRACE("CreateSurface(%ux%ux%u caps %#x%s) = %#x\n", s->w, s->h, s->bpp, caps, s->back ? " con trasera" : "", r);
    wr32(g_mem, out, r);
    ret(DD_OK, 3);
}
static void m_dd7_createsurface(void) { creating_v7 = 1; m_dd_createsurface(); creating_v7 = 0; }

static void m_dd_createpalette(void)
{
    uint32_t entries = rt_arg(2), out = rt_arg(3);
    WIN_TRACE("DD CreatePalette(flags %#x, [128]=%06x) desde %#x\n", rt_arg(1), entries ? rd32(g_mem, entries + 512) & 0xffffffu : 0, rd32(g_mem, cpu.esp));
    Palette *p = calloc(1, sizeof *p);
    p->refs = 1;
    if (entries) memcpy(p->entries, g_mem + entries, sizeof p->entries);
    p->obj = com_new(vt_pal, p, 0);
    wr32(g_mem, out, p->obj);
    ret(DD_OK, 4);
}
static void m_dd_createclipper(void)
{
    Clipper *c = calloc(1, sizeof *c);
    c->refs = 1;
    c->obj = com_new(vt_clip, c, 0);
    wr32(g_mem, rt_arg(2), c->obj);
    ret(DD_OK, 3);
}

/* Surface: QI, AddRef, Release. */
static void m_s_qi(void)
{
    Surface *s = surf(rt_arg(0));
    uint32_t iid = rt_arg(1), out = rt_arg(2);
    int ok = com_guid_is(iid, IID_IDirectDrawSurface) || com_guid_is(iid, IID_IDirectDrawSurface2) || com_guid_is(iid, IID_IDirectDrawSurface3)
          || (s->v7 && (com_guid_is(iid, IID_IDirectDrawSurface4) || com_guid_is(iid, IID_IDirectDrawSurface7)));
    if (!ok) { char t[40]; com_guid_text(iid, t); fprintf(stderr, "recomp: IDirectDrawSurface::QueryInterface(%s): no\n", t); }
    if (out) wr32(g_mem, out, ok ? s->obj : 0);
    if (ok) s->refs++;
    ret(ok ? S_OK : E_NOINTERFACE, 2);
}
static void m_s_addref(void) { Surface *s = surf(rt_arg(0)); ret(++s->refs, 0); }
/* Una referencia menos. Sin referencias: se libera la memoria de los píxeles; en DirectDraw 7 (PC Fútbol 2001, que crea
 * y suelta miles), también el objeto y la superficie, como en Windows. En DirectDraw 1/2 sólo los píxeles (com.c). */
void ddraw_surface_unref(Surface *s)
{
    if (s->refs) s->refs--;
    if (s->refs || s->primary) return;
    if (s->bits) { rt_free(s->bits); s->bits = 0; }
    if (!s->v7) return;
    if (s->zbuffer) { Surface *z = surf(s->zbuffer); s->zbuffer = 0; if (z) ddraw_surface_unref(z); }
    for (int i = 0; i < nsurf; i++) if (all_surfaces[i] == s) all_surfaces[i] = NULL;
    com_free(s->obj);
    free(s);
}
static void m_s_release(void)
{
    Surface *s = surf(rt_arg(0));
    uint32_t left = s->refs ? s->refs - 1 : 0;
    ddraw_surface_unref(s);
    ret(left, 0);
}

static void present_if_primary(Surface *s) { if (s->primary) screen_dirty(); }
void ddraw_surface_changed(Surface *s) { present_if_primary(s); }
/* AddAttachedSurface(this, otra): en DirectDraw 7, el z-buffer del destino de Direct3D (d3d.c lo usa). */
static void m_s_addattached(void)
{
    Surface *s = surf(rt_arg(0)), *o = surf(rt_arg(1));
    if (o && (o->caps & DDSCAPS_ZBUFFER)) { s->zbuffer = o->obj; o->refs++; }
    ret(DD_OK, 1);
}

static void m_s_lock(void)
{
    Surface *s = surf(rt_arg(0));
    uint32_t rect = rt_arg(1), d = rt_arg(2);
    uint32_t off = 0;
    if (rect) off = rd32(g_mem, rect + 4) * s->pitch + rd32(g_mem, rect) * px_bytes(s);
    put_desc(d, s->w, s->h, s->pitch, s->caps, s->bits + off, s->bpp);
    if (s->v7) put_surface_pf(d + 72, s);
    ret(DD_OK, 4);
}
static void m_s_unlock(void) { present_if_primary(surf(rt_arg(0))); ret(DD_OK, 1); }

/* Copia de src (rect sr) a dst (rect dr), con escala por vecino más cercano y color clave de origen opcional. */
static void blit(Surface *dst, int32_t dl, int32_t dt, int32_t dr, int32_t db, Surface *src, int32_t sl, int32_t st, int32_t sr, int32_t sb, int key)
{
    int32_t dw = dr - dl, dh = db - dt, sw = sr - sl, sh = sb - st;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    uint32_t n = px_bytes(dst);
    if (px_bytes(src) != n) {                          /* formatos distintos: DirectDraw tampoco convierte en Blt */
        static int said;
        if (!said++) fprintf(stderr, "recomp: Blt entre %u y %u bits: no se convierte\n", src->bpp, dst->bpp);
        return;
    }
    int same = src == dst || (src->bits == dst->bits);
    uint8_t *tmp = NULL;
    const uint8_t *sbase = g_mem + src->bits;
    if (same) {                                        /* copia a sí misma con solapamiento: pasar por un buffer */
        tmp = malloc((size_t)src->pitch * src->h);
        memcpy(tmp, sbase, (size_t)src->pitch * src->h);
        sbase = tmp;
    }
    for (int32_t y = 0; y < dh; y++) {
        int32_t ty = dt + y;
        if (ty < 0 || ty >= (int32_t)dst->h) continue;
        int32_t sy = st + (int32_t)((int64_t)y * sh / dh);
        if (sy < 0 || sy >= (int32_t)src->h) continue;
        const uint8_t *srow = sbase + (uint32_t)sy * src->pitch;
        uint8_t *drow = g_mem + dst->bits + (uint32_t)ty * dst->pitch;
        if (!key && dw == sw && dl >= 0 && dr <= (int32_t)dst->w && sl >= 0 && sr <= (int32_t)src->w) {
            memcpy(drow + (uint32_t)dl * n, srow + (uint32_t)sl * n, (size_t)dw * n);
            continue;
        }
        for (int32_t x = 0; x < dw; x++) {
            int32_t tx = dl + x;
            if (tx < 0 || tx >= (int32_t)dst->w) continue;
            int32_t sx = sl + (int32_t)((int64_t)x * sw / dw);
            if (sx < 0 || sx >= (int32_t)src->w) continue;
            uint32_t c = px_get(srow + (uint32_t)sx * n, n);
            if (key && c >= src->ck_lo && c <= src->ck_hi) continue;
            px_put(drow + (uint32_t)tx * n, n, c);
        }
    }
    free(tmp);
    present_if_primary(dst);
}

static void rect_or_full(uint32_t r, Surface *s, int32_t *l, int32_t *t, int32_t *rr, int32_t *b)
{
    if (r) { *l = (int32_t)rd32(g_mem, r); *t = (int32_t)rd32(g_mem, r + 4); *rr = (int32_t)rd32(g_mem, r + 8); *b = (int32_t)rd32(g_mem, r + 12); }
    else { *l = 0; *t = 0; *rr = (int32_t)s->w; *b = (int32_t)s->h; }
}

/* Blt(this, rectDst, src, rectSrc, flags, fx) */
static void m_s_blt(void)
{
    Surface *d = surf(rt_arg(0)), *s = surf(rt_arg(2));
    n_blt++; if (d->primary) n_blt_to_primary++; if (d->caps & DDSCAPS_BACKBUFFER) n_blt_to_back++;
    uint32_t flags = rt_arg(4), fx = rt_arg(5);
    int32_t dl, dt, dr, db;
    rect_or_full(rt_arg(1), d, &dl, &dt, &dr, &db);
    if (win_trace >= 2) fprintf(stderr, "Blt(%#x %ux%u%s, [%d,%d,%d,%d], src %#x%s, flags %#x, fx %#x)\n", d->obj, d->w, d->h, d->primary ? " primaria" : "",
              dl, dt, dr, db, rt_arg(2), s && s->primary ? " primaria" : "", flags, fx);
    if (flags & 0x400u) {                              /* DDBLT_COLORFILL: dwFillColor en DDBLTFX+80 */
        uint32_t n = px_bytes(d), c = fx ? rd32(g_mem, fx + 80) & px_mask(d) : 0;
        if (dl < 0) dl = 0;
        if (dt < 0) dt = 0;
        if (dr > (int32_t)d->w) dr = (int32_t)d->w;
        if (db > (int32_t)d->h) db = (int32_t)d->h;
        for (int32_t y = dt; y < db; y++) {
            if (dr <= dl) continue;
            uint8_t *row = g_mem + d->bits + (uint32_t)y * d->pitch + (uint32_t)dl * n;
            if (n == 1) memset(row, (int)c, (size_t)(dr - dl));
            else for (int32_t x = 0; x < dr - dl; x++) px_put(row + (uint32_t)x * n, n, c);
        }
        present_if_primary(d);
        ret(DD_OK, 5);
        return;
    }
    if (!s) { ret(DD_OK, 5); return; }
    int32_t sl, st, sr, sb;
    rect_or_full(rt_arg(3), s, &sl, &st, &sr, &sb);
    blit(d, dl, dt, dr, db, s, sl, st, sr, sb, (flags & 0x8000u) && s->has_ck);   /* DDBLT_KEYSRC */
    ret(DD_OK, 5);
}

/* BltFast(this, x, y, src, rectSrc, trans) */
static void m_s_bltfast(void)
{
    Surface *d = surf(rt_arg(0)), *s = surf(rt_arg(3));
    n_bltfast++; if (d->primary) n_blt_to_primary++; if (d->caps & DDSCAPS_BACKBUFFER) n_blt_to_back++;
    int32_t x = (int32_t)rt_arg(1), y = (int32_t)rt_arg(2);
    int32_t sl, st, sr, sb;
    rect_or_full(rt_arg(4), s, &sl, &st, &sr, &sb);
    blit(d, x, y, x + (sr - sl), y + (sb - st), s, sl, st, sr, sb, (rt_arg(5) & 1u) && s->has_ck);   /* DDBLTFAST_SRCCOLORKEY */
    ret(DD_OK, 5);
}

static void m_s_flip(void)
{
    n_flip++;
    Surface *s = surf(rt_arg(0));
    Surface *b = surf(s->back);
    if (s->primary && b) memcpy(g_mem + s->bits, g_mem + b->bits, (size_t)s->pitch * s->h);
    screen_dirty();
    screen_flush();
    win_pump_host_events();
    ret(DD_OK, 2);
}

static void m_s_getattached(void)
{
    Surface *s = surf(rt_arg(0));
    uint32_t out = rt_arg(2);
    if (!s->back) { wr32(g_mem, out, 0); ret(DDERR_NOTFOUND, 2); return; }
    surf(s->back)->refs++;
    wr32(g_mem, out, s->back);
    ret(DD_OK, 2);
}

static void apply_palette(Palette *p) { screen_set_palette(0, 256, &p->entries[0][0]); screen_dirty(); }

static void m_s_setpalette(void)
{
    Surface *s = surf(rt_arg(0));
    WIN_TRACE("DD SetPalette(%#x%s, %#x) desde %#x\n", s->obj, s->primary ? " primaria" : "", rt_arg(1), rd32(g_mem, cpu.esp));
    s->palette = rt_arg(1);
    if (s->palette) {
        Palette *p = (Palette *)com_ptr(s->palette);
        if (s->primary) { p->attached_primary = 1; apply_palette(p); }
    }
    ret(DD_OK, 1);
}
static void m_s_getpalette(void)
{
    Surface *s = surf(rt_arg(0));
    wr32(g_mem, rt_arg(1), s->palette);
    ret(s->palette ? DD_OK : 0x887600F3u, 1);           /* DDERR_NOPALETTEATTACHED */
}
static void m_s_setclipper(void) { surf(rt_arg(0))->clipper = rt_arg(1); ret(DD_OK, 1); }
static void m_s_getclipper(void) { wr32(g_mem, rt_arg(1), surf(rt_arg(0))->clipper); ret(DD_OK, 1); }

uint32_t gdi_surface_dc(uint32_t bits, uint32_t w, uint32_t h, uint32_t pitch, int is_primary);
void gdi_release_dc(uint32_t hdc);
static void m_s_getdc(void)
{
    Surface *s = surf(rt_arg(0));
    s->dc = gdi_surface_dc(s->bits, s->w, s->h, s->pitch, s->primary);
    WIN_TRACE("Surface::GetDC(%#x %ux%u%s) = %#x\n", s->obj, s->w, s->h, s->primary ? " primaria" : "", s->dc);
    wr32(g_mem, rt_arg(1), s->dc);
    ret(DD_OK, 1);
}
static void m_s_releasedc(void) { Surface *s = surf(rt_arg(0)); gdi_release_dc(rt_arg(1)); present_if_primary(s); ret(DD_OK, 1); }
static void m_s_ok0(void) { ret(DD_OK, 0); }
static void m_s_ok1(void) { ret(DD_OK, 1); }
static void m_s_ok2(void) { ret(DD_OK, 2); }
static void m_s_getdesc(void)
{
    Surface *s = surf(rt_arg(0));
    put_desc(rt_arg(1), s->w, s->h, s->pitch, s->caps, s->bits, s->bpp);
    if (s->v7) put_surface_pf(rt_arg(1) + 72, s);
    ret(DD_OK, 1);
}
static void m_s_getcaps(void)
{
    Surface *s = surf(rt_arg(0));
    if (s->v7) memset(g_mem + rt_arg(1), 0, 16);       /* DDSCAPS2 */
    wr32(g_mem, rt_arg(1), s->caps);
    ret(DD_OK, 1);
}
static void m_s_getpf(void)
{
    Surface *s = surf(rt_arg(0));
    if (s->v7) put_surface_pf(rt_arg(1), s); else put_pixel_format_bpp(rt_arg(1), s->bpp);
    ret(DD_OK, 1);
}
static void m_s_setck(void)
{
    Surface *s = surf(rt_arg(0));
    uint32_t flags = rt_arg(1), ck = rt_arg(2);
    if (flags & 8u) { s->has_ck = ck != 0; if (ck) { s->ck_lo = rd32(g_mem, ck) & px_mask(s); s->ck_hi = rd32(g_mem, ck + 4) & px_mask(s); } }   /* DDCKEY_SRCBLT */
    ret(DD_OK, 2);
}
static void m_s_getck(void)
{
    Surface *s = surf(rt_arg(0));
    uint32_t ck = rt_arg(2);
    if (!s->has_ck) { ret(0x887600B4u, 2); return; }     /* DDERR_NOCOLORKEY */
    wr32(g_mem, ck, s->ck_lo); wr32(g_mem, ck + 4, s->ck_hi);
    ret(DD_OK, 2);
}
static void m_s_getdd(void) { wr32(g_mem, rt_arg(1), surf(rt_arg(0))->v7 ? dd_obj7() : the_dd->obj1); ret(DD_OK, 1); }
static void m_s_ok4(void) { ret(DD_OK, 4); }
static void m_s_uniqueness(void) { wr32(g_mem, rt_arg(1), 1); ret(DD_OK, 1); }
static void m_s_getzero(void) { wr32(g_mem, rt_arg(1), 0); ret(DD_OK, 1); }

/* IDirectDraw7::GetDeviceIdentifier(this, DDDEVICEIDENTIFIER2*, flags): una placa genérica sin datos de fábrica. */
static void m_dd7_devid(void)
{
    uint32_t p = rt_arg(1);
    memset(g_mem + p, 0, 1072);
    win_put_str(p, "display", 512);
    win_put_str(p + 512, "Primary Display Driver", 512);
    ret(DD_OK, 2);
}

/* ---- paleta ---- */
static void m_p_qi(void) { ret(E_NOINTERFACE, 2); }
static void m_p_addref(void) { Palette *p = com_ptr(rt_arg(0)); ret(++p->refs, 0); }
static void m_p_release(void) { Palette *p = com_ptr(rt_arg(0)); if (p->refs) p->refs--; ret(p->refs, 0); }
static void m_p_getcaps(void) { wr32(g_mem, rt_arg(1), 0x4u | 0x40u); ret(DD_OK, 1); }
static void m_p_getentries(void)
{
    Palette *p = com_ptr(rt_arg(0));
    uint32_t start = rt_arg(2), n = rt_arg(3), out = rt_arg(4);
    for (uint32_t i = 0; i < n && start + i < 256; i++) memcpy(g_mem + out + 4 * i, p->entries[start + i], 4);
    ret(DD_OK, 4);
}
static void m_p_setentries(void)
{
    n_setentries++;
    Palette *p = com_ptr(rt_arg(0));
    uint32_t start = rt_arg(2), n = rt_arg(3), in = rt_arg(4);
    for (uint32_t i = 0; i < n && start + i < 256; i++) memcpy(p->entries[start + i], g_mem + in + 4 * i, 4);
    static uint32_t traced;
    if (win_trace >= 2 && traced++ < 60)
        fprintf(stderr, "SetEntries(%u..%u) desde %#x, [100]=%02x%02x%02x\n", start, start + n - 1, rd32(g_mem, cpu.esp),
                p->entries[100][0], p->entries[100][1], p->entries[100][2]);
    if (p->attached_primary) apply_palette(p);
    ret(DD_OK, 4);
}

/* ---- clipper ---- */
static void m_c_qi(void) { ret(E_NOINTERFACE, 2); }
static void m_c_addref(void) { Clipper *c = com_ptr(rt_arg(0)); ret(++c->refs, 0); }
static void m_c_release(void) { Clipper *c = com_ptr(rt_arg(0)); if (c->refs) c->refs--; ret(c->refs, 0); }
static void m_c_sethwnd(void) { Clipper *c = com_ptr(rt_arg(0)); c->hwnd = rt_arg(2); ret(DD_OK, 2); }
static void m_c_gethwnd(void) { Clipper *c = com_ptr(rt_arg(0)); wr32(g_mem, rt_arg(1), c->hwnd); ret(DD_OK, 1); }
static void m_c_getcliplist(void) { if (rt_arg(4)) wr32(g_mem, rt_arg(4), 0); ret(DD_OK, 3); }
static void m_c_setcliplist(void) { ret(DD_OK, 2); }
static void m_c_changed(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 0); ret(DD_OK, 1); }

/* ---- tablas ---- */
#define M(n, f) {n, f}
static void build_vtables(void)
{
    if (vt_dd1) return;
    const ComMethod dd[24] = {
        M("QueryInterface", m_dd_qi), M("AddRef", m_dd_addref), M("Release", m_dd_release), M("Compact", m_s_ok0),
        M("CreateClipper", m_dd_createclipper), M("CreatePalette", m_dd_createpalette), M("CreateSurface", m_dd_createsurface),
        M("DuplicateSurface", NULL), M("EnumDisplayModes", m_dd_enummodes), M("EnumSurfaces", NULL),
        M("FlipToGDISurface", m_dd_fliptogdi), M("GetCaps", m_dd_getcaps), M("GetDisplayMode", m_dd_getmode),
        M("GetFourCCCodes", NULL), M("GetGDISurface", NULL), M("GetMonitorFrequency", m_dd_monitor_freq),
        M("GetScanLine", m_dd_scanline), M("GetVerticalBlankStatus", m_dd_vbstatus), M("Initialize", m_s_ok1),
        M("RestoreDisplayMode", m_dd_restoremode), M("SetCooperativeLevel", m_dd_coop), M("SetDisplayMode", m_dd1_setmode),
        M("WaitForVerticalBlank", m_dd_vblank), M("GetAvailableVidMem", m_dd_vidmem)};
    vt_dd1 = com_vtable("IDirectDraw", dd, 23);
    ComMethod dd2[24];
    memcpy(dd2, dd, sizeof dd2);
    dd2[21].fn = m_dd2_setmode;
    vt_dd2 = com_vtable("IDirectDraw2", dd2, 24);
    const ComMethod sf[40] = {
        M("QueryInterface", m_s_qi), M("AddRef", m_s_addref), M("Release", m_s_release), M("AddAttachedSurface", m_s_addattached),
        M("AddOverlayDirtyRect", m_s_ok1), M("Blt", m_s_blt), M("BltBatch", NULL), M("BltFast", m_s_bltfast),
        M("DeleteAttachedSurface", m_s_ok2), M("EnumAttachedSurfaces", NULL), M("EnumOverlayZOrders", NULL), M("Flip", m_s_flip),
        M("GetAttachedSurface", m_s_getattached), M("GetBltStatus", m_s_ok1), M("GetCaps", m_s_getcaps), M("GetClipper", m_s_getclipper),
        M("GetColorKey", m_s_getck), M("GetDC", m_s_getdc), M("GetFlipStatus", m_s_ok1), M("GetOverlayPosition", NULL),
        M("GetPalette", m_s_getpalette), M("GetPixelFormat", m_s_getpf), M("GetSurfaceDesc", m_s_getdesc), M("Initialize", m_s_ok2),
        M("IsLost", m_s_ok0), M("Lock", m_s_lock), M("ReleaseDC", m_s_releasedc), M("Restore", m_s_ok0),
        M("SetClipper", m_s_setclipper), M("SetColorKey", m_s_setck), M("SetOverlayPosition", NULL), M("SetPalette", m_s_setpalette),
        M("Unlock", m_s_unlock), M("UpdateOverlay", NULL), M("UpdateOverlayDisplay", NULL), M("UpdateOverlayZOrder", NULL),
        M("GetDDInterface", m_s_getdd), M("PageLock", m_s_ok1), M("PageUnlock", m_s_ok1), M("SetSurfaceDesc", m_s_ok2)};
    vt_surf = com_vtable("IDirectDrawSurface3", sf, 40);
    /* DirectDraw 7: los mismos métodos con DDSURFACEDESC2/DDSCAPS2 (put_desc y m_s_getcaps miran el tamaño), más
     * los de la 4 (datos privados, unicidad) y la 7 (prioridad y LOD de texturas, que acá no hay). */
    ComMethod dd7[30];
    memcpy(dd7, dd2, sizeof dd2);
    dd7[6].fn = m_dd7_createsurface;
    dd7[8].fn = m_dd7_enummodes;
    const ComMethod dd7_more[6] = {M("GetSurfaceFromDC", NULL), M("RestoreAllSurfaces", m_s_ok0), M("TestCooperativeLevel", m_s_ok0),
                                   M("GetDeviceIdentifier", m_dd7_devid), M("StartModeTest", NULL), M("EvaluateMode", NULL)};
    memcpy(dd7 + 24, dd7_more, sizeof dd7_more);
    vt_dd7 = com_vtable("IDirectDraw7", dd7, 30);
    vt_dd4 = com_vtable("IDirectDraw4", dd7, 28);    /* la 4: la misma sin StartModeTest ni EvaluateMode */
    ComMethod sf7[49];
    memcpy(sf7, sf, sizeof sf);
    const ComMethod sf7_more[9] = {M("SetPrivateData", m_s_ok4), M("GetPrivateData", NULL), M("FreePrivateData", m_s_ok1),
                                   M("GetUniquenessValue", m_s_uniqueness), M("ChangeUniquenessValue", m_s_ok0),
                                   M("SetPriority", m_s_ok1), M("GetPriority", m_s_getzero), M("SetLOD", m_s_ok1), M("GetLOD", m_s_getzero)};
    memcpy(sf7 + 40, sf7_more, sizeof sf7_more);
    vt_surf7 = com_vtable("IDirectDrawSurface7", sf7, 49);
    const ComMethod pl[7] = {M("QueryInterface", m_p_qi), M("AddRef", m_p_addref), M("Release", m_p_release), M("GetCaps", m_p_getcaps),
                             M("GetEntries", m_p_getentries), M("Initialize", m_s_ok2), M("SetEntries", m_p_setentries)};
    vt_pal = com_vtable("IDirectDrawPalette", pl, 7);
    const ComMethod cl[9] = {M("QueryInterface", m_c_qi), M("AddRef", m_c_addref), M("Release", m_c_release), M("GetClipList", m_c_getcliplist),
                             M("GetHWnd", m_c_gethwnd), M("Initialize", m_s_ok2), M("IsClipListChanged", m_c_changed),
                             M("SetClipList", m_c_setcliplist), M("SetHWnd", m_c_sethwnd)};
    vt_clip = com_vtable("IDirectDrawClipper", cl, 9);
}

/* El objeto DirectDraw (uno solo, con sus tres caras: 1, 2 y 7). */
static void host_DirectDrawCreate_obj(void)
{
    build_vtables();
    if (!the_dd) {
        the_dd = calloc(1, sizeof *the_dd);
        the_dd->obj1 = com_new(vt_dd1, the_dd, 0);
        the_dd->obj2 = com_new(vt_dd2, the_dd, 0);
    }
    the_dd->refs++;
}

/* DirectDrawCreate(guid, lplpDD, outer) */
static void host_DirectDrawCreate(void)
{
    host_DirectDrawCreate_obj();
    wr32(g_mem, rt_arg(1), the_dd->obj1);
    fprintf(stderr, "recomp: DirectDrawCreate = %#x\n", the_dd->obj1);
    rt_ret_stdcall(DD_OK, 3);
}
/* DirectDrawEnumerateA(callback, ctx): un solo dispositivo, el primario. */
static void host_DirectDrawEnumerateA(void)
{
    uint32_t cb = rt_arg(0), ctx = rt_arg(1);
    Cpu s = cpu;
    uint32_t desc = win_static_alloc(32), name = win_static_alloc(32);
    win_put_str(desc, "Primary Display Driver", 32);
    win_put_str(name, "display", 32);
    uint32_t a[4] = {0, desc, name, ctx};
    win_call(cb, 4, a);
    cpu = s;
    rt_ret_stdcall(DD_OK, 2);
}

void ddraw_register(void)
{
    win_register_proc("ddraw.dll", "DirectDrawCreate", host_DirectDrawCreate);
    win_register_proc("ddraw.dll", "DirectDrawEnumerateA", host_DirectDrawEnumerateA);
}

/* PC Fútbol 2001: gfxwin.dll importa DirectDraw de forma estática (otros juegos lo piden con GetProcAddress). */
RT_IMPORT(DirectDrawCreate) { host_DirectDrawCreate(); }
RT_IMPORT(DirectDrawEnumerateA) { host_DirectDrawEnumerateA(); }
/* DirectDrawEnumerateExA(callback, ctx, flags): callback(guid, desc, name, ctx, hmonitor), un solo dispositivo. */
RT_IMPORT(DirectDrawEnumerateExA)
{
    uint32_t cb = rt_arg(0), ctx = rt_arg(1);
    Cpu s = cpu;
    uint32_t desc = win_static_alloc(32), name = win_static_alloc(32);
    win_put_str(desc, "Primary Display Driver", 32);
    win_put_str(name, "display", 32);
    uint32_t a[5] = {0, desc, name, ctx, 0};
    win_call(cb, 5, a);
    cpu = s;
    rt_ret_stdcall(DD_OK, 3);
}
/* DirectDrawCreateEx(guid, lplpDD, iid, outer): sólo IDirectDraw7 (es lo único que acepta el DirectX 7 real). */
RT_IMPORT(DirectDrawCreateEx)
{
    uint32_t out = rt_arg(1), iid = rt_arg(2);
    if (!com_guid_is(iid, IID_IDirectDraw7)) {
        char g[40];
        com_guid_text(iid, g);
        fprintf(stderr, "recomp: DirectDrawCreateEx(%s): sólo IDirectDraw7\n", g);
        wr32(g_mem, out, 0);
        rt_ret_stdcall(DDERR_INVALIDPARAMS, 4);
        return;
    }
    host_DirectDrawCreate_obj();
    wr32(g_mem, out, dd_obj7());
    fprintf(stderr, "recomp: DirectDrawCreateEx = %#x (IDirectDraw7)\n", the_dd->obj7);
    rt_ret_stdcall(DD_OK, 4);
}
