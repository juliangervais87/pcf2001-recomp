/* Hilos del juego, cooperativos y deterministas (docs/investigacion/fase-2.md § 6): cada hilo del juego tiene su
 * propio hilo del host, pero sólo uno corre a la vez (el que tiene el testigo). Se conmuta únicamente en
 * win_yield(), que llaman las funciones que bloquean (Sleep, WaitFor*, el bucle de mensajes), y siempre al
 * siguiente hilo listo en orden de creación. Mismo orden en toda corrida y en toda plataforma: lo que pide la
 * fase 3. Al conmutar se guardan y restauran cpu, fpu y el bloque del hilo en fs:0 (RT_FS_BASE es una
 * constante del C traducido: el bloque se copia, no se mueve).
 *
 * Eventos, mutex y la espera también viven acá. */
#include "win.h"
#include "loader.h"

#include <setjmp.h>

#ifdef __EMSCRIPTEN__
/* En el browser no hay hilos del host sin SharedArrayBuffer (y pedirle COOP/COEP al servidor de Dinamic es un costo
 * evitable, docs/investigacion/fase-2.md § 6): cada hilo del juego es una FIBRA de Emscripten (sobre Asyncify). El
 * testigo es implícito: la fibra que corre es la que lo tiene. */
#include <emscripten.h>
#include <emscripten/fiber.h>
typedef int HostThread;
#define FIBER_C_STACK (512 * 1024)
#define FIBER_ASYNC_STACK (256 * 1024)
#define lock_init(l) ((void)0)
#define lock_take(l) ((void)0)
#define lock_give(l) ((void)0)
#define cond_init(c) ((void)0)
typedef int HostLock;
typedef int HostCond;
#elif defined(_WIN32)
#include <windows.h>
typedef HANDLE HostThread;
typedef CRITICAL_SECTION HostLock;
typedef CONDITION_VARIABLE HostCond;
#define lock_init(l) InitializeCriticalSection(l)
#define lock_take(l) EnterCriticalSection(l)
#define lock_give(l) LeaveCriticalSection(l)
#define cond_init(c) InitializeConditionVariable(c)
#define cond_wait(c, l) SleepConditionVariableCS(c, l, INFINITE)
#define cond_wake_all(c) WakeAllConditionVariable(c)
#else
#include <pthread.h>
typedef pthread_t HostThread;
typedef pthread_mutex_t HostLock;
typedef pthread_cond_t HostCond;
#define lock_init(l) pthread_mutex_init(l, NULL)
#define lock_take(l) pthread_mutex_lock(l)
#define lock_give(l) pthread_mutex_unlock(l)
#define cond_init(c) pthread_cond_init(c, NULL)
#define cond_wait(c, l) pthread_cond_wait(c, l)
#define cond_wake_all(c) pthread_cond_broadcast(c)
#endif

#define MAX_THREADS 32
#define TEB_SIZE 0x40u
#define THREAD_STACK_BASE 0x1A000000u   /* 32 pilas de 512 KB: hasta 0x1B000000 (docs/mapa-de-memoria.md) */
#define THREAD_STACK_SIZE 0x00080000u

typedef struct {
    int used, alive, ready;
    uint32_t handle, id, start, param, exit_code;
    Cpu cpu;
    Fpu fpu;
    uint8_t teb[TEB_SIZE];
    HostThread host;
    jmp_buf exit_jmp;
#ifdef __EMSCRIPTEN__
    emscripten_fiber_t fiber;
    void *c_stack, *async_stack;
#endif
} Thread;

static Thread threads[MAX_THREADS];
static int current;                 /* índice del hilo que tiene el testigo */
static HostLock big;
static HostCond turn;
static int inited;

#ifdef __EMSCRIPTEN__
static char main_async_stack[FIBER_ASYNC_STACK];
/* Cambia de la fibra `me` a la `next` (la fibra principal se inicializa la primera vez que cede). */
static void switch_to(int me, int next)
{
    static int main_ready;
    if (!main_ready) {
        emscripten_fiber_init_from_current_context(&threads[0].fiber, main_async_stack, sizeof main_async_stack);
        main_ready = 1;
    }
    current = next;
    emscripten_fiber_swap(&threads[me].fiber, &threads[next].fiber);
}
#endif

