/* Buffer de DirectSound compartido entre dsound.c (la interfaz COM, del lado del juego) y audio.c (el mezclador, en el
 * hilo de audio de SDL). Los cambios que el mezclador no debe ver a medias (alta o baja de un buffer) van entre
 * audio_lock() y audio_unlock(). */
#ifndef RT_WIN_DSOUND_INT_H
#define RT_WIN_DSOUND_INT_H

#include <stdint.h>

typedef struct {
    uint32_t obj, refs, bytes, mem, flags;
    uint32_t rate, channels, bits;             /* formato (WAVEFORMATEX) */
    uint32_t freq;                             /* SetFrequency (0 = la del formato) */
    int32_t volume, pan;
    int playing, looping;
    uint32_t start_ms, start_pos;              /* posición = start_pos + bytes por ms * (ahora - start_ms) */
    int primary;
    int *mem_refs;                             /* cuántos buffers (original + DuplicateSoundBuffer) comparten mem */
} DSBuf;

#define MAX_BUFS 256
extern DSBuf *ds_bufs[MAX_BUFS];               /* huecos NULL: buffers liberados, se reusan */
extern int ds_nbufs;                           /* tope usado de ds_bufs */

void audio_start(void);
void audio_lock(void);
void audio_unlock(void);

#endif
