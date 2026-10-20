/* USER32: clases, ventanas, mensajes, timers, hooks, cursor y lo chico. Un manejador de ventanas sin escritorio:
 * las ventanas son registros con su procedimiento (código del juego o de MFC, que se llama con rt_callback); no se
 * dibuja ningún marco porque el juego pinta todo él mismo (DirectDraw o GDI sobre el framebuffer de screen.c).
 * El orden de los mensajes de creación sigue a Windows (hook CBT, WM_GETMINMAXINFO, WM_NCCREATE, WM_NCCALCSIZE,
 * WM_CREATE, WM_SIZE, WM_MOVE), que es lo que MFC42 necesita para subclasear. */
#include "user32.h"
#include "screen.h"

#include <ctype.h>
#include <stdarg.h>

/* ---- clases ---- */
#define MAX_CLASSES 128
typedef struct { int used; char name[64]; uint32_t atom, proc, style, cls_extra, wnd_extra, hinst, icon, cursor, brush, menu; } WndClass;
static WndClass classes[MAX_CLASSES];
static uint32_t next_atom = 0xC100;

static void host_DefWindowProcA(void);
static uint32_t def_proc_addr(void) { static uint32_t a; if (!a) a = rt_host_fn("DefWindowProcA", host_DefWindowProcA); return a; }

static WndClass *class_find(uint32_t name)
{
    for (int i = 0; i < MAX_CLASSES; i++) {
        if (!classes[i].used) continue;
        if (name < 0x10000 ? classes[i].atom == name : !strcasecmp(classes[i].name, win_str(name))) return &classes[i];
    }
    return NULL;
}

static WndClass *class_add(const char *name)
{
    for (int i = 0; i < MAX_CLASSES; i++)
        if (!classes[i].used) {
            memset(&classes[i], 0, sizeof classes[i]);
            classes[i].used = 1;
            snprintf(classes[i].name, sizeof classes[i].name, "%s", name);
            classes[i].atom = next_atom++;
            return &classes[i];
        }
    rt_fatal("demasiadas clases de ventana");
}

static void builtin_classes(void)
{
    static int done;
    if (done) return;
    done = 1;
    static const char *names[] = {"BUTTON", "EDIT", "STATIC", "LISTBOX", "COMBOBOX", "SCROLLBAR", "#32770", "MDICLIENT", "ComboLBox"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) { WndClass *c = class_add(names[i]); c->proc = def_proc_addr(); c->wnd_extra = 32; }
}

/* RegisterClassA(WNDCLASSA*): style, proc, cbClsExtra, cbWndExtra, hInstance, hIcon, hCursor, hbrBackground, menu, name */
static uint32_t register_class(uint32_t wc, int ex)
{
    builtin_classes();
    uint32_t o = ex ? 4 : 0;
    uint32_t namep = rd32(g_mem, wc + o + 36);
    char name[64];
    if (namep < 0x10000) snprintf(name, sizeof name, "#%u", namep); else snprintf(name, sizeof name, "%s", win_str(namep));
    WndClass *c = class_find(namep);
    if (c) { win_last_error = 1410; return 0; }          /* ERROR_CLASS_ALREADY_EXISTS */
    c = class_add(name);
    c->style = rd32(g_mem, wc + o);
    c->proc = rd32(g_mem, wc + o + 4);
    c->cls_extra = rd32(g_mem, wc + o + 8);
    c->wnd_extra = rd32(g_mem, wc + o + 12);
    c->hinst = rd32(g_mem, wc + o + 16);
    c->icon = rd32(g_mem, wc + o + 20);
    c->cursor = rd32(g_mem, wc + o + 24);
    c->brush = rd32(g_mem, wc + o + 28);
    c->menu = rd32(g_mem, wc + o + 32);
    WIN_TRACE("RegisterClass(%s) proc %#x\n", name, c->proc);
    return c->atom;
}
RT_IMPORT(RegisterClassA) { rt_ret_stdcall(register_class(rt_arg(0), 0), 1); }
RT_IMPORT(RegisterClassExA) { rt_ret_stdcall(register_class(rt_arg(0), 1), 1); }
RT_IMPORT(UnregisterClassA)
{
    WndClass *c = class_find(rt_arg(0));
    if (c) c->used = 0;
    rt_ret_stdcall(c != NULL, 2);
}
static int class_info(uint32_t name, uint32_t out, int ex)
{
    builtin_classes();
    WndClass *c = class_find(name);
    if (!c) { win_last_error = 1411; return 0; }         /* ERROR_CLASS_DOES_NOT_EXIST */
    uint32_t o = ex ? 4 : 0;
    if (ex) wr32(g_mem, out, 48);
    wr32(g_mem, out + o, c->style);
    wr32(g_mem, out + o + 4, c->proc);
    wr32(g_mem, out + o + 8, c->cls_extra);
    wr32(g_mem, out + o + 12, c->wnd_extra);
    wr32(g_mem, out + o + 16, c->hinst);
    wr32(g_mem, out + o + 20, c->icon);
    wr32(g_mem, out + o + 24, c->cursor);
    wr32(g_mem, out + o + 28, c->brush);
    wr32(g_mem, out + o + 32, c->menu);
    wr32(g_mem, out + o + 36, name);
    if (ex) wr32(g_mem, out + 44, 0);
    return 1;
}
RT_IMPORT(GetClassInfoA) { rt_ret_stdcall(class_info(rt_arg(1), rt_arg(2), 0), 3); }
RT_IMPORT(GetClassInfoExA) { rt_ret_stdcall(class_info(rt_arg(1), rt_arg(2), 1), 3); }
RT_IMPORT(GetClassNameA)
{
    Wnd *w = wnd_get(rt_arg(0));
    win_put_str(rt_arg(1), w ? classes[w->cls].name : "", rt_arg(2));
    rt_ret_stdcall((uint32_t)strlen(win_str(rt_arg(1))), 3);
}

/* ---- ventanas ---- */
#define MAX_WND 512
#define HWND_BASE 0x00010000u
static Wnd wnds[MAX_WND];

static uint32_t hwnd_of(int i) { return HWND_BASE + 4u * (uint32_t)i; }
Wnd *wnd_get(uint32_t hwnd)
{
    if (hwnd < HWND_BASE || (hwnd & 3)) return NULL;
    uint32_t i = (hwnd - HWND_BASE) / 4;
    return i && i < MAX_WND && wnds[i].used ? &wnds[i] : NULL;
}
static uint32_t main_wnd;
uint32_t wnd_main(void) { return main_wnd; }

void wnd_screen_origin(uint32_t hwnd, int32_t *x, int32_t *y)
{
    *x = *y = 0;
    for (Wnd *w = wnd_get(hwnd); w; w = (w->style & WS_CHILD) ? wnd_get(w->parent) : NULL) { *x += w->x; *y += w->y; }
}

uint32_t msg_send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    Wnd *w = wnd_get(hwnd);
    if (!w || !w->proc) return 0;
    uint32_t args[4] = {hwnd, msg, wp, lp};
    if (win_trace > 1) fprintf(stderr, "  msg %#x -> hwnd %#x (wp %#x lp %#x)\n", msg, hwnd, wp, lp);
    return win_call(w->proc, 4, args);
}

