/* Archivos sobre un sistema de archivos virtual (VFS):
 *   C:\     -> win_hd_dir (la "instalación" del juego y sus partidas: todo lo que el juego escribe)
 *   D:\     -> win_cd_dir (el contenido de la ISO del CD del juego; sólo lectura)
 *   relativa -> la carpeta actual (al arrancar, la carpeta del juego, como un acceso directo)
 * Una lectura de la carpeta del juego (C:\<carpeta>\<ruta>) que no existe en el disco virtual se busca en la raíz del
 * CD: es lo que un instalador completo habría copiado. Cada parte de la ruta se compara sin distinguir mayúsculas (Linux).
 * Más: búsquedas (FindFirstFile), archivos mapeados, .ini (Get/WritePrivateProfile*), datos del volumen. */
#include "win.h"
#include "core/host.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>

char win_hd_dir[1024];
char win_cd_dir[1024];
char win_cwd[1024];                             /* main.c: la carpeta del juego */

/* ---- rutas ---- */

/* Absoluta en forma de Windows, con '\', sin "." ni "..", con la carpeta actual si hace falta. */
void win_full_path(const char *in, char *out, size_t room)
{
    char tmp[2048];
    if (in[0] && in[1] == ':' && in[2] != '\\' && in[2] != '/') {
        /* "C:TACTICS\x": relativa a la carpeta actual de esa unidad (la del juego si es la misma), como en Windows */
        if (toupper((unsigned char)in[0]) == toupper((unsigned char)win_cwd[0])) snprintf(tmp, sizeof tmp, "%s\\%s", win_cwd, in + 2);
        else snprintf(tmp, sizeof tmp, "%c:\\%s", in[0], in + 2);
    } else if (in[0] && in[1] == ':') snprintf(tmp, sizeof tmp, "%s", in);
    else if (in[0] == '\\' || in[0] == '/') snprintf(tmp, sizeof tmp, "%c:%s", win_cwd[0], in);
    else snprintf(tmp, sizeof tmp, "%s\\%s", win_cwd, in);
    char *parts[128];
    int n = 0;
    char drive = (char)toupper((unsigned char)tmp[0]);
    for (char *p = strtok(tmp + 2, "\\/"); p && n < 128; p = strtok(NULL, "\\/")) {
        if (!strcmp(p, ".")) continue;
        if (!strcmp(p, "..")) { if (n) n--; continue; }
        parts[n++] = p;
    }
    size_t len = (size_t)snprintf(out, room, "%c:", drive);
    if (!n && len < room) len += (size_t)snprintf(out + len, room - len, "\\");
    for (int i = 0; i < n && len < room; i++) len += (size_t)snprintf(out + len, room - len, "\\%s", parts[i]);
}

/* Busca el nombre real de cada parte en el host (sin distinguir mayúsculas). Devuelve 1 si la ruta entera existe;
 * si no, deja en out la mejor ruta (lo que existe + el resto tal cual) para crear. */
static int resolve_case(const char *root, const char *rest, char *out, size_t room)
{
    snprintf(out, room, "%s", root);
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", rest);
    int exists = 1;
    for (char *p = strtok(buf, "\\/"); p; p = strtok(NULL, "\\/")) {
        char found[512] = "";
        if (exists && !HOST_FOLDS_CASE) {
            HostDir *d = host_opendir(out);
            const char *e;
            while (d && (e = host_readdir(d))) if (!strcasecmp(e, p)) { snprintf(found, sizeof found, "%s", e); break; }
            if (d) host_closedir(d);
        }
        size_t len = strlen(out);
        snprintf(out + len, room - len, "/%s", found[0] ? found : p);
        if (exists) {
            HostStat st;
            if (host_stat(out, &st)) exists = 0;
        }
    }
    return exists;
}

static int map_drive(char drive, const char **root)
{
    if (drive == 'C') { *root = win_hd_dir; return 1; }
    if (drive == WIN_CD_LETTER) { *root = win_cd_dir; return 1; }
    return 0;
}

