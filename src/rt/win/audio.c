/* Salida de audio: mezcla por SDL los buffers de DirectSound (dsound.c) que están sonando. Sólo con reloj real y con
 * ventana: con RECOMP_FIXED_CLOCK=1 o RECOMP_HEADLESS=1 no hay audio (el hilo de audio no debe tocar el reloj virtual:
 * la fase 3 exige determinismo). El volumen y el paneo son los de DirectSound (centésimas de dB).
 *
 * Cada buffer tiene una posición de lectura propia que avanza muestra a muestra; el reloj (la misma cuenta que
 * ds_position) sólo la re-sincroniza tras un Play/SetCurrentPosition/SetFrequency o si se desvía más de DRIFT_MS.
 * Recalcularla del reloj en cada callback saltaba o repetía unos milisegundos entre callbacks (la «fritura»). La
 * lectura va LAG_MS detrás del cursor de reproducción que ve el juego: el juego escribe a partir del cursor de
 * escritura (20 ms adelante), así que leer adelante del cursor mezclaba datos todavía sin escribir. */
#include "win.h"
#include "screen.h"
#include "dsound_int.h"

#include <SDL.h>
#include <math.h>

#define OUT_RATE 44100
#define DRIFT_MS 40
static SDL_AudioDeviceID dev;
static int tried;
static uint32_t lag_ms = 50;

/* estado del mezclador por hueco de ds_bufs */
static struct {
    DSBuf *b;
    uint32_t start_ms, start_pos, freq;
    int playing;
    double f;                                  /* cuadro de lectura (fraccional) */
} st[MAX_BUFS];

static float db_gain(int32_t centi_db) { return centi_db >= 0 ? 1.0f : (float)pow(10.0, centi_db / 2000.0); }

static void sample(const DSBuf *b, uint64_t fi, int *l, int *r)
{
    const uint8_t *p = g_mem + b->mem + fi * b->channels * (b->bits / 8u);
    if (b->bits == 16) { *l = (int16_t)(p[0] | p[1] << 8); *r = b->channels > 1 ? (int16_t)(p[2] | p[3] << 8) : *l; }
    else { *l = ((int)p[0] - 128) << 8; *r = b->channels > 1 ? ((int)p[1] - 128) << 8 : *l; }
}

static void mix(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    static float acc[8192];
    if (frames > 4096) frames = 4096;
    memset(acc, 0, sizeof(float) * 2u * (size_t)frames);
    extern uint32_t audio_tick_base;
    uint32_t game_now = SDL_GetTicks() - audio_tick_base + 1000000u;   /* reloj del juego (win_ticks_ms) */
    for (int i = 0; i < ds_nbufs; i++) {
        DSBuf *b = ds_bufs[i];
        if (!b || !b->playing || b->primary || !b->bytes || !b->mem) { st[i].b = NULL; continue; }
        uint32_t rate = b->freq ? b->freq : b->rate;
        uint32_t bpf = b->channels * (b->bits / 8u);
        uint64_t nframes = b->bytes / bpf;
        if (!rate || !bpf || !nframes) continue;
        /* adónde debería ir la lectura según el reloj, LAG_MS atrás del cursor del juego */
        /* (negativa en los primeros LAG_MS tras el Play: silencio hasta llegar al comienzo) */
        double el = (double)(int32_t)(game_now - b->start_ms) - (double)lag_ms;
        double want = b->start_pos / (double)bpf + el * rate / 1000.0;
        if (b->looping && want >= 0) want = fmod(want, (double)nframes);
        int fresh = st[i].b != b || !st[i].playing || st[i].start_ms != b->start_ms || st[i].start_pos != b->start_pos ||
                    st[i].freq != b->freq;
        double d = st[i].f - want;
        if (b->looping) { if (d > nframes / 2.0) d -= nframes; else if (d < -(nframes / 2.0)) d += nframes; }
        if (fresh || fabs(d) > (double)DRIFT_MS * rate / 1000.0) st[i].f = want;
        st[i].b = b; st[i].playing = 1; st[i].start_ms = b->start_ms; st[i].start_pos = b->start_pos; st[i].freq = b->freq;
        float g = db_gain(b->volume);
        float gl = g * (b->pan > 0 ? db_gain(-b->pan) : 1.0f), gr = g * (b->pan < 0 ? db_gain(b->pan) : 1.0f);
        double step = (double)rate / OUT_RATE, f = st[i].f;
        int k = 0;
        for (; k < frames; k++, f += step) {
            if (f < 0) continue;
            if (f >= (double)nframes) { if (!b->looping) break; f -= (double)nframes; }
            uint64_t fi = (uint64_t)f, fn = fi + 1;
            if (fn >= nframes) fn = b->looping ? 0 : fi;
            float t = (float)(f - (double)fi);
            int l0, r0, l1, r1;
            sample(b, fi, &l0, &r0);
            sample(b, fn, &l1, &r1);
            acc[2 * k] += ((float)l0 + ((float)l1 - (float)l0) * t) * gl;
            acc[2 * k + 1] += ((float)r0 + ((float)r1 - (float)r0) * t) * gr;
        }
        st[i].f = f;
    }
    for (int k = 0; k < 2 * frames; k++) out[k] = (int16_t)(acc[k] > 32767.f ? 32767 : acc[k] < -32768.f ? -32768 : (int)acc[k]);
    if (frames * 4 < len) memset(stream + frames * 4, 0, (size_t)(len - frames * 4));
}

uint32_t audio_tick_base;

void audio_lock(void) { if (dev) SDL_LockAudioDevice(dev); }
void audio_unlock(void) { if (dev) SDL_UnlockAudioDevice(dev); }

/* Lo llama dsound.c en el primer Play. */
void audio_start(void)
{
    if (tried) return;
    tried = 1;
    if (win_fixed_clock() || screen_headless() || getenv("RECOMP_MUTE")) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) { fprintf(stderr, "recomp: sin audio: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 2048;
#ifdef __EMSCRIPTEN__
    want.callback = NULL;     /* web: cola (audio_pump) — un callback entraría al wasm durante una espera de Asyncify */
#else
    want.callback = mix;
#endif
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { fprintf(stderr, "recomp: sin audio: %s\n", SDL_GetError()); return; }
    /* base para pasar de SDL_GetTicks al reloj del juego: win_ticks_ms() = host_ms - base + 1000000 */
    audio_tick_base = SDL_GetTicks() - (win_ticks_ms() - 1000000u);
#ifdef __EMSCRIPTEN__
    lag_ms = 170;                                                                         /* la cola de audio_pump */
#else
    lag_ms = 1000u * have.samples / (have.freq ? (uint32_t)have.freq : OUT_RATE) + 4u;   /* un callback */
#endif
    SDL_PauseAudioDevice(dev, 0);
    fprintf(stderr, "recomp: audio %d Hz, lectura %u ms detrás\n", have.freq, lag_ms);
}

/* Web: lo llama el bombeo de eventos (input.c) desde el hilo del juego. Mantiene ~150 ms de audio encolado. */
void audio_pump(void)
{
#ifdef __EMSCRIPTEN__
    static int16_t buf[2 * 1024];
    if (!dev) return;
    while (SDL_GetQueuedAudioSize(dev) < OUT_RATE * 4 * 15 / 100) {
        mix(NULL, (Uint8 *)buf, sizeof buf);
        SDL_QueueAudio(dev, buf, sizeof buf);
    }
#endif
}