/* Hooks: WH_CBT (5) es el que MFC usa para subclasear al crear. */
#define MAX_HOOKS 16
static struct { uint32_t id, proc, handle; } hooks[MAX_HOOKS];
static uint32_t next_hook = 0x7700;
RT_IMPORT(SetWindowsHookExA)
{
    for (int i = 0; i < MAX_HOOKS; i++)
        if (!hooks[i].handle) { hooks[i].id = rt_arg(0); hooks[i].proc = rt_arg(1); hooks[i].handle = next_hook++; WIN_TRACE("hook %d -> %#x\n", (int)rt_arg(0), rt_arg(1)); rt_ret_stdcall(hooks[i].handle, 4); return; }
    rt_ret_stdcall(0, 4);
}
RT_IMPORT(SetWindowsHookA)
{
    for (int i = 0; i < MAX_HOOKS; i++)
        if (!hooks[i].handle) { hooks[i].id = rt_arg(0); hooks[i].proc = rt_arg(1); hooks[i].handle = next_hook++; rt_ret_stdcall(hooks[i].handle, 2); return; }
    rt_ret_stdcall(0, 2);
}
RT_IMPORT(UnhookWindowsHookEx)
{
    for (int i = 0; i < MAX_HOOKS; i++) if (hooks[i].handle == rt_arg(0)) hooks[i].handle = 0;
    rt_ret_stdcall(1, 1);
}
RT_IMPORT(UnhookWindowsHook)
{
    for (int i = 0; i < MAX_HOOKS; i++) if (hooks[i].proc == rt_arg(1)) hooks[i].handle = 0;
    rt_ret_stdcall(1, 2);
}
RT_IMPORT(CallNextHookEx) { rt_ret_stdcall(0, 4); }          /* un solo hook por tipo en la práctica */
RT_IMPORT(DefHookProc) { rt_ret_stdcall(0, 4); }
static uint32_t call_hooks(uint32_t id, uint32_t code, uint32_t wp, uint32_t lp)
{
    uint32_t r = 0;
    for (int i = 0; i < MAX_HOOKS; i++)
        if (hooks[i].handle && hooks[i].id == id) { uint32_t a[3] = {code, wp, lp}; r = win_call(hooks[i].proc, 3, a); }
    return r;
}

static void destroy(uint32_t hwnd);

/* CreateWindowExA(ex, clase, nombre, estilo, x, y, w, h, padre, menú/id, instancia, parámetro) */
RT_IMPORT(CreateWindowExA)
{
    builtin_classes();
    uint32_t ex = rt_arg(0), clsn = rt_arg(1), name = rt_arg(2), style = rt_arg(3);
    int32_t x = (int32_t)rt_arg(4), y = (int32_t)rt_arg(5), w = (int32_t)rt_arg(6), h = (int32_t)rt_arg(7);
    uint32_t parent = rt_arg(8), menu = rt_arg(9), inst = rt_arg(10), param = rt_arg(11);
    uint32_t ret = rd32(g_mem, cpu.esp);
    WndClass *c = class_find(clsn);
    if (!c) {
        fprintf(stderr, "recomp: CreateWindowExA: clase %s no registrada\n", clsn < 0x10000 ? "(átomo)" : win_str(clsn));
        win_last_error = 1407;
        rt_ret_stdcall(0, 12);
        return;
    }
    int i = 1;
    while (i < MAX_WND && wnds[i].used) i++;
    if (i == MAX_WND) rt_fatal("demasiadas ventanas");
    Wnd *wd = &wnds[i];
    memset(wd, 0, sizeof *wd);
    wd->used = 1;
    wd->cls = (uint32_t)(c - classes);
    wd->proc = c->proc;
    wd->style = style & ~WS_VISIBLE;
    wd->exstyle = ex;
    wd->hinst = inst;
    if (style & WS_CHILD) { wd->parent = parent; wd->id = menu; } else wd->owner = parent;
    if ((uint32_t)x == 0x80000000u) x = 0;
    if ((uint32_t)y == 0x80000000u) y = 0;
    if ((uint32_t)w == 0x80000000u) w = (int32_t)screen.w;
    if ((uint32_t)h == 0x80000000u) h = (int32_t)screen.h;
    wd->x = x; wd->y = y; wd->w = w; wd->h = h;
    wd->enabled = !(style & WS_DISABLED);
    if (name >= 0x10000) snprintf(wd->text, sizeof wd->text, "%s", win_str(name));
    uint32_t hwnd = hwnd_of(i);
    WIN_TRACE("CreateWindowExA(%s, \"%s\", %#x, %d,%d %dx%d) = %#x (desde %#x)\n", c->name, wd->text, style, x, y, w, h, hwnd, ret);

    /* CREATESTRUCTA + CBT_CREATEWNDA en memoria del juego (debajo de la pila). */
    Cpu saved = cpu;
    cpu.esp -= 0x200;
    uint32_t cs = cpu.esp + 0x100, cbt = cpu.esp + 0x180, mmi = cpu.esp + 0x1a0;
    wr32(g_mem, cs, param); wr32(g_mem, cs + 4, inst); wr32(g_mem, cs + 8, menu); wr32(g_mem, cs + 12, parent);
    wr32(g_mem, cs + 16, (uint32_t)h); wr32(g_mem, cs + 20, (uint32_t)w); wr32(g_mem, cs + 24, (uint32_t)y); wr32(g_mem, cs + 28, (uint32_t)x);
    wr32(g_mem, cs + 32, style); wr32(g_mem, cs + 36, name); wr32(g_mem, cs + 40, clsn); wr32(g_mem, cs + 44, ex);
    wr32(g_mem, cbt, cs); wr32(g_mem, cbt + 4, 0);
    if (call_hooks(5, 3, hwnd, cbt)) { destroy(hwnd); cpu = saved; rt_ret_stdcall(0, 12); return; }   /* HCBT_CREATEWND */
    if (!(style & WS_CHILD)) {
        memset(g_mem + mmi, 0, 40);
        msg_send(hwnd, WM_GETMINMAXINFO, 0, mmi);
    }
    if (!msg_send(hwnd, WM_NCCREATE, 0, cs)) { destroy(hwnd); cpu = saved; rt_ret_stdcall(0, 12); return; }
    msg_send(hwnd, WM_NCCALCSIZE, 0, mmi);
    if ((int32_t)msg_send(hwnd, WM_CREATE, 0, cs) == -1) { destroy(hwnd); cpu = saved; rt_ret_stdcall(0, 12); return; }
    msg_send(hwnd, WM_SIZE, 0, ((uint32_t)wd->h << 16) | ((uint32_t)wd->w & 0xffff));
    msg_send(hwnd, WM_MOVE, 0, ((uint32_t)wd->y << 16) | ((uint32_t)wd->x & 0xffff));
    if (!(style & WS_CHILD) && !main_wnd) main_wnd = hwnd;
    if (style & WS_VISIBLE) {
        wd->visible = 1;
        wd->dirty = 1;
        msg_send(hwnd, WM_SHOWWINDOW, 1, 0);
        if (!(style & WS_CHILD)) {
            msg_send(hwnd, WM_ACTIVATEAPP, 1, 0);
            msg_send(hwnd, WM_NCACTIVATE, 1, 0);
            msg_send(hwnd, WM_ACTIVATE, 1, 0);
            focus_hwnd = hwnd;
            msg_send(hwnd, WM_SETFOCUS, 0, 0);
        }
    }
    cpu = saved;
    rt_ret_stdcall(hwnd, 12);
}

static void destroy(uint32_t hwnd)
{
    Wnd *w = wnd_get(hwnd);
    if (!w) return;
    for (int i = 1; i < MAX_WND; i++)
        if (wnds[i].used && (wnds[i].style & WS_CHILD) && wnds[i].parent == hwnd) destroy(hwnd_of(i));
    msg_send(hwnd, WM_DESTROY, 0, 0);
    msg_send(hwnd, WM_NCDESTROY, 0, 0);
    w = wnd_get(hwnd);
    if (w) w->used = 0;
    if (focus_hwnd == hwnd) focus_hwnd = 0;
    if (capture_hwnd == hwnd) capture_hwnd = 0;
    if (main_wnd == hwnd) main_wnd = 0;
}
RT_IMPORT(DestroyWindow)
{
    Cpu saved = cpu;
    uint32_t h = rt_arg(0);
    destroy(h);
    cpu = saved;
    rt_ret_stdcall(1, 1);
}
RT_IMPORT(IsWindow) { rt_ret_stdcall(wnd_get(rt_arg(0)) != NULL, 1); }
RT_IMPORT(IsWindowVisible) { Wnd *w = wnd_get(rt_arg(0)); rt_ret_stdcall(w && w->visible, 1); }
RT_IMPORT(IsWindowEnabled) { Wnd *w = wnd_get(rt_arg(0)); rt_ret_stdcall(w && w->enabled, 1); }
RT_IMPORT(IsIconic) { rt_ret_stdcall(0, 1); }
RT_IMPORT(IsZoomed) { rt_ret_stdcall(0, 1); }
RT_IMPORT(IsChild)
{
    for (Wnd *w = wnd_get(rt_arg(1)); w && (w->style & WS_CHILD); w = wnd_get(w->parent))
        if (w->parent == rt_arg(0)) { rt_ret_stdcall(1, 2); return; }
    rt_ret_stdcall(0, 2);
}
RT_IMPORT(EnableWindow)
{
    Wnd *w = wnd_get(rt_arg(0));
    uint32_t was_disabled = w && !w->enabled;
    if (w) { w->enabled = rt_arg(1) != 0; Cpu s = cpu; msg_send(rt_arg(0), WM_ENABLE, rt_arg(1) != 0, 0); cpu = s; }
    rt_ret_stdcall(was_disabled, 2);
}