/* Ruta del host para abrir/crear. Devuelve 1 si existe. */
int win_host_path(const char *winpath, char *out, size_t room)
{
    char full[2048];
    win_full_path(winpath, full, sizeof full);
    const char *root;
    if (!map_drive(full[0], &root)) { snprintf(out, room, "/nonexistent/%s", full + 2); return 0; }
    return resolve_case(root, full + 2, out, room);
}

/* Para leer: el disco virtual y, si no está y la ruta es de la instalación (C:\<carpeta del juego>\...), el CD. */
int win_read_path(const char *winpath, char *out, size_t room)
{
    if (win_host_path(winpath, out, room)) return 1;
    char full[2048];
    win_full_path(winpath, full, sizeof full);
    size_t g = strlen(WIN_GAME_DIR);
    if (!strncasecmp(full, WIN_GAME_DIR, g) && (full[g] == '\\' || !full[g])) {
        char alt[2048];
        if (resolve_case(win_cd_dir, full + g, alt, sizeof alt)) { snprintf(out, room, "%s", alt); return 1; }
    }
    return 0;
}

static void error_from_errno(void)
{
    win_last_error = errno == ENOENT ? ERROR_FILE_NOT_FOUND : errno == EACCES ? ERROR_ACCESS_DENIED
                   : errno == EEXIST ? ERROR_FILE_EXISTS : ERROR_PATH_NOT_FOUND;
}

/* ---- archivos abiertos ---- */
typedef struct { int fd; char path[1024]; } WinFile;

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define INVALID_HANDLE 0xffffffffu

static int is_cd(const char *host) { return !strncmp(host, win_cd_dir, strlen(win_cd_dir)); }

RT_IMPORT(CreateFileA)
{
    const char *name = win_str(rt_arg(0));
    uint32_t access = rt_arg(1), disp = rt_arg(4);
    char host[1024];
    int exists = (access & GENERIC_WRITE) ? win_host_path(name, host, sizeof host) : win_read_path(name, host, sizeof host);
    WIN_TRACE("CreateFileA(%s, %#x, %u) -> %s%s\n", name, access, disp, host, exists ? "" : " (no existe)");
    if (exists && is_cd(host) && (access & GENERIC_WRITE)) { win_last_error = ERROR_ACCESS_DENIED; rt_ret_stdcall(INVALID_HANDLE, 7); return; }
    int flags = (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case CREATE_NEW: if (exists) { win_last_error = ERROR_FILE_EXISTS; rt_ret_stdcall(INVALID_HANDLE, 7); return; } flags |= O_CREAT; break;
    case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: if (!exists) { win_last_error = ERROR_FILE_NOT_FOUND; rt_ret_stdcall(INVALID_HANDLE, 7); return; } break;
    case OPEN_ALWAYS: flags |= O_CREAT; break;
    case TRUNCATE_EXISTING: if (!exists) { win_last_error = ERROR_FILE_NOT_FOUND; rt_ret_stdcall(INVALID_HANDLE, 7); return; } flags |= O_TRUNC; break;
    }
    HostStat st;
    if (exists && !host_stat(host, &st) && st.is_dir) { win_last_error = ERROR_ACCESS_DENIED; rt_ret_stdcall(INVALID_HANDLE, 7); return; }
    int fd = host_open(host, flags, 0644);
    if (fd < 0) { error_from_errno(); rt_ret_stdcall(INVALID_HANDLE, 7); return; }
    WinFile *f = calloc(1, sizeof *f);
    f->fd = fd;
    snprintf(f->path, sizeof f->path, "%s", host);
    win_last_error = (exists && (disp == CREATE_ALWAYS || disp == OPEN_ALWAYS)) ? ERROR_ALREADY_EXISTS : 0;
    rt_ret_stdcall(win_handle_new(WH_FILE, f), 7);
}

static WinFile *file_of(uint32_t h) { WinHandle *w = win_handle_get(h, WH_FILE); return w ? (WinFile *)w->ptr : NULL; }

