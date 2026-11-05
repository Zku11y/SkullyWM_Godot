Controlled DMA-BUF import/copy results — 2026-11-05

Historical three-patch v1 checkpoint. The current runner defaults to asynchronous
v2; use `--legacy` to exercise this original synchronous path. See
[the later async results](../dmabuf-async/README.md) for the four-patch build.

Actual DMA-BUF storage was exported by a separate Vulkan process, passed with
SCM_RIGHTS, imported into Godot's existing Vulkan device, and GPU-copied into
Godot-owned textures. The displayed pattern appeared in both a TextureRect and
on a 3D quad. The screenshot was visually inspected. This is the controlled
milestone, not real application-buffer capture or the production live bridge.

Device: AMD Radeon 780M Graphics, RADV PHOENIX, Godot GPU index 0. Both processes
use matching physical-device and driver UUIDs. NVIDIA import and cross-GPU use
were not tested. This proof does not change the running compositor.

| Case | Verification | Result |
| --- | --- | --- |
| Eight sequential frames, one reused source allocation | Every destination RGBA byte matches the expected pattern | Passed |
| Eight frames without destination readback | Rendered 2D/3D screenshot; no texture pixel verification/upload | Passed |
| Oversized dimensions | Reject before import | Passed |
| Incorrect device UUID | Reject before image creation | Passed |
| Extra SCM_RIGHTS descriptor | Reject and close owned descriptors | Passed |
| Truncated header | Reject | Passed |
| Zero-byte packet carrying an FD | Reject after taking ownership of received FD | Passed |
| Producer disconnect after send | Retained storage imports/copies and matches every byte | Passed |

Core and synchronization validation were configured for **both processes**.
The runner requires loader confirmation that Khronos validation was inserted,
then fails on Validation Error, VUID, SYNC-HAZARD or script errors. All final
cases passed these checks. `vk_layer_settings.txt` enables `validate_sync`;
the installed validation layer's manifest recognizes that key.

The positive runs alternate quadrant colors with an asymmetric white corner,
checking orientation and source reuse over two full pattern cycles. Default
verification reads the copied destination back to CPU solely for comparison.
The no-readback run omits those texture readbacks; its final viewport screenshot
is still an explicit test readback. Neither run uploads CPU pixels to produce
the displayed image. There is one GPU copy per frame.

An additional temporary **nested dwl + foot** regression passed using this
three-patch engine through AMD Vulkan. Its received SHM RGB matched the original
PPM capture byte for byte and rendered successfully. See `shm-regression-*.log`.
The compositor and its SHM capture/transport code were unchanged in this proof.

**Evidence and reproduction**

Each case subdirectory contains producer/Godot logs, GPU identity, result JSON
and validation configuration. `run-summary.json` records original artifact
directories and raw verification-pixel hashes. Large raw images remain under
those temporary directories instead of being checked into the repository.
The retained screenshot is
[milestone_screenshots/dmabuf_proof.png](../../milestone_screenshots/dmabuf_proof.png).

`engine-build-manifest.json` records the exact pinned source revision, three
patch hashes, source diff hash, compiler/build options and engine binary hash.
`proof-build-manifest.json` records producer/extension/engine binary hashes,
godot-cpp revision, relevant proof source hashes, repository baseline and SHM
regression comparison. Earlier two-patch capability reports are historical and
keep their original binary hashes.

All three engine patches were applied in order to a temporary pristine tree of
the pinned source, with Git index verification, and reproduced all six built
source files exactly. The build helper now replays overlapping patches in a
private tree to recognize an already patched checkout; it refuses unknown or
partial changes rather than resetting the source.

Commands, function-by-function ownership explanation, protocol fields and
limitations are in [dmabuf-proof/README.md](../../dmabuf-proof/README.md).
The primary commands after building are:

```sh
python3 tests/test_dmabuf_proof.py --frames 8
python3 tests/test_dmabuf_proof.py --frames 8 --no-readback
python3 tests/test_dmabuf_proof.py --fault disconnect
python3 tests/test_godot_nested.py \
  --engine engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge \
  --vulkan --gpu-index 0
```

**Boundaries of this result**

The test accepts only one-plane LINEAR RGBA8, identical GPU/driver, bounded
dimensions/allocation and at most 32 frames, with one source in flight. CPU
handshakes between completed Vulkan fences establish readiness and reuse;
acquire/release sync-FDs are not transferred. The copy hook waits synchronously
on Godot's render thread, and no GPU-hang recovery is implemented. New
destination allocation per frame is diagnostic, not a performance design.

During development, validation caught missing SRGB-view compatibility and
incorrect initial texture tracking; both were corrected before final runs.
The no-readback run also exposed a screenshot wait when drawing stopped.
The test now explicitly draws/presents its final frames and flushes render
cleanup before exit. Drawing without presentation was tried and produced
render-pass synchronization warnings; that path is not used in the final test.

Next: asynchronous completion/retirement and explicit synchronization, followed
by compositor-owned exports and real client leases. Editor DMA-BUF reload/exit,
arbitrary client formats/modifiers, multi-window lifecycle, SHM integration into
a unified protocol, input and direct sampling are not established by this proof.
