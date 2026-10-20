/* Direct3D 7 (QueryInterface de IDirectDraw7 por IID_IDirect3D7): el dispositivo "RGB Emulation", el rasterizador por
 * software que DirectX 7 trae aunque no haya placa 3D. PC Fútbol 2001 dibuja todo con él (su motor de fábrica, dcsfx
 * «Software MMX», busca justo ese GUID en EnumDevices).
 *
 * El dispositivo es un rasterizador de triángulos con las reglas de Direct3D 7: centros de píxel en coordenadas enteras,
 * regla arriba-izquierda, atributos con corrección de perspectiva (1/w), z-buffer, texturas en cualquier formato de
 * DirectDraw (máscaras o paleta de 8 bits) con muestreo puntual o bilineal, operaciones de etapas de textura, prueba de
 * alfa, color clave y mezcla. Los vértices llegan ya transformados (XYZRHW) o se transforman con mundo*vista*proyección
 * y se recortan contra el plano cercano. La luz por vértice todavía no: con LIGHTING y sin color difuso se usa el del
 * material. Los métodos que faltan paran con su nombre (com.c). */
#include "ddraw_int.h"

#include <math.h>

#define D3D_OK 0u
#define DDERR_UNSUPPORTED 0x80004001u
#define S_FALSE_ 1u

static const char *IID_IDirect3DRGBDevice = "a4665c60-2673-11cf-a31a-00aa00b93356";

/* Render states (d3dtypes.h de DirectX 7) que el rasterizador usa. */
enum {
    RS_ZENABLE = 7, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15, RS_SRCBLEND = 19,
    RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25,
    RS_ALPHABLENDENABLE = 27, RS_COLORKEYENABLE = 41, RS_TEXTUREFACTOR = 60, RS_LIGHTING = 137
};
/* Estados de etapa de textura. */
enum {
    TSS_COLOROP = 1, TSS_COLORARG1 = 2, TSS_COLORARG2 = 3, TSS_ALPHAOP = 4, TSS_ALPHAARG1 = 5, TSS_ALPHAARG2 = 6,
    TSS_TEXCOORDINDEX = 11, TSS_ADDRESSU = 13, TSS_ADDRESSV = 14, TSS_MAGFILTER = 16, TSS_MINFILTER = 17
};

typedef struct {
    uint32_t obj, refs, target;
    uint32_t rs[256];
    uint32_t tss[8][32];
    uint32_t tex[8];
    float world[16], view[16], proj[16];
    uint32_t vx, vy, vw, vh;
    float vminz, vmaxz;
    uint8_t material[68];
    float *zbuf;                 /* el z-buffer del destino, como float por píxel (tamaño del destino) */
    uint32_t zw, zh;
} Device;

static uint32_t d3d_obj, vt_device;

static void ret(uint32_t hr, int nargs) { rt_ret_stdcall(hr, nargs + 1); }
static float rdf(uint32_t a) { float f; memcpy(&f, g_mem + a, 4); return f; }

/* ---- IDirect3D7 ---- */
static void m_d3d_addref(void) { ret(2, 0); }
static void m_d3d_release(void) { ret(1, 0); }
static void m_d3d_qi(void) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), 0); ret(E_NOINTERFACE, 2); }

/* D3DPRIMCAPS (56 bytes) de un rasterizador por software de DirectX 7. */
static void put_primcaps(uint32_t p)
{
    memset(g_mem + p, 0, 56);
    wr32(g_mem, p, 56);
    wr32(g_mem, p + 4, 0x2u | 0x4u | 0x8u | 0x10u | 0x20u);   /* MiscCaps: masks z/planes, cull none/cw/ccw */
    wr32(g_mem, p + 8, 0x1u | 0x2u | 0x10u | 0x80u | 0x100u | 0x200u);   /* RasterCaps: dither, rop2, ztest, fog */
    wr32(g_mem, p + 12, 0xFFu);                                /* ZCmpCaps: todas */
    wr32(g_mem, p + 16, 0x1FFFu);                              /* SrcBlendCaps */
    wr32(g_mem, p + 20, 0x1FFFu);                              /* DestBlendCaps */
    wr32(g_mem, p + 24, 0xFFu);                                /* AlphaCmpCaps */
    wr32(g_mem, p + 28, 0x3FFFFu);                             /* ShadeCaps */
    wr32(g_mem, p + 32, 0x1u | 0x2u | 0x4u | 0x8u | 0x40u);    /* TextureCaps: perspective, pow2, alpha, transparency */
    wr32(g_mem, p + 36, 0x1u | 0x2u | 0x100u | 0x200u);        /* TextureFilterCaps: nearest, linear */
    wr32(g_mem, p + 40, 0xFFu);                                /* TextureBlendCaps (DX6) */
    wr32(g_mem, p + 44, 0x1u | 0x2u | 0x4u);                   /* TextureAddressCaps: wrap, mirror, clamp */
    wr32(g_mem, p + 48, 4);
    wr32(g_mem, p + 52, 4);
}

/* D3DDEVICEDESC7 (236 bytes) del dispositivo RGB. */
static void put_device_desc(uint32_t d)
{
    memset(g_mem + d, 0, 236);
    wr32(g_mem, d, 0x1u | 0x10u | 0x40u | 0x100u | 0x400u | 0x2000u);   /* DevCaps: TL en memoria del sistema, DrawPrimitive */
    put_primcaps(d + 4);                                       /* líneas */
    put_primcaps(d + 60);                                      /* triángulos */
    wr32(g_mem, d + 116, 0x400u | 0x200u | 0x100u);            /* DeviceRenderBitDepth: DDBD_16 | 24 | 32 */
    wr32(g_mem, d + 120, 0x400u | 0x200u | 0x100u);            /* DeviceZBufferBitDepth */
    wr32(g_mem, d + 124, 1); wr32(g_mem, d + 128, 1);          /* textura mínima */
    wr32(g_mem, d + 132, 2048); wr32(g_mem, d + 136, 2048);    /* y máxima */
    wr32(g_mem, d + 140, 2048);                                /* MaxTextureRepeat */
    wr32(g_mem, d + 144, 2048);                                /* MaxTextureAspectRatio */
    wr32(g_mem, d + 148, 1);                                   /* MaxAnisotropy */
    wr32(g_mem, d + 176, 0x8u);                                /* FVFCaps: 8 juegos de coordenadas */
    wr32(g_mem, d + 180, 0x3FFFFFu);                           /* TextureOpCaps */
    g_mem[d + 184] = 8; g_mem[d + 186] = 8;                    /* MaxTextureBlendStages, MaxSimultaneousTextures */
    wr32(g_mem, d + 188, 8);                                   /* MaxActiveLights */
    float maxw = 1e10f;
    memcpy(g_mem + d + 192, &maxw, 4);
    static const uint8_t rgb_guid[16] = {0x60, 0x5c, 0x66, 0xa4, 0x73, 0x26, 0xcf, 0x11,
                                         0xa3, 0x1a, 0x00, 0xaa, 0x00, 0xb9, 0x33, 0x56};
    memcpy(g_mem + d + 196, rgb_guid, 16);
}