RT_IMPORT(ReadFile)
{
    WinFile *f = file_of(rt_arg(0));
    uint32_t buf = rt_arg(1), n = rt_arg(2), got = rt_arg(3);
    long r = f ? host_read(f->fd, g_mem + buf, n) : -1;
    if (got) wr32(g_mem, got, r > 0 ? (uint32_t)r : 0);
    if (r < 0) win_last_error = ERROR_INVALID_HANDLE;
    rt_ret_stdcall(r >= 0, 5);
}

RT_IMPORT(WriteFile)
{
    uint32_t h = rt_arg(0), buf = rt_arg(1), n = rt_arg(2), put = rt_arg(3);
    if (h >= 0xF0000000u) {                     /* stdout/stderr */
        fwrite(g_mem + buf, 1, n, stderr);
        if (put) wr32(g_mem, put, n);
        rt_ret_stdcall(1, 5);
        return;
    }
    WinFile *f = file_of(h);
    long r = f ? host_write(f->fd, g_mem + buf, n) : -1;
    if (put) wr32(g_mem, put, r > 0 ? (uint32_t)r : 0);
    if (r < 0) win_last_error = ERROR_ACCESS_DENIED;
    rt_ret_stdcall(r >= 0, 5);
}

RT_IMPORT(SetFilePointer)
{
    WinFile *f = file_of(rt_arg(0));
    int64_t off = (int32_t)rt_arg(1);
    uint32_t high = rt_arg(2), how = rt_arg(3);
    if (high) off = (int64_t)(((uint64_t)rd32(g_mem, high) << 32) | rt_arg(1));
    int64_t r = f ? host_seek(f->fd, off, how == 0 ? SEEK_SET : how == 1 ? SEEK_CUR : SEEK_END) : -1;
    if (r < 0) { win_last_error = ERROR_INVALID_PARAMETER; rt_ret_stdcall(0xffffffffu, 4); return; }
    if (high) wr32(g_mem, high, (uint32_t)(r >> 32));
    win_last_error = 0;
    rt_ret_stdcall((uint32_t)r, 4);
}

RT_IMPORT(SetEndOfFile)
{
    WinFile *f = file_of(rt_arg(0));
    int ok = f && !host_truncate(f->fd, host_seek(f->fd, 0, SEEK_CUR));
    rt_ret_stdcall(ok, 1);
}
RT_IMPORT(FlushFileBuffers) { rt_ret_stdcall(1, 1); }

RT_IMPORT(GetFileSize)
{
    WinFile *f = file_of(rt_arg(0));
    if (!f) { win_last_error = ERROR_INVALID_HANDLE; rt_ret_stdcall(0xffffffffu, 2); return; }
    int64_t cur = host_seek(f->fd, 0, SEEK_CUR), end = host_seek(f->fd, 0, SEEK_END);
    host_seek(f->fd, cur, SEEK_SET);
    if (rt_arg(1)) wr32(g_mem, rt_arg(1), (uint32_t)(end >> 32));
    rt_ret_stdcall((uint32_t)end, 2);
}

RT_IMPORT(GetFileType) { rt_ret_stdcall(rt_arg(0) >= 0xF0000000u ? 2u : 1u, 1); }   /* CHAR o DISK */

static uint64_t unix_to_filetime(int64_t s) { return (uint64_t)(s + 11644473600ll) * 10000000ull; }

RT_IMPORT(GetFileTime)
{
    WinFile *f = file_of(rt_arg(0));
    HostStat st = {0};
    if (f) host_stat(f->path, &st);
    uint64_t ft = unix_to_filetime(st.mtime);
    for (int i = 1; i <= 3; i++) if (rt_arg(i)) wr64(g_mem, rt_arg(i), ft);
    rt_ret_stdcall(f != NULL, 4);
}
RT_IMPORT(SetFileTime) { rt_ret_stdcall(1, 4); }

RT_IMPORT(GetFileInformationByHandle)
{
    WinFile *f = file_of(rt_arg(0));
    uint32_t p = rt_arg(1);
    HostStat st = {0};
    if (f) host_stat(f->path, &st);
    memset(g_mem + p, 0, 52);
    wr32(g_mem, p, 0x20);
    uint64_t ft = unix_to_filetime(st.mtime);
    wr64(g_mem, p + 4, ft); wr64(g_mem, p + 12, ft); wr64(g_mem, p + 20, ft);
    wr32(g_mem, p + 32, (uint32_t)(st.size >> 32));
    wr32(g_mem, p + 36, (uint32_t)st.size);
    wr32(g_mem, p + 40, 1);
    rt_ret_stdcall(f != NULL, 2);
}

