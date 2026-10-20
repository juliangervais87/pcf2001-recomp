/* Objetos COM del runtime en la memoria plana (com.h). */
#include "com.h"

/* PC Fútbol 2001 crea cientos de superficies por pantalla (cada una es un objeto COM): tope alto y los liberados se
 * reusan (com_free). En una partida larga se pasó de 262.144: el tope sube a 786.432 (16 bytes cada uno en la región estática de 16 MB de kernel32.c, con
 * lugar para el framebuffer y las vtables; si no alcanzara, para con «memoria estática del runtime llena»). Liberar las
 * superficies de DirectDraw 1/2 como en Windows rompió (el juego llamó AddRef sobre una ya soltada: algún método nuestro
 * no suma la referencia que Windows suma): pendiente. */
#define MAX_OBJS 786432
static void *objs[MAX_OBJS];
static uint32_t nobjs = 1;
static uint32_t free_objs[MAX_OBJS], nfree;              /* objetos de 8 bytes libres, para reusar */

/* Un método que falta: para con el nombre de la interfaz y el número de método (lo pide el juego). */
static const char *missing_iface[64];
static int nmissing;
static void missing_stub(void)
{
    /* rt_call_target dice qué dirección se llamó; rt_name_of la traduce al nombre registrado */
    rt_fatal("método COM sin reemplazo: %s (llamado desde %#x)", rt_name_of(rt_call_target), rd32(g_mem, cpu.esp));
}

/* Traza de métodos (RECOMP_TRACE=2): cada método pasa por traced(), que busca el real por la dirección llamada y
 * escribe «Interfaz::Método -> hresult». Sirve para ver qué llamada COM devolvió el error que el juego reporta. */
#define MAX_TRACED 2048
static uint32_t traced_addr[MAX_TRACED];
static RtFn traced_fn[MAX_TRACED];
static const char *traced_name[MAX_TRACED];
static int ntraced;
static void traced(void)
{
    uint32_t a = rt_call_target, from = rd32(g_mem, cpu.esp);
    for (int i = 0; i < ntraced; i++)
        if (traced_addr[i] == a) {
            traced_fn[i]();
            fprintf(stderr, "COM %s -> %#x (desde %#x)\n", traced_name[i], cpu.eax, from);
            return;
        }
    rt_fatal("método COM trazado sin registro: %#x", a);
}

uint32_t com_vtable(const char *iface, const ComMethod *methods, int count)
{
    uint32_t vt = win_static_alloc(4u * (uint32_t)count);
    for (int i = 0; i < count; i++) {
        const char *name = methods[i].name;
        RtFn fn = methods[i].fn;
        uint32_t addr;
        if (!fn) {
            char *n = malloc(96);
            snprintf(n, 96, "%s::%s", iface, name ? name : "?");
            name = n;
            fn = missing_stub;
            if (nmissing < 64) missing_iface[nmissing++] = n;
            addr = rt_host_fn(name, fn);
        } else if (win_trace >= 2 && ntraced < MAX_TRACED) {
            char *n = malloc(96);
            snprintf(n, 96, "%s::%s", iface, name ? name : "?");
            addr = rt_host_fn(n, traced);
            traced_addr[ntraced] = addr;
            traced_fn[ntraced] = fn;
            traced_name[ntraced++] = n;
        } else {
            addr = rt_host_fn(name, fn);
        }
        wr32(g_mem, vt + 4u * (uint32_t)i, addr);
    }
    return vt;
}

uint32_t com_new(uint32_t vtable, void *host, uint32_t extra)
{
    if (!extra && nfree) {
        uint32_t obj = free_objs[--nfree], id = rd32(g_mem, obj + 4);
        wr32(g_mem, obj, vtable);
        objs[id] = host;
        return obj;
    }
    if (nobjs == MAX_OBJS) rt_fatal("demasiados objetos COM");
    uint32_t obj = win_static_alloc(8 + extra);
    wr32(g_mem, obj, vtable);
    wr32(g_mem, obj + 4, nobjs);
    objs[nobjs++] = host;
    return obj;
}

/* Un objeto (sin bytes extra) que ya nadie usa: su lugar se reusa en el próximo com_new. */
void com_free(uint32_t obj)
{
    uint32_t id = rd32(g_mem, obj + 4);
    if (!id || id >= nobjs || nfree == MAX_OBJS) return;
    objs[id] = NULL;
    wr32(g_mem, obj, 0);
    free_objs[nfree++] = obj;
}
void *com_ptr(uint32_t obj)
{
    uint32_t id = rd32(g_mem, obj + 4);
    return id && id < nobjs ? objs[id] : NULL;
}