/* EnumDevices(this, callback, ctx): callback(descripción, nombre, D3DDEVICEDESC7*, ctx). Sólo el RGB. */
static void m_d3d_enumdevices(void)
{
    uint32_t cb = rt_arg(1), ctx = rt_arg(2);
    Cpu s = cpu;
    uint32_t d = win_static_alloc(236), desc = win_static_alloc(64), name = win_static_alloc(32);
    win_put_str(desc, "Microsoft Direct3D RGB Software Emulation", 64);
    win_put_str(name, "RGB Emulation", 32);
    put_device_desc(d);
    uint32_t a[4] = {desc, name, d, ctx};
    win_call(cb, 4, a);
    cpu = s;
    ret(D3D_OK, 2);
}

static uint32_t device_vtable(void);
static void identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }

/* CreateDevice(this, rclsid, render target (IDirectDrawSurface7), out). */
static void m_d3d_createdevice(void)
{
    uint32_t clsid = rt_arg(1), target = rt_arg(2), out = rt_arg(3);
    char g[40];
    com_guid_text(clsid, g);
    if (!com_guid_is(clsid, IID_IDirect3DRGBDevice)) {
        fprintf(stderr, "recomp: IDirect3D7::CreateDevice(%s): sólo el RGB\n", g);
        if (out) wr32(g_mem, out, 0);
        ret(DDERR_UNSUPPORTED, 3);
        return;
    }
    Device *dv = calloc(1, sizeof *dv);
    Surface *t = surf(target);
    dv->refs = 1;
    dv->target = target;
    /* los valores por defecto de DirectX 7 */
    dv->rs[RS_ZENABLE] = t && t->zbuffer ? 1 : 0;
    dv->rs[8] = 3;                                             /* FILLMODE: sólido */
    dv->rs[RS_SHADEMODE] = 2;                                  /* gouraud */
    dv->rs[RS_ZWRITEENABLE] = 1;
    dv->rs[RS_SRCBLEND] = 2;                                   /* ONE */
    dv->rs[RS_DESTBLEND] = 1;                                  /* ZERO */
    dv->rs[RS_CULLMODE] = 3;                                   /* CCW */
    dv->rs[RS_ZFUNC] = 4;                                      /* LESSEQUAL */
    dv->rs[RS_ALPHAFUNC] = 8;                                  /* ALWAYS */
    dv->rs[RS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    dv->rs[136] = 1;                                           /* CLIPPING */
    dv->rs[RS_LIGHTING] = 1;
    dv->rs[141] = 1;                                           /* COLORVERTEX */
    for (int i = 0; i < 8; i++) {
        dv->tss[i][TSS_COLOROP] = i ? 1u : 4u;                 /* MODULATE en la 0, DISABLE en las demás */
        dv->tss[i][TSS_COLORARG1] = 2;                         /* TEXTURE */
        dv->tss[i][TSS_COLORARG2] = 1;                         /* CURRENT */
        dv->tss[i][TSS_ALPHAOP] = i ? 1u : 2u;                 /* SELECTARG1 en la 0 */
        dv->tss[i][TSS_ALPHAARG1] = 2;
        dv->tss[i][TSS_ALPHAARG2] = 1;
        dv->tss[i][TSS_TEXCOORDINDEX] = (uint32_t)i;
        dv->tss[i][TSS_ADDRESSU] = dv->tss[i][TSS_ADDRESSV] = 1;   /* WRAP */
        dv->tss[i][TSS_MAGFILTER] = dv->tss[i][TSS_MINFILTER] = 1; /* POINT */
    }
    identity(dv->world); identity(dv->view); identity(dv->proj);
    if (t) { dv->vw = t->w; dv->vh = t->h; }
    dv->vmaxz = 1.0f;
    dv->obj = com_new(device_vtable(), dv, 0);
    fprintf(stderr, "recomp: IDirect3D7::CreateDevice(RGB, destino %#x %ux%ux%u) = %#x\n", target, t ? t->w : 0,
            t ? t->h : 0, t ? t->bpp : 0, dv->obj);
    wr32(g_mem, out, dv->obj);
    ret(D3D_OK, 3);
}
static void m_d3d_createvb(void);                             /* más abajo, con los vertex buffers */
static void build_vb_vtable(void);

/* EnumZBufferFormats(this, clsid, callback, ctx): z de 16 bits (DDPF_ZBUFFER). */
static void m_d3d_enumz(void)
{
    uint32_t cb = rt_arg(2), ctx = rt_arg(3);
    Cpu s = cpu;
    uint32_t pf = win_static_alloc(32);
    memset(g_mem + pf, 0, 32);
    wr32(g_mem, pf, 32);
    wr32(g_mem, pf + 4, 0x400u);                               /* DDPF_ZBUFFER */
    wr32(g_mem, pf + 12, 16);                                  /* dwZBufferBitDepth */
    wr32(g_mem, pf + 20, 0xFFFFu);                             /* dwZBitMask */
    uint32_t a[2] = {pf, ctx};
    win_call(cb, 2, a);
    cpu = s;
    ret(D3D_OK, 3);
}
static void m_d3d_evict(void) { ret(D3D_OK, 0); }

uint32_t d3d7_object(void)
{
    if (!d3d_obj) {
        static const ComMethod m[8] = {{"QueryInterface", m_d3d_qi}, {"AddRef", m_d3d_addref}, {"Release", m_d3d_release},
                                       {"EnumDevices", m_d3d_enumdevices}, {"CreateDevice", m_d3d_createdevice},
                                       {"CreateVertexBuffer", m_d3d_createvb}, {"EnumZBufferFormats", m_d3d_enumz},
                                       {"EvictManagedTextures", m_d3d_evict}};
        d3d_obj = com_new(com_vtable("IDirect3D7", m, 8), NULL, 0);
        build_vb_vtable();
    }
    return d3d_obj;
}

/* ---- píxeles: de y hacia el formato de una superficie ---- */
static int mask_shift(uint32_t m) { int s = 0; if (!m) return 0; while (!(m & 1u)) { m >>= 1; s++; } return s; }
static uint32_t mask_max(uint32_t m) { return m ? m >> mask_shift(m) : 0; }

/* Un canal de 0..255 desde el valor crudo con su máscara (sin máscara: 255). */
static int chan(uint32_t raw, uint32_t m)
{
    if (!m) return 255;
    uint32_t mx = mask_max(m);
    return (int)(((raw & m) >> mask_shift(m)) * 255u / mx);
}
static uint32_t pack(const Surface *s, int r, int g, int b, int a)
{
    uint32_t v = 0;
    if (s->rmask) v |= ((uint32_t)r * mask_max(s->rmask) / 255u) << mask_shift(s->rmask);
    if (s->gmask) v |= ((uint32_t)g * mask_max(s->gmask) / 255u) << mask_shift(s->gmask);
    if (s->bmask) v |= ((uint32_t)b * mask_max(s->bmask) / 255u) << mask_shift(s->bmask);
    if (s->amask) v |= ((uint32_t)a * mask_max(s->amask) / 255u) << mask_shift(s->amask);
    return v;
}
static void unpack(const Surface *s, uint32_t raw, int *c)
{
    if (s->bpp == 8 && s->palette) {
        const Palette *p = (const Palette *)com_ptr(s->palette);
        c[0] = p->entries[raw & 255][0]; c[1] = p->entries[raw & 255][1]; c[2] = p->entries[raw & 255][2]; c[3] = 255;
        return;
    }
    c[0] = chan(raw, s->rmask); c[1] = chan(raw, s->gmask); c[2] = chan(raw, s->bmask);
    c[3] = s->amask ? chan(raw, s->amask) : 255;
}

/* ---- vértices ---- */
typedef struct {
    float x, y, z, w;            /* recorte: x, y, z, w; en pantalla: x, y, z, 1/w */
    float c[4], sp[4];           /* difuso y especular (r, g, b, a de 0 a 1) */
    float t[2][2];               /* dos juegos de coordenadas de textura */
} Vtx;

static void mul4(const float *a, const float *b, float *o)     /* o = a * b (filas de D3D) */
{
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, r, 64);
}
static void color_of(uint32_t argb, float *c)
{
    c[0] = (float)(argb >> 16 & 255) / 255.0f; c[1] = (float)(argb >> 8 & 255) / 255.0f;
    c[2] = (float)(argb & 255) / 255.0f; c[3] = (float)(argb >> 24) / 255.0f;
}

