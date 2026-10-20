/* The host functions of host.h on macOS and Linux (POSIX). */
#include "../host.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <sys/resource.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

void *host_reserve(size_t size)
{
#ifdef __EMSCRIPTEN__
    /* wasm32 no tiene 4 GB para reservar; el mapa compacto del juego termina en 0x1B000000
     * (src/rt/win/kernel32.c, threads.c): se reserva eso más un margen, en cero. */
    (void)size;
    return calloc(1, 0x1C000000u);
#else
    void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return m == MAP_FAILED ? NULL : m;
#endif
}

#ifdef RECOMP_WEB
/* Web: el CD en el browser se lee por fetch con Range (webcd.c); el resto es MEMFS. */
int webcd_is(const char *path); int webcd_open(const char *path); int webcd_fd(int fd);
long webcd_fd_read(int fd, void *buf, uint32_t n); int64_t webcd_fd_seek(int fd, int64_t off, int whence);
int webcd_fd_close(int fd); int webcd_stat(const char *path, HostStat *st);
void *webcd_opendir(const char *path); const char *webcd_readdir(void *d); void webcd_closedir(void *d);
#define WEB(cond, expr) do { if (cond) return expr; } while (0)
#else
#define WEB(cond, expr) do { } while (0)
#endif
int host_open(const char *path, int flags, int mode) { WEB(webcd_is(path), (flags & 3) ? -1 : webcd_open(path)); return open(path, flags, mode); }
long host_read(int fd, void *buf, uint32_t n) { WEB(webcd_fd(fd), webcd_fd_read(fd, buf, n)); return (long)read(fd, buf, n); }
long host_write(int fd, const void *buf, uint32_t n) { return (long)write(fd, buf, n); }
int64_t host_seek(int fd, int64_t off, int whence) { WEB(webcd_fd(fd), webcd_fd_seek(fd, off, whence)); return (int64_t)lseek(fd, (off_t)off, whence); }
int host_truncate(int fd, int64_t size) { return ftruncate(fd, (off_t)size); }
int host_close(int fd) { WEB(webcd_fd(fd), webcd_fd_close(fd)); return close(fd); }

int host_stat(const char *path, HostStat *out)
{
    WEB(webcd_is(path), webcd_stat(path, out));
    struct stat st;
    if (stat(path, &st)) return -1;
    out->is_dir = S_ISDIR(st.st_mode);
    out->size = (uint64_t)st.st_size;
    out->mtime = (int64_t)st.st_mtime;
    return 0;
}

int host_mkdir(const char *path) { return mkdir(path, 0755); }
int host_unlink(const char *path) { return unlink(path); }
int host_rmdir(const char *path) { return rmdir(path); }
int host_rename(const char *from, const char *to) { return rename(from, to); }
int host_replace(const char *from, const char *to) { return rename(from, to); }
int host_sync(FILE *f) { return fsync(fileno(f)); }

HostDir *host_opendir(const char *path)
{
#ifdef RECOMP_WEB
    if (webcd_is(path)) { void *d = webcd_opendir(path); return d ? (HostDir *)((uintptr_t)d | 1u) : NULL; }   /* marca: bit bajo */
#endif
    return (HostDir *)opendir(path);
}

const char *host_readdir(HostDir *d)
{
    WEB((uintptr_t)d & 1u, webcd_readdir((void *)((uintptr_t)d & ~(uintptr_t)1)));
    WEB((uintptr_t)d & 1u, webcd_readdir((void *)((uintptr_t)d & ~(uintptr_t)1)));
    struct dirent *e = readdir((DIR *)d);
    return e ? e->d_name : NULL;
}

void host_closedir(HostDir *d)
{
#ifdef RECOMP_WEB
    if ((uintptr_t)d & 1u) { webcd_closedir((void *)((uintptr_t)d & ~(uintptr_t)1)); return; }
#endif
    closedir((DIR *)d);
}

int host_realpath(const char *path, char *out, size_t room)
{
#ifdef RECOMP_WEB
    if (webcd_is(path)) return (size_t)snprintf(out, room, "%s", path) < room;
#endif
    char buf[PATH_MAX];
    if (!realpath(path, buf)) return 0;
    return (size_t)snprintf(out, room, "%s", buf) < room;
}

int host_program_path(char *out, size_t room)
{
    char exe[PATH_MAX];
#ifdef __APPLE__
    uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n)) return 0;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
#endif
    return host_realpath(exe, out, room);
}

int host_current_dir(char *out, size_t room) { return getcwd(out, room) != NULL; }

int host_user_dir(char *out, size_t room)
{
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "recomp: HOME is not set, so the user folder is unknown.\n");
        return 0;
    }
#ifdef __APPLE__
    return (size_t)snprintf(out, room, "%s/Library/Application Support/pcf2001", home) < room;
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return (size_t)snprintf(out, room, "%s/pcf2001", xdg) < room;
    return (size_t)snprintf(out, room, "%s/.local/share/pcf2001", home) < room;
#endif
}

int host_home_dir(char *out, size_t room)
{
    const char *home = getenv("HOME");
    return home && *home && (size_t)snprintf(out, room, "%s", home) < room;
}

char host_cd_drive(void) { return 'D'; }
int host_has_drive(char letter) { (void)letter; return 0; }

void host_gmtime(time_t t, struct tm *out) { gmtime_r(&t, out); }
void host_localtime(time_t t, struct tm *out) { localtime_r(&t, out); }

int32_t host_bias_minutes(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    return (int32_t)(-t.tm_gmtoff / 60);
}

uint64_t host_unix_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

FILE *host_fopen(const char *utf8_path, const char *mode)
{
    return fopen(utf8_path, mode);
}

#ifdef __linux__
/* The game makes a host call for every call of the original, so it needs a deep stack (the Mac build
 * asks the linker for 256 MB). On Linux the stack limit is set before the program starts: raise it,
 * then start the program again. RECOMP_STACK_SET stops a second start. */
__attribute__((constructor)) static void raise_stack(void)
{
    const rlim_t want = 0x10000000;
    struct rlimit r;
    if (getenv("RECOMP_STACK_SET") || getrlimit(RLIMIT_STACK, &r)) return;
    if (r.rlim_cur == RLIM_INFINITY || r.rlim_cur >= want) return;
    if (r.rlim_max != RLIM_INFINITY && r.rlim_max < want) r.rlim_cur = r.rlim_max;
    else r.rlim_cur = want;
    if (setrlimit(RLIMIT_STACK, &r)) return;
    setenv("RECOMP_STACK_SET", "1", 1);
    extern char **environ;
    /* Keep the arguments: read them from /proc/self/cmdline. */
    static char buf[16384];
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *args[256];
    int k = 0;
    for (char *p = buf; p < buf + n && k < 255; p += strlen(p) + 1) args[k++] = p;
    args[k] = NULL;
    execve("/proc/self/exe", args, environ);
}
#endif

#include <unistd.h>
void host_sleep_ms(uint32_t ms) { usleep(ms * 1000u); }
