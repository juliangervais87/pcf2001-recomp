/* Entrada: eventos del host (SDL) -> mensajes de Windows, y un guion de entrada para correr sin manos (fase 3).
 *
 * RECOMP_SCRIPT=archivo, una orden por línea (# comenta):
 *   wait <ms>            espera ms de reloj del juego (virtual con RECOMP_FIXED_CLOCK=1)
 *   click <x> <y>        mueve el mouse y hace clic izquierdo (coordenadas del juego, 640x480)
 *   rclick <x> <y>       ídem con el derecho
 *   move <x> <y>         mueve el mouse
 *   key <vk>             aprieta y suelta una tecla (código virtual en decimal o 0x..)
 *   type <texto>         teclea el texto
 *   shot                 captura PNG (necesita RECOMP_SHOT_DIR)
 *   quit                 cierra el juego (WM_CLOSE a la ventana principal)
 * Cada orden se ejecuta cuando el juego vuelve a pedir mensajes, así que el guion avanza al ritmo del juego. */
#include "user32.h"
#include "screen.h"

#include <SDL.h>
#include <ctype.h>

int32_t mouse_x = 320, mouse_y = 240;
uint8_t key_down[256];
uint32_t capture_hwnd, focus_hwnd;
static uint32_t buttons;

/* La ventana que recibe el mouse: la que capturó, o la hija visible más profunda bajo el punto. */
static uint32_t target_at(int x, int y, int32_t *cx, int32_t *cy)
{
    uint32_t t = capture_hwnd ? capture_hwnd : wnd_main();
    if (!capture_hwnd && t) {
        for (int changed = 1; changed;) {
            changed = 0;
            for (uint32_t h = 0x00010004u; h < 0x00010000u + 4u * 512; h += 4) {
                Wnd *w = wnd_get(h);
                if (!w || !(w->style & WS_CHILD) || w->parent != t || !w->visible) continue;
                int32_t ox, oy;
                wnd_screen_origin(h, &ox, &oy);
                if (x >= ox && x < ox + w->w && y >= oy && y < oy + w->h) { t = h; changed = 1; break; }
            }
        }
    }
    int32_t ox = 0, oy = 0;
    if (t) wnd_screen_origin(t, &ox, &oy);
    *cx = x - ox; *cy = y - oy;
    return t;
}

static uint32_t mk_flags(void) { return buttons | (key_down[0x10] ? 4u : 0u) | (key_down[0x11] ? 8u : 0u); }

void input_mouse(int x, int y, uint32_t msg, uint32_t bit)
{
    mouse_x = x; mouse_y = y;
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) buttons |= bit;
    if (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP) buttons &= ~bit;
    key_down[0x01] = (buttons & 1) != 0;
    key_down[0x02] = (buttons & 2) != 0;
    int32_t cx, cy;
    uint32_t t = target_at(x, y, &cx, &cy);
    if (!t) return;
    uint32_t lp = ((uint32_t)(uint16_t)cy << 16) | (uint16_t)cx;
    if (msg != WM_MOUSEMOVE) msg_post(t, 0x0020, t, ((uint32_t)msg << 16) | 1u);   /* WM_SETCURSOR antes del clic */
    msg_post(t, msg, mk_flags(), lp);
}

/* RECOMP_TRACE_KEYS=1: traza sólo del teclado (dónde se postea cada tecla, quién la saca de la cola y a qué ventana
 * se entrega), para cuando el juego no reacciona y la traza general sería demasiado (el partido 3D del 2001). */
