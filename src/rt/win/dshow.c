/* DirectShow mínimo (quartz: CLSID_FilterGraph) para la música del PC Fútbol 2001. d2s2.dll (0xfaad50) arma un grafo
 * con DOS filtros suyos: una fuente IAsyncReader que lee una pista MP3 cruda de SFX\Musicas y un renderer que junta el
 * PCM que le llega por IMemInputPin::Receive. Entre los dos, el DirectShow real pone el splitter y el decodificador
 * MP3; acá Render() lee la pista entera por el IAsyncReader de la fuente, la decodifica con minimp3 y conecta el pin
 * de entrada del renderer a un pin de salida propio con audio PCM (WAVEFORMATEX de 18 bytes, que d2s2 exige).
 *
 * Entrega sincrónica: IMediaControl::Run le da al renderer muestras de 1152 cuadros hasta que él pide Pause (ya
 * juntó lo que el consumidor quería; Receive llama a Pause/Run del grafo desde adentro) o se acaba la pista
 * (IPin::EndOfStream). El consumidor de d2s2 (0xfac2d0) llama a Run y espera el evento que el renderer señala, así
 * que cuando Run vuelve el evento ya está puesto.
 *
 * Interfaces y slots: los del SDK de DirectX 7 (strmif.h / control.h); d2s2 usa AddFilter, Render, Run, Pause, Stop y
 * GetDuration (informe en docs/estado-2001.md). */
#include "com.h"

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "../../../vendor/minimp3/minimp3.h"

#define VFW_E_NOT_CONNECTED 0x80040209u
#define S_FALSE 1u

typedef struct {
    uint32_t face_graph, face_control, face_seeking, out_pin, sample;
    uint32_t refs;
    uint32_t filters[4];
    int nfilters;
    uint32_t in_pin, in_mem;                   /* pin de entrada del renderer y su IMemInputPin */
    uint32_t mt, wfx;                          /* AM_MEDIA_TYPE y WAVEFORMATEX en memoria del juego */
    uint32_t buf, buf_cap;                     /* búfer de la muestra que se entrega */
    uint32_t sample_len;
    int16_t *pcm;                              /* pista decodificada (host) */
    uint64_t pcm_bytes, sent;
    uint32_t rate, channels;
    int running, delivering, eos_sent, renderer_running;
} Graph;

static uint32_t vt_graph, vt_control, vt_seeking, vt_pin, vt_sample;

static void ret(uint32_t hr, int nargs) { rt_ret_stdcall(hr, nargs + 1); }
static Graph *gr(void) { return (Graph *)com_ptr(rt_arg(0)); }

/* Método COM de un objeto del juego: args[0] es el objeto. */
static uint32_t gcall(uint32_t obj, int slot, int n, const uint32_t *args)
{
    uint32_t fn = rd32(g_mem, rd32(g_mem, obj) + 4u * (uint32_t)slot);
    return win_call(fn, n, args);
}
static uint32_t gcall0(uint32_t obj, int slot) { uint32_t a[1] = {obj}; return gcall(obj, slot, 1, a); }
static uint32_t gqi(uint32_t obj, const uint8_t guid[16], uint32_t *out)
{
    uint32_t g = rt_alloc(20), o = g + 16;
    memcpy(g_mem + g, guid, 16);
    wr32(g_mem, o, 0);
    uint32_t a[3] = {obj, g, o};
    uint32_t hr = gcall(obj, 0, 3, a);
    *out = rd32(g_mem, o);
    rt_free(g);
    return hr;
}

