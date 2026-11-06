Godot-managed single-window development session

This project makes the working GPU bridge usable through ordinary Godot Play.
Godot starts one isolated nested dwl and one configured native application. Its
content appears on the editable ApplicationWindow mesh and a TextureRect. The
existing SHM/editor and GPU proof demos in `../demo/` remain separate references.

Build from `/home/skully/projects/SkullyWM_Godot`:

```sh
make dwl
cmake -S godot-bridge -B godot-bridge/build
cmake --build godot-bridge/build -j 6
python3 engine/build.py --jobs 6
```

The last command updates/builds the separate patched editor, including the
Wayland shutdown wakeup fix. No install, desktop restart, commit or push is
performed. Close project instances before rebuilding the native extension.

Open `project.godot` with our patched editor. You can register
`engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge` in Godot's project
manager or open it once from the repository root:

```sh
./engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge \
  --editor --display-driver wayland --path godot-bridge/world
```

The project saves Mobile/Vulkan and the five required device extension requests.
Press **F5 / Play**. It launches the configured application inside nested dwl;
there is no Python runner or manual socket setup. **Start session** and **Stop
session** allow restart without closing the world. Closing the game window waits
for session cleanup. The editor's **F8 / Stop** forcibly kills the game on Linux;
the native guardian independently cleans up its session when the lease closes.

The nested dwl output remains visible for keyboard/mouse interaction. Input
through the mesh and removing this flat representation are later milestones.
An idle terminal may show only one update until you type; the rate limit does
not force applications to redraw.

Use native Wayland for both editor and game. In **Debug > Customize Run
Instances**, set Run Arguments to `--gpu-index 0 --display-driver wayland`
(adjust the GPU index if needed). The earlier saved preset only selected the
GPU. Editor display selection can depend on its own preferences; forcing the
editor flag above avoids starting it through XWayland. To verify the game,
`print(DisplayServer.get_name())` should print `Wayland`. Vulkan is the rendering
driver, a separate choice from the Wayland/X11 display driver.

Repeated `display_server_x11.cpp` / `BadWindow` / opcode 20 messages indicate
an X11 GetProperty request against an invalid window ID. Similar Godot focus
and embedded-game reports exist, but the generic error does not identify its
exact caller. Start both processes on native Wayland before investigating an
engine patch; do not suppress the error or treat it as a DMA-BUF failure.

Choose your own test program

Open `development_world.tscn` and select **BridgeSession** in the scene tree.
Change these Inspector properties, save, then press Play:

| Property | Meaning |
| --- | --- |
| Application Arguments | Executable followed by one argument per array entry; not a shell command string. |
| Application App Id | Exact native Wayland app ID exported by dwl. |
| Maximum Fps | Upper export rate, 1–60; not a guaranteed presentation rate. |
| Dwl Executable | Defaults to `../../dwl`, relative to this project directory. |
| Auto Start | Start when the running scene enters the tree. No editor `@tool` execution. |

Examples:

```gdscript
# Default terminal, with an explicit app ID.
application_arguments = PackedStringArray([
    "foot", "--app-id=bridge-world-test", "-o", "font=monospace:size=18"
])
application_app_id = "bridge-world-test"

# A terminal running top.
application_arguments = PackedStringArray([
    "foot", "--app-id=world-top", "--", "top"
])
application_app_id = "world-top"

# A native GPU test application.
application_arguments = PackedStringArray(["weston-simple-egl"])
application_app_id = "org.freedesktop.weston.simple-egl"
```

Ordinary arguments are quoted literally for dwl's existing `-s` interface. If you
intend to run a shell script, explicitly provide `"sh", "-c", "your script"` as
application arguments (or as the program run inside foot). No separate
`launch_application()` control command exists in this milestone. Stop the
session, change its configured application, and start again.

GPU selection

The saved editor Run Arguments are `--gpu-index 0`, matching the tested AMD
render node on this machine. Editor Play applies these arguments to the game;
the editor itself can use another device. Check **Debug > Customize Run
Instances** to change the Run Arguments on another machine. This is a local
development preset, not portable GPU auto-selection. DMA-BUF sharing still
requires the Godot game and dwl to use the same supported DRM render device.

A direct runtime launch does not apply editor Run Arguments:

```sh
./engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge \
  --path godot-bridge/world --display-driver wayland --gpu-index 0
```

