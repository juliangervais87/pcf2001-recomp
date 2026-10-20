/* DirectSound (dsound.dll por LoadLibraryA): IDirectSound, IDirectSoundBuffer, IDirectSound3DListener. Los buffers
 * viven en memoria plana; la posición de reproducción avanza con el reloj del juego (win_ticks_ms), así el streaming
 * del reproductor de módulos (.s3m) y los efectos funcionan igual con reloj real o virtual (fase 3). La salida de
 * audio por SDL la mezcla audio.c leyendo estos buffers. Métodos según directx-com-layer.md § 3. */
#include "com.h"
#include "dsound_int.h"

#define DS_OK 0u

static uint32_t vt_ds, vt_buf, vt_listener;
static uint32_t ds_obj, listener_obj;

static void ret(uint32_t hr, int nargs) { rt_ret_stdcall(hr, nargs + 1); }

DSBuf *ds_bufs[MAX_BUFS];
int ds_nbufs;

/* Alta en ds_bufs, reusando el hueco de un buffer liberado (el 2001 crea uno por efecto que suena). */
static void ds_add(DSBuf *b)
{
    int i = 0;
    while (i < ds_nbufs && ds_bufs[i]) i++;
    if (i == MAX_BUFS) { fprintf(stderr, "recomp: más de %d buffers de DirectSound vivos: éste no suena\n", MAX_BUFS); return; }
    audio_lock();
    ds_bufs[i] = b;
    if (i == ds_nbufs) ds_nbufs++;
    audio_unlock();
}

static uint32_t bytes_per_sec(DSBuf *b)
{
    uint32_t rate = b->freq ? b->freq : b->rate;
    return rate * b->channels * (b->bits / 8u);
}

uint32_t ds_position(DSBuf *b)
{
    if (!b->playing || !b->bytes) return b->start_pos;
    uint64_t adv = (uint64_t)(win_ticks_ms() - b->start_ms) * bytes_per_sec(b) / 1000u;
    uint32_t align = b->channels * (b->bits / 8u);
    uint64_t pos = b->start_pos + adv;
    if (!b->looping && pos >= b->bytes) { b->playing = 0; b->start_pos = 0; return 0; }
    pos %= b->bytes;
    return align ? (uint32_t)(pos - pos % align) : (uint32_t)pos;
}

static void read_format(DSBuf *b, uint32_t wfx)
{
    if (!wfx) { b->rate = 22050; b->channels = 2; b->bits = 16; return; }
    b->channels = rd16(g_mem, wfx + 2);
    b->rate = rd32(g_mem, wfx + 4);
    b->bits = rd16(g_mem, wfx + 14);
    if (!b->channels) b->channels = 1;
    if (b->bits != 8 && b->bits != 16) b->bits = 16;
}

/* ---- IDirectSound ---- */
static void m_ds_qi(void) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), 0); ret(E_NOINTERFACE, 2); }
static void m_ds_addref(void) { ret(2, 0); }
static void m_ds_release(void) { ret(1, 0); }
static void m_ds_coop(void) { ret(DS_OK, 2); }
static void m_ds_getcaps(void)
{
    uint32_t p = rt_arg(1);
    uint32_t size = rd32(g_mem, p);
    if (size < 8 || size > 0x200) size = 96;
    memset(g_mem + p + 4, 0, size - 4);
    wr32(g_mem, p + 4, 0x0F0Fu);                  /* PRIMARY/SECONDARY 8/16 bits mono/estéreo */
    wr32(g_mem, p + 8, 4000); wr32(g_mem, p + 12, 48000);
    wr32(g_mem, p + 16, 1);
    wr32(g_mem, p + 20, 32);
    ret(DS_OK, 1);
}
static void m_ds_ok0(void) { ret(DS_OK, 0); }
static void m_ds_speaker(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 4); ret(DS_OK, 1); }   /* DSSPEAKER_STEREO */
static void m_ds_ok1(void) { ret(DS_OK, 1); }