/* GUID binario a partir del texto (mismo formato que com_guid_is). */
static void guid_bytes(const char *t, uint8_t g[16])
{
    unsigned a, b, c, d[8];
    sscanf(t, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
    g[0] = (uint8_t)a; g[1] = (uint8_t)(a >> 8); g[2] = (uint8_t)(a >> 16); g[3] = (uint8_t)(a >> 24);
    g[4] = (uint8_t)b; g[5] = (uint8_t)(b >> 8); g[6] = (uint8_t)c; g[7] = (uint8_t)(c >> 8);
    for (int i = 0; i < 8; i++) g[8 + i] = (uint8_t)d[i];
}
static void put_guid(uint32_t at, const char *t) { uint8_t g[16]; guid_bytes(t, g); memcpy(g_mem + at, g, 16); }

static const char *IID_IUnknown = "00000000-0000-0000-c000-000000000046";
static const char *IID_IFilterGraph = "56a8689f-0ad4-11ce-b03a-0020af0ba770";
static const char *IID_IGraphBuilder = "56a868a9-0ad4-11ce-b03a-0020af0ba770";
static const char *IID_IMediaControl = "56a868b1-0ad4-11ce-b03a-0020af0ba770";
static const char *IID_IMediaSeeking = "36b73880-c2c8-11cf-8b46-00805f6cef60";
static const char *IID_IPin = "56a86891-0ad4-11ce-b03a-0020af0ba770";
static const char *IID_IMediaSample = "56a8689a-0ad4-11ce-b03a-0020af0ba770";
static const char *IID_IAsyncReader = "56a868aa-0ad4-11ce-b03a-0020af0ba770";
static const char *IID_IMemInputPin = "56a8689d-0ad4-11ce-b03a-0020af0ba770";

/* ---- el grafo: IGraphBuilder / IMediaControl / IMediaSeeking comparten el estado ---- */
static void g_qi(void)
{
    Graph *g = gr();
    uint32_t iid = rt_arg(1), out = rt_arg(2), r = 0;
    if (com_guid_is(iid, IID_IUnknown) || com_guid_is(iid, IID_IFilterGraph) || com_guid_is(iid, IID_IGraphBuilder)) r = g->face_graph;
    else if (com_guid_is(iid, IID_IMediaControl)) r = g->face_control;
    else if (com_guid_is(iid, IID_IMediaSeeking)) r = g->face_seeking;
    else { char t[40]; com_guid_text(iid, t); WIN_TRACE("FilterGraph::QueryInterface(%s): no\n", t); }
    if (r) g->refs++;
    if (out) wr32(g_mem, out, r);
    ret(r ? S_OK : E_NOINTERFACE, 2);
}
static void g_addref(void) { ret(++gr()->refs, 0); }
static void g_release(void)
{
    Graph *g = gr();
    if (g->refs) g->refs--;
    if (!g->refs) { free(g->pcm); g->pcm = NULL; g->pcm_bytes = g->sent = 0; }   /* los objetos quedan: d2s2 no los vuelve a usar */
    ret(g->refs, 0);
}
static void g_addfilter(void)
{
    Graph *g = gr();
    uint32_t f = rt_arg(1);
    if (g->nfilters < 4) g->filters[g->nfilters++] = f;
    uint32_t a[3] = {f, g->face_graph, rt_arg(2)};
    gcall(f, 13, 3, a);                                  /* IBaseFilter::JoinFilterGraph */
    ret(S_OK, 2);
}
static void g_notimpl1(void) { ret(E_NOTIMPL, 1); }
static void g_notimpl2(void) { ret(E_NOTIMPL, 2); }
static void g_notimpl3(void) { ret(E_NOTIMPL, 3); }
static void g_ok0(void) { ret(S_OK, 0); }
static void g_ok1(void) { ret(S_OK, 1); }

/* Primer pin del filtro con esa dirección (PINDIR_INPUT 0, PINDIR_OUTPUT 1), con referencia. */
static uint32_t find_pin(uint32_t filter, uint32_t dir)
{
    uint32_t t = rt_alloc_zero(12);
    uint32_t a[2] = {filter, t};
    if (gcall(filter, 10, 2, a) || !rd32(g_mem, t)) { rt_free(t); return 0; }   /* IBaseFilter::EnumPins */
    uint32_t en = rd32(g_mem, t), found = 0;
    for (;;) {
        uint32_t b[4] = {en, 1, t + 4, t + 8};
        wr32(g_mem, t + 4, 0);
        if (gcall(en, 3, 4, b) != S_OK || !rd32(g_mem, t + 4)) break;   /* IEnumPins::Next */
        uint32_t pin = rd32(g_mem, t + 4);
        uint32_t c[2] = {pin, t + 8};
        if (gcall(pin, 9, 2, c) == S_OK && rd32(g_mem, t + 8) == dir) { found = pin; break; }   /* QueryDirection */
        gcall0(pin, 2);
    }
    gcall0(en, 2);
    rt_free(t);
    return found;
}

/* Lee la pista entera por IAsyncReader::SyncRead y la decodifica. */
static int decode_source(Graph *g, uint32_t src_pin)
{
    uint8_t iid[16];
    guid_bytes(IID_IAsyncReader, iid);
    uint32_t rd;
    if (gqi(src_pin, iid, &rd) || !rd) { fprintf(stderr, "recomp: DirectShow: la fuente no da IAsyncReader\n"); return 0; }
    uint32_t t = rt_alloc_zero(16);
    uint32_t a[3] = {rd, t, t + 8};
    gcall(rd, 8, 3, a);                                  /* Length(&total, &disponible) */
    uint32_t total = rd32(g_mem, t);
    rt_free(t);
    uint8_t *mp3 = malloc(total ? total : 1);
    uint32_t chunk = 65536, tmp = rt_alloc(chunk), got = 0;
    while (got < total) {
        uint32_t n = total - got < chunk ? total - got : chunk;
        uint32_t b[5] = {rd, got, 0, n, tmp};
        uint32_t hr = gcall(rd, 7, 5, b);                /* SyncRead(posición, largo, búfer) */
        if (hr != S_OK && hr != S_FALSE) break;
        memcpy(mp3 + got, g_mem + tmp, n);
        got += n;
    }
    rt_free(tmp);
    gcall0(rd, 2);
    mp3dec_t dec;
    mp3dec_init(&dec);
    size_t cap = (size_t)total * 12 + 4096, used = 0;
    g->pcm = malloc(cap * sizeof(int16_t) / 2 + MINIMP3_MAX_SAMPLES_PER_FRAME * 2);
    int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t at = 0;
    g->rate = 0;
    while (at < got) {
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(&dec, mp3 + at, (int)(got - at), frame, &info);
        if (!info.frame_bytes) break;
        at += (size_t)info.frame_bytes;
        if (!n) continue;
        if (!g->rate) { g->rate = (uint32_t)info.hz; g->channels = (uint32_t)info.channels; }
        if ((uint32_t)info.channels != g->channels) continue;
        size_t bytes = (size_t)n * g->channels * 2u;
        if (used + bytes > cap * sizeof(int16_t) / 2) break;
        memcpy((uint8_t *)g->pcm + used, frame, bytes);
        used += bytes;
    }
    free(mp3);
    g->pcm_bytes = used;
    g->sent = 0;
    g->eos_sent = 0;
    WIN_TRACE("DirectShow: pista MP3 de %u bytes -> %u Hz, %u canales, %.1f s\n", total, g->rate, g->channels,
              g->rate ? (double)used / (g->rate * g->channels * 2.0) : 0.0);
    return g->rate != 0;
}

/* Render(pin de la fuente): decodifica y conecta nuestro pin de salida PCM al pin de entrada del renderer. */
static void g_render(void)
{
    Graph *g = gr();
    uint32_t src_pin = rt_arg(1);
    uint32_t hr = VFW_E_NOT_CONNECTED;
    uint32_t renderer = 0;
    for (int i = 0; i < g->nfilters && !g->in_pin; i++) {
        uint32_t p = find_pin(g->filters[i], 0);
        if (p) { g->in_pin = p; renderer = g->filters[i]; }
    }
    if (g->in_pin && decode_source(g, src_pin)) {
        uint32_t w = g->wfx;
        wr16(g_mem, w, 1); wr16(g_mem, w + 2, (uint16_t)g->channels); wr32(g_mem, w + 4, g->rate);
        wr32(g_mem, w + 8, g->rate * g->channels * 2u); wr16(g_mem, w + 12, (uint16_t)(g->channels * 2u));
        wr16(g_mem, w + 14, 16); wr16(g_mem, w + 16, 0);
        uint32_t a[3] = {g->in_pin, g->out_pin, g->mt};
        hr = gcall(g->in_pin, 4, 3, a);                  /* IPin::ReceiveConnection(nuestro pin, tipo) */
        if (hr == S_OK) {
            uint8_t iid[16];
            guid_bytes(IID_IMemInputPin, iid);
            gqi(g->in_pin, iid, &g->in_mem);
            if (!g->in_mem) hr = E_NOINTERFACE;
        }
    }
    WIN_TRACE("DirectShow: Render -> %#x (renderer %#x, pin %#x)\n", hr, renderer, g->in_pin);
    ret(hr, 1);
}

/* Entrega muestras hasta que el renderer pide Pause o se acaba la pista. */
static void deliver(Graph *g)
{
    if (g->delivering || !g->in_mem) return;
    g->delivering = 1;
    uint32_t chunk = 1152u * g->channels * 2u;
    while (g->running && g->sent < g->pcm_bytes) {
        uint32_t n = g->pcm_bytes - g->sent < chunk ? (uint32_t)(g->pcm_bytes - g->sent) : chunk;
        memcpy(g_mem + g->buf, (uint8_t *)g->pcm + g->sent, n);
        g->sample_len = n;
        uint32_t a[2] = {g->in_mem, g->sample};
        uint32_t hr = gcall(g->in_mem, 6, 2, a);        /* IMemInputPin::Receive */
        g->sent += n;
        if (hr != S_OK && hr != S_FALSE) { WIN_TRACE("DirectShow: Receive -> %#x\n", hr); break; }
    }
    if (g->sent >= g->pcm_bytes && !g->eos_sent && g->in_pin) {
        g->eos_sent = 1;
        gcall0(g->in_pin, 14);                           /* IPin::EndOfStream */
    }
    g->delivering = 0;
}
static void c_run(void)
{
    Graph *g = gr();
    if (!g->renderer_running) {
        g->renderer_running = 1;
        for (int i = 0; i < g->nfilters; i++) { uint32_t a[3] = {g->filters[i], 0, 0}; gcall(g->filters[i], 6, 3, a); }   /* IBaseFilter::Run(0) */
    }
    g->running = 1;
    deliver(g);
    ret(S_OK, 0);
}
static void c_pause(void) { gr()->running = 0; ret(S_OK, 0); }
static void c_stop(void)
{
    Graph *g = gr();
    g->running = 0;
    if (g->renderer_running && !g->delivering) {
        g->renderer_running = 0;
        for (int i = 0; i < g->nfilters; i++) gcall0(g->filters[i], 4);   /* IBaseFilter::Stop */
    }
    ret(S_OK, 0);
}
static void c_getstate(void) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), gr()->running ? 2u : 1u); ret(S_OK, 2); }
static void c_typeinfocount(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 0); ret(S_OK, 1); }
static void c_notimpl5(void) { ret(E_NOTIMPL, 5); }
static void c_notimpl8(void) { ret(E_NOTIMPL, 8); }

