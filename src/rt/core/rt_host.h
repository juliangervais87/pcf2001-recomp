/* Host side of the runtime: setup, the heap, host functions and the tables that the recompiler writes. */
#ifndef RECOMP_RT_HOST_H
#define RECOMP_RT_HOST_H

#include <stdint.h>

#include "rt.h"

/* The same layout as the Unicorn run of tests/recomp/reference.py. */
#define RT_STACK_BASE 0x20000000u
#define RT_STACK_SIZE 0x100000u
#define RT_STACK_TOP (RT_STACK_BASE + RT_STACK_SIZE - 0x1000u)
#define RT_RETURN_ADDR 0x40000000u
/* El heap no empieza en 0x70000000, que pisaría MSVCRT (0x78000000). El inicio sale del perfil del juego
 * (gen/images.c: 0x01000000 de base, más arriba si el exe y sus DLL no entran debajo); llega hasta 0x10000000:
 * punteros debajo de 2 GB, como en Win9x. */
extern const uint32_t rt_heap_base;
#define RT_HEAP_BASE rt_heap_base
#define RT_HEAP_SIZE (0x10000000u - rt_heap_base)
/* Host functions (COM methods, GetProcAddress results) get addresses from here up; see rt_host_fn. */
#define RT_HOST_FN_BASE 0x63000000u

typedef struct { uint32_t iat, stub; } RtIat;
extern const RtIat rt_iat[];
extern const uint32_t rt_iat_size;

void rt_init(const char *exe_path);
uint32_t rt_image_base(void);
uint32_t rt_image_size(void);
extern const uint8_t *rt_exe_data;      /* the whole exe file */
extern uint32_t rt_exe_size;

/* The heap (heap.c). With rt_heap_bump set it never reuses memory and rounds blocks to 16 bytes, as the
 * Unicorn run of the load test does. Otherwise freed blocks are reused. */
extern int rt_heap_bump;
uint32_t rt_alloc(uint32_t size);
uint32_t rt_alloc_zero(uint32_t size);
uint32_t rt_alloc_str(const char *s);
void rt_free(uint32_t addr);
uint32_t rt_realloc(uint32_t addr, uint32_t size, int in_place_only);
uint32_t rt_block_size(uint32_t addr);
void rt_set_block_size(uint32_t addr, uint32_t size);
int rt_is_block(uint32_t addr);
uint32_t rt_heap_top(void);

/* A host function at a fake address that the original can call (stdcall: it pops its own arguments).
 * The same function under another name gets another address; rt_call_target tells them apart. */
extern uint32_t rt_call_target;
uint32_t rt_host_fn(const char *name, RtFn fn);
const char *rt_name_of(uint32_t target);
/* Calls the original's function at target with nargs 32-bit arguments; returns eax. */
uint32_t rt_callback(uint32_t target, int nargs, const uint32_t *args);
void rt_missing_method(const char *iface, uint32_t offset) __attribute__((noreturn));
/* Runs before a fatal stop exits, for example to save the screen. */
extern void (*rt_on_fatal)(void);

/* How often each replacement ran (tests). */
void rt_count(const char *name);
void rt_print_counts(void);

#endif
