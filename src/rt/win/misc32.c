/* Lo chico de KERNEL32/USER32/GDI32 que MFC42 y MSVCRT piden al arrancar o de pasada: códigos de página, versión del
 * proceso, propiedades de ventana, mensajes registrados, semáforos, menús y caret inertes, transformaciones de GDI
 * identidad. Lo que no aparece acá y el juego pide, para con su nombre (stubs.c). */
#include "user32.h"
#include "screen.h"

/* Códigos de página: 1252, de un byte. */
RT_IMPORT(GetACP) { rt_ret_stdcall(1252, 0); }
RT_IMPORT(GetOEMCP) { rt_ret_stdcall(850, 0); }
RT_IMPORT(GetCPInfo)
{
    uint32_t info = rt_arg(1);
    memset(g_mem + info, 0, 20);
    wr32(g_mem, info, 1);
    wr8(g_mem, info + 4, '?');
    rt_ret_stdcall(1, 2);
}
RT_IMPORT(GetStringTypeExA)
{
    /* GetStringTypeExA(lcid, tipo, s, n, out) == GetStringTypeA(lcid, tipo, s, n, out) en Win9x: lo resuelve locale.c */
    extern void imp_GetStringTypeA(void);
    imp_GetStringTypeA();
}
RT_IMPORT(CharToOemA) { if (rt_arg(0) != rt_arg(1)) strcpy(win_str(rt_arg(1)), win_str(rt_arg(0))); rt_ret_stdcall(1, 2); }
RT_IMPORT(OemToCharA) { if (rt_arg(0) != rt_arg(1)) strcpy(win_str(rt_arg(1)), win_str(rt_arg(0))); rt_ret_stdcall(1, 2); }
/* Las de largo fijo (PC Fútbol 2001), con el mismo criterio que las de arriba: se copia tal cual. */
RT_IMPORT(CharToOemBuffA) { if (rt_arg(0) != rt_arg(1)) memmove(g_mem + rt_arg(1), g_mem + rt_arg(0), rt_arg(2)); rt_ret_stdcall(1, 3); }
RT_IMPORT(OemToCharBuffA) { if (rt_arg(0) != rt_arg(1)) memmove(g_mem + rt_arg(1), g_mem + rt_arg(0), rt_arg(2)); rt_ret_stdcall(1, 3); }
RT_IMPORT(EnumSystemLocalesA) { rt_ret_stdcall(1, 2); }

/* Proceso y módulos. */
RT_IMPORT(DisableThreadLibraryCalls) { rt_ret_stdcall(1, 1); }
RT_IMPORT(GetProcessVersion) { rt_ret_stdcall(0x00040000u, 1); }           /* subsistema 4.0 */
RT_IMPORT(GetExitCodeProcess) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 0); rt_ret_stdcall(1, 2); }
RT_IMPORT(GetShortPathNameA) { win_put_str(rt_arg(1), win_str(rt_arg(0)), rt_arg(2)); rt_ret_stdcall((uint32_t)strlen(win_str(rt_arg(1))), 3); }
RT_IMPORT(SearchPathA) { win_last_error = ERROR_FILE_NOT_FOUND; rt_ret_stdcall(0, 6); }
RT_IMPORT(SetLocalTime) { rt_ret_stdcall(1, 1); }
RT_IMPORT(Beep) { rt_ret_stdcall(1, 2); }
RT_IMPORT(HeapValidate) { rt_ret_stdcall(1, 3); }
RT_IMPORT(HeapCompact) { rt_ret_stdcall(0x100000, 2); }
RT_IMPORT(HeapWalk) { win_last_error = ERROR_NO_MORE_FILES; rt_ret_stdcall(0, 2); }
RT_IMPORT(CreateSemaphoreA) { win_last_error = 0; rt_ret_stdcall(win_handle_new(WH_OTHER, NULL), 4); }
RT_IMPORT(ReleaseSemaphore) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), 0); rt_ret_stdcall(1, 3); }
RT_IMPORT(GlobalGetAtomNameA) { win_put_str(rt_arg(1), "", rt_arg(2)); rt_ret_stdcall(0, 3); }
RT_IMPORT(SetConsoleCtrlHandler) { rt_ret_stdcall(1, 2); }
RT_IMPORT(GetConsoleMode) { rt_ret_stdcall(0, 2); }