RT_IMPORT(ShowWindow)
{
    uint32_t hwnd = rt_arg(0), cmd = rt_arg(1);
    Wnd *w = wnd_get(hwnd);
    uint32_t was = w && w->visible;
    if (w) {
        int show = cmd != 0;                              /* SW_HIDE = 0 */
        Cpu s = cpu;
        if (show != w->visible) {
            w->visible = show;
            msg_send(hwnd, WM_SHOWWINDOW, (uint32_t)show, 0);
            if (show) w->dirty = 1;
        }
        if (show && !(w->style & WS_CHILD)) {
            if (!main_wnd) main_wnd = hwnd;
            msg_send(hwnd, WM_ACTIVATEAPP, 1, 0);
            msg_send(hwnd, WM_NCACTIVATE, 1, 0);
            msg_send(hwnd, WM_ACTIVATE, 1, 0);
            if (focus_hwnd != hwnd) { focus_hwnd = hwnd; msg_send(hwnd, WM_SETFOCUS, 0, 0); }
        }
        cpu = s;
    }
    rt_ret_stdcall(was, 2);
}
RT_IMPORT(UpdateWindow)
{
    Wnd *w = wnd_get(rt_arg(0));
    if (w && w->dirty && w->visible) { Cpu s = cpu; w->dirty = 0; msg_send(rt_arg(0), WM_PAINT, 0, 0); cpu = s; }
    rt_ret_stdcall(1, 1);
}

static void set_pos(uint32_t hwnd, int32_t x, int32_t y, int32_t cx, int32_t cy, uint32_t flags)
{
    Wnd *w = wnd_get(hwnd);
    if (!w) return;
    int moved = 0, sized = 0;
    if (!(flags & 2u) && (w->x != x || w->y != y)) { w->x = x; w->y = y; moved = 1; }     /* SWP_NOMOVE */
    if (!(flags & 1u) && (w->w != cx || w->h != cy)) { w->w = cx; w->h = cy; sized = 1; } /* SWP_NOSIZE */
    if (flags & 0x40u) w->visible = 1;                                                     /* SWP_SHOWWINDOW */
    if (flags & 0x80u) w->visible = 0;                                                     /* SWP_HIDEWINDOW */
    w->dirty = 1;
    Cpu s = cpu;
    if (sized) msg_send(hwnd, WM_SIZE, 0, ((uint32_t)w->h << 16) | ((uint32_t)w->w & 0xffff));
    if (moved) msg_send(hwnd, WM_MOVE, 0, ((uint32_t)w->y << 16) | ((uint32_t)w->x & 0xffff));
    cpu = s;
}
/* Lo que Windows le hace a la ventana de un juego que toma la pantalla (DirectDraw exclusivo + SetDisplayMode): la
 * lleva a (0,0) con el tamaño del modo, le manda WM_SIZE/WM_MOVE y la invalida (llega un WM_PAINT). ddraw.c lo llama. */
void wnd_fullscreen(uint32_t hwnd, uint32_t w, uint32_t h)
{
    Wnd *wd = wnd_get(hwnd ? hwnd : main_wnd);
    if (!wd) return;
    set_pos(hwnd ? hwnd : main_wnd, 0, 0, (int32_t)w, (int32_t)h, 0);
    for (int i = 1; i < MAX_WND; i++) if (wnds[i].used) wnds[i].dirty = 1;
}

RT_IMPORT(SetWindowPos) { set_pos(rt_arg(0), (int32_t)rt_arg(2), (int32_t)rt_arg(3), (int32_t)rt_arg(4), (int32_t)rt_arg(5), rt_arg(6)); rt_ret_stdcall(1, 7); }
RT_IMPORT(MoveWindow) { set_pos(rt_arg(0), (int32_t)rt_arg(1), (int32_t)rt_arg(2), (int32_t)rt_arg(3), (int32_t)rt_arg(4), 0); rt_ret_stdcall(1, 6); }
RT_IMPORT(BringWindowToTop) { rt_ret_stdcall(1, 1); }

static void put_rect(uint32_t p, int32_t l, int32_t t, int32_t r, int32_t b)
{
    wr32(g_mem, p, (uint32_t)l); wr32(g_mem, p + 4, (uint32_t)t); wr32(g_mem, p + 8, (uint32_t)r); wr32(g_mem, p + 12, (uint32_t)b);
}
RT_IMPORT(GetClientRect)
{
    Wnd *w = wnd_get(rt_arg(0));
    if (w) put_rect(rt_arg(1), 0, 0, w->w, w->h); else put_rect(rt_arg(1), 0, 0, (int32_t)screen.w, (int32_t)screen.h);
    rt_ret_stdcall(w != NULL, 2);
}
RT_IMPORT(GetWindowRect)
{
    int32_t x = 0, y = 0;
    Wnd *w = wnd_get(rt_arg(0));
    if (w) { wnd_screen_origin(rt_arg(0), &x, &y); put_rect(rt_arg(1), x, y, x + w->w, y + w->h); }
    else put_rect(rt_arg(1), 0, 0, (int32_t)screen.w, (int32_t)screen.h);
    rt_ret_stdcall(1, 2);
}
RT_IMPORT(AdjustWindowRect) { rt_ret_stdcall(1, 3); }       /* sin marco: el rectángulo no cambia */
RT_IMPORT(AdjustWindowRectEx) { rt_ret_stdcall(1, 4); }
RT_IMPORT(ClientToScreen)
{
    int32_t x, y;
    wnd_screen_origin(rt_arg(0), &x, &y);
    uint32_t p = rt_arg(1);
    wr32(g_mem, p, rd32(g_mem, p) + (uint32_t)x);
    wr32(g_mem, p + 4, rd32(g_mem, p + 4) + (uint32_t)y);
    rt_ret_stdcall(1, 2);
}
RT_IMPORT(ScreenToClient)
{
    int32_t x, y;
    wnd_screen_origin(rt_arg(0), &x, &y);
    uint32_t p = rt_arg(1);
    wr32(g_mem, p, rd32(g_mem, p) - (uint32_t)x);
    wr32(g_mem, p + 4, rd32(g_mem, p + 4) - (uint32_t)y);
    rt_ret_stdcall(1, 2);
}
RT_IMPORT(MapWindowPoints)
{
    int32_t fx, fy, tx, ty;
    wnd_screen_origin(rt_arg(0), &fx, &fy);
    wnd_screen_origin(rt_arg(1), &tx, &ty);
    uint32_t p = rt_arg(2), n = rt_arg(3);
    for (uint32_t i = 0; i < n; i++) {
        wr32(g_mem, p + 8 * i, rd32(g_mem, p + 8 * i) + (uint32_t)(fx - tx));
        wr32(g_mem, p + 8 * i + 4, rd32(g_mem, p + 8 * i + 4) + (uint32_t)(fy - ty));
    }
    rt_ret_stdcall(((uint32_t)(uint16_t)(fy - ty) << 16) | (uint16_t)(fx - tx), 4);
}

