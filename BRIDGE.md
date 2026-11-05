# One-frame SHM bridge

The next step, receiving this frame in a C++ Godot extension and displaying it
in 2D and 3D, is now available in [godot-bridge/README.md](godot-bridge/README.md).

Build with `make dwl`. No install or desktop restart is required.

From the repository directory, start a private receiver in terminal A:

```sh
bridge_dir=$(mktemp -d "${XDG_RUNTIME_DIR}/dwl-bridge.XXXXXX")
echo "$bridge_dir"
python3 tools/bridge-receiver.py --socket "$bridge_dir/frame.sock" --output "$bridge_dir/received.ppm"
```

In terminal B, substitute the printed directory, then run the nested compositor:

```sh
export DWL_BRIDGE_SOCKET=/run/user/1000/dwl-bridge.XXXXXX/frame.sock
WLR_BACKENDS=wayland ./dwl -d -s foot 2>/tmp/dwl-bridge-trace.log
```

In terminal C, from the same directory where nested dwl was launched:

```sh
touch bridge-send.request
```

Type in the nested foot window to submit content. The receiver saves one image,
exits, and removes its socket. For a byte-for-byte comparison, create both
requests **before** triggering a commit:

```sh
touch bridge-send.request bridge-capture.request
# Trigger an update, wait for the receiver to report success, then:
cmp bridge-frame.ppm /run/user/1000/dwl-bridge.XXXXXX/received.ppm
rg -F '[bridge]' /tmp/dwl-bridge-trace.log
```

This comparison overwrites the existing diagnostic `bridge-frame.ppm`. Launch
from a temporary working directory with an absolute dwl path to preserve it.
Restart the receiver at the same socket path for another attempt. If the receiver
is absent, the send request stays pending and is retried on eligible commits.
Once queued, an attempt consumes the request: disconnect or timeout discards the
snapshot. Create another request to retry. A new request while busy waits for a
later surface commit; frames are never queued behind the current one.

## Ownership and event flow

`client_commit` selects the next supported native main-surface SHM submission.
Capture locks the source, begins read access, copies pixels to packed RGB888,
ends access, and unlocks. It never adds the SHM offset to the returned pointer.
The sender takes ownership of the RGB allocation only when setup succeeds.
It holds no Client, surface, wlroots buffer, or borrowed FD afterward, so a
window can disappear during sending. Cleanup removes event sources, closes the
socket, and frees the RGB copy on completion, error, timeout, or compositor exit.

The sender uses a nonblocking Unix stream and at most 64 KiB per writable
callback. Short writes, EAGAIN, EINTR, missing receivers, and disconnects are
handled. One snapshot is limited to 64 MiB and 8192 pixels per dimension, with a
two-second total transfer deadline. These bounds include the synchronous CPU
copy; they do not make that copy asynchronous. Transport-only requests do no
disk image writing in dwl. The old explicit capture request still does synchronous
disk I/O and should only be used diagnostically.

The receiver binds a mode-0600 socket in an owned mode-0700 directory and refuses
to replace an existing socket. Use a private directory under XDG_RUNTIME_DIR.
The environment variable is an explicit opt-in, trusted receiver address.
No listening socket is added to dwl. This protocol is available to the same user;
it is not a security boundary against other processes running as that user.

## Protocol v1

One connection carries one header and one payload, followed by sender close.
All eight header fields are unsigned 32-bit integers in network byte order:

| Field | Value |
| --- | --- |
| Magic | `0x44574c42` (`DWLB`) |
| Version | 1 |
| Header size | 32 bytes |
| Width | 1–8192 pixels |
| Height | 1–8192 pixels |
| Pixel format | 1 = RGB888, byte order R, G, B |
| Stride | width × 3, no padding |
| Payload size | stride × height, at most 64 MiB |

The receiver validates every field before creating its temporary output. It reads
at most 64 KiB at a time, rejects truncation and trailing bytes, and atomically
replaces the output only on success. Socket operations time out after five
seconds of inactivity. A sender completion log means bytes reached the socket;
the receiver's success message confirms image saving. There is no acknowledgement.

Chunked stream transport is intentionally simple and easy to inspect, at the
cost of kernel copies. An owned shared-memory allocation passed via SCM_RIGHTS
would avoid sending the pixel payload through the socket, but adds FD transfer,
mapping, and lifetime rules. This version sends pixel bytes and never transfers
FD numbers or GPU handles.

This remains SHM-only, main-surface-only, submitted-content capture. Alpha,
subsurfaces, popups, presentation tracking, DMA-BUF and Godot import are outside
this milestone. App IDs `Godot` and `org.godotengine.Godot` are excluded; set
`DWL_BRIDGE_EXCLUDE_APP_ID` to the actual Godot application's app ID if different
(before launching dwl). The installed Godot version and app ID are not verified.

## Automated transport checks

```sh
cc -I. $(pkg-config --cflags wayland-server wlroots-0.19) \
  -o /tmp/bridge-sender tests/bridge-sender.c bridge.c \
  $(pkg-config --libs wayland-server wlroots-0.19)
python3 tests/test_bridge.py /tmp/bridge-sender
```

These exercise the actual C event-loop sender with a 3 MiB image, compare every
received byte, test receiver absence and disconnects, and verify timeout with
continued event-loop dispatch under backpressure. Receiver checks cover split
headers, truncation, extra bytes, invalid metadata and oversized payloads, and
preservation of an existing output on failure. They do not exercise wlroots
capture; use the nested graphical comparison above for that.

Verified during implementation on the installed wlroots 0.19.3: `make dwl`
succeeded (one existing configuration-related unused `setlayout` warning), all
four transport test groups passed, and a temporary nested Wayland-backend dwl
with foot produced a 684×473 capture whose received PPM matched byte for byte.
The stalled-peer check completed at 2.000 seconds with 198 event-loop dispatches.
The nested test used its own working directory and stopped only the compositor
it started. Godot and DMA-BUF were not tested.
