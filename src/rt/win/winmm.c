/* WINMM: tiempo (timeGetTime sobre el reloj del juego), joystick (no hay) y mmio: lectura de archivos RIFF (.wav) por
 * chunks sobre el VFS de files.c. */
#include "win.h"
#include "core/host.h"

#include <fcntl.h>

RT_IMPORT(timeGetTime) { win_poll_hint(); rt_ret_stdcall(win_ticks_ms(), 0); }
RT_IMPORT(timeGetSystemTime)
{
    uint32_t p = rt_arg(0);
    wr32(g_mem, p, 1);                       /* TIME_MS */
    wr32(g_mem, p + 4, win_ticks_ms());
    rt_ret_stdcall(0, 2);
}
RT_IMPORT(timeBeginPeriod) { rt_ret_stdcall(0, 1); }
RT_IMPORT(timeEndPeriod) { rt_ret_stdcall(0, 1); }
RT_IMPORT(timeGetDevCaps) { wr32(g_mem, rt_arg(0), 1); wr32(g_mem, rt_arg(0) + 4, 1000000); rt_ret_stdcall(0, 2); }

#define JOYERR_UNPLUGGED 167u
RT_IMPORT(joySetCapture) { rt_ret_stdcall(JOYERR_UNPLUGGED, 4); }
RT_IMPORT(joyReleaseCapture) { rt_ret_stdcall(0, 1); }
RT_IMPORT(joyGetNumDevs) { rt_ret_stdcall(0, 0); }
RT_IMPORT(joyGetPos) { rt_ret_stdcall(JOYERR_UNPLUGGED, 2); }
RT_IMPORT(joyGetPosEx) { rt_ret_stdcall(JOYERR_UNPLUGGED, 2); }
RT_IMPORT(joyGetDevCapsA) { rt_ret_stdcall(JOYERR_UNPLUGGED, 3); }

/* ---- mmio ---- */
/* Un mmio es un archivo del VFS (fd) o un archivo en memoria (FOURCC_MEM: nombre NULL y el MMIOINFO trae pchBuffer y
 * cchBuffer; d2s2 del 2001 parsea así los WAV que ya leyó de sus paquetes). MMIOINFO (72 bytes): dwFlags 0,
 * fccIOProc 4, pIOProc 8, wErrorRet 12, htask 16, cchBuffer 20, pchBuffer 24, pchNext 28, pchEndRead 32,
 * pchEndWrite 36, lBufOffset 40, lDiskOffset 44, adwInfo 48, dwReserved 60, hmmio 68. */
typedef struct { int fd; uint32_t mem, size; int64_t pos; } Mmio;
static uint32_t fourcc(const char *s) { return (uint32_t)s[0] | (uint32_t)s[1] << 8 | (uint32_t)s[2] << 16 | (uint32_t)s[3] << 24; }
#define FOURCC_MEM 0x204d454du

static int64_t m_tell(Mmio *m) { return m->fd >= 0 ? host_seek(m->fd, 0, SEEK_CUR) : m->pos; }
static int64_t m_seek(Mmio *m, int64_t off, int how)
{
    if (m->fd >= 0) return host_seek(m->fd, off, how);
    int64_t p = how == SEEK_SET ? off : how == SEEK_CUR ? m->pos + off : (int64_t)m->size + off;
    if (p < 0 || p > m->size) return -1;
    return m->pos = p;
}
static long m_read(Mmio *m, void *dst, uint32_t n)
{
    if (m->fd >= 0) return host_read(m->fd, dst, n);
    if (m->pos >= m->size) return 0;
    if (n > m->size - m->pos) n = (uint32_t)(m->size - m->pos);
    memcpy(dst, g_mem + m->mem + m->pos, n);
    m->pos += n;
    return (long)n;
}

