Real application windows through compositor-owned DMA-BUFs

Run from the repository root. This opens a Godot runtime viewer and a temporary
nested dwl. The viewer shows the selected application's main surface in a
TextureRect and on a 3D mesh. It stays open until Ctrl+C in the launching
terminal or until you close the Godot viewer.

```sh
python3 tools/run_gpu_client.py
```

The default program is a foot window printing changing text. To choose your own
program, replace the string after **`--command`**, and provide its exact native
Wayland **`--app-id`**:

```sh
python3 tools/run_gpu_client.py \
  --command 'foot --app-id=my-test top' --app-id my-test --fps 10

python3 tools/run_gpu_client.py \
  --command 'weston-simple-egl' --app-id org.freedesktop.weston.simple-egl
```

The GPU runner accepts `--fps 1` through `--fps 60`, with 10 as the default.
For a 60 FPS ceiling:

```sh
python3 tools/run_gpu_client.py --fps 60 \
  --command 'weston-simple-egl' --app-id org.freedesktop.weston.simple-egl
```

The former 30 FPS ceiling was a prototype software guard, not a DMA-BUF or
Godot limit. Actual updates depend on new client content and the single-slot
copy/release pipeline; `--fps` sets the maximum export cadence, not measured
presentation FPS. The old SHM editor runner retains its separate rate limit.

Use a GUI application, or a terminal wrapper such as `foot ... top`; `top` alone
does not create a window. The command runs through a shell **inside the nested
Wayland environment**, so environment assignments and quoting work. Keep one
matching native window open. Click that window in nested dwl to type/interact;
input through the Godot mesh is a later milestone.

The runner prints the mapped native app IDs and its private /tmp artifact path.
App ID is not necessarily the executable name or window title. If the preview
waits, inspect the printed directory, then rerun with the actual ID:

```sh
rg 'MAP .*app-id=|\[gpu-bridge\]' /tmp/dwl-gpu-client-XXXX/dwl.log
```

Replace `XXXX` with the actual printed suffix. For programs which change their
ID after mapping, prefix the command with `WAYLAND_DEBUG=client` and search
`set_app_id` in `dwl.log`. A program that reuses an existing instance outside
the nested compositor does not create capturable content there; use that
program's separate-instance option/profile. Firefox has not been verified in
this milestone.

The installed SDK's vkcube does not set a native app ID. dwl reports its fallback
marker `broken`; in this isolated single-window test it worked with:

```sh
python3 tools/run_gpu_client.py \
  --command 'vkcube --wsi wayland --gpu_number 0' --app-id broken
```

That marker matches unidentified windows, not a stable vkcube identity. Prefer a
real application ID when available. `--gpu_number` selects vkcube's GPU;
`--gpu-index` selects Godot's GPU. On the verified machine Godot index 0 is AMD,
matching nested dwl. The runner checks the actual DRM identities and reports a
mismatch. NVIDIA import and cross-GPU sharing have not been tested here.

Ctrl+C stops only the runner's viewer, nested compositor and startup process
group. Nothing is installed, your active desktop compositor is not restarted,
and no repository changes are pushed. If the application window closes, the
viewer may retain its last image; stop the runner when finished. This is a
runtime preview; the existing SHM editor-preview runner remains separate.

**Build**

The source, godot-cpp checkout and private engine build environment from the
earlier milestones must already be prepared. No new system package is required
on the verified machine. The engine now includes patch 0005 for foreign-API
queue ownership; the original supplied executable remains separate.

```sh
make dwl
python3 engine/build.py --jobs 6
cmake -S godot-bridge -B godot-bridge/build
cmake --build godot-bridge/build -j4
```

The runner verifies that these local outputs exist, copies the demo into a
private directory, and configures the Vulkan extensions only in that copy.
It explicitly uses the nested Wayland backend and the verified GLES2 renderer.
The normal interactive path performs no bridge pixel readbacks or file-based
frame transfer. Logs and the latest progress JSON remain in the artifact folder.

