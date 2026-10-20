/* Recursos de las imágenes cargadas, leídos de su .rsrc en la memoria plana: FindResourceA/LoadResource/
 * LockResource/SizeofResource y LoadStringA. Vale para el exe, sus DLL y MFC42 (que trae sus cadenas y diálogos). */
#include "win.h"
#include "loader.h"

#include <ctype.h>

/* Un nombre o tipo de recurso: un número (< 0x10000, o "#123") o una cadena. */
static int id_of(uint32_t p, uint32_t *num, char *name, size_t room)
{
    if (p < 0x10000) { *num = p; return 1; }
    const char *s = win_str(p);
    if (s[0] == '#') { *num = (uint32_t)strtoul(s + 1, NULL, 10); return 1; }
    snprintf(name, room, "%s", s);
    return 0;
}

static uint32_t rsrc_base(uint32_t module)
{
    uint32_t pe = module + rd32(g_mem, module + 0x3c);
    uint32_t rva = rd32(g_mem, pe + 0x88);              /* directorio 2: recursos */
    return rva ? module + rva : 0;
}

/* Busca una entrada en un directorio de recursos; devuelve la dirección del subdirectorio/dato, o 0. */
static uint32_t find_entry(uint32_t root, uint32_t dir, int by_num, uint32_t num, const char *name, uint32_t *out_flag)
{
    uint32_t named = rd16(g_mem, dir + 12), ids = rd16(g_mem, dir + 14);
    for (uint32_t i = 0; i < named + ids; i++) {
        uint32_t e = dir + 16 + 8 * i, key = rd32(g_mem, e), val = rd32(g_mem, e + 4);
        int match = 0;
        if (key & 0x80000000u) {
            if (!by_num) {
                uint32_t s = root + (key & 0x7fffffffu), n = rd16(g_mem, s);
                if (n == strlen(name)) {
                    match = 1;
                    for (uint32_t k = 0; k < n; k++)
                        if (toupper(rd16(g_mem, s + 2 + 2 * k) & 0xff) != toupper((unsigned char)name[k])) { match = 0; break; }
                }
            }
        } else if (by_num && key == num) match = 1;
        if (match) { *out_flag = val & 0x80000000u; return root + (val & 0x7fffffffu); }
    }
    return 0;
}

static uint32_t first_entry(uint32_t root, uint32_t dir, uint32_t *flag)
{
    if (rd16(g_mem, dir + 12) + rd16(g_mem, dir + 14) == 0) return 0;
    uint32_t val = rd32(g_mem, dir + 16 + 4);
    *flag = val & 0x80000000u;
    return root + (val & 0x7fffffffu);
}

/* Devuelve la dirección del IMAGE_RESOURCE_DATA_ENTRY (el "HRSRC") o 0. */
static uint32_t find_resource(uint32_t module, uint32_t name, uint32_t type)
{
    if (!module) module = 0x400000u;
    uint32_t root = rsrc_base(module);
    if (!root) return 0;
    uint32_t tn, nn, flag = 0;
    char ts[128] = "", ns[128] = "";
    int tb = id_of(type, &tn, ts, sizeof ts), nb = id_of(name, &nn, ns, sizeof ns);
    uint32_t d = find_entry(root, root, tb, tn, ts, &flag);
    if (!d || !flag) return 0;
    d = find_entry(root, d, nb, nn, ns, &flag);
    if (!d || !flag) return 0;
    d = first_entry(root, d, &flag);                     /* el primer idioma */
    return d && !flag ? d : 0;
}

/* Las imágenes en orden: primero la pedida y, si no lo tiene, como hace AfxFindResourceHandle, las demás. */
RT_IMPORT(FindResourceA)
{
    uint32_t r = find_resource(rt_arg(0), rt_arg(1), rt_arg(2));
    if (!r) win_last_error = 1814;                      /* ERROR_RESOURCE_NAME_NOT_FOUND */
    rt_ret_stdcall(r, 3);
}

static uint32_t module_of(uint32_t addr)
{
    for (uint32_t i = 0; i < rt_image_count; i++)
        if (addr >= rt_images[i].base && addr < rt_images[i].base + rt_images[i].size) return rt_images[i].base;
    return 0x400000u;
}

RT_IMPORT(LoadResource)
{
    uint32_t r = rt_arg(1);
    rt_ret_stdcall(r ? module_of(r) + rd32(g_mem, r) : 0, 2);
}
RT_IMPORT(LockResource) { rt_ret_stdcall(rt_arg(0), 1); }
RT_IMPORT(FreeResource) { rt_ret_stdcall(0, 1); }
RT_IMPORT(SizeofResource) { rt_ret_stdcall(rt_arg(1) ? rd32(g_mem, rt_arg(1) + 4) : 0, 2); }

/* LoadStringA(módulo, id, buffer, tamaño): las cadenas van de a 16 por bloque (RT_STRING = 6), contadas, UTF-16. */
RT_IMPORT(LoadStringA)
{
    uint32_t module = rt_arg(0), id = rt_arg(1), buf = rt_arg(2), room = rt_arg(3);
    uint32_t r = find_resource(module, id / 16 + 1, 6);
    uint32_t n = 0;
    if (r && room) {
        uint32_t p = module_of(r) + rd32(g_mem, r);
        for (uint32_t i = 0; i < id % 16; i++) p += 2 + 2 * rd16(g_mem, p);
        uint32_t len = rd16(g_mem, p);
        for (; n < len && n + 1 < room; n++) wr8(g_mem, buf + n, rd16(g_mem, p + 2 + 2 * n) & 0xff);
    }
    if (room) wr8(g_mem, buf + n, 0);
    rt_ret_stdcall(n, 4);
}