/* Mapeos de archivos: un mapeo es una copia del archivo en el heap (sólo lectura en el único uso del juego). */
RT_IMPORT(CreateFileMappingA)
{
    WinFile *f = file_of(rt_arg(0));
    if (!f) { win_last_error = ERROR_INVALID_HANDLE; rt_ret_stdcall(0, 6); return; }
    int64_t cur = host_seek(f->fd, 0, SEEK_CUR), size = host_seek(f->fd, 0, SEEK_END);
    uint32_t mem = rt_alloc((uint32_t)size + 1);
    host_seek(f->fd, 0, SEEK_SET);
    host_read(f->fd, g_mem + mem, (uint32_t)size);
    host_seek(f->fd, cur, SEEK_SET);
    uint32_t h = win_handle_new(WH_MAPPING, NULL);
    win_handle_get(h, WH_MAPPING)->a = mem;
    win_last_error = 0;
    rt_ret_stdcall(h, 6);
}
RT_IMPORT(MapViewOfFile)
{
    WinHandle *w = win_handle_get(rt_arg(0), WH_MAPPING);
    rt_ret_stdcall(w ? w->a + rt_arg(3) : 0, 5);
}
RT_IMPORT(UnmapViewOfFile) { rt_ret_stdcall(1, 1); }
RT_IMPORT(LockFile) { rt_ret_stdcall(1, 5); }
RT_IMPORT(UnlockFile) { rt_ret_stdcall(1, 5); }

/* CloseHandle sirve para todo tipo de handle. */
RT_IMPORT(CloseHandle)
{
    uint32_t h = rt_arg(0);
    WinFile *f = file_of(h);
    if (f) { host_close(f->fd); free(f); }
    WinHandle *w = win_handle_get(h, WH_FREE);
    if (w && w->kind != WH_THREAD) win_handle_free(h);       /* un hilo vivo sigue en su tabla */
    rt_ret_stdcall(1, 1);
}

/* ---- atributos, carpetas y nombres ---- */
RT_IMPORT(GetFileAttributesA)
{
    char host[1024];
    HostStat st;
    const char *name = win_str(rt_arg(0));
    if (!win_read_path(name, host, sizeof host) || host_stat(host, &st)) {
        WIN_TRACE("GetFileAttributesA(%s): no existe\n", name);
        win_last_error = ERROR_FILE_NOT_FOUND;
        rt_ret_stdcall(0xffffffffu, 1);
        return;
    }
    uint32_t a = st.is_dir ? 0x10u : 0x20u;
    if (is_cd(host)) a |= 1u;                                /* READONLY */
    rt_ret_stdcall(a, 1);
}
RT_IMPORT(SetFileAttributesA) { rt_ret_stdcall(1, 2); }

static void simple_op(int (*op)(const char *), int nargs)
{
    char host[1024];
    win_host_path(win_str(rt_arg(0)), host, sizeof host);
    int ok = op(host) == 0;
    if (!ok) error_from_errno();
    rt_ret_stdcall(ok, nargs);
}
RT_IMPORT(DeleteFileA) { simple_op(host_unlink, 1); }
RT_IMPORT(RemoveDirectoryA) { simple_op(host_rmdir, 1); }
RT_IMPORT(CreateDirectoryA) { simple_op(host_mkdir, 2); }

RT_IMPORT(MoveFileA)
{
    char a[1024], b[1024];
    win_host_path(win_str(rt_arg(0)), a, sizeof a);
    win_host_path(win_str(rt_arg(1)), b, sizeof b);
    int ok = host_rename(a, b) == 0;
    if (!ok) error_from_errno();
    rt_ret_stdcall(ok, 2);
}

