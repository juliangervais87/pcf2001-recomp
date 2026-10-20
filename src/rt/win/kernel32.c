/* KERNEL32: versión (Windows 98), memoria (heap, Global/Local, Virtual), proceso y módulos, carga dinámica
 * (LoadLibraryA/GetProcAddress: DirectDraw, DirectSound, D3DRM), tiempo, cadenas y lo chico. Archivos en
 * files.c, hilos y espera en threads.c, excepciones en seh.c, código de página en locale.c.
 * Procedencia: docs/procedencia.md. */
#include "win.h"
#include "core/host.h"
#include "loader.h"

#include <ctype.h>
#include <time.h>

uint32_t win_last_error;
int win_trace;
char win_command_line[1024];                    /* main.c: "<exe del perfil>" */

/* Versión: Windows 98 SE (4.10.2222). En 9x el bit alto de GetVersion está prendido; MSVCRT y MFC toman los
 * caminos de 9x, que son los que el juego conoció. */
RT_IMPORT(GetVersion) { rt_ret_stdcall(0xC0000A04u, 0); }

RT_IMPORT(GetVersionExA)
{
    uint32_t info = rt_arg(0);
    wr32(g_mem, info + 4, 4);
    wr32(g_mem, info + 8, 10);
    wr32(g_mem, info + 12, 0x040A08AEu);           /* 9x: la versión en la palabra alta del build */
    wr32(g_mem, info + 16, 1);                     /* VER_PLATFORM_WIN32_WINDOWS */
    win_put_str(info + 20, " A ", 128);
    rt_ret_stdcall(1, 1);
}

/* --- Memoria --- */
#define HEAP_HANDLE 0x00150000u
#define HEAP_ZERO_MEMORY 8u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u

RT_IMPORT(HeapCreate) { rt_ret_stdcall(HEAP_HANDLE, 3); }
RT_IMPORT(HeapDestroy) { rt_ret_stdcall(1, 1); }
RT_IMPORT(GetProcessHeap) { rt_ret_stdcall(HEAP_HANDLE, 0); }
RT_IMPORT(HeapAlloc)
{
    uint32_t flags = rt_arg(1), size = rt_arg(2);
    rt_ret_stdcall(flags & HEAP_ZERO_MEMORY ? rt_alloc_zero(size) : rt_alloc(size), 3);
}
/* Liberar un puntero que no es un bloque: Windows devuelve error y sigue (en 9x, sin quejarse); acá tampoco
 * se para. Se anota (las primeras veces, con quién llamó) y se devuelve FALSE. Visto en la pantalla de elegir equipo
 * desde free() de MSVCRT: puede ser una rareza del original. */
static int free_ok(uint32_t p)
{
    if (!p) return 1;
    if (rt_is_block(p)) { rt_free(p); return 1; }
    static uint32_t warned;
    if (warned++ < 20) fprintf(stderr, "recomp: aviso: se libera %#x, que no es un bloque del heap (desde %#x); se ignora como Windows\n",
                               p, rd32(g_mem, cpu.esp));
    win_last_error = ERROR_INVALID_PARAMETER;
    return 0;
}
RT_IMPORT(HeapFree) { rt_ret_stdcall((uint32_t)free_ok(rt_arg(2)), 3); }
RT_IMPORT(HeapReAlloc)
{
    uint32_t flags = rt_arg(1), block = rt_arg(2), size = rt_arg(3);
    uint32_t old = block ? rt_block_size(block) : 0;
    uint32_t n = rt_realloc(block, size, (flags & HEAP_REALLOC_IN_PLACE_ONLY) != 0);
    if (n && (flags & HEAP_ZERO_MEMORY) && size > old) memset(g_mem + n + old, 0, size - old);
    rt_ret_stdcall(n, 4);
}
RT_IMPORT(HeapSize) { uint32_t p = rt_arg(2); rt_ret_stdcall(rt_is_block(p) ? rt_block_size(p) : 0xffffffffu, 3); }

