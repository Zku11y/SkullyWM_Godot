Vulkan feasibility checkpoint — 2026-11-05

The proposed same-GPU DMA-BUF + sync-FD path cannot run on the supplied stock
Godot device yet. Hardware support is present on AMD, but the engine does not
enable several required extensions. A small engine capability patch is the next
step, followed by a controlled import/synchronization experiment. Enabling the
extensions alone will not prove the full bridge.

**Measured results**

Engine: `4.7.2.stable.official.ed1daf0bf001b61586d9930840f2f1394092c079`.
Godot was explicitly launched with the Vulkan driver and Mobile renderer. The
normal demo project still defaults to Compatibility/OpenGL.

| Run | Godot GPU | Godot render node | Nested dwl renderer | Match |
| --- | --- | --- | --- | --- |
| Default selection | NVIDIA GeForce RTX 4070 Laptop GPU | 226:129 | 226:128 | No |
| `--gpu-index 0` | AMD Radeon 780M / RADV PHOENIX | 226:128 | 226:128 | Yes |

GPU indices are observations on this machine, not portable device identities.
Recheck after driver or hardware changes. The comparison uses DRM major/minor
numbers from the Vulkan physical device and `fstat()` on wlroots' borrowed
renderer FD; no renderer FD is closed or transferred by the diagnostic.

| Device extension | Driver supports it, both GPUs | Godot enabled it, both runs |
| --- | --- | --- |
| VK_KHR_external_memory_fd | Yes | Yes |
| VK_EXT_external_memory_dma_buf | Yes | No |
| VK_EXT_image_drm_format_modifier | Yes | No |
| VK_KHR_external_semaphore_fd | Yes | No |

Both GPUs report sync-FD import/export support. Each returned 14 format/modifier
rows across B8G8R8A8_UNORM and R8G8B8A8_UNORM; all queried rows report DMA-BUF
import support separately for transfer-source usage and sampled usage. Linear
single-memory-plane candidates are present for both formats. These queries do
not establish that a particular client allocation has compatible memory types,
offsets, usage, producer state or synchronization. Combined usage combinations,+YUV/multi-plane formats and actual imports were not tested.

**Evidence**

- `default-device.json` and `amd-device.json` contain the selected physical-device
  identities, all supported extensions, per-modifier queries, enabled extension
  names, API availability and compositor comparison. They were captured from a
  dirty worktree identified in each file; the revision alone is not the complete
  implementation provenance.
- `*-device-create.txt` are verbatim excerpts of the sole successful
  `vkCreateDevice` call in each trace. These show the actual enabled extension
  list, rather than merely enumerating driver support. Full traces are retained
  under the `/tmp` paths recorded in the JSON, with their SHA-256 hashes; the
  small excerpts are retained here to avoid checking in multi-megabyte traces.
- `*-godot.log` record renderer/device selection. The AMD run also includes
  loader confirmation that both API-dump and Khronos validation layers were
  inserted. It produced no VUID/validation-error messages. Validation covered
  startup and read-only queries, not external image use.
- `milestone_screenshots/shm_vulkan.png` at the repository root records the
  existing SHM snapshot rendered in 2D and 3D using AMD Vulkan. Its received RGB
  matched dwl's PPM byte for byte. It remains a CPU upload, not DMA-BUF import.

The initial default capture predates the extra validation/selected-index metadata
in the runner; its selected device and enablement are still recorded directly
in its engine log, native queries and device-creation excerpt.

**Reproduce**

Build dwl and the extension from the repository root. The extension now also
requires Vulkan development headers and the Vulkan loader at build/run time.
The audit requires the LunarG API-dump layer, available in the installed SDK.
`VULKAN_SDK` should name its platform directory containing `share/vulkan`.

```sh
make dwl
cmake -S godot-bridge -B godot-bridge/build -DCMAKE_BUILD_TYPE=Debug
cmake --build godot-bridge/build -j 4
python3 tools/probe_vulkan.py
python3 tools/probe_vulkan.py --gpu-index 0 --validation
python3 tests/test_vulkan_audit.py
python3 tests/test_godot_nested.py --vulkan --gpu-index 0
```

Pass `--api-dump-layer-dir /path/to/share/vulkan/explicit_layer.d` if SDK
discovery fails. `--output /new/directory` chooses an artifact directory; otherwise
the runner creates one under `/tmp`. These are graphical tests: headless mode
has no global Vulkan device and is explicitly rejected by the probe. The runner
opens a temporary nested compositor and a separate Godot window, stops its own
processes, and does not install or restart the desktop compositor. It removes
caller VK_* overrides in its child environment to make device/layer selection
reproducible; display/session variables remain inherited.

**What the new code does**

`vulkan_probe.gd` schedules work on Godot's render thread, checks that the active
driver is Vulkan, and obtains the global RD's physical-device handle. That handle
stays inside this process. `BridgeVulkanProbe` uses it for read-only properties,+extension, semaphore and image-format queries. It does not allocate images,
import memory, submit commands or synchronize GPU work. It writes ordinary report
data on the main thread. Each native query enumerator has a bounded count.

The Python runner records the nested renderer identity, starts the probe with
LunarG's API-dump layer, parses exactly one successful device creation, and joins
that enablement evidence with the supported-capability report. It refuses
missing, truncated or ambiguous capture data; parser tests cover those cases.
The `required_capabilities_present` field is a limited prerequisite check, not
an assertion of importer or synchronization correctness.
[LunarG API-dump documentation](https://vulkan.lunarg.com/doc/view/1.4.350.0/mac/api_dump_layer.html).

**Engine integration decision**

For the proposed direct import on Godot's existing VkDevice, enable the missing
extensions before device creation and expose their enabled state. An ordinary
extension cannot add capabilities retroactively. The matching engine source's
initialization/wrapping path agrees with the capture; wrapping an image does not
import its backing memory or establish its external synchronization.
[Matching Vulkan source](https://github.com/godotengine/godot/blob/ed1daf0bf001b61586d9930840f2f1394092c079/drivers/vulkan/rendering_device_driver_vulkan.cpp).

The supplied engine exposes external-image wrapping but no enabled-extension
inspection method in the runtime check. Its global RD also rejects direct
`submit()`/`sync()` use, so these cannot provide the frame handoff.
[Matching RD source](https://github.com/godotengine/godot/blob/ed1daf0bf001b61586d9930840f2f1394092c079/servers/rendering/rendering_device.cpp).

Prepare an isolated source build at the exact engine commit, then add optional
extension requests plus introspection. Review foreign/external queue ownership
requirements for the chosen GLES2/Vulkan producer; enabling the four baseline
extensions is not a complete ownership contract. Keep stock Godot and the SHM
demo usable. Re-run this audit on the patched engine, then try one controlled
linear DMA-BUF with explicit producer completion and receiver completion before
involving real application buffers. Render-thread access, layout/queue ownership,
submission ordering and safe resource retirement are separate acceptance tests.
The upstream additional-extension PR remains a reference rather than an assumed
shipped API. [PR #114940](https://github.com/godotengine/godot/pull/114940).