/* Tamaño de un vértice según su FVF. */
static uint32_t fvf_size(uint32_t fvf)
{
    uint32_t n = 0;
    switch (fvf & 0xE) {
    case 0x2: n = 12; break;                                   /* XYZ */
    case 0x4: n = 16; break;                                   /* XYZRHW */
    default: n = 12 + 4 * (((fvf & 0xE) - 0x4) / 2); break;    /* XYZB1..5 */
    }
    if (fvf & 0x10) n += 12;                                   /* NORMAL */
    if (fvf & 0x20) n += 4;                                    /* PSIZE */
    if (fvf & 0x40) n += 4;                                    /* DIFFUSE */
    if (fvf & 0x80) n += 4;                                    /* SPECULAR */
    uint32_t nt = fvf >> 8 & 15;
    for (uint32_t i = 0; i < nt; i++) {
        static const uint32_t sz[4] = {8, 12, 16, 4};
        n += sz[fvf >> (16 + 2 * i) & 3];
    }
    return n;
}

static void fetch(Device *dv, uint32_t fvf, uint32_t p, const float *mvp, Vtx *v)
{
    memset(v, 0, sizeof *v);
    uint32_t o = 0;
    float x = rdf(p), y = rdf(p + 4), z = rdf(p + 8);
    int rhw = (fvf & 0xE) == 0x4;
    if (rhw) {
        v->x = x; v->y = y; v->z = z; v->w = rdf(p + 12);
        o = 16;
    } else {
        v->x = x * mvp[0] + y * mvp[4] + z * mvp[8] + mvp[12];
        v->y = x * mvp[1] + y * mvp[5] + z * mvp[9] + mvp[13];
        v->z = x * mvp[2] + y * mvp[6] + z * mvp[10] + mvp[14];
        v->w = x * mvp[3] + y * mvp[7] + z * mvp[11] + mvp[15];
        o = (fvf & 0xE) == 0x2 ? 12 : 12 + 4 * (((fvf & 0xE) - 0x4) / 2);
    }
    if (fvf & 0x10) o += 12;
    if (fvf & 0x20) o += 4;
    if (fvf & 0x40) { color_of(rd32(g_mem, p + o), v->c); o += 4; }
    else if (!rhw && dv->rs[RS_LIGHTING]) {                    /* sin luz todavía: el difuso del material */
        for (int k = 0; k < 4; k++) { float f; memcpy(&f, dv->material + 4 * k, 4); v->c[k] = f; }
        if (v->c[0] == 0 && v->c[1] == 0 && v->c[2] == 0 && v->c[3] == 0) v->c[0] = v->c[1] = v->c[2] = v->c[3] = 1.0f;
    } else v->c[0] = v->c[1] = v->c[2] = v->c[3] = 1.0f;
    if (fvf & 0x80) { color_of(rd32(g_mem, p + o), v->sp); o += 4; }
    uint32_t nt = fvf >> 8 & 15;
    for (uint32_t i = 0; i < nt; i++) {
        uint32_t f = fvf >> (16 + 2 * i) & 3;
        static const uint32_t sz[4] = {8, 12, 16, 4};
        if (i < 2) { v->t[i][0] = rdf(p + o); v->t[i][1] = f == 3 ? 0.0f : rdf(p + o + 4); }
        o += sz[f];
    }
}

/* De recorte a pantalla (viewport de D3D: y hacia abajo, z en [minz, maxz]); en x, y, z, 1/w. */
static void project(Device *dv, Vtx *v)
{
    float iw = 1.0f / v->w;
    v->x = (float)dv->vx + (1.0f + v->x * iw) * (float)dv->vw * 0.5f;
    v->y = (float)dv->vy + (1.0f - v->y * iw) * (float)dv->vh * 0.5f;
    v->z = dv->vminz + v->z * iw * (dv->vmaxz - dv->vminz);
    v->w = iw;
}

/* ---- texturas ---- */
static int wrap_coord(int i, int n, uint32_t mode)
{
    switch (mode) {
    case 3: return i < 0 ? 0 : i >= n ? n - 1 : i;             /* CLAMP */
    case 2: {                                                  /* MIRROR */
        int p = 2 * n, k = ((i % p) + p) % p;
        return k < n ? k : p - 1 - k;
    }
    default: return ((i % n) + n) % n;                         /* WRAP */
    }
}
/* Texel (x, y) en RGBA 0..255; devuelve 0 si es el color clave y COLORKEYENABLE lo descarta. */
static int texel(Device *dv, Surface *t, int x, int y, int *c)
{
    uint32_t n = px_bytes(t);
    uint32_t raw = px_get(g_mem + t->bits + (uint32_t)y * t->pitch + (uint32_t)x * n, n);
    if (dv->rs[RS_COLORKEYENABLE] && t->has_ck && raw >= t->ck_lo && raw <= t->ck_hi) return 0;
    unpack(t, raw, c);
    return 1;
}
static int sample(Device *dv, int stage, float u, float v, int *c)
{
    Surface *t = surf(dv->tex[stage]);
    if (!t || !t->bits || !t->w || !t->h) { c[0] = c[1] = c[2] = c[3] = 255; return 1; }
    uint32_t au = dv->tss[stage][TSS_ADDRESSU], av = dv->tss[stage][TSS_ADDRESSV];
    int w = (int)t->w, h = (int)t->h;
    if (dv->tss[stage][TSS_MAGFILTER] == 2 || dv->tss[stage][TSS_MINFILTER] == 2) {      /* LINEAR */
        float fx = u * (float)w - 0.5f, fy = v * (float)h - 0.5f;
        int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
        float ax = fx - (float)x0, ay = fy - (float)y0;
        int q[4][4], ok = 0;
        static const int dx[4] = {0, 1, 0, 1}, dy[4] = {0, 0, 1, 1};
        float wsum = 0, acc[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; k++) {
            int ok_k = texel(dv, t, wrap_coord(x0 + dx[k], w, au), wrap_coord(y0 + dy[k], h, av), q[k]);
            float wk = (dx[k] ? ax : 1 - ax) * (dy[k] ? ay : 1 - ay);
            if (!ok_k) continue;
            ok = 1;
            wsum += wk;
            for (int i = 0; i < 4; i++) acc[i] += wk * (float)q[k][i];
        }
        if (!ok || wsum < 0.5f) return 0;                      /* mayoría de texels clave: transparente */
        for (int i = 0; i < 4; i++) c[i] = (int)(acc[i] / wsum + 0.5f);
        return 1;
    }
    int x = wrap_coord((int)floorf(u * (float)w), w, au), y = wrap_coord((int)floorf(v * (float)h), h, av);
    return texel(dv, t, x, y, c);
}

