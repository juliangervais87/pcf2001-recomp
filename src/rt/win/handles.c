/* Tabla de handles de los objetos del runtime. Los valores son múltiplos de 4 desde 0x100, como los de Windows,
 * y nunca chocan con los handles especiales (GetCurrentProcess = -1, GetCurrentThread = -2, stdio). */
#include "win.h"

#define MAX_HANDLES 4096
#define HANDLE_BASE 0x100u

static WinHandle table[MAX_HANDLES];

uint32_t win_handle_new(int kind, void *ptr)
{
    for (uint32_t i = 1; i < MAX_HANDLES; i++) {
        if (table[i].kind == WH_FREE) {
            table[i].kind = kind;
            table[i].ptr = ptr;
            table[i].a = table[i].b = 0;
            return HANDLE_BASE + 4 * i;
        }
    }
    rt_fatal("se acabaron los handles");
}

WinHandle *win_handle_get(uint32_t h, int kind)
{
    if (h < HANDLE_BASE || (h & 3)) return NULL;
    uint32_t i = (h - HANDLE_BASE) / 4;
    if (i == 0 || i >= MAX_HANDLES || table[i].kind == WH_FREE) return NULL;
    if (kind != WH_FREE && table[i].kind != kind) return NULL;
    return &table[i];
}

void win_handle_free(uint32_t h)
{
    WinHandle *w = win_handle_get(h, WH_FREE);
    if (w) memset(w, 0, sizeof *w);
}