/* IMediaSeeking: sólo la duración (en 100 ns) importa. */
static void s_duration(void)
{
    Graph *g = gr();
    uint64_t d = g->rate ? g->pcm_bytes * 10000000ull / (g->rate * g->channels * 2ull) : 0;
    if (rt_arg(1)) { wr32(g_mem, rt_arg(1), (uint32_t)d); wr32(g_mem, rt_arg(1) + 4, (uint32_t)(d >> 32)); }
    ret(S_OK, 1);
}
static void s_pos(void)
{
    Graph *g = gr();
    uint64_t d = g->rate ? g->sent * 10000000ull / (g->rate * g->channels * 2ull) : 0;
    if (rt_arg(1)) { wr32(g_mem, rt_arg(1), (uint32_t)d); wr32(g_mem, rt_arg(1) + 4, (uint32_t)(d >> 32)); }
    ret(S_OK, 1);
}
static void s_notimpl4(void) { ret(E_NOTIMPL, 4); }

/* ---- nuestro pin de salida ---- */
static Graph *pin_graph(void) { return (Graph *)com_ptr(rt_arg(0)); }
static void p_qi(void)
{
    uint32_t iid = rt_arg(1), out = rt_arg(2);
    int ok = com_guid_is(iid, IID_IUnknown) || com_guid_is(iid, IID_IPin);
    if (out) wr32(g_mem, out, ok ? rt_arg(0) : 0);
    ret(ok ? S_OK : E_NOINTERFACE, 2);
}
static void p_ref(void) { ret(1, 0); }
static void p_connectedto(void)
{
    Graph *g = pin_graph();
    if (g->in_pin) gcall0(g->in_pin, 1);
    if (rt_arg(1)) wr32(g_mem, rt_arg(1), g->in_pin);
    ret(g->in_pin ? S_OK : VFW_E_NOT_CONNECTED, 1);
}
static void p_mediatype(void)
{
    Graph *g = pin_graph();
    uint32_t p = rt_arg(1);
    if (p) {
        memcpy(g_mem + p, g_mem + g->mt, 72);
        uint32_t f = rt_alloc(18);
        memcpy(g_mem + f, g_mem + g->wfx, 18);
        wr32(g_mem, p + 68, f);
    }
    ret(S_OK, 1);
}
static void p_pininfo(void)
{
    uint32_t p = rt_arg(1);
    if (p) {
        memset(g_mem + p, 0, 8 + 256);
        wr32(g_mem, p + 4, 1);                           /* PINDIR_OUTPUT */
        wr16(g_mem, p + 8, 'O'); wr16(g_mem, p + 10, 'u'); wr16(g_mem, p + 12, 't');
    }
    ret(S_OK, 1);
}
static void p_direction(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 1); ret(S_OK, 1); }
static void p_queryaccept(void) { ret(S_OK, 1); }
static void p_ok0(void) { ret(S_OK, 0); }
static void p_newsegment(void) { ret(S_OK, 6); }
static void p_notimpl1(void) { ret(E_NOTIMPL, 1); }
static void p_notimpl2(void) { ret(E_NOTIMPL, 2); }