RT_IMPORT(mmioOpenA)
{
    const char *name = rt_arg(0) ? win_str(rt_arg(0)) : "";
    uint32_t info = rt_arg(1), flags = rt_arg(2);
    Mmio *m;
    if (!*name && info && rd32(g_mem, info + 4) == FOURCC_MEM && rd32(g_mem, info + 24)) {
        m = calloc(1, sizeof *m);
        m->fd = -1;
        m->mem = rd32(g_mem, info + 24);
        m->size = rd32(g_mem, info + 20);
        WIN_TRACE("mmioOpenA(memoria %#x, %u bytes, %#x)\n", m->mem, m->size, flags);
    } else {
        char host[1024];
        int fd = *name && win_read_path(name, host, sizeof host) ? host_open(host, O_RDONLY, 0) : -1;
        WIN_TRACE("mmioOpenA(%s, %#x) = %d%s\n", name, flags, fd,
                  info ? "" : " (sin MMIOINFO)");
        if (fd < 0) {
            if (info) WIN_TRACE("  MMIOINFO fcc %#x buf %#x len %u\n", rd32(g_mem, info + 4), rd32(g_mem, info + 24), rd32(g_mem, info + 20));
            rt_ret_stdcall(0, 3);
            return;
        }
        m = calloc(1, sizeof *m);
        m->fd = fd;
    }
    rt_ret_stdcall(win_handle_new(WH_OTHER, m), 3);
}
static Mmio *mm(uint32_t h) { WinHandle *w = win_handle_get(h, WH_OTHER); return w ? (Mmio *)w->ptr : NULL; }
RT_IMPORT(mmioClose)
{
    Mmio *m = mm(rt_arg(0));
    if (m) { if (m->fd >= 0) host_close(m->fd); free(m); win_handle_free(rt_arg(0)); }
    rt_ret_stdcall(0, 2);
}
RT_IMPORT(mmioRead)
{
    Mmio *m = mm(rt_arg(0));
    long r = m ? m_read(m, g_mem + rt_arg(1), rt_arg(2)) : -1;
    rt_ret_stdcall((uint32_t)r, 3);
}
RT_IMPORT(mmioSeek)
{
    Mmio *m = mm(rt_arg(0));
    uint32_t how = rt_arg(2);
    int64_t r = m ? m_seek(m, (int32_t)rt_arg(1), how == 0 ? SEEK_SET : how == 1 ? SEEK_CUR : SEEK_END) : -1;
    rt_ret_stdcall((uint32_t)r, 3);
}

/* MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags. mmioDescend(h, ck, padre, flags):
 * MMIO_FINDCHUNK 0x10, MMIO_FINDRIFF 0x20, MMIO_FINDLIST 0x40. */
RT_IMPORT(mmioDescend)
{
    Mmio *m = mm(rt_arg(0));
    uint32_t ck = rt_arg(1), parent = rt_arg(2), flags = rt_arg(3);
    if (!m) { rt_ret_stdcall(257, 4); return; }                     /* MMIOERR_CHUNKNOTFOUND (aprox.) */
    uint32_t want_id = rd32(g_mem, ck), want_type = rd32(g_mem, ck + 8);
    int64_t end = parent ? (int64_t)rd32(g_mem, parent + 12) + rd32(g_mem, parent + 4) : INT64_MAX;
    for (;;) {
        int64_t at = m_tell(m);
        if (at + 8 > end) { rt_ret_stdcall(257, 4); return; }
        uint8_t h[12];
        if (m_read(m, h, 8) != 8) { rt_ret_stdcall(257, 4); return; }
        uint32_t id = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
        uint32_t size = (uint32_t)h[4] | (uint32_t)h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
        uint32_t type = 0;
        int is_list = id == fourcc("RIFF") || id == fourcc("LIST");
        if (is_list) { m_read(m, h + 8, 4); type = (uint32_t)h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24; }
        int ok = 0;
        if (flags & 0x20u) ok = id == fourcc("RIFF") && (!want_type || type == want_type);
        else if (flags & 0x40u) ok = id == fourcc("LIST") && (!want_type || type == want_type);
        else if (flags & 0x10u) ok = id == want_id;
        else ok = 1;
        if (ok) {
            wr32(g_mem, ck, id); wr32(g_mem, ck + 4, size); wr32(g_mem, ck + 8, type);
            wr32(g_mem, ck + 12, (uint32_t)(at + 8)); wr32(g_mem, ck + 16, 0);
            rt_ret_stdcall(0, 4);
            return;
        }
        m_seek(m, at + 8 + size + (size & 1), SEEK_SET);
    }
}
RT_IMPORT(mmioAscend)
{
    Mmio *m = mm(rt_arg(0));
    uint32_t ck = rt_arg(1);
    if (m) { uint32_t size = rd32(g_mem, ck + 4); m_seek(m, (int64_t)rd32(g_mem, ck + 12) + size + (size & 1), SEEK_SET); }
    rt_ret_stdcall(0, 3);
}
/* Acceso directo al búfer: en memoria el búfer es el archivo entero (pchNext = posición actual); en disco no hay búfer
 * (pchNext == pchEndRead, el llamador cae a mmioRead). */
