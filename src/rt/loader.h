/* Cargador de varias imágenes PE (exe + DLL traducidas) en la memoria plana, cada una en su base.
 * La lista la escribe tools/recomp/build.py en gen/images.c, en orden de inicialización. */
#ifndef PCF2001_RECOMP_LOADER_H
#define PCF2001_RECOMP_LOADER_H

#include <stdint.h>

typedef struct {
    const char *file;     /* nombre del archivo dentro de la carpeta de entrada */
    uint32_t base, size, entry;
    int is_dll;
} RtImage;

extern const RtImage rt_images[];
extern const uint32_t rt_image_count;

/* El perfil del juego (config/juegos/<id>.json), también en gen/images.c: dónde cree que está instalado, la
 * etiqueta del CD, dónde empieza el heap y lo que el instalador deja en C: (carpetas y archivos por defecto). */
typedef struct { const char *path, *text; } RtGameFile;
extern const char rt_game_dir[], rt_exe_path[], rt_cd_label[], rt_command_line[];
extern const char *const rt_game_dirs[];
extern const uint32_t rt_game_dir_count;
extern const RtGameFile rt_game_files[];
extern const uint32_t rt_game_file_count;

/* Reserva la memoria, copia las secciones de cada imagen desde dir y escribe las IAT. */
void rt_load_images(const char *dir);
/* TLS implícito: arreglo de bloques para un hilo nuevo (va en fs:[0x2c]); 0 si ninguna imagen usa TLS. */
uint32_t rt_tls_new_thread(void);

#endif
