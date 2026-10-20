/* Instrucciones que el traductor delega al runtime (tools/recomp/translator.py las emite): cpuid y las x87
 * de la biblioteca matemática de MSVCRT. Semántica según el manual de Intel (SDM vol. 2). */
#include "core/rt.h"

#include <math.h>
#include <stdlib.h>

#define C0 0x0100u
#define C1 0x0200u
#define C2 0x0400u
#define C3 0x4000u

/* Un Pentium MMX (familia 5, modelo 4): una máquina de la época del juego. Sin SSE. RECOMP_CPUID_1 (el perfil
 * "cpuid_1_eax") cambia la firma de la hoja 1. Un juego puede elegir opciones según la clase de CPU; la detección
 * llega a CPUID porque popfd/pushfd guardan el bit ID (translator.py). */
static uint32_t cpuid_1(void)
{
    static uint32_t v;
    if (!v) { const char *e = getenv("RECOMP_CPUID_1"); v = e && *e ? (uint32_t)strtoul(e, 0, 0) : 0x543u; }
    return v;
}

void rt_cpuid(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    if (*eax == 0) {
        *eax = 1;
        *ebx = 0x756e6547u;    /* "Genu" */
        *edx = 0x49656e69u;    /* "ineI" */
        *ecx = 0x6c65746eu;    /* "ntel" */
    } else {
        *eax = cpuid_1();
        *ebx = 0;
        *ecx = 0;
        *edx = 0x008001bfu;    /* fpu vme de pse tsc msr mce cx8 mmx */
    }
}

static void set_cc(uint16_t bits, uint16_t mask) { fpu.sw = (uint16_t)((fpu.sw & ~mask) | bits); }

/* fprem (truncando, ieee = 0) y fprem1 (redondeando al par, ieee = 1). El resto sale exacto de una vez
 * (C2 = 0); C0, C3 y C1 llevan los bits 2, 1 y 0 del cociente. */
void rt_fprem(int ieee)
{
    double x = ST(0), y = ST(1);
    uint16_t cc = 0;
    if (isnan(x) || isnan(y) || isinf(x) || y == 0.0) {
        ST(0) = NAN;
    } else {
        int q;
        double r;
        if (ieee) {
            r = remquo(x, y, &q);
            q = q < 0 ? -q : q;
        } else {
            r = fmod(x, y);
            double r8 = fmod(fabs(x), 8.0 * fabs(y));
            q = (int)(r8 / fabs(y));
        }
        ST(0) = r;
        if (q & 4) cc |= C0;
        if (q & 2) cc |= C3;
        if (q & 1) cc |= C1;
    }
    set_cc(cc, C0 | C1 | C2 | C3);
}

/* fptan: ST(0) = tan(ST(0)) y apila 1.0. */
/* fsincos: ST(0) = sin, y se apila el coseno (queda ST(0) = cos, ST(1) = sin). */
void rt_fsincos(void)
{
    double v = ST(0);
    ST(0) = sin(v);
    fpush(cos(v));
    set_cc(0, C2);
}

void rt_fptan(void)
{
    ST(0) = tan(ST(0));
    fpush(1.0);
    set_cc(0, C2);
}

/* fxam: clasifica ST(0). No se lleva la marca de registro vacío: un vacío se ve como el valor que quedó. */
void rt_fxam(void)
{
    double v = ST(0);
    uint16_t cc = signbit(v) ? C1 : 0;
    switch (fpclassify(v)) {
    case FP_NAN: cc |= C0; break;
    case FP_INFINITE: cc |= C2 | C0; break;
    case FP_ZERO: cc |= C3; break;
    case FP_SUBNORMAL: cc |= C3 | C2; break;
    default: cc |= C2; break;
    }
    set_cc(cc, C0 | C1 | C2 | C3);
}

/* fnstenv / fldenv: el entorno de 28 bytes (palabras de control, estado y marcas; punteros en cero).
 * fnstenv además enmascara todas las excepciones, como el original. */
void rt_fnstenv(uint8_t *m, uint32_t a)
{
    memset(m + a, 0, 28);
    wr32(m, a, fpu.cw);
    wr32(m, a + 4, fstatus());
    wr32(m, a + 8, 0);
    fpu.cw |= 0x3f;
}

void rt_fldenv(uint8_t *m, uint32_t a)
{
    uint32_t sw = rd32(m, a + 4);
    fpu.cw = (uint16_t)rd32(m, a);
    fpu.top = (sw >> 11) & 7;
    fpu.sw = (uint16_t)(sw & ~0x3800u);
}