/* ---- la muestra que se entrega (una sola por grafo, se reusa) ---- */
static void m_qi(void)
{
    uint32_t iid = rt_arg(1), out = rt_arg(2);
    int ok = com_guid_is(iid, IID_IUnknown) || com_guid_is(iid, IID_IMediaSample);
    if (out) wr32(g_mem, out, ok ? rt_arg(0) : 0);
    ret(ok ? S_OK : E_NOINTERFACE, 2);
}
static void m_ref(void) { ret(1, 0); }
static void m_getpointer(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), pin_graph()->buf); ret(S_OK, 1); }
static void m_getsize(void) { ret(pin_graph()->buf_cap, 0); }
static void m_gettime(void)
{
    Graph *g = pin_graph();
    uint64_t bps = g->rate * g->channels * 2ull;
    uint64_t t0 = bps ? g->sent * 10000000ull / bps : 0, t1 = bps ? (g->sent + g->sample_len) * 10000000ull / bps : 0;
    if (rt_arg(1)) { wr32(g_mem, rt_arg(1), (uint32_t)t0); wr32(g_mem, rt_arg(1) + 4, (uint32_t)(t0 >> 32)); }
    if (rt_arg(2)) { wr32(g_mem, rt_arg(2), (uint32_t)t1); wr32(g_mem, rt_arg(2) + 4, (uint32_t)(t1 >> 32)); }
    ret(S_OK, 2);
}
static void m_ok1(void) { ret(S_OK, 1); }
static void m_ok2(void) { ret(S_OK, 2); }
static void m_yes(void) { ret(S_OK, 0); }
static void m_no(void) { ret(S_FALSE, 0); }
static void m_discont(void) { ret(pin_graph()->sent == 0 ? S_OK : S_FALSE, 0); }
static void m_getlen(void) { ret(pin_graph()->sample_len, 0); }
static void m_setlen(void) { pin_graph()->sample_len = rt_arg(1); ret(S_OK, 1); }
static void m_getmt(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 0); ret(S_FALSE, 1); }
static void m_getmediatime(void) { ret(0x80040251u, 2); }                       /* VFW_E_MEDIA_TIME_NOT_SET */

