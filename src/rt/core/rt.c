/* The flat memory, the PE load, calls through addresses, host functions at fake addresses, the x87
 * state save and the stops. The heap is in heap.c. */
#include "rt.h"
#include "rt_host.h"
#include "host.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

Cpu cpu;
Fpu fpu;
uint8_t *g_mem;

static uint32_t image_base, image_size;
const uint8_t *rt_exe_data;
uint32_t rt_exe_size;

uint32_t rt_image_base(void) { return image_base; }
uint32_t rt_image_size(void) { return image_size; }

static uint32_t u32_at(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16_at(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

void rt_init(const char *exe_path)
{
    /* 4 GB and a page, so that an access at the top of the 32-bit space stays inside. */
    size_t span = (size_t)1 << 32;
    void *m = host_reserve(span + 0x10000);
    if (!m) rt_fatal("cannot reserve 4 GB of address space");
    g_mem = m;

    FILE *f = host_fopen(exe_path, "rb");
    if (!f) rt_fatal("cannot open %s", exe_path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    if (fread(data, 1, (size_t)n, f) != (size_t)n) rt_fatal("cannot read %s", exe_path);
    fclose(f);
    uint32_t pe = u32_at(data + 0x3c);
    uint16_t count = u16_at(data + pe + 6), optional = u16_at(data + pe + 20);
    image_base = u32_at(data + pe + 24 + 28);
    image_size = u32_at(data + pe + 24 + 56);
    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *s = data + pe + 24 + optional + i * 40;
        uint32_t vsize = u32_at(s + 8), va = u32_at(s + 12), rsize = u32_at(s + 16), rptr = u32_at(s + 20);
        uint32_t len = vsize && vsize < rsize ? vsize : rsize;
        memcpy(g_mem + image_base + va, data + rptr, len);
    }
    rt_exe_data = data;                      /* kept for the resources (cursors, icon) */
    rt_exe_size = (uint32_t)n;
    for (uint32_t i = 0; i < rt_iat_size; i++) wr32(g_mem, rt_iat[i].iat, rt_iat[i].stub);
    fpu_init();
    fpu.cw = 0x27f;                        /* what Windows gives a new process */
}

void fpu_init(void)
{
    memset(&fpu, 0, sizeof fpu);
    fpu.cw = 0x37f;
}

/* 80-bit extended to double and back. */
double rdf80(const uint8_t *m, uint32_t a)
{
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, m + a, 8);
    memcpy(&se, m + a + 8, 2);
    int sign = se >> 15, exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void wrf80(uint8_t *m, uint32_t a, double v)
{
    uint64_t mant = 0;
    uint16_t se = signbit(v) ? 0x8000 : 0;
    double x = fabs(v);
    if (x == 0.0) {
    } else if (isinf(x)) {
        se |= 0x7fff; mant = 1ull << 63;
    } else if (isnan(x)) {
        se |= 0x7fff; mant = 3ull << 62;
    } else {
        int e;
        double fr = frexp(x, &e);                 /* x = fr * 2^e, 0.5 <= fr < 1 */
        mant = (uint64_t)ldexp(fr, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(m + a, &mant, 8);
    memcpy(m + a + 8, &se, 2);
}

/* fnsave and frstor. The 108-byte image: control, status and tag words at 0, 4 and 8, then the eight
 * registers ST(0)..ST(7) as 80-bit values from offset 28. fnsave then resets the x87. */
void rt_fnsave(uint8_t *m, uint32_t a)
{
    memset(m + a, 0, 108);
    wr32(m, a, fpu.cw);
    wr32(m, a + 4, fstatus());
    wr32(m, a + 8, 0);
    for (int i = 0; i < 8; i++) wrf80(m, a + 28 + 10 * (uint32_t)i, ST(i));
    fpu_init();
}

void rt_frstor(uint8_t *m, uint32_t a)
{
    uint32_t sw = rd32(m, a + 4);
    fpu.cw = (uint16_t)rd32(m, a);
    fpu.top = (sw >> 11) & 7;
    fpu.sw = (uint16_t)(sw & ~0x3800u);
    for (int i = 0; i < 8; i++) ST(i) = rdf80(m, a + 28 + 10 * (uint32_t)i);
}

/* Host functions at fake addresses: COM methods and the results of GetProcAddress. */
#define MAX_HOST_FNS 1024
static struct { RtFn fn; const char *name; } host_fns[MAX_HOST_FNS];
static uint32_t host_fn_count;

uint32_t rt_host_fn(const char *name, RtFn fn)
{
    for (uint32_t i = 0; i < host_fn_count; i++)
        if (host_fns[i].fn == fn && host_fns[i].name == name) return RT_HOST_FN_BASE + 4 * i;
    if (host_fn_count == MAX_HOST_FNS) rt_fatal("too many host functions");
    host_fns[host_fn_count].fn = fn;
    host_fns[host_fn_count].name = name;
    return RT_HOST_FN_BASE + 4 * host_fn_count++;
}

static int is_host_fn(uint32_t target)
{
    return target >= RT_HOST_FN_BASE && target < RT_HOST_FN_BASE + 4 * host_fn_count && !(target & 3);
}

/* Calls through an address. */
static const RtEntry *find_entry(uint32_t target)
{
    uint32_t lo = 0, hi = rt_table_size;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rt_table[mid].addr < target) lo = mid + 1;
        else hi = mid;
    }
    return lo < rt_table_size && rt_table[lo].addr == target ? &rt_table[lo] : NULL;
}

RtFn rt_lookup(uint32_t target)
{
    if (is_host_fn(target)) return host_fns[(target - RT_HOST_FN_BASE) / 4].fn;
    const RtEntry *e = find_entry(target);
    return e ? e->fn : NULL;
}

const char *rt_name_of(uint32_t target)
{
    if (is_host_fn(target)) return host_fns[(target - RT_HOST_FN_BASE) / 4].name;
    const RtEntry *e = find_entry(target);
    return e ? e->name : "?";
}

int rt_unwinding;
uint32_t rt_call_target;

void rt_call(uint32_t target)
{
    rt_call_target = target;
    RtFn fn = rt_lookup(target);
    if (!fn) rt_fatal("call to %#x, which is not a recompiled function (from %#x) eax=%#x ebx=%#x ecx=%#x edx=%#x esi=%#x edi=%#x ebp=%#x esp=%#x [ebx]=%#x [ecx]=%#x", target, rd32(g_mem, cpu.esp), cpu.eax, cpu.ebx, cpu.ecx, cpu.edx, cpu.esi, cpu.edi, cpu.ebp, cpu.esp, rd32(g_mem, cpu.ebx), rd32(g_mem, cpu.ecx));
    fn();
}

void rt_jump(uint32_t target)
{
    if (is_host_fn(target)) { host_fns[(target - RT_HOST_FN_BASE) / 4].fn(); return; }
    const RtEntry *e = find_entry(target);
    if (!e) rt_fatal("jump to %#x, which is not a recompiled entry", target);
    e->fn();
    /* A resume entry ran the rest of an older frame: unwind the host frames above it. */
    if (e->flags & RT_RESUME) rt_unwinding = 1;
}

/* Calls a function of the original from the host with 32-bit arguments and returns eax. The callee may
 * be cdecl or stdcall: esp and the other registers come back as they were. If the call unwinds past the
 * host (rt_unwinding), cpu stays as the unwinding left it. */
uint32_t rt_callback(uint32_t target, int nargs, const uint32_t *args)
{
    Cpu saved = cpu;
    for (int i = nargs - 1; i >= 0; i--) {
        cpu.esp -= 4;
        wr32(g_mem, cpu.esp, args[i]);
    }
    cpu.esp -= 4;
    wr32(g_mem, cpu.esp, RT_RETURN_ADDR);
    rt_call(target);
    if (rt_unwinding) return cpu.eax;
    uint32_t eax = cpu.eax;
    cpu = saved;
    cpu.eax = eax;
    return eax;
}

void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "recomp: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    if (rt_on_fatal) rt_on_fatal();
    exit(70);
}

void (*rt_on_fatal)(void);

uint32_t rt_screen_w = 800, rt_screen_h = 600;
uint32_t rt_season_year = 2001;       /* the start year: win/season.c sets it */
uint32_t rt_staff_reserve = 23000;    /* the blank staff rows of a database load: win/staff_reserve.c */

void rt_unimpl(uint32_t at, const char *text) { rt_fatal("instruction at %#x is not translated: %s", at, text); }
void rt_fell_off(uint32_t at) { rt_fatal("execution reached %#x, where the recompiler found no code", at); }
void rt_bad_jump(uint32_t at, uint32_t target) { rt_fatal("jump table at %#x: target %#x is not in the table", at, target); }
void rt_missing_import(const char *name) { rt_fatal("import %s has no replacement (called from %#x)", name, rd32(g_mem, cpu.esp)); }
void rt_missing_method(const char *iface, uint32_t offset)
{
    rt_fatal("%s method at vtable offset %#x has no replacement (called from %#x)", iface, offset, rd32(g_mem, cpu.esp));
}
void rt_divide_error(uint32_t at) { rt_fatal("divide error at %#x", at); }
