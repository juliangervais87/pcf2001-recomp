/* La pantalla: un modo (por defecto 640x480 a 8 bits), su paleta y la ventana SDL que lo muestra. DirectDraw y GDI
 * escriben píxeles en memoria plana; screen_present() los convierte con la paleta y los muestra.
 *   RECOMP_HEADLESS=1      sin ventana (fase 3, CI): todo igual salvo mostrar
 *   RECOMP_SCALE=n         escala entera de la ventana (2 por defecto; menos si no entra en el escritorio)
 *   Alt+Enter o F11        pantalla completa / ventana; maximizada, la imagen se escala con su proporción
 *   RECOMP_SHOT_DIR=dir    captura PNG con F12 y al terminar (shot_NNNN.png)
 *   RECOMP_SHOT_EVERY=n    además, una captura cada n presentaciones (para ver la corrida sin mirar) */
#include "screen.h"

#include <SDL.h>

/* La paleta del sistema de Windows a 8 bits arranca con los 20 colores fijos (0-9 y 246-255); el medio, en negro. */
ScreenState screen = {640, 480, 8, 0, {
    {0x00, 0x00, 0x00}, {0x80, 0x00, 0x00}, {0x00, 0x80, 0x00}, {0x80, 0x80, 0x00}, {0x00, 0x00, 0x80},
    {0x80, 0x00, 0x80}, {0x00, 0x80, 0x80}, {0xC0, 0xC0, 0xC0}, {0xC0, 0xDC, 0xC0}, {0xA6, 0xCA, 0xF0},
    [246] = {0xFF, 0xFB, 0xF0}, {0xA0, 0xA0, 0xA4}, {0x80, 0x80, 0x80}, {0xFF, 0x00, 0x00}, {0x00, 0xFF, 0x00},
    {0xFF, 0xFF, 0x00}, {0x00, 0x00, 0xFF}, {0xFF, 0x00, 0xFF}, {0x00, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF}}};
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static int headless = -1, scale = 2;
static uint32_t rgba[1600 * 1200];

/* RECOMP_ESTIRAR=1: la imagen llena la ventana o la pantalla completa aunque se deforme (sin barras negras); si no,
 * conserva la proporción del modo. Estirada, SDL ya no traduce el mouse: lo hace screen_to_game. */
static int stretch(void) { static int s = -1; if (s < 0) { const char *e = getenv("RECOMP_ESTIRAR"); s = e && *e == '1'; } return s; }
static void set_logical(uint32_t w, uint32_t h)
{
    if (stretch()) SDL_RenderSetLogicalSize(renderer, 0, 0);
    else SDL_RenderSetLogicalSize(renderer, (int)w, (int)h);
}
static uint32_t presents, shot_every, shots;
static uint32_t last_addr, last_pitch;

int screen_headless(void)
{
    if (headless < 0) {
        const char *h = getenv("RECOMP_HEADLESS");
        headless = h && *h && strcmp(h, "0");
        const char *s = getenv("RECOMP_SCALE");
        if (s && atoi(s) > 0) scale = atoi(s);
        const char *e = getenv("RECOMP_SHOT_EVERY");
        if (e) shot_every = (uint32_t)atoi(e);
    }
    return headless;
}

/* El video de SDL arranca antes que la ventana: el juego carga sus cursores al inicio (cursor.c). */
void screen_video_init(void)
{
    static int done;
    if (done || screen_headless()) return;
    done = 1;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) rt_fatal("SDL_Init: %s", SDL_GetError());
}

static int fit_scale(uint32_t w, uint32_t h);

