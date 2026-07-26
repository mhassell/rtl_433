#ifndef SHM_RINGBUF_H
#define SHM_RINGBUF_H

/**
 * shm_ringbuf.h - Shared memory SPSC (Single Producer, Single Consumer) ring buffer
 *
 * Lock-free, zero-copy ring buffer using memfd_create + mmap for high-performance
 * IPC on the same host.  Producer signals the consumer via an eventfd.
 *
 * Intended interop: gqrx feature/shm-ringbuf-namedmem branch acts as producer,
 * rtl_433 fix/memfd-spsc branch acts as consumer.  File descriptors are handed
 * from producer to consumer either via inherited fds (child process) or via a
 * Unix-domain socket using SCM_RIGHTS ancillary data.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>   /* ssize_t */
#include <sys/eventfd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic number to identify valid shared segments.
 * Must match the value used on the producer side (gqrx feature/shm-ringbuf-namedmem). */
#define SHM_RINGBUF_MAGIC   0x52494E47u  /* "RING" */
#define SHM_RINGBUF_VERSION 1

/* Ring buffer header (at offset 0 in the shared region).
 *
 * Field layout (all fixed-width) -- this MUST byte-for-byte match the
 * producer-side struct in gqrx's include/shm_ringbuf.h (feature/shm-ringbuf-namedmem):
 *   magic       - 4 bytes  (SHM_RINGBUF_MAGIC = 0x52494E47), offset  0
 *   version     - 4 bytes  (SHM_RINGBUF_VERSION = 1),        offset  4
 *   head        - 8 bytes  (producer write index, monotonically increasing), offset  8
 *   tail        - 8 bytes  (consumer read index, monotonically increasing),  offset 16
 *   bufsize     - 8 bytes  (ring buffer capacity, must be a power of two),   offset 24
 *   sample_rate - 4 bytes  (sample rate in Hz, 0 if unknown),                offset 32
 *   sample_size - 4 bytes  (bytes per IQ sample pair: 2=CU8, 4=CS16),        offset 36
 *
 * Total: 40 bytes, 8-byte aligned.
 *
 * NOTE: sample_rate/sample_size are uint32_t (not uint64_t) and there is no
 * trailing reserved padding -- both are required to match gqrx's producer
 * layout exactly, otherwise shm_ringbuf_open()'s bufsize/region-size sanity
 * check will fail (sizeof(shm_ringbuf_header_t) would disagree between the
 * two sides, causing the consumer to misinterpret where ring data begins).
 *
 * head and tail are accessed via __atomic_load_n / __atomic_store_n with
 * acquire/release ordering for correct lock-free SPSC operation.
 */
typedef struct {
    uint32_t magic;         /* validation magic (SHM_RINGBUF_MAGIC) */
    uint32_t version;       /* format version (SHM_RINGBUF_VERSION) */
    volatile uint64_t head; /* producer write index (monotonically increasing) */
    volatile uint64_t tail; /* consumer read index (monotonically increasing) */
    /* Note: head/tail are accessed via __atomic_load_n/__atomic_store_n with acquire/release
     * ordering (see shm_ringbuf.c).  The 'volatile' qualifier is kept as a secondary hint
     * to the compiler that these locations are modified by an external process sharing the
     * same mapped memory, preventing unintended load/store optimisation. */
    uint64_t bufsize;       /* ring buffer capacity in bytes (power of two) */
    uint32_t sample_rate;   /* sample rate in Hz (0 = unspecified) */
    uint32_t sample_size;   /* bytes per IQ sample pair (2=CU8, 4=CS16) */
} shm_ringbuf_header_t;

/* Shared ring buffer context (consumer-side state, not shared) */
typedef struct {
    int fd;                    /* memfd file descriptor */
    int evtfd;                 /* eventfd for notifications */
    void *region;              /* mmap'd region base */
    size_t region_size;        /* total mmap'd size (header + buffer) */
    shm_ringbuf_header_t *hdr; /* pointer into region */
    uint8_t *buffer;           /* pointer to ring buffer data (after header) */
} shm_ringbuf_t;

/**
 * Create and initialize a new shared memory ring buffer (producer side).
 *
 * @param bufsize_bytes  desired buffer size (rounded to next power-of-two)
 * @param out_ctx        filled with fd, evtfd, mmap info
 * @return 0 on success, -1 on error (errno set)
 */
int shm_ringbuf_create(size_t bufsize_bytes, shm_ringbuf_t *out_ctx);

/**
 * Open an existing shared memory ring buffer (consumer side).
 * Validates the header magic and version before returning.
 *
 * @param memfd    memfd file descriptor (inherited or received via SCM_RIGHTS)
 * @param evtfd    eventfd file descriptor (inherited or received via SCM_RIGHTS)
 * @param out_ctx  filled with mmap info
 * @return 0 on success, -1 on error (errno set; EINVAL for magic/version mismatch)
 */
int shm_ringbuf_open(int memfd, int evtfd, shm_ringbuf_t *out_ctx);

/**
 * Receive the memfd and eventfd from a Unix-domain socket using SCM_RIGHTS.
 * The producer (gqrx) must have sent them via sendmsg() with SCM_RIGHTS.
 *
 * @param sock_path  path to the Unix-domain socket (SOCK_SEQPACKET or SOCK_STREAM)
 * @param out_memfd  receives the memfd
 * @param out_evtfd  receives the eventfd
 * @return 0 on success, -1 on error
 */
int shm_ringbuf_receive_fds(const char *sock_path, int *out_memfd, int *out_evtfd);

/**
 * Append data to the ring buffer (producer).
 *
 * @param ctx     ring buffer context
 * @param data    data to append
 * @param len     number of bytes to append
 * @param notify  if true, signal eventfd after append
 * @return number of bytes written, or -1 on error (ENOSPC = buffer full)
 */
ssize_t shm_ringbuf_write(shm_ringbuf_t *ctx, const void *data, size_t len, bool notify);

/**
 * Read up to max_len bytes from the ring buffer (consumer).
 * Blocks until data is available, timeout expires, or an error occurs.
 * Handles spurious wakeups and wraparound transparently.
 *
 * @param ctx        ring buffer context
 * @param out_buf    buffer to fill
 * @param max_len    maximum bytes to read
 * @param timeout_ms -1 = block indefinitely, 0 = non-blocking, >0 = timeout ms
 * @return bytes read (>0), 0 on timeout/no-data, -1 on error
 */
ssize_t shm_ringbuf_read(shm_ringbuf_t *ctx, void *out_buf, size_t max_len, int timeout_ms);

/**
 * Signal the eventfd to wake the consumer (use after batched writes with notify=false).
 */
int shm_ringbuf_notify(shm_ringbuf_t *ctx);

/**
 * Return the number of bytes currently available for reading.
 */
size_t shm_ringbuf_available(shm_ringbuf_t *ctx);

/**
 * Return the number of bytes of free space remaining in the buffer.
 */
size_t shm_ringbuf_free(shm_ringbuf_t *ctx);

/**
 * Close and release all resources (munmap + close fds). Safe to call multiple times.
 */
void shm_ringbuf_close(shm_ringbuf_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* SHM_RINGBUF_H */