/* Global* y Local*: memoria fija (el handle ES el puntero); GMEM_ZEROINIT = 0x40. */
static uint32_t g_alloc(uint32_t flags, uint32_t size) { return flags & 0x40 ? rt_alloc_zero(size ? size : 1) : rt_alloc(size ? size : 1); }
static uint32_t g_realloc(uint32_t h, uint32_t size, uint32_t flags)
{
    uint32_t old = h ? rt_block_size(h) : 0;
    uint32_t n = rt_realloc(h, size ? size : 1, 0);
    if (n && (flags & 0x40) && size > old) memset(g_mem + n + old, 0, size - old);
    return n;
}
RT_IMPORT(GlobalAlloc) { rt_ret_stdcall(g_alloc(rt_arg(0), rt_arg(1)), 2); }
RT_IMPORT(GlobalFree) { uint32_t p = rt_arg(0); rt_ret_stdcall(free_ok(p) ? 0 : p, 1); }
RT_IMPORT(GlobalReAlloc) { rt_ret_stdcall(g_realloc(rt_arg(0), rt_arg(1), rt_arg(2)), 3); }
RT_IMPORT(GlobalLock) { rt_ret_stdcall(rt_arg(0), 1); }
RT_IMPORT(GlobalUnlock) { rt_ret_stdcall(1, 1); }
RT_IMPORT(GlobalHandle) { rt_ret_stdcall(rt_arg(0), 1); }
RT_IMPORT(GlobalFlags) { rt_ret_stdcall(0, 1); }
RT_IMPORT(GlobalSize) { rt_ret_stdcall(rt_arg(0) ? rt_block_size(rt_arg(0)) : 0, 1); }
RT_IMPORT(LocalAlloc) { rt_ret_stdcall(g_alloc(rt_arg(0), rt_arg(1)), 2); }
RT_IMPORT(LocalFree) { uint32_t p = rt_arg(0); rt_ret_stdcall(free_ok(p) ? 0 : p, 1); }
RT_IMPORT(LocalReAlloc) { rt_ret_stdcall(g_realloc(rt_arg(0), rt_arg(1), rt_arg(2)), 3); }
RT_IMPORT(LocalLock) { rt_ret_stdcall(rt_arg(0), 1); }
RT_IMPORT(LocalUnlock) { rt_ret_stdcall(1, 1); }

/* Memoria estática del runtime (docs/mapa-de-memoria.md): 0x11000000-0x12000000. */
#define STATIC_BASE 0x11000000u
#define STATIC_SIZE 0x01000000u
static uint32_t static_top = STATIC_BASE;
uint32_t win_static_alloc(uint32_t size)
{
    uint32_t a = static_top;
    static_top += (size + 15) & ~15u;
    if (static_top > STATIC_BASE + STATIC_SIZE) rt_fatal("memoria estática del runtime llena");
    memset(g_mem + a, 0, size);
    return a;
}

/* VirtualAlloc: regiones alineadas a 64 KB en 0x12000000-0x1A000000. La memoria plana ya existe: reservar es
 * entregar una dirección; comprometer dentro de una región ya reservada devuelve esa dirección. */
#define VIRT_BASE 0x12000000u
#define VIRT_SIZE 0x08000000u
static uint32_t virt_top = VIRT_BASE;
RT_IMPORT(VirtualAlloc)
{
    uint32_t addr = rt_arg(0), size = rt_arg(1);
    if (addr && addr >= VIRT_BASE && addr < virt_top) { rt_ret_stdcall(addr & ~0xfffu, 4); return; }
    uint32_t a = (virt_top + 0xffffu) & ~0xffffu;
    uint32_t n = (size + 0xffffu) & ~0xffffu;
    if (a + n > VIRT_BASE + VIRT_SIZE) { win_last_error = 8; rt_ret_stdcall(0, 4); return; }
    virt_top = a + n;
    memset(g_mem + a, 0, n);
    rt_ret_stdcall(a, 4);
}
RT_IMPORT(VirtualFree) { rt_ret_stdcall(1, 3); }
RT_IMPORT(VirtualProtect) { if (rt_arg(3)) wr32(g_mem, rt_arg(3), 4); rt_ret_stdcall(1, 4); }
RT_IMPORT(VirtualQuery)
{
    uint32_t addr = rt_arg(0), buf = rt_arg(1), len = rt_arg(2);
    memset(g_mem + buf, 0, len);
    wr32(g_mem, buf, addr & ~0xfffu);
    wr32(g_mem, buf + 4, addr & ~0xffffu);
    wr32(g_mem, buf + 8, 4);
    wr32(g_mem, buf + 12, 0x1000);
    wr32(g_mem, buf + 16, 0x1000);                 /* MEM_COMMIT */
    wr32(g_mem, buf + 20, 4);
    wr32(g_mem, buf + 24, 0x20000);                /* MEM_PRIVATE */
    rt_ret_stdcall(28, 3);
}
RT_IMPORT(IsBadReadPtr) { rt_ret_stdcall(0, 2); }
RT_IMPORT(IsBadWritePtr) { rt_ret_stdcall(0, 2); }
RT_IMPORT(IsBadCodePtr) { rt_ret_stdcall(0, 1); }
RT_IMPORT(IsBadStringPtrA) { rt_ret_stdcall(0, 2); }

