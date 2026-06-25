#include "shm_ringbuf.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <stdatomic.h>
#include <poll.h>

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
        return -1;
    }

    /* mmap the region */
    out_ctx->region = mmap(NULL, region_size, PROT_READ | PROT_WRITE, MAP_SHARED, out_ctx->fd, 0);
    if (out_ctx->region == MAP_FAILED) {
        perror("mmap");
        close(out_ctx->fd);
        return -1;
    }

    out_ctx->region_size = region_size;
    out_ctx->hdr = (shm_ringbuf_header_t *)out_ctx->region;
    out_ctx->buffer = (uint8_t *)out_ctx->region + sizeof(shm_ringbuf_header_t);

    /* Initialize header */
    memset(out_ctx->hdr, 0, sizeof(*out_ctx->hdr));
    out_ctx->hdr->magic = SHM_RINGBUF_MAGIC;
    out_ctx->hdr->version = 1;
    out_ctx->hdr->bufsize = bufsize;
    out_ctx->hdr->head = 0;
    out_ctx->hdr->tail = 0;

    /* Create eventfd for notifications */
    out_ctx->evtfd = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE | EFD_NONBLOCK);
    if (out_ctx->evtfd < 0) {
        perror("eventfd");
        munmap(out_ctx->region, region_size);
        close(out_ctx->fd);
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

    /* Get memfd size */
    off_t size = lseek(memfd, 0, SEEK_END);
    if (size <= 0) {
        perror("lseek");
        return -1;
    }

    /* mmap the region */
    void *region = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (region == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    out_ctx->fd = memfd;
    out_ctx->evtfd = evtfd;
    out_ctx->region = region;
    out_ctx->region_size = (size_t)size;
    out_ctx->hdr = (shm_ringbuf_header_t *)region;
    out_ctx->buffer = (uint8_t *)region + sizeof(shm_ringbuf_header_t);

    /* Validate header */
    if (out_ctx->hdr->magic != SHM_RINGBUF_MAGIC) {
        fprintf(stderr, "Invalid ring buffer magic\n");
        munmap(region, (size_t)size);
        errno = EINVAL;
        return -1;
    }

    return 0;
}

ssize_t shm_ringbuf_write(shm_ringbuf_t *ctx, const void *data, size_t len, bool notify)
{
    if (!ctx || !data || len == 0) return 0;

    size_t bufsize = ctx->hdr->bufsize;
    uint64_t head = atomic_load_explicit(&ctx->hdr->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&ctx->hdr->tail, memory_order_acquire);

    size_t free_space = bufsize - (head - tail);
    if (free_space < len) {
        errno = ENOSPC;
        return -1;  /* buffer full */
    }

    /* Write data in two parts if it wraps around */
    size_t idx = head & (bufsize - 1);
    if (idx + len <= bufsize) {
        /* Fits without wrapping */
        memcpy(ctx->buffer + idx, data, len);
    } else {
        /* Wraps around */
        size_t first_part = bufsize - idx;
        memcpy(ctx->buffer + idx, data, first_part);
        memcpy(ctx->buffer, (const uint8_t *)data + first_part, len - first_part);
    }

    /* Update head with release semantics */
    atomic_store_explicit(&ctx->hdr->head, head + len, memory_order_release);

    if (notify) {
        shm_ringbuf_notify(ctx);
    }

    return (ssize_t)len;
}

ssize_t shm_ringbuf_read(shm_ringbuf_t *ctx, void *out_buf, size_t max_len, int timeout_ms)
{
    if (!ctx || !out_buf || max_len == 0) return 0;

    size_t bufsize = ctx->hdr->bufsize;

    /* Wait for data with timeout */
    while (1) {
        uint64_t head = atomic_load_explicit(&ctx->hdr->head, memory_order_acquire);
        uint64_t tail = atomic_load_explicit(&ctx->hdr->tail, memory_order_acquire);

        size_t available = head - tail;
        if (available > 0) {
            /* Data available */
            size_t to_read = (available < max_len) ? available : max_len;
            size_t idx = tail & (bufsize - 1);

            if (idx + to_read <= bufsize) {
                memcpy(out_buf, ctx->buffer + idx, to_read);
            } else {
                size_t first_part = bufsize - idx;
                memcpy(out_buf, ctx->buffer + idx, first_part);
                memcpy((uint8_t *)out_buf + first_part, ctx->buffer, to_read - first_part);
            }

            atomic_store_explicit(&ctx->hdr->tail, tail + to_read, memory_order_release);
            return (ssize_t)to_read;
        }

        /* No data; wait on eventfd */
        struct pollfd pfd;
        pfd.fd = ctx->evtfd;
        pfd.events = POLLIN;

        int pret = poll(&pfd, 1, timeout_ms);
        if (pret < 0) {
            perror("poll");
            return -1;
        } else if (pret == 0) {
            /* Timeout */
            return 0;
        }

        /* eventfd signaled, loop to check data */
        uint64_t dummy;
        (void)read(ctx->evtfd, &dummy, sizeof(dummy)); /* drain eventfd */
    }
}

int shm_ringbuf_notify(shm_ringbuf_t *ctx)
{
    if (!ctx || ctx->evtfd < 0) return -1;
    uint64_t val = 1;
    ssize_t wr = write(ctx->evtfd, &val, sizeof(val));
    return (wr == sizeof(val)) ? 0 : -1;
}

size_t shm_ringbuf_available(shm_ringbuf_t *ctx)
{
    if (!ctx) return 0;
    uint64_t head = atomic_load_explicit(&ctx->hdr->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&ctx->hdr->tail, memory_order_acquire);
    return (size_t)(head - tail);
}

size_t shm_ringbuf_free(shm_ringbuf_t *ctx)
{
    if (!ctx) return 0;
    size_t bufsize = ctx->hdr->bufsize;
    return bufsize - shm_ringbuf_available(ctx);
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
    memset(ctx, 0, sizeof(*ctx));
}