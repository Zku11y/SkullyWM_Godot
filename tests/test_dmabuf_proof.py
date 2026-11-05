"""Controlled GPU producer -> SCM_RIGHTS -> Godot import/GPU copy; graphical."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]


def expected_pixels(width, height, frame):
    colors = (bytes((255, 0, 0, 255)), bytes((0, 255, 0, 255)),
              bytes((0, 0, 255, 255)), bytes((255, 255, 0, 255)))
    rows = []
    for y in range(height):
        left = colors[((2 if y >= height // 2 else 0) + frame) % 4]
        right = colors[((2 if y >= height // 2 else 0) + 1 + frame) % 4]
        row = left * (width // 2) + right * (width - width // 2)
        if y < 16:
            row = bytes((255, 255, 255, 255)) * 16 + row[64:]
        rows.append(row)
    return b"".join(rows)


def stop(proc):
    if proc is not None and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--legacy", action="store_true", help="regression: original v1/synchronous copy path")
    parser.add_argument("--no-readback", action="store_true", help="display without diagnostic texture readback/pixel comparison")
    parser.add_argument("--gpu-index", type=int, default=0)
    parser.add_argument("--frames", type=int, choices=range(1, 33), default=4)
    parser.add_argument("--fault", choices=("size", "uuid", "extra-fd", "truncated", "empty", "disconnect", "bad-fence", "delayed", "cancel-acquire", "cancel-submit"))
    parser.add_argument("--engine", type=Path, default=REPO / "engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge")
    args = parser.parse_args()
    if args.legacy and args.fault:
        parser.error("--legacy and --fault cannot be combined")
    folder = Path(tempfile.mkdtemp(prefix="dwl-dmabuf-proof-"))
    print(f"Artifacts: {folder}", flush=True)
    project = folder / "project"
    shutil.copytree(REPO / "godot-bridge/demo", project, ignore=shutil.ignore_patterns("bin", ".godot", "addons"))
    (project / "bin").mkdir()
    (project / "bin/libdwl_bridge.so").symlink_to(REPO / "godot-bridge/demo/bin/libdwl_bridge.so")
    (project / ".godot").mkdir()
    (project / ".godot/extension_list.cfg").write_text("res://bridge.gdextension\n")
    with (project / "project.godot").open("a") as file:
        file.write('\nrendering_device/vulkan/additional_device_extensions=PackedStringArray("VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd", "VK_EXT_queue_family_foreign")\n')
    sdk = Path(os.environ.get("VULKAN_SDK", "/nonexistent"))
    layer = sdk / "share/vulkan/explicit_layer.d"
    if not (layer / "VkLayer_khronos_validation.json").exists():
        layer = Path("/usr/share/vulkan/explicit_layer.d")
    env = os.environ.copy()
    for key in list(env):
        if key.startswith("VK_"):
            del env[key]
    env.update(XDG_DATA_HOME=str(folder / "data"), XDG_CONFIG_HOME=str(folder / "config"),
               XDG_CACHE_HOME=str(folder / "cache"), VK_LAYER_PATH=str(layer),
               VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation", VK_LOADER_DEBUG="layer")
    # Enable synchronization validation in both processes as well as core validation.
    (folder / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    env["VK_LAYER_SETTINGS_PATH"] = str(folder)
    if args.fault in ("delayed", "cancel-acquire"):
        env["DWL_BRIDGE_TEST_DELAY_ACQUIRE"] = "1"
    else:
        env.pop("DWL_BRIDGE_TEST_DELAY_ACQUIRE", None)
    viewer = producer = None
    frames = 1 if args.fault else args.frames
    with (folder / "godot.log").open("w") as godot_log, (folder / "producer.log").open("w") as producer_log:
        try:
            viewer = subprocess.Popen([
                str(args.engine.resolve()), "--path", str(project), "--display-driver", "wayland",
                "--rendering-method", "mobile", "--rendering-driver", "vulkan", "--gpu-index", str(args.gpu_index),
                "--audio-driver", "Dummy", "res://dmabuf_proof.tscn", "--",
                f"--socket={folder / 'frame.sock'}", f"--artifacts={folder}", f"--frames={frames}",
                *([] if args.no_readback else ["--verify-readback"]),
                *(["--sync-copy"] if args.legacy else []),
                *(["--cancel-acquire"] if args.fault == "cancel-acquire" else []),
                *(["--cancel-submit"] if args.fault == "cancel-submit" else []),
            ], env=env, stdout=godot_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 20
            while not (folder / "ready.json").exists():
                assert viewer.poll() is None and time.monotonic() < deadline, (folder / "godot.log").read_text()
                time.sleep(.02)
            ready = json.loads((folder / "ready.json").read_text())
            command = [str(REPO / "dmabuf-proof/build/dmabuf-producer"), str(folder / "frame.sock"), ready["device_uuid"], str(frames)]
            if args.fault or args.legacy:
                command.append("legacy" if args.legacy else args.fault)
            producer = subprocess.Popen(command, env=env, stdout=producer_log, stderr=subprocess.STDOUT)
            viewer_code = viewer.wait(timeout=30)
            producer_code = producer.wait(timeout=15)
            result = json.loads((folder / "result.json").read_text())
            reject = args.fault in ("size", "uuid", "extra-fd", "truncated", "empty", "bad-fence")
            cancelled = args.fault in ("cancel-acquire", "cancel-submit")
            if cancelled:
                assert viewer_code == 0 and producer_code != 0 and result["success"] and result["copied"] == 0, result
                if args.fault == "cancel-acquire":
                    assert result["stop_ms"] < 250 and result["wait_frames"] >= 3, result
                else:
                    assert result["pending_copies"] == 0, result
            elif reject:
                assert viewer_code == 2 and producer_code != 0 and not result["success"], result
            else:
                assert viewer_code == 0 and producer_code == 0 and result["success"] and result["copied"] == frames, result
                for frame in range(0 if args.no_readback else frames):
                    assert (folder / f"frame-{frame}.rgba").read_bytes() == expected_pixels(512, 256, frame), f"Pixel mismatch at frame {frame}"
                assert (folder / "screenshot.png").stat().st_size > 1000
                if args.fault == "delayed":
                    assert result["wait_frames"] >= 3 and "ACQUIRE_SYNC_EXPORTED" in (folder / "producer.log").read_text(), result
                if not args.legacy and args.fault != "disconnect":
                    assert (folder / "producer.log").read_text().count("RELEASE_SYNC_IMPORTED") == frames
            for name in ("godot.log", "producer.log"):
                log = (folder / name).read_text()
                assert 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in log, f"Validation not confirmed in {name}"
                assert "Validation Error" not in log and "VUID-" not in log and "SYNC-HAZARD" not in log and "SCRIPT ERROR" not in log, log
                assert "was leaked" not in log and "were leaked" not in log, log
            assert not (folder / "frame.sock").exists(), "Receiver left its socket bound"
            print(f"PASS: {'cancelled safely: ' + args.fault if cancelled else 'rejected ' + args.fault if reject else str(frames) + (' GPU-displayed frames (no texture readback)' if args.no_readback else ' GPU-generated frames imported, GPU-copied and pixel-exact')}; both validation logs clean")
        finally:
            stop(producer)
            stop(viewer)


if __name__ == "__main__":
    main()