RT_IMPORT(GlobalMemoryStatus)
{
    uint32_t p = rt_arg(0);
    wr32(g_mem, p, 32);
    wr32(g_mem, p + 4, 20);
    wr32(g_mem, p + 8, 0x04000000u);               /* 64 MB: una máquina de 1998 */
    wr32(g_mem, p + 0xc, 0x02000000u);
    wr32(g_mem, p + 0x10, 0x08000000u);
    wr32(g_mem, p + 0x14, 0x06000000u);
    wr32(g_mem, p + 0x18, 0x7ffe0000u);
    wr32(g_mem, p + 0x1c, 0x70000000u);
    rt_ret_stdcall(0, 1);
}

RT_IMPORT(GetSystemInfo)
{
    uint32_t p = rt_arg(0);
    memset(g_mem + p, 0, 36);
    wr32(g_mem, p + 4, 4096);
    wr32(g_mem, p + 8, 0x10000);
    wr32(g_mem, p + 0xc, 0x7ffeffff);
    wr32(g_mem, p + 0x10, 1);
    wr32(g_mem, p + 0x14, 1);
    wr32(g_mem, p + 0x18, 586);
    wr32(g_mem, p + 0x1c, 0x10000);
    wr16(g_mem, p + 0x20, 5);
    rt_ret_stdcall(0, 1);
}

/* --- Proceso --- */
RT_IMPORT(GetCommandLineA)
{
    static uint32_t line;
    if (!line) { uint32_t room = (uint32_t)strlen(win_command_line) + 1; line = win_static_alloc(room); win_put_str(line, win_command_line, room); }
    rt_ret_stdcall(line, 0);
}
RT_IMPORT(GetEnvironmentStrings) { static uint32_t b; if (!b) b = win_static_alloc(4); rt_ret_stdcall(b, 0); }
RT_IMPORT(GetEnvironmentStringsA) { static uint32_t b; if (!b) b = win_static_alloc(4); rt_ret_stdcall(b, 0); }
RT_IMPORT(GetEnvironmentStringsW) { rt_ret_stdcall(0, 0); }
RT_IMPORT(FreeEnvironmentStringsA) { rt_ret_stdcall(1, 1); }
RT_IMPORT(FreeEnvironmentStringsW) { rt_ret_stdcall(1, 1); }
RT_IMPORT(GetEnvironmentVariableA) { win_last_error = 203; rt_ret_stdcall(0, 3); }
RT_IMPORT(SetEnvironmentVariableA) { rt_ret_stdcall(1, 2); }
RT_IMPORT(GetStartupInfoA)
{
    uint32_t info = rt_arg(0);
    memset(g_mem + info, 0, 0x44);
    wr32(g_mem, info, 0x44);
    rt_ret_stdcall(0, 1);
}
#define STD_HANDLE_BASE 0xF0000000u
RT_IMPORT(GetStdHandle) { rt_ret_stdcall(STD_HANDLE_BASE + (uint32_t)(-(int32_t)rt_arg(0)), 1); }
RT_IMPORT(SetStdHandle) { rt_ret_stdcall(1, 2); }
RT_IMPORT(SetHandleCount) { rt_ret_stdcall(rt_arg(0), 1); }
RT_IMPORT(SetErrorMode) { static uint32_t mode; uint32_t old = mode; mode = rt_arg(0); rt_ret_stdcall(old, 1); }
RT_IMPORT(GetLastError) { rt_ret_stdcall(win_last_error, 0); }
RT_IMPORT(SetLastError) { win_last_error = rt_arg(0); rt_ret_stdcall(0, 1); }
RT_IMPORT(GetCurrentProcess) { rt_ret_stdcall(0xffffffffu, 0); }
RT_IMPORT(GetCurrentProcessId) { rt_ret_stdcall(0x1234, 0); }
RT_IMPORT(OutputDebugStringA) { WIN_TRACE("OutputDebugString: %s\n", win_str(rt_arg(0))); rt_ret_stdcall(0, 1); }
RT_IMPORT(ExitProcess)
{
    fprintf(stderr, "recomp: ExitProcess(%u)\n", rt_arg(0));
    fflush(NULL);
    exit((int)rt_arg(0));
}
RT_IMPORT(TerminateProcess)
{
    fprintf(stderr, "recomp: TerminateProcess(%u)\n", rt_arg(1));
    fflush(NULL);
    exit((int)rt_arg(1));
}
RT_IMPORT(DuplicateHandle)
{
    if (rt_arg(3)) wr32(g_mem, rt_arg(3), rt_arg(1));
    rt_ret_stdcall(1, 7);
}
/* CreateProcessA: el gestor lanza otros exe del CD (INFOSURF, DBASEARG) o el visionado. No se lanza nada:
 * se anota y se devuelve error, como si el exe no estuviera. (Medición pedida en docs/investigacion/fase-2.md.) */