/* Mensajes registrados: un número por nombre, desde 0xC000 (como Windows). */
#define MAX_REG 128
static char reg_names[MAX_REG][64];
static int nreg;
RT_IMPORT(RegisterWindowMessageA)
{
    const char *n = win_str(rt_arg(0));
    for (int i = 0; i < nreg; i++) if (!strcmp(reg_names[i], n)) { rt_ret_stdcall(0xC000u + (uint32_t)i, 1); return; }
    if (nreg == MAX_REG) { rt_ret_stdcall(0, 1); return; }
    snprintf(reg_names[nreg], 64, "%s", n);
    rt_ret_stdcall(0xC000u + (uint32_t)nreg++, 1);
}
RT_IMPORT(RegisterClipboardFormatA) { static uint32_t f = 0xC100; rt_ret_stdcall(f++, 1); }
RT_IMPORT(IsClipboardFormatAvailable) { rt_ret_stdcall(0, 1); }
RT_IMPORT(CountClipboardFormats) { rt_ret_stdcall(0, 0); }

/* Propiedades de ventana (MFC las usa para el subclaseo de diálogos). */
#define MAX_PROPS 256
static struct { uint32_t hwnd; char name[64]; uint32_t value; } props[MAX_PROPS];
static void prop_key(uint32_t p, char *out) { if (p < 0x10000) snprintf(out, 64, "#%u", p); else snprintf(out, 64, "%s", win_str(p)); }
RT_IMPORT(SetPropA)
{
    char k[64];
    prop_key(rt_arg(1), k);
    int free_slot = -1;
    for (int i = 0; i < MAX_PROPS; i++) {
        if (props[i].hwnd == rt_arg(0) && !strcmp(props[i].name, k)) { props[i].value = rt_arg(2); rt_ret_stdcall(1, 3); return; }
        if (!props[i].hwnd && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) { rt_ret_stdcall(0, 3); return; }
    props[free_slot].hwnd = rt_arg(0);
    snprintf(props[free_slot].name, 64, "%s", k);
    props[free_slot].value = rt_arg(2);
    rt_ret_stdcall(1, 3);
}
RT_IMPORT(GetPropA)
{
    char k[64];
    prop_key(rt_arg(1), k);
    for (int i = 0; i < MAX_PROPS; i++) if (props[i].hwnd == rt_arg(0) && !strcmp(props[i].name, k)) { rt_ret_stdcall(props[i].value, 2); return; }
    rt_ret_stdcall(0, 2);
}
RT_IMPORT(RemovePropA)
{
    char k[64];
    prop_key(rt_arg(1), k);
    for (int i = 0; i < MAX_PROPS; i++)
        if (props[i].hwnd == rt_arg(0) && !strcmp(props[i].name, k)) { uint32_t v = props[i].value; props[i].hwnd = 0; rt_ret_stdcall(v, 2); return; }
    rt_ret_stdcall(0, 2);
}

/* Ventanas: lo que no cambia nada visible. */
RT_IMPORT(FindWindowA) { rt_ret_stdcall(0, 2); }
RT_IMPORT(GetWindowThreadProcessId) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 0x1234); rt_ret_stdcall(1, 2); }
RT_IMPORT(GetWindowPlacement)
{
    uint32_t p = rt_arg(1);
    memset(g_mem + p, 0, 44);
    wr32(g_mem, p, 44);
    wr32(g_mem, p + 8, 1);                                     /* SW_SHOWNORMAL */
    wr32(g_mem, p + 36, screen.w);
    wr32(g_mem, p + 40, screen.h);
    rt_ret_stdcall(1, 2);
}
RT_IMPORT(SetWindowPlacement) { rt_ret_stdcall(1, 2); }
RT_IMPORT(RedrawWindow) { Wnd *w = wnd_get(rt_arg(0)); if (w) w->dirty = 1; rt_ret_stdcall(1, 4); }
RT_IMPORT(LockWindowUpdate) { rt_ret_stdcall(1, 1); }
RT_IMPORT(ShowOwnedPopups) { rt_ret_stdcall(1, 2); }
RT_IMPORT(EnumChildWindows) { rt_ret_stdcall(1, 3); }
RT_IMPORT(ScrollWindow) { rt_ret_stdcall(1, 5); }
RT_IMPORT(ScrollWindowEx) { rt_ret_stdcall(1, 8); }
RT_IMPORT(GetUpdateRgn) { rt_ret_stdcall(1, 3); }               /* NULLREGION */
RT_IMPORT(ShowCaret) { rt_ret_stdcall(1, 1); }
RT_IMPORT(HideCaret) { rt_ret_stdcall(1, 1); }
RT_IMPORT(GetDialogBaseUnits) { rt_ret_stdcall((13u << 16) | 6u, 0); }
RT_IMPORT(MapDialogRect) { rt_ret_stdcall(1, 2); }
RT_IMPORT(BeginDeferWindowPos) { rt_ret_stdcall(0x5000, 1); }
RT_IMPORT(DeferWindowPos) { extern void imp_SetWindowPos(void); uint32_t h = rt_arg(0); for (int i = 0; i < 7; i++) wr32(g_mem, cpu.esp + 4 + 4u * (uint32_t)i, rt_arg(i + 1)); (void)h; imp_SetWindowPos(); cpu.esp += 4; cpu.eax = 0x5000; }
RT_IMPORT(EndDeferWindowPos) { rt_ret_stdcall(1, 1); }
RT_IMPORT(GetScrollPos) { rt_ret_stdcall(0, 2); }
RT_IMPORT(SetScrollPos) { rt_ret_stdcall(0, 4); }
RT_IMPORT(GetScrollRange) { wr32(g_mem, rt_arg(2), 0); wr32(g_mem, rt_arg(3), 0); rt_ret_stdcall(1, 4); }
RT_IMPORT(SetScrollRange) { rt_ret_stdcall(1, 5); }
RT_IMPORT(ShowScrollBar) { rt_ret_stdcall(1, 3); }
RT_IMPORT(GetScrollInfo) { rt_ret_stdcall(0, 3); }
RT_IMPORT(SetScrollInfo) { rt_ret_stdcall(0, 4); }