/* Tecla virtual -> código de escaneo del set 1 de un teclado de PC (el de Windows); 0x100 marca las extendidas (E0). */
uint32_t win_vk_to_scan(uint32_t vk)
{
    static const uint8_t letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
    static const uint8_t digits[10] = {0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
    static const uint8_t fkeys[12] = {0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58};
    vk &= 0xff;
    if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
    if (vk >= '0' && vk <= '9') return digits[vk - '0'];
    if (vk >= 0x70 && vk <= 0x7B) return fkeys[vk - 0x70];
    if (vk >= 0x60 && vk <= 0x69) {                                         /* teclado numérico */
        static const uint8_t pad[10] = {0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49};
        return pad[vk - 0x60];
    }
    switch (vk) {
    case 0x1B: return 0x01;   case 0x08: return 0x0E;   case 0x09: return 0x0F;   case 0x0D: return 0x1C;
    case 0x10: return 0x2A;   case 0x11: return 0x1D;   case 0x12: return 0x38;   case 0x20: return 0x39;
    case 0x14: return 0x3A;   case 0xBD: return 0x0C;   case 0xBB: return 0x0D;   case 0xBC: return 0x33;
    case 0xBE: return 0x34;   case 0xBF: return 0x35;   case 0xBA: return 0x27;   case 0xDE: return 0x28;
    case 0xC0: return 0x29;   case 0xDB: return 0x1A;   case 0xDD: return 0x1B;   case 0xDC: return 0x2B;
    case 0x6A: return 0x37;   case 0x6D: return 0x4A;   case 0x6B: return 0x4E;   case 0x6E: return 0x53;
    case 0x26: return 0x148;  case 0x28: return 0x150;  case 0x25: return 0x14B;  case 0x27: return 0x14D;
    case 0x2D: return 0x152;  case 0x2E: return 0x153;  case 0x24: return 0x147;  case 0x23: return 0x14F;
    case 0x21: return 0x149;  case 0x22: return 0x151;
    default: return 0;
    }
}

int win_trace_keys(void)
{
    static int on = -1;
    if (on < 0) { const char *v = getenv("RECOMP_TRACE_KEYS"); on = v && *v && strcmp(v, "0"); }
    return on;
}

void input_key(uint32_t vk, int down)
{
    vk &= 0xff;
    int repeat = down && key_down[vk];
    key_down[vk] = (uint8_t)down;
    uint32_t t = focus_hwnd ? focus_hwnd : wnd_main();
    if (win_trace_keys()) fprintf(stderr, "TECLA vk %#x %s -> hwnd %#x (foco %#x, principal %#x)\n", vk, down ? "abajo" : "arriba", t, focus_hwnd, wnd_main());
    if (!t) return;
    /* lParam como en Windows: repetición 1, código de escaneo en los bits 16-23 (el partido del 2001 mapea sus teclas
     * por ese código, no por la tecla virtual), bit 24 de tecla extendida (flechas, Insert...), 30 y 31. */
    uint32_t sc = win_vk_to_scan(vk);
    uint32_t lp = 1u | (sc & 0xFFu) << 16 | (sc & 0x100u ? 0x01000000u : 0) | (repeat ? 0x40000000u : 0) | (down ? 0 : 0xC0000000u);
    msg_post(t, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
}

static uint32_t vk_of(SDL_Keycode k)
{
    if (k >= SDLK_a && k <= SDLK_z) return 'A' + (uint32_t)(k - SDLK_a);
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (uint32_t)(k - SDLK_0);
    if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + (uint32_t)(k - SDLK_F1);
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 0x0D;
    case SDLK_ESCAPE: return 0x1B;
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09;
    case SDLK_SPACE: return 0x20;
    case SDLK_LEFT: return 0x25;
    case SDLK_UP: return 0x26;
    case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN: return 0x28;
    case SDLK_INSERT: return 0x2D;
    case SDLK_DELETE: return 0x2E;
    case SDLK_HOME: return 0x24;
    case SDLK_END: return 0x23;
    case SDLK_PAGEUP: return 0x21;
    case SDLK_PAGEDOWN: return 0x22;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10;
    case SDLK_LCTRL: case SDLK_RCTRL: return 0x11;
    case SDLK_LALT: case SDLK_RALT: return 0x12;
    case SDLK_PERIOD: return 0xBE;
    case SDLK_COMMA: return 0xBC;
    case SDLK_MINUS: return 0xBD;
    }
    return 0;
}

/* ---- guion ---- */
static FILE *script;
static int script_tried;
static uint32_t script_wait_until;
static int script_done;

static void script_step(void)
{
    if (!script_tried) {
        script_tried = 1;
        const char *p = getenv("RECOMP_SCRIPT");
        if (p && *p) { script = fopen(p, "r"); if (!script) fprintf(stderr, "recomp: no se puede abrir el guion %s\n", p); }
    }
    if (!script || script_done) return;
    if (script_wait_until && (int32_t)(win_ticks_ms() - script_wait_until) < 0) return;
    script_wait_until = 0;
    char line[512];
    while (fgets(line, sizeof line, script)) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        char *e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
        if (!*s || *s == '#') continue;
        char cmd[32] = "";
        int a = 0, b = 0;
        sscanf(s, "%31s", cmd);
        fprintf(stderr, "recomp: guion: %s\n", s);
        if (!strcmp(cmd, "wait")) { sscanf(s, "%*s %d", &a); script_wait_until = win_ticks_ms() + (uint32_t)a; return; }
        if (!strcmp(cmd, "click") || !strcmp(cmd, "rclick")) {
            sscanf(s, "%*s %d %d", &a, &b);
            int right = cmd[0] == 'r';
            input_mouse(a, b, WM_MOUSEMOVE, 0);
            input_mouse(a, b, right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN, right ? 2u : 1u);
            input_mouse(a, b, right ? WM_RBUTTONUP : WM_LBUTTONUP, right ? 2u : 1u);
            return;
        }
        /* clicksi X Y RRGGBB CX CY: clic en (CX,CY) sólo si el píxel (X,Y) del cuadro actual es de ese color. Sirve para
         * los avisos que el juego mete cuando quiere (manager del mes, ofertas...): un guion de temporada los cierra
         * sin desfasarse. Determinista: mira el mismo cuadro que una captura. */
        if (!strcmp(cmd, "clicksi")) {
            unsigned rgb = 0;
            int cx = 0, cy = 0;
            sscanf(s, "%*s %d %d %x %d %d", &a, &b, &rgb, &cx, &cy);
            screen_flush();
            uint32_t got = screen_pixel((uint32_t)a, (uint32_t)b);
            if (got != rgb) { fprintf(stderr, "recomp: guion: clicksi: (%d,%d) es %06x, no %06x: sigue\n", a, b, got, rgb); continue; }
            input_mouse(cx, cy, WM_MOUSEMOVE, 0);
            input_mouse(cx, cy, WM_LBUTTONDOWN, 1u);
            input_mouse(cx, cy, WM_LBUTTONUP, 1u);
            return;
        }
        if (!strcmp(cmd, "move")) { sscanf(s, "%*s %d %d", &a, &b); input_mouse(a, b, WM_MOUSEMOVE, 0); return; }
        if (!strcmp(cmd, "key")) { a = (int)strtol(s + 4, NULL, 0); input_key((uint32_t)a, 1); input_key((uint32_t)a, 0); return; }
        if (!strcmp(cmd, "type")) {
            for (const char *t = s + 5; *t; t++) {
                int c = toupper((unsigned char)*t);
                uint32_t vk = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? (uint32_t)c : c == ' ' ? 0x20u : c == '.' ? 0xBEu : 0;
                if (vk) { input_key(vk, 1); input_key(vk, 0); }
            }
            return;
        }
        if (!strcmp(cmd, "shot")) { screen_flush(); screen_shot(); continue; }
        if (!strcmp(cmd, "quit")) { if (wnd_main()) msg_post(wnd_main(), WM_CLOSE, 0, 0); script_done = 1; return; }
        fprintf(stderr, "recomp: guion: orden desconocida '%s'\n", cmd);
    }
    script_done = 1;
    fprintf(stderr, "recomp: guion terminado\n");
    /* Huella de la memoria del juego (0x400000-0x1B000000: imágenes, heap, estática, pilas): con RECOMP_FIXED_CLOCK=1,
     * dos corridas del mismo guion tienen que dar la misma, en cualquier plataforma (fase 3). */
    uint64_t h = 1469598103934665603ull;
    for (uint32_t a = 0x400000u; a < 0x1B000000u; a += 8) {
        uint64_t v;
        memcpy(&v, g_mem + a, 8);
        h = (h ^ v) * 1099511628211ull;
    }
    fprintf(stderr, "recomp: huella de memoria al terminar el guion: %016llx (reloj virtual %s)\n", (unsigned long long)h,
            win_fixed_clock() ? "sí" : "no");
    /* RECOMP_SALIR=1: terminar acá. Con reloj virtual el guion entero corre en segundos; sin esto la corrida se
     * queda con el juego quieto hasta el timeout de afuera (minutos por corrida). */
    const char *salir = getenv("RECOMP_SALIR");
    if (salir && *salir && strcmp(salir, "0")) { fprintf(stderr, "recomp: fin del guion, se sale (RECOMP_SALIR)\n"); fflush(NULL); exit(0); }
}

