Godot SHM bridge — snapshots and bounded live updates

The receiver accepts DWLB v1 RGB snapshots in a Linux C++ GDExtension and
displays an ImageTexture in both a TextureRect and a tilted QuadMesh. It can
listen once or continuously. The project runs with Godot's Compatibility renderer
to keep this CPU upload milestone independent of Vulkan interoperability. No engine patch is
needed for this step. The reusable `window_preview.tscn` scene also updates in
the editor's 3D viewport.

**Build**

Requirements: CMake 3.20+, a C++17 compiler, Python 3, Git, and the supplied Linux
Godot executable. No system installation is required. From the repository root:

```sh
# First build only; skip clone if the dependency is already present.
git clone --depth 1 --branch godot-4.5-stable \
  https://github.com/godotengine/godot-cpp.git godot-bridge/vendor/godot-cpp
git -C godot-bridge/vendor/godot-cpp rev-parse HEAD
# Expected: e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77
cmake -S godot-bridge -B godot-bridge/build -DCMAKE_BUILD_TYPE=Debug
cmake --build godot-bridge/build -j 4
```

The extension uses the pinned 4.5 binding/API baseline and was exercised in the
provided executable reporting 4.7.2.stable.official.ed1daf0bf. It uses only this
older API subset. This does not establish compatibility with every engine build.
The dependency and build outputs are ignored; source is not bundled into dwl.
The bindings retain their upstream license in vendor/godot-cpp/LICENSE.md.

Output: `godot-bridge/demo/bin/libdwl_bridge.so`. The `.gdextension` file tells
Godot which shared library to load and which initialization function to call.
On a fresh checkout, import the project once to discover the extension:

```sh
./godot/Godot_v4.7.2-stable_linux.x86_64 --headless --editor \
  --path godot-bridge/demo --import
```

**Run manually**

Terminal A, from the repository root:

```sh
bridge_dir=$(mktemp -d "$XDG_RUNTIME_DIR/dwl-godot.XXXXXX")
echo "$bridge_dir"
DWL_BRIDGE_SOCKET="$bridge_dir/frame.sock" \
  ./godot/Godot_v4.7.2-stable_linux.x86_64 --path godot-bridge/demo
```

Godot listens automatically when DWL_BRIDGE_SOCKET is set. Otherwise paste the
socket path into the demo and click **Start / reconnect**. The parent directory
must exist, belong to your user and have mode 0700. Existing socket paths are
never removed on startup. Do not run the Python receiver at the same address.

Terminal B, substitute the directory printed in A:

```sh
make dwl
capture_dir=$(mktemp -d /tmp/dwl-godot-capture.XXXXXX)
echo "$capture_dir"
cd "$capture_dir"
DWL_BRIDGE_SOCKET=/run/user/1000/dwl-godot.XXXXXX/frame.sock \
  WLR_BACKENDS=wayland /home/skully/projects/SkullyWM_Godot/dwl -d -s foot \
  2>trace.log
```

Wait for foot to appear and show content. In terminal C, create
`bridge-send.request` inside the printed capture directory, then type in nested
foot. The next supported commit supplies the snapshot. Both Godot views should
show the same terminal contents. To capture another snapshot, click **Start / reconnect** again, create another request, and trigger another commit.

This temporary compositor working directory preserves the repository's existing
PPM and trace. Godot runs as a normal window under the existing desktop, while
foot runs under the nested test compositor. The bridge also has app-ID exclusions
for a viewer run inside nested dwl; verify the actual ID before relying on them.

**What happens inside**

1. `BridgeReceiver.start_listening()` validates the private path, binds a
   nonblocking Unix socket, and starts a worker thread.
2. The worker accepts one connection and checks the 32-byte header before pixel
   allocation: DWLB magic, v1, RGB888, dimensions, packed stride, and payload
   length. Dimensions are capped at 8192 and pixels at 64 MiB. Header, payload
   and EOF must arrive within five seconds of acceptance.