/* ---- etapas de textura ---- */
static void arg_value(Device *dv, uint32_t a, const float *cur, const float *diff, const float *spec, const float *tex, float *o)
{
    const float *src;
    float tf[4];
    switch (a & 0xF) {
    case 0: src = diff; break;
    case 2: src = tex; break;
    case 3: color_of(dv->rs[RS_TEXTUREFACTOR], tf); src = tf; break;
    case 4: src = spec; break;
    default: src = cur; break;
    }
    for (int i = 0; i < 4; i++) o[i] = src[i];
    if (a & 0x20) o[0] = o[1] = o[2] = src[3];                 /* ALPHAREPLICATE */
    if (a & 0x10) for (int i = 0; i < 4; i++) o[i] = 1.0f - o[i];   /* COMPLEMENT */
}
static float op1(uint32_t op, float a, float b, float da, float ta, float fa, float ca)
{
    switch (op) {
    case 2: return a;
    case 3: return b;
    case 4: return a * b;
    case 5: return fminf(1.0f, 2 * a * b);
    case 6: return fminf(1.0f, 4 * a * b);
    case 7: return fminf(1.0f, a + b);
    case 8: return fminf(1.0f, fmaxf(0.0f, a + b - 0.5f));
    case 9: return fminf(1.0f, fmaxf(0.0f, 2 * (a + b - 0.5f)));
    case 10: return fmaxf(0.0f, a - b);
    case 11: return fminf(1.0f, a + b - a * b);
    case 12: return a * da + b * (1 - da);
    case 13: return a * ta + b * (1 - ta);
    case 14: return a * fa + b * (1 - fa);
    case 15: return fminf(1.0f, a + b * (1 - ta));
    case 16: return a * ca + b * (1 - ca);
    default: return a;
    }
}
/* Combina las etapas: devuelve 0 si el píxel se descarta (texel con color clave). */
static int combine(Device *dv, const float *diff, const float *spec, const float uv[2][2], float *out)
{
    float cur[4] = {diff[0], diff[1], diff[2], diff[3]};
    for (int s = 0; s < 8; s++) {
        uint32_t cop = dv->tss[s][TSS_COLOROP];
        if (cop == 1) break;                                   /* DISABLE: termina la cadena */
        float tex[4] = {1, 1, 1, 1};
        if (dv->tex[s]) {
            int ci = (int)(dv->tss[s][TSS_TEXCOORDINDEX] & 1);
            int c[4];
            if (!sample(dv, s, uv[ci][0], uv[ci][1], c)) return 0;
            for (int i = 0; i < 4; i++) tex[i] = (float)c[i] / 255.0f;
        }
        float a1[4], a2[4], b1[4], b2[4], tf[4];
        color_of(dv->rs[RS_TEXTUREFACTOR], tf);
        arg_value(dv, dv->tss[s][TSS_COLORARG1], cur, diff, spec, tex, a1);
        arg_value(dv, dv->tss[s][TSS_COLORARG2], cur, diff, spec, tex, a2);
        arg_value(dv, dv->tss[s][TSS_ALPHAARG1], cur, diff, spec, tex, b1);
        arg_value(dv, dv->tss[s][TSS_ALPHAARG2], cur, diff, spec, tex, b2);
        float nc[4];
        for (int i = 0; i < 3; i++) nc[i] = op1(cop, a1[i], a2[i], diff[3], tex[3], tf[3], cur[3]);
        uint32_t aop = dv->tss[s][TSS_ALPHAOP];
        nc[3] = aop == 1 ? cur[3] : op1(aop, b1[3], b2[3], diff[3], tex[3], tf[3], cur[3]);
        memcpy(cur, nc, sizeof cur);
    }
    memcpy(out, cur, sizeof cur);
    return 1;
}

static int cmp(uint32_t f, float a, float b)
{
    switch (f) {
    case 1: return 0;
    case 2: return a < b;
    case 3: return a == b;
    case 4: return a <= b;
    case 5: return a > b;
    case 6: return a != b;
    case 7: return a >= b;
    default: return 1;
    }
}
static float blend_factor(uint32_t f, int ch, const float *src, const float *dst)
{
    switch (f) {
    case 1: return 0;
    case 2: return 1;
    case 3: return src[ch];
    case 4: return 1 - src[ch];
    case 5: case 12: return src[3];
    case 6: case 13: return 1 - src[3];
    case 7: return dst[3];
    case 8: return 1 - dst[3];
    case 9: return dst[ch];
    case 10: return 1 - dst[ch];
    case 11: return ch == 3 ? 1 : fminf(src[3], 1 - dst[3]);
    default: return 1;
    }
}

/* Un píxel: prueba z, combina, prueba alfa, mezcla y escribe. */
static void shade(Device *dv, Surface *t, int x, int y, float z, const float *diff, const float *spec, const float uv[2][2])
{
    int zon = dv->rs[RS_ZENABLE] && dv->zbuf;
    float *zp = zon ? &dv->zbuf[(uint32_t)y * dv->zw + (uint32_t)x] : NULL;
    if (zon && !cmp(dv->rs[RS_ZFUNC], z, *zp)) return;
    float col[4];
    if (!combine(dv, diff, spec, uv, col)) return;
    if (dv->rs[RS_ALPHATESTENABLE] && !cmp(dv->rs[RS_ALPHAFUNC], col[3] * 255.0f, (float)(dv->rs[RS_ALPHAREF] & 255))) return;
    uint32_t n = px_bytes(t);
    uint8_t *p = g_mem + t->bits + (uint32_t)y * t->pitch + (uint32_t)x * n;
    if (dv->rs[RS_ALPHABLENDENABLE]) {
        int dc[4];
        unpack(t, px_get(p, n), dc);
        float dst[4] = {(float)dc[0] / 255.0f, (float)dc[1] / 255.0f, (float)dc[2] / 255.0f, (float)dc[3] / 255.0f};
        uint32_t sb = dv->rs[RS_SRCBLEND], db = dv->rs[RS_DESTBLEND];
        if (sb == 12) db = 6;                                  /* BOTHSRCALPHA */
        if (sb == 13) db = 5;                                  /* BOTHINVSRCALPHA */
        float o[4];
        for (int i = 0; i < 4; i++) o[i] = fminf(1.0f, col[i] * blend_factor(sb, i, col, dst) + dst[i] * blend_factor(db, i, col, dst));
        memcpy(col, o, sizeof o);
    }
    px_put(p, n, pack(t, (int)(col[0] * 255.0f + 0.5f), (int)(col[1] * 255.0f + 0.5f), (int)(col[2] * 255.0f + 0.5f),
                      (int)(col[3] * 255.0f + 0.5f)));
    if (zon && dv->rs[RS_ZWRITEENABLE]) *zp = z;
}