/* Get/SetWindowLongA: GWL_WNDPROC -4, GWL_HINSTANCE -6, GWL_HWNDPARENT -8, GWL_STYLE -16, GWL_EXSTYLE -20,
 * GWL_USERDATA -21, GWL_ID -12; positivos: los bytes extra de la clase. */
static uint32_t *long_slot(Wnd *w, int32_t idx)
{
    switch (idx) {
    case -4: return &w->proc;
    case -6: return &w->hinst;
    case -8: return (w->style & WS_CHILD) ? &w->parent : &w->owner;
    case -12: return &w->id;
    case -16: return &w->style;
    case -20: return &w->exstyle;
    case -21: return &w->userdata;
    }
    if (idx >= 0 && idx + 4 <= (int32_t)sizeof w->extra) return (uint32_t *)(w->extra + idx);
    return NULL;
}
RT_IMPORT(GetWindowLongA)
{
    Wnd *w = wnd_get(rt_arg(0));
    uint32_t *s = w ? long_slot(w, (int32_t)rt_arg(1)) : NULL;
    uint32_t v = 0;
    if (s) memcpy(&v, s, 4);
    if (s && (int32_t)rt_arg(1) == -16 && w->visible) v |= WS_VISIBLE;
    rt_ret_stdcall(v, 2);
}
RT_IMPORT(SetWindowLongA)
{
    Wnd *w = wnd_get(rt_arg(0));
    uint32_t *s = w ? long_slot(w, (int32_t)rt_arg(1)) : NULL;
    uint32_t old = 0, v = rt_arg(2);
    if (s) {
        memcpy(&old, s, 4);
        if ((int32_t)rt_arg(1) == -16) { w->visible = (v & WS_VISIBLE) != 0; v &= ~WS_VISIBLE; }
        memcpy(s, &v, 4);
    }
    rt_ret_stdcall(old, 3);
}
RT_IMPORT(GetWindowWord) { rt_ret_stdcall(0, 2); }
RT_IMPORT(GetParent)
{
    Wnd *w = wnd_get(rt_arg(0));
    rt_ret_stdcall(w ? ((w->style & WS_CHILD) ? w->parent : w->owner) : 0, 1);
}
RT_IMPORT(SetParent) { Wnd *w = wnd_get(rt_arg(0)); uint32_t old = w ? w->parent : 0; if (w) w->parent = rt_arg(1); rt_ret_stdcall(old, 2); }
RT_IMPORT(GetDlgCtrlID) { Wnd *w = wnd_get(rt_arg(0)); rt_ret_stdcall(w ? w->id : 0, 1); }
RT_IMPORT(GetDlgItem)
{
    for (int i = 1; i < MAX_WND; i++)
        if (wnds[i].used && (wnds[i].style & WS_CHILD) && wnds[i].parent == rt_arg(0) && wnds[i].id == rt_arg(1)) { rt_ret_stdcall(hwnd_of(i), 2); return; }
    rt_ret_stdcall(0, 2);
}

/* GetWindow: GW_HWNDFIRST 0, LAST 1, NEXT 2, PREV 3, OWNER 4, CHILD 5. */
RT_IMPORT(GetWindow)
{
    uint32_t hwnd = rt_arg(0), cmd = rt_arg(1), r = 0;
    Wnd *w = wnd_get(hwnd);
    if (w && cmd == 4) r = w->owner;
    else if (w && cmd == 5) { for (int i = 1; i < MAX_WND && !r; i++) if (wnds[i].used && (wnds[i].style & WS_CHILD) && wnds[i].parent == hwnd) r = hwnd_of(i); }
    else if (w && cmd == 2) {
        int child = (w->style & WS_CHILD) != 0;
        for (int i = (int)((hwnd - HWND_BASE) / 4) + 1; i < MAX_WND && !r; i++)
            if (wnds[i].used && ((wnds[i].style & WS_CHILD) != 0) == child && (!child || wnds[i].parent == w->parent)) r = hwnd_of(i);
    }
    rt_ret_stdcall(r, 2);
}
RT_IMPORT(GetTopWindow)
{
    uint32_t r = 0, p = rt_arg(0);
    for (int i = 1; i < MAX_WND && !r; i++)
        if (wnds[i].used && (p ? ((wnds[i].style & WS_CHILD) && wnds[i].parent == p) : !(wnds[i].style & WS_CHILD))) r = hwnd_of(i);
    rt_ret_stdcall(r, 1);
}
RT_IMPORT(GetLastActivePopup) { rt_ret_stdcall(rt_arg(0), 1); }
RT_IMPORT(GetDesktopWindow) { rt_ret_stdcall(0x0001fff0u, 0); }
RT_IMPORT(GetActiveWindow) { rt_ret_stdcall(main_wnd, 0); }
RT_IMPORT(GetForegroundWindow) { rt_ret_stdcall(main_wnd, 0); }
RT_IMPORT(SetForegroundWindow) { rt_ret_stdcall(1, 1); }
RT_IMPORT(SetActiveWindow) { uint32_t old = main_wnd; rt_ret_stdcall(old, 1); }
RT_IMPORT(GetFocus) { rt_ret_stdcall(focus_hwnd, 0); }
RT_IMPORT(SetFocus)
{
    uint32_t old = focus_hwnd, h = rt_arg(0);
    if (h != old) {
        Cpu s = cpu;
        if (old) msg_send(old, WM_KILLFOCUS, h, 0);
        focus_hwnd = h;
        if (h) msg_send(h, WM_SETFOCUS, old, 0);
        cpu = s;
    }
    rt_ret_stdcall(old, 1);
}
RT_IMPORT(GetCapture) { rt_ret_stdcall(capture_hwnd, 0); }
RT_IMPORT(SetCapture) { uint32_t old = capture_hwnd; capture_hwnd = rt_arg(0); rt_ret_stdcall(old, 1); }
RT_IMPORT(ReleaseCapture) { capture_hwnd = 0; rt_ret_stdcall(1, 0); }
RT_IMPORT(WindowFromPoint) { rt_ret_stdcall(main_wnd, 2); }
RT_IMPORT(ChildWindowFromPoint) { rt_ret_stdcall(rt_arg(0), 3); }
RT_IMPORT(GetMenu) { rt_ret_stdcall(0, 1); }
RT_IMPORT(SetMenu) { rt_ret_stdcall(1, 2); }
RT_IMPORT(GetSystemMenu) { rt_ret_stdcall(0, 2); }
RT_IMPORT(DrawMenuBar) { rt_ret_stdcall(1, 1); }

RT_IMPORT(SetWindowTextA)
{
    Wnd *w = wnd_get(rt_arg(0));
    if (w) snprintf(w->text, sizeof w->text, "%s", rt_arg(1) ? win_str(rt_arg(1)) : "");
    rt_ret_stdcall(w != NULL, 2);
}
RT_IMPORT(GetWindowTextA)
{
    Wnd *w = wnd_get(rt_arg(0));
    win_put_str(rt_arg(1), w ? w->text : "", rt_arg(2));
    rt_ret_stdcall((uint32_t)strlen(win_str(rt_arg(1))), 3);
}
RT_IMPORT(GetWindowTextLengthA) { Wnd *w = wnd_get(rt_arg(0)); rt_ret_stdcall(w ? (uint32_t)strlen(w->text) : 0, 1); }