/* Menús: el juego no tiene barra de menú; todo es inerte. */
RT_IMPORT(CreateMenu) { rt_ret_stdcall(0x6000, 0); }
RT_IMPORT(DestroyMenu) { rt_ret_stdcall(1, 1); }
RT_IMPORT(LoadMenuA) { rt_ret_stdcall(0, 2); }
RT_IMPORT(GetSubMenu) { rt_ret_stdcall(0, 2); }
RT_IMPORT(GetMenuItemCount) { rt_ret_stdcall(0, 1); }
RT_IMPORT(GetMenuItemID) { rt_ret_stdcall(0xffffffffu, 2); }
RT_IMPORT(GetMenuState) { rt_ret_stdcall(0xffffffffu, 3); }
RT_IMPORT(GetMenuStringA) { if (rt_arg(2) && rt_arg(3)) wr8(g_mem, rt_arg(2), 0); rt_ret_stdcall(0, 5); }
RT_IMPORT(EnableMenuItem) { rt_ret_stdcall(0xffffffffu, 3); }
RT_IMPORT(CheckMenuItem) { rt_ret_stdcall(0xffffffffu, 3); }
RT_IMPORT(AppendMenuA) { rt_ret_stdcall(1, 4); }
RT_IMPORT(InsertMenuA) { rt_ret_stdcall(1, 5); }
RT_IMPORT(ModifyMenuA) { rt_ret_stdcall(1, 5); }
RT_IMPORT(DeleteMenu) { rt_ret_stdcall(1, 3); }
RT_IMPORT(RemoveMenu) { rt_ret_stdcall(1, 3); }
RT_IMPORT(SetMenuItemBitmaps) { rt_ret_stdcall(1, 5); }
RT_IMPORT(GetMenuCheckMarkDimensions) { rt_ret_stdcall((13u << 16) | 13u, 0); }
RT_IMPORT(TrackPopupMenu) { rt_ret_stdcall(0, 7); }
RT_IMPORT(LoadAcceleratorsA) { rt_ret_stdcall(0, 2); }
RT_IMPORT(TranslateMDISysAccel) { rt_ret_stdcall(0, 2); }
RT_IMPORT(LoadBitmapA) { rt_ret_stdcall(0, 2); }

