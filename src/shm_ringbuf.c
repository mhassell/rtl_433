#include "shm_ringbuf.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>

/*
 * Atomic helpers using GCC built-ins (__atomic_*) which work in C99.
 * We use acquire/release semantics for correct SPSC ordering:
 *   - Producer: reads tail with acquire, writes head with release.
 *   - Consumer: reads head with acquire, writes tail with release.
 */
#define ATOMIC_LOAD_ACQUIRE(ptr)   __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
#define ATOMIC_STORE_RELEASE(ptr, val) __atomic_store_n((ptr), (val), __ATOMIC_RELEASE)
#define ATOMIC_STORE_RELAXED(ptr, val) __atomic_store_n((ptr), (val), __ATOMIC_RELAXED)
#define ATOMIC_LOAD_RELAXED(ptr)   __atomic_load_n((ptr), __ATOMIC_RELAXED)

/* Round up to the next power of two */
static size_t round_up_pow2(size_t v)
{
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

int shm_ringbuf_create(size_t bufsize_bytes, shm_ringbuf_t *out_ctx)
{
    if (!out_ctx) {
        errno = EINVAL;
        return -1;
    }

    memset(out_ctx, 0, sizeof(*out_ctx));
    out_ctx->fd    = -1;
    out_ctx->evtfd = -1;

    /* Round buffer size to power of two */
    size_t bufsize = round_up_pow2(bufsize_bytes);
    if (bufsize == 0) bufsize = 4096;

    size_t region_size = sizeof(shm_ringbuf_header_t) + bufsize;

    /* Create memfd */
    out_ctx->fd = memfd_create("rtl433_iq_ringbuf", MFD_CLOEXEC);
    if (out_ctx->fd < 0) {
        perror("memfd_create");
        return -1;
    }

    /* Resize memfd to hold header + buffer */
    if (ftruncate(out_ctx->fd, (off_t)region_size) < 0) {
        perror("ftruncate");
        close(out_ctx->fd);
        out_ctx->fd = -1;
        return -1;
    }

    /* mmap the region */
    out_ctx->region = mmap(NULL, region_size, PROT_READ | PROT_WRITE, MAP_SHARED, out_ctx->fd, 0);
    if (out_ctx->region == MAP_FAILED) {
        perror("mmap");
        close(out_ctx->fd);
        out_ctx->fd = -1;
        out_ctx->region = NULL;
        return -1;
    }

    out_ctx->region_size = region_size;
    out_ctx->hdr    = (shm_ringbuf_header_t *)out_ctx->region;
    out_ctx->buffer = (uint8_t *)out_ctx->region + sizeof(shm_ringbuf_header_t);

    /* Initialize header */
    memset(out_ctx->hdr, 0, sizeof(*out_ctx->hdr));
    out_ctx->hdr->magic       = SHM_RINGBUF_MAGIC;
    out_ctx->hdr->version     = SHM_RINGBUF_VERSION;
    out_ctx->hdr->bufsize     = bufsize;
    /* head/tail start at 0 (memset above handles it, but be explicit) */
    ATOMIC_STORE_RELAXED(&out_ctx->hdr->head, (uint64_t)0);
    ATOMIC_STORE_RELAXED(&out_ctx->hdr->tail, (uint64_t)0);

    /* Create eventfd for notifications (EFD_SEMAPHORE: each read decrements by 1) */
    out_ctx->evtfd = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE | EFD_NONBLOCK);
    if (out_ctx->evtfd < 0) {
        perror("eventfd");
        munmap(out_ctx->region, region_size);
        close(out_ctx->fd);
        out_ctx->region = NULL;
        out_ctx->fd     = -1;
        return -1;
    }

    return 0;
}