/* CreateSoundBuffer(this, DSBUFFERDESC*, lplpBuf, outer): {dwSize, dwFlags, dwBufferBytes, dwReserved, lpwfxFormat} */
static void m_ds_createbuffer(void)
{
    uint32_t desc = rt_arg(1), out = rt_arg(2);
    DSBuf *b = calloc(1, sizeof *b);
    b->refs = 1;
    b->flags = rd32(g_mem, desc + 4);
    b->primary = (b->flags & 1u) != 0;            /* DSBCAPS_PRIMARYBUFFER */
    read_format(b, rd32(g_mem, desc + 16));
    b->bytes = b->primary ? 65536u : rd32(g_mem, desc + 8);
    b->mem = rt_alloc_zero(b->bytes ? b->bytes : 4);
    b->mem_refs = calloc(1, sizeof *b->mem_refs);
    *b->mem_refs = 1;
    b->obj = com_new(vt_buf, b, 0);
    ds_add(b);
    WIN_TRACE("CreateSoundBuffer(%u bytes, %u Hz, %u ch, %u bits, flags %#x) = %#x\n", b->bytes, b->rate, b->channels, b->bits, b->flags, b->obj);
    wr32(g_mem, out, b->obj);
    ret(DS_OK, 3);
}
static void m_ds_duplicate(void)
{
    DSBuf *src = com_ptr(rt_arg(1));
    DSBuf *b = calloc(1, sizeof *b);
    *b = *src;
    b->refs = 1; b->playing = 0; b->start_pos = 0;
    ++*b->mem_refs;
    b->obj = com_new(vt_buf, b, 0);
    ds_add(b);
    wr32(g_mem, rt_arg(2), b->obj);
    ret(DS_OK, 2);
}

/* ---- IDirectSoundBuffer ---- */
static const char *IID_IDirectSound3DListener = "279afa84-4981-11ce-a521-0020af0be560";
static void m_b_qi(void)
{
    uint32_t iid = rt_arg(1), out = rt_arg(2);
    if (com_guid_is(iid, IID_IDirectSound3DListener)) {
        if (!listener_obj) listener_obj = com_new(vt_listener, NULL, 0);
        wr32(g_mem, out, listener_obj);
        ret(DS_OK, 2);
        return;
    }
    char t[40];
    com_guid_text(iid, t);
    fprintf(stderr, "recomp: IDirectSoundBuffer::QueryInterface(%s): no\n", t);
    if (out) wr32(g_mem, out, 0);
    ret(E_NOINTERFACE, 2);
}
static DSBuf *buf(void) { return (DSBuf *)com_ptr(rt_arg(0)); }
static void m_b_addref(void) { ret(++buf()->refs, 0); }
/* Al llegar a cero se da de baja (fuera del mezclador) y se libera; la memoria del buffer, cuando ya no la comparte
 * ningún duplicado. */
static void m_b_release(void)
{
    uint32_t obj = rt_arg(0);
    DSBuf *b = com_ptr(obj);
    if (b->refs) b->refs--;
    if (b->refs) { ret(b->refs, 0); return; }
    audio_lock();
    for (int i = 0; i < ds_nbufs; i++) if (ds_bufs[i] == b) ds_bufs[i] = NULL;
    while (ds_nbufs && !ds_bufs[ds_nbufs - 1]) ds_nbufs--;
    audio_unlock();
    if (b->mem_refs && !--*b->mem_refs) { rt_free(b->mem); free(b->mem_refs); }
    com_free(obj);
    free(b);
    ret(0, 0);
}
static void m_b_getcaps(void) { uint32_t p = rt_arg(1); wr32(g_mem, p + 4, buf()->flags); wr32(g_mem, p + 8, buf()->bytes); ret(DS_OK, 1); }
static void m_b_getpos(void)
{
    DSBuf *b = buf();
    uint32_t pos = ds_position(b);
    if (rt_arg(1)) wr32(g_mem, rt_arg(1), pos);
    if (rt_arg(2)) wr32(g_mem, rt_arg(2), b->bytes ? (pos + bytes_per_sec(b) / 50u) % b->bytes : 0);   /* escritura: 20 ms adelante */
    ret(DS_OK, 2);
}
static void m_b_getformat(void)
{
    DSBuf *b = buf();
    uint32_t p = rt_arg(1);
    if (p) {
        wr16(g_mem, p, 1); wr16(g_mem, p + 2, b->channels); wr32(g_mem, p + 4, b->rate);
        wr32(g_mem, p + 8, bytes_per_sec(b)); wr16(g_mem, p + 12, b->channels * b->bits / 8u); wr16(g_mem, p + 14, b->bits);
    }
    if (rt_arg(3)) wr32(g_mem, rt_arg(3), 16);
    ret(DS_OK, 3);
}
static void m_b_getvolume(void) { wr32(g_mem, rt_arg(1), (uint32_t)buf()->volume); ret(DS_OK, 1); }
static void m_b_getpan(void) { wr32(g_mem, rt_arg(1), (uint32_t)buf()->pan); ret(DS_OK, 1); }
static void m_b_getfreq(void) { DSBuf *b = buf(); wr32(g_mem, rt_arg(1), b->freq ? b->freq : b->rate); ret(DS_OK, 1); }
static void m_b_getstatus(void)
{
    DSBuf *b = buf();
    ds_position(b);
    wr32(g_mem, rt_arg(1), b->playing ? (1u | (b->looping ? 4u : 0u)) : 0u);
    ret(DS_OK, 1);
}
static void m_b_init(void) { ret(DS_OK, 2); }