RT_IMPORT(CopyFileA)
{
    char a[1024], b[1024];
    int ok = win_read_path(win_str(rt_arg(0)), a, sizeof a);
    win_host_path(win_str(rt_arg(1)), b, sizeof b);
    HostStat st;
    if (ok && rt_arg(2) && !host_stat(b, &st)) { win_last_error = ERROR_FILE_EXISTS; rt_ret_stdcall(0, 3); return; }
    FILE *in = ok ? host_fopen(a, "rb") : NULL, *out = in ? host_fopen(b, "wb") : NULL;
    char buf[65536];
    size_t n;
    while (out && (n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    if (in) fclose(in);
    if (out) fclose(out);
    if (!out) win_last_error = ERROR_FILE_NOT_FOUND;
    rt_ret_stdcall(out != NULL, 3);
}

RT_IMPORT(GetCurrentDirectoryA)
{
    uint32_t room = rt_arg(0), buf = rt_arg(1), n = (uint32_t)strlen(win_cwd);
    if (room > n) win_put_str(buf, win_cwd, room);
    rt_ret_stdcall(room > n ? n : n + 1, 2);
}
RT_IMPORT(SetCurrentDirectoryA)
{
    char full[1024], host[1024];
    win_full_path(win_str(rt_arg(0)), full, sizeof full);
    if (!win_read_path(full, host, sizeof host)) { win_last_error = ERROR_PATH_NOT_FOUND; rt_ret_stdcall(0, 1); return; }
    snprintf(win_cwd, sizeof win_cwd, "%s", full);
    rt_ret_stdcall(1, 1);
}
RT_IMPORT(GetFullPathNameA)
{
    uint32_t room = rt_arg(1), buf = rt_arg(2), filep = rt_arg(3);
    char full[1024];
    win_full_path(win_str(rt_arg(0)), full, sizeof full);
    uint32_t n = (uint32_t)strlen(full);
    if (room <= n) { rt_ret_stdcall(n + 1, 4); return; }
    win_put_str(buf, full, room);
    if (filep) {
        char *s = strrchr(win_str(buf), '\\');
        wr32(g_mem, filep, s ? buf + (uint32_t)(s - win_str(buf)) + 1 : buf);
    }
    rt_ret_stdcall(n, 4);
}
RT_IMPORT(GetWindowsDirectoryA) { win_put_str(rt_arg(0), "C:\\WINDOWS", rt_arg(1)); rt_ret_stdcall(10, 2); }
RT_IMPORT(GetSystemDirectoryA) { win_put_str(rt_arg(0), "C:\\WINDOWS\\SYSTEM", rt_arg(1)); rt_ret_stdcall(17, 2); }
RT_IMPORT(GetTempPathA) { win_put_str(rt_arg(1), "C:\\WINDOWS\\TEMP\\", rt_arg(0)); rt_ret_stdcall(16, 2); }
RT_IMPORT(GetTempFileNameA)
{
    static uint32_t n = 1;
    char name[300];
    snprintf(name, sizeof name, "%s\\%.3s%04X.TMP", win_str(rt_arg(0)), win_str(rt_arg(1)), n);
    win_put_str(rt_arg(3), name, 260);
    rt_ret_stdcall(n++, 4);
}

/* ---- unidades ---- */
#define DRIVE_FIXED 3u
#define DRIVE_CDROM 5u
#define DRIVE_NO_ROOT_DIR 1u
static char drive_of(uint32_t root) { return root ? (char)toupper((unsigned char)win_str(root)[0]) : win_cwd[0]; }
/* RECOMP_SIN_CD=1: la máquina no tiene unidad de CD (sólo C:), con el DISK.ID en la carpeta
 * de instalación. Las rutas D:\ siguen resolviendo. */
static int sin_cd(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("RECOMP_SIN_CD"); v = e && *e && strcmp(e, "0"); }
    return v;
}
RT_IMPORT(GetDriveTypeA)
{
    char d = drive_of(rt_arg(0));
    uint32_t t = d == 'C' ? DRIVE_FIXED : d == WIN_CD_LETTER && !sin_cd() ? DRIVE_CDROM : DRIVE_NO_ROOT_DIR;
    WIN_TRACE("GetDriveTypeA(%c) = %u\n", d, t);
    rt_ret_stdcall(t, 1);
}
RT_IMPORT(GetLogicalDrives) { rt_ret_stdcall((1u << 2) | (sin_cd() ? 0u : 1u << (WIN_CD_LETTER - 'A')), 0); }
RT_IMPORT(GetLogicalDriveStringsA)
{
    static const char s[] = {'C', ':', '\\', 0, WIN_CD_LETTER, ':', '\\', 0, 0};
    uint32_t n = sin_cd() ? 5u : (uint32_t)sizeof s;        /* sin CD: «C:\» y el doble cero */
    uint32_t room = rt_arg(0);
    if (room >= n) { memcpy(g_mem + rt_arg(1), s, n - 1); g_mem[rt_arg(1) + n - 1] = 0; }
    rt_ret_stdcall(n - 1, 2);
}
RT_IMPORT(GetDiskFreeSpaceA)
{
    /* 2 GB libres de 4 GB: lo que reportaba un disco de 1998 con FAT32. */
    if (rt_arg(1)) wr32(g_mem, rt_arg(1), 64);
    if (rt_arg(2)) wr32(g_mem, rt_arg(2), 512);
    if (rt_arg(3)) wr32(g_mem, rt_arg(3), 65536);
    if (rt_arg(4)) wr32(g_mem, rt_arg(4), 131072);
    rt_ret_stdcall(1, 5);
}

/* GetVolumeInformationA: la etiqueta y el número de serie salen de la ISO (main.c los lee del descriptor
 * primario del volumen) para el CD; el disco C: tiene una etiqueta fija. */
char win_cd_label[33];                          /* main.c: la del perfil (tools/iso_label.py) */
uint32_t win_cd_serial = 0x1997AB97u;
RT_IMPORT(GetVolumeInformationA)
{
    char d = drive_of(rt_arg(0));
    uint32_t name = rt_arg(1), name_room = rt_arg(2), serial = rt_arg(3), maxlen = rt_arg(4), flags = rt_arg(5);
    uint32_t fs = rt_arg(6), fs_room = rt_arg(7);
    fprintf(stderr, "recomp: GetVolumeInformationA(%c:)\n", d);
    if (d != 'C' && d != WIN_CD_LETTER) { win_last_error = ERROR_PATH_NOT_FOUND; rt_ret_stdcall(0, 8); return; }
    if (name) win_put_str(name, d == 'C' ? "DISCO" : win_cd_label, name_room);
    if (serial) wr32(g_mem, serial, d == 'C' ? 0x12345678u : win_cd_serial);
    if (maxlen) wr32(g_mem, maxlen, 255);
    if (flags) wr32(g_mem, flags, d == 'C' ? 6u : 0x80004u);
    if (fs) win_put_str(fs, d == 'C' ? "FAT32" : "CDFS", fs_room);
    rt_ret_stdcall(1, 8);
}

/* ---- búsquedas ---- */
typedef struct { char dir[1024], pattern[260]; HostDir *d; char dir2[1024]; int second; } WinFind;

static int match(const char *pat, const char *s)
{
    if (!*pat) return !*s;
    if (*pat == '*') { for (;; s++) { if (match(pat + 1, s)) return 1; if (!*s) return 0; } }
    if (!*s) return !strcmp(pat, ".") || !strcmp(pat, "*");   /* "*.*" casa nombres sin punto */
    if (*pat == '?' || toupper((unsigned char)*pat) == toupper((unsigned char)*s)) return match(pat + 1, s + 1);
    if (*pat == '.' && !strchr(s, '.') && !strcmp(pat, ".*")) return 1;
    return 0;
}

static void fill_find(uint32_t p, const char *dir, const char *name)
{
    char full[2048];
    snprintf(full, sizeof full, "%s/%s", dir, name);
    HostStat st = {0};
    host_stat(full, &st);
    memset(g_mem + p, 0, 320);
    wr32(g_mem, p, st.is_dir ? 0x10u : 0x20u);
    uint64_t ft = unix_to_filetime(st.mtime);
    wr64(g_mem, p + 4, ft); wr64(g_mem, p + 12, ft); wr64(g_mem, p + 20, ft);
    wr32(g_mem, p + 28, (uint32_t)(st.size >> 32));
    wr32(g_mem, p + 32, (uint32_t)st.size);
    char up[260];
    snprintf(up, sizeof up, "%s", name);
    win_put_str(p + 44, up, 260);
    win_put_str(p + 304, "", 14);
}

static int find_next(WinFind *f, uint32_t out)
{
    for (;;) {
        const char *e = f->d ? host_readdir(f->d) : NULL;
        if (e) {
            if (match(f->pattern, e)) { fill_find(out, f->second ? f->dir2 : f->dir, e); return 1; }
            continue;
        }
        if (f->d) host_closedir(f->d);
        f->d = NULL;
        if (!f->second && f->dir2[0]) { f->second = 1; f->d = host_opendir(f->dir2); if (f->d) continue; }
        return 0;
    }
}

RT_IMPORT(FindFirstFileA)
{
    const char *spec = win_str(rt_arg(0));
    uint32_t out = rt_arg(1);
    char full[1024];
    win_full_path(spec, full, sizeof full);
    char *slash = strrchr(full, '\\');
    WinFind *f = calloc(1, sizeof *f);
    snprintf(f->pattern, sizeof f->pattern, "%s", slash ? slash + 1 : full);
    if (slash) *slash = 0;
    if (strlen(full) == 2) strcat(full, "\\");
    char host[1024];
    int have = win_host_path(full, f->dir, sizeof f->dir);
    size_t g = strlen(WIN_GAME_DIR);                         /* la carpeta del juego también lista el CD */
    if (!strncasecmp(full, WIN_GAME_DIR, g) && (full[g] == '\\' || !full[g]))
        resolve_case(win_cd_dir, full + g, f->dir2, sizeof f->dir2);
    (void)host;
    f->d = have ? host_opendir(f->dir) : NULL;
    if (!f->d && f->dir2[0]) { f->second = 1; f->d = host_opendir(f->dir2); }
    WIN_TRACE("FindFirstFileA(%s)\n", spec);
    if (!find_next(f, out)) { free(f); win_last_error = ERROR_FILE_NOT_FOUND; rt_ret_stdcall(0xffffffffu, 2); return; }
    rt_ret_stdcall(win_handle_new(WH_FIND, f), 2);
}
RT_IMPORT(FindNextFileA)
{
    WinHandle *w = win_handle_get(rt_arg(0), WH_FIND);
    int ok = w && find_next((WinFind *)w->ptr, rt_arg(1));
    if (!ok) win_last_error = ERROR_NO_MORE_FILES;
    rt_ret_stdcall(ok, 2);
}
RT_IMPORT(FindClose)
{
    WinHandle *w = win_handle_get(rt_arg(0), WH_FIND);
    if (w) { WinFind *f = w->ptr; if (f->d) host_closedir(f->d); free(f); win_handle_free(rt_arg(0)); }
    rt_ret_stdcall(w != NULL, 1);
}

/* ---- .ini ---- */
static int ini_read(const char *file, const char *sec, const char *key, char *out, size_t room)
{
    char host[1024];
    char winname[600];
    snprintf(winname, sizeof winname, "%s", strchr(file, '\\') || strchr(file, ':') ? file : "");
    if (!winname[0]) snprintf(winname, sizeof winname, "C:\\WINDOWS\\%s", file);
    if (!win_read_path(winname, host, sizeof host)) return 0;
    FILE *f = host_fopen(host, "rb");
    if (!f) return 0;
    char line[1024];
    int in = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (line[0] == '[') { char *c = strchr(line, ']'); if (c) *c = 0; in = !strcasecmp(line + 1, sec); continue; }
        char *eq = strchr(line, '=');
        if (in && eq) { *eq = 0; if (!strcasecmp(line, key)) { snprintf(out, room, "%s", eq + 1); found = 1; break; } }
    }
    fclose(f);
    return found;
}