/* Lo llaman GetMessage/PeekMessage/las esperas: lee el host, avanza el guion, presenta la pantalla. */
void win_pump_host_events(void)
{
    static uint32_t last_present, dump_at = 1;
    extern void win_watchdog_tick(void);
    extern void audio_pump(void);
    audio_pump();
    win_watchdog_tick();
    script_step();
    if (dump_at == 1) { const char *d = getenv("RECOMP_DUMP_AFTER_MS"); dump_at = d ? win_ticks_ms() + (uint32_t)atoi(d) : 0; }
    if (dump_at > 1 && (int32_t)(win_ticks_ms() - dump_at) >= 0) {
        extern void ddraw_dump(const char *);
        const char *dir = getenv("RECOMP_SHOT_DIR");
        ddraw_dump(dir ? dir : ".");
        dump_at = 0;
    }
    if (screen_has_window()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int gx, gy;
            switch (e.type) {
            case SDL_QUIT: fprintf(stderr, "recomp: ventana cerrada\n"); fflush(NULL); exit(0);   /* el juego ignora WM_CLOSE; cerrar la ventana termina */
            case SDL_MOUSEMOTION: screen_to_game(e.motion.x, e.motion.y, &gx, &gy); input_mouse(gx, gy, WM_MOUSEMOVE, 0); break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                int down = e.type == SDL_MOUSEBUTTONDOWN;
                screen_to_game(e.button.x, e.button.y, &gx, &gy);
                uint32_t msg = e.button.button == SDL_BUTTON_LEFT ? (down ? WM_LBUTTONDOWN : WM_LBUTTONUP)
                             : e.button.button == SDL_BUTTON_RIGHT ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP) : (down ? WM_MBUTTONDOWN : WM_MBUTTONUP);
                if (down && e.button.clicks == 2 && e.button.button == SDL_BUTTON_LEFT) msg = WM_LBUTTONDBLCLK;
                uint32_t bit = e.button.button == SDL_BUTTON_LEFT ? 1u : e.button.button == SDL_BUTTON_RIGHT ? 2u : 16u;
                input_mouse(gx, gy, msg, bit);
                break;
            }
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (e.key.keysym.sym == SDLK_F12 && e.type == SDL_KEYDOWN) { screen_shot(); break; }
#ifndef __EMSCRIPTEN__
                /* Alt+Enter y F11 son de la ventana, no del juego (en la web la pantalla completa la maneja la página). */
                if ((e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)) || e.key.keysym.sym == SDLK_F11) {
                    if (e.type == SDL_KEYDOWN && !e.key.repeat) screen_toggle_fullscreen();
                    break;
                }
