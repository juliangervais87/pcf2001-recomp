/* The host system under the runtime of the recompiled game: memory, files, folders, time and the
 * folders of the program and the user. host/posix.c serves macOS, host/win32.c serves 64-bit Windows.
 *
 * Host paths are UTF-8 with '/' between the parts. On macOS a path starts at the root ("/Users/max").
 * On Windows it starts with a drive ("C:/Users/max"). The functions that can fail set errno as the C
 * library does. */
#ifndef RECOMP_HOST_H
#define RECOMP_HOST_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

/* 1 when the host file system ignores case (Windows). The runtime then skips its own lookup of the
 * spelling of each part of a path. */
#ifdef _WIN32
#define HOST_FOLDS_CASE 1
#else
#define HOST_FOLDS_CASE 0
#endif

/* A block of size bytes that reads as zero and that the program can write anywhere. Pages may come
 * on first use. NULL on failure. */
void *host_reserve(size_t size);

/* Files. host_open takes the O_ flags of <fcntl.h> and opens in binary mode. */
int host_open(const char *path, int flags, int mode);
long host_read(int fd, void *buf, uint32_t n);
long host_write(int fd, const void *buf, uint32_t n);
int64_t host_seek(int fd, int64_t off, int whence);
int host_truncate(int fd, int64_t size);     /* 0 on success */
int host_close(int fd);

/* fopen for a UTF-8 host path. */
FILE *host_fopen(const char *utf8_path, const char *mode);

typedef struct { int is_dir; uint64_t size; int64_t mtime; } HostStat;
int host_stat(const char *path, HostStat *st);   /* 0 on success */
int host_mkdir(const char *path);                /* 0 on success */
int host_unlink(const char *path);               /* 0 on success */
int host_rmdir(const char *path);                /* removes an empty folder; 0 on success */
int host_rename(const char *from, const char *to);
/* Moves from over to, and replaces to when it exists (also on Windows). 0 on success. */
int host_replace(const char *from, const char *to);
/* Flushes the file to the disk. 0 on success. Call fflush first. */
int host_sync(FILE *f);

/* Folder listing: each name, "." and ".." included, then NULL. */
typedef struct HostDir HostDir;
HostDir *host_opendir(const char *path);
const char *host_readdir(HostDir *d);
void host_closedir(HostDir *d);

/* The absolute form of a path that exists, with no "." or "..". 0 on failure. */
int host_realpath(const char *path, char *out, size_t room);
/* The path of the running program. 0 on failure. */
int host_program_path(char *out, size_t room);
/* The current folder. 0 on failure. */
int host_current_dir(char *out, size_t room);
/* The folder for every write of the game, not yet created: ~/Library/Application Support/pcf2001 or
 * %APPDATA%\pcf2001. 0 with a message on stderr when the system gives no base folder. */
int host_user_dir(char *out, size_t room);
/* The home folder of the player, with "/" between parts. 0 when the system gives none. */
int host_home_dir(char *out, size_t room);

/* The drive letter of the CD drive that holds the game: D on macOS, a letter that the
 * system does not use on Windows. */
char host_cd_drive(void);
/* 1 when the host has this drive (Windows only; macOS has none). */
int host_has_drive(char letter);

/* Time. */
void host_gmtime(time_t t, struct tm *out);
void host_localtime(time_t t, struct tm *out);
int32_t host_bias_minutes(void);             /* minutes behind UTC now, as Windows gives it */
uint64_t host_unix_time_us(void);            /* microseconds since 1970 */
void host_sleep_ms(uint32_t ms);             /* cede el procesador */

#endif
