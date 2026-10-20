/* Carga del exe y sus DLL en la memoria plana. Las bases no se pisan (build.py lo verifica), así que no se reubica nada. */
#include "loader.h"

#include "core/host.h"
#include "core/rt.h"
#include "core/rt_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u32_at(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16_at(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void load_one(const char *dir, const RtImage *im)
{
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, im->file);
    FILE *f = host_fopen(path, "rb");
    if (!f) rt_fatal("no se puede abrir %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) rt_fatal("no se puede leer %s", path);
    fclose(f);
    uint32_t pe = u32_at(data + 0x3c);
    uint16_t count = u16_at(data + pe + 6), optional = u16_at(data + pe + 20);
    uint32_t base = u32_at(data + pe + 24 + 28);
    if (base != im->base) rt_fatal("%s: base %#x, el build esperaba %#x", path, base, im->base);
    uint32_t headers = u32_at(data + pe + 24 + 60);
    memcpy(g_mem + base, data, headers < (uint32_t)n ? headers : (uint32_t)n);   /* cabeceras: GetModuleHandle, recursos */
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *s = data + pe + 24 + optional + i * 40;
        uint32_t vsize = u32_at(s + 8), va = u32_at(s + 12), rsize = u32_at(s + 16), rptr = u32_at(s + 20);
        uint32_t len = vsize && vsize < rsize ? vsize : rsize;
        memcpy(g_mem + base + va, data + rptr, len);
    }
    free(data);
}

/* TLS implícito (__declspec(thread)): el directorio TLS del PE (directorio 9) da la plantilla [inicio, fin), el relleno
 * en cero y dónde escribir el índice del módulo. Cada hilo tiene un arreglo de punteros (uno por módulo con TLS) en
 * fs:[0x2c] (ThreadLocalStoragePointer); el juego lee mov eax, fs:[0x2c]; mov ecx, [eax + _tls_index*4]. */
#define MAX_TLS_MODULES 8
static struct { uint32_t start, end, zero; } tls_mod[MAX_TLS_MODULES];
static uint32_t tls_count;

static void tls_scan(void)
{
    for (uint32_t i = 0; i < rt_image_count; i++) {
        uint32_t base = rt_images[i].base, pe = base + rd32(g_mem, base + 0x3c);
        uint32_t rva = rd32(g_mem, pe + 0x78 + 8 * 9);
        if (!rva || tls_count == MAX_TLS_MODULES) continue;
        uint32_t d = base + rva;
        tls_mod[tls_count].start = rd32(g_mem, d);
        tls_mod[tls_count].end = rd32(g_mem, d + 4);
        tls_mod[tls_count].zero = rd32(g_mem, d + 16);
        uint32_t index_addr = rd32(g_mem, d + 8);
        if (index_addr) wr32(g_mem, index_addr, tls_count);
        fprintf(stderr, "recomp: TLS de %s: plantilla %#x-%#x, cero %u, índice %u\n", rt_images[i].file,
                tls_mod[tls_count].start, tls_mod[tls_count].end, tls_mod[tls_count].zero, tls_count);
        tls_count++;
    }
}

/* Arma los bloques TLS de un hilo nuevo y devuelve el arreglo (lo que va en fs:[0x2c]). */
uint32_t rt_tls_new_thread(void)
{
    if (!tls_count) return 0;
    uint32_t array = rt_alloc_zero(4 * MAX_TLS_MODULES);
    for (uint32_t m = 0; m < tls_count; m++) {
        uint32_t n = tls_mod[m].end - tls_mod[m].start;
        uint32_t block = rt_alloc_zero(n + tls_mod[m].zero + 16);
        memcpy(g_mem + block, g_mem + tls_mod[m].start, n);
        wr32(g_mem, array + 4 * m, block);
    }
    return array;
}

void rt_load_images(const char *dir)
{
    size_t span = (size_t)1 << 32;
    void *m = host_reserve(span + 0x10000);
    if (!m) rt_fatal("no se pueden reservar 4 GB de espacio de direcciones");
    g_mem = m;
    for (uint32_t i = 0; i < rt_image_count; i++) load_one(dir, &rt_images[i]);
    for (uint32_t i = 0; i < rt_iat_size; i++) wr32(g_mem, rt_iat[i].iat, rt_iat[i].stub);
    tls_scan();
    fpu_init();
    fpu.cw = 0x27f;                        /* lo que Windows le da a un proceso nuevo */
}
