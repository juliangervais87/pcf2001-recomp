/* The heap inside the flat memory, from RT_HEAP_BASE up.
 *
 * Two modes:
 * - bump (rt_heap_bump = 1): never reuses memory and rounds each block to 16 bytes. The database load
 *   test (tests/recomp) uses it, so that the addresses match the Unicorn run byte for byte.
 * - free list (the default): each block has a 16-byte header before it with the asked size and the
 *   room. Small blocks (up to SMALL_MAX bytes of room) go back to a list per room size. Large blocks go
 *   back to a list sorted by address and join their free neighbours. */
#include "rt.h"
#include "rt_host.h"

#include <stdlib.h>
#include <string.h>

int rt_heap_bump = 0;
static uint32_t heap_top;                    /* 0 hasta el primer uso: el inicio sale del perfil del juego */

uint32_t rt_heap_top(void) { if (!heap_top) heap_top = RT_HEAP_BASE; return heap_top; }

/* Bump mode: block sizes for realloc, an open hash from address to size. */
static uint32_t *size_keys, *size_vals;
static uint32_t size_cap, size_count;

static void size_put(uint32_t addr, uint32_t size)
{
    if (size_count * 2 >= size_cap) {
        uint32_t old_cap = size_cap, *ok = size_keys, *ov = size_vals;
        size_cap = size_cap ? size_cap * 2 : 1024;
        size_keys = calloc(size_cap, 4);
        size_vals = calloc(size_cap, 4);
        size_count = 0;
        for (uint32_t i = 0; i < old_cap; i++)
            if (ok[i]) size_put(ok[i], ov[i]);
        free(ok);
        free(ov);
    }
    uint32_t i = (addr >> 4) * 2654435761u & (size_cap - 1);
    while (size_keys[i] && size_keys[i] != addr) i = (i + 1) & (size_cap - 1);
    if (!size_keys[i]) size_count++;
    size_keys[i] = addr;
    size_vals[i] = size;
}

static uint32_t size_get(uint32_t addr)
{
    if (!size_cap) return 0;
    uint32_t i = (addr >> 4) * 2654435761u & (size_cap - 1);
    while (size_keys[i]) {
        if (size_keys[i] == addr) return size_vals[i];
        i = (i + 1) & (size_cap - 1);
    }
    return 0;
}

/* Free list mode. Header at block - 16: asked size, room, state. */
#define HDR 16u
#define MAGIC_USED 0x55534544u
#define MAGIC_FREE 0x46524545u
#define SMALL_MAX 4096u
#define SMALL_CLASSES (SMALL_MAX / 16 + 1)

static uint32_t small_free[SMALL_CLASSES];     /* first free block of each room; next is in its first word */

typedef struct { uint32_t addr, room; } Span;
static Span *large;                             /* free large blocks, sorted by address */
static uint32_t large_count, large_cap;

static uint32_t h_size(uint32_t a) { return rd32(g_mem, a - HDR); }
static uint32_t h_room(uint32_t a) { return rd32(g_mem, a - HDR + 4); }
static uint32_t h_state(uint32_t a) { return rd32(g_mem, a - HDR + 8); }
static void h_set(uint32_t a, uint32_t size, uint32_t room, uint32_t state)
{
    wr32(g_mem, a - HDR, size);
    wr32(g_mem, a - HDR + 4, room);
    wr32(g_mem, a - HDR + 8, state);
    wr32(g_mem, a - HDR + 12, 0);
}

static uint32_t from_top(uint32_t room)
{
    if (!heap_top) heap_top = RT_HEAP_BASE;
    uint32_t a = heap_top + HDR;
    if ((uint64_t)heap_top + HDR + room > (uint64_t)RT_HEAP_BASE + RT_HEAP_SIZE) rt_fatal("heap full");
    heap_top += HDR + room;
    return a;
}

static void large_remove(uint32_t i)
{
    memmove(large + i, large + i + 1, (large_count - i - 1) * sizeof *large);
    large_count--;
}