RT_IMPORT(CreateProcessA)
{
    fprintf(stderr, "recomp: CreateProcessA(%s, %s) no se lanza\n",
            rt_arg(0) ? win_str(rt_arg(0)) : "-", rt_arg(1) ? win_str(rt_arg(1)) : "-");
    win_last_error = ERROR_FILE_NOT_FOUND;
    rt_ret_stdcall(0, 10);
}

/* --- Módulos: las imágenes cargadas (loader.c) y los módulos de Windows que se piden por nombre --- */
#define KERNEL32_HANDLE 0x7c800000u
#define USER32_HANDLE 0x7e400000u
#define GDI32_HANDLE 0x77f00000u
#define DDRAW_HANDLE 0x73700000u
#define DSOUND_HANDLE 0x73800000u
#define D3DRM_HANDLE 0x73900000u

static const char *base_name(const char *p)
{
    const char *s = p;
    for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') s = q + 1;
    return s;
}

static int name_is(const char *name, const char *want)          /* "mfc42" == "MFC42.DLL" */
{
    const char *b = base_name(name);
    size_t n = strlen(want);
    if (strncasecmp(b, want, n)) return 0;
    return b[n] == 0 || !strcasecmp(b + n, ".dll");
}

static uint32_t module_by_name(const char *name)
{
    for (uint32_t i = 0; i < rt_image_count; i++) {
        char stem[64];
        snprintf(stem, sizeof stem, "%s", rt_images[i].file);
        char *dot = strrchr(stem, '.');
        if (dot && rt_images[i].is_dll) *dot = 0;
        if (rt_images[i].is_dll ? name_is(name, stem) : !strcasecmp(base_name(name), stem)) return rt_images[i].base;
    }
    if (name_is(name, "kernel32")) return KERNEL32_HANDLE;
    if (name_is(name, "user32")) return USER32_HANDLE;
    if (name_is(name, "gdi32")) return GDI32_HANDLE;
    if (name_is(name, "ddraw")) return DDRAW_HANDLE;
    if (name_is(name, "dsound")) return DSOUND_HANDLE;
    if (name_is(name, "d3drm")) return D3DRM_HANDLE;
    /* Otras DLL del sistema que un juego carga a mano (MFC carga ole32 para sus funciones OLE; PC Fútbol 2001):
     * GetProcAddress les busca la función entre las que el runtime ya imita (rt_table). */
    static const char *const others[] = {"ole32", "oleaut32", "advapi32", "winmm", "comdlg32", "shell32", "version",
                                         "wsock32", "comctl32"};
    for (unsigned i = 0; i < sizeof others / sizeof *others; i++)
        if (name_is(name, others[i])) return 0x7a000000u + 0x100000u * i;
    return 0;
}

static uint32_t exe_base(void)
{
    for (uint32_t i = 0; i < rt_image_count; i++) if (!rt_images[i].is_dll) return rt_images[i].base;
    return 0x400000u;
}

RT_IMPORT(GetModuleHandleA)
{
    uint32_t name = rt_arg(0);
    if (!name) { rt_ret_stdcall(exe_base(), 1); return; }
    uint32_t m = module_by_name(win_str(name));
    WIN_TRACE("GetModuleHandleA(%s) = %#x\n", win_str(name), m);
    if (!m) win_last_error = 126;
    rt_ret_stdcall(m, 1);
}

RT_IMPORT(GetModuleFileNameA)
{
    uint32_t mod = rt_arg(0), buf = rt_arg(1), room = rt_arg(2);
    char path[300];
    snprintf(path, sizeof path, "%s", WIN_EXE_NAME);
    for (uint32_t i = 0; i < rt_image_count; i++)
        if (mod && rt_images[i].base == mod && rt_images[i].is_dll) snprintf(path, sizeof path, "C:\\WINDOWS\\SYSTEM\\%s", rt_images[i].file);
    win_put_str(buf, path, room);
    rt_ret_stdcall((uint32_t)strlen(win_str(buf)), 3);
}

RT_IMPORT(LoadLibraryA)
{
    const char *name = win_str(rt_arg(0));
    uint32_t m = module_by_name(name);
    fprintf(stderr, "recomp: LoadLibraryA(%s) = %#x\n", name, m);
    if (!m) win_last_error = 126;
    rt_ret_stdcall(m, 1);
}
RT_IMPORT(LoadLibraryExA) { uint32_t m = module_by_name(win_str(rt_arg(0))); if (!m) win_last_error = 126; rt_ret_stdcall(m, 3); }
RT_IMPORT(FreeLibrary) { rt_ret_stdcall(1, 1); }