#define M(n, f) {n, f}
static void build(void)
{
    if (vt_graph) return;
    static const ComMethod gb[18] = {M("QueryInterface", g_qi), M("AddRef", g_addref), M("Release", g_release),
        M("AddFilter", g_addfilter), M("RemoveFilter", g_ok1), M("EnumFilters", g_notimpl1), M("FindFilterByName", g_notimpl2),
        M("ConnectDirect", g_notimpl3), M("Reconnect", g_notimpl1), M("Disconnect", g_ok1), M("SetDefaultSyncSource", g_ok0),
        M("Connect", g_notimpl2), M("Render", g_render), M("RenderFile", g_notimpl2), M("AddSourceFilter", g_notimpl3),
        M("SetLogFile", g_ok1), M("Abort", g_ok0), M("ShouldOperationContinue", g_ok0)};
    vt_graph = com_vtable("IGraphBuilder", gb, 18);
    static const ComMethod mc[16] = {M("QueryInterface", g_qi), M("AddRef", g_addref), M("Release", g_release),
        M("GetTypeInfoCount", c_typeinfocount), M("GetTypeInfo", g_notimpl3), M("GetIDsOfNames", c_notimpl5),
        M("Invoke", c_notimpl8), M("Run", c_run), M("Pause", c_pause), M("Stop", c_stop), M("GetState", c_getstate),
        M("RenderFile", g_notimpl1), M("AddSourceFilter", g_notimpl2), M("get_FilterCollection", g_notimpl1),
        M("get_RegFilterCollection", g_notimpl1), M("StopWhenReady", c_stop)};
    vt_control = com_vtable("IMediaControl", mc, 16);
    static const ComMethod ms[20] = {M("QueryInterface", g_qi), M("AddRef", g_addref), M("Release", g_release),
        M("GetCapabilities", g_notimpl1), M("CheckCapabilities", g_notimpl1), M("IsFormatSupported", g_ok1),
        M("QueryPreferredFormat", g_notimpl1), M("GetTimeFormat", g_notimpl1), M("IsUsingTimeFormat", g_ok1),
        M("SetTimeFormat", g_ok1), M("GetDuration", s_duration), M("GetStopPosition", s_duration),
        M("GetCurrentPosition", s_pos), M("ConvertTimeFormat", c_notimpl5), M("SetPositions", s_notimpl4),
        M("GetPositions", g_notimpl2), M("GetAvailable", g_notimpl2), M("SetRate", g_notimpl2), M("GetRate", g_notimpl1),
        M("GetPreroll", g_notimpl1)};
    vt_seeking = com_vtable("IMediaSeeking", ms, 20);
    static const ComMethod pin[18] = {M("QueryInterface", p_qi), M("AddRef", p_ref), M("Release", p_ref),
        M("Connect", p_notimpl2), M("ReceiveConnection", p_notimpl2), M("Disconnect", p_ok0), M("ConnectedTo", p_connectedto),
        M("ConnectionMediaType", p_mediatype), M("QueryPinInfo", p_pininfo), M("QueryDirection", p_direction),
        M("QueryId", p_notimpl1), M("QueryAccept", p_queryaccept), M("EnumMediaTypes", p_notimpl1),
        M("QueryInternalConnections", p_notimpl2), M("EndOfStream", p_ok0), M("BeginFlush", p_ok0), M("EndFlush", p_ok0),
        M("NewSegment", p_newsegment)};
    vt_pin = com_vtable("IPin(salida PCM)", pin, 18);
    static const ComMethod sm[19] = {M("QueryInterface", m_qi), M("AddRef", m_ref), M("Release", m_ref),
        M("GetPointer", m_getpointer), M("GetSize", m_getsize), M("GetTime", m_gettime), M("SetTime", m_ok2),
        M("IsSyncPoint", m_yes), M("SetSyncPoint", m_ok1), M("IsPreroll", m_no), M("SetPreroll", m_ok1),
        M("GetActualDataLength", m_getlen), M("SetActualDataLength", m_setlen), M("GetMediaType", m_getmt),
        M("SetMediaType", m_ok1), M("IsDiscontinuity", m_discont), M("SetDiscontinuity", m_ok1),
        M("GetMediaTime", m_getmediatime), M("SetMediaTime", m_ok2)};
    vt_sample = com_vtable("IMediaSample", sm, 19);
}

