Controlled DMA-BUF → Godot proof

The default runner now uses the bounded asynchronous v2 path with real
acquire/release sync-files: see [ASYNC.md](ASYNC.md) for current ownership,
protocol and cancellation details. The v1 flow below documents the original
synchronous checkpoint and remains available with `--legacy`. Its historical
results and build hashes remain in `reports/dmabuf-proof/`; current results
are in `reports/dmabuf-async/`.

This milestone passes an actual DMA-BUF from a standalone Vulkan producer to
Godot, imports it into Godot's Vulkan device, and copies it on the GPU into an
ordinary Godot-owned texture. The same `Texture2DRD` is shown in a TextureRect
and on a 3D quad. The producer generates changing test patterns with a compute
shader; it never uploads CPU pixels. No dwl capture code changes are involved.

This is a finite interoperability test, not the live application-window bridge.
It uses one allocation and 1–32 sequential frames on the same physical GPU and
driver. The tested device is AMD Radeon 780M with RADV. NVIDIA image import and
cross-GPU transport have not been tested by this proof.

**Build and run**

Run these commands from the repository root. Requirements are the prepared
engine checkout/private build environment, pinned godot-cpp checkout described
in [the extension README](../godot-bridge/README.md), Vulkan development headers
and loader, CMake, and `glslangValidator` on PATH. The graphical verifier also
requires Khronos validation layers, found through VULKAN_SDK or the system layer
directory. Existing builds can be reused.

```sh
python3 engine/build.py --jobs 6
cmake -S dmabuf-proof -B dmabuf-proof/build
cmake --build dmabuf-proof/build -j4
cmake -S godot-bridge -B godot-bridge/build
cmake --build godot-bridge/build -j4
python3 tests/test_dmabuf_proof.py --frames 8
python3 tests/test_dmabuf_proof.py --frames 8 --no-readback
```

The runner creates a mode-0700 temporary directory, copies the demo, configures
optional engine extensions in that copy, opens a temporary Godot window using
Mobile/Vulkan on GPU index 0, and starts the producer with Godot's selected GPU
UUID. It exits automatically after the requested frames. It stops only its own
processes. The existing interactive SHM editor runner is unchanged.

The artifact path printed by each run contains both logs, GPU identity,
progress/result JSON, validation configuration, and a screenshot. The default
verification run additionally reads back each **destination** texture into
`frame-N.rgba` and compares every byte against an independently generated CPU
reference. `--no-readback` skips those target readbacks/comparisons; the final
screenshot still reads the rendered viewport for evidence. Neither mode uploads
these verification bytes to make the displayed image.

Fault cases can be run individually:

```sh
python3 tests/test_dmabuf_proof.py --fault size
python3 tests/test_dmabuf_proof.py --fault uuid
python3 tests/test_dmabuf_proof.py --fault extra-fd
python3 tests/test_dmabuf_proof.py --fault truncated
python3 tests/test_dmabuf_proof.py --fault empty
python3 tests/test_dmabuf_proof.py --fault disconnect
```

The first five must be rejected cleanly. `disconnect` instead closes the
producer after sending its buffer and destroys its Vulkan objects; the receiver
must still copy and display the valid retained storage. Every run requires
confirmed validation-layer loading in both processes and rejects Vulkan VUIDs,
synchronization hazards, and script errors. Results and build hashes are in
[the proof report](../reports/dmabuf-proof/README.md).

**What each part does**

1. `producer.cpp`, `Producer::initialize()` creates an exportable image and its
   allocation. `render()` runs `pattern.comp`, releases queue ownership to
   EXTERNAL, and waits for its GPU fence. Only then does `send_frame()` export a
   new DMA-BUF FD and send it with layout metadata using `SCM_RIGHTS`.
2. `BridgeDMABufReceiver::receive()` runs on a socket worker. It validates the
   small message and descriptors, then publishes one pending owned FD. It never
   performs GPU work. Nonblocking sockets and cancellation polling keep socket
   waits off Godot's main/render threads. The pending slot cannot accumulate
   frames.
3. `copy_next()` runs through `RenderingServer.call_on_render_thread()`. It
   checks GPU/driver UUIDs, allocation size, layout and memory requirements,
   creates an image with that explicit DRM modifier layout, imports the FD, and
   binds its storage. Successful `vkAllocateMemory()` consumes the owned import
   FD; before success, the receiver owns and closes it on failure.