/* Lock(this, offset, bytes, &p1, &n1, &p2, &n2, flags): circular; DSBLOCK_FROMWRITECURSOR 1, ENTIREBUFFER 2. */
static void m_b_lock(void)
{
    DSBuf *b = buf();
    uint32_t off = rt_arg(1), n = rt_arg(2), p1 = rt_arg(3), n1 = rt_arg(4), p2 = rt_arg(5), n2 = rt_arg(6), flags = rt_arg(7);
    if (flags & 2u) { off = 0; n = b->bytes; }
    if (flags & 1u) off = (ds_position(b) + bytes_per_sec(b) / 50u) % (b->bytes ? b->bytes : 1);
    if (!b->bytes) { ret(0x80070057u, 7); return; }
    off %= b->bytes;
    if (n > b->bytes) n = b->bytes;
    uint32_t first = n <= b->bytes - off ? n : b->bytes - off;
    wr32(g_mem, p1, b->mem + off);
    wr32(g_mem, n1, first);
    if (p2) wr32(g_mem, p2, first < n ? b->mem : 0);
    if (n2) wr32(g_mem, n2, n - first);
    ret(DS_OK, 7);
}
static void m_b_unlock(void) { ret(DS_OK, 4); }
static void m_b_play(void)
{
    extern void audio_start(void);
    audio_start();
    DSBuf *b = buf();
    if (!b->playing) { b->start_ms = win_ticks_ms(); b->playing = 1; }
    b->looping = (rt_arg(3) & 1u) != 0;
    ret(DS_OK, 3);
}
static void m_b_setpos(void) { DSBuf *b = buf(); b->start_pos = b->bytes ? rt_arg(1) % b->bytes : 0; b->start_ms = win_ticks_ms(); ret(DS_OK, 1); }
static void m_b_setformat(void) { read_format(buf(), rt_arg(1)); ret(DS_OK, 1); }
static void m_b_setvolume(void) { buf()->volume = (int32_t)rt_arg(1); ret(DS_OK, 1); }
static void m_b_setpan(void) { buf()->pan = (int32_t)rt_arg(1); ret(DS_OK, 1); }
static void m_b_setfreq(void)
{
    DSBuf *b = buf();
    uint32_t pos = ds_position(b);
    b->freq = rt_arg(1);
    b->start_pos = pos; b->start_ms = win_ticks_ms();
    ret(DS_OK, 1);
}
static void m_b_stop(void) { DSBuf *b = buf(); b->start_pos = ds_position(b); b->playing = 0; ret(DS_OK, 0); }
static void m_b_restore(void) { ret(DS_OK, 0); }

/* ---- IDirectSound3DListener: se pide y se libera (directx-com-layer.md § 2); todo devuelve OK. ---- */
static void m_l_ok0(void) { ret(DS_OK, 0); }
static void m_l_one(void) { ret(DS_OK, 1); }
static void m_l_two(void) { ret(DS_OK, 2); }
static void m_l_four(void) { ret(DS_OK, 4); }
static void m_l_seven(void) { ret(DS_OK, 7); }

