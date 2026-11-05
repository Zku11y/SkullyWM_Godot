Asynchronous copy/sync-file results — 2026-11-05

The controlled DMA-BUF proof now submits the copy and returns without waiting
for GPU completion on Godot's render thread. It transfers real acquire/release
sync-file FDs, retains one engine-owned import until completion, and publishes
only completed Godot-owned textures. This is a synchronization prerequisite;
real Wayland application DMA-BUFs are not connected by this milestone.

Verified device: AMD Radeon 780M Graphics, RADV PHOENIX, Godot GPU index 0.
Producer and Godot GPU/driver UUIDs match. The rebuilt Godot 4.7.2 editor uses
four local patches; the pinned godot-cpp bindings remain unchanged. NVIDIA
DMA-BUF import and cross-GPU transport remain untested.

| Case | Required evidence | Result |
| --- | --- | --- |
| Eight asynchronous frames | Every destination byte matches; release sync-file imported for every ACK | Passed |
| Eight asynchronous frames without destination readback | 2D/3D screenshot and eight release imports | Passed |
| Four legacy v1 frames | Original synchronous path remains pixel-exact | Passed |
| Invalid size, UUID, extra FD, truncated header, empty packet with FD | Clean rejection; no published frame | Passed |
| Invalid acquire FD type | DMA-BUF supplied as acquire descriptor rejected as non-sync-file | Passed |
| Producer disconnect | Retained allocation remains usable and pixel-exact | Passed |
| Delayed acquire consumption | Main-loop updates continue; completed frame matches reference | Passed |
| Stop during acquire wait | Zero copies, continued updates, stop under 250 ms | Passed |
| Discard after submission | Zero published frames; zero pending engine jobs after retirement | Passed |
| Nested dwl/foot SHM regression | Received RGB equals the PPM diagnostic byte for byte | Passed |

All controlled cases require loader-confirmed Khronos core/synchronization
validation in both processes, and fail on VUIDs, synchronization hazards, script
errors or reported RID leaks. Final cases passed. The source SHA-256s, engine
patch/build hashes and all three native binary hashes are recorded alongside
case logs/results. All four patches were replayed in order against pristine
pinned files using `git apply --index`, reproducing the six compiled source
files exactly. No compositor installation, restart, commit or push was performed.

The delayed/cancel-acquire tests hold the receiver worker for 700 ms before
consuming a real sync-file. This is a deterministic CPU-side test hold, not an
assertion that the GPU fence itself stayed unsignalled for 700 ms. The default
producer separately logs submissions that were still pending before send; it
does not wait on their fence before sending v2 frames. The acquire wait checks
sync-file status before making any source available to the render thread.

During implementation, validation caught reuse of a release semaphore payload
before observing completion through the producer's own Vulkan fence. The
producer now checks its fence before reimporting that semaphore. An artificial
unresolved timeline wait was also rejected by validation as unsuitable for
exporting a copy-transference sync-file; that experiment was removed. The final
delay test places no unresolved dependency on a GPU queue.

Cancellation exposed an idle-window cleanup issue. Discarded jobs can now be
retired through explicit nonblocking status checks even without normal redraws.
The test flushes render cleanup, then quits directly on the main thread. The
previously intermittent truncated-message exit was repeated after this fix.

**Ownership and limits**

The socket worker owns received FDs. Successful Vulkan memory import consumes
its DMA-BUF FD. Successful engine submission adopts the imported VkImage and
VkDeviceMemory; failure leaves them with the extension. Polling releases these
objects only after fence completion and returns an owned release FD, which the
worker sends/closes. Successful producer semaphore import consumes its received
release FD. Export returning the already-signalled -1 sentinel is represented
by a zero FD count, never by transmitting a raw descriptor number.

Discard does not mean GPU completion. It suppresses publication while the engine
retains resources for safe retirement. One engine job and one pending receiver
slot bound the pipeline. The existing 2048²/64 MiB, one-plane LINEAR RGBA8 and
same-device/driver restrictions remain. Destinations are still allocated per
frame. The normal finalization path can wait for GPU completion; GPU-hang
recovery and a complete editor preview teardown/reconnect contract are future
work. ACKs remain completion-based even though they now carry release fences.

See [dmabuf-proof/ASYNC.md](../../dmabuf-proof/ASYNC.md) for current build/run
commands, protocol v2 fields, function-by-function explanation and next steps.
The SHM path remains separate and working; no automatic path selection is
claimed. Next is a compositor-owned export pool, applied-state leases,
wlroots readiness and cross-API ownership/format negotiation before real GPU
client content reaches Godot.