/* Lo que GetProcAddress puede devolver de los módulos de Windows: funciones del host (ddraw.c, dsound.c, ...).
 * Cada una se registra con win_register_proc desde su archivo. */
#define MAX_PROCS 64
static struct { uint32_t module; const char *name; RtFn fn; } procs[MAX_PROCS];
static int nprocs;
void win_register_proc(const char *module, const char *name, RtFn fn)
{
    if (nprocs == MAX_PROCS) rt_fatal("demasiados procs");
    procs[nprocs].module = module_by_name(module);
    procs[nprocs].name = name;
    procs[nprocs].fn = fn;
    nprocs++;
}

/* Un export de una imagen traducida, por nombre u ordinal (lo pide MFC/MSVCRT con GetProcAddress). */
static uint32_t image_export(uint32_t base, uint32_t name)
{
    uint32_t pe = base + rd32(g_mem, base + 0x3c);
    uint32_t dir = rd32(g_mem, pe + 0x78);
    if (!dir) return 0;
    uint32_t d = base + dir;
    uint32_t ord_base = rd32(g_mem, d + 16), nf = rd32(g_mem, d + 20), nn = rd32(g_mem, d + 24);
    uint32_t af = base + rd32(g_mem, d + 28), an = base + rd32(g_mem, d + 32), ao = base + rd32(g_mem, d + 36);
    uint32_t idx = 0xffffffffu;
    if (name < 0x10000) idx = name - ord_base;
    else for (uint32_t i = 0; i < nn; i++)
        if (!strcmp(win_str(base + rd32(g_mem, an + 4 * i)), win_str(name))) { idx = rd16(g_mem, ao + 2 * i); break; }
    if (idx >= nf) return 0;
    uint32_t rva = rd32(g_mem, af + 4 * idx);
    return rva ? base + rva : 0;
}

RT_IMPORT(GetProcAddress)
{
    uint32_t module = rt_arg(0), name = rt_arg(1), result = 0;
    for (uint32_t i = 0; i < rt_image_count; i++)
        if (rt_images[i].base == module) result = image_export(module, name);
    if (!result && name >= 0x10000)
        for (int i = 0; i < nprocs; i++)
            if (procs[i].module == module && !strcmp(procs[i].name, win_str(name))) result = rt_host_fn(procs[i].name, procs[i].fn);
    /* De un módulo del sistema: el stub de import con ese nombre, si alguna imagen lo importa (llamarlo va a la
     * función del runtime, o para con su nombre si no la hay). */
    if (!result && name >= 0x10000 && module >= 0x70000000u && module < 0x80000000u)
        for (uint32_t i = 0; i < rt_table_size; i++)
            if (rt_table[i].addr >= 0x62000000u && rt_table[i].addr < 0x63000000u && rt_table[i].name
                && !strcmp(rt_table[i].name, win_str(name))) { result = rt_table[i].addr; break; }
    uint32_t oleaut32_by_ordinal(uint32_t n);
    if (!result && name < 0x10000 && module == 0x7a100000u) result = oleaut32_by_ordinal(name);   /* oleaut32 */
    if (name < 0x10000) fprintf(stderr, "recomp: GetProcAddress(%#x, #%u) = %#x\n", module, name, result);
    else fprintf(stderr, "recomp: GetProcAddress(%#x, %s) = %#x\n", module, win_str(name), result);
    if (!result) win_last_error = 127;
    rt_ret_stdcall(result, 2);
}

/* --- Tiempo ---
 * Reloj real: el del host. Con RECOMP_FIXED_CLOCK=1 (fase 3): la fecha es fija (1997-08-15 12:00) y el reloj de
 * milisegundos es VIRTUAL: avanza 1 ms en cada lectura y 1 ms en cada espera ociosa (rt_host_idle). Así dos
 * corridas con los mismos clics leen las mismas horas, y una espera por tiempo igual termina. */
#define FIXED_UNIX 871646400ull          /* 1997-08-15 12:00:00 UTC */
static int fixed = -1;
static uint64_t start_us;
static uint32_t virtual_ms;

int win_fixed_clock(void)
{
    if (fixed < 0) {
        const char *v = getenv("RECOMP_FIXED_CLOCK");
        fixed = v && *v && strcmp(v, "0") != 0;
        start_us = host_unix_time_us();
    }
    return fixed;
}

uint32_t win_ticks_ms(void)
{
    if (win_fixed_clock()) return 1000000u + virtual_ms++;
    return (uint32_t)((host_unix_time_us() - start_us) / 1000u) + 1000000u;
}