RT_IMPORT(GetPrivateProfileStringA)
{
    uint32_t sec = rt_arg(0), key = rt_arg(1), def = rt_arg(2), buf = rt_arg(3), room = rt_arg(4), file = rt_arg(5);
    char v[1024];
    if (!sec || !key || !ini_read(win_str(file), win_str(sec), win_str(key), v, sizeof v)) snprintf(v, sizeof v, "%s", def ? win_str(def) : "");
    win_put_str(buf, v, room);
    WIN_TRACE("GetPrivateProfileStringA([%s] %s, %s) = %s\n", sec ? win_str(sec) : "-", key ? win_str(key) : "-", win_str(file), v);
    rt_ret_stdcall((uint32_t)strlen(win_str(buf)), 6);
}
RT_IMPORT(GetPrivateProfileIntA)
{
    char v[64];
    uint32_t r = rt_arg(2);
    if (ini_read(win_str(rt_arg(3)), win_str(rt_arg(0)), win_str(rt_arg(1)), v, sizeof v)) r = (uint32_t)strtol(v, NULL, 10);
    rt_ret_stdcall(r, 4);
}
RT_IMPORT(GetProfileStringA)
{
    win_put_str(rt_arg(3), rt_arg(2) ? win_str(rt_arg(2)) : "", rt_arg(4));
    rt_ret_stdcall((uint32_t)strlen(win_str(rt_arg(3))), 5);
}
RT_IMPORT(GetProfileIntA) { rt_ret_stdcall(rt_arg(2), 3); }

