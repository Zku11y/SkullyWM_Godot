Research and workplan — dwl windows inside Godot

Prepared 2026-11-02. The milestone descriptions are the plan; dated progress
entries below distinguish completed experiments from remaining work.

Current execution checkpoint, 2026-11-06: the single-window development-session
milestone is implemented in `godot-bridge/world/`. Pressing Play starts the
existing GPU receiver, a native supervisor, nested dwl and one Inspector-configured
application. Stop/restart and closing the game retire GPU work and clean owned
processes. Editor Stop uses SIGKILL; the supervisor's private lease independently
handles that exit. Native lifecycle tests, real foot/EGL runtime sessions and
real editor Play/Stop/Play passed. Saved Run Arguments select the game GPU without
requiring the editor to use the same GPU. See [the development guide](godot-bridge/world/README.md)
and [verification](reports/development-session/README.md).

An existing engine-exit stall was reproduced with an empty Wayland project and
the debugger found its Wayland event thread blocked in poll. Engine patch 0006
adds a deterministic shutdown wakeup. Ten empty-project exits and the session
lifecycle tests passed after rebuilding. This is part of session shutdown
correctness; the buffer transport remains unchanged. Original SHM receiver and
byte-exact GPU-copy regressions passed.

Execution agreement: one active milestone at a time. Before implementing each,
research the exact installed/upstream APIs and ownership requirements, explain
the chosen approach, then implement, verify and explain what was achieved.
Do not start the next milestone while required checks in the current one remain
unresolved. The original numbered milestones below remain architectural context;
the next focused sequence is:

1. Godot-managed single-window development session — completed as above.
2. Stable window identity and multiple-window lifecycle.
3. Mesh input and application control through dwl's seat.
4. Godot-only presentation with correct pacing; retain flat view as a debug option.
5. GPU editor-preview lifecycle and reusable window scenes.
6. Complete surface trees, popups, alpha and compatibility.
7. Patched export templates, helper packaging and standalone session startup.
8. Sustained performance/recovery work and selective direct-buffer optimization.

Synchronization, bounded ownership and regressions remain acceptance requirements
at every step. Neither direct sampling nor exported-runtime support is claimed
by the current checkpoint.

Progress 2026-11-05: milestone 1 is implemented in `godot-bridge/`. The C++
receiver and demo were built and exercised in Godot 4.7.2 using pinned 4.5
godot-cpp bindings. Four headless test groups passed. A graphical nested
dwl/foot test transferred a populated 1404×1404 RGB frame, compared it exactly
against the PPM diagnostic, and showed it in a TextureRect and on a 3D quad.
The rendered screenshot was visually inspected. This used Compatibility/OpenGL
on the AMD Radeon 780M, not Vulkan. Milestone 0's Vulkan capability audit and
milestone 2's live/editor-preview work were still outstanding at that checkpoint. See
`godot-bridge/README.md` for build, run, ownership and verification details.

Progress 2026-11-05, milestone 2: bounded live SHM updates and a reusable `@tool`
3D preview scene are implemented. An explicit app-ID selector enables automatic
captures at 10 attempts/s by default (configurable 1–30). The proven v1 message
format is retained with one connection per snapshot and a persistent listener.
The receiver holds one latest complete frame, and same-size images update the
existing texture. Native tests passed for replacement, malformed-frame recovery,
restart and partial-read cancellation. Nested live runtime testing displayed
eight frames with one texture allocation. Real-editor tests verified uploaded
pixels, resize, and scene close/reopen; a separate nested foot test fed the
editor's actual 3D viewport and survived close/reopen. These used OpenGL, not
DMA-BUF/Vulkan. Applied-state tracking, multi-window identity, final-state delivery
after throttling, and input are still deferred. The Vulkan capability audit is
the next architectural checkpoint before pursuing GPU sharing.

Progress 2026-11-05, Vulkan checkpoint: a read-only GDExtension probe and LunarG
device-creation capture now distinguish support from actual enablement. Godot's
default Vulkan GPU is NVIDIA (DRM 226:129), while nested dwl uses AMD (226:128).
Godot GPU index 0 selects the matching AMD device. Both support the queried
DMA-BUF/modifier and sync-FD features, but the stock engine enables only external
memory FD support among the four baseline extensions. AMD queries ran with
confirmed validation-layer activation and no validation errors. The existing
SHM demo also rendered a pixel-exact captured frame through AMD Vulkan.
Reports, provenance, source audit, reproduction and next engine-build steps are
in `reports/vulkan/README.md`. No external image import, GPU buffer sharing, or
engine patch is claimed by this checkpoint.

