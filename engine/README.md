Isolated Godot Vulkan integration build

This checkout and patch series add optional Vulkan extension requests, a runtime
enabled-extension query, a synchronous diagnostic external-image GPU copy, and
a bounded asynchronous copy/retirement path.
The original executable in `godot/` is separate. The GDExtension performs the
controlled DMA-BUF import; this engine hook owns queue submission and destination
texture state tracking.

Base: official Godot 4.7.2-stable, exact commit
`ed1daf0bf001b61586d9930840f2f1394092c079`.

**Changes**

`0001-optional-vulkan-extension-requests.patch` adds the restart-required project
setting `rendering/rendering_device/vulkan/additional_device_extensions`.
Its default is an empty PackedStringArray. Vulkan initialization adds these names
to its optional requests before creating the device. Empty names and duplicate
requests are skipped. Unsupported requests follow the existing optional-extension
handling. This enables names only: dependencies and feature structs still need
to be chosen correctly by the integration using them.

`0002-expose-enabled-device-extensions.patch` adds
`RenderingDevice.get_device_enabled_extensions()`. On the render thread it
returns the Vulkan driver's sorted enabled names. Other RenderingDevice drivers
inherit an empty result. The Vulkan audit compares this API result against the
actual successful vkCreateDevice call recorded by LunarG, rather than trusting
the presence of a project setting.

The setting/API names follow the upstream proposal's design, but this is a small
local patch against the exact engine version, not an upstream merged feature.
[Upstream PR #114940](https://github.com/godotengine/godot/pull/114940).

`0003-controlled-external-snapshot-copy.patch` adds
`RenderingDevice.texture_create_from_external_snapshot(image, width, height)`.
It accepts a same-process Vulkan image already complete and released to
EXTERNAL in GENERAL layout, with the restricted RGBA8 contract documented in
[the controlled proof](../dmabuf-proof/README.md). On the render thread it copies
into a new Godot-owned texture, using the Vulkan driver's actual queue mutex and
a completed fence, then initializes texture tracking and UNORM/SRGB view support.
This is a synchronous diagnostic API for trusted native integration, not a
general safe image-handle API or the asynchronous production path.

`0004-async-external-copy-retirement.patch` adds submit/poll/discard APIs for one
external copy per RenderingDevice. A successful submit adopts an imported image
and memory, then returns without waiting for GPU completion. Poll uses fence
status; completion publishes the destination and an owned release sync-file FD.
Discard retains resources until completion; normal device finalization retires
any remaining job before teardown. Only finalization can block on that fence.
The existing synchronous hook remains available for regression checks. See
[the async ownership guide](../dmabuf-proof/ASYNC.md).

`0005-foreign-compositor-image-ownership.patch` adds an optional `foreign`
argument to asynchronous submission (default false). Compositor-owned GLES2/GBM
exports acquire/release `VK_QUEUE_FAMILY_FOREIGN_EXT` and require its enabled
extension. The controlled Vulkan producer retains EXTERNAL ownership by default.
See [the real client guide](../GPU_BRIDGE.md) and
[five-patch verification](../reports/gpu-client/README.md).

**Prepare and build**

Requirements are Godot's Linux build dependencies, Clang/LLD, Python with venv,
and the pinned private SCons installation. The source, Python environment, build
outputs and logs are ignored by the compositor repository; patches and this
build helper are tracked. No system installation or desktop restart is involved.
[Godot Linux build instructions](https://docs.godotengine.org/en/stable/engine_details/development/compiling/compiling_for_linuxbsd.html).

From the compositor repository root, for a fresh checkout:

```sh
git clone --depth 1 --branch 4.7.2-stable \
  https://github.com/godotengine/godot.git engine/source
python3 -m venv engine/venv
engine/venv/bin/python -m pip install scons==4.10.1
python3 engine/build.py --jobs 6
```

Skip clone/venv setup when already present. The build helper verifies the exact
base revision, applies the reviewed patches only to a clean checkout, or detects
that they are already applied. It refuses a partial/unknown patch state instead
of resetting the source. `--prepare-only` checks/applies without compiling.

Build options are Linux editor, Clang + LLD, no debug symbols, no optimization,
no LTO, and suffix `dwl_bridge`. This is a development build; performance is not
representative of the official optimized executable. Six jobs limit contention
and memory use on this machine. Full 3D/Vulkan/GDExtension/editor functionality
remains enabled.

```sh
tail -f engine/logs/build.log
./engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge --version
```

The helper records the base revision, source-diff hash, patch hashes, binary hash,
build command, SCons version and executable version in `engine/logs/manifest.json`.
Restart any running Godot process before testing another native library build.

The earlier two-patch capability build's measured control matrix and rendering
results are in
[reports/vulkan/patched-engine/README.md](../reports/vulkan/patched-engine/README.md).
Those records retain the earlier binary's hashes. The three-patch import/copy
build and results are in
[reports/dmabuf-proof/README.md](../reports/dmabuf-proof/README.md).
The four-patch asynchronous build has its own manifest and evidence in
[reports/dmabuf-async/README.md](../reports/dmabuf-async/README.md).

**Audit without changing the demo's saved configuration**

```sh
python3 tools/probe_vulkan.py \
  --engine engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge \
  --gpu-index 0 --validation --request-bridge-extensions
```

The runner copies the small demo into its private artifact directory, links the
existing extension binary there, and sets additional_device_extensions in that
copy. It requests the four baseline bridge extensions plus
`VK_EXT_queue_family_foreign` for later external ownership experiments. Enabling
that extension does not itself perform a queue-family ownership transfer.

Omit `--request-bridge-extensions` to test the empty-default behavior. Add
`--request-extension NAME` repeatedly to exercise optional unsupported names or
duplicates. `--engine` defaults to the stock executable, so it can serve as the
control. Explicit requests on the stock executable do not change its device
initialization code.

Each audit records the selected engine's binary hash and extension requests.
When the new inspection API exists, the runner requires its list to agree exactly
with the captured device-creation list. Device choice is still explicit: on the
verified machine index 0 is AMD, matching nested dwl; index 1 is NVIDIA and does
not match the current compositor GPU. Extension enablement does not make
cross-GPU buffer import work automatically.

For later interactive use of a patched engine, set this in the project's existing
`[rendering]` section and restart that engine:

```ini
rendering_device/vulkan/additional_device_extensions=PackedStringArray("VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd", "VK_EXT_queue_family_foreign")
```

The default controlled proof now transfers acquire/release sync-FDs and submits
and polls asynchronously. Acquire readiness is checked by the socket worker;
Godot's queue does not wait on an unready producer. This preserves responsiveness
and bounds retirement, but remains a controlled producer rather than real
application buffers. The legacy v1 test still uses the synchronous hook.

The saved demo defaults to OpenGL. For a Vulkan editor session, select the patched
executable and pass `--rendering-method mobile --rendering-driver vulkan
--gpu-index 0`; the extension request setting must also be configured in the
chosen project. The audit runner makes such a private project for experiments.
The interactive editor runner now accepts `--engine`, `--project`, `--vulkan`
and `--gpu-index`, while keeping the stock-engine defaults. Export templates
have not been built; a future exported application will need compatible patched
templates if it depends on these capabilities.