int shm_ringbuf_open(int memfd, int evtfd, shm_ringbuf_t *out_ctx)
{
    if (!out_ctx || memfd < 0 || evtfd < 0) {
        errno = EINVAL;
        return -1;
    }

    memset(out_ctx, 0, sizeof(*out_ctx));
    out_ctx->fd    = -1;
    out_ctx->evtfd = -1;

    /* Determine memfd size */
    off_t size = lseek(memfd, 0, SEEK_END);
    if (size <= (off_t)sizeof(shm_ringbuf_header_t)) {
        fprintf(stderr, "shm_ringbuf: memfd too small (%" PRId64 " bytes)\n", (int64_t)size);
        errno = EINVAL;
        return -1;
    }

    /* mmap the region */
    void *region = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (region == MAP_FAILED) {
        perror("shm_ringbuf: mmap");
        return -1;
    }

    shm_ringbuf_header_t *hdr = (shm_ringbuf_header_t *)region;

    /* Validate header */
    if (hdr->magic != SHM_RINGBUF_MAGIC) {
        fprintf(stderr, "shm_ringbuf: invalid magic 0x%08X (expected 0x%08X)\n",
                hdr->magic, SHM_RINGBUF_MAGIC);
        munmap(region, (size_t)size);
        errno = EINVAL;
        return -1;
    }
    if (hdr->version != SHM_RINGBUF_VERSION) {
        fprintf(stderr, "shm_ringbuf: version mismatch: got %u, expected %u "
                "(ensure both gqrx and rtl_433 use the same protocol version)\n",
                hdr->version, SHM_RINGBUF_VERSION);
        munmap(region, (size_t)size);
        errno = EINVAL;
        return -1;
    }
    /* Sanity: bufsize must be power-of-two and fit within the mapped region */
    uint64_t bs = hdr->bufsize;
    if (bs == 0 || (bs & (bs - 1)) != 0 ||
        (size_t)size < sizeof(shm_ringbuf_header_t) + (size_t)bs) {
        fprintf(stderr, "shm_ringbuf: invalid bufsize %" PRIu64
                " (must be a power of two and fit within the memfd region)\n", bs);
        munmap(region, (size_t)size);
        errno = EINVAL;
        return -1;
    }

    /* All validation passed — store the fds in the context (ownership transfers to caller
     * of shm_ringbuf_close).  Note: fds are NOT stored before this point, so on any
     * failure above the caller retains ownership and must close them. */
    out_ctx->fd          = memfd;
    out_ctx->evtfd       = evtfd;
    out_ctx->region      = region;
    out_ctx->region_size = (size_t)size;
    out_ctx->hdr         = hdr;
    out_ctx->buffer      = (uint8_t *)region + sizeof(shm_ringbuf_header_t);

    return 0;
}

int shm_ringbuf_receive_fds(const char *sock_path, int *out_memfd, int *out_evtfd)
{
    if (!sock_path || !out_memfd || !out_evtfd) {
        errno = EINVAL;
        return -1;
    }

    *out_memfd = -1;
    *out_evtfd = -1;

    /* Must match the socket type the producer (gqrx shm_ringbuf_send_fds)
     * binds/listens with -- gqrx always uses SOCK_STREAM. Connecting with a
     * mismatched type (e.g. SOCK_SEQPACKET) fails with EPROTOTYPE
     * ("Protocol wrong type for socket"), since AF_UNIX enforces that both
     * ends of a connection use the same socket type. */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("shm_ringbuf: socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0'; /* ensure null termination */

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "shm_ringbuf: connect(%s): %s\n", sock_path, strerror(errno));
        close(sock);
        return -1;
    }

    /* Receive two fds via SCM_RIGHTS */
    char cmsgbuf[CMSG_SPACE(2 * sizeof(int))];
    char databuf[1];

    struct iovec iov = {
        .iov_base = databuf,
        .iov_len  = sizeof(databuf)
    };
    struct msghdr msg = {0};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    if (recvmsg(sock, &msg, 0) < 0) {
        fprintf(stderr, "shm_ringbuf: recvmsg: %s\n", strerror(errno));
        close(sock);
        return -1;
    }
    close(sock);

    if (msg.msg_controllen < CMSG_SPACE(2 * sizeof(int))) {
        fprintf(stderr, "shm_ringbuf: SCM_RIGHTS message too short (got %zu bytes, need %zu)\n",
                (size_t)msg.msg_controllen, (size_t)CMSG_SPACE(2 * sizeof(int)));
        errno = EINVAL;
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        fprintf(stderr, "shm_ringbuf: expected SCM_RIGHTS control message\n");
        errno = EINVAL;
        return -1;
    }

    /* Verify exactly 2 file descriptors were received */
    size_t expected_len = CMSG_LEN(2 * sizeof(int));
    if (cmsg->cmsg_len != expected_len) {
        fprintf(stderr, "shm_ringbuf: SCM_RIGHTS payload wrong size "
                "(got %zu bytes, expected %zu — need exactly 2 fds)\n",
                (size_t)cmsg->cmsg_len, expected_len);
        /* Close all fds that were actually received to prevent leaks */
        size_t n_received = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < n_received; ++i) {
            int tmp;
            memcpy(&tmp, (char *)CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
            close(tmp);
        }
        errno = EINVAL;
        return -1;
    }

    int fds[2];
    memcpy(fds, CMSG_DATA(cmsg), 2 * sizeof(int));
    *out_memfd = fds[0];
    *out_evtfd = fds[1];
    return 0;
}

