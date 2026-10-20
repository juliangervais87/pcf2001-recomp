/* Arranque del original recompilado: carga las imágenes, arma el disco virtual (C: y el CD), registra los módulos
 * de Windows que el juego carga a mano (ddraw, dsound, d3drm), arma el bloque del hilo (fs:0) y la pila, llama al
 * DllMain de cada DLL (DLL_PROCESS_ATTACH) en orden y después al entry point del exe.
 *
 * Uso: pcf2001_recomp <imágenes> <cd> <disco C:>
 *   <imágenes>  generated/pcf2001/build/images (el exe y las DLL reubicadas)
 *   <cd>        el contenido de la ISO (tools/pcf2001_preparar.sh)
 *   <disco C:>  carpeta donde el juego "está instalado" y escribe sus partidas (se crea)
 * Variables: RECOMP_TRACE=1|2, RECOMP_HEADLESS=1, RECOMP_FIXED_CLOCK=1, RECOMP_SCRIPT, RECOMP_SHOT_DIR, RECOMP_SCALE. */
#define SDL_MAIN_HANDLED
#include "loader.h"

#include "core/host.h"
#include "core/rt.h"
#include "core/rt_host.h"
#include "win/win.h"

#include <SDL.h>
#include <stdio.h>

#define GAME_STACK_BASE 0x10000000u
#define GAME_STACK_SIZE 0x01000000u
#define GAME_STACK_TOP (GAME_STACK_BASE + GAME_STACK_SIZE - 0x1000u)
#define DLL_PROCESS_ATTACH 1u

void ddraw_register(void);
void dsound_register(void);
void ole32_register(void);

static void mkdirs(const char *base, const char *rel)
{
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", base, rel);
    for (char *c = p + strlen(base) + 1; *c; c++)
        if (*c == '/') { *c = 0; host_mkdir(p); *c = '/'; }
    host_mkdir(p);
}

static void absolute(const char *in, char *out, size_t room)
{
    if (!host_realpath(in, out, room)) snprintf(out, room, "%s", in);
    for (char *c = out; *c; c++) if (*c == '\\') *c = '/';
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "uso: %s <carpeta de imágenes> <carpeta del CD> <carpeta del disco C:>\n", argv[0]);
        return 2;
    }
    SDL_SetMainReady();
    const char *t = getenv("RECOMP_TRACE");
    win_trace = t ? atoi(t) : 0;
    host_mkdir(argv[3]);
    absolute(argv[2], win_cd_dir, sizeof win_cd_dir);
    absolute(argv[3], win_hd_dir, sizeof win_hd_dir);
    mkdirs(win_hd_dir, "WINDOWS/SYSTEM");
    mkdirs(win_hd_dir, "WINDOWS/TEMP");
    /* Lo que deja el instalador además de copiar el CD, según el perfil del juego (config/juegos/<id>.json):
     * carpetas que el juego escribe y archivos con sus valores por defecto (por ejemplo, su .INI). */
    for (uint32_t i = 0; i < rt_game_dir_count; i++) mkdirs(win_hd_dir, rt_game_dirs[i]);
    for (uint32_t i = 0; i < rt_game_file_count; i++) {
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", win_hd_dir, rt_game_files[i].path);
        HostStat st;
        if (!host_stat(p, &st)) continue;
        FILE *f = host_fopen(p, "wb");
        if (f) {
            fputs(rt_game_files[i].text, f);
            fclose(f);
        }
    }
    snprintf(win_cwd, sizeof win_cwd, "%s", WIN_GAME_DIR);
    snprintf(win_command_line, sizeof win_command_line, "%s", rt_command_line);
    snprintf(win_cd_label, sizeof win_cd_label, "%s", rt_cd_label);
    fprintf(stderr, "recomp: CD = %s, C: = %s\n", win_cd_dir, win_hd_dir);

    rt_heap_bump = 0;
    rt_load_images(argv[1]);
    ddraw_register();
    dsound_register();
    ole32_register();

    /* El bloque de información del hilo en fs:0: cadena de excepciones vacía y los límites de la pila. */
    wr32(g_mem, RT_FS_BASE, 0xffffffffu);
    wr32(g_mem, RT_FS_BASE + 4, GAME_STACK_BASE + GAME_STACK_SIZE);
    wr32(g_mem, RT_FS_BASE + 8, GAME_STACK_BASE);
    wr32(g_mem, RT_FS_BASE + 0x18, RT_FS_BASE);
    wr32(g_mem, RT_FS_BASE + 0x2c, rt_tls_new_thread());     /* ThreadLocalStoragePointer */
    cpu.esp = GAME_STACK_TOP;

    for (uint32_t i = 0; i < rt_image_count; i++) {
        const RtImage *im = &rt_images[i];
        if (!im->is_dll) continue;
        fprintf(stderr, "recomp: DllMain de %s (%#x)\n", im->file, im->entry);
        uint32_t args[3] = {im->base, DLL_PROCESS_ATTACH, 0};
        uint32_t ok = rt_callback(im->entry, 3, args);
        if (!ok) rt_fatal("DllMain de %s devolvió FALSE", im->file);
    }
    for (uint32_t i = 0; i < rt_image_count; i++) {
        const RtImage *im = &rt_images[i];
        if (im->is_dll) continue;
        fprintf(stderr, "recomp: entry point de %s (%#x)\n", im->file, im->entry);
        cpu.esp -= 4;
        wr32(g_mem, cpu.esp, RT_RETURN_ADDR);
        rt_call(im->entry);
        fprintf(stderr, "recomp: el entry point volvió con %u\n", cpu.eax);
    }
    return (int)cpu.eax;
}
