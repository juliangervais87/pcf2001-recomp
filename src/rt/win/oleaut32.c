/* OLEAUT32 (por número de orden, como lo importan el exe y MFC): cadenas BSTR y VARIANT básicos. Un BSTR apunta a los
 * caracteres de 16 bits; los 4 bytes de adelante tienen el largo en bytes y termina en un 0 de 16 bits. Va en el heap
 * del juego, como SysAllocString en Windows (que lo pide al de OLE). */
#include "com.h"

static uint32_t bstr_new(uint32_t src, uint32_t bytes)
{
    uint32_t p = rt_alloc(bytes + 6);
    wr32(g_mem, p, bytes);
    if (src) memcpy(g_mem + p + 4, g_mem + src, bytes);
    else memset(g_mem + p + 4, 0, bytes);
    g_mem[p + 4 + bytes] = 0;
    g_mem[p + 5 + bytes] = 0;
    return p + 4;
}
static uint32_t wlen(uint32_t s) { uint32_t n = 0; while (rd16(g_mem, s + 2 * n)) n++; return n; }
/* Un BSTR desde texto de 8 bits (Latin-1): para lo que el runtime devuelve como BSTR (webbrowser.c). */
uint32_t oleaut32_bstr_from(const char *s)
{
    uint32_t n = (uint32_t)strlen(s), b = bstr_new(0, 2 * n);
    for (uint32_t i = 0; i < n; i++) wr16(g_mem, b + 2 * i, (uint8_t)s[i]);
    return b;
}

RT_IMPORT(OLEAUT32_ord2) { uint32_t s = rt_arg(0); rt_ret_stdcall(s ? bstr_new(s, 2 * wlen(s)) : 0, 1); }       /* SysAllocString */
RT_IMPORT(OLEAUT32_ord4) { rt_ret_stdcall(bstr_new(rt_arg(0), 2 * rt_arg(1)), 2); }                            /* SysAllocStringLen */
RT_IMPORT(OLEAUT32_ord6) { if (rt_arg(0)) rt_free(rt_arg(0) - 4); rt_ret_stdcall(0, 1); }                       /* SysFreeString */
RT_IMPORT(OLEAUT32_ord7) { uint32_t b = rt_arg(0); rt_ret_stdcall(b ? rd32(g_mem, b - 4) / 2 : 0, 1); }         /* SysStringLen */
RT_IMPORT(OLEAUT32_ord149) { uint32_t b = rt_arg(0); rt_ret_stdcall(b ? rd32(g_mem, b - 4) : 0, 1); }           /* SysStringByteLen */
RT_IMPORT(OLEAUT32_ord150) { rt_ret_stdcall(bstr_new(rt_arg(0), rt_arg(1)), 2); }                              /* SysAllocStringByteLen */
/* SysReAllocString(BSTR*, s) y SysReAllocStringLen(BSTR*, s, n) */
RT_IMPORT(OLEAUT32_ord3)
{
    uint32_t pp = rt_arg(0), s = rt_arg(1), old = rd32(g_mem, pp);
    wr32(g_mem, pp, s ? bstr_new(s, 2 * wlen(s)) : 0);
    if (old) rt_free(old - 4);
    rt_ret_stdcall(1, 2);
}
RT_IMPORT(OLEAUT32_ord5)
{
    uint32_t pp = rt_arg(0), old = rd32(g_mem, pp);
    wr32(g_mem, pp, bstr_new(rt_arg(1), 2 * rt_arg(2)));
    if (old) rt_free(old - 4);
    rt_ret_stdcall(1, 3);
}

/* VARIANT (16 bytes, el tipo en los 2 primeros): sólo hace falta liberar el BSTR (VT_BSTR = 8). */
RT_IMPORT(OLEAUT32_ord8) { memset(g_mem + rt_arg(0), 0, 2); rt_ret_stdcall(0, 1); }                             /* VariantInit */
static void variant_clear(uint32_t v)
{
    if (rd16(g_mem, v) == 8 && rd32(g_mem, v + 8)) rt_free(rd32(g_mem, v + 8) - 4);
    memset(g_mem + v, 0, 16);
}
RT_IMPORT(OLEAUT32_ord9) { variant_clear(rt_arg(0)); rt_ret_stdcall(0, 1); }                                  /* VariantClear */
RT_IMPORT(OLEAUT32_ord10)                                                                                      /* VariantCopy */
{
    uint32_t d = rt_arg(0), s = rt_arg(1);
    if (d != s) {
        variant_clear(d);
        memcpy(g_mem + d, g_mem + s, 16);
        uint32_t b = rd32(g_mem, s + 8);
        if (rd16(g_mem, s) == 8 && b) wr32(g_mem, d + 8, bstr_new(b, rd32(g_mem, b - 4)));
    }
    rt_ret_stdcall(0, 2);
}

/* GetProcAddress(oleaut32, #n): estas mismas, para las que ninguna imagen importa (MFC las pide al lanzar una
 * COleException). */
uint32_t oleaut32_by_ordinal(uint32_t n)
{
    static const struct { uint32_t n; RtFn fn; const char *name; } t[] = {
        {2, imp_OLEAUT32_ord2, "OLEAUT32_ord2"}, {3, imp_OLEAUT32_ord3, "OLEAUT32_ord3"},
        {4, imp_OLEAUT32_ord4, "OLEAUT32_ord4"}, {5, imp_OLEAUT32_ord5, "OLEAUT32_ord5"},
        {6, imp_OLEAUT32_ord6, "OLEAUT32_ord6"}, {7, imp_OLEAUT32_ord7, "OLEAUT32_ord7"},
        {8, imp_OLEAUT32_ord8, "OLEAUT32_ord8"}, {9, imp_OLEAUT32_ord9, "OLEAUT32_ord9"},
        {10, imp_OLEAUT32_ord10, "OLEAUT32_ord10"}, {149, imp_OLEAUT32_ord149, "OLEAUT32_ord149"},
        {150, imp_OLEAUT32_ord150, "OLEAUT32_ord150"}};
    for (unsigned i = 0; i < sizeof t / sizeof *t; i++)
        if (t[i].n == n) return rt_host_fn(t[i].name, t[i].fn);
    return 0;
}