#endif
                if (vk_of(e.key.keysym.sym)) input_key(vk_of(e.key.keysym.sym), e.type == SDL_KEYDOWN);
                break;
            }
        }
    }
    /* Presentar lo que GDI o DirectDraw dejaron en el framebuffer: a lo sumo cada 16 ms de reloj del juego. */
    uint32_t now = win_ticks_ms();
    if (now - last_present >= 16) { last_present = now; screen_flush(); }
}

/* Un juego que espera en un bucle propio consultando el mouse, el teclado o el reloj (sin PeekMessage) también tiene
 * que ver el guion, los eventos del host y a los otros hilos: en Windows real lo resolvía el planificador preventivo.
 * Cada 32 consultas se bombea y se cede (determinista: depende sólo de cuántas consultas hace el juego). */
void win_poll_hint(void)
{
    static uint32_t n;
    if (++n % 32) return;
    win_pump_host_events();
    win_yield();
}

/* Vigía de cuelgues (RECOMP_WATCHDOG=1): un hilo del host que, si el juego pasa 8 s sin bombear eventos, vuelca las
 * direcciones de código que hay en la pila del juego desde el último esp sincronizado: la cadena de llamadas del
 * bucle en el que quedó (el C traducido no lleva EIP, pero cada call deja su dirección de retorno). */
#include "loader.h"
static volatile uint32_t last_pump_host_ms;
static int watchdog(void *unused)
{
    (void)unused;
    for (;;) {
        SDL_Delay(1000);
        uint32_t now = SDL_GetTicks();
        if (!last_pump_host_ms || now - last_pump_host_ms < 8000) continue;
        uint32_t esp = cpu.esp;
        fprintf(stderr, "recomp: VIGÍA: %u ms sin bombear; esp=%#x; pila (direcciones de código):", now - last_pump_host_ms, esp);
        int shown = 0;
        for (uint32_t a = esp; a < esp + 0x4000 && shown < 24; a += 4) {
            uint32_t v = rd32(g_mem, a);
            for (uint32_t i = 0; i < rt_image_count; i++)
                if (v > rt_images[i].base + 0x1000 && v < rt_images[i].base + rt_images[i].size && rt_lookup(v - 0) != NULL) { }
            if ((v >= 0x401000u && v < 0x5e8e00u) || (v >= 0x700000u && v < 0x900000u)) { fprintf(stderr, " %#x", v); shown++; }
        }
        fprintf(stderr, "\n");
        last_pump_host_ms = now;           /* una vez por cada 8 s */
    }
    return 0;
}
void win_watchdog_tick(void)
{
    static int started = -1;
    if (started < 0) {
        const char *w = getenv("RECOMP_WATCHDOG");
        started = w && *w && strcmp(w, "0");
        if (started) SDL_CreateThread(watchdog, "vigia", NULL);
    }
    last_pump_host_ms = SDL_GetTicks();
}