3. Short reads are accumulated. The worker uses only ordinary C++ storage and
   socket operations; it never creates scene objects or textures. One complete
   frame is moved into a mutex-protected handoff slot. Failure publishes a status
   message and discards partial pixels.
4. Godot's main thread calls `take_frame()`. This takes the owned RGB bytes once,
   creates an Image in RGB8 format and updates an ImageTexture.
   Matching dimensions use `update()`; a size change uses `set_image()`.
   The texture Resource remains the same object. Both the flat
   preview and mesh material reference that texture. The mesh aspect ratio is
   adjusted to the frame dimensions.
5. After each snapshot the worker closes the accepted connection. In one-shot
   mode it also closes the listener and removes its bound path. Live mode keeps
   listening and replaces the previous unread frame with the latest complete one.
   Stop or destruction cancels polling and joins the thread; polling checks
   cancellation at most roughly every 50 ms. Stop cancels network waiting and
   clears queued pixels. The already displayed texture remains available.

There is no wlroots pointer or borrowed client buffer in Godot. dwl already
copied those pixels into its own RGB allocation before sending. That independence
is why disconnect and timeout can safely discard this snapshot. DMA-BUF sharing
will require stronger GPU completion and reuse rules.

Receiving does not block the main/render threads on socket I/O. Creating the
PackedByteArray, Image and GPU texture still costs CPU time and memory on the
main thread. Temporary copies can exceed the 64 MiB payload cap; all are bounded
by the single-frame design. This is a correctness milestone; the modest rate cap does not guarantee
that large CPU copies or uploads meet a particular latency budget. Alpha is discarded by the existing RGB protocol.

**Verification**

```sh
python3 tests/test_godot_bridge.py
# Graphical session required; opens test windows and retains artifacts under /tmp:
python3 tests/test_godot_nested.py
```

The first command loads the real native extension in the supplied headless
engine and checks exact received bytes, split input, truncation, extra bytes,
oversized payload rejection, timeout, and cancellation. Headless tests do not
prove texture rendering. The second starts a temporary nested dwl with foot,
waits for populated terminal content, requests a frame, compares Godot's bytes
with dwl's PPM, and saves a screenshot of the actual TextureRect and 3D mesh.
It stops only processes it starts and retains logs/source/received data.

The graphical tests use Compatibility/OpenGL. Vulkan import, DMA-BUF,
window-tree composition, and input routing remain future work. Failed later requests leave the last texture
visible. Native library hot reload is disabled; restart Godot after rebuilding.


**Live runtime updates**

Start the Godot demo with DWL_BRIDGE_SOCKET as above and check **Live** before
clicking **Start / reconnect**. Alternatively set DWL_BRIDGE_LIVE_APP_ID=foot
in Godot's environment to select Live automatically. Start nested dwl with:

```sh
DWL_BRIDGE_SOCKET=/run/user/1000/dwl-godot.XXXXXX/frame.sock \
  DWL_BRIDGE_LIVE_APP_ID=foot DWL_BRIDGE_FPS=10 \
  WLR_BACKENDS=wayland /home/skully/projects/SkullyWM_Godot/dwl -d -s foot
```

Do this from the temporary capture directory. No request files are needed for
live updates. Set the app ID to the native application you intend to preview;
for this milestone run only one matching window. Multiple windows sharing that
ID can otherwise alternate in the preview. Default rate is 10; valid integer
rates are 1–30, and an invalid value disables automatic captures. This is an
upper bound on capture attempts, not a timer that makes idle clients redraw.
Updates occur only on eligible SHM buffer submissions. Unsupported formats and
GPU buffers still do not produce an image. A throttled last update needs a later
commit before it can be captured; final-state delivery is not guaranteed here.