Ownership and events

1. `bridge/session.gd` owns configuration, a transient Texture2DRD, status and
   frame signals. Your world scripts can call `start()`/`stop()`, use `texture`,
   and connect `texture_changed(texture)`, `frame_ready(size, updates)`,
   `status_changed(message)`, `stopped`. Connect `texture_changed` to rebind your
   mesh/material when a new session starts. The Resource stays the same during
   that run; a restart creates a fresh one because clearing Texture2DRD destroys
   its RenderingServer handle. `development_world.gd` demonstrates this binding.
2. `BridgeSessionProcess.prepare()` validates bounded arguments and creates a
   mode-0700 `/tmp/dwl-godot-session-*` directory. Nothing is launched yet.
3. `session_gpu.gd` checks the actual Vulkan device on the rendering thread and
   configures the existing receiver. Main starts its socket before starting dwl.
4. The native extension uses `posix_spawn` with a child-only environment and
   closes unrelated inherited descriptors. It starts `bin/bridge_session_host`
   with a private nonblocking Unix socket lease. The helper starts dwl, whose
   normal startup path supplies the application's correct Wayland environment.
5. Frame transport is unchanged: applied surface texture → compositor-owned
   export DMA-BUF → asynchronous GPU copy → completed Godot RID → Texture2DRD.
   There is one bridge export/copy in flight; normal operation reads no pixels
   back to the CPU and writes no per-frame progress/image files.
6. Stop closes the lease and stops reception, clears the displayed binding,
   and queues render-thread discard/free. Pending imports remain engine-owned
   until GPU completion. Main polls retirement without a per-frame GPU wait.
7. The Linux helper is a subreaper. It sends TERM to its own direct children,
   adopts descendants when their parents exit, and repeats; after 1.5 seconds
   it uses KILL. It reaps children before exiting and removes `gpu.sock`.
   Cleanup never signals unrelated desktop processes or trusts stale process
   group IDs. Forced game exit also closes the lease, even without scene cleanup.

The helper repeatedly reads `/proc/.../children`, rather than treating that file
as a complete census: its final condition is `waitpid` reporting ECHILD. This
also handles startup descendants that create their own sessions. Programs
activated through an existing external service are not necessarily descendants;
this milestone manages the process tree it launches, not arbitrary applications
elsewhere in the desktop. Uninterruptible kernel tasks/GPU hangs can delay final
cleanup; there is no complete device-loss recovery implementation.

Private directories and `dwl.log` are retained for diagnosis. The Godot Output
panel prints their path. Startup failures and missing first frames produce a
status message; check native app ID, executable, GPU selection and log. This
project currently supports one native main surface, with the existing opaque
material and export-size limits. Multi-window lifecycle, input, GPU editor
preview, direct sampling and export packaging are not implemented here.

Research and verification

Before implementation we reviewed the pinned engine's editor Stop implementation
(`EditorRun::stop` → `OS_Unix::kill` → SIGKILL), process creation, render-thread
callbacks, copy retirement and dwl's startup/cleanup. The supervisor uses
[posix_spawn](https://man7.org/linux/man-pages/man3/posix_spawn.3.html),
[Linux subreaping](https://man7.org/linux/man-pages/man2/PR_SET_CHILD_SUBREAPER.2const.html),
and repeated [child snapshots](https://man7.org/linux/man-pages/man5/proc_tid_children.5.html).
The engine shutdown race is documented separately in [engine/README.md](../../engine/README.md).

```sh
python3 -B tests/test_session_process.py
# Graphical Wayland/GPU access required; opens only isolated test instances:
python3 -B tests/test_bridge_session.py
python3 -B tests/test_bridge_session.py --shutdown-only
```

The headless process test checks literal arguments, environment isolation,
repeated restart, stubborn descendants and owner SIGKILL. The graphical tests
exercise actual GPU textures with foot and weston-simple-egl, normal stop/restart,
and real editor Play/Stop/Play, with core and synchronization validation enabled.
They also check the rendered mesh binding after restart. Diagnostic
readbacks/screenshots are test-only.
The shutdown-only test uses an empty project without a compositor session.

This is a source development project. Matching patched export templates and a
packaged compositor helper remain a later milestone; do not use stock export
templates and expect the custom Vulkan APIs to be present.
