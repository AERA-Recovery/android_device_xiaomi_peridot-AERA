/* SPDX-License-Identifier: Apache-2.0 */
/* Host regression test for the RMTFS read-only and Bionic FORTIFY guards. */
#include "../readonly-guard.c"
#include <assert.h>

static unsigned calls;
static bool readonly_used;
int __real_open(const char *path, int flags, ...)
{
    (void)path; (void)flags;
    ++calls;
    errno = ENOENT;
    return -1;
}
int __real_openat(int dirfd, const char *path, int flags, ...)
{
    (void)dirfd;
    return __real_open(path, flags);
}
int __real_storage_init(const char *root, bool ro, bool partitions)
{
    (void)root; (void)partitions;
    readonly_used = ro;
    return 0;
}
int main(void)
{
    const char *nv = "/dev/block/bootdevice/by-name/modemst1";
    assert(__wrap_open(nv, O_RDONLY) == -1 && calls == 1);
    assert(__wrap___open_2(nv, O_RDONLY) == -1 && calls == 2);
    assert(__wrap_open(nv, O_RDWR) == -1 && errno == EROFS && calls == 2);
    assert(__wrap_openat(AT_FDCWD, nv, O_WRONLY) == -1 && errno == EROFS && calls == 2);
    assert(__wrap___open_2(nv, O_TRUNC | O_RDONLY) == -1 && errno == EROFS && calls == 2);
    assert(__wrap___openat_2(AT_FDCWD, nv, O_CREAT | O_RDONLY) == -1 && errno == EROFS && calls == 2);
    assert(__wrap_pwrite(-1, "x", 1, 0) == -1 && errno == EROFS);
    __wrap_storage_init("unused", false, true);
    assert(readonly_used);
    puts("Readonly backing storage and Bionic FORTIFY guards passed");
    return 0;
}