/* ---- procedimiento por defecto ---- */
static void host_DefWindowProcA(void)
{
    uint32_t hwnd = rt_arg(0), msg = rt_arg(1), wp = rt_arg(2), lp = rt_arg(3), r = 0;
    Wnd *w = wnd_get(hwnd);
    Cpu s = cpu;
    switch (msg) {
    case WM_NCCREATE:
        if (w && lp && rd32(g_mem, lp + 36) >= 0x10000) snprintf(w->text, sizeof w->text, "%s", win_str(rd32(g_mem, lp + 36)));
        r = 1;
        break;
    case WM_CLOSE: destroy(hwnd); break;
    case WM_ERASEBKGND: r = 1; break;
    case WM_SETTEXT: if (w) snprintf(w->text, sizeof w->text, "%s", lp ? win_str(lp) : ""); r = 1; break;
    case WM_GETTEXT: if (w) win_put_str(lp, w->text, wp); r = lp ? (uint32_t)strlen(win_str(lp)) : 0; break;
    case WM_GETTEXTLENGTH: r = w ? (uint32_t)strlen(w->text) : 0; break;
    case WM_NCHITTEST: r = 1; break;                 /* HTCLIENT */
    case WM_MOUSEACTIVATE: r = 1; break;             /* MA_ACTIVATE */
    case WM_PAINT: if (w) w->dirty = 0; break;
    case WM_SYSCOMMAND: if ((wp & 0xfff0u) == 0xF060u) msg_post(hwnd, WM_CLOSE, 0, 0); break;   /* SC_CLOSE */
    case 0x0011: r = 1; break;                       /* WM_QUERYENDSESSION */
    case WM_WINDOWPOSCHANGED:
        if (w && lp) { msg_send(hwnd, WM_SIZE, 0, ((uint32_t)w->h << 16) | ((uint32_t)w->w & 0xffff)); msg_send(hwnd, WM_MOVE, 0, ((uint32_t)w->y << 16) | ((uint32_t)w->x & 0xffff)); }
        break;
    case WM_SETCURSOR:
        if (w && (w->style & WS_CHILD) && w->parent) r = msg_send(w->parent, WM_SETCURSOR, wp, lp);
        break;
    default: break;
    }
    cpu = s;
    rt_ret_stdcall(r, 4);
}
RT_IMPORT(DefWindowProcA) { host_DefWindowProcA(); }
RT_IMPORT(DefDlgProcA) { host_DefWindowProcA(); }
RT_IMPORT(DefFrameProcA)
{
    /* DefFrameProc(hwnd, hwndMDIClient, msg, wp, lp): igual que DefWindowProc sin el segundo argumento. */
    uint32_t hwnd = rt_arg(0), msg = rt_arg(2), wp = rt_arg(3), lp = rt_arg(4);
    wr32(g_mem, cpu.esp + 8, msg); wr32(g_mem, cpu.esp + 12, wp); wr32(g_mem, cpu.esp + 16, lp);
    (void)hwnd;
    host_DefWindowProcA();
    cpu.esp += 4;
}
RT_IMPORT(DefMDIChildProcA) { host_DefWindowProcA(); }
RT_IMPORT(CallWindowProcA)
{
    uint32_t proc = rt_arg(0);
    uint32_t a[4] = {rt_arg(1), rt_arg(2), rt_arg(3), rt_arg(4)};
    Cpu s = cpu;
    uint32_t r = proc ? win_call(proc, 4, a) : 0;
    cpu = s;
    rt_ret_stdcall(r, 5);
}
RT_IMPORT(SendMessageA)
{
    Cpu s = cpu;
    uint32_t r = msg_send(rt_arg(0), rt_arg(1), rt_arg(2), rt_arg(3));
    cpu = s;
    rt_ret_stdcall(r, 4);
}
RT_IMPORT(SendDlgItemMessageA) { rt_ret_stdcall(0, 5); }

/* ---- cola de mensajes ---- */
#define QSIZE 1024
typedef struct { uint32_t hwnd, msg, wp, lp, time; int32_t x, y; } Msg;
static Msg queue[QSIZE];
static int qhead, qcount;
static int quit_posted;
static uint32_t quit_code;

void msg_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    if (qcount == QSIZE) { fprintf(stderr, "recomp: cola de mensajes llena, se descarta %#x\n", msg); return; }
    Msg *m = &queue[(qhead + qcount++) % QSIZE];
    m->hwnd = hwnd; m->msg = msg; m->wp = wp; m->lp = lp; m->time = win_ticks_ms(); m->x = mouse_x; m->y = mouse_y;
}
RT_IMPORT(PostMessageA) { msg_post(rt_arg(0), rt_arg(1), rt_arg(2), rt_arg(3)); rt_ret_stdcall(1, 4); }
RT_IMPORT(PostThreadMessageA) { msg_post(0, rt_arg(1), rt_arg(2), rt_arg(3)); rt_ret_stdcall(1, 4); }
RT_IMPORT(PostQuitMessage) { quit_posted = 1; quit_code = rt_arg(0); rt_ret_stdcall(0, 1); }

/* Timers. */
#define MAX_TIMERS 64
static struct { int used; uint32_t hwnd, id, ms, proc, next; } timers[MAX_TIMERS];
RT_IMPORT(SetTimer)
{
    uint32_t hwnd = rt_arg(0), id = rt_arg(1);
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) if (timers[i].used && timers[i].hwnd == hwnd && timers[i].id == id) slot = i;
    for (int i = 0; i < MAX_TIMERS && slot < 0; i++) if (!timers[i].used) slot = i;
    if (slot < 0) { rt_ret_stdcall(0, 4); return; }
    if (!hwnd && !id) id = 0x100 + (uint32_t)slot;
    timers[slot].used = 1; timers[slot].hwnd = hwnd; timers[slot].id = id;
    timers[slot].ms = rt_arg(2) ? rt_arg(2) : 1; timers[slot].proc = rt_arg(3);
    timers[slot].next = win_ticks_ms() + timers[slot].ms;
    WIN_TRACE("SetTimer(%#x, %u, %u ms)\n", hwnd, id, rt_arg(2));
    rt_ret_stdcall(id, 4);
}
RT_IMPORT(KillTimer)
{
    for (int i = 0; i < MAX_TIMERS; i++) if (timers[i].used && timers[i].hwnd == rt_arg(0) && timers[i].id == rt_arg(1)) timers[i].used = 0;
    rt_ret_stdcall(1, 2);
}

static int filter_ok(const Msg *m, uint32_t hwnd, uint32_t lo, uint32_t hi)
{
    if (hwnd && m->hwnd != hwnd) return 0;
    if (lo || hi) return m->msg >= lo && m->msg <= hi;
    return 1;
}

/* Saca el próximo mensaje para (hwnd, lo, hi): la cola, después WM_PAINT de una ventana sucia, después un timer
 * vencido. 0 si no hay nada. */
static int next_message(Msg *out, uint32_t hwnd, uint32_t lo, uint32_t hi, int remove)
{
    for (int k = 0; k < qcount; k++) {
        Msg *m = &queue[(qhead + k) % QSIZE];
        if (!filter_ok(m, hwnd, lo, hi)) continue;
        *out = *m;
        if (remove) {
            for (int j = k; j > 0; j--) queue[(qhead + j) % QSIZE] = queue[(qhead + j - 1) % QSIZE];
            qhead = (qhead + 1) % QSIZE;
            qcount--;
        }
        return 1;
    }
    if (quit_posted && (!lo || lo <= WM_QUIT) && (!hi || hi >= WM_QUIT)) {
        memset(out, 0, sizeof *out);
        out->msg = WM_QUIT; out->wp = quit_code;
        if (remove) quit_posted = 0;
        return 1;
    }
    for (int i = 1; i < MAX_WND; i++)
        if (wnds[i].used && wnds[i].visible && wnds[i].dirty && (!hwnd || hwnd_of(i) == hwnd) && (!lo || (lo <= WM_PAINT && hi >= WM_PAINT))) {
            memset(out, 0, sizeof *out);
            out->hwnd = hwnd_of(i); out->msg = WM_PAINT; out->time = win_ticks_ms();
            return 1;                                    /* se limpia al validar (BeginPaint/ValidateRect) */
        }
    uint32_t now = win_ticks_ms();
    for (int i = 0; i < MAX_TIMERS; i++)
        if (timers[i].used && (int32_t)(now - timers[i].next) >= 0 && (!hwnd || timers[i].hwnd == hwnd) && (!lo || (lo <= WM_TIMER && hi >= WM_TIMER))) {
            memset(out, 0, sizeof *out);
            out->hwnd = timers[i].hwnd; out->msg = WM_TIMER; out->wp = timers[i].id; out->lp = timers[i].proc; out->time = now;
            if (remove) timers[i].next = now + timers[i].ms;
            return 1;
        }
    return 0;
}