static void init(void)
{
    if (inited) return;
    inited = 1;
    lock_init(&big);
    cond_init(&turn);
    threads[0].used = threads[0].alive = threads[0].ready = 1;
    threads[0].id = 1;
    lock_take(&big);                 /* el hilo principal arranca con el testigo */
}

static void save(Thread *t)
{
    t->cpu = cpu;
    t->fpu = fpu;
    memcpy(t->teb, g_mem + RT_FS_BASE, TEB_SIZE);
}

static void restore(Thread *t)
{
    cpu = t->cpu;
    fpu = t->fpu;
    memcpy(g_mem + RT_FS_BASE, t->teb, TEB_SIZE);
}

/* Pasa el testigo al siguiente hilo listo y espera a que vuelva. Si no hay otro, vuelve enseguida. */
void win_yield(void)
{
    init();
    int me = current, next = -1;
    for (int k = 1; k <= MAX_THREADS; k++) {
        int i = (me + k) % MAX_THREADS;
        if (threads[i].used && threads[i].alive && threads[i].ready) { next = i; break; }
    }
    if (next < 0 || next == me) return;
    static uint32_t sw;
    if (win_trace >= 3 && sw++ < 300) fprintf(stderr, "cambio hilo %u -> %u\n", threads[me].id, threads[next].id);
    save(&threads[me]);
#ifdef __EMSCRIPTEN__
    switch_to(me, next);
    current = me;
#else
    current = next;
    cond_wake_all(&turn);
    while (current != me) cond_wait(&turn, &big);
#endif
    restore(&threads[me]);
}

uint32_t win_current_thread_id(void) { init(); return threads[current].id; }

/* Se llama con el testigo, desde el hilo que termina: lo marca terminado y pasa el turno sin volver. */
static void finish_current(uint32_t code)
{
    Thread *t = &threads[current];
    t->alive = 0;
    t->exit_code = code;
    WIN_TRACE("hilo %u terminó con %u\n", t->id, code);
    /* El handle sobrevive al hilo (queda señalado, con el código de salida) y la ranura se puede reusar: el juego
     * crea hilos nuevos a medida que avanza (la música, por pantalla). */
    WinHandle *hw = win_handle_get(t->handle, WH_THREAD);
    if (hw) { hw->ptr = NULL; hw->a = code; }
    t->used = 0;
    int next = -1;
    for (int k = 1; k <= MAX_THREADS; k++) {
        int i = (current + k) % MAX_THREADS;
        if (threads[i].used && threads[i].alive && threads[i].ready) { next = i; break; }
    }
    if (next < 0) rt_fatal("el último hilo del juego terminó");
#ifdef __EMSCRIPTEN__
    switch_to((int)(t - threads), next);      /* una fibra terminada no vuelve a correr */
    rt_fatal("una fibra terminada volvió a correr");
#else
    current = next;
    cond_wake_all(&turn);
#endif
}

#ifdef __EMSCRIPTEN__
static void host_main(void *arg)
#elif defined(_WIN32)
static DWORD WINAPI host_main(LPVOID arg)
#else
static void *host_main(void *arg)
#endif
{
    int me = (int)(intptr_t)arg;
    Thread *t = &threads[me];
#ifndef __EMSCRIPTEN__
    lock_take(&big);
    while (current != me) cond_wait(&turn, &big);
#endif
    restore(t);
    if (!setjmp(t->exit_jmp)) {
        cpu.esp -= 4;
        wr32(g_mem, cpu.esp, t->param);
        cpu.esp -= 4;
        wr32(g_mem, cpu.esp, RT_RETURN_ADDR);
        rt_call(t->start);
        t->exit_code = cpu.eax;
    }
    finish_current(t->exit_code);
    lock_give(&big);
#ifndef __EMSCRIPTEN__
    return 0;
#endif
}

