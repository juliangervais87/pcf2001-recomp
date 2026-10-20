/* The code page imports that the CRT start uses to build its character tables (_setmbcp and the
 * ctype setup): code page 1252, with characters 0x00..0xFF taken as Latin-1. */
#include "win.h"

#include <ctype.h>

/* CT_CTYPE1 flags of one Latin-1 character. */
static uint32_t ctype1(uint32_t c)
{
    enum { UPPER = 1, LOWER = 2, DIGIT = 4, SPACE = 8, PUNCT = 0x10, CNTRL = 0x20, BLANK = 0x40, XDIGIT = 0x80, ALPHA = 0x100 };
    if (c < 0x80) {
        uint32_t f = 0;
        if (isupper((int)c)) f |= UPPER | ALPHA;
        if (islower((int)c)) f |= LOWER | ALPHA;
        if (isdigit((int)c)) f |= DIGIT;
        if (isspace((int)c)) f |= SPACE;
        if (ispunct((int)c)) f |= PUNCT;
        if (iscntrl((int)c)) f |= CNTRL;
        if (c == ' ' || c == '\t') f |= BLANK;
        if (isxdigit((int)c)) f |= XDIGIT;
        return f;
    }
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return UPPER | ALPHA;
    if (c >= 0xDF && c != 0xF7) return LOWER | ALPHA;
    if (c == 0xA0) return SPACE | BLANK;
    if (c < 0xA0) return c == 0x8A || c == 0x8C || c == 0x8E || c == 0x9F ? UPPER | ALPHA
                       : c == 0x9A || c == 0x9C || c == 0x9E || c == 0x83 ? LOWER | ALPHA : PUNCT;
    return PUNCT;
}

static uint32_t to_upper(uint32_t c)
{
    if (c < 0x80) return (uint32_t)toupper((int)c);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
    return c;
}

static uint32_t to_lower(uint32_t c)
{
    if (c < 0x80) return (uint32_t)tolower((int)c);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    return c;
}

static uint32_t length_a(uint32_t s, int32_t n) { return n >= 0 ? (uint32_t)n : (uint32_t)strlen(win_str(s)) + 1; }
static uint32_t length_w(uint32_t s, int32_t n)
{
    if (n >= 0) return (uint32_t)n;
    uint32_t k = 0;
    while (rd16(g_mem, s + 2 * k)) k++;
    return k + 1;
}

/* GetStringTypeW(type, source, count, types) */
RT_IMPORT(GetStringTypeW)
{
    uint32_t type = rt_arg(0), src = rt_arg(1), out = rt_arg(3);
    uint32_t n = length_w(src, (int32_t)rt_arg(2));
    if (type != 1) { win_last_error = 87; rt_ret_stdcall(0, 4); return; }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = rd16(g_mem, src + 2 * i);
        wr16(g_mem, out + 2 * i, c < 0x100 ? ctype1(c) : 0x100);
    }
    rt_ret_stdcall(1, 4);
}

/* GetStringTypeA(locale, type, source, count, types) */
RT_IMPORT(GetStringTypeA)
{
    uint32_t type = rt_arg(1), src = rt_arg(2), out = rt_arg(4);
    uint32_t n = length_a(src, (int32_t)rt_arg(3));
    if (type != 1) { win_last_error = 87; rt_ret_stdcall(0, 5); return; }
    for (uint32_t i = 0; i < n; i++) wr16(g_mem, out + 2 * i, ctype1(rd8(g_mem, src + i)));
    rt_ret_stdcall(1, 5);
}

/* MultiByteToWideChar(code page, flags, source, count, target, room) */
RT_IMPORT(MultiByteToWideChar)
{
    uint32_t src = rt_arg(2), dst = rt_arg(4), room = rt_arg(5);
    uint32_t n = length_a(src, (int32_t)rt_arg(3));
    if (room) {
        if (room < n) { win_last_error = 122; rt_ret_stdcall(0, 6); return; }
        for (uint32_t i = 0; i < n; i++) wr16(g_mem, dst + 2 * i, rd8(g_mem, src + i));
    }
    rt_ret_stdcall(n, 6);
}

/* WideCharToMultiByte(code page, flags, source, count, target, room, default, used default) */
RT_IMPORT(WideCharToMultiByte)
{
    uint32_t src = rt_arg(2), dst = rt_arg(4), room = rt_arg(5), used = rt_arg(7);
    uint32_t n = length_w(src, (int32_t)rt_arg(3));
    if (used) wr32(g_mem, used, 0);
    if (room) {
        if (room < n) { win_last_error = 122; rt_ret_stdcall(0, 8); return; }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t c = rd16(g_mem, src + 2 * i);
            if (c >= 0x100) { c = '?'; if (used) wr32(g_mem, used, 1); }
            wr8(g_mem, dst + i, c);
        }
    }
    rt_ret_stdcall(n, 8);
}

#define LCMAP_LOWERCASE 0x100u
#define LCMAP_UPPERCASE 0x200u

static uint32_t map_char(uint32_t flags, uint32_t c)
{
    if (flags & LCMAP_UPPERCASE) return to_upper(c);
    if (flags & LCMAP_LOWERCASE) return to_lower(c);
    return c;
}

/* LCMapStringA(locale, flags, source, count, target, room) */
RT_IMPORT(LCMapStringA)
{
    uint32_t flags = rt_arg(1), src = rt_arg(2), dst = rt_arg(4), room = rt_arg(5);
    uint32_t n = length_a(src, (int32_t)rt_arg(3));
    if (flags & ~(LCMAP_LOWERCASE | LCMAP_UPPERCASE)) rt_fatal("LCMapStringA: flags %#x", flags);
    if (room) {
        if (room < n) { win_last_error = 122; rt_ret_stdcall(0, 6); return; }
        for (uint32_t i = 0; i < n; i++) wr8(g_mem, dst + i, map_char(flags, rd8(g_mem, src + i)));
    }
    rt_ret_stdcall(n, 6);
}

/* LCMapStringW(locale, flags, source, count, target, room) */
RT_IMPORT(LCMapStringW)
{
    uint32_t flags = rt_arg(1), src = rt_arg(2), dst = rt_arg(4), room = rt_arg(5);
    uint32_t n = length_w(src, (int32_t)rt_arg(3));
    if (flags & ~(LCMAP_LOWERCASE | LCMAP_UPPERCASE)) rt_fatal("LCMapStringW: flags %#x", flags);
    if (room) {
        if (room < n) { win_last_error = 122; rt_ret_stdcall(0, 6); return; }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t c = rd16(g_mem, src + 2 * i);
            wr16(g_mem, dst + 2 * i, c < 0x100 ? map_char(flags, c) : c);
        }
    }
    rt_ret_stdcall(n, 6);
}
