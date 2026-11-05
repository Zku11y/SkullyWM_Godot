Real native application GPU exports — 2026-11-05

The original checkpoint below used a 1–30 FPS guard. The GPU runner now accepts
1–60 with finer pacing; see [the 60 FPS follow-up](fps-60/README.md). Its evidence
and build hashes are separate from this historical snapshot.

Real application content now travels through dwl's compositor-owned DMA-BUF
export into Godot's existing Vulkan device and a Godot-owned Texture2DRD.
The runtime viewer displays the same frame in 2D and on an aspect-correct 3D
quad. This replaces the controlled pattern as the source for this path; the
earlier controlled tests remain regression controls.

Verified stack: wlroots 0.19.3, nested Wayland backend, GLES2/radeonsi on AMD
Radeon 780M; Godot 4.7.2 with five isolated engine patches, Mobile/Vulkan/RADV
on matching DRM render node 226:128. NVIDIA import and cross-GPU use were not
tested. No active compositor restart, install, commit or push occurred.

| Check | Result |
| --- | --- |
| Real foot, SHM input | Exact export/Godot pixel comparison passed |
| weston-simple-egl, DMA-BUF input | Eight exact frames passed; input included tiled multi-plane buffers |
| vkcube Wayland/Vulkan on AMD, DMA-BUF input | Eight exact frames passed |
| Nested-output resize | Sixteen exact frames across 1404², 798×598 and 998×698 main-surface sizes |
| Final update during 1 FPS throttling, then idle | Two exact, different frames delivered; final update retained |
| Continuous interactive mode | At least 35 frames; no source/destination texture readback files |
| Ctrl+C | Runner's Godot, nested dwl and startup process all stopped |
| No receiver / wrong DRM device | Client commits continued; no export allocated before a valid hello |
| Receiver withholds ACK | One allocation and one sent frame; applied client commits continued |
| Disconnect while retaining exported FD | Reconnected export used different storage; old FD remained held |
| Existing controlled v2 and legacy v1 | Exact pixels passed; malformed fence/header rejection passed |
| Original SHM transport | Nested capture still equals its PPM diagnostic byte for byte |

Godot Vulkan core/synchronization validation was enabled and clean for the real
application comparisons; the Vulkan client also inherited validation. EGL work
does not run through Vulkan validation. These checks cannot establish all
cross-API driver correctness, but combine real FD import, exact pixels,
completion/reuse accounting and actual client traffic. The rendered screenshots
were visually inspected.

The first vkcube attempt used an incorrect selector. Inspection of actual
Wayland traffic/mapping showed that this installed vkcube sets no app ID. Its
isolated passing run used dwl's `broken` fallback marker. This was a selection
issue, not a failed DMA-BUF import. The runner now prints mapped native IDs and
includes them in finite-test timeout errors.

The comparison reads back the **completed compositor export** with wlroots and
the **Godot-owned destination** with RenderingDevice, then compares every RGBA
byte. It does not compare unlike source/output formats, assume packed source
rows or add SHM offsets. Source tiled/compressed buffers are sampled normally
by wlroots into the negotiated LINEAR RGBA8 export. Interactive mode does not
enable either texture readback. Verification readbacks and file writes are
explicit test operations and can block the compositor; they are not the normal
transport path. Final screenshots are also diagnostic readbacks.

The default bridge does two GPU copies/renders, one into the export and one into
Godot's texture. SHM clients still use normal wlroots upload. It is not direct
sampling of client allocations and is not a zero-copy performance claim.

**Evidence and reproduction**

Case folders retain logs, launch/GPU identity and result/progress JSON where
available. `run-summary.json` records original temporary artifact paths, frame
hashes, observed input kinds, export dimensions and allocation counts. Large
raw reference images remain in those temporary directories. Engine and proof
manifests record exact patch, source and binary hashes; earlier milestones keep
their historical manifests unchanged. All five engine patches were replayed in
order using Git index verification and reproduce the six compiled source files.

The interactive guide and launch commands are in
[GPU_BRIDGE.md](../../GPU_BRIDGE.md). Representative commands:

```sh
python3 tools/run_gpu_client.py
python3 tools/run_gpu_client.py --command 'foot --app-id=my-test top' --app-id my-test
python3 tools/run_gpu_client.py --auto-exit --frames 8 --verify --validation \
  --command 'weston-simple-egl' --app-id org.freedesktop.weston.simple-egl \
  --expect-input DMA-BUF
python3 tests/test_gpu_bridge.py
python3 tests/test_gpu_client_interactive.py
```

The resize test used `wlr-randr` **inside the nested compositor's startup
environment**, changing only its WL-1 output to 800×600 and 1000×700. The smaller
main-surface sizes account for dwl borders. The final-idle test disabled foot's
cursor blink, wrote initial text, wrote final text 250 ms later, then produced
no more text; export hashes differ and both match Godot. Backpressure testing
used a private fake receiver that retained FDs and withheld ACKs while checking
continued applied commits and a single export slot. The interactive test sent
SIGINT to its own Python runner after 35 frames and checked process cleanup.

**Supported boundary**

One selected native xdg main surface, up to 2048² logical pixels, one-plane
LINEAR RGBA8 export, same verified DRM render node, explicit producer/consumer
fences, one export slot and a configurable 1–30 FPS ceiling. Source allocation
format/plane count may differ when the wlroots renderer supports sampling it.
The source is retained through the compositor render; Godot retains its separate
copy after release. Idle updates are coalesced rather than lost to throttling.

Popups/subsurfaces as a complete tree, XWayland exports, multiple window IDs,
mesh input, presentation feedback, HDR/color management, production editor
teardown/reconnect, cross-GPU support and GPU-hang recovery remain future work.
Buffer transform/viewport code follows the reviewed wlroots main-surface path;
arbitrary rotated clients were not graphically verified. Unknown GPU completion
pins the one slot until process/session restart instead of releasing it early.
The original SHM fallback path is preserved as a separate runner.
