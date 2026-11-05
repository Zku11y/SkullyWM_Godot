#!/usr/bin/env python3
"""Preview a chosen native Wayland application through dwl's owned DMA-BUF export.

Interactive by default; Ctrl+C stops only this runner's Godot/nested dwl.
--auto-exit --verify adds diagnostic readbacks and exact export comparisons.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import signal
import shutil
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge"
DEFAULT_APP_ID = "bridge-gpu-test"
DEFAULT_COMMAND = (
    "foot --app-id=bridge-gpu-test -o font=monospace:size=16 -- sh -c '"
    "i=0; while true; do i=$((i+1)); "
    "printf \"\\033[H\\033[2JReal foot window through a DMA-BUF export\\n"
    "Update %s\\n\\033[31mRED \\033[32mGREEN \\033[34mBLUE\\033[0m\\n\" \"$i\"; "
    "sleep 0.08; done'"
)


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
    parser.add_argument("--command", help="application shell command, launched inside the nested compositor")
    parser.add_argument("--app-id", help="exact native xdg app ID (required with --command)")
    parser.add_argument("--fps", type=int, choices=range(1, 61), default=10,
                        help="maximum export rate, 1–60; actual updates depend on the client and copy pipeline")
    parser.add_argument("--gpu-index", type=int, default=0, help="Godot GPU index; must match dwl's DRM render device")
    parser.add_argument("--engine", type=Path, default=ENGINE)
    parser.add_argument("--auto-exit", action="store_true", help="finite automated run instead of waiting for Ctrl+C")
    parser.add_argument("--frames", type=int, default=8, help="frames required in --auto-exit mode")
    parser.add_argument("--verify", action="store_true", help="test-only readback of every export and copied texture; requires --auto-exit")
    parser.add_argument("--validation", action="store_true", help="enable Khronos Vulkan core and synchronization validation")
    parser.add_argument("--expect-input", choices=("SHM", "DMA-BUF"), help="automated assertion about actual application submissions")
    parser.add_argument("--timeout", type=float, default=40, help="finite-test deadline; interactive mode has no frame deadline")
    args = parser.parse_args()
    if args.command and not args.app_id:
        parser.error("--command requires the application's exact --app-id")
    if args.verify and not args.auto_exit:
        parser.error("--verify requires --auto-exit; interactive operation skips pixel readback")
    if args.frames < 1 or args.timeout <= 0:
        parser.error("--frames and --timeout must be positive")
    app_id = args.app_id or DEFAULT_APP_ID
    if app_id in ("Godot", "org.godotengine.Godot"):
        parser.error("Godot viewers are excluded from export")
    for path in (args.engine, REPO / "dwl", REPO / "godot-bridge/demo/bin/libdwl_bridge.so"):
        if not path.is_file():
            parser.error(f"Build dependency missing: {path}")
    folder = Path(tempfile.mkdtemp(prefix="dwl-gpu-client-"))
    print(f"Artifacts: {folder}", flush=True)
    project = folder / "project"
    shutil.copytree(REPO / "godot-bridge/demo", project,
                    ignore=shutil.ignore_patterns("bin", ".godot", "addons"))
    (project / "bin").mkdir()
    (project / "bin/libdwl_bridge.so").symlink_to(REPO / "godot-bridge/demo/bin/libdwl_bridge.so")
    (project / ".godot").mkdir()
    (project / ".godot/extension_list.cfg").write_text("res://bridge.gdextension\n")
    with (project / "project.godot").open("a") as file:
        file.write('\nrendering_device/vulkan/additional_device_extensions=PackedStringArray("VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd", "VK_EXT_queue_family_foreign")\n')
    env = os.environ.copy()
    # Isolate prior experiments and request files; do not inherit SHM live export.
    for key in list(env):
        if key.startswith(("DWL_BRIDGE_", "VK_")):
            del env[key]
    env.update(XDG_DATA_HOME=str(folder / "data"), XDG_CONFIG_HOME=str(folder / "config"),
               XDG_CACHE_HOME=str(folder / "cache"))
    if args.validation:
        sdk = Path(os.environ.get("VULKAN_SDK", "/nonexistent"))
        layer = sdk / "share/vulkan/explicit_layer.d"
        if not (layer / "VkLayer_khronos_validation.json").exists():
            layer = Path("/usr/share/vulkan/explicit_layer.d")
        if not (layer / "VkLayer_khronos_validation.json").exists():
            parser.error("Khronos validation layer unavailable; set VULKAN_SDK or omit --validation")
        (folder / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
        env.update(VK_LAYER_PATH=str(layer), VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation",
                   VK_LOADER_DEBUG="layer", VK_LAYER_SETTINGS_PATH=str(folder))
    command = args.command or DEFAULT_COMMAND
    (folder / "launch.json").write_text(json.dumps({"command": command, "app_id": app_id,
        "fps": args.fps, "gpu_index": args.gpu_index, "verify": args.verify,
        "auto_exit": args.auto_exit, "engine": str(args.engine.resolve())}, indent=2) + "\n")
    viewer = nested = None
    startup_group = None
    reported_identity = False
    reported_windows = False
    with (folder / "godot.log").open("w") as godot_log, (folder / "dwl.log").open("w") as dwl_log:
        try:
            viewer = subprocess.Popen([str(args.engine.resolve()), "--path", str(project),
                "--display-driver", "wayland", "--rendering-method", "mobile", "--rendering-driver", "vulkan",
                "--gpu-index", str(args.gpu_index), "--audio-driver", "Dummy", "res://dmabuf_proof.tscn", "--",
                "--compositor", f"--socket={folder / 'gpu.sock'}", f"--artifacts={folder}", f"--frames={args.frames}",
                *([] if args.auto_exit else ["--live"]), *(["--verify-readback"] if args.verify else [])],
                env=env, stdout=godot_log, stderr=subprocess.STDOUT, start_new_session=True)
            deadline = time.monotonic() + 20
            while not (folder / "ready.json").exists():
                if viewer.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError(f"Godot did not initialize; inspect {folder / 'godot.log'}")
                time.sleep(.02)
            nested_env = env.copy()
            nested_env.update(WLR_BACKENDS="wayland", WLR_RENDERER="gles2",
                DWL_BRIDGE_GPU_SOCKET=str(folder / "gpu.sock"), DWL_BRIDGE_GPU_APP_ID=app_id,
                DWL_BRIDGE_FPS=str(args.fps))
            if args.verify:
                nested_env["DWL_BRIDGE_GPU_REFERENCE_DIR"] = str(folder)
            wrapped = "printf '%s\\n' \"$$\" > " + shlex.quote(str(folder / "startup.pid")) + "; exec /bin/sh -c " + shlex.quote(command)
            nested = subprocess.Popen([str(REPO / "dwl"), "-d", "-s", wrapped],
                cwd=folder, env=nested_env, stdout=subprocess.DEVNULL, stderr=dwl_log, start_new_session=True)
            (folder / "processes.json").write_text(json.dumps({"godot": viewer.pid, "nested_dwl": nested.pid}) + "\n")
            print(f"Program: {command}\nNative app ID: {app_id}; up to {args.fps} FPS", flush=True)
            print("Click the application in nested dwl to interact; input through the Godot mesh is not implemented.", flush=True)
            print("Press Ctrl+C here to stop the preview and its nested compositor.", flush=True)
            deadline = time.monotonic() + args.timeout
            while viewer.poll() is None:
                if nested.poll() is not None:
                    raise RuntimeError(f"Nested dwl exited; inspect {folder / 'dwl.log'}")
                if not reported_identity or not reported_windows:
                    current_log = (folder / "dwl.log").read_text(errors="replace")
                    if "[gpu-bridge] Cannot enable" in current_log or "No compatible LINEAR" in current_log:
                        raise RuntimeError(f"Export unsupported by the current renderer/allocator; inspect {folder / 'dwl.log'}")
                    identity = re.search(r"ENABLED .*DRM=(\d+):(\d+)", current_log)
                    if identity and not reported_identity:
                        ready = json.loads((folder / "ready.json").read_text())
                        actual = tuple(map(int, identity.groups()))
                        expected = (ready["drm_major"], ready["drm_minor"])
                        if actual != expected:
                            raise RuntimeError(f"GPU mismatch: dwl DRM {actual}, Godot DRM {expected}; select a matching --gpu-index")
                        reported_identity = True
                    mapped = re.findall(r"MAP .*app-id=(.*)", current_log)
                    if mapped and not reported_windows:
                        print(f"Mapped native app IDs: {mapped}", flush=True)
                        reported_windows = True
                if args.auto_exit and time.monotonic() > deadline:
                    mapped = re.findall(r"MAP .*app-id=(.*)", (folder / "dwl.log").read_text(errors="replace"))
                    raise RuntimeError(f"Timed out waiting for '{app_id}'. Mapped native app IDs: {mapped}. Inspect {folder / 'dwl.log'}")
                time.sleep(.05)
            if not args.auto_exit:
                if viewer.returncode != 0:
                    raise RuntimeError(f"Godot exited {viewer.returncode}; inspect {folder / 'godot.log'}")
                print("Preview closed.", flush=True)
                return
            result = json.loads((folder / "result.json").read_text())
            assert viewer.returncode == 0 and result["success"] and result["copied"] == args.frames, result
            assert (folder / "screenshot.png").stat().st_size > 1000
            # Let the compositor dispatch its own producer-fence callbacks before reading artifacts.
            time.sleep(.1)
            dwl_text = (folder / "dwl.log").read_text(errors="replace")
            godot_text = (folder / "godot.log").read_text(errors="replace")
            assert "[gpu-bridge] ENABLED" in dwl_text and "[gpu-bridge] CONNECT matched" in dwl_text, dwl_text
            assert dwl_text.count("[gpu-bridge] SEND frame=") >= args.frames
            assert "SCRIPT ERROR" not in godot_text and "was leaked" not in godot_text and "were leaked" not in godot_text
            if args.validation:
                assert 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in godot_text
                for log in (godot_text, dwl_text):
                    assert not re.search(r"Validation Error|VUID-|SYNC-HAZARD", log), log
            if args.expect_input:
                assert f"input={args.expect_input}" in dwl_text, f"No confirmed {args.expect_input} application submission"
            if args.verify:
                for frame in range(args.frames):
                    source = (folder / f"export-{frame}.rgba").read_bytes()
                    target = (folder / f"frame-{frame}.rgba").read_bytes()
                    assert source == target, f"Export/copy pixels differ at frame {frame}"
            print(f"PASS: {args.frames} real application frames through compositor DMA-BUF exports"
                  + (", every byte matched" if args.verify else ", no texture readback")
                  + (", Vulkan validation clean" if args.validation else ""), flush=True)
        except KeyboardInterrupt:
            print("\nStopping this preview and nested compositor.", flush=True)
        finally:
            stop(viewer)
            # dwl starts the application shell in its own session. Stop that
            # known group while its Wayland server is still alive, then dwl.
            pid_file = folder / "startup.pid"
            if pid_file.exists():
                startup_group = int(pid_file.read_text().strip())
                for action in (signal.SIGTERM, signal.SIGKILL):
                    try:
                        os.killpg(startup_group, action)
                    except ProcessLookupError:
                        break
                    if action == signal.SIGTERM:
                        time.sleep(.15)
            stop(nested)


if __name__ == "__main__":
    main()
