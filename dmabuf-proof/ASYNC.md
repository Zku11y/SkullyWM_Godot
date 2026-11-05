Asynchronous DMA-BUF copies and sync-file ownership

The default controlled proof now uses protocol v2, with explicit acquire/release
sync-files and a single asynchronous GPU-copy job. Frame pixels still originate
in the standalone Vulkan producer, not a Wayland application. This milestone
removes the render-thread completion wait needed before connecting client data.

**Run**

Use the prepared dependencies from [README.md](README.md). From the repository
root, build the four-patch engine, extension and producer:

```sh
python3 engine/build.py --jobs 6
cmake --build godot-bridge/build -j4
cmake --build dmabuf-proof/build -j4
python3 tests/test_dmabuf_proof.py --frames 8
python3 tests/test_dmabuf_proof.py --frames 8 --no-readback
```

The graphical test chooses AMD/GPU index 0 on the verified machine. It runs a
finite number of frames and preserves evidence under the printed /tmp directory.
No compositor is installed or restarted. Existing SHM editor controls remain
unchanged. `--legacy --frames 4` exercises the original v1/synchronous path on
the current engine as a regression control.

**How a frame moves**

1. The producer submits its compute render and exports a binary semaphore's
   payload as a sync-file. It sends metadata, a DMA-BUF FD and, when needed, an
   acquire sync-file FD. It does **not** wait on its rendering fence before
   sending v2 frames. An FD flag of zero represents Vulkan's already-signalled
   sync-file sentinel, not a missing promise to finish writing.
2. The receiver's socket worker validates all descriptors, checks that the
   acquire descriptor is actually a sync-file with `SYNC_IOC_FILE_INFO`, and
   waits for readiness using cancellable 25 ms polls. It rejects an errored fence
   or a ten-second readiness timeout. This wait happens off Godot's main/render
   threads, before importing/publishing the source. Godot's queue never receives
   a wait on an unready, peer-controlled semaphore.
3. On the render thread, `BridgeDMABufReceiver.submit_next()` creates/binds an
   imported Vulkan image, then calls the engine's
   `texture_external_snapshot_submit(image, memory, width, height)`. Successful
   submission returns a ticket and **adopts both image and memory**. On failure
   the extension keeps ownership and destroys them. The extension cannot free
   an adopted import while the GPU is still using it.
4. The engine creates a private Godot destination, records ownership/layout
   transitions and the GPU copy, and submits under its real queue mutex. It
   returns immediately after submission. `poll_copy()` calls
   `texture_external_snapshot_poll(ticket)` on later frames; `vkGetFenceStatus`
   checks completion without waiting. The destination RID stays private until
   GPU completion, when Godot's texture tracking is initialized and the RID is
   returned for `Texture2DRD` display.
5. On completion, the engine exports its release semaphore to a sync-file,
   frees the completed command/import resources, and the socket worker sends
   ACK2 with the release FD. The producer imports that FD with
   `VK_SEMAPHORE_IMPORT_TEMPORARY_BIT`, and its next GPU write waits on it.
   Successful Vulkan import consumes that owned FD. The producer also observes
   its own previous submission fence before resetting/reimporting Vulkan
   resources; a remote ACK alone is not a Vulkan host synchronization call.

The current ACK is deliberately sent only after copy completion. Its release
fence is therefore already complete, but transferring and consuming it proves
the ownership contract for later pipelining. It does not claim fully GPU-side
overlap, real client syncobj integration, or a production latency result.

```text
producer submission ── export acquire sync-file + DMA-BUF ──► socket worker
                                                            │ readiness poll
                                                            ▼
render thread ── import + submit ──► engine owns one in-flight job
                     returns                │ later fence-status polls
                                            ▼
                        completed RID ──► Texture2DRD ──► 2D / mesh
                                            │
producer next write ◄── imported release FD ◄┴── ACK2 after completion
```

**Bounds and cancellation**