static uint32_t create(uint32_t start, uint32_t param, int suspended)
{
    init();
    int i = 1;
    while (i < MAX_THREADS && threads[i].used) i++;
    if (i == MAX_THREADS) rt_fatal("demasiados hilos del juego");
    Thread *t = &threads[i];
#ifdef __EMSCRIPTEN__
    free(t->c_stack);                     /* la fibra anterior de esta ranura terminó y no vuelve a correr */
    free(t->async_stack);
#endif
    memset(t, 0, sizeof *t);
    t->used = t->alive = 1;
    t->ready = !suspended;
    t->id = (uint32_t)i + 1;
    t->start = start;
    t->param = param;
    t->cpu = cpu;
    t->fpu = fpu;
    t->cpu.esp = THREAD_STACK_BASE + (uint32_t)i * THREAD_STACK_SIZE - 0x100;
    memset(t->teb, 0, TEB_SIZE);
    uint32_t top = THREAD_STACK_BASE + (uint32_t)i * THREAD_STACK_SIZE;
    wr32(t->teb, 0, 0xffffffffu);
    wr32(t->teb, 4, top);
    wr32(t->teb, 8, top - THREAD_STACK_SIZE);
    wr32(t->teb, 0x18, RT_FS_BASE);
    wr32(t->teb, 0x2c, rt_tls_new_thread());                  /* TLS implícito propio del hilo */
    t->handle = win_handle_new(WH_THREAD, t);
    WIN_TRACE("hilo %u: start %#x param %#x%s\n", t->id, start, param, suspended ? " (suspendido)" : "");
#ifdef __EMSCRIPTEN__
    t->c_stack = malloc(FIBER_C_STACK);
    t->async_stack = malloc(FIBER_ASYNC_STACK);
    emscripten_fiber_init(&t->fiber, host_main, (void *)(intptr_t)i, t->c_stack, FIBER_C_STACK, t->async_stack, FIBER_ASYNC_STACK);
#elif defined(_WIN32)
    t->host = CreateThread(NULL, 0, host_main, (LPVOID)(intptr_t)i, 0, NULL);
    if (!t->host) rt_fatal("CreateThread del host falló");
#else
    if (pthread_create(&t->host, NULL, host_main, (void *)(intptr_t)i)) rt_fatal("pthread_create falló");
#endif
    return t->handle;
}

/* CreateThread(attrs, stack, start, param, flags, id*) */
RT_IMPORT(CreateThread)
{
    uint32_t start = rt_arg(2), param = rt_arg(3), flags = rt_arg(4), idp = rt_arg(5);
    uint32_t h = create(start, param, (flags & 4u) != 0);    /* CREATE_SUSPENDED */
    if (idp) wr32(g_mem, idp, ((Thread *)win_handle_get(h, WH_THREAD)->ptr)->id);
    rt_ret_stdcall(h, 6);
}

RT_IMPORT(ExitThread)
{
    Thread *t = &threads[current];
    if (current == 0) rt_fatal("ExitThread en el hilo principal");
    t->exit_code = rt_arg(0);
#ifdef __EMSCRIPTEN__
    finish_current(t->exit_code);       /* la fibra no vuelve: su pila queda abandonada */
#else
    longjmp(t->exit_jmp, 1);
#endif
}

static Thread *thread_of(uint32_t h)
{
    if (h == 0xfffffffeu) return &threads[current];          /* GetCurrentThread */
    WinHandle *w = win_handle_get(h, WH_THREAD);
    return w ? (Thread *)w->ptr : NULL;      /* NULL: hilo terminado (o handle inválido) */
}

RT_IMPORT(ResumeThread)
{
    Thread *t = thread_of(rt_arg(0));
    uint32_t was = t && !t->ready ? 1u : 0u;
    if (t) t->ready = 1;
    rt_ret_stdcall(was, 1);
}

RT_IMPORT(SuspendThread)
{
    Thread *t = thread_of(rt_arg(0));
    uint32_t was = t && !t->ready ? 1u : 0u;
    if (t) t->ready = 0;
    rt_ret_stdcall(was, 1);
    if (t == &threads[current]) win_yield();
}