static void ensure_zbuf(Device *dv, Surface *t)
{
    if (dv->zbuf && dv->zw == t->w && dv->zh == t->h) return;
    free(dv->zbuf);
    dv->zw = t->w; dv->zh = t->h;
    dv->zbuf = malloc(sizeof(float) * dv->zw * dv->zh);
    for (uint32_t i = 0; i < dv->zw * dv->zh; i++) dv->zbuf[i] = 1.0f;
}

/* Triángulo en pantalla (x, y, z, 1/w): funciones de borde sobre la caja, centros de píxel enteros (D3D7), regla
 * arriba-izquierda, atributos con corrección de perspectiva. */
static void raster(Device *dv, Surface *t, const Vtx *a, const Vtx *b, const Vtx *c)
{
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (area == 0.0f || !isfinite(area)) return;
    /* cull: en pantalla (y hacia abajo), area > 0 es horario */
    uint32_t cull = dv->rs[RS_CULLMODE];
    if ((cull == 2 && area > 0) || (cull == 3 && area < 0)) return;
    if (area < 0) { const Vtx *tmp = b; b = c; c = tmp; area = -area; }
    int x0 = (int)ceilf(fminf(a->x, fminf(b->x, c->x))), x1 = (int)floorf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int y0 = (int)ceilf(fminf(a->y, fminf(b->y, c->y))), y1 = (int)floorf(fmaxf(a->y, fmaxf(b->y, c->y)));
    int cx0 = (int)dv->vx, cy0 = (int)dv->vy, cx1 = (int)(dv->vx + dv->vw) - 1, cy1 = (int)(dv->vy + dv->vh) - 1;
    if (cx1 >= (int)t->w) cx1 = (int)t->w - 1;
    if (cy1 >= (int)t->h) cy1 = (int)t->h - 1;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 > x1 || y0 > y1) return;
    const Vtx *v[3] = {a, b, c};
    int flat = dv->rs[RS_SHADEMODE] == 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float px = (float)x, py = (float)y, w[3];
            int inside = 1;
            for (int k = 0; k < 3; k++) {
                const Vtx *p = v[(k + 1) % 3], *q = v[(k + 2) % 3];
                float e = (q->x - p->x) * (py - p->y) - (q->y - p->y) * (px - p->x);
                /* regla arriba-izquierda: un borde superior o izquierdo incluye sus píxeles */
                int topleft = (q->y == p->y && q->x < p->x) || q->y > p->y;
                if (e < 0 || (e == 0 && !topleft)) { inside = 0; break; }
                w[k] = e / area;
            }
            if (!inside) continue;
            float z = w[0] * a->z + w[1] * b->z + w[2] * c->z;
            float iw = w[0] * a->w + w[1] * b->w + w[2] * c->w;
            float pw[3];
            for (int k = 0; k < 3; k++) pw[k] = w[k] * v[k]->w / (iw != 0.0f ? iw : 1.0f);
            float diff[4], spec[4], uv[2][2];
            for (int i = 0; i < 4; i++) {
                diff[i] = flat ? a->c[i] : pw[0] * a->c[i] + pw[1] * b->c[i] + pw[2] * c->c[i];
                spec[i] = flat ? a->sp[i] : pw[0] * a->sp[i] + pw[1] * b->sp[i] + pw[2] * c->sp[i];
            }
            for (int s = 0; s < 2; s++)
                for (int i = 0; i < 2; i++) uv[s][i] = pw[0] * a->t[s][i] + pw[1] * b->t[s][i] + pw[2] * c->t[s][i];
            shade(dv, t, x, y, z, diff, spec, uv);
        }
}

/* Recorte contra el plano cercano (z >= 0 en el espacio de recorte de D3D) y w > 0; después proyecta y rasteriza. */
static void lerp_vtx(const Vtx *a, const Vtx *b, float f, Vtx *o)
{
    o->x = a->x + (b->x - a->x) * f; o->y = a->y + (b->y - a->y) * f;
    o->z = a->z + (b->z - a->z) * f; o->w = a->w + (b->w - a->w) * f;
    for (int i = 0; i < 4; i++) { o->c[i] = a->c[i] + (b->c[i] - a->c[i]) * f; o->sp[i] = a->sp[i] + (b->sp[i] - a->sp[i]) * f; }
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 2; i++) o->t[s][i] = a->t[s][i] + (b->t[s][i] - a->t[s][i]) * f;
}
static void triangle(Device *dv, Surface *t, const Vtx *a, const Vtx *b, const Vtx *c, int pretransformed)
{
    if (pretransformed) { raster(dv, t, a, b, c); return; }
    Vtx in[4] = {*a, *b, *c}, out[4];
    int n = 0;
    for (int i = 0; i < 3; i++) {
        const Vtx *p = &in[i], *q = &in[(i + 1) % 3];
        int pin = p->z >= 0 && p->w > 0, qin = q->z >= 0 && q->w > 0;
        if (pin) out[n++] = *p;
        if (pin != qin) {
            float f = p->z / (p->z - q->z);
            if (!isfinite(f)) continue;
            lerp_vtx(p, q, f, &out[n++]);
        }
    }
    if (n < 3) return;
    for (int i = 0; i < n; i++) { if (out[i].w <= 0) return; project(dv, &out[i]); }
    raster(dv, t, &out[0], &out[1], &out[2]);
    if (n == 4) raster(dv, t, &out[0], &out[2], &out[3]);
}

