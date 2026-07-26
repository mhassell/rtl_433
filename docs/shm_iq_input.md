# Shared-Memory IQ Input (`-J` / `shm_iq`)

This document describes the **memfd + eventfd SPSC ring buffer** transport that
allows a running gqrx instance (`feature/shm-ringbuf-namedmem` branch of
`mhassell/gqrx`) to stream raw IQ samples to rtl_433 (`fix/memfd-spsc` branch)
**without any kernel copy** — ideal for high-throughput captures at ≥10 Msps.

---

## Overview

```
┌─────────────────────────────────────────────────────────┐
│ gqrx (producer)                                         │
│  memfd_create("gqrx_iq_ringbuf", MFD_CLOEXEC)          │
│  eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK)               │
│  → writes CU8 IQ samples into ring buffer                │
│  → writes 1 to eventfd after each batch                  │
└────────────┬────────────────────────────┬───────────────┘
             │ memfd (mapped read/write)  │ eventfd
             │ passed via:                │ passed via:
             │  • inherited fds           │  • inherited fds
             │  • SCM_RIGHTS socket       │  • SCM_RIGHTS socket
             ▼                            ▼
┌─────────────────────────────────────────────────────────┐
│ rtl_433 (consumer)  -J fd:<m>:<e>  or  -J sock:<path>  │
│  shm_ringbuf_open() → validates header → mmap           │
│  poll(eventfd) → shm_ringbuf_read() → sdr_callback()    │
└─────────────────────────────────────────────────────────┘
```

---

## CLI Usage

### Option `-J` (long name: `shm_iq`)

```
rtl_433 -J fd:<memfd_num>:<evtfd_num>
rtl_433 -J sock:<unix_socket_path>
```

| Format | Description |
|--------|-------------|
| `fd:<memfd>:<evtfd>` | Use two **pre-opened file descriptors** passed from the producer (gqrx) — typical when rtl_433 is spawned as a child process by gqrx. |
| `sock:<path>` | Connect to the Unix-domain socket at `<path>` and receive the two fds via `SCM_RIGHTS` ancillary data (works when rtl_433 is started independently). |

---

## FD Handoff Mechanisms

### 1. Inherited file descriptors (child-process model)

If gqrx spawns rtl_433 as a child process, the memfd and eventfd survive the
`fork()`/`exec()` because they are **not** created with `FD_CLOEXEC` in the
producer or the `CLOEXEC` flag is stripped before the exec.  The producer then
passes the fd numbers through the environment or a command-line argument:

```sh
# gqrx side (conceptual launcher):
RTL433_MEMFD=5 RTL433_EVTFD=6 exec rtl_433 -J fd:5:6 [other options...]
```

### 2. SCM_RIGHTS over a Unix-domain socket (independent-process model)

gqrx listens on a Unix-domain socket (e.g. `/tmp/gqrx_shm_handoff.sock`) and
sends a `sendmsg()` with an `SCM_RIGHTS` control message containing the two
fds.  rtl_433 connects, receives the fds, and maps the shared memory:

```sh
rtl_433 -J sock:/tmp/gqrx_shm_handoff.sock [other options...]
```

The socket must be a `SOCK_SEQPACKET` or `SOCK_STREAM` Unix socket.
rtl_433 tries `SOCK_SEQPACKET` first and falls back to `SOCK_STREAM`.

---

## Shared-Memory Header Layout

The first `sizeof(shm_ringbuf_header_t)` = **96 bytes** of the `memfd` region
contain a fixed-width header (all fields little-endian on all supported platforms):

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | `magic` | `0x52494E47` ("RING") — validates the segment |
| 4 | 4 | `version` | Currently `1` |
| 8 | 8 | `head` | Producer write index (monotonically increasing) |
| 16 | 8 | `tail` | Consumer read index (monotonically increasing) |
| 24 | 8 | `bufsize` | Ring buffer capacity in bytes (must be a power of two) |
| 32 | 8 | `sample_rate` | Sample rate in Hz (`0` = unspecified; rtl_433 uses its default) |
| 40 | 8 | `sample_size` | Bytes per IQ sample pair (`2` = CU8, `4` = CS16) |
| 48 | 48 | `reserved` | Zero-filled; reserved for future use |

