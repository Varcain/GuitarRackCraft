// AhbChannelProtocol.h — Phase 2 of the GPU X-server upgrade.
//
// A private control channel for passing AHardwareBuffer handles + sync-fds
// between a frame producer (the wine winex11.drv Vulkan present path, Phase 3 —
// or a synthetic test client) and the in-process X11 server's GLES compositor,
// DECOUPLED from the TCP X11 protocol socket. SCM_RIGHTS fd-passing is
// impossible on the custom-libxcb TCP socket, so AHB/fence handles travel here.
//
// Transport: AF_UNIX SOCK_STREAM, abstract namespace ("\0guitarrack-ahb-<N>"),
// one socket per X11 display number. Additive + independently disableable; does
// NOT touch the TCP path. Keyed by X11 window id (both sides already know it).
//
// This header is intentionally plain C (fixed-width types, static-inline socket
// helpers, no C++/namespaces) so it can be vendored verbatim into the wine tree
// for the Phase 3 producer.
#ifndef GUITARRACKCRAFT_X11_AHB_CHANNEL_PROTOCOL_H
#define GUITARRACKCRAFT_X11_AHB_CHANNEL_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* NOTE: this header deliberately does NOT include <android/hardware_buffer.h> —
 * the helpers below only use POSIX sockets, so the header is vendorable verbatim
 * into the wine tree (winex11.drv, which has bionic sockets but not the NDK
 * AHB headers). The AHB *handles* themselves travel via the NDK's
 * AHardwareBuffer_send/recvHandleFromUnixSocket, called by the endpoint code
 * (which includes/declares those itself), NOT by anything here. */

#ifdef __cplusplus
extern "C" {
#endif

#define AHBCH_MAGIC   0x41484243u  /* 'AHBC' */
#define AHBCH_VERSION 1u

enum AhbChMsgType {
    AHBCH_REGISTER   = 1, /* client->server, followed by buffer_count AHB handles
                           * (each via AHardwareBuffer_sendHandleToUnixSocket) */
    AHBCH_PRESENT    = 2, /* client->server, has_fence=1 => one sync-fd via SCM_RIGHTS */
    AHBCH_UNREGISTER = 3, /* client->server */
    AHBCH_RELEASE    = 4, /* server->client: buffer_index is free to reuse */
};

/* Fixed 40-byte message header. Host-local socket → native byte order (both
 * ends are LE ARM64/x86_64). Field meaning depends on `type`. */
typedef struct AhbChMsg {
    uint32_t magic;        /* AHBCH_MAGIC */
    uint16_t version;      /* AHBCH_VERSION */
    uint16_t type;         /* enum AhbChMsgType */
    uint32_t window_id;    /* X11 window id the AHB(s) back */
    uint32_t width;        /* REGISTER: buffer width  */
    uint32_t height;       /* REGISTER: buffer height */
    uint32_t format;       /* REGISTER: AHARDWAREBUFFER_FORMAT_* */
    uint32_t buffer_count; /* REGISTER: number of AHB handles that follow */
    uint32_t buffer_index; /* PRESENT / RELEASE: which buffer in the ring */
    uint32_t has_fence;    /* PRESENT: 1 => one sync-fd follows via SCM_RIGHTS */
    uint32_t reserved;     /* pad to 40 / future use */
} AhbChMsg;

/* Fill `addr` with the abstract-namespace address for display N. Returns the
 * socklen_t to pass to bind()/connect() (covers the leading NUL + name, NOT the
 * whole sun_path — abstract sockets are length-delimited, not NUL-delimited). */
static inline socklen_t ahbch_make_addr(struct sockaddr_un* addr, int display_number) {
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    char name[64];
    int n = snprintf(name, sizeof(name), "guitarrack-ahb-%d", display_number);
    if (n < 0) n = 0;
    if (n > (int)sizeof(name) - 1) n = (int)sizeof(name) - 1;
    addr->sun_path[0] = '\0';                 /* abstract namespace */
    memcpy(addr->sun_path + 1, name, (size_t)n);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

/* Send one message, optionally with a single fd attached via SCM_RIGHTS
 * (fence_fd < 0 => none). Returns 1 on success, 0 on failure. */
static inline int ahbch_send_msg(int fd, const AhbChMsg* msg, int fence_fd) {
    struct iovec iov;
    iov.iov_base = (void*)msg;
    iov.iov_len  = sizeof(*msg);
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov    = &iov;
    mh.msg_iovlen = 1;
    char cbuf[CMSG_SPACE(sizeof(int))];
    if (fence_fd >= 0) {
        memset(cbuf, 0, sizeof(cbuf));
        mh.msg_control    = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        struct cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type  = SCM_RIGHTS;
        c->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fence_fd, sizeof(int));
    }
    ssize_t r = sendmsg(fd, &mh, 0);
    return r == (ssize_t)sizeof(*msg) ? 1 : 0;
}

/* Receive one message. If a fence fd arrives via SCM_RIGHTS it is stored in
 * *out_fence_fd (the caller owns + must close it); otherwise *out_fence_fd = -1.
 * Returns 1 on success, 0 on EOF/error. */
static inline int ahbch_recv_msg(int fd, AhbChMsg* msg, int* out_fence_fd) {
    if (out_fence_fd) *out_fence_fd = -1;
    struct iovec iov;
    iov.iov_base = msg;
    iov.iov_len  = sizeof(*msg);
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov    = &iov;
    mh.msg_iovlen = 1;
    char cbuf[CMSG_SPACE(sizeof(int))];
    mh.msg_control    = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    ssize_t r = recvmsg(fd, &mh, MSG_WAITALL);
    if (r != (ssize_t)sizeof(*msg)) return 0;
    struct cmsghdr* c;
    for (c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int got;
            memcpy(&got, CMSG_DATA(c), sizeof(int));
            if (out_fence_fd) *out_fence_fd = got; else if (got >= 0) close(got);
            break;
        }
    }
    return 1;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* GUITARRACKCRAFT_X11_AHB_CHANNEL_PROTOCOL_H */