/* Dibuja count vértices (o índices) de tipo type. idx = 0: en orden. */
static void draw(Device *dv, uint32_t type, uint32_t fvf, uint32_t verts, uint32_t nverts, uint32_t idx, uint32_t count)
{
    Surface *t = surf(dv->target);
    if (!t || !t->bits) return;
    if (dv->rs[RS_ZENABLE]) ensure_zbuf(dv, t);
    uint32_t stride = fvf_size(fvf);
    float mvp[16], wv[16];
    mul4(dv->world, dv->view, wv);
    mul4(wv, dv->proj, mvp);
    int pre = (fvf & 0xE) == 0x4;
    Vtx *vs = malloc(sizeof(Vtx) * (nverts ? nverts : 1));
    for (uint32_t i = 0; i < nverts; i++) fetch(dv, fvf, verts + i * stride, mvp, &vs[i]);
#define V(k) (&vs[idx ? rd16(g_mem, idx + 2 * (k)) % nverts : (k)])
    switch (type) {
    case 4:                                                    /* TRIANGLELIST */
        for (uint32_t i = 0; i + 2 < count; i += 3) triangle(dv, t, V(i), V(i + 1), V(i + 2), pre);
        break;
    case 5:                                                    /* TRIANGLESTRIP */
        for (uint32_t i = 0; i + 2 < count; i++)
            if (i & 1) triangle(dv, t, V(i + 1), V(i), V(i + 2), pre);
            else triangle(dv, t, V(i), V(i + 1), V(i + 2), pre);
        break;
    case 6:                                                    /* TRIANGLEFAN */
        for (uint32_t i = 1; i + 1 < count; i++) triangle(dv, t, V(0), V(i), V(i + 1), pre);
        break;
    default: {                                                 /* puntos y líneas: el primer vértice, píxel a píxel */
        static int said;
        if (!said++) fprintf(stderr, "recomp: Direct3D: primitiva %u (puntos/líneas) aproximada\n", type);
        uint32_t step = type == 1 ? 1 : type == 3 ? 1 : 2;
        for (uint32_t i = 0; i + (type == 1 ? 0 : 1) < count; i += step) {
            Vtx p = *V(i), q = type == 1 ? p : *V(i + 1);
            if (!pre) { if (p.w <= 0 || q.w <= 0) continue; project(dv, &p); project(dv, &q); }
            int steps = (int)fmaxf(fabsf(q.x - p.x), fabsf(q.y - p.y)) + 1;
            for (int k = 0; k < steps; k++) {
                float f = steps > 1 ? (float)k / (float)(steps - 1) : 0;
                int x = (int)(p.x + (q.x - p.x) * f), y = (int)(p.y + (q.y - p.y) * f);
                if (x < 0 || y < 0 || x >= (int)t->w || y >= (int)t->h) continue;
                float uv[2][2] = {{p.t[0][0], p.t[0][1]}, {p.t[1][0], p.t[1][1]}};
                shade(dv, t, x, y, p.z, p.c, p.sp, uv);
            }
        }
    }
    }
#undef V
    free(vs);
}

/* ---- vertex buffers (IDirect3DVertexBuffer7): los vértices viven en la memoria del juego, que los llena con Lock ---- */
typedef struct { uint32_t obj, refs, fvf, n, stride, data; } VBuf;
static uint32_t vt_vb;
static VBuf *vbuf(uint32_t obj) { return obj ? (VBuf *)com_ptr(obj) : NULL; }

/* Escribe un vértice ya en pantalla (x, y, z, 1/w) en el formato fvf (lo que deja ProcessVertices). */
static uint32_t argb_of(const float *c)
{
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        float f = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        uint32_t v = (uint32_t)(f * 255.0f + 0.5f);
        r |= v << (i == 3 ? 24 : 16 - 8 * i);
    }
    return r;
}
static void put_vtx(uint32_t fvf, uint32_t p, const Vtx *v)
{
    uint32_t o;
    memcpy(g_mem + p, &v->x, 4); memcpy(g_mem + p + 4, &v->y, 4); memcpy(g_mem + p + 8, &v->z, 4);
    if ((fvf & 0xE) == 0x4) { memcpy(g_mem + p + 12, &v->w, 4); o = 16; }
    else o = (fvf & 0xE) == 0x2 ? 12 : 12 + 4 * (((fvf & 0xE) - 0x4) / 2);
    if (fvf & 0x10) o += 12;
    if (fvf & 0x20) o += 4;
    if (fvf & 0x40) { wr32(g_mem, p + o, argb_of(v->c)); o += 4; }
    if (fvf & 0x80) { wr32(g_mem, p + o, argb_of(v->sp)); o += 4; }
    uint32_t nt = fvf >> 8 & 15;
    for (uint32_t i = 0; i < nt; i++) {
        uint32_t f = fvf >> (16 + 2 * i) & 3;
        static const uint32_t sz[4] = {8, 12, 16, 4};
        if (i < 2) { memcpy(g_mem + p + o, &v->t[i][0], 4); if (f != 3) memcpy(g_mem + p + o + 4, &v->t[i][1], 4); }
        o += sz[f];
    }
}

/* CreateVertexBuffer(this, D3DVERTEXBUFFERDESC*, out, flags): {dwSize, dwCaps, dwFVF, dwNumVertices}. */
static void m_d3d_createvb(void)
{
    uint32_t d = rt_arg(1), out = rt_arg(2);
    VBuf *b = calloc(1, sizeof *b);
    b->refs = 1;
    b->fvf = rd32(g_mem, d + 8);
    b->n = rd32(g_mem, d + 12);
    b->stride = fvf_size(b->fvf);
    b->data = rt_alloc_zero(b->stride * (b->n ? b->n : 1));
    b->obj = com_new(vt_vb, b, 0);
    wr32(g_mem, out, b->obj);
    ret(D3D_OK, 3);
}
static void m_vb_qi(void) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), 0); ret(E_NOINTERFACE, 2); }
static void m_vb_addref(void) { ret(++vbuf(rt_arg(0))->refs, 0); }
static void m_vb_release(void)
{
    VBuf *b = vbuf(rt_arg(0));
    if (b->refs) b->refs--;
    uint32_t left = b->refs;
    if (!left) { rt_free(b->data); com_free(b->obj); free(b); }
    ret(left, 0);
}
static void m_vb_lock(void)                                    /* Lock(flags, **data, *size) */
{
    VBuf *b = vbuf(rt_arg(0));
    if (rt_arg(2)) wr32(g_mem, rt_arg(2), b->data);
    if (rt_arg(3)) wr32(g_mem, rt_arg(3), b->stride * b->n);
    ret(D3D_OK, 3);
}
static void m_vb_unlock(void) { ret(D3D_OK, 0); }
static void m_vb_desc(void)
{
    VBuf *b = vbuf(rt_arg(0));
    uint32_t d = rt_arg(1);
    wr32(g_mem, d, 16); wr32(g_mem, d + 4, 0); wr32(g_mem, d + 8, b->fvf); wr32(g_mem, d + 12, b->n);
    ret(D3D_OK, 1);
}
static void m_vb_optimize(void) { ret(D3D_OK, 2); }
/* ProcessVertices(this = destino, op, destIndex, count, srcVB, srcIndex, device, flags): transforma con las matrices y
 * el viewport del dispositivo y deja los vértices en pantalla en el formato del destino. */