void com_guid_text(uint32_t p, char *out)
{
    const uint8_t *b = g_mem + p;
    sprintf(out, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", rd32(g_mem, p), rd16(g_mem, p + 4), rd16(g_mem, p + 6),
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

int com_guid_is(uint32_t p, const char *text)
{
    char t[40];
    com_guid_text(p, t);
    return !strcasecmp(t, text);
}

/* ole32 (PC Fútbol 2001). Ningún servidor COM registrado: CoCreateInstance avisa qué clase pidió y falla como en
 * una máquina sin ese componente (REGDB_E_CLASSNOTREG). */
RT_IMPORT(CoInitialize) { rt_ret_stdcall(0, 1); }
RT_IMPORT(CoUninitialize) { rt_ret_stdcall(0, 0); }
RT_IMPORT(CoFreeUnusedLibraries) { rt_ret_stdcall(0, 0); }
/* DirectMusic (CLSID 636b9f10-…): d2s2.dll del 2001 lo exige para arrancar su sonido (le pasa el DirectSound con
 * SetDirectSound) aunque la música es MP3 que decodifica él; con DirectMusic sólo cuenta puertos MIDI. Acá no hay
 * puertos MIDI (EnumPort termina enseguida) y el resto acepta. */
static uint32_t dmusic_obj;
static void dm_qi(void)
{
    uint32_t iid = rt_arg(1), out = rt_arg(2);
    int ok = com_guid_is(iid, "6536115a-7b2d-11d2-ba18-0000f875ac12") || com_guid_is(iid, "6fc2cae1-bc78-11d2-afa6-00aa0024d8b6")
          || com_guid_is(iid, "00000000-0000-0000-c000-000000000046");
    if (out) wr32(g_mem, out, ok ? dmusic_obj : 0);
    rt_ret_stdcall(ok ? 0 : E_NOINTERFACE, 3);
}
static void dm_ref(void) { rt_ret_stdcall(1, 1); }
static void dm_none2(void) { rt_ret_stdcall(1, 3); }                                  /* EnumPort / EnumMasterClock: S_FALSE */
static void dm_notimpl3(void) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), 0); rt_ret_stdcall(E_NOTIMPL, 4); }   /* CreateMusicBuffer */
static void dm_createport(void) { if (rt_arg(3)) wr32(g_mem, rt_arg(3), 0); rt_ret_stdcall(E_NOTIMPL, 5); }
static void dm_getclock(void) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), 0); rt_ret_stdcall(E_NOTIMPL, 3); }
static void dm_ok1(void) { rt_ret_stdcall(0, 2); }
static void dm_ok2(void) { rt_ret_stdcall(0, 3); }
static void dm_defport(void) { if (rt_arg(1)) memset(g_mem + rt_arg(1), 0, 16); rt_ret_stdcall(0, 2); }
static uint32_t dmusic_object(void)
{
    if (!dmusic_obj) {
        static const ComMethod m[13] = {{"QueryInterface", dm_qi}, {"AddRef", dm_ref}, {"Release", dm_ref},
                                        {"EnumPort", dm_none2}, {"CreateMusicBuffer", dm_notimpl3}, {"CreatePort", dm_createport},
                                        {"EnumMasterClock", dm_none2}, {"GetMasterClock", dm_getclock},
                                        {"SetMasterClock", dm_ok1}, {"Activate", dm_ok1}, {"GetDefaultPort", dm_defport},
                                        {"SetDirectSound", dm_ok2}, {"SetExternalMasterClock", dm_ok1}};
        dmusic_obj = com_new(com_vtable("IDirectMusic2", m, 13), NULL, 0);
    }
    return dmusic_obj;
}

RT_IMPORT(CoCreateInstance)
{
    if (com_guid_is(rt_arg(0), "636b9f10-0c7d-11d1-95b2-0020afdc7421")) {      /* CLSID_DirectMusic */
        uint32_t out = rt_arg(4);
        if (out) wr32(g_mem, out, dmusic_object());
        rt_ret_stdcall(0, 5);
        return;
    }
    if (com_guid_is(rt_arg(0), "e436ebb3-524f-11ce-9f53-0020af0ba770")) {      /* CLSID_FilterGraph (dshow.c) */
        extern uint32_t dshow_filter_graph(uint32_t iid);
        uint32_t out = rt_arg(4);
        if (out) wr32(g_mem, out, dshow_filter_graph(rt_arg(3)));
        rt_ret_stdcall(0, 5);
        return;
    }
    char clsid[40], iid[40];
    com_guid_text(rt_arg(0), clsid);
    com_guid_text(rt_arg(3), iid);
    fprintf(stderr, "recomp: CoCreateInstance(%s, %s): clase no registrada\n", clsid, iid);
    if (rt_arg(4)) wr32(g_mem, rt_arg(4), 0);
    rt_ret_stdcall(0x80040154u, 5);
}
RT_IMPORT(CoTaskMemAlloc) { rt_ret_stdcall(rt_alloc(rt_arg(0)), 1); }
RT_IMPORT(CoTaskMemFree) { if (rt_arg(0)) rt_free(rt_arg(0)); rt_ret_stdcall(0, 1); }
/* CoCreateGuid: determinista (contador), para que dos corridas den lo mismo. */
RT_IMPORT(CoCreateGuid)
{
    static uint32_t n;
    uint32_t p = rt_arg(0);
    wr32(g_mem, p, 0x1997AB97u);
    wr32(g_mem, p + 4, 0x20010000u);
    wr32(g_mem, p + 8, 0x0u);
    wr32(g_mem, p + 12, ++n);
    rt_ret_stdcall(0, 1);
}

