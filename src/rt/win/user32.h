/* Lo que user32.c comparte con gdi32.c, ddraw.c e input.c. */
#ifndef PCF2001_RECOMP_USER32_H
#define PCF2001_RECOMP_USER32_H

#include "win.h"

typedef struct {
    int used;
    uint32_t cls;                 /* índice de clase */
    uint32_t proc, style, exstyle, parent, owner, id, hinst, userdata;
    int32_t x, y, w, h;           /* rectángulo de la ventana: pantalla (top-level) o cliente del padre (hija) */
    int visible, enabled, dirty;
    char text[256];
    uint8_t extra[128];
} Wnd;

Wnd *wnd_get(uint32_t hwnd);
uint32_t wnd_main(void);                       /* la ventana principal (la primera top-level visible) */
void wnd_screen_origin(uint32_t hwnd, int32_t *x, int32_t *y);   /* origen del área cliente en la pantalla */

/* Cola de mensajes. */
void msg_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t msg_send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

/* Entrada (input.c). */
extern int32_t mouse_x, mouse_y;
extern uint8_t key_down[256];
extern uint32_t capture_hwnd, focus_hwnd;
void input_mouse(int x, int y, uint32_t msg, uint32_t wp);
void input_key(uint32_t vk, int down);

#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_ENABLE 0x000Au
#define WM_SETTEXT 0x000Cu
#define WM_GETTEXT 0x000Du
#define WM_GETTEXTLENGTH 0x000Eu
#define WM_PAINT 0x000Fu
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_ERASEBKGND 0x0014u
#define WM_SHOWWINDOW 0x0018u
#define WM_ACTIVATEAPP 0x001Cu
#define WM_SETCURSOR 0x0020u
#define WM_MOUSEACTIVATE 0x0021u
#define WM_GETMINMAXINFO 0x0024u
#define WM_WINDOWPOSCHANGING 0x0046u
#define WM_WINDOWPOSCHANGED 0x0047u
#define WM_NCCREATE 0x0081u
#define WM_NCDESTROY 0x0082u
#define WM_NCCALCSIZE 0x0083u
#define WM_NCHITTEST 0x0084u
#define WM_NCACTIVATE 0x0086u
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP 0x0101u
#define WM_CHAR 0x0102u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSCOMMAND 0x0112u
#define WM_TIMER 0x0113u
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP 0x0202u
#define WM_LBUTTONDBLCLK 0x0203u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP 0x0205u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP 0x0208u

#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u

#endif