4. Engine patch 0003 adds `texture_create_from_external_snapshot()`. It creates
   a Godot-owned destination, acquires source queue ownership, performs
   `vkCmdCopyImage()`, releases the source back to EXTERNAL, and waits for the
   copy fence. It uses Godot's actual queue submission mutex and initializes
   Godot's texture state tracking. It permits UNORM/SRGB views needed by normal
   Godot materials. The extension then destroys its imported image and memory
   and lets the socket worker send a successful ACK. **That ACK means GPU copy
   completion**, so the producer can overwrite the source for the next frame.
5. `dmabuf_proof.gd` assigns the returned RID to
   `Texture2DRD.texture_rd_rid`. Both `TextureRect.texture` and
   `StandardMaterial3D.albedo_texture` use that resource. These nodes sample the
   Godot-owned copy, whose lifetime is independent of the source. Replaced RIDs
   are freed on the render thread; shutdown stops the worker and clears the
   texture before freeing the final RID.

The wire carries a shared-storage FD and metadata, never another process's
`VkImage` integer. The imported `VkImage` is created inside Godot's own process
and device, then passed locally to the engine hook.

```text
Producer GPU compute → fence completed → metadata + DMA-BUF FD
                                                ↓ socket worker
                                     one owned pending FD
                                                ↓ render thread
                              Vulkan import → GPU copy → copy fence
                                                ↓
                             Godot-owned RID → Texture2DRD → 2D / 3D
                                                ↓
                         release imported source → ACK → producer may reuse
```

**Protocol and limits**

`protocol.h` defines a 96-byte network-order DMB1 header. It is one small
SOCK_SEQPACKET message with exactly one `SCM_RIGHTS` descriptor, not a packet of
image bytes. The private parent directory must be owned by the current user
and mode 0700; the receiver binds mode 0600 and refuses to overwrite an existing
path. Truncated payload/control data and extra descriptors are rejected; all
received owned FDs are closed on rejection.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | DMB1 magic |
| 4 | 4 | Version 1 |
| 8 | 4 | Header length 96 |
| 12, 16 | 4 each | Width, height |
| 20 | 4 | DRM ABGR8888: RGBA byte order on this little-endian host |
| 24 | 4 | One plane |
| 28 | 4 | Vulkan image usage contract: transfer src/dst, sampled, storage |
| 32, 40, 48 | 8 each | Plane offset, stride, DRM modifier |
| 56 | 8 | Sequential frame ID |
| 64, 80 | 16 each | Device UUID, driver UUID |

Only LINEAR modifier 0, one layer/mip, RGBA8 UNORM, dimensions up to 2048²,
stride at most 64 KiB and backing allocation at most 64 MiB are accepted.
The producer currently uses 512×256. Import requires the exact same allocation
requirements and identical GPU/driver UUIDs. This restrictive contract is
deliberate; it does not negotiate arbitrary application image usage/modifiers.

The reply is 16 bytes: ACK1 magic, 32-bit status, 64-bit frame ID. The producer
waits at most ten seconds for an ACK, and never reuses the source on a failed or
missing ACK. The receiver holds at most one pending frame and accepts at most
32 frames from one producer. Cancellation wakes socket/condition waits. Exported
FD copies and imported Vulkan memory retain storage even if the producer exits.
Storage lifetime alone does not authorize modifying that storage before ACK.

**What this proves and what comes next**

This proves FD passing, actual image import on Godot's existing Vulkan device,
GPU copying, display through standard texture resources, and repeated safe
source reuse for the controlled contract. Normal transport/display does not
map or CPU-copy source pixels. There is still one GPU copy per image.

Readiness uses a conservative CPU handshake between completed GPU fences.
No acquire/release sync-FDs are transferred yet, despite enabling their optional
engine extension for later work. Same GPU and driver allow EXTERNAL queue
ownership transfers here; this does not establish the FOREIGN ownership rules
needed for other APIs/drivers.

The engine copy hook blocks Godot's render thread until its fence completes and
allocates a new destination per frame. Its infinite fence wait avoids freeing
in-flight resources on a timeout; GPU-hang recovery is not implemented. This is
appropriate for a controlled diagnostic, not the production live bridge. It
does not block dwl's event loop: dwl does not participate in this test.

The asynchronous submission/completion and sync-file checkpoint is now implemented
in [ASYNC.md](ASYNC.md). The next step is to connect a deliberately
owned compositor export pool and safe applied-state leases; retain SHM fallback.
Real client DMA-BUF capture, editor import/reload lifecycle, arbitrary formats,
surface trees, cross-GPU sharing, input, pacing and direct sampling remain
future work. The existing SHM editor preview remains the usable editor path.
