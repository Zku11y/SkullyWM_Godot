60 FPS runtime GPU follow-up

The 30 FPS ceiling was enforced by the Python runner and GPU helper, not the
DMA-BUF protocol or Godot texture API. Both now allow 1–60; default remains 10.
The helper uses nanosecond phase deadlines so 60 Hz is not rounded to 17 ms,
and a 1 ms timer above 30 FPS removes the old 10 ms scheduling granularity.
Missed slots are skipped rather than queued. One export and its existing
producer/consumer release rules still bound the pipeline.

Compiled with `make dwl`. A live, no-readback weston-simple-egl test with
`--fps 60 --validation` copied at least 120 frames, measured 59.98 exported
updates/s over the first 120 exports, reported no Vulkan core/synchronization
validation errors, and stopped its own Godot/nested-dwl/application processes
when the Python runner received Ctrl+C. This is export/copy throughput, not a
measurement of presented frames, and not a guarantee for every client or GPU.
An application must submit changing content to produce new exported frames.

The first finite `--auto-exit --frames 120` attempt also completed all copies at
about 60 updates/s and wrote a successful result/screenshot, but the engine
process did not exit before the harness deadline after its cleanup messages.
That run is retained as an exit-stall observation, not a passing finite test.
The passing follow-up exercised the interactive mode and Ctrl+C used by the
user-facing command; engine-exit diagnosis was not part of this rate change.

The nested receiver-absence/wrong-device/backpressure/disconnect regression
passed with the new pacing. Parser boundaries reject 0 and 61. The separate
legacy SHM editor runner is unchanged; this change applies to
`tools/run_gpu_client.py` and `gpu_bridge.c`.

```sh
python3 tools/run_gpu_client.py --fps 60 \
  --command 'weston-simple-egl' --app-id org.freedesktop.weston.simple-egl
```

Logs, measured throughput, launch metadata and exact component hashes are saved
here. Earlier checkpoint manifests retain their original hashes.