The ring buffer data immediately follows the header at offset 96.

`head` and `tail` are accessed with GCC `__atomic_*` built-ins using
acquire/release memory ordering for correct lock-free SPSC operation.

---

## Sample Format

gqrx writes **CU8** (unsigned 8-bit complex) interleaved I/Q samples using the
symmetric conversion:

```
I_out = clamp(I_float * 127.5 + 127.5, 0, 255)
Q_out = clamp(Q_float * 127.5 + 127.5, 0, 255)
```

where `I_float` and `Q_float` are normalised IQ values in the range **[-1.0, +1.0]**
(as produced by GNU Radio's complex float output).  The conversion maps -1.0 → 0,
0.0 → 127 (DC centre), +1.0 → 255.

Each sample pair is 2 bytes: `[I₀ Q₀ I₁ Q₁ …]`.

rtl_433 feeds these bytes directly into its normal `sdr_callback()` pipeline,
which handles CU8 natively (the same format used by RTL-SDR hardware and
`-r file.cu8` file inputs).

---

## Worked Example

### Scenario: gqrx spawns rtl_433 as a child (inherited fds)

```sh
# In gqrx's shm_iq_sink, after creating the memfd (fd=5) and eventfd (fd=6):
exec /usr/local/bin/rtl_433 \
    -J fd:5:6 \
    -f 433.92M \
    -R 0 -R 40 -R 41 \
    -F json:/tmp/rtl_433.json
```

### Scenario: rtl_433 running independently, receiving fds via socket

```sh
# Terminal 1 — start rtl_433 first (it will block waiting for the socket)
rtl_433 -J sock:/tmp/gqrx_shm_handoff.sock -f 433.92M -F json

# Terminal 2 — start gqrx; it will create the SHM, open the socket,
#              and send the fds once the ring buffer is ready
gqrx
```

gqrx listens on `/tmp/gqrx_shm_handoff.sock` and sends the fds via
`SCM_RIGHTS` as soon as the ring buffer is initialised.

---

## ZMQ Transport (alternative, lower-priority)

The existing `-Z <address>` flag enables a ZMQ `SUB` socket instead of
shared memory.  The address is now correctly used (previously hardcoded).

Example:
```sh
rtl_433 -Z ipc:///tmp/gqrx_iq.sock
```

> **Note:** ZMQ involves an extra kernel copy per message and can silently drop
> samples under back-pressure.  The shared-memory ring buffer (`-J`) is the
> recommended high-throughput transport for gqrx → rtl_433 IPC.

---

## Known Limitations and Follow-up Work

1. **Magic number must be coordinated**: The magic `0x52494E47` ("RING") and
   version `1` must be agreed upon by both gqrx and rtl_433.  Update the gqrx
   `shm_iq_sink` to write this exact magic before the ring buffer is considered
   valid by rtl_433.

2. **Sample format mismatch risk**: The gqrx branch must use the symmetric
   conversion `value * 127.5 + 127.5` (not `* 127.0 + 128.0`) to ensure
   correct DC centering.  Verify this on the gqrx side.

3. **CS16 not yet tested via SHM**: The header `sample_size=4` path is wired
   up but has not been exercised end-to-end.  The CF32 → CS16 conversion path
   used in the file-input loop is not replicated in the SHM path; add it if
   gqrx ever writes CS16.

4. **No reconnect logic**: If gqrx restarts, rtl_433 must be restarted too.
   The `sock:` path could be extended to poll for reconnect in the future.

5. **Integration test needed**: Once both branches converge, run a loopback
   test (producer writes known IQ data, consumer decodes an expected packet)
   to validate the full pipeline.