/* Escribir un .ini: reescribe el archivo con la clave cambiada (sólo en el disco virtual). */
RT_IMPORT(WritePrivateProfileStringA)
{
    uint32_t secp = rt_arg(0), keyp = rt_arg(1), valp = rt_arg(2), filep = rt_arg(3);
    char winname[600], host[1024], src[1024];
    const char *file = win_str(filep);
    snprintf(winname, sizeof winname, "%s", strchr(file, '\\') || strchr(file, ':') ? file : "");
    if (!winname[0]) snprintf(winname, sizeof winname, "C:\\WINDOWS\\%s", file);
    win_host_path(winname, host, sizeof host);
    int have = win_read_path(winname, src, sizeof src);
    const char *sec = secp ? win_str(secp) : "", *key = keyp ? win_str(keyp) : NULL, *val = valp ? win_str(valp) : NULL;
    char *text = NULL;
    size_t len = 0;
    FILE *in = have ? host_fopen(src, "rb") : NULL;
    if (in) { fseek(in, 0, SEEK_END); len = (size_t)ftell(in); fseek(in, 0, SEEK_SET); text = malloc(len + 1); len = fread(text, 1, len, in); text[len] = 0; fclose(in); }
    FILE *out = host_fopen(host, "wb");
    if (!out) { free(text); rt_ret_stdcall(0, 4); return; }
    int in_sec = 0, done = 0, seen_sec = 0;
    char *save, *line = text ? strtok_r(text, "\n", &save) : NULL;
    for (; line; line = strtok_r(NULL, "\n", &save)) {
        char clean[1024];
        snprintf(clean, sizeof clean, "%s", line);
        char *e = clean + strlen(clean);
        while (e > clean && (e[-1] == '\r')) *--e = 0;
        if (clean[0] == '[') {
            if (in_sec && !done && key && val) { fprintf(out, "%s=%s\r\n", key, val); done = 1; }
            char name[512]; snprintf(name, sizeof name, "%s", clean + 1); char *c = strchr(name, ']'); if (c) *c = 0;
            in_sec = !strcasecmp(name, sec);
            seen_sec |= in_sec;
            if (in_sec && !key) continue;                    /* borrar la sección */
        } else if (in_sec) {
            if (!key) continue;
            char *eq = strchr(clean, '=');
            if (eq) { *eq = 0; if (!strcasecmp(clean, key)) { if (val && !done) fprintf(out, "%s=%s\r\n", key, val); done = 1; continue; } *eq = '='; }
        }
        fprintf(out, "%s\r\n", clean);
    }
    if (key && val && !done) { if (!seen_sec) fprintf(out, "[%s]\r\n", sec); fprintf(out, "%s=%s\r\n", key, val); }
    fclose(out);
    free(text);
    rt_ret_stdcall(1, 4);
}
