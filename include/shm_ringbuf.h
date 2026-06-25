#ifndef SHM_RINGBUF_H
#define SHM_RINGBUF_H

/**
 * shm_ringbuf.h - Shared memory SPSC (Single Producer, Single Consumer) ring buffer
 * 
 * Lock-free, zero-copy ring buffer using memfd_create + mmap for high-performance
 * IPC on the same host. Notifications via eventfd.
 * 
 * Producer: appends data and signals eventfd.
 * Consumer: waits on eventfd, reads data, updates tail index.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/eventfd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic number to identify valid shared segments */
#define SHM_RINGBUF_MAGIC 0x12345678

/* Ring buffer header (at offset 0 in the shared region) */
typedef struct {
    uint32_t magic;           /* validation magic */
    uint32_t version;         /* format version (for future compat) */
    uint64_t bufsize;         /* power-of-two ring buffer size (bytes) */
    uint64_t head;            /* producer index (atomic) */
    uint64_t tail;            /* consumer index (atomic) */
    uint64_t padding[8];      /* reserved for future fields */
} shm_ringbuf_header_t;

/* Shared ring buffer context (client-side) */
typedef struct {
    int fd;                   /* memfd file descriptor */
    int evtfd;                /* eventfd for notifications */
    void *region;             /* mmap'd region base */
    size_t region_size;       /* total mmap'd size (header + buffer) */
    shm_ringbuf_header_t *hdr; /* pointer to header */
    uint8_t *buffer;          /* pointer to ring buffer data */
} shm_ringbuf_t;

/**
 * Create and initialize a new shared memory ring buffer (producer side).
 * 
 * @param bufsize_bytes   desired buffer size (will be rounded to next power-of-two)
 * @param out_ctx         output context (will be filled with fd, mmap, etc.)
 * @return 0 on success, -1 on error (check errno)
 */
int shm_ringbuf_create(size_t bufsize_bytes, shm_ringbuf_t *out_ctx);

/**
 * Open an existing shared memory ring buffer (consumer side).
 * Assumes the producer has created it and the fd/path is available.
 * For now, we use an inherited fd approach (producer passes fd or stores in env).
 * 
 * @param memfd           file descriptor of the memfd (passed from producer or via env)
 * @param evtfd           file descriptor of the eventfd (passed from producer or via env)
 * @param out_ctx         output context
 * @return 0 on success, -1 on error
 */
int shm_ringbuf_open(int memfd, int evtfd, shm_ringbuf_t *out_ctx);

/**
 * Append data to the ring buffer (producer).
 * 
 * @param ctx             ring buffer context
 * @param data            data to append
 * @param len             number of bytes to append
 * @param notify          if true, signal eventfd after append (batching: caller can pass false, then call shm_ringbuf_notify)
 * @return number of bytes written, or -1 on error (buffer full, etc.)
 */
ssize_t shm_ringbuf_write(shm_ringbuf_t *ctx, const void *data, size_t len, bool notify);

/**
 * Consume (read) up to max_len bytes from the ring buffer (consumer).
 * Blocks until data available or timeout. Updates tail index.
 * 
 * @param ctx             ring buffer context
 * @param out_buf         buffer to fill with data
 * @param max_len         maximum bytes to read
 * @param timeout_ms      timeout in milliseconds (-1 = wait indefinitely, 0 = non-blocking)
 * @return number of bytes read, 0 if timeout, -1 on error
 */
ssize_t shm_ringbuf_read(shm_ringbuf_t *ctx, void *out_buf, size_t max_len, int timeout_ms);

/**
 * Signal the eventfd to wake the consumer (useful for batched writes).
 */
int shm_ringbuf_notify(shm_ringbuf_t *ctx);

/**
 * Get the current number of bytes available for reading.
 */
size_t shm_ringbuf_available(shm_ringbuf_t *ctx);

/**
 * Get the current free space in the buffer.
 */
size_t shm_ringbuf_free(shm_ringbuf_t *ctx);

/**
 * Close and cleanup (munmap, close fds, etc.). Safe to call multiple times.
 */
void shm_ringbuf_close(shm_ringbuf_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SHM_RINGBUF_H */