RT_IMPORT(GetExitCodeThread)
{
    WinHandle *w = win_handle_get(rt_arg(0), WH_THREAD);
    Thread *t = w ? (Thread *)w->ptr : NULL;
    if (rt_arg(1)) wr32(g_mem, rt_arg(1), !w ? 0u : !t ? w->a : !t->alive ? t->exit_code : 0x103u);   /* STILL_ACTIVE */
    rt_ret_stdcall(w ? 1 : 0, 2);
}

RT_IMPORT(SetThreadPriority) { rt_ret_stdcall(1, 2); }
RT_IMPORT(GetThreadPriority) { rt_ret_stdcall(0, 1); }
RT_IMPORT(GetCurrentThread) { rt_ret_stdcall(0xfffffffeu, 0); }
RT_IMPORT(GetCurrentThreadId) { rt_ret_stdcall(win_current_thread_id(), 0); }

/* Eventos y mutex. Un evento: a = señalado, b = reseteo manual. */
RT_IMPORT(CreateEventA)
{
    uint32_t h = win_handle_new(WH_EVENT, NULL);
    WinHandle *w = win_handle_get(h, WH_EVENT);
    w->b = rt_arg(1) != 0;
    w->a = rt_arg(2) != 0;
    win_last_error = 0;
    rt_ret_stdcall(h, 4);
}
RT_IMPORT(OpenEventA) { win_last_error = ERROR_FILE_NOT_FOUND; rt_ret_stdcall(0, 3); }
RT_IMPORT(SetEvent) { WinHandle *w = win_handle_get(rt_arg(0), WH_EVENT); if (w) w->a = 1; rt_ret_stdcall(w != NULL, 1); }
RT_IMPORT(ResetEvent) { WinHandle *w = win_handle_get(rt_arg(0), WH_EVENT); if (w) w->a = 0; rt_ret_stdcall(w != NULL, 1); }
RT_IMPORT(PulseEvent)
{
    WinHandle *w = win_handle_get(rt_arg(0), WH_EVENT);
    if (w) { w->a = 1; win_yield(); w->a = 0; }
    rt_ret_stdcall(w != NULL, 1);
}
RT_IMPORT(CreateMutexA)
{
    uint32_t h = win_handle_new(WH_MUTEX, NULL);
    win_last_error = 0;
    rt_ret_stdcall(h, 3);
}
RT_IMPORT(ReleaseMutex) { rt_ret_stdcall(1, 1); }

/* ¿Está señalado h? Si lo está y es de consumo (evento automático), lo consume. */
static int signaled(uint32_t h)
{
    WinHandle *w = win_handle_get(h, WH_FREE);
    if (!w) return 1;                     /* un handle que no conocemos no bloquea */
    switch (w->kind) {
    case WH_EVENT:
        if (!w->a) return 0;
        if (!w->b) w->a = 0;
        return 1;
    case WH_THREAD: return !w->ptr || !((Thread *)w->ptr)->alive;
    default: return 1;                    /* mutex (un hilo por vez de todos modos), archivos */
    }
}

#define WAIT_TIMEOUT 0x102u
static uint32_t wait_any(int n, const uint32_t *hs, int all, uint32_t timeout)
{
    uint32_t deadline = win_ticks_ms() + timeout;
    static uint32_t traced;
    if (win_trace >= 3 && traced++ < 400) {
        WinHandle *w = win_handle_get(hs[0], WH_FREE);
        fprintf(stderr, "espera hilo %u: %d handle(s) [%#x tipo %d] timeout %u (desde %#x)\n", threads[current].id, n, hs[0],
                w ? w->kind : -1, timeout, rd32(g_mem, cpu.esp));
    }
    for (;;) {
        if (all) {
            int ok = 1;
            for (int i = 0; i < n; i++) ok &= signaled(hs[i]);
            if (ok) return 0;
        } else {
            for (int i = 0; i < n; i++) if (signaled(hs[i])) return (uint32_t)i;
        }
        if (timeout == 0 || (timeout != 0xffffffffu && (int32_t)(win_ticks_ms() - deadline) >= 0)) return WAIT_TIMEOUT;
        win_yield();
        rt_host_idle();
    }
}