**What was implemented**

```text
Native Wayland application: SHM or DMA-BUF submissions
                     ↓ normal wlroots import/upload and applied state
Current main-surface texture
                     ↓ GPU render, with client acquire synchronization
One dwl-owned LINEAR RGBA8 DMA-BUF + producer completion sync-file
                     ↓ small private Unix message, SCM_RIGHTS
Godot worker waits for readiness; render thread imports with FOREIGN ownership
                     ↓ asynchronous GPU copy
Godot-owned Texture2DRD → TextureRect and aspect-correct 3D quad
                     ↓ completion ACK + release sync-file
dwl may reuse its export slot after both producer and consumer have finished
```

There are **two GPU copies/renders** in this design: one into dwl's export buffer,
one into Godot's texture. It does not directly sample the application's original
allocation. SHM applications still require wlroots' normal CPU-to-GPU upload;
the bridge does not read their pixels back to CPU during normal operation.

The small hooks in `dwl.c` initialize the optional helper, pass applied native
commits to it, and clean it up. Existing config, scene/output rendering, input,
SHM diagnostic capture and SHM socket transport stay in place. GPU export is
disabled unless both `DWL_BRIDGE_GPU_SOCKET` and `DWL_BRIDGE_GPU_APP_ID` are set;
the runner sets these only for its nested child process.

**Ownership and event flow**

`gpu_bridge_commit()` selects the first mapped native main surface with the
configured ID and marks its applied state dirty. Unmap/destroy listeners clear
the surface pointer. An event-loop timer coalesces changes and captures the
latest state at the requested maximum rate. Rates above 30 use a 1 ms timer;
lower rates retain the 10 ms timer. Nanosecond deadlines preserve the fractional
16.667 ms cadence at 60 Hz and skip missed slots without queueing catch-up work.
A final update during throttling
therefore remains dirty until captured, even if no further commit arrives.

`capture()` uses `wlr_surface_get_texture()` and the surface's current source
box, inverse buffer transform and logical dimensions. It never identifies the
frame through `surface->buffer->source`. It locks the **applied client-buffer
wrapper** until its render completes. That retains the texture and prevents
wlroots from updating a mutable SHM texture in place while it is being sampled.
GPU texture references also retain their underlying buffers. Existing output
reads and client releases remain part of wlroots' accounting.

Client explicit acquire timelines are included in the render options. The
render pass signals an owned DRM timeline; its exported sync-file is watched by
the Wayland event loop. Only confirmed producer completion permits the source
wrapper to be unlocked. This is separate from allocation lifetime and consumer
release. There is no CPU fence wait, blocking socket operation, `glFinish()` or
image readback in the normal compositor path.

The allocator owns one export buffer, separate from application storage. Borrowed
DMA-BUF metadata is used only while that buffer lives; borrowed FDs are never
closed by the helper. `SCM_RIGHTS` creates owned descriptors in Godot. Its worker
validates/waits for the acquire sync-file, then the render thread imports the
image. Successful Vulkan memory import consumes the owned DMA-BUF FD; successful
engine submission adopts the imported image and memory until completion.

Engine patch 0005 chooses `VK_QUEUE_FAMILY_FOREIGN_EXT` for these GLES2/GBM
exports and returns ownership to FOREIGN afterward. Controlled Vulkan producer
tests still use EXTERNAL. The Godot destination is published only after its GPU
copy fence completes; a completion ACK carries the release sync-file back to
dwl. The helper polls that release before reusing the slot. The displayed copy
then belongs entirely to Godot and can outlive the export or source window.

On receiver absence, backpressure, invalid negotiation or disconnect, dwl keeps
processing clients. There is one export in flight and no image queue. After a
disconnect it drops the old export rather than overwriting storage the receiver
could still retain, and retries with a different allocation. Its own outstanding
render must finish before releasing the source/export. An uncertain producer
fence pins that one slot and requires restarting the test; GPU-hang recovery is
not implemented. Compositor shutdown also avoids prematurely unlocking such a
source. This is bounded failure behavior, not a production recovery system.

