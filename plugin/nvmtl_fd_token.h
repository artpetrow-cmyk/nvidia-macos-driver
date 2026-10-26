/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef NVMTL_FD_TOKEN_H
#define NVMTL_FD_TOKEN_H
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

/* The RM bridge uses a read-only null descriptor as an owned handle number.
 * If the host sandbox forbids that path, an anonymous pipe at EOF has the
 * same read-only behavior. No inherited descriptor or filesystem access is
 * borrowed. Other paths, access modes and failures retain normal semantics. */
static int nvmtl_icd_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args; va_start(args, flags); mode = (mode_t)va_arg(args, int); va_end(args);
    }
    /* sbx4: a way the sandbox refused once is not tried again - every refusal is a kernel sandbox report, and NVK opens
     * one of these per RM connection (WebKit's GPU process logged 326 /dev/null and 326 pipe refusals in 30 minutes) */
    static int refused;   /* bit 1: /dev/null, bit 2: pipe() */
    const int nullHandle = path && !strcmp(path, "/dev/null") && (flags & ~O_CLOEXEC) == O_RDONLY;
    if (nullHandle && (__atomic_load_n(&refused, __ATOMIC_RELAXED) & 3) == 3)
        return open("/dev/random", O_RDONLY | (flags & O_CLOEXEC));
    int fd = (nullHandle && (__atomic_load_n(&refused, __ATOMIC_RELAXED) & 1)) ? (errno = EPERM, -1) : open(path, flags, mode);
    if (fd >= 0 || !nullHandle || (errno != EACCES && errno != EPERM)) return fd;
    __atomic_fetch_or(&refused, 1, __ATOMIC_RELAXED);
    int ends[2];
    if (pipe(ends)) {
        /* WebKit's GPU process refuses /dev/null and pipe() alike; it may read /dev/random (sbx1). The descriptor is
         * still only a handle number: nothing reads from it. */
        __atomic_fetch_or(&refused, 2, __ATOMIC_RELAXED);
        return open("/dev/random", O_RDONLY | (flags & O_CLOEXEC));
    }
    close(ends[1]);
    if ((flags & O_CLOEXEC) && fcntl(ends[0], F_SETFD, FD_CLOEXEC) < 0) {
        int error = errno; close(ends[0]); errno = error; return -1;
    }
    return ends[0];
}
#endif