#ifdef RECOMP_WEB
/* Ceder en el navegador. emscripten_sleep es un setTimeout, y encadenado el navegador lo lleva a >= 4 ms: con uno por
 * cuadro presentado, a 60 cuadros se iba un cuarto del tiempo esperando. Un mensaje a uno mismo (MessageChannel) le
 * devuelve el control al navegador (pinta, sonido, eventos) y vuelve enseguida.
 *   ?yield=0 en la página (RECOMP_YIELD=0): como antes, todo con emscripten_sleep; para comparar.
 *   ?perf=1 (RECOMP_PERF=1): cada 5 s, cuántas veces y cuánto tiempo se cedió, y cuántos cuadros. */
#include <emscripten.h>
EM_ASYNC_JS(void, web_yield_message, (void), {
    await new Promise((ok) => { const c = new MessageChannel(); c.port1.onmessage = () => ok(); c.port2.postMessage(0); });
});
static int web_mode = -1, web_perf;
static uint64_t web_last_us, web_report_us, web_away_us[2], web_n[2], web_skipped;
static void web_init(void)
{
    if (web_mode >= 0) return;
    const char *y = getenv("RECOMP_YIELD"), *p = getenv("RECOMP_PERF");
    web_mode = y && *y ? atoi(y) : 1;
    web_perf = p && *p && strcmp(p, "0");
}
/* kind 0: el cuadro está listo (screen_present); kind 1: espera ociosa (rt_host_idle). */
void web_yield(int kind)
{
    web_init();
    uint64_t t0 = host_unix_time_us();
    if (web_mode == 0) emscripten_sleep(kind ? 1 : 0);
    else if (kind == 0 && t0 - web_last_us < 8000) { web_skipped++; return; }      /* el navegador pinta a ~60 Hz: no más */
    else if (kind == 0) web_yield_message();
    else emscripten_sleep(1);                                                      /* esperar de verdad: el juego no tiene nada */
    uint64_t t1 = host_unix_time_us();
    web_last_us = t1;
    web_n[kind]++;
    web_away_us[kind] += t1 - t0;
    if (!web_perf) return;
    if (!web_report_us) web_report_us = t1;
    if (t1 - web_report_us >= 5000000u) {
        extern uint32_t screen_presents(void);
        static uint32_t last_presents;
        uint32_t pr = screen_presents();
        double span = (double)(t1 - web_report_us) / 1000.0;
        fprintf(stderr, "recomp: perf %.0f ms: cuadros %u, cedido al presentar %llu veces %.0f ms (salteadas %llu), "
                        "en espera %llu veces %.0f ms, juego %.0f%%\n", span, pr - last_presents,
                (unsigned long long)web_n[0], web_away_us[0] / 1000.0, (unsigned long long)web_skipped,
                (unsigned long long)web_n[1], web_away_us[1] / 1000.0,
                100.0 * (1.0 - (web_away_us[0] + web_away_us[1]) / 1000.0 / span));
        last_presents = pr;
        web_report_us = t1;
        web_n[0] = web_n[1] = web_away_us[0] = web_away_us[1] = web_skipped = 0;
    }
}
#endif

/* Espera ociosa: en real, cede el procesador 1 ms y atiende los eventos del host; en virtual, avanza el reloj. */
void rt_host_idle(void)
{
    extern void win_pump_host_events(void);
    win_pump_host_events();
#ifdef __EMSCRIPTEN__
    /* En el browser, esperar es devolverle el control al navegador (Asyncify). */
    extern void emscripten_sleep(unsigned int ms);
    static uint32_t n;
    if (win_fixed_clock()) { virtual_ms++; if (++n % 64 == 0) emscripten_sleep(0); }
#ifdef RECOMP_WEB
    else web_yield(1);
#else
    else emscripten_sleep(1);
#endif
#else
    if (win_fixed_clock()) virtual_ms++;
    else host_sleep_ms(1);
#endif
}

static uint64_t now_us(void)
{
    if (win_fixed_clock()) return FIXED_UNIX * 1000000ull + (uint64_t)virtual_ms * 1000u;
    return host_unix_time_us();
}

uint64_t win_filetime_now(void) { return (now_us() + 11644473600000000ull) * 10u; }
int32_t win_bias_minutes(void) { return win_fixed_clock() ? 180 : host_bias_minutes(); }   /* fijo: Buenos Aires */

