Patched engine capability results — 2026-11-05

The isolated build at
`engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge` successfully
enables the requested external-memory and synchronization extensions. This
removes the measured capability blocker on Godot's existing Vulkan device.
DMA-BUF image import and external acquire/release are not implemented or tested
by this milestone.

Base engine: `ed1daf0bf001b61586d9930840f2f1394092c079` (4.7.2-stable).
Build: `4.7.2.stable.custom_build.ed1daf0bf`, Clang/LLD, editor, no optimization,
no debug symbols or LTO. See `build-manifest.json` for exact commands, versions,
patch/source hashes and binary SHA-256. All patched-engine reports in this
directory reference that same binary hash.

| Test | New inspection API | Four baseline extensions enabled | Result |
| --- | --- | --- | --- |
| Stock engine, requests present | Absent | No | Control passed |
| Patched engine, default empty setting | Present | No | Defaults preserved |
| Patched engine, AMD, requests present | Present | Yes | Passed with validation |
| Patched engine, duplicates/empty/unsupported request | Present | Yes | Startup and filtering passed with validation |
| Patched engine, NVIDIA, requests present | Present | Yes | Passed with validation; different GPU from dwl |

The baseline extensions are VK_KHR_external_memory_fd,
VK_EXT_external_memory_dma_buf, VK_EXT_image_drm_format_modifier and
VK_KHR_external_semaphore_fd. VK_EXT_queue_family_foreign was also requested
and enabled on both GPUs. Unsupported VK_DWL_BRIDGE_intentionally_unsupported
and the empty name were not enabled. Duplicate requests did not create duplicate
device-create entries. All new-API results agreed exactly with the actual
vkCreateDevice extension lists captured by LunarG.

These are capability/startup tests. AMD remains the matching device for the
existing nested compositor (DRM 226:128). NVIDIA (226:129) having enabled
extensions does not prove cross-GPU sharing or make it the current bridge target.

Each case directory contains `report.json`, its engine log, and a device-create
excerpt. JSON records the private full-trace location and hash. Validation was
confirmed by loader insertion messages, and the validation-enabled audits had
no VUID/validation errors. Full traces remain in the temporary artifact folders;
the excerpts keep this repository's evidence small. The build's first full
compile completed in about five minutes on this machine; future builds depend
on compiler, cache, hardware and options.

Additional checks passed with the rebuilt engine:

- Existing one-shot SHM capture to Godot through AMD Vulkan, with the requested
  extensions configured and validation requested: received RGB matched dwl's
  PPM byte for byte. See `shm-vulkan.log` and the repository-root screenshot
  `milestone_screenshots/patched_engine_vulkan.png`.
- The same project still rendered its pixel-exact SHM snapshot through OpenGL.
  See `shm-opengl.log`.
- Real Vulkan editor: uploaded pixels, texture reuse, resizing, scene close and
  reopen all passed in the isolated editor test. See `editor-vulkan.log`.
- Both patches applied in order to pristine pinned files, including Git index
  verification, and reproduced the six built source files exactly.
- Four audit-parser tests still pass.

Reproduce the capability matrix after building with `python3 engine/build.py`:

```sh
python3 tests/test_engine_capabilities.py --other-gpu-index 1
```

The optional second GPU index is specific to this machine. For one matching GPU,
omit it and set `--matching-gpu-index` if needed. The runner opens temporary test
windows and requires the SDK API-dump/validation layers and a graphical session.

For a single audit:

```sh
python3 tools/probe_vulkan.py \
  --engine engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge \
  --gpu-index 0 --validation --request-bridge-extensions
```

The output directory contains a private `project/` with the extension requests
configured. You can use that project with either graphical test runner via
`--engine ... --project /printed/artifact/directory/project --vulkan --gpu-index 0`.
The editor runner stays open until Ctrl+C unless `--auto-exit` is specified.
This continues testing SHM CPU uploads; no application DMA-BUF is used yet.

The next proof is a controlled producer allocation and one import/copy, including
the required image metadata, layout and queue ownership transitions, acquire
wait, copy completion and resource retirement. General live client sharing must
wait until that whole sequence is correct. The remaining integration may require
more engine code; this patch does not claim the existing public wrapping API
solves external synchronization.