#define M(n, f) {n, f}
static void build(void)
{
    if (vt_ds) return;
    const ComMethod ds[11] = {M("QueryInterface", m_ds_qi), M("AddRef", m_ds_addref), M("Release", m_ds_release),
                              M("CreateSoundBuffer", m_ds_createbuffer), M("GetCaps", m_ds_getcaps), M("DuplicateSoundBuffer", m_ds_duplicate),
                              M("SetCooperativeLevel", m_ds_coop), M("Compact", m_ds_ok0), M("GetSpeakerConfig", m_ds_speaker),
                              M("SetSpeakerConfig", m_ds_ok1), M("Initialize", m_ds_ok1)};
    vt_ds = com_vtable("IDirectSound", ds, 11);
    const ComMethod b[21] = {M("QueryInterface", m_b_qi), M("AddRef", m_b_addref), M("Release", m_b_release), M("GetCaps", m_b_getcaps),
                             M("GetCurrentPosition", m_b_getpos), M("GetFormat", m_b_getformat), M("GetVolume", m_b_getvolume),
                             M("GetPan", m_b_getpan), M("GetFrequency", m_b_getfreq), M("GetStatus", m_b_getstatus), M("Initialize", m_b_init),
                             M("Lock", m_b_lock), M("Play", m_b_play), M("SetCurrentPosition", m_b_setpos), M("SetFormat", m_b_setformat),
                             M("SetVolume", m_b_setvolume), M("SetPan", m_b_setpan), M("SetFrequency", m_b_setfreq), M("Stop", m_b_stop),
                             M("Unlock", m_b_unlock), M("Restore", m_b_restore)};
    vt_buf = com_vtable("IDirectSoundBuffer", b, 21);
    ComMethod l[18];
    l[0] = (ComMethod)M("QueryInterface", m_ds_qi); l[1] = (ComMethod)M("AddRef", m_ds_addref); l[2] = (ComMethod)M("Release", m_ds_release);
    const char *names[] = {"GetAllParameters", "GetDistanceFactor", "GetDopplerFactor", "GetOrientation", "GetPosition", "GetRolloffFactor",
                           "GetVelocity", "SetAllParameters", "SetDistanceFactor", "SetDopplerFactor", "SetOrientation", "SetPosition",
                           "SetRolloffFactor", "SetVelocity", "CommitDeferredSettings"};
    RtFn fns[] = {m_l_one, m_l_one, m_l_one, m_l_two, m_l_one, m_l_one, m_l_one, m_l_two, m_l_two, m_l_two, m_l_seven, m_l_four,
                  m_l_two, m_l_four, m_l_ok0};
    for (int i = 0; i < 15; i++) { l[3 + i].name = names[i]; l[3 + i].fn = fns[i]; }
    vt_listener = com_vtable("IDirectSound3DListener", l, 18);
}

static void host_DirectSoundCreate(void)
{
    build();
    if (!ds_obj) ds_obj = com_new(vt_ds, NULL, 0);
    wr32(g_mem, rt_arg(1), ds_obj);
    fprintf(stderr, "recomp: DirectSoundCreate = %#x\n", ds_obj);
    rt_ret_stdcall(DS_OK, 3);
}
static void host_DirectSoundEnumerateA(void)
{
    uint32_t cb = rt_arg(0), ctx = rt_arg(1);
    Cpu s = cpu;
    uint32_t desc = win_static_alloc(32), mod = win_static_alloc(16);
    win_put_str(desc, "Primary Sound Driver", 32);
    uint32_t a[4] = {0, desc, mod, ctx};
    win_call(cb, 4, a);
    cpu = s;
    rt_ret_stdcall(DS_OK, 2);
}

/* D3DRM: no alcanzable en el juego (directx-com-layer.md § 2); si alguna vez se pide, falla y queda en el log. */
static void host_Direct3DRMCreate(void)
{
    fprintf(stderr, "recomp: Direct3DRMCreate pedido (no soportado: partido gráfico 3D, fase 5)\n");
    if (rt_arg(0)) wr32(g_mem, rt_arg(0), 0);
    rt_ret_stdcall(E_FAIL, 1);
}

void dsound_register(void)
{
    win_register_proc("dsound.dll", "DirectSoundCreate", host_DirectSoundCreate);
    win_register_proc("dsound.dll", "DirectSoundEnumerateA", host_DirectSoundEnumerateA);
    win_register_proc("d3drm.dll", "Direct3DRMCreate", host_Direct3DRMCreate);
}