The same v1 framing is used with one connection per image. dwl has one in-flight
snapshot and never queues another behind it. Godot has one worker receiving a
snapshot and one complete pending frame. If the main thread is paused, new
complete frames replace the unread one. `get_stats()` reports received/replaced
counts and pending bytes; it does not measure presentation. Each frame remains
limited to 64 MiB. Additional copies for reception, main-thread conversion and
GPU upload have bounded but larger combined memory cost. This is a single-user,
single-active-listener prototype; an occupied socket is refused. A receiver
restart is retried automatically on later eligible source commits.

**Live editor preview**

Open `godot-bridge/demo/project.godot` in the editor, then open
`window_preview.tscn`. Select the BridgeWindow root in the Scene tree. In the
Inspector set **Socket Path** to the private address, leave **Live** on, then
check **Enabled**. The mesh updates in the editor's 3D viewport without pressing
Play. You can also instance this scene into another 3D scene. Each instance needs
its own socket if enabled. Disable the running demo before using the same address
in the editor. Disable **Enabled** to stop; the last texture remains visible.

Scene exit/close stops and joins the receiver and removes the socket. Opening
a scene saved with Enabled on reconnects to its configured path. Runtime sockets,
threads, pixel storage, textures and generated mesh children are transient and
are not saved into the scene. Leave Enabled off in reusable saved scenes unless
that automatic connection is intentional. Native library hot reload is disabled;
close the editor before rebuilding the extension. A source/GDScript hot reload
while streaming is not a tested lifecycle guarantee; disable the preview first.
The existing default Godot app-ID exclusions remain in force; verify the actual
editor/game app IDs before placing a viewer inside the exporting compositor.

**Live checks completed**

```sh
python3 tests/test_godot_live.py
python3 tests/test_godot_nested.py --live
python3 tests/test_godot_editor.py --auto-exit
python3 tests/test_godot_editor.py --nested --auto-exit
```

The native test deliberately leaves frames unread, checks that exactly one slot
holds the newest image, rejects malformed input, restarts the listener, and stops
while a header is incomplete. The nested runtime test checks repeated updates,
capture pacing and texture reuse. The real editor test uses an isolated copy of
the project and a temporary test-only EditorPlugin; it checks image upload bytes,
same-size reuse, resized storage, and scene close/reopen cleanup. Its --nested
variant feeds the editor from a real foot client rather than synthetic pixels.
The scripts retain graphical logs/screenshots under /tmp and stop only their own
processes. All of these passed on the supplied engine during this milestone.

The source still uses the earlier submitted-content capture hook. It is not yet
a production applied-state/lifecycle protocol. Live editor preview does not imply
that input on the mesh reaches the application; input routing is a later step.


**Keep the editor preview open and choose a program**

The editor runner now stays open until Ctrl+C by default:

```sh
python3 tests/test_godot_editor.py
```

It starts the built-in animated foot demo inside a temporary nested dwl. Supply
an application command and its exact native Wayland app ID to test something
else. For example, a terminal running top with a distinct app ID:

```sh
python3 tests/test_godot_editor.py \
  --command 'foot --app-id=my-shm-test top' --app-id my-shm-test --fps 10
```

Replace the string after `--command` with your own application launch command,
and `--app-id` with the ID that application actually sets. The command is run
through dwl's startup shell inside the nested Wayland environment; shell quoting
and environment assignments are supported. App ID is not necessarily the
executable name or window title. If uncertain, prefix the command with
`WAYLAND_DEBUG=client` and look for `set_app_id` in the printed artifact directory's
`dwl.log`. For example: `rg set_app_id /tmp/dwl-godot-editor.XXXX/dwl.log` (use the
actual printed directory). Keep one matching window open. The selector currently
supports native main-surface SHM submissions; a correct app ID does not make a
DMA-BUF-only client capturable.

The interactive mode skips test-only GPU readbacks, per-frame marker files,
scene cycling and automatic editor shutdown. It retains the private project,
logs and working directory under the printed /tmp path. Ctrl+C stops its editor
and nested compositor. Closing the editor also ends the runner. Use `--auto-exit`
to retain the finite automated lifecycle test (`--nested --auto-exit` for real
foot; `--auto-exit` alone for synthetic resize/reuse checks).