Progress 2026-11-05, engine capability build: the exact 4.7.2 engine source was
patched and compiled separately. Two small patches add optional project-level
Vulkan extension requests and an enabled-extension query. Five runtime controls
passed: stock requests, patched empty defaults, AMD requests, unsupported/empty/
duplicate requests, and NVIDIA requests. The new API agrees with captured device
creation in every patched case. All four baseline extensions plus foreign queue
support can now be enabled. AMD still matches dwl; NVIDIA does not. Pixel-exact
SHM rendering passed on the rebuilt engine through Vulkan and OpenGL, and the
Vulkan editor passed upload/reuse/resize/scene lifecycle checks. Build provenance,
patches and results are in `engine/` and `reports/vulkan/patched-engine/`.
The next experiment remains a controlled external allocation/import/copy with
correct synchronization and retirement; capability enablement is not that proof.

Progress 2026-11-05, controlled DMA-BUF proof: a standalone Vulkan compute
producer now exports one LINEAR RGBA8 allocation, sends a real FD using
SCM_RIGHTS, and waits for a copy-complete ACK before reuse. A separate bounded
GDExtension receiver imports it on Godot's existing AMD Vulkan device. Engine
patch 0003 performs a synchronous GPU copy using the engine queue mutex and
initializes destination texture tracking. Texture2DRD displays the owned copy
in a TextureRect and on a 3D quad. The protocol enforces matching GPU/driver
UUIDs, one plane, layout/allocation bounds and a single pending source. See
`dmabuf-proof/README.md` and `reports/dmabuf-proof/` for verification and ownership.
This is milestone 3's controlled image experiment plus a diagnostic part of
milestone 4, not a production synchronization/retirement implementation. CPU
handshakes between completed fences establish readiness/reuse; sync-FD transfer,
asynchronous rendering, real compositor buffers and editor DMA-BUF lifecycle
remain open. The original SHM fallback is preserved.

Progress 2026-11-05, asynchronous synchronization checkpoint: protocol v2 now
passes real acquire/release sync-files with the controlled producer. The socket
worker checks readiness with bounded cancellable polls before import. Engine
patch 0004 adopts one imported image/memory pair at submission, returns without
waiting, and publishes the copied RID only after fence-status polling confirms
GPU completion. Cancellation retains GPU resources until safe retirement; normal
device finalization handles a remaining job. The source cannot be reused before
completion/release. This is still a finite same-GPU LINEAR RGBA8 producer test,
not a real Wayland DMA-BUF client. See `dmabuf-proof/ASYNC.md` and the separate
`reports/dmabuf-async/` evidence. The next implementation is a compositor-owned
export pool and safe applied-state leases, including wlroots completion,
cross-API ownership and format/modifier negotiation. SHM remains a separate
working path; automatic fallback and editor DMA-BUF lifecycle remain open.

Progress 2026-11-05, real client export: `gpu_bridge.c` now renders the selected
native main surface's applied texture into one compositor-owned LINEAR RGBA8
DMA-BUF, signals a DRM timeline and sends its real FDs. Source wrappers are held
until that render completes, and the export is not reused until Godot's copy
and release complete. V3 negotiates the actual DRM device and restricted layout;
engine patch 0005 uses FOREIGN ownership for GLES2/GBM imports. SHM foot,
DMA-BUF EGL weston-simple-egl and Vulkan vkcube reached Godot with byte-exact
export/copy comparisons and clean validation. Resizing, final idle updates,
bounded backpressure, disconnected-allocation abandonment and continuous
no-readback operation past 32 frames were tested. The interactive runner accepts
`--command` and `--app-id` and survives Ctrl+C cleanup. See `GPU_BRIDGE.md` and
`reports/gpu-client/`. This completes the first compositor-owned export slice,
not a full multi-window lifecycle protocol or direct client allocation sharing.
Editor GPU lifecycle, surface trees, XWayland, input, pacing/presentation and
cross-GPU use remain open. No active desktop restart, install or push occurred.