static void large_insert(uint32_t addr, uint32_t room)
{
    uint32_t lo = 0, hi = large_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (large[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    /* Join with the free block after it and the free block before it. */
    if (lo < large_count && addr + room + HDR == large[lo].addr) {
        room += HDR + large[lo].room;
        large_remove(lo);
    }
    if (lo > 0 && large[lo - 1].addr + large[lo - 1].room + HDR == addr) {
        lo--;
        addr = large[lo].addr;
        room += HDR + large[lo].room;
        large_remove(lo);
    }
    if (addr + room == heap_top) {              /* the last block: give it back to the top */
        heap_top = addr - HDR;
        return;
    }
    if (large_count == large_cap) {
        large_cap = large_cap ? large_cap * 2 : 256;
        large = realloc(large, large_cap * sizeof *large);
    }
    memmove(large + lo + 1, large + lo, (large_count - lo) * sizeof *large);
    large[lo].addr = addr;
    large[lo].room = room;
    large_count++;
    h_set(addr, 0, room, MAGIC_FREE);
}

/* RECOMP_HEAP_CERO=1 (el perfil "heap_cero"): todo bloque sale en cero, también los reusados. Sirve para comparar
 * partidas grabadas contra el original en Windows: hay malloc que el juego graba sin escribir enteros, y en Windows
 * llegan en cero, mientras que acá un bloque reusado trae restos. La memoria nueva ya llega en cero sin esto. */
static int heap_cero(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMP_HEAP_CERO"); v = e && *e == '1'; }
    return v;
}

static uint32_t list_alloc(uint32_t size)
{
    uint32_t room = ((size ? size : 1) + 15) & ~15u;
    uint32_t a;
    if (room <= SMALL_MAX) {
        uint32_t c = room / 16;
        a = small_free[c];
        if (a) small_free[c] = rd32(g_mem, a);
        else a = from_top(room);
    } else {
        uint32_t best = UINT32_MAX;
        for (uint32_t i = 0; i < large_count; i++)
            if (large[i].room >= room && (best == UINT32_MAX || large[i].room < large[best].room)) best = i;
        if (best == UINT32_MAX) {
            a = from_top(room);
        } else {
            a = large[best].addr;
            uint32_t have = large[best].room;
            large_remove(best);
            if (have - room > SMALL_MAX + HDR) {          /* split: the rest stays free */
                large_insert(a + room + HDR, have - room - HDR);
            } else {
                room = have;
            }
        }
    }
    h_set(a, size, room, MAGIC_USED);
    if (heap_cero()) memset(g_mem + a, 0, room);
    return a;
}

static void list_free(uint32_t a)
{
    if (a < RT_HEAP_BASE + HDR || a >= heap_top || h_state(a) != MAGIC_USED) {
        rt_fatal("free of %#x, which is not a heap block (from %#x)", a, rd32(g_mem, cpu.esp));
    }
    uint32_t room = h_room(a);
    if (room <= SMALL_MAX) {
        h_set(a, 0, room, MAGIC_FREE);
        wr32(g_mem, a, small_free[room / 16]);
        small_free[room / 16] = a;
    } else {
        large_insert(a, room);
    }
}

uint32_t rt_alloc(uint32_t size)
{
    if (!rt_heap_bump) return list_alloc(size);
    if (!heap_top) heap_top = RT_HEAP_BASE;
    uint32_t addr = heap_top;
    heap_top += ((size ? size : 1) + 15) & ~15u;
    if (heap_top > RT_HEAP_BASE + RT_HEAP_SIZE) rt_fatal("heap full");
    size_put(addr, size);
    return addr;
}

uint32_t rt_alloc_zero(uint32_t size)
{
    uint32_t a = rt_alloc(size);
    memset(g_mem + a, 0, size);
    return a;
}

void rt_free(uint32_t addr)
{
    if (!addr || rt_heap_bump) return;
    list_free(addr);
}

uint32_t rt_block_size(uint32_t addr)
{
    if (rt_heap_bump) return size_get(addr);
    return h_size(addr);
}

void rt_set_block_size(uint32_t addr, uint32_t size)
{
    if (rt_heap_bump) size_put(addr, size);
    else wr32(g_mem, addr - HDR, size);
}

int rt_is_block(uint32_t addr)
{
    if (rt_heap_bump) return size_get(addr) || addr == heap_top;
    return addr >= RT_HEAP_BASE + HDR && addr < heap_top && h_state(addr) == MAGIC_USED;
}

uint32_t rt_realloc(uint32_t addr, uint32_t size, int in_place_only)
{
    if (!addr) return in_place_only ? 0 : rt_alloc(size);
    uint32_t old = rt_block_size(addr);
    if (rt_heap_bump ? size <= old : size <= h_room(addr)) {
        rt_set_block_size(addr, size);
        return addr;
    }
    if (in_place_only) return 0;
    uint32_t n = rt_alloc(size);
    memcpy(g_mem + n, g_mem + addr, old < size ? old : size);
    rt_free(addr);
    return n;
}

uint32_t rt_alloc_str(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t a = rt_alloc(n);
    memcpy(g_mem + a, s, n);
    return a;
}
