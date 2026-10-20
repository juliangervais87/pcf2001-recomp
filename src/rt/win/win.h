/* Partes compartidas de los reemplazos de Windows del recompilado (src/rt/win). Toda importación es stdcall:
 * un reemplazo lee sus argumentos con rt_arg y vuelve con rt_ret_stdcall (o rt_ret_cdecl para wsprintfA). */
#ifndef PCF2001_RECOMP_WIN_H
#define PCF2001_RECOMP_WIN_H

#include "rt.h"
#include "rt_host.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s
#else
#include <strings.h>
#endif

/* Cadenas en la memoria plana. */
static inline char *win_str(uint32_t a) { return (char *)(g_mem + a); }
static inline void win_put_str(uint32_t dst, const char *s, uint32_t room)
{
    if (!room) return;
    size_t n = strlen(s);
    if (n >= room) n = room - 1;
    memcpy(g_mem + dst, s, n);
    g_mem[dst + n] = 0;
}

/* Traza: RECOMP_TRACE=1 imprime cada importación que la pide (WIN_TRACE) y, con =2, toda llamada a Windows. */
extern int win_trace;
#define WIN_TRACE(...) do { if (win_trace) fprintf(stderr, __VA_ARGS__); } while (0)

/* GetLastError. */
extern uint32_t win_last_error;
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_FILE_EXISTS 80u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_ALREADY_EXISTS 183u

/* La configuración de la corrida (main.c). */
extern char win_hd_dir[1024];       /* carpeta del host que es C:\ (instalación del juego y partidas) */
extern char win_cd_dir[1024];       /* carpeta del host con el contenido de la ISO: la unidad del CD */
extern char win_command_line[1024];
extern const char rt_game_dir[], rt_exe_path[];   /* el perfil del juego (gen/images.c, config/juegos) */
#define WIN_GAME_DIR rt_game_dir           /* donde el juego cree que está instalado */
#define WIN_EXE_NAME rt_exe_path
#define WIN_CD_LETTER 'D'

/* Memoria del runtime que nunca se libera, en cero (kernel32.c): línea de comandos, objetos COM, etc. */
uint32_t win_static_alloc(uint32_t size);

/* Objetos con handle (handles.c): archivos, búsquedas, eventos, hilos, mapeos. */
enum { WH_FREE = 0, WH_FILE, WH_FIND, WH_EVENT, WH_THREAD, WH_MAPPING, WH_MUTEX, WH_OTHER };
typedef struct {
    int kind;
    void *ptr;          /* lo que el dueño quiera */
    uint32_t a, b;      /* ídem */
} WinHandle;
uint32_t win_handle_new(int kind, void *ptr);
WinHandle *win_handle_get(uint32_t h, int kind);   /* NULL si no es de ese tipo */
void win_handle_free(uint32_t h);

/* Rutas (files.c): una ruta de Windows → ruta del host, sin distinguir mayúsculas en cada parte. */
int win_host_path(const char *winpath, char *out, size_t room);
int win_read_path(const char *winpath, char *out, size_t room);   /* para leer: disco virtual y, si no, el CD */
void win_full_path(const char *winpath, char *out, size_t room);   /* absoluta, con la carpeta actual */
extern char win_cwd[1024];
extern char win_cd_label[33];
int win_trace_keys(void);                 /* RECOMP_TRACE_KEYS=1 (input.c) */
uint32_t win_vk_to_scan(uint32_t vk);     /* tecla virtual -> código de escaneo (input.c) */

/* Tiempo (kernel32.c). Con RECOMP_FIXED_CLOCK=1 los relojes son repetibles (fase 3). */
int win_fixed_clock(void);
uint64_t win_filetime_now(void);
uint32_t win_ticks_ms(void);        /* el reloj de GetTickCount/timeGetTime */
struct tm;
void win_put_systemtime(uint32_t p, const struct tm *t, uint32_t ms);
int32_t win_bias_minutes(void);

/* Ceder: la conmutación de hilos cooperativos y la del bucle de eventos del host (threads.c). */
void win_yield(void);
void rt_host_idle(void);              /* espera ociosa: eventos del host + 1 ms (o reloj virtual) */
void win_pump_host_events(void);      /* input.c: lee teclado/mouse/ventana del host a la cola */
void win_poll_hint(void);             /* input.c: consultas de sondeo del juego: bombea y cede cada tanto */
uint32_t win_current_thread_id(void);

/* GetProcAddress de los módulos de Windows (ddraw.dll, dsound.dll, ...): lo registra cada archivo. */
void win_register_proc(const char *module, const char *name, RtFn fn);

/* Llamada a un procedimiento de ventana o callback del juego (stdcall). */
static inline uint32_t win_call(uint32_t fn, int n, const uint32_t *args) { return rt_callback(fn, n, args); }

#endif