static void m_info(uint32_t h, Mmio *m, uint32_t info)
{
    memset(g_mem + info, 0, 72);
    if (m && m->fd < 0) {
        wr32(g_mem, info + 4, FOURCC_MEM);
        wr32(g_mem, info + 20, m->size);
        wr32(g_mem, info + 24, m->mem);
        wr32(g_mem, info + 28, m->mem + (uint32_t)m->pos);
        wr32(g_mem, info + 32, m->mem + m->size);
        wr32(g_mem, info + 36, m->mem + m->size);
    }
    if (m) wr32(g_mem, info + 44, (uint32_t)m_tell(m));
    wr32(g_mem, info + 68, h);
}
RT_IMPORT(mmioGetInfo) { m_info(rt_arg(0), mm(rt_arg(0)), rt_arg(1)); rt_ret_stdcall(0, 3); }
RT_IMPORT(mmioSetInfo)
{
    Mmio *m = mm(rt_arg(0));
    if (m && m->fd < 0) m_seek(m, (int64_t)rd32(g_mem, rt_arg(1) + 28) - m->mem, SEEK_SET);
    rt_ret_stdcall(0, 3);
}
RT_IMPORT(mmioAdvance)
{
    uint32_t h = rt_arg(0), info = rt_arg(1);
    Mmio *m = mm(h);
    if (m && m->fd < 0 && info) { m_seek(m, (int64_t)rd32(g_mem, info + 28) - m->mem, SEEK_SET); m_info(h, m, info); }
    rt_ret_stdcall(0, 3);
}
RT_IMPORT(mmioStringToFOURCCA) { char s[5] = {0}; strncpy(s, win_str(rt_arg(0)), 4); rt_ret_stdcall(fourcc(s), 2); }

/* Sonido simple sin DirectSound: se anota y se ignora. */
RT_IMPORT(PlaySoundA) { WIN_TRACE("PlaySoundA(%s)\n", rt_arg(0) ? win_str(rt_arg(0)) : "-"); rt_ret_stdcall(1, 3); }
RT_IMPORT(sndPlaySoundA) { rt_ret_stdcall(1, 2); }
RT_IMPORT(waveOutGetNumDevs) { rt_ret_stdcall(0, 0); }
RT_IMPORT(mciSendCommandA) { rt_ret_stdcall(263, 4); }          /* MCIERR_INVALID_DEVICE_NAME: sin CD de audio */
RT_IMPORT(mciSendStringA) { rt_ret_stdcall(263, 4); }
RT_IMPORT(auxGetNumDevs) { rt_ret_stdcall(0, 0); }
RT_IMPORT(auxSetVolume) { rt_ret_stdcall(0, 2); }
RT_IMPORT(auxGetVolume) { rt_ret_stdcall(0, 2); }