/* CoCreateInstance(CLSID_FilterGraph, iid): un grafo nuevo, con la cara pedida. */
uint32_t dshow_filter_graph(uint32_t iid)
{
    build();
    Graph *g = calloc(1, sizeof *g);
    g->refs = 1;
    g->face_graph = com_new(vt_graph, g, 0);
    g->face_control = com_new(vt_control, g, 0);
    g->face_seeking = com_new(vt_seeking, g, 0);
    g->out_pin = com_new(vt_pin, g, 0);
    g->sample = com_new(vt_sample, g, 0);
    g->buf_cap = 1152u * 2u * 2u;
    g->buf = rt_alloc_zero(g->buf_cap);
    g->wfx = rt_alloc_zero(20);
    g->mt = rt_alloc_zero(72);
    put_guid(g->mt, "73647561-0000-0010-8000-00aa00389b71");          /* MEDIATYPE_Audio */
    put_guid(g->mt + 16, "00000001-0000-0010-8000-00aa00389b71");     /* MEDIASUBTYPE_PCM */
    wr32(g_mem, g->mt + 32, 1);                                       /* bFixedSizeSamples */
    wr32(g_mem, g->mt + 40, 4);                                       /* lSampleSize */
    put_guid(g->mt + 44, "05589f81-c356-11ce-bf01-00aa0055595a");     /* FORMAT_WaveFormatEx */
    wr32(g_mem, g->mt + 64, 18);                                      /* cbFormat */
    wr32(g_mem, g->mt + 68, g->wfx);
    if (com_guid_is(iid, IID_IMediaControl)) return g->face_control;
    if (com_guid_is(iid, IID_IMediaSeeking)) return g->face_seeking;
    return g->face_graph;
}