RT_IMPORT(GetTickCount) { win_poll_hint(); rt_ret_stdcall(win_ticks_ms(), 0); }
RT_IMPORT(GetSystemTimeAsFileTime) { wr64(g_mem, rt_arg(0), win_filetime_now()); rt_ret_stdcall(0, 1); }
RT_IMPORT(QueryPerformanceFrequency) { wr64(g_mem, rt_arg(0), 1000000ull); rt_ret_stdcall(1, 1); }
RT_IMPORT(QueryPerformanceCounter)
{
    uint64_t v = win_fixed_clock() ? (uint64_t)win_ticks_ms() * 1000u : host_unix_time_us() - start_us;
    wr64(g_mem, rt_arg(0), v);
    rt_ret_stdcall(1, 1);
}

void win_put_systemtime(uint32_t p, const struct tm *t, uint32_t ms)
{
    wr16(g_mem, p, (uint32_t)t->tm_year + 1900);
    wr16(g_mem, p + 2, (uint32_t)t->tm_mon + 1);
    wr16(g_mem, p + 4, (uint32_t)t->tm_wday);
    wr16(g_mem, p + 6, (uint32_t)t->tm_mday);
    wr16(g_mem, p + 8, (uint32_t)t->tm_hour);
    wr16(g_mem, p + 10, (uint32_t)t->tm_min);
    wr16(g_mem, p + 12, (uint32_t)t->tm_sec);
    wr16(g_mem, p + 14, ms);
}

static void system_time(uint32_t p, int local)
{
    uint64_t us = now_us();
    time_t s = (time_t)(us / 1000000u) - (local ? (time_t)win_bias_minutes() * 60 : 0);
    struct tm t;
    host_gmtime(s, &t);
    win_put_systemtime(p, &t, (uint32_t)(us % 1000000u / 1000u));
}
RT_IMPORT(GetLocalTime) { system_time(rt_arg(0), 1); rt_ret_stdcall(0, 1); }
RT_IMPORT(GetSystemTime) { system_time(rt_arg(0), 0); rt_ret_stdcall(0, 1); }
RT_IMPORT(GetTimeZoneInformation)
{
    uint32_t p = rt_arg(0);
    memset(g_mem + p, 0, 172);
    wr32(g_mem, p, (uint32_t)win_bias_minutes());
    rt_ret_stdcall(0, 1);
}