ssize_t shm_ringbuf_write(shm_ringbuf_t *ctx, const void *data, size_t len, bool notify)
{
    if (!ctx || !data || len == 0) return 0;

    size_t bufsize = (size_t)ctx->hdr->bufsize;
    /* Producer owns head; load tail with acquire to see consumer's updates. */
    uint64_t head = ATOMIC_LOAD_RELAXED(&ctx->hdr->head);
    uint64_t tail = ATOMIC_LOAD_ACQUIRE(&ctx->hdr->tail);

    size_t free_space = bufsize - (size_t)(head - tail);
    if (free_space < len) {
        errno = ENOSPC;
        return -1;
    }

    /* Write data, handling wraparound */
    size_t idx = (size_t)(head & (bufsize - 1));
    if (idx + len <= bufsize) {
        memcpy(ctx->buffer + idx, data, len);
    } else {
        size_t first_part = bufsize - idx;
        memcpy(ctx->buffer + idx, data, first_part);
        memcpy(ctx->buffer, (const uint8_t *)data + first_part, len - first_part);
    }

    /* Publish new head with release so consumer sees the written data */
    ATOMIC_STORE_RELEASE(&ctx->hdr->head, head + len);

    if (notify) {
        shm_ringbuf_notify(ctx);
    }

    return (ssize_t)len;
}

ssize_t shm_ringbuf_read(shm_ringbuf_t *ctx, void *out_buf, size_t max_len, int timeout_ms)
{
    if (!ctx || !out_buf || max_len == 0) return 0;

    size_t bufsize = (size_t)ctx->hdr->bufsize;

    for (;;) {
        /* Consumer reads head with acquire to see producer's data */
        uint64_t head = ATOMIC_LOAD_ACQUIRE(&ctx->hdr->head);
        uint64_t tail = ATOMIC_LOAD_RELAXED(&ctx->hdr->tail);

        size_t available = (size_t)(head - tail);
        if (available > 0) {
            size_t to_read = (available < max_len) ? available : max_len;
            size_t idx = (size_t)(tail & (bufsize - 1));

            if (idx + to_read <= bufsize) {
                memcpy(out_buf, ctx->buffer + idx, to_read);
            } else {
                size_t first_part = bufsize - idx;
                memcpy(out_buf, ctx->buffer + idx, first_part);
                memcpy((uint8_t *)out_buf + first_part, ctx->buffer, to_read - first_part);
            }

            /* Publish new tail with release so producer sees freed space */
            ATOMIC_STORE_RELEASE(&ctx->hdr->tail, tail + to_read);
            return (ssize_t)to_read;
        }

        /* No data yet — wait on eventfd */
        struct pollfd pfd;
        pfd.fd     = ctx->evtfd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pret;
        do {
            pret = poll(&pfd, 1, timeout_ms);
        } while (pret < 0 && errno == EINTR);

        if (pret < 0) {
            /* Real poll error */
            return -1;
        }
        if (pret == 0) {
            /* Timeout */
            return 0;
        }

        /* Drain the eventfd counter (spurious wakeup: loop and recheck data) */
        if (pfd.revents & POLLIN) {
            uint64_t dummy;
            ssize_t r = read(ctx->evtfd, &dummy, sizeof(dummy));
            (void)r; /* EFD_SEMAPHORE decrements by 1; drain one credit */
        }
        /* Loop to recheck head/tail */
    }
}

int shm_ringbuf_notify(shm_ringbuf_t *ctx)
{
    if (!ctx || ctx->evtfd < 0) return -1;
    uint64_t val = 1;
    ssize_t wr = write(ctx->evtfd, &val, sizeof(val));
    return (wr == (ssize_t)sizeof(val)) ? 0 : -1;
}

size_t shm_ringbuf_available(shm_ringbuf_t *ctx)
{
    if (!ctx) return 0;
    uint64_t head = ATOMIC_LOAD_ACQUIRE(&ctx->hdr->head);
    uint64_t tail = ATOMIC_LOAD_RELAXED(&ctx->hdr->tail);
    return (size_t)(head - tail);
}

size_t shm_ringbuf_free(shm_ringbuf_t *ctx)
{
    if (!ctx) return 0;
    return (size_t)ctx->hdr->bufsize - shm_ringbuf_available(ctx);
}

void shm_ringbuf_close(shm_ringbuf_t *ctx)
{
    if (!ctx) return;
    if (ctx->region && ctx->region_size > 0) {
        munmap(ctx->region, ctx->region_size);
        ctx->region = NULL;
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
    if (ctx->evtfd >= 0) {
        close(ctx->evtfd);
        ctx->evtfd = -1;
    }
    ctx->region_size = 0;
    ctx->hdr         = NULL;
    ctx->buffer      = NULL;
}