static void m_vb_process(void)
{
    VBuf *dst = vbuf(rt_arg(0)), *src = vbuf(rt_arg(4));
    uint32_t di = rt_arg(2), count = rt_arg(3), si = rt_arg(5);
    Device *dv = (Device *)com_ptr(rt_arg(6));
    if (!dst || !src || !dv) { ret(D3D_OK, 7); return; }
    float mvp[16], wv[16];
    mul4(dv->world, dv->view, wv);
    mul4(wv, dv->proj, mvp);
    int pre = (src->fvf & 0xE) == 0x4;
    for (uint32_t i = 0; i < count && si + i < src->n && di + i < dst->n; i++) {
        Vtx v;
        fetch(dv, src->fvf, src->data + (si + i) * src->stride, mvp, &v);
        if (!pre) { if (v.w > 0) project(dv, &v); else v.w = 0; }
        put_vtx(dst->fvf, dst->data + (di + i) * dst->stride, &v);
    }
    ret(D3D_OK, 7);
}
static void build_vb_vtable(void)
{
    if (vt_vb) return;
    static const ComMethod m[9] = {{"QueryInterface", m_vb_qi}, {"AddRef", m_vb_addref}, {"Release", m_vb_release},
                                   {"Lock", m_vb_lock}, {"Unlock", m_vb_unlock}, {"ProcessVertices", m_vb_process},
                                   {"GetVertexBufferDesc", m_vb_desc}, {"Optimize", m_vb_optimize},
                                   {"ProcessVerticesStrided", NULL}};
    vt_vb = com_vtable("IDirect3DVertexBuffer7", m, 9);
}


/* ---- IDirect3DDevice7 ---- */
static Device *dev(void) { return (Device *)com_ptr(rt_arg(0)); }
static void m_dev_addref(void) { ret(++dev()->refs, 0); }
static void m_dev_release(void) { Device *d = dev(); if (d->refs) d->refs--; ret(d->refs, 0); }
static void m_dev_qi(void) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), 0); ret(E_NOINTERFACE, 2); }
static void m_dev_getcaps(void) { put_device_desc(rt_arg(1)); ret(D3D_OK, 1); }

/* EnumTextureFormats(this, callback, ctx): callback(DDPIXELFORMAT*, ctx), los del rasterizador RGB. */
static void m_dev_enumtex(void)
{
    uint32_t cb = rt_arg(1), ctx = rt_arg(2);
    static const uint32_t f[][6] = {   /* flags, bits, r, g, b, a */
        {0x40, 16, 0xF800, 0x07E0, 0x001F, 0},                     /* 5-6-5 */
        {0x40, 16, 0x7C00, 0x03E0, 0x001F, 0},                     /* 5-5-5 */
        {0x41, 16, 0x7C00, 0x03E0, 0x001F, 0x8000},                /* 1-5-5-5 */
        {0x41, 16, 0x0F00, 0x00F0, 0x000F, 0xF000},                /* 4-4-4-4 */
        {0x40, 32, 0xFF0000, 0x00FF00, 0x0000FF, 0},               /* X8R8G8B8 */
        {0x41, 32, 0xFF0000, 0x00FF00, 0x0000FF, 0xFF000000u},     /* A8R8G8B8 */
        {0x60, 8, 0, 0, 0, 0}};                                    /* paleta de 8 bits */
    Cpu s = cpu;
    uint32_t pf = win_static_alloc(32);
    for (unsigned i = 0; i < sizeof f / sizeof *f; i++) {
        memset(g_mem + pf, 0, 32);
        wr32(g_mem, pf, 32);
        wr32(g_mem, pf + 4, f[i][0]);
        wr32(g_mem, pf + 12, f[i][1]);
        wr32(g_mem, pf + 16, f[i][2]);
        wr32(g_mem, pf + 20, f[i][3]);
        wr32(g_mem, pf + 24, f[i][4]);
        wr32(g_mem, pf + 28, f[i][5]);
        uint32_t a[2] = {pf, ctx};
        if (!win_call(cb, 2, a)) break;
    }
    cpu = s;
    ret(D3D_OK, 2);
}
static void m_dev_beginscene(void) { ret(D3D_OK, 0); }
static void m_dev_endscene(void) { Surface *t = surf(dev()->target); if (t) ddraw_surface_changed(t); ret(D3D_OK, 0); }
static void m_dev_getd3d(void) { wr32(g_mem, rt_arg(1), d3d7_object()); ret(D3D_OK, 1); }
static void m_dev_setrt(void) { Device *d = dev(); d->target = rt_arg(1); ret(D3D_OK, 2); }
static void m_dev_getrt(void) { Device *d = dev(); Surface *t = surf(d->target); if (t) t->refs++; wr32(g_mem, rt_arg(1), d->target); ret(D3D_OK, 1); }

/* Clear(this, count, rects, flags, color, z, stencil) */
static void m_dev_clear(void)
{
    Device *dv = dev();
    uint32_t count = rt_arg(1), rects = rt_arg(2), flags = rt_arg(3), color = rt_arg(4);
    float z = rdf(cpu.esp + 4 + 24);
    Surface *t = surf(dv->target);
    if (!t || !t->bits) { ret(D3D_OK, 6); return; }
    if (flags & 2) ensure_zbuf(dv, t);
    uint32_t n = px_bytes(t);
    uint32_t px = pack(t, (int)(color >> 16 & 255), (int)(color >> 8 & 255), (int)(color & 255), (int)(color >> 24));
    for (uint32_t r = 0; r < (count ? count : 1); r++) {
        int x0 = (int)dv->vx, y0 = (int)dv->vy, x1 = (int)(dv->vx + dv->vw), y1 = (int)(dv->vy + dv->vh);
        if (count && rects) {
            x0 = (int)rd32(g_mem, rects + 16 * r); y0 = (int)rd32(g_mem, rects + 16 * r + 4);
            x1 = (int)rd32(g_mem, rects + 16 * r + 8); y1 = (int)rd32(g_mem, rects + 16 * r + 12);
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int)t->w) x1 = (int)t->w;
        if (y1 > (int)t->h) y1 = (int)t->h;
        for (int y = y0; y < y1; y++) {
            if (flags & 1)
                for (int x = x0; x < x1; x++) px_put(g_mem + t->bits + (uint32_t)y * t->pitch + (uint32_t)x * n, n, px);
            if ((flags & 2) && dv->zbuf)
                for (int x = x0; x < x1; x++) dv->zbuf[(uint32_t)y * dv->zw + (uint32_t)x] = z;
        }
    }
    ret(D3D_OK, 6);
}

