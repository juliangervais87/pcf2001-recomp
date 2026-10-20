/* Objetos COM en la memoria plana (DirectDraw, DirectSound): el objeto del juego es {vtable, id}; la vtable son
 * direcciones de funciones del host (rt_host_fn). Un método lee `this` con rt_arg(0) y vuelve con
 * rt_ret_stdcall(hresult, n + 1). */
#ifndef PCF2001_RECOMP_COM_H
#define PCF2001_RECOMP_COM_H

#include "win.h"

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define E_FAIL 0x80004005u

typedef struct { const char *name; RtFn fn; } ComMethod;

/* Vtable en memoria plana a partir de una lista de métodos (los NULL paran con el nombre de la interfaz). */
uint32_t com_vtable(const char *iface, const ComMethod *methods, int count);
/* Objeto nuevo: vtable + un puntero del host que se recupera con com_ptr. */
uint32_t com_new(uint32_t vtable, void *host, uint32_t extra_bytes);
void *com_ptr(uint32_t obj);
/* Libera un objeto creado con extra_bytes 0: su lugar se reusa. */
void com_free(uint32_t obj);
/* GUID del juego en memoria == este texto "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx". */
int com_guid_is(uint32_t p, const char *text);
void com_guid_text(uint32_t p, char *out);

#endif