/* GDI: estado y transformaciones identidad (MM_TEXT). */
RT_IMPORT(GetBkMode) { rt_ret_stdcall(2, 1); }
RT_IMPORT(GetTextAlign) { rt_ret_stdcall(0, 1); }
RT_IMPORT(GetROP2) { rt_ret_stdcall(13, 1); }
RT_IMPORT(SetROP2) { rt_ret_stdcall(13, 2); }
RT_IMPORT(GetStretchBltMode) { rt_ret_stdcall(1, 1); }
RT_IMPORT(GetPolyFillMode) { rt_ret_stdcall(1, 1); }
RT_IMPORT(SetPolyFillMode) { rt_ret_stdcall(1, 2); }
RT_IMPORT(GetObjectType) { uint32_t h = rt_arg(0); rt_ret_stdcall(h >= 0x00050000u && h < 0x00060000u ? 3u : 1u, 1); }   /* OBJ_DC / OBJ_PEN */
RT_IMPORT(DPtoLP) { rt_ret_stdcall(1, 3); }
RT_IMPORT(LPtoDP) { rt_ret_stdcall(1, 3); }
RT_IMPORT(GetViewportExtEx) { wr32(g_mem, rt_arg(1), 1); wr32(g_mem, rt_arg(1) + 4, 1); rt_ret_stdcall(1, 2); }
RT_IMPORT(GetWindowExtEx) { wr32(g_mem, rt_arg(1), 1); wr32(g_mem, rt_arg(1) + 4, 1); rt_ret_stdcall(1, 2); }
RT_IMPORT(GetWindowOrgEx) { wr32(g_mem, rt_arg(1), 0); wr32(g_mem, rt_arg(1) + 4, 0); rt_ret_stdcall(1, 2); }
RT_IMPORT(OffsetWindowOrgEx) { rt_ret_stdcall(1, 4); }
RT_IMPORT(SetBrushOrgEx) { rt_ret_stdcall(1, 4); }
RT_IMPORT(GetCurrentPositionEx) { wr32(g_mem, rt_arg(1), 0); wr32(g_mem, rt_arg(1) + 4, 0); rt_ret_stdcall(1, 2); }
RT_IMPORT(SetRectRgn) { rt_ret_stdcall(1, 5); }
RT_IMPORT(OffsetRgn) { rt_ret_stdcall(2, 3); }
RT_IMPORT(OffsetClipRgn) { rt_ret_stdcall(2, 3); }
RT_IMPORT(GetClipRgn) { rt_ret_stdcall(0, 2); }
RT_IMPORT(ExtSelectClipRgn) { rt_ret_stdcall(2, 3); }
RT_IMPORT(SetMapperFlags) { rt_ret_stdcall(0, 2); }
RT_IMPORT(SetColorAdjustment) { rt_ret_stdcall(1, 2); }
RT_IMPORT(SetArcDirection) { rt_ret_stdcall(1, 2); }
RT_IMPORT(EnumFontFamiliesA) { rt_ret_stdcall(1, 4); }
RT_IMPORT(InvertRect) { rt_ret_stdcall(1, 2); }
RT_IMPORT(DrawFocusRect) { rt_ret_stdcall(1, 2); }
RT_IMPORT(DrawEdge) { rt_ret_stdcall(1, 4); }
RT_IMPORT(GrayStringA) { rt_ret_stdcall(1, 9); }
RT_IMPORT(TabbedTextOutA) { rt_ret_stdcall(0, 8); }
RT_IMPORT(GetTabbedTextExtentA) { rt_ret_stdcall(0, 5); }