/* FILETIME <-> SYSTEMTIME / DOS. */
static void put_filetime_as_system(uint64_t ft, uint32_t p)
{
    int64_t us = (int64_t)(ft / 10u) - 11644473600000000ll;
    time_t s = (time_t)(us / 1000000);
    struct tm t;
    host_gmtime(s, &t);
    win_put_systemtime(p, &t, (uint32_t)(us % 1000000 / 1000));
}
static uint64_t system_to_filetime(uint32_t p)
{
    struct tm t = {0};
    t.tm_year = (int)rd16(g_mem, p) - 1900;
    t.tm_mon = (int)rd16(g_mem, p + 2) - 1;
    t.tm_mday = (int)rd16(g_mem, p + 6);
    t.tm_hour = (int)rd16(g_mem, p + 8);
    t.tm_min = (int)rd16(g_mem, p + 10);
    t.tm_sec = (int)rd16(g_mem, p + 12);
    int64_t days = 0;                                /* días desde 1970 sin depender de timegm del host */
    int y = t.tm_year + 1900, m = t.tm_mon + 1;
    static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    for (int yy = 1970; yy < y; yy++) days += 365 + ((yy % 4 == 0 && yy % 100) || yy % 400 == 0);
    days += cum[(m - 1) % 12] + t.tm_mday - 1 + (m > 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
    int64_t s = days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
    return (uint64_t)(s * 1000 + rd16(g_mem, p + 14)) * 10000u + 116444736000000000ull;
}
RT_IMPORT(FileTimeToSystemTime) { put_filetime_as_system(rd64(g_mem, rt_arg(0)), rt_arg(1)); rt_ret_stdcall(1, 2); }
RT_IMPORT(SystemTimeToFileTime) { wr64(g_mem, rt_arg(1), system_to_filetime(rt_arg(0))); rt_ret_stdcall(1, 2); }
RT_IMPORT(FileTimeToLocalFileTime)
{
    wr64(g_mem, rt_arg(1), rd64(g_mem, rt_arg(0)) - (uint64_t)((int64_t)win_bias_minutes() * 600000000ll));
    rt_ret_stdcall(1, 2);
}
RT_IMPORT(LocalFileTimeToFileTime)
{
    wr64(g_mem, rt_arg(1), rd64(g_mem, rt_arg(0)) + (uint64_t)((int64_t)win_bias_minutes() * 600000000ll));
    rt_ret_stdcall(1, 2);
}
RT_IMPORT(FileTimeToDosDateTime)
{
    uint32_t st = win_static_alloc(16);
    put_filetime_as_system(rd64(g_mem, rt_arg(0)), st);
    uint32_t y = rd16(g_mem, st), mo = rd16(g_mem, st + 2), d = rd16(g_mem, st + 6);
    uint32_t h = rd16(g_mem, st + 8), mi = rd16(g_mem, st + 10), s = rd16(g_mem, st + 12);
    if (rt_arg(1)) wr16(g_mem, rt_arg(1), ((y - 1980) << 9) | (mo << 5) | d);
    if (rt_arg(2)) wr16(g_mem, rt_arg(2), (h << 11) | (mi << 5) | (s / 2));
    rt_ret_stdcall(1, 3);
}
RT_IMPORT(DosDateTimeToFileTime)
{
    uint32_t date = rt_arg(0) & 0xffff, tim = rt_arg(1) & 0xffff, st = win_static_alloc(16);
    wr16(g_mem, st, (date >> 9) + 1980);
    wr16(g_mem, st + 2, (date >> 5) & 15);
    wr16(g_mem, st + 6, date & 31);
    wr16(g_mem, st + 8, tim >> 11);
    wr16(g_mem, st + 10, (tim >> 5) & 63);
    wr16(g_mem, st + 12, (tim & 31) * 2);
    wr64(g_mem, rt_arg(2), system_to_filetime(st));
    rt_ret_stdcall(1, 3);
}
RT_IMPORT(CompareFileTime)
{
    uint64_t a = rd64(g_mem, rt_arg(0)), b = rd64(g_mem, rt_arg(1));
    rt_ret_stdcall(a < b ? 0xffffffffu : a > b, 2);
}

/* --- Cadenas --- */
RT_IMPORT(lstrlenA) { rt_ret_stdcall(rt_arg(0) ? (uint32_t)strlen(win_str(rt_arg(0))) : 0, 1); }
RT_IMPORT(lstrcpyA) { strcpy(win_str(rt_arg(0)), win_str(rt_arg(1))); rt_ret_stdcall(rt_arg(0), 2); }
RT_IMPORT(lstrcatA) { strcat(win_str(rt_arg(0)), win_str(rt_arg(1))); rt_ret_stdcall(rt_arg(0), 2); }
RT_IMPORT(lstrcpynA)
{
    uint32_t dst = rt_arg(0), n = rt_arg(2);
    if (n) win_put_str(dst, win_str(rt_arg(1)), n);
    rt_ret_stdcall(dst, 3);
}
RT_IMPORT(lstrcmpA) { int r = strcmp(win_str(rt_arg(0)), win_str(rt_arg(1))); rt_ret_stdcall((uint32_t)(r < 0 ? -1 : r > 0), 2); }
RT_IMPORT(lstrcmpiA) { int r = strcasecmp(win_str(rt_arg(0)), win_str(rt_arg(1))); rt_ret_stdcall((uint32_t)(r < 0 ? -1 : r > 0), 2); }
RT_IMPORT(MulDiv)
{
    int64_t a = (int32_t)rt_arg(0), b = (int32_t)rt_arg(1), c = (int32_t)rt_arg(2);
    if (!c) { rt_ret_stdcall(0xffffffffu, 3); return; }
    int64_t p = a * b, q = (p + (((p < 0) != (c < 0)) ? -c / 2 : c / 2)) / c;   /* redondea como Windows */
    rt_ret_stdcall((uint32_t)(int32_t)q, 3);
}

/* FormatMessageA: sólo con FORMAT_MESSAGE_FROM_SYSTEM; un texto genérico con el número. */
RT_IMPORT(FormatMessageA)
{
    uint32_t flags = rt_arg(0), id = rt_arg(2), buf = rt_arg(4), room = rt_arg(5);
    char text[64];
    snprintf(text, sizeof text, "Error %u.\r\n", id);
    uint32_t n = (uint32_t)strlen(text);
    if (flags & 0x100u) {                            /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        uint32_t p = rt_alloc(n + 1);
        win_put_str(p, text, n + 1);
        wr32(g_mem, buf, p);
    } else win_put_str(buf, text, room);
    rt_ret_stdcall(n, 7);
}

/* Átomos globales: MFC los usa para registrar clases. */
RT_IMPORT(GlobalAddAtomA) { static uint32_t next = 0xC000; rt_ret_stdcall(next++, 1); }
RT_IMPORT(GlobalDeleteAtom) { rt_ret_stdcall(0, 1); }
RT_IMPORT(GlobalFindAtomA) { rt_ret_stdcall(0, 1); }

/* Recursos: los de cada imagen cargada (el exe, sus DLL y MFC42). FindResourceA busca de verdad en la .rsrc
 * (resources.c). */
