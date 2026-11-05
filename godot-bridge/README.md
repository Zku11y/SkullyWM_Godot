One-frame Godot bridge

This milestone receives the existing DWLB v1 RGB snapshot directly in a Linux
C++ GDExtension and displays one ImageTexture in both a TextureRect and a tilted
QuadMesh. The project runs with Godot's Compatibility renderer to keep this CPU
upload milestone independent of Vulkan interoperability. No engine patch is
needed for this step. Live updates and live editor-viewport preview are later
milestones; open the project in the editor and press F6/F5 to run this demo.

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
socket path into the demo and click **Listen for one frame**. The parent directory
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
show the same terminal contents. To capture another snapshot, click **Listen for
one frame** again, create another request, and trigger another commit.

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
   creates an Image in RGB8 format and uploads an ImageTexture. Both the flat
   preview and mesh material reference that texture. The mesh aspect ratio is
   adjusted to the frame dimensions.
5. The worker closes its accepted/listening sockets and removes its bound path.
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
by the single-frame design. This is a correctness milestone, not a streaming
performance implementation. Alpha is discarded by the existing RGB protocol.

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

The graphical test uses Compatibility/OpenGL; Vulkan import, DMA-BUF, continuous
updates, window-tree composition, input routing, and in-editor live preview are
not implemented by this milestone. Failed later requests leave the last texture
visible. Native library hot reload is disabled; restart Godot after rebuilding.