static float *transform_slot(Device *d, uint32_t state)
{
    return state == 1 || state == 256 ? d->world : state == 2 ? d->view : state == 3 ? d->proj : NULL;
}
static void m_dev_settransform(void)
{
    float *m = transform_slot(dev(), rt_arg(1));
    if (m) memcpy(m, g_mem + rt_arg(2), 64);
    ret(D3D_OK, 2);
}
static void m_dev_gettransform(void)
{
    float *m = transform_slot(dev(), rt_arg(1));
    if (m) memcpy(g_mem + rt_arg(2), m, 64);
    ret(D3D_OK, 2);
}
static void m_dev_multiplytransform(void)
{
    float *m = transform_slot(dev(), rt_arg(1));
    if (m) { float b[16]; memcpy(b, g_mem + rt_arg(2), 64); mul4(b, m, m); }
    ret(D3D_OK, 2);
}
static void m_dev_setviewport(void)
{
    Device *d = dev();
    uint32_t p = rt_arg(1);
    d->vx = rd32(g_mem, p); d->vy = rd32(g_mem, p + 4); d->vw = rd32(g_mem, p + 8); d->vh = rd32(g_mem, p + 12);
    d->vminz = rdf(p + 16); d->vmaxz = rdf(p + 20);
    ret(D3D_OK, 1);
}
static void m_dev_getviewport(void)
{
    Device *d = dev();
    uint32_t p = rt_arg(1);
    wr32(g_mem, p, d->vx); wr32(g_mem, p + 4, d->vy); wr32(g_mem, p + 8, d->vw); wr32(g_mem, p + 12, d->vh);
    memcpy(g_mem + p + 16, &d->vminz, 4); memcpy(g_mem + p + 20, &d->vmaxz, 4);
    ret(D3D_OK, 1);
}
static void m_dev_setmaterial(void) { memcpy(dev()->material, g_mem + rt_arg(1), 68); ret(D3D_OK, 1); }
static void m_dev_getmaterial(void) { memcpy(g_mem + rt_arg(1), dev()->material, 68); ret(D3D_OK, 1); }
static void m_dev_ok1(void) { ret(D3D_OK, 1); }
static void m_dev_ok2(void) { ret(D3D_OK, 2); }
static void m_dev_ok0(void) { ret(D3D_OK, 0); }
static void m_dev_setrs(void)
{
    uint32_t st = rt_arg(1);
    if (st < 256) dev()->rs[st] = rt_arg(2);
    ret(D3D_OK, 2);
}
static void m_dev_getrs(void) { uint32_t st = rt_arg(1); wr32(g_mem, rt_arg(2), st < 256 ? dev()->rs[st] : 0); ret(D3D_OK, 2); }

/* DrawPrimitiveVB(this, type, vb, start, count, flags) y DrawIndexedPrimitiveVB(this, type, vb, start, nverts, indices,
 * nindices, flags): lo mismo desde un vertex buffer. */
static void m_dev_drawprimvb(void)
{
    VBuf *b = vbuf(rt_arg(2));
    if (b) draw(dev(), rt_arg(1), b->fvf, b->data + rt_arg(3) * b->stride, rt_arg(4), 0, rt_arg(4));
    ret(D3D_OK, 5);
}
static void m_dev_drawindexedvb(void)
{
    VBuf *b = vbuf(rt_arg(2));
    if (b) draw(dev(), rt_arg(1), b->fvf, b->data + rt_arg(3) * b->stride, rt_arg(4), rt_arg(5), rt_arg(6));
    ret(D3D_OK, 7);
}

/* DrawPrimitive(this, type, fvf, vertices, count, flags) */
static void m_dev_drawprim(void)
{
    draw(dev(), rt_arg(1), rt_arg(2), rt_arg(3), rt_arg(4), 0, rt_arg(4));
    ret(D3D_OK, 5);
}
/* DrawIndexedPrimitive(this, type, fvf, vertices, nverts, indices, nindices, flags) */
static void m_dev_drawindexed(void)
{
    draw(dev(), rt_arg(1), rt_arg(2), rt_arg(3), rt_arg(4), rt_arg(5), rt_arg(6));
    ret(D3D_OK, 7);
}
static void m_dev_gettexture(void)
{
    Device *d = dev();
    uint32_t st = rt_arg(1) & 7;
    Surface *t = surf(d->tex[st]);
    if (t) t->refs++;
    wr32(g_mem, rt_arg(2), d->tex[st]);
    ret(D3D_OK, 2);
}
/* SetTexture toma una referencia de la textura nueva y suelta la anterior (como Direct3D 7). */
static void m_dev_settexture(void)
{
    Device *d = dev();
    uint32_t st = rt_arg(1) & 7, t = rt_arg(2);
    Surface *nt = surf(t), *old = surf(d->tex[st]);
    if (nt) nt->refs++;
    d->tex[st] = t;
    if (old) ddraw_surface_unref(old);
    ret(D3D_OK, 2);
}
static void m_dev_gettss(void) { wr32(g_mem, rt_arg(3), dev()->tss[rt_arg(1) & 7][rt_arg(2) & 31]); ret(D3D_OK, 3); }
static void m_dev_settss(void) { dev()->tss[rt_arg(1) & 7][rt_arg(2) & 31] = rt_arg(3); ret(D3D_OK, 3); }
static void m_dev_validate(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 1); ret(D3D_OK, 1); }
static void m_dev_getinfo(void) { ret(S_FALSE_, 3); }
static void m_dev_lightenable(void) { ret(D3D_OK, 2); }

static uint32_t device_vtable(void)
{
    if (!vt_device) {
        /* Los 49 métodos en orden (d3d.h de DirectX 7); NULL = todavía no, para con el nombre si el juego lo llama. */
        static const ComMethod m[49] = {
            {"QueryInterface", m_dev_qi}, {"AddRef", m_dev_addref}, {"Release", m_dev_release},
            {"GetCaps", m_dev_getcaps}, {"EnumTextureFormats", m_dev_enumtex}, {"BeginScene", m_dev_beginscene},
            {"EndScene", m_dev_endscene}, {"GetDirect3D", m_dev_getd3d}, {"SetRenderTarget", m_dev_setrt},
            {"GetRenderTarget", m_dev_getrt}, {"Clear", m_dev_clear}, {"SetTransform", m_dev_settransform},
            {"GetTransform", m_dev_gettransform}, {"SetViewport", m_dev_setviewport},
            {"MultiplyTransform", m_dev_multiplytransform}, {"GetViewport", m_dev_getviewport},
            {"SetMaterial", m_dev_setmaterial}, {"GetMaterial", m_dev_getmaterial}, {"SetLight", m_dev_ok2},
            {"GetLight", NULL}, {"SetRenderState", m_dev_setrs}, {"GetRenderState", m_dev_getrs},
            {"BeginStateBlock", NULL}, {"EndStateBlock", NULL}, {"PreLoad", m_dev_ok1},
            {"DrawPrimitive", m_dev_drawprim}, {"DrawIndexedPrimitive", m_dev_drawindexed}, {"SetClipStatus", m_dev_ok1},
            {"GetClipStatus", NULL}, {"DrawPrimitiveStrided", NULL}, {"DrawIndexedPrimitiveStrided", NULL},
            {"DrawPrimitiveVB", m_dev_drawprimvb}, {"DrawIndexedPrimitiveVB", m_dev_drawindexedvb}, {"ComputeSphereVisibility", NULL},
            {"GetTexture", m_dev_gettexture}, {"SetTexture", m_dev_settexture}, {"GetTextureStageState", m_dev_gettss},
            {"SetTextureStageState", m_dev_settss}, {"ValidateDevice", m_dev_validate}, {"ApplyStateBlock", NULL},
            {"CaptureStateBlock", NULL}, {"DeleteStateBlock", NULL}, {"CreateStateBlock", NULL}, {"Load", NULL},
            {"LightEnable", m_dev_lightenable}, {"GetLightEnable", NULL}, {"SetClipPlane", NULL}, {"GetClipPlane", NULL},
            {"GetInfo", m_dev_getinfo}};
        (void)m_dev_ok0;
        vt_device = com_vtable("IDirect3DDevice7", m, 49);
    }
    return vt_device;
}