static void ensure_window(void)
{
    if (screen_headless() || window) return;
    screen_video_init();
    int s = fit_scale(screen.w, screen.h);
    window = SDL_CreateWindow("PC Fútbol 2001 (recompilado)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              (int)screen.w * s, (int)screen.h * s, SDL_WINDOW_RESIZABLE);
    if (!window) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    set_logical(screen.w, screen.h);
}

/* La escala pedida (RECOMP_SCALE), achicada hasta que la ventana entre en el escritorio: 1024x768 a escala 2 no entra. */
static int fit_scale(uint32_t w, uint32_t h)
{
    SDL_Rect r;
    int s = scale;
    if (SDL_GetDisplayUsableBounds(0, &r) == 0)
        while (s > 1 && ((int)w * s > r.w || (int)h * s > r.h - 32)) s--;     /* 32: la barra de título */
    return s;
}

/* Alt+Enter o F11: pantalla completa (la del escritorio, sin cambiar el modo del monitor) o de vuelta a la ventana. */
void screen_toggle_fullscreen(void)
{
    if (!window) return;
    int full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

void screen_set_mode(uint32_t w, uint32_t h, uint32_t bpp)
{
    if (w * h > sizeof rgba / 4) rt_fatal("modo %ux%u demasiado grande", w, h);
    screen.w = w; screen.h = h; screen.bpp = bpp;
    fprintf(stderr, "recomp: modo de pantalla %ux%ux%u\n", w, h, bpp);
    if (texture) { SDL_DestroyTexture(texture); texture = NULL; }
    if (!window) return;
    /* Maximizada o en pantalla completa, la ventana se queda como está y el modo nuevo se escala adentro. */
    if (!(SDL_GetWindowFlags(window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
        int s = fit_scale(w, h);
        SDL_SetWindowSize(window, (int)w * s, (int)h * s);
    }
    set_logical(w, h);
}

void screen_set_palette(uint32_t first, uint32_t count, const uint8_t *entries)   /* PALETTEENTRY: r, g, b, flags */
{
    for (uint32_t i = 0; i < count && first + i < 256; i++) {
        screen.palette[first + i][0] = entries[4 * i];
        screen.palette[first + i][1] = entries[4 * i + 1];
        screen.palette[first + i][2] = entries[4 * i + 2];
    }
    screen.palette_serial++;
}

/* El color (0xRRGGBB) del píxel x de una fila, según los bits del modo: 8 con la paleta, 16 en 5-6-5 (lo que da la
 * placa más común de 2000; PC Fútbol 2001), 24 y 32 en BGR(X). */
static uint32_t rgb_at(const uint8_t *row, uint32_t x)
{
    switch (screen.bpp) {
    case 16: {
        uint32_t v = (uint32_t)row[2 * x] | (uint32_t)row[2 * x + 1] << 8;
        uint32_t r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
        return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    }
    case 24: return (uint32_t)row[3 * x + 2] << 16 | (uint32_t)row[3 * x + 1] << 8 | row[3 * x];
    case 32: return (uint32_t)row[4 * x + 2] << 16 | (uint32_t)row[4 * x + 1] << 8 | row[4 * x];
    default: {
        const uint8_t *c = screen.palette[row[x]];
        return (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
    }
    }
}

/* El color (0xRRGGBB) de un píxel del último cuadro presentado, como lo ve una captura; 0xFFFFFFFF si no hay cuadro o
 * el punto cae afuera. Para las órdenes condicionales del guion (clicksi, input.c). */
uint32_t screen_pixel(uint32_t x, uint32_t y)
{
    if (!last_addr || x >= screen.w || y >= screen.h) return 0xFFFFFFFFu;
    return rgb_at(g_mem + last_addr + y * last_pitch, x);
}

static void convert(uint32_t addr, uint32_t pitch)
{
    const uint8_t *src = g_mem + addr;
    for (uint32_t y = 0; y < screen.h; y++)
        for (uint32_t x = 0; x < screen.w; x++)
            rgba[y * screen.w + x] = 0xff000000u | rgb_at(src + y * pitch, x);
}

/* ---- PNG sin compresión (bloques "stored" de deflate): suficiente para capturas ---- */
static uint32_t crc_table[256];
static uint32_t crc(const uint8_t *p, size_t n, uint32_t c)
{
    if (!crc_table[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1; crc_table[i] = v; }
    c = ~c;
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t h[8];
    be32(h, n);
    memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    fwrite(data, 1, n, f);
    uint32_t c = crc((const uint8_t *)type, 4, 0);
    c = crc(data, n, c);
    be32(h, c);
    fwrite(h, 1, 4, f);
}

int screen_save_png(const char *path)
{
    if (!last_addr) return 0;
    return screen_write_png(path, last_addr, screen.w, screen.h, last_pitch);
}

/* Cualquier buffer de 8 bits de la memoria plana, con la paleta actual de la pantalla. */
int screen_write_png(const char *path, uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch)
{
    uint32_t row = 1 + 3 * w;
    uint32_t raw_n = row * h;
    uint8_t *raw = malloc(raw_n);
    for (uint32_t y = 0; y < h; y++) {
        raw[y * row] = 0;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t p = rgb_at(g_mem + addr + y * pitch, x);
            raw[y * row + 1 + 3 * x] = (uint8_t)(p >> 16);
            raw[y * row + 2 + 3 * x] = (uint8_t)(p >> 8);
            raw[y * row + 3 + 3 * x] = (uint8_t)p;
        }
    }
    uint32_t blocks = (raw_n + 65534) / 65535;
    uint32_t z_n = 2 + raw_n + 5 * blocks + 4;
    uint8_t *z = malloc(z_n), *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (uint32_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (uint32_t off = 0; off < raw_n; off += 65535) {
        uint32_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        *q++ = off + n == raw_n;
        *q++ = (uint8_t)n; *q++ = (uint8_t)(n >> 8); *q++ = (uint8_t)~n; *q++ = (uint8_t)(~n >> 8);
        memcpy(q, raw + off, n);
        q += n;
    }
    be32(q, b << 16 | a);
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, w); be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, z_n);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 1;
}

void screen_shot(void)
{
    const char *dir = getenv("RECOMP_SHOT_DIR");
    if (!dir || !last_addr) return;
    char path[1200];
    snprintf(path, sizeof path, "%s/shot_%04u.png", dir, shots++);
    if (screen_save_png(path)) fprintf(stderr, "recomp: captura %s\n", path);
}

void screen_present(uint32_t addr, uint32_t pitch)
{
    last_addr = addr;
    last_pitch = pitch;
    presents++;
    if (shot_every && presents % shot_every == 0) screen_shot();
    if (screen_headless()) return;
    ensure_window();
    convert(addr, pitch);
    if (!texture) texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, (int)screen.w, (int)screen.h);
    SDL_UpdateTexture(texture, NULL, rgba, (int)screen.w * 4);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
#ifdef __EMSCRIPTEN__
    /* el canvas se actualiza cuando el navegador recupera el control */
#ifdef RECOMP_WEB
    extern void web_yield(int kind);
    web_yield(0);
#else
    extern void emscripten_sleep(unsigned int ms);
    emscripten_sleep(0);
#endif
#endif
}

uint32_t screen_presents(void) { return presents; }

#define FB_MAX (1024u * 768u)
static uint32_t fb, fb_size;
static int fb_dirty;
/* El framebuffer: 1024x768 a 8 bits de base (así la memoria no se mueve para quien no pide más); un modo de más
 * bits (PC Fútbol 2001) pide uno nuevo del tamaño justo. */
uint32_t screen_fb(void)
{
    uint32_t need = screen.w * screen.h * (screen.bpp > 8 ? screen.bpp / 8 : 1);
    if (!fb || need > fb_size) {
        fb_size = need > FB_MAX ? need : FB_MAX;
        fb = win_static_alloc(fb_size);
    }
    return fb;
}
void screen_dirty(void) { fb_dirty = 1; }
void screen_flush(void)
{
    if (!fb_dirty) return;
    fb_dirty = 0;
    screen_present(screen_fb(), screen.w * (screen.bpp / 8 ? screen.bpp / 8 : 1));
}

/* Ventana del host -> coordenadas del juego. */
void screen_to_game(int wx, int wy, int *gx, int *gy)
{
    /* Con SDL_RenderSetLogicalSize, SDL ya entrega los eventos del mouse en coordenadas del juego: sólo se acotan.
     * Estirada, se escala cada eje por separado. */
    int ww, wh;
    if (stretch() && window && (SDL_GetWindowSize(window, &ww, &wh), ww > 0 && wh > 0)) {
        wx = (int)((int64_t)wx * (int)screen.w / ww);
        wy = (int)((int64_t)wy * (int)screen.h / wh);
    }
    *gx = wx < 0 ? 0 : wx >= (int)screen.w ? (int)screen.w - 1 : wx;
    *gy = wy < 0 ? 0 : wy >= (int)screen.h ? (int)screen.h - 1 : wy;
}

void screen_show_cursor(int on) { if (window) SDL_ShowCursor(on ? SDL_ENABLE : SDL_DISABLE); }
int screen_has_window(void) { return window != NULL; }
