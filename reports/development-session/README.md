Single-window Godot development session — verified 2026-11-06

This checkpoint adds Godot-owned startup/stop around the existing frame bridge.
It does not implement multiple windows, mesh input or direct client sampling.
No compositor config replacement, install, active desktop restart or push occurred.
See [the user guide](../../godot-bridge/world/README.md).

Verified on this machine:

- `make dwl` (existing target up to date), CMake native library/supervisor build,
  and a rebuilt pinned Godot editor with engine patch 0006.
- `tests/test_session_process.py`: three start/stop cycles; literal spaces,
  quotes, newline, shell metacharacters and empty arguments; child-only env;
  no inherited application sockets; unrelated process survives; TERM-resistant
  descendants using setsid are killed/reaped; owner SIGKILL and owner-object
  removal clean their processes. Unsupported renderer, missing executable and
  cancellation before initialization completion leave no running session.
- `tests/test_bridge_session.py`: SHM foot followed by DMA-BUF weston-simple-egl
  through the same session node, at least 12 updates each; nonempty GPU texture
  readbacks and screenshots; stop/restart, process/socket cleanup and normal
  engine exit. The rendered mesh center is checked after restart, and screenshots
  were visually inspected. That caught a stale material handle after clearing
  Texture2DRD: each new session now publishes a fresh Resource through
  `texture_changed`, and the world rebinds it. Readbacks are test-only;
  no CPU pixel readback is added to ordinary session operation.
- The same test invokes actual EditorInterface Play/Stop/Play. Both games and
  their owned process trees disappear after Stop. The editor selected NVIDIA
  RTX 4070; project Run Arguments selected AMD 780M for each game. These are
  independent devices; shared DMA-BUFs still stay between dwl and the AMD game.
- Validation-layer loading was confirmed in logs; no Vulkan validation errors,
  VUIDs, synchronization hazards, GDScript errors or runtime RID/object leak
  warnings were found in the final run. Synchronization validation is explicitly
  enabled by the saved `vk_layer_settings.txt`. An earlier fixture forced draws
  without presentation and triggered attachment-write hazards; using the normal
  presented-frame screenshot path removed those test-induced hazards. The GPU
  format/copy restrictions remain unchanged.
- `tools/run_gpu_client.py --auto-exit --frames 8 --verify --validation`: all
  eight original export/copied-frame comparisons matched every byte; clean exit.
- `tests/test_godot_bridge.py`: four original SHM test groups passed.

Shutdown investigation:

Before patch 0006, repeated runtime tests sometimes completed session cleanup
and printed their pass marker but remained alive during engine finalization.
An empty SceneTree without a session also hung on its fourth attempted exit.
The saved debugger trace shows the Wayland event thread in poll while main is
waiting. The editor binary is stripped, so the trace does not resolve internal
Godot symbols; the pinned source and upstream issue identify the matching race.
See [upstream #123059](https://github.com/godotengine/godot/issues/123059) and
[PR #123946](https://github.com/godotengine/godot/pull/123946).

Our local wakeup-pipe patch avoids relying on the main/event-thread race for a
Wayland sync reply. The original template/build configuration is retained.
`tests/test_bridge_session.py --shutdown-only` then passed ten empty-project
Wayland exits, followed by passing runtime/editor lifecycle tests. This is
evidence for the specific reproduced race, not a general GPU-hang recovery claim.

The manifest records source/build hashes and original artifact directories.
Saved logs contain local test paths/device names; no multi-megabyte pixel dumps
are copied into this report. The execution sandbox isolates process/graphics
access, so real lifecycle/GPU tests ran with explicit approval outside it.

Known boundaries: one selected native main surface, same supported GPU/render
node for dwl and the game, opaque mesh material, existing export bounds, visible
nested dwl for input, no packaged export templates. Private session logs are
retained in `/tmp`; normal stop removes the transport socket. Uninterruptible
kernel tasks/device loss and application activation through external existing
services remain outside this process-tree cleanup guarantee.