There is one source pending on the receiver, and at most one external GPU-copy
job per RenderingDevice. The producer waits for each completion reply. This
preserves the existing 2048² dimension, 64 MiB allocation, one-plane LINEAR RGBA8,
same-GPU/driver and 32-frame limits. No backlog of GPU jobs is permitted.
Each frame still allocates a fresh destination; texture pooling is future work.

`stop()` cancels socket/acquire waits, closes pending owned FDs and joins the
socket worker. On the render thread, `discard_copy(rd)` tells the engine to
discard the ticket. Discarding never frees unfinished GPU resources: the engine
polls them during subsequent frames or explicit retirement checks, then frees
the import and destination after completion. Render cleanup remains possible
when no window redraw is scheduled. Normal engine finalization waits for any
remaining owned job before destroying textures/device; that shutdown wait is
separate from the frame submission/poll path.

Callers must stop reception and discard any outstanding ticket before restarting
or dropping a preview. This finite demo performs that sequence. A production
editor component still needs dedicated scene reload, project close, plugin
disable, reconnect and engine-exit tests. GPU-hang recovery is not implemented;
engine finalization can still wait for a malfunctioning GPU, like normal Vulkan
device shutdown. Neither source locks nor FD lifetime provide that recovery.

**Protocol v2**

The first 96 bytes retain the v1 field offsets. Version becomes 2, header size
104. At offset 96 is a network-order 32-bit acquire-FD count (0 or 1); offset
100 is reserved and must be zero. Exactly one DMA-BUF FD comes first, followed
by an acquire sync-file FD when the count is 1. No whole image is put in a
SOCK_SEQPACKET packet.

ACK2 is 20 bytes: magic `0x41434b32`, 32-bit status, 64-bit frame ID, 32-bit
release-FD count (0 or 1), all network order. A release FD is attached with
SCM_RIGHTS when the count is 1. Count 0 represents the already-signalled sentinel.
V1 remains accepted with its 96-byte header and 16-byte ACK1.

Kernel-received descriptors are owned immediately, including descriptors on an
empty packet. Malformed/truncated messages, unexpected counts, wrong GPU UUIDs
and invalid acquire descriptor types are rejected and owned FDs closed. No raw
FD integer or foreign-process Vulkan image is transmitted as a usable handle.

**Verification and scope**

The runner supports the existing malformed-message/disconnect cases plus:

```sh
python3 tests/test_dmabuf_proof.py --fault bad-fence
python3 tests/test_dmabuf_proof.py --fault delayed
python3 tests/test_dmabuf_proof.py --fault cancel-acquire
python3 tests/test_dmabuf_proof.py --fault cancel-submit
```

`delayed` and `cancel-acquire` set a test-only receiver environment variable,
`DWL_BRIDGE_TEST_DELAY_ACQUIRE=1`, which delays consuming a **real** acquire
sync-file for 700 ms on the worker. This provides deterministic cancellation and
main-loop responsiveness checks without placing an artificial unresolved
dependency on a GPU queue. It does not force that fence itself to remain
unsignalled. Normal rendering logs separately record whether the producer's
submission was pending before send. The hold is cancelled in 25 ms increments.

The normal test compares every byte of each copied destination with the expected
pattern and requires a release import per acknowledged v2 frame. No-readback
mode verifies rendered output without those destination readbacks. Cancel before
readiness requires zero copies, continued main-loop updates, and stop under
250 ms. Cancel after submission requires zero published frames and retirement
back to zero pending engine jobs. All cases require confirmed core/synchronization
validation and reject validation errors, script errors and reported RID leaks.
Evidence is in [the async report](../reports/dmabuf-async/README.md).

Next is a deliberately owned compositor export pool and one selected native
window in nested dwl. That needs format/modifier negotiation, the wlroots
renderer completion signal, cross-API queue ownership, and applied-state/client
release accounting. The current controlled allocation/import contract cannot be
applied blindly to arbitrary application or GBM buffers. SHM remains supported
through the existing separate path; automatic fallback between paths is future
work. NVIDIA import, cross-GPU transport and direct sampling remain unproven.
