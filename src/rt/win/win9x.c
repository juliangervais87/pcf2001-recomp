/* Lo que Windows 9x NO tiene: las variantes Unicode (W) de KERNEL32/USER32 fallan con ERROR_CALL_NOT_IMPLEMENTED, y
 * MSVCRT/MFC caen a las variantes A. Es lo que el juego conoció en 1997-98 (kernel32.c: GetVersion de Windows 98).
 * Las W que Windows 9x sí implementa (MultiByteToWideChar, WideCharToMultiByte, GetStringTypeW, LCMapStringW) están en
 * locale.c. */
#include "win.h"

#define ERROR_CALL_NOT_IMPLEMENTED 120u
#define NO_W(name, nargs) RT_IMPORT(name) { win_last_error = ERROR_CALL_NOT_IMPLEMENTED; rt_ret_stdcall(0, nargs); }

NO_W(GetCommandLineW, 0)
NO_W(GetModuleFileNameW, 3)
NO_W(GetModuleHandleW, 1)
NO_W(LoadLibraryW, 1)
NO_W(CreateFileW, 7)
NO_W(GetEnvironmentVariableW, 3)
NO_W(SetEnvironmentVariableW, 2)
NO_W(CompareStringW, 6)
NO_W(GetLocaleInfoW, 4)
NO_W(GetCurrentDirectoryW, 2)
NO_W(GetFullPathNameW, 4)
NO_W(GetFileAttributesW, 1)
NO_W(FindFirstFileW, 2)
NO_W(OutputDebugStringW, 1)
NO_W(GetStartupInfoW, 1)
NO_W(lstrlenW, 1)
NO_W(MessageBoxW, 4)
NO_W(LoadStringW, 4)
NO_W(GetWindowTextW, 3)
NO_W(SetWindowTextW, 2)
NO_W(SendMessageW, 4)
NO_W(DefWindowProcW, 4)
NO_W(CallWindowProcW, 5)
NO_W(RegisterClassW, 1)
NO_W(CreateWindowExW, 12)
NO_W(GetClassInfoW, 3)
NO_W(GetTextExtentPoint32W, 4)
NO_W(TextOutW, 5)
NO_W(ExtTextOutW, 8)

/* Locale A: español de Argentina (LCID 0x2C0A), código de página 1252. */
#include <ctype.h>
RT_IMPORT(GetUserDefaultLCID) { rt_ret_stdcall(0x2C0Au, 0); }
RT_IMPORT(GetSystemDefaultLCID) { rt_ret_stdcall(0x2C0Au, 0); }
RT_IMPORT(GetThreadLocale) { rt_ret_stdcall(0x2C0Au, 0); }
RT_IMPORT(GetUserDefaultLangID) { rt_ret_stdcall(0x2C0Au, 0); }
RT_IMPORT(GetSystemDefaultLangID) { rt_ret_stdcall(0x2C0Au, 0); }
RT_IMPORT(IsDBCSLeadByte) { rt_ret_stdcall(0, 1); }
RT_IMPORT(IsValidCodePage) { rt_ret_stdcall(rt_arg(0) == 1252 || rt_arg(0) == 850 || rt_arg(0) == 437, 1); }
RT_IMPORT(IsValidLocale) { rt_ret_stdcall(1, 2); }
RT_IMPORT(GetKeyboardLayout) { rt_ret_stdcall(0x2C0A2C0Au, 1); }
RT_IMPORT(GetLocaleInfoA)
{
    uint32_t type = rt_arg(1) & 0xffff, buf = rt_arg(2), room = rt_arg(3);
    const char *v = "";
    char tmp[16];
    switch (type) {
    case 0x1004: v = "1252"; break;            /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x000B: v = "850"; break;             /* LOCALE_IDEFAULTCODEPAGE */
    case 0x000E: v = ","; break;               /* LOCALE_SDECIMAL */
    case 0x000F: v = "."; break;               /* LOCALE_STHOUSAND */
    case 0x0014: v = "$"; break;               /* LOCALE_SCURRENCY */
    case 0x001D: v = "/"; break;               /* LOCALE_SDATE */
    case 0x001E: v = ":"; break;               /* LOCALE_STIME */
    case 0x001F: v = "dd/MM/yy"; break;        /* LOCALE_SSHORTDATE */
    case 0x0001: snprintf(tmp, sizeof tmp, "%04x", 0x2C0Au); v = tmp; break;
    case 0x0002: v = "Español (Argentina)"; break;
    case 0x0007: v = "ESS"; break;
    case 0x005A: v = "es"; break;
    case 0x005B: v = "AR"; break;
    default: v = ""; break;
    }
    uint32_t n = (uint32_t)strlen(v) + 1;
    if (room) win_put_str(buf, v, room);
    rt_ret_stdcall(room && room < n ? 0 : n, 4);
}
/* CompareStringA(lcid, flags, s1, n1, s2, n2): NORM_IGNORECASE 1. Devuelve 1 (<), 2 (=), 3 (>). */
RT_IMPORT(CompareStringA)
{
    uint32_t flags = rt_arg(1), a = rt_arg(2), b = rt_arg(4);
    int32_t na = (int32_t)rt_arg(3), nb = (int32_t)rt_arg(5);
    size_t la = na < 0 ? strlen(win_str(a)) : (size_t)na, lb = nb < 0 ? strlen(win_str(b)) : (size_t)nb;
    int r = 0;
    for (size_t i = 0; i < la && i < lb && !r; i++) {
        int x = (unsigned char)g_mem[a + i], y = (unsigned char)g_mem[b + i];
        if (flags & 1u) { x = toupper(x); y = toupper(y); }
        r = (x > y) - (x < y);
    }
    if (!r) r = (la > lb) - (la < lb);
    rt_ret_stdcall((uint32_t)(r + 2), 6);
}

/* Las W que pide PC Fútbol 2001 (MSVCRT/MFC prueban la W y caen a la A, como en Windows 98). */
NO_W(CreateDirectoryW, 2)
NO_W(CreateProcessW, 10)
NO_W(DeleteFileW, 1)
NO_W(FindNextFileW, 2)
NO_W(GetDriveTypeW, 1)
NO_W(MoveFileW, 2)
NO_W(RemoveDirectoryW, 1)
NO_W(SetCurrentDirectoryW, 1)
NO_W(SetFileAttributesW, 2)