GPU rate follow-up: the runtime GPU runner and compositor helper now accept
1–60 FPS (default remains 10). Nanosecond cadence deadlines and a 1 ms timer for
rates above 30 remove the prototype's validation and timer-rounding ceilings.
Missed slots are skipped; the one-export ownership/backpressure limits remain.
A live weston-simple-egl run delivered 120 frames at about 59.98 export updates/s
with clean Vulkan validation, no texture readbacks and successful Ctrl+C cleanup.
This measures export/copy throughput, not presentation FPS. The separate SHM
editor runner retains its original limit. Follow-up evidence, including a finite
test's engine-exit stall after successful copies, is in `reports/gpu-client/fps-60/`.

The recommended target is a C++ GDExtension providing ordinary Texture2D
resources and window nodes, supported by the smallest Godot Vulkan engine patch
the feasibility tests show necessary. Initially import compatible DMA-BUFs and
copy their content on the GPU into Godot-owned textures. Direct sampling of
client-owned buffers is a later optimization. Keep a supported SHM upload path.

**What is established**

- Repository: `main`, checkpoint `21aaf2c`, tracking `origin/main`. The one-frame
  stream bridge from this session is present as uncommitted changes. Existing
  config, image, trace, and unrelated untracked files must be preserved.
- Installed wlroots headers and pkg-config report 0.19.3. The existing build
  enables XWayland. Previous bridge build, transport tests, and nested foot PPM
  comparison passed; see BRIDGE.md. Those checks do not establish GPU sharing.
- A local archive at
  `/home/skully/projects/godot/Godot_v4.7.2-stable_linux.x86_64.zip` contains an
  executable reporting `4.7.2.stable.official.ed1daf0bf`. Its `--version` was
  executed after extraction to `/tmp`; no editor or project was launched.
  Neither `godot` nor `godot4` was found on PATH.
- A Vulkan SDK's `vulkaninfo` and AMD/NVIDIA ICD files are visible, but `/dev/dri`
  is not exposed in this execution environment. Device choice, driver operation,
  enabled Godot extensions, and actual import compatibility remain unverified.
  ICD files alone do not prove that either GPU is usable.

**Research findings and architectural consequences**

