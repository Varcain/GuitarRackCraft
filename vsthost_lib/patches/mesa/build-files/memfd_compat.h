/* vstpoc: force-included (via -include) into the lavapipe/llvmpipe mesa build.
 *
 * With -DMESA_FORCE_LINUX, mesa's llvmpipe dma-buf path (lp_texture.c) calls
 * memfd_create(). The Bionic libc WRAPPER + its <sys/mman.h> declaration only
 * exist from Android API 30; we build the whole stack against API 28, so the
 * call is undeclared (fatal under -Werror=implicit-function-declaration) and
 * would also be an unresolved symbol against the API-28 libc stub.
 *
 * The memfd_create SYSCALL has existed since Linux 3.17, so we provide a
 * portable inline wrapper over it. This keeps the build at API 28 and produces
 * a .so that loads on any of our devices (no API-30 libc wrapper needed).
 *
 * Safe because at API 28 nothing else declares memfd_create (no conflict). If
 * the build API is ever raised to >= 30, <sys/mman.h> will declare it as a
 * normal extern function and this static-inline must be dropped.
 */
#ifndef VSTPOC_MEMFD_COMPAT_H
#define VSTPOC_MEMFD_COMPAT_H

#include <sys/syscall.h>
#include <unistd.h>
#include <linux/memfd.h> /* MFD_CLOEXEC / MFD_ALLOW_SEALING */

#ifndef __NR_memfd_create
#define __NR_memfd_create 279 /* arm64 */
#endif

static inline int memfd_create(const char *name, unsigned int flags)
{
    return (int)syscall(__NR_memfd_create, name, flags);
}

#endif /* VSTPOC_MEMFD_COMPAT_H */