static void put_msg(uint32_t p, const Msg *m)
{
    wr32(g_mem, p, m->hwnd); wr32(g_mem, p + 4, m->msg); wr32(g_mem, p + 8, m->wp); wr32(g_mem, p + 12, m->lp);
    wr32(g_mem, p + 16, m->time); wr32(g_mem, p + 20, (uint32_t)m->x); wr32(g_mem, p + 24, (uint32_t)m->y);
}

RT_IMPORT(GetMessageA)
{
    uint32_t p = rt_arg(0), hwnd = rt_arg(1), lo = rt_arg(2), hi = rt_arg(3);
    Msg m;
    static uint32_t ng;
    if (win_trace >= 3 && ng++ < 100) fprintf(stderr, "GetMessageA hilo %u (cola %d) desde %#x\n", win_current_thread_id(), qcount, rd32(g_mem, cpu.esp));
    for (;;) {
        win_pump_host_events();
        if (next_message(&m, hwnd, lo, hi, 1)) break;
        rt_host_idle();
        win_yield();
    }
    put_msg(p, &m);
    if (m.msg >= 0x100 && m.msg <= 0x102 && win_trace_keys()) fprintf(stderr, "TECLA sale de la cola por GetMessage (msg %#x hwnd %#x) desde %#x\n", m.msg, m.hwnd, rd32(g_mem, cpu.esp));
    rt_ret_stdcall(m.msg != WM_QUIT, 4);
}

static uint32_t empty_peeks;
RT_IMPORT(PeekMessageA)
{
    uint32_t p = rt_arg(0), hwnd = rt_arg(1), lo = rt_arg(2), hi = rt_arg(3), flags = rt_arg(4);
    Msg m;
    static uint32_t callers[16], ncallers;
    if (win_trace >= 3) {
        uint32_t c = rd32(g_mem, cpu.esp), k = 0;
        while (k < ncallers && callers[k] != c) k++;
        if (k == ncallers && ncallers < 16) { callers[ncallers++] = c; fprintf(stderr, "PeekMessageA nuevo llamador %#x (hwnd %#x, %#x-%#x, flags %#x)\n", c, hwnd, lo, hi, flags); }
    }
    win_pump_host_events();
    int got = next_message(&m, hwnd, lo, hi, flags & 1u);
    if (got) {
        put_msg(p, &m);
        empty_peeks = 0;
        if (m.msg >= 0x100 && m.msg <= 0x102 && win_trace_keys())
            fprintf(stderr, "TECLA PeekMessage(%s) msg %#x hwnd %#x desde %#x\n", flags & 1u ? "saca" : "mira", m.msg, m.hwnd, rd32(g_mem, cpu.esp));
    }
    else if (++empty_peeks % 8 == 0) { win_yield(); rt_host_idle(); }   /* un bucle de juego que sólo espía: ceder */
    rt_ret_stdcall(got, 5);
}
RT_IMPORT(WaitMessage) { win_pump_host_events(); rt_host_idle(); win_yield(); rt_ret_stdcall(1, 0); }
RT_IMPORT(MsgWaitForMultipleObjects) { win_pump_host_events(); rt_host_idle(); win_yield(); rt_ret_stdcall(rt_arg(0), 5); }
RT_IMPORT(GetQueueStatus) { win_pump_host_events(); rt_ret_stdcall(qcount ? 0x00ff00ffu : 0, 1); }
RT_IMPORT(GetInputState) { rt_ret_stdcall(qcount != 0, 0); }
RT_IMPORT(GetMessageTime) { rt_ret_stdcall(win_ticks_ms(), 0); }
RT_IMPORT(GetMessagePos) { rt_ret_stdcall(((uint32_t)(uint16_t)mouse_y << 16) | (uint16_t)mouse_x, 0); }
RT_IMPORT(InSendMessage) { rt_ret_stdcall(0, 0); }

/* TranslateMessage: WM_KEYDOWN de una tecla con carácter -> WM_CHAR. */
RT_IMPORT(TranslateMessage)
{
    uint32_t p = rt_arg(0), msg = rd32(g_mem, p + 4), vk = rd32(g_mem, p + 8);
    int c = 0;
    if (msg == WM_KEYDOWN) {
        int shift = key_down[0x10] != 0;
        if (vk >= 'A' && vk <= 'Z') c = shift ? (int)vk : (int)vk + 32;
        else if (vk >= '0' && vk <= '9') c = shift ? ")!\"#$%&/()"[vk - '0'] : (int)vk;   /* teclado español */
        else if (vk == 0x20 || vk == 0x0d || vk == 0x08 || vk == 0x1b || vk == 0x09) c = (int)vk;
        else if (vk >= 0x60 && vk <= 0x69) c = '0' + (int)(vk - 0x60);
        else if (vk == 0xBE) c = shift ? ':' : '.';
        else if (vk == 0xBC) c = shift ? ';' : ',';
        else if (vk == 0xBD || vk == 0x6D) c = shift ? '_' : '-';
    }
    if (c) msg_post(rd32(g_mem, p), WM_CHAR, (uint32_t)c, rd32(g_mem, p + 12));
    rt_ret_stdcall(c != 0, 1);
}
RT_IMPORT(TranslateAcceleratorA) { rt_ret_stdcall(0, 3); }
RT_IMPORT(IsDialogMessageA) { rt_ret_stdcall(0, 2); }

RT_IMPORT(DispatchMessageA)
{
    uint32_t p = rt_arg(0);
    uint32_t hwnd = rd32(g_mem, p), msg = rd32(g_mem, p + 4), wp = rd32(g_mem, p + 8), lp = rd32(g_mem, p + 12);
    Cpu s = cpu;
    uint32_t r;
    if (msg >= 0x100 && msg <= 0x102 && win_trace_keys()) {
        Wnd *w = wnd_get(hwnd);
        fprintf(stderr, "TECLA DispatchMessage msg %#x vk %#x a hwnd %#x (proc %#x)\n", msg, wp, hwnd, w ? w->proc : 0);
    }
    if (msg == WM_TIMER && lp) { uint32_t a[4] = {hwnd, msg, wp, win_ticks_ms()}; r = win_call(lp, 4, a); }
    else r = msg_send(hwnd, msg, wp, lp);
    if (msg == WM_PAINT) { Wnd *w = wnd_get(hwnd); if (w) w->dirty = 0; }   /* un WM_PAINT sin BeginPaint no repite */
    cpu = s;
    rt_ret_stdcall(r, 1);
}

