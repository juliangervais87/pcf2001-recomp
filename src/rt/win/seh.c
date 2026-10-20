/* Structured exceptions over the fs:[0] chain in flat memory: RaiseException (the CRT's C++ throw) and
 * RtlUnwind (longjmp and the C++ frame handler). Each record of the chain is {next, handler}; the
 * chain ends with 0xffffffff. A handler is cdecl: handler(record, frame, context, dispatcher context)
 * and returns 0 (continue execution) or 1 (continue search).
 *
 * A handler that finds a catch block jumps to the continuation inside an older frame. The runtime runs
 * that as a resume entry and unwinds the host frames (rt_unwinding); then this code returns at once. */
#include "win.h"

#define EXCEPTION_NONCONTINUABLE 1u
#define EXCEPTION_UNWINDING 2u
#define EXCEPTION_EXIT_UNWIND 4u
#define STATUS_UNWIND 0xC0000027u
#define CHAIN_END 0xffffffffu

#define REC_SIZE 0x50u
#define CTX_SIZE 0x2ccu

static uint32_t chain_head(void) { return rd32(g_mem, RT_FS_BASE); }
static void set_chain_head(uint32_t r) { wr32(g_mem, RT_FS_BASE, r); }

/* A CONTEXT with the integer registers as they are at the call. */
static void fill_context(uint32_t ctx, uint32_t eip, uint32_t esp)
{
    memset(g_mem + ctx, 0, CTX_SIZE);
    wr32(g_mem, ctx, 0x10007u);
    wr32(g_mem, ctx + 0x9c, cpu.edi);
    wr32(g_mem, ctx + 0xa0, cpu.esi);
    wr32(g_mem, ctx + 0xa4, cpu.ebx);
    wr32(g_mem, ctx + 0xa8, cpu.edx);
    wr32(g_mem, ctx + 0xac, cpu.ecx);
    wr32(g_mem, ctx + 0xb0, cpu.eax);
    wr32(g_mem, ctx + 0xb4, cpu.ebp);
    wr32(g_mem, ctx + 0xb8, eip);
    wr32(g_mem, ctx + 0xc4, esp);
}

/* RtlUnwind(target frame, target ip, record, return value): calls every handler above the target frame
 * with the unwinding flag and takes its record off the chain. Then it returns to the caller, which is
 * what jumping to the target ip does for every caller in the original (_global_unwind2 passes the
 * address after its own call). */
RT_IMPORT(RtlUnwind)
{
    uint32_t target = rt_arg(0), record = rt_arg(2), value = rt_arg(3);
    uint32_t ret = rd32(g_mem, cpu.esp);
    Cpu saved = cpu;
    cpu.esp -= 0x1000;
    uint32_t ctx = cpu.esp, disp = ctx + CTX_SIZE;
    fill_context(ctx, ret, saved.esp + 20);
    if (!record) {
        record = disp + 16;
        memset(g_mem + record, 0, REC_SIZE);
        wr32(g_mem, record, STATUS_UNWIND);
        wr32(g_mem, record + 12, ret);
    }
    wr32(g_mem, record + 4, rd32(g_mem, record + 4) | EXCEPTION_UNWINDING | (target ? 0 : EXCEPTION_EXIT_UNWIND));
    uint32_t r = chain_head();
    while (r != CHAIN_END && r != target) {
        if (target && r > target) rt_fatal("RtlUnwind: frame %#x is not on the chain", target);
        uint32_t args[4] = {record, r, ctx, disp};
        rt_callback(rd32(g_mem, r + 4), 4, args);
        if (rt_unwinding) return;
        r = rd32(g_mem, r);
        set_chain_head(r);
    }
    cpu = saved;
    rt_ret_stdcall(value, 4);
}

/* RaiseException(code, flags, count, arguments): offers the exception to each handler of the chain. */
RT_IMPORT(RaiseException)
{
    uint32_t code = rt_arg(0), flags = rt_arg(1), count = rt_arg(2), params = rt_arg(3);
    uint32_t ret = rd32(g_mem, cpu.esp);
    Cpu saved = cpu;
    cpu.esp -= 0x1000;
    uint32_t ctx = cpu.esp, disp = ctx + CTX_SIZE, record = disp + 16;
    fill_context(ctx, ret, saved.esp + 20);
    memset(g_mem + record, 0, REC_SIZE);
    wr32(g_mem, record, code);
    wr32(g_mem, record + 4, flags & EXCEPTION_NONCONTINUABLE);
    wr32(g_mem, record + 12, ret);
    if (count > 15) count = 15;
    wr32(g_mem, record + 16, count);
    for (uint32_t i = 0; i < count; i++) wr32(g_mem, record + 20 + 4 * i, rd32(g_mem, params + 4 * i));
    WIN_TRACE("RaiseException(%#x) desde %#x; cadena:", code, ret);
    for (uint32_t r = chain_head(), n = 0; r != CHAIN_END && n < 16; r = rd32(g_mem, r), n++) WIN_TRACE(" %#x(%#x)", r, rd32(g_mem, r + 4));
    WIN_TRACE("\n");
    for (uint32_t r = chain_head(); r != CHAIN_END; r = rd32(g_mem, r)) {
        uint32_t args[4] = {record, r, ctx, disp};
        uint32_t result = rt_callback(rd32(g_mem, r + 4), 4, args);
        if (rt_unwinding) return;
        if (result == 0) {                       /* continue execution */
            if (flags & EXCEPTION_NONCONTINUABLE) rt_fatal("exception %#x: a handler continued a noncontinuable exception", code);
            cpu = saved;
            rt_ret_stdcall(0, 4);
            return;
        }
    }
    rt_fatal("unhandled exception %#x raised from %#x%s", code, ret, code == 0xE06D7363u ? " (a C++ throw)" : "");
}

/* The CRT's last-chance filter: say what happened and stop. */
RT_IMPORT(UnhandledExceptionFilter)
{
    uint32_t pointers = rt_arg(0);
    uint32_t record = rd32(g_mem, pointers);
    rt_fatal("unhandled exception %#x at %#x", rd32(g_mem, record), rd32(g_mem, record + 12));
}

RT_IMPORT(SetUnhandledExceptionFilter) { rt_ret_stdcall(0, 1); }