**Protocol v3 and limits**

Godot first checks LINEAR RGBA8 external-memory import with transfer-source usage
on its actual Vulkan physical device. Its 32-byte DGH1 hello contains version 3,
header length, ABGR8888 format, LINEAR modifier and DRM render major/minor.
dwl requires an exact device/format/modifier match before allocating an export.
This deliberately fails closed for different GPU/node identities.

The 112-byte DMB1 frame retains the v1 field offsets, sets version 3 and usage
to Vulkan TRANSFER_SRC, and adds DRM major/minor at offsets 104/108. The UUID
slots are zero because the producer API is GLES2/GBM. Offset 96 requires one
acquire sync-file, offset 100 is reserved. Exactly two descriptors are attached:
the export DMA-BUF and its acquire fence. ACK2 retains the v2 20-byte format and
zero/one release-FD count. Metadata is big endian; image bytes are not sent in
the socket packet. Messages and ancillary data must be complete and exact.

The export is one-plane DRM ABGR8888 / Vulkan RGBA8, explicit LINEAR modifier 0,
up to 2048×2048 logical pixels, bounded stride and at most 64 MiB backing storage.
The receiver validates layout and checks that Vulkan memory requirements fit
the actual foreign allocation; it does not assume a GBM allocation has the same
size as a Vulkan-created test allocation. The current same-node restriction is
verified for the nested Wayland backend on AMD/RADV, not a general primary-node
mapping or multi-GPU solution.

Only one selected native xdg main surface is exported. The 3D material is opaque;
translucent-window presentation is unfinished. Complete surface trees, popups,
XWayland, multi-window identity, input routing, presentation feedback,
HDR/color management, production editor lifecycle and direct sampling remain
future work. Scale/viewport/transform handling follows wlroots' main-surface
render path; arbitrary rotations were source-reviewed, not graphically tested.
The quad and TextureRect preserve the frame's aspect ratio when resizing.

**Tests and diagnostics**

Interactive use skips pixel readback. For an automated comparison:

```sh
python3 tools/run_gpu_client.py --auto-exit --frames 8 --verify --validation
python3 tools/run_gpu_client.py --auto-exit --frames 8 --verify --validation \
  --command 'weston-simple-egl' --app-id org.freedesktop.weston.simple-egl \
  --expect-input DMA-BUF
python3 tests/test_gpu_bridge.py
python3 tests/test_gpu_client_interactive.py
```

`--verify` is allowed only with `--auto-exit`. It explicitly enables a synchronous
test-only readback of dwl's completed export and Godot's copied destination, then
compares every byte. That compositor readback and reference-file writing can
block the event loop and are **not** enabled in interactive operation. A final
viewport screenshot is also diagnostic. `--validation` needs the installed
Khronos layer through VULKAN_SDK or the system layer directory.

Compiled and graphically tested: SHM foot, GPU/DMA-BUF weston-simple-egl and
GPU/DMA-BUF vkcube; exact export/copy pixels, nested output resize, final idle
update after throttling, more than 32 live frames without texture readback,
Ctrl+C cleanup, receiver absence/mismatch, withheld ACKs and safe abandonment.
Godot Vulkan core/synchronization validation was clean in those runs. The old
SHM transport and controlled v1/v2 GPU proofs also passed regression checks.
Evidence and hashes are in [reports/gpu-client/README.md](reports/gpu-client/README.md).

The lifetime/synchronization review used exact wlroots 0.19.3 source, especially
[client-buffer accounting](https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/0.19.3/types/buffer/client.c),
[GLES2 render passes](https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/0.19.3/render/gles2/pass.c),
and [scene surface synchronization](https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/0.19.3/types/scene/surface.c).