RT_IMPORT(WaitForSingleObject)
{
    uint32_t h = rt_arg(0), t = rt_arg(1);
    rt_ret_stdcall(wait_any(1, &h, 0, t), 2);
}

RT_IMPORT(WaitForMultipleObjects)
{
    uint32_t n = rt_arg(0), arr = rt_arg(1), all = rt_arg(2), t = rt_arg(3);
    uint32_t hs[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) hs[i] = rd32(g_mem, arr + 4 * i);
    rt_ret_stdcall(wait_any((int)n, hs, all != 0, t), 4);
}

RT_IMPORT(Sleep)
{
    uint32_t ms = rt_arg(0);
    static uint32_t ns;
    if (win_trace >= 3 && ns++ < 200) fprintf(stderr, "Sleep(%u) hilo %u desde %#x\n", ms, threads[current].id, rd32(g_mem, cpu.esp));
    rt_ret_stdcall(0, 1);
    uint32_t deadline = win_ticks_ms() + ms;
    do { win_yield(); rt_host_idle(); } while ((int32_t)(win_ticks_ms() - deadline) < 0);
}

/* Secciones críticas: con un solo hilo corriendo a la vez no hace falta excluir nada. */
RT_IMPORT(InitializeCriticalSection) { memset(g_mem + rt_arg(0), 0, 24); rt_ret_stdcall(0, 1); }
RT_IMPORT(DeleteCriticalSection) { rt_ret_stdcall(0, 1); }
RT_IMPORT(EnterCriticalSection) { rt_ret_stdcall(0, 1); }
RT_IMPORT(LeaveCriticalSection) { rt_ret_stdcall(0, 1); }
RT_IMPORT(InterlockedIncrement) { uint32_t p = rt_arg(0), v = rd32(g_mem, p) + 1; wr32(g_mem, p, v); rt_ret_stdcall(v, 1); }
RT_IMPORT(InterlockedDecrement) { uint32_t p = rt_arg(0), v = rd32(g_mem, p) - 1; wr32(g_mem, p, v); rt_ret_stdcall(v, 1); }
RT_IMPORT(InterlockedExchange) { uint32_t p = rt_arg(0), v = rd32(g_mem, p); wr32(g_mem, p, rt_arg(1)); rt_ret_stdcall(v, 2); }

/* TLS: 64 ranuras por hilo. */
#define TLS_SLOTS 64
static uint32_t tls_used[TLS_SLOTS];
static uint32_t tls_value[MAX_THREADS][TLS_SLOTS];
RT_IMPORT(TlsAlloc)
{
    for (uint32_t i = 0; i < TLS_SLOTS; i++)
        if (!tls_used[i]) { tls_used[i] = 1; rt_ret_stdcall(i, 0); return; }
    rt_ret_stdcall(0xffffffffu, 0);
}
RT_IMPORT(TlsFree) { uint32_t i = rt_arg(0); if (i < TLS_SLOTS) tls_used[i] = 0; rt_ret_stdcall(1, 1); }
RT_IMPORT(TlsGetValue) { uint32_t i = rt_arg(0); win_last_error = 0; rt_ret_stdcall(i < TLS_SLOTS ? tls_value[current][i] : 0, 1); }
RT_IMPORT(TlsSetValue) { uint32_t i = rt_arg(0); if (i < TLS_SLOTS) tls_value[current][i] = rt_arg(1); rt_ret_stdcall(i < TLS_SLOTS, 2); }

/* RT_POLL del C traducido (translator.py): cada 50.000 vueltas de bucle, ceder a otro hilo listo y atender el host.
 * Determinista: depende sólo de cuántas vueltas da el código. */
int32_t rt_poll_budget = 50000;
void rt_poll(void)
{
    static uint32_t n;
    rt_poll_budget = 50000;
    if (++n % 4 == 0) win_pump_host_events();
    win_yield();
}