/* Lo que MFC pide de ole32 con GetProcAddress (AfxOleInit, PC Fútbol 2001 al salir de la pretemporada): OLE sin
 * servidores ni portapapeles, todo responde que anduvo. */
static void ole_initialize(void) { rt_ret_stdcall(0, 1); }
static void ole_uninitialize(void) { rt_ret_stdcall(0, 0); }
static void ole_message_filter(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 0); rt_ret_stdcall(0, 2); }
static void ole_flush_clipboard(void) { rt_ret_stdcall(0, 0); }
static void ole_is_current_clipboard(void) { rt_ret_stdcall(1, 1); }   /* S_FALSE: no es nuestro */
void ole32_register(void)
{
    win_register_proc("ole32.dll", "OleInitialize", ole_initialize);
    win_register_proc("ole32.dll", "OleUninitialize", ole_uninitialize);
    win_register_proc("ole32.dll", "CoRegisterMessageFilter", ole_message_filter);
    win_register_proc("ole32.dll", "OleFlushClipboard", ole_flush_clipboard);
    win_register_proc("ole32.dll", "OleIsCurrentClipboard", ole_is_current_clipboard);
    void ole32_register_more(void);
    ole32_register_more();
}

/* El resto de lo que MFC carga de ole32 a medida que lo usa (CWnd::CreateControl, servidores OLE...): en esta máquina no
 * hay componentes registrados, así que crear o buscar una clase falla como en un Windows sin ese componente, y se anota
 * cuál se pidió. Registrar, bloquear y desconectar objetos propios anda. */
static void co_get_class_object(void)
{
    uint32_t webbrowser_factory(uint32_t clsid);
    uint32_t factory = webbrowser_factory(rt_arg(0));         /* el WebBrowser de IE: webbrowser.c */
    if (factory) {
        if (rt_arg(4)) wr32(g_mem, rt_arg(4), factory);
        rt_ret_stdcall(0, 5);
        return;
    }
    char clsid[40];
    com_guid_text(rt_arg(0), clsid);
    fprintf(stderr, "recomp: CoGetClassObject(%s): clase no registrada\n", clsid);
    if (rt_arg(4)) wr32(g_mem, rt_arg(4), 0);
    rt_ret_stdcall(0x80040154u, 5);                           /* REGDB_E_CLASSNOTREG */
}
static void clsid_from_progid(void)
{
    char name[128] = "";
    for (uint32_t i = 0, p = rt_arg(0); p && i < sizeof name - 1 && rd16(g_mem, p + 2 * i); i++)
        name[i] = (char)rd16(g_mem, p + 2 * i), name[i + 1] = 0;
    fprintf(stderr, "recomp: CLSIDFromProgID(%s): no registrado\n", name);
    if (rt_arg(1)) memset(g_mem + rt_arg(1), 0, 16);
    rt_ret_stdcall(0x800401F3u, 2);                           /* CO_E_CLASSSTRING */
}
static void co_ok1(void) { rt_ret_stdcall(0, 1); }
static void co_ok2(void) { rt_ret_stdcall(0, 2); }
static void co_ok3(void) { rt_ret_stdcall(0, 3); }
static void co_register_class(void) { if (rt_arg(4)) wr32(g_mem, rt_arg(4), 1); rt_ret_stdcall(0, 5); }
static void co_fail1(void) { if (rt_arg(0)) wr32(g_mem, rt_arg(0), 0); rt_ret_stdcall(E_FAIL, 1); }
static void co_fail2(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 0); rt_ret_stdcall(E_FAIL, 2); }
void ole32_register_more(void)
{
    win_register_proc("ole32.dll", "CoGetClassObject", co_get_class_object);
    win_register_proc("ole32.dll", "CLSIDFromProgID", clsid_from_progid);
    win_register_proc("ole32.dll", "CoLockObjectExternal", co_ok3);
    win_register_proc("ole32.dll", "CoDisconnectObject", co_ok2);
    win_register_proc("ole32.dll", "CoRegisterClassObject", co_register_class);
    win_register_proc("ole32.dll", "CoRevokeClassObject", co_ok1);
    win_register_proc("ole32.dll", "OleGetClipboard", co_fail1);
    win_register_proc("ole32.dll", "GetRunningObjectTable", co_fail2);
}
