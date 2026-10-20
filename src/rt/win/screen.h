/* La pantalla del juego (screen.c). */
#ifndef PCF2001_RECOMP_SCREEN_H
#define PCF2001_RECOMP_SCREEN_H

#include "win.h"

typedef struct {
    uint32_t w, h, bpp;
    uint32_t palette_serial;
    uint8_t palette[256][3];
} ScreenState;
extern ScreenState screen;

/* El framebuffer de la pantalla (8 bits, pitch = ancho) en memoria plana: la superficie primaria de DirectDraw y el
 * destino de GDI para las ventanas. screen_dirty() pide presentarlo en el próximo bombeo de eventos. */
uint32_t screen_fb(void);
void screen_dirty(void);
void screen_flush(void);              /* presenta si está sucio (lo llama el bombeo de eventos) */

int screen_headless(void);
void screen_video_init(void);
void screen_set_mode(uint32_t w, uint32_t h, uint32_t bpp);
void screen_toggle_fullscreen(void);
uint32_t screen_pixel(uint32_t x, uint32_t y);   /* 0xRRGGBB del último cuadro presentado (0xFFFFFFFF: no hay) */
void screen_set_palette(uint32_t first, uint32_t count, const uint8_t *entries);
void screen_present(uint32_t addr, uint32_t pitch);
uint32_t screen_presents(void);
void screen_shot(void);
int screen_save_png(const char *path);
int screen_write_png(const char *path, uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch);
void screen_to_game(int wx, int wy, int *gx, int *gy);
void screen_show_cursor(int on);
int screen_has_window(void);

#endif
