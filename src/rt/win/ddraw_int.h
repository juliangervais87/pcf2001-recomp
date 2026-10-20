/* Lo que DirectDraw (ddraw.c) comparte con Direct3D (d3d.c): las superficies y sus píxeles. */
#ifndef PCF2001_RECOMP_DDRAW_INT_H
#define PCF2001_RECOMP_DDRAW_INT_H

#include "com.h"

typedef struct Surface {
    uint32_t obj, refs, caps, w, h, pitch, bits;
    int primary;
    uint32_t palette;            /* objeto IDirectDrawPalette o 0 */
    uint32_t clipper;
    uint32_t back;               /* superficie trasera (objeto) */
    uint32_t bpp;                /* 8 (con paleta), 16 (5-6-5) o 32 */
    int has_ck;
    uint32_t ck_lo, ck_hi;
    uint32_t dc;
    int v7;                      /* creada por IDirectDraw7: vtable de IDirectDrawSurface7, DDSCAPS2 */
    /* El formato pedido en CreateSurface (texturas de Direct3D: 5-6-5, 1-5-5-5, 4-4-4-4, 8-8-8-8...): flags de
     * DDPIXELFORMAT y las máscaras. Sin formato pedido, el de la pantalla. */
    uint32_t pf_flags, rmask, gmask, bmask, amask;
    uint32_t zbuffer;            /* superficie z adjunta (AddAttachedSurface) */
} Surface;

typedef struct { uint32_t obj, refs; uint8_t entries[256][4]; uint32_t attached_primary; } Palette;

static inline Surface *surf(uint32_t obj) { return obj ? (Surface *)com_ptr(obj) : NULL; }
static inline uint32_t px_bytes(const Surface *s) { return s->bpp > 8 ? s->bpp / 8 : 1; }
static inline uint32_t px_mask(const Surface *s) { return s->bpp >= 32 ? 0xFFFFFFFFu : (1u << (s->bpp > 8 ? s->bpp : 8)) - 1u; }
static inline uint32_t px_get(const uint8_t *p, uint32_t n)
{
    return n == 1 ? p[0] : n == 2 ? (uint32_t)(p[0] | p[1] << 8) : n == 3 ? (uint32_t)(p[0] | p[1] << 8 | p[2] << 16)
         : (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void px_put(uint8_t *p, uint32_t n, uint32_t v) { for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8 * i)); }

/* La superficie se mostró / cambió: si es la primaria, presentar. */
void ddraw_surface_changed(Surface *s);
/* Release de una superficie (libera todo al llegar a 0 si es de DirectDraw 7). */
void ddraw_surface_unref(Surface *s);

#endif