Godot's global RenderingDevice can expose native resources and wrap an existing
image. Texture2DRD makes an RD texture usable by a material. A local RD is not
the device used to render the scene, and headless/Compatibility operation does
not establish the Vulkan path. Use the global device for scene textures.
[RenderingDevice](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html)
and [Texture2DRD](https://docs.godotengine.org/en/stable/classes/class_texture2drd.html).

In the pinned 4.7.2 Vulkan driver, device initialization requests external-memory
FD support, but the reviewed list does not request DMA-BUF, DRM-modifier, or
semaphore-FD extensions. External-image wrapping creates a view; it is not a
DMA-BUF importer or a complete synchronization contract. Image/view ownership
and destruction also need separate treatment. This makes engine work likely,
not proven universally necessary.
[Pinned Vulkan implementation](https://github.com/godotengine/godot/blob/4.7.2-stable/drivers/vulkan/rendering_device_driver_vulkan.cpp).

Godot's main RenderingDevice does not permit `submit()`/`sync()` as a way to
take control of its frame submissions. Thus, getting its VkQueue and running on
the rendering thread is not enough: ordering, external waits/signals, queue
host synchronization, and resource state tracking need an explicit integration
contract.
[Pinned RenderingDevice implementation](https://github.com/godotengine/godot/blob/4.7.2-stable/servers/rendering/rendering_device.cpp).

Upstream PR #114940 proposes both additional device-extension configuration and
enabled-extension inspection. It was open when researched. Treat it as a design
reference, not a shipped feature or a patch to apply without review.
[Upstream proposal implementation](https://github.com/godotengine/godot/pull/114940).

DMA-BUF image import requires compatible formats, modifiers, layouts and usage;
support for external memory alone does not establish image support. Negotiate
exact combinations instead of assuming that a buffer accepted by GLES2 is
accepted by Vulkan. Start on the same physical GPU.
[DMA-BUF extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_external_memory_dma_buf.html)
and [DRM modifier extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_image_drm_format_modifier.html).

Keep a stable Godot-facing texture for each window. The producer may rotate
through several allocations; import identity belongs to the allocation, while
frame identity belongs to each use of its contents. A texture can keep its old
contents until another complete frame is ready. Never identify either using an
FD integer or a Client address.

Proposed normal flow:

```text
Wayland client buffer
  -> dwl selects an applied surface state and obtains a safe content lease
  -> private socket sends metadata, DMA-BUF FDs, and acquire information
  -> Godot import waits for producer readiness
  -> GPU copy or conversion into a Godot-owned Texture2D
  -> GPU completion permits source release
  -> material samples the Godot texture on a mesh
```

If a client buffer cannot be imported safely, use an explicitly implemented
fallback: SHM copy/upload, or a compositor-owned export pool rendered in a
negotiated format/modifier. The latter can normalize GPU clients and later
compose entire surface trees. It adds GPU work, so measure it rather than calling
it zero-copy. Preserve dwl's backend, scene rendering, and output management.

**Milestone 0 — Freeze the baseline and record capabilities**

Deliverables: a reproducible nested launch recipe, engine/build manifest,
Godot rendering log, and capability report. Implementation should use a feature
branch or isolated checkout without discarding the existing worktree changes.
Keep custom config intact; do not commit, push, install, or replace the running
desktop implicitly.

Record the engine hash and matching godot-cpp/API version, compiler, wlroots,
kernel, Mesa/proprietary driver, rendering method, explicit Vulkan driver
selection, device UUID and DRM render-node identity. Compare these with dwl's
actual renderer device. Record supported versus enabled extensions separately;
`vulkaninfo` describes its own device, not the device Godot created. Use API
capture or engine instrumentation where runtime inspection is insufficient.

Inspect the exact 4.7.2 source and generated extension API. List availability of
native device access, external image wrapping, frame scheduling, texture state
handoff, completion notification, and editor execution. Do not assume the
internal VulkanHooks C++ interface is an ordinary GDExtension API.

Pass: the real editor/runtime can render with Vulkan in a nested test, the GPU
choice is known, and every required integration capability is marked verified,
missing, or untested. A headless engine version check alone does not pass.

**Milestone 1 — Show one existing SHM snapshot in Godot**

Build the smallest C++ extension and demo project in separate directories, using
pinned godot-cpp bindings. Reuse the current v1 RGB framing and private receiver
address. Receive the validated snapshot off the main/render threads and publish
only owned complete pixel data through a bounded handoff.

Create an ImageTexture, show it in a TextureRect, then on a QuadMesh through an
unshaded material. Use a test pattern and foot. Keep this independent of Vulkan
import so it provides a visible reference for subsequent work.

Pass: orientation, aspect ratio and colors match the PPM; truncated input leaves
the previous image intact; no socket wait occurs on Godot's render thread.

**Milestone 2 — Make the texture live, including editor preview**

Add explicit connect/disconnect controls and a reusable window display node.
The first live implementation can repeatedly request bounded SHM snapshots;
start at a modest configurable update rate. Use latest-ready-frame selection
and reuse texture storage until dimensions change. Bound pending CPU snapshots,
uploads and total bytes. Avoid decoding PPM or touching diagnostic request files
for each production frame; replace that mechanism when the protocol advances.

Enable the node in the editor with a small tool/editor integration. Test scene
switching, reload, stop, plugin disable, and engine exit. Runtime connections,
FDs and imported resources must not be serialized into scenes. An editor and a
running game are separate consumers: initially allow only one active consumer
and show a clear busy state. Avoid capturing the editor, game instance, or any
bridge viewer; verify their actual app IDs instead of relying on defaults.
[Editor execution](https://docs.godotengine.org/en/stable/tutorials/plugins/running_code_in_the_editor.html).

Pass: typing and resizing update a mesh in both the running project and editor
viewport; disconnection preserves or deliberately clears the last frame; repeated
reloads do not accumulate FDs, memory, threads or pending callbacks.

**Milestone 3 — Prove Vulkan interoperability before expanding the compositor**

Create a controlled producer of an exportable image on the selected GPU. Begin
with a known supported single-plane RGB(A) format/modifier and an immutable test
pattern. This can be a small standalone producer; it must not depend on an
arbitrary browser allocating a convenient buffer. Pass its real FDs using
SCM_RIGHTS and compare with the SHM reference.

Check the capabilities required by the chosen import/synchronization path:
`VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`,
`VK_EXT_image_drm_format_modifier`, and commonly
`VK_KHR_external_semaphore_fd`; include their dependencies and required features.
Check external queue-family ownership support where applicable. Query each
format/modifier/tiling/usage combination, external importability, memory-type
compatibility, and dedicated-allocation requirements. Choose copy, blit, or
shader conversion based on actual format features; sampling support does not
guarantee transfer support.

The immutable-image experiment is an isolation test, not proof of live safety.
Next alternate two known images with explicit readiness and completion, import
them on Godot's actual device, and copy into a Godot-owned destination before
sampling it. Enable Vulkan validation and synchronization validation where
available. Do not discard imported contents with an inappropriate UNDEFINED
layout transition; document external ownership and layouts at every handoff.
[Vulkan synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).

Pass: repeatable import, correct pixels, no relevant validation errors, correct
completion/release under artificial producer delay, and resource teardown after
in-flight work retires. No CPU pixel readback in this GPU test.

Decision: a pure extension is acceptable only if all of extension enablement,
queue ordering, external waits/signals, image state tracking and GPU completion
are demonstrated through maintainable interfaces. Otherwise take milestone 4.
Do not spend later milestones working around a missing foundation.

**Milestone 4 — Add the minimum Godot engine integration if required**

Maintain a separate pinned engine checkout and a small patch series, with the
extension retaining IPC, window tracking and scene-level APIs. First expose
optional extension enablement before device creation and report which features
were enabled. Unsupported capabilities must disable the GPU bridge cleanly.

Then provide the missing external-image handoff: register its real state and
ownership, schedule acquire waits and copies in the engine's submission flow,
signal or observe copy completion, and retire foreign resources safely. A
source wrapper plus `texture_copy()` is not accepted without verifying how the
render graph handles its state. A CompositorEffect may supply a rendering-stage
callback, but it does not automatically provide external-memory synchronization.
[CompositorEffect](https://docs.godotengine.org/en/stable/classes/class_compositoreffect.html).

Prototype a small API around import/copy/retire rather than exposing arbitrary
unsynchronized queue submission. Review upstream PR #114940 for the extension
configuration portion, but do not assume it covers ordering or completion.
Avoid global vkDeviceWaitIdle per frame, private engine-memory access, loader
interposition, or repurposing OpenXR initialization as the long-term solution.

Pass: milestone 3 works in both editor and runtime using the patched binary;
SHM remains usable with the stock engine. Record patch size, affected subsystems,
build instructions and upgrade constraints. The exact API is selected after
the source audit and spike, not promised in this planning document.

**Milestone 5 — Define live protocol, identities, and resource ownership**

Keep v1 as a diagnostic. Introduce a separately versioned protocol for persistent
sessions and FDs. Prefer small bounded SOCK_SEQPACKET control records with image
storage passed as FDs; never send a multi-megabyte image in one packet. A stream
alternative must explicitly associate ancillary FDs with framing and support
partial sendmsg/recvmsg. Keep both process event loops nonblocking.

Specify handshake/capabilities, window create/update/unmap/destroy, buffer
definition, frame-ready, release, retire, errors, and reconnect snapshot. Include
session ID, stable window ID, buffer ID, frame ID, resize generation, dimensions,
FourCC, explicit modifier semantics, plane count, FD-to-plane mapping, offsets,
strides, source crop, buffer transform/scale, alpha/color interpretation, damage,
and synchronization type. Buffer allocation identity and per-use frame identity
must remain distinct even when the producer reuses storage.

Validate lengths, arithmetic, dimensions, formats, plane and FD counts before
import. Reject ancillary truncation, unexpected FDs and unsupported combinations;
close every owned FD on every rejection path. Use CLOEXEC and peer credentials
in a private owned runtime directory. Serialize fields explicitly with documented
endianness and limits, not by sending native C structs.

Set an initial single-consumer policy, one in-flight source use and one pending
latest update per window, a small reusable destination ring, and an explicit
global memory/import/FD budget. When full, drop or coalesce updates before taking
new leases. Do not overwrite a busy slot or grow the queue. A buffer can be
retired only after all frame uses complete.

Pass: protocol tests cover fragmented/truncated input where applicable, invalid
metadata, unexpected FDs, stale generations, duplicate release, reconnect, stalls,
and peer exit. Resource counts remain bounded.

**Milestone 6 — Bridge one real DMA-BUF client with correct commit semantics**

Use a known GPU test client inside nested dwl. Audit wlroots 0.19.3 commit order
and synchronization lifetime first: client_commit exposes submitted state and
can precede application. It must not become the production displayed-frame
definition. Associate an owned source reference and sync state with the exact
commit, then export only when that state applies. Handle discarded/cached commits
without mixing their metadata with a later buffer. Do not use the diagnostic
wrapper-source fallback as authoritative frame identity.

Clone borrowed DMA-BUF attributes or duplicate their FDs explicitly. A received
memory FD is consumed by a successful Vulkan memory import, so retain any
independently needed handle through an owned duplicate and close it on failures.
[Vulkan FD ownership](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMemoryFdInfoKHR.html).

Before exposing real client buffers, prove both readiness and permission to
reuse contents. A wlr_buffer lock alone does not prove the compositor's explicit
release path is delayed. Audit wlroots' per-commit release handling, including
ordinary scene/output use and any direct scanout. Either extend that accounting
correctly or copy into a compositor-owned export slot under wlroots-controlled
acquire/release before involving Godot.

Pass: one animated GPU client reaches the mesh without CPU readback; closing,
unmapping and rapid resizing while imports are in flight is safe; no premature
client release or content tearing occurs under deliberate Godot delays.

**Milestone 7 — Complete synchronization and failure recovery**

Explicit path: carry the client's acquire point through wlroots, bridge it into
a supported receiver wait, and account for Godot copy completion in release.
An exported sync_file may need an asynchronously materialized timeline point;
never block dwl waiting for it. DRM syncobj FDs, sync_file FDs, and Vulkan opaque
semaphore FDs are different handle types and cannot be substituted by integer.
Use sync_file binary-semaphore imports with their temporary-import rules rather
than assuming DRM timelines are directly Vulkan timeline semaphores.
[Wayland explicit synchronization](https://github.com/wayland-mirror/wayland-protocols/blob/main/staging/linux-drm-syncobj/linux-drm-syncobj-v1.xml)
and [Vulkan semaphore FD rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportSemaphoreFdInfoKHR.html).

Implicit path: establish a tested bridge from DMA-BUF reservation fences into
Vulkan waits and, where required, return completion to implicit consumers. Linux
provides export/import sync_file ioctls, but ordering across that sequence is
userspace's responsibility. Do not assume raw Vulkan import automatically waits
for GLES2 writes. If this path is not available, use an implemented normalized
export path or report the client unsupported; do not claim universal GPU capture.
[Kernel synchronization bridge](https://docs.kernel.org/driver-api/dma-buf.html).

For each frame define states: selected, waiting-for-producer, queued-for-copy,
GPU-reading, source-released, and retired. Godot texture publication is separate
from source release: release may follow copy completion before later mesh draws.
Track allocation ownership, content lease, image layout/queue ownership, and GPU
completion independently. Dropping before GPU submission is different from
dropping after submission.

Disconnect and timeout must stop new submissions immediately, but must not
unlock a client buffer while GPU reads can still be running. Socket closure is
not proof of GPU completion. Establish a completion/teardown protocol or use
compositor-owned slots with bounded quarantine and recovery. A CPU-copy snapshot
timeout policy cannot simply be reused for shared client buffers.

Pass: delayed acquire, delayed completion, disconnect before/after submission,
receiver crash, plugin unload and device-loss tests preserve compositor
responsiveness and source reuse rules. Editor pause/minimization cannot hold an
unbounded number of client buffers. Document any driver/device-loss limitation.

**Milestone 8 — Add complete window lifecycle, SHM, and compatibility fallbacks**

Generalize from the selected test window to multiple windows with stable IDs,
metadata, resize generations, initial snapshots and reconnect enumeration.
Handle buffer reuse without reimporting every frame; bound and retire the cache.
Keep existing dwl rules and placement policy. Explicitly support SHM uploads,
including padded source rows, resize, and alpha. An owned shared-memory snapshot
pool passed through SCM_RIGHTS is a possible later optimization over pixel
streaming, after release/backpressure are defined.

For incompatible GPU buffers, add compositor-owned, exportable per-window slots
using the existing renderer and a negotiated format/modifier. Verify wlroots'
actual offscreen rendering/export APIs and synchronization support before
choosing an allocator. Keep slots immutable while Godot reads them. Do not assume
all GPU buffers are CPU-mappable or pretend a readback fallback already exists.

Initially restrict DMA-BUF to a verified RGB(A), same-GPU subset. Treat multi-plane
YUV, implicit/invalid modifiers and cross-GPU import as explicit compatibility
work; unsupported layouts must fail cleanly. Negotiate consumer support without
restricting every Wayland client's allocation to Godot's needs.

Pass: SHM and GPU clients coexist; multiple windows resize, close and reconnect;
memory/import counts stabilize; unsupported paths have an honest status and a
tested fallback where promised. Firefox remains unverified until tested directly.

**Milestone 9 — Route input from the mesh through dwl's seat**

Send authenticated, validated commands using stable window IDs, never Client
pointers. Raycast the mesh, map UV coordinates to displayed source crop, then to
surface-local coordinates accounting for scale/transform and window geometry.
dwl resolves the target and retains focus/seat authority. Never inject input by
global cursor warping as a substitute for surface targeting.

Add pointer enter/motion/leave, buttons, scroll, keyboard focus and key events.
Keep a button grab pinned to the original target until release. Define keycode,
keymap and modifier semantics, including Godot shortcut interception. Release
pressed keys/buttons on disconnect or focus loss. Later handle relative pointer,
pointer constraints, text input, touch, drag-and-drop and clipboard separately.

Pass: click corners accurately, type and scroll in two windows, drag outside a
mesh without losing the release event, and recover cleanly from peer exit.
Keep an emergency compositor shortcut outside the bridge input path.

**Milestone 10 — Render complete surface trees and support desktop behavior**

Choose compositor-side offscreen composition as the initial full-window model:
render the applied main surface, synchronized subsurfaces and popups in correct
order into owned export slots. Transport composed origin/extents and input-region
mapping so popups outside the main rectangle remain visible and clickable.
Preserve alpha with the right premultiplication, crop, scale, color treatment
and output transforms. A separate Godot mesh per surface can be investigated
later if necessary; it should not be required for basic application correctness.

Add native popups and subsurfaces first, then XWayland association/lifecycle,
transient windows and override-redirect behavior. Reuse existing compositor
scene and seat logic where possible without reparenting or destroying the
desktop scene representation accidentally. Account for normal output sampling
and any direct scanout when defining source release.

Pass: menus, tooltips, dialogs, client-side decorations, video subsurfaces,
fractional scaling and synchronized child updates behave correctly. Exported
window trees exclude every Godot viewer surface and its descendants.

**Milestone 11 — Pacing, performance, and fullscreen world mode**

Establish who sends Wayland frame callbacks as windows become visible only in
the 3D world. Keep the current output-based behavior during initial nested tests;
do not duplicate frame callbacks. For hidden or world-only windows, define
visibility, throttling and damage scheduling. Do not claim a Godot copy-complete
event is Wayland presentation feedback; actual presentation needs its own
mapping or must not be reported as measured presentation.

Measure commit-to-copy and copy-to-display latency, compositor callback duration,
GPU copy/conversion time, CPU usage, bandwidth, memory, FDs, import counts and
dropped/coalesced updates. Target smooth 60 Hz for a 1080p window on the verified
machine as an initial measurement goal, not a performance promise. Compare
one/many windows and idle, terminal scrolling, video, resize and overload cases.

Only after correctness, optimize damage, allocation reuse, conversion, and
possibly direct sampling. Direct sampling must prove the longer source lease is
worth avoiding a GPU copy and must retain all release/failure guarantees.

Make the runtime world fullscreen as an opt-in final step. Retain wlroots output
backend, input handling, monitors, hotplug and an emergency exit/recovery route.
Test receiver failure, renderer restart and multi-output changes in nested mode
before the user elects to use it as their session. Never auto-restart the active
desktop or install over its compositor.

Pass: repeatable performance reports, bounded resource use through a sustained
multi-client run, usable recovery, and documented supported/unsupported cases.

**Implementation order and review checkpoints**

Do 0, 1 and 2 first for visible progress. Run 3 and the engine decision next,
before investing in the production protocol. Complete 4 if required, then 5–8
for a reliable live window bridge. Add 9 for interaction, 10 for application
completeness, and 11 for the desktop mode and measured optimization.

Each milestone should be several small reviewable changes when needed: a brief
design/ownership note, implementation, meaningful tests, and a tested launch
recipe. End each with what was compiled, what ran in nested graphics, what was
only source-reviewed, and what still needs the user's graphical session. Avoid
unrelated refactors and rewriting config. No fixed completion estimate is
credible before the milestone 3/4 engine integration gate.

The first usable result is a live SHM window on a mesh, including editor preview.
The GPU proof requires import plus correct readiness and release. The project
goal requires multiple interactive application windows, supported SHM and GPU
paths, full window trees, bounded updates, pacing and recovery. These are
different completion criteria and must be reported separately.