/* ---- pintar ---- */
uint32_t gdi_window_dc(uint32_t hwnd);       /* gdi32.c */
void gdi_release_dc(uint32_t hdc);
RT_IMPORT(InvalidateRect) { Wnd *w = wnd_get(rt_arg(0)); if (w) w->dirty = 1; else for (int i = 1; i < MAX_WND; i++) if (wnds[i].used) wnds[i].dirty = 1; rt_ret_stdcall(1, 3); }
RT_IMPORT(InvalidateRgn) { Wnd *w = wnd_get(rt_arg(0)); if (w) w->dirty = 1; rt_ret_stdcall(1, 3); }
RT_IMPORT(ValidateRect) { Wnd *w = wnd_get(rt_arg(0)); if (w) w->dirty = 0; rt_ret_stdcall(1, 2); }
RT_IMPORT(ValidateRgn) { Wnd *w = wnd_get(rt_arg(0)); if (w) w->dirty = 0; rt_ret_stdcall(1, 2); }
RT_IMPORT(GetUpdateRect)
{
    Wnd *w = wnd_get(rt_arg(0));
    if (rt_arg(1)) put_rect(rt_arg(1), 0, 0, w && w->dirty ? w->w : 0, w && w->dirty ? w->h : 0);
    rt_ret_stdcall(w && w->dirty, 3);
}
RT_IMPORT(BeginPaint)
{
    uint32_t hwnd = rt_arg(0), ps = rt_arg(1);
    Wnd *w = wnd_get(hwnd);
    uint32_t hdc = gdi_window_dc(hwnd);
    memset(g_mem + ps, 0, 64);
    wr32(g_mem, ps, hdc);
    wr32(g_mem, ps + 4, 1);
    if (w) { put_rect(ps + 8, 0, 0, w->w, w->h); w->dirty = 0; }
    rt_ret_stdcall(hdc, 2);
}
RT_IMPORT(EndPaint) { gdi_release_dc(rd32(g_mem, rt_arg(1))); rt_ret_stdcall(1, 2); }
RT_IMPORT(GetDC) { rt_ret_stdcall(gdi_window_dc(rt_arg(0)), 1); }
RT_IMPORT(GetDCEx) { rt_ret_stdcall(gdi_window_dc(rt_arg(0)), 3); }
RT_IMPORT(GetWindowDC) { rt_ret_stdcall(gdi_window_dc(rt_arg(0)), 1); }
RT_IMPORT(ReleaseDC) { gdi_release_dc(rt_arg(1)); rt_ret_stdcall(1, 2); }
RT_IMPORT(FillRect);           /* gdi32.c */
RT_IMPORT(FrameRect) { rt_ret_stdcall(1, 3); }
RT_IMPORT(DrawTextA) { rt_ret_stdcall(16, 5); }
RT_IMPORT(DrawIcon) { rt_ret_stdcall(1, 4); }
RT_IMPORT(GetSysColor) { static const uint32_t c[] = {0xc0c0c0, 0x808000, 0x800000, 0x808080, 0xc0c0c0, 0xffffff, 0, 0, 0, 0xffffff}; uint32_t i = rt_arg(0); rt_ret_stdcall(i < 10 ? c[i] : 0xc0c0c0u, 1); }
RT_IMPORT(GetSysColorBrush) { rt_ret_stdcall(0x00080000u + rt_arg(0), 1); }

/* ---- rectángulos ---- */
RT_IMPORT(SetRect) { put_rect(rt_arg(0), (int32_t)rt_arg(1), (int32_t)rt_arg(2), (int32_t)rt_arg(3), (int32_t)rt_arg(4)); rt_ret_stdcall(1, 5); }
RT_IMPORT(SetRectEmpty) { put_rect(rt_arg(0), 0, 0, 0, 0); rt_ret_stdcall(1, 1); }
RT_IMPORT(CopyRect) { memcpy(g_mem + rt_arg(0), g_mem + rt_arg(1), 16); rt_ret_stdcall(1, 2); }
RT_IMPORT(EqualRect) { rt_ret_stdcall(!memcmp(g_mem + rt_arg(0), g_mem + rt_arg(1), 16), 2); }
RT_IMPORT(IsRectEmpty)
{
    uint32_t r = rt_arg(0);
    rt_ret_stdcall((int32_t)rd32(g_mem, r + 8) <= (int32_t)rd32(g_mem, r) || (int32_t)rd32(g_mem, r + 12) <= (int32_t)rd32(g_mem, r + 4), 1);
}
RT_IMPORT(PtInRect)
{
    uint32_t r = rt_arg(0);
    int32_t x = (int32_t)rt_arg(1), y = (int32_t)rt_arg(2);
    rt_ret_stdcall(x >= (int32_t)rd32(g_mem, r) && x < (int32_t)rd32(g_mem, r + 8) && y >= (int32_t)rd32(g_mem, r + 4) && y < (int32_t)rd32(g_mem, r + 12), 3);
}
RT_IMPORT(OffsetRect)
{
    uint32_t r = rt_arg(0), dx = rt_arg(1), dy = rt_arg(2);
    put_rect(r, (int32_t)(rd32(g_mem, r) + dx), (int32_t)(rd32(g_mem, r + 4) + dy), (int32_t)(rd32(g_mem, r + 8) + dx), (int32_t)(rd32(g_mem, r + 12) + dy));
    rt_ret_stdcall(1, 3);
}
RT_IMPORT(InflateRect)
{
    uint32_t r = rt_arg(0), dx = rt_arg(1), dy = rt_arg(2);
    put_rect(r, (int32_t)(rd32(g_mem, r) - dx), (int32_t)(rd32(g_mem, r + 4) - dy), (int32_t)(rd32(g_mem, r + 8) + dx), (int32_t)(rd32(g_mem, r + 12) + dy));
    rt_ret_stdcall(1, 3);
}
static int32_t mx(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t mn(int32_t a, int32_t b) { return a < b ? a : b; }
RT_IMPORT(IntersectRect)
{
    uint32_t d = rt_arg(0), a = rt_arg(1), b = rt_arg(2);
    int32_t l = mx((int32_t)rd32(g_mem, a), (int32_t)rd32(g_mem, b)), t = mx((int32_t)rd32(g_mem, a + 4), (int32_t)rd32(g_mem, b + 4));
    int32_t r = mn((int32_t)rd32(g_mem, a + 8), (int32_t)rd32(g_mem, b + 8)), bo = mn((int32_t)rd32(g_mem, a + 12), (int32_t)rd32(g_mem, b + 12));
    if (r <= l || bo <= t) { put_rect(d, 0, 0, 0, 0); rt_ret_stdcall(0, 3); return; }
    put_rect(d, l, t, r, bo);
    rt_ret_stdcall(1, 3);
}
RT_IMPORT(UnionRect)
{
    uint32_t d = rt_arg(0), a = rt_arg(1), b = rt_arg(2);
    put_rect(d, mn((int32_t)rd32(g_mem, a), (int32_t)rd32(g_mem, b)), mn((int32_t)rd32(g_mem, a + 4), (int32_t)rd32(g_mem, b + 4)),
             mx((int32_t)rd32(g_mem, a + 8), (int32_t)rd32(g_mem, b + 8)), mx((int32_t)rd32(g_mem, a + 12), (int32_t)rd32(g_mem, b + 12)));
    rt_ret_stdcall(1, 3);
}

/* ---- cursor, íconos, sistema ---- */
static int cursor_count;
RT_IMPORT(LoadIconA) { rt_ret_stdcall(0x00060000u + (rt_arg(1) & 0xffff), 2); }
RT_IMPORT(DestroyIcon) { rt_ret_stdcall(1, 1); }
RT_IMPORT(ShowCursor)
{
    cursor_count += rt_arg(0) ? 1 : -1;
    screen_show_cursor(cursor_count >= 0);
    rt_ret_stdcall((uint32_t)cursor_count, 1);
}
RT_IMPORT(GetCursorPos) { win_poll_hint(); wr32(g_mem, rt_arg(0), (uint32_t)mouse_x); wr32(g_mem, rt_arg(0) + 4, (uint32_t)mouse_y); rt_ret_stdcall(1, 1); }
RT_IMPORT(SetCursorPos) { mouse_x = (int32_t)rt_arg(0); mouse_y = (int32_t)rt_arg(1); rt_ret_stdcall(1, 2); }
RT_IMPORT(ClipCursor) { rt_ret_stdcall(1, 1); }
RT_IMPORT(GetKeyState) { win_poll_hint(); uint32_t vk = rt_arg(0) & 0xff; rt_ret_stdcall(key_down[vk] ? 0xff80u : 0u, 1); }
RT_IMPORT(GetAsyncKeyState) { win_poll_hint(); uint32_t vk = rt_arg(0) & 0xff; rt_ret_stdcall(key_down[vk] ? 0x8000u : 0u, 1); }
RT_IMPORT(GetKeyboardState) { memset(g_mem + rt_arg(0), 0, 256); for (int i = 0; i < 256; i++) if (key_down[i]) g_mem[rt_arg(0) + (uint32_t)i] = 0x80; rt_ret_stdcall(1, 1); }
/* MapVirtualKeyA(código, tipo): 0 tecla virtual -> escaneo, 1 escaneo -> tecla virtual, 2 tecla virtual -> carácter,
 * 3 escaneo -> tecla virtual (distingue izquierda/derecha; acá igual que 1). */
RT_IMPORT(MapVirtualKeyA)
{
    uint32_t code = rt_arg(0), type = rt_arg(1), r = 0;
    switch (type) {
    case 0: r = win_vk_to_scan(code) & 0xFFu; break;
    case 1: case 3:
        for (uint32_t vk = 1; vk < 256 && !r; vk++)
            if ((win_vk_to_scan(vk) & 0xFFu) == (code & 0xFFu) && code) r = vk;
        break;
    case 2: r = (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') || code == 0x20 ? code : 0; break;
    }
    rt_ret_stdcall(r, 2);
}
RT_IMPORT(MessageBeep) { rt_ret_stdcall(1, 1); }
RT_IMPORT(GetDoubleClickTime) { rt_ret_stdcall(500, 0); }

RT_IMPORT(GetSystemMetrics)
{
    uint32_t i = rt_arg(0), r = 0;
    switch (i) {
    case 0: case 16: r = screen.w; break;      /* SM_CXSCREEN, SM_CXFULLSCREEN */
    case 1: case 17: r = screen.h; break;
    case 2: case 3: case 20: case 21: r = 16; break;
    case 4: r = 19; break;                      /* SM_CYCAPTION */
    case 5: case 6: r = 1; break;
    case 7: case 8: r = 3; break;
    case 11: case 12: r = 32; break;            /* íconos */
    case 13: case 14: r = 32; break;            /* cursor */
    case 15: r = 19; break;                     /* menú */
    case 32: case 33: r = 4; break;             /* SM_CXFRAME */
    case 43: r = 3; break;                      /* SM_CMOUSEBUTTONS */
    case 19: r = 1; break;                      /* SM_MOUSEPRESENT */
    default: r = 0; break;
    }
    rt_ret_stdcall(r, 1);
}

RT_IMPORT(SystemParametersInfoA)
{
    uint32_t action = rt_arg(0), pv = rt_arg(2);
    if (action == 0x30 && pv) put_rect(pv, 0, 0, (int32_t)screen.w, (int32_t)screen.h);      /* SPI_GETWORKAREA */
    else if (action == 0x29 && pv) memset(g_mem + pv + 4, 0, rd32(g_mem, pv) > 4 ? rd32(g_mem, pv) - 4 : 0);   /* NONCLIENTMETRICS */
    rt_ret_stdcall(1, 4);
}
RT_IMPORT(ChangeDisplaySettingsA) { rt_ret_stdcall(0, 2); }       /* DISP_CHANGE_SUCCESSFUL */
RT_IMPORT(EnumDisplaySettingsA) { rt_ret_stdcall(0, 3); }
RT_IMPORT(ExitWindowsEx) { rt_ret_stdcall(0, 2); }
RT_IMPORT(WinHelpA) { rt_ret_stdcall(1, 4); }
RT_IMPORT(CharUpperA)
{
    uint32_t p = rt_arg(0);
    if (p < 0x10000) { rt_ret_stdcall((uint32_t)toupper((int)p), 1); return; }
    for (char *s = win_str(p); *s; s++) *s = (char)toupper((unsigned char)*s);
    rt_ret_stdcall(p, 1);
}
RT_IMPORT(CharLowerA)
{
    uint32_t p = rt_arg(0);
    if (p < 0x10000) { rt_ret_stdcall((uint32_t)tolower((int)p), 1); return; }
    for (char *s = win_str(p); *s; s++) *s = (char)tolower((unsigned char)*s);
    rt_ret_stdcall(p, 1);
}
/* CharLowerBuffA / CharUpperBuffA(texto, n): n caracteres en el lugar, con las letras de Windows-1252 (Á→á, Ñ→ñ; ×, ÷ y
 * ß no cambian), como en un Windows en español. PC Fútbol 2001 lo pide al pasar de la pretemporada. */
static uint8_t lower_1252(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? (uint8_t)(c + 0x20) : c; }
static uint8_t upper_1252(uint8_t c) { return (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? (uint8_t)(c - 0x20) : c; }
RT_IMPORT(CharLowerBuffA)
{
    uint32_t p = rt_arg(0), n = rt_arg(1);
    for (uint32_t i = 0; i < n; i++) g_mem[p + i] = lower_1252(g_mem[p + i]);
    rt_ret_stdcall(n, 2);
}
RT_IMPORT(CharUpperBuffA)
{
    uint32_t p = rt_arg(0), n = rt_arg(1);
    for (uint32_t i = 0; i < n; i++) g_mem[p + i] = upper_1252(g_mem[p + i]);
    rt_ret_stdcall(n, 2);
}
RT_IMPORT(CharNextA) { uint32_t p = rt_arg(0); rt_ret_stdcall(g_mem[p] ? p + 1 : p, 1); }
RT_IMPORT(CharPrevA) { uint32_t s = rt_arg(0), p = rt_arg(1); rt_ret_stdcall(p > s ? p - 1 : s, 2); }
RT_IMPORT(IsCharAlphaA) { rt_ret_stdcall(isalpha((int)(rt_arg(0) & 0xff)) != 0, 1); }

/* MessageBoxA: no hay quién lo mire; se escribe y se contesta lo primero (IDOK/IDYES). */
RT_IMPORT(MessageBoxA)
{
    uint32_t type = rt_arg(3);
    fprintf(stderr, "recomp: MessageBox [%s] %s\n", rt_arg(2) ? win_str(rt_arg(2)) : "", rt_arg(1) ? win_str(rt_arg(1)) : "");
    uint32_t kind = type & 0xf, r = 1;
    if (kind == 4 || kind == 3) r = 6;                 /* MB_YESNO(CANCEL) -> IDYES */
    else if (kind == 2) r = 3;                         /* MB_ABORTRETRYIGNORE -> IDABORT */
    else if (kind == 5) r = 4;                         /* MB_RETRYCANCEL -> IDRETRY */
    rt_ret_stdcall(r, 4);
}

/* ---- wsprintfA (cdecl, argumentos variables en la pila del juego) ---- */
static int format_win(char *out, size_t room, const char *fmt, uint32_t args)
{
    size_t n = 0;
    uint32_t ai = 0;
#define PUT(c) do { if (n + 1 < room) out[n] = (char)(c); n++; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUT(*p); continue; }
        char spec[32] = "%";
        int k = 1;
        p++;
        while (*p && strchr("-+ #0", *p) && k < 20) spec[k++] = *p++;
        while (*p && isdigit((unsigned char)*p) && k < 24) spec[k++] = *p++;
        if (*p == '.') { spec[k++] = *p++; while (*p && isdigit((unsigned char)*p) && k < 28) spec[k++] = *p++; }
        while (*p == 'l' || *p == 'h') p++;
        char conv = *p;
        if (!conv) break;
        char tmp[512];
        if (conv == '%') { PUT('%'); continue; }
        uint32_t v = rd32(g_mem, args + 4 * ai++);
        spec[k++] = conv == 'i' ? 'd' : conv;
        spec[k] = 0;
        if (conv == 's') snprintf(tmp, sizeof tmp, spec, v ? win_str(v) : "(null)");
        else if (conv == 'c') snprintf(tmp, sizeof tmp, spec, (int)(v & 0xff));
        else if (conv == 'd' || conv == 'i') snprintf(tmp, sizeof tmp, spec, (int32_t)v);
        else snprintf(tmp, sizeof tmp, spec, v);
        for (char *q = tmp; *q; q++) PUT(*q);
    }
    if (room) out[n < room ? n : room - 1] = 0;
#undef PUT
    return (int)n;
}
RT_IMPORT(wsprintfA)
{
    char buf[1025];
    int n = format_win(buf, sizeof buf, win_str(rt_arg(1)), cpu.esp + 12);
    win_put_str(rt_arg(0), buf, 1025);
    rt_ret_cdecl((uint32_t)(n > 1024 ? 1024 : n));
}
RT_IMPORT(wvsprintfA)
{
    char buf[1025];
    int n = format_win(buf, sizeof buf, win_str(rt_arg(1)), rt_arg(2));
    win_put_str(rt_arg(0), buf, 1025);
    rt_ret_stdcall((uint32_t)(n > 1024 ? 1024 : n), 3);
}
