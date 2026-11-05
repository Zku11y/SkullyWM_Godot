"""Interactive editor preview until Ctrl+C; --auto-exit runs lifecycle assertions."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from test_godot_live import send, wait_for

REPO = Path(__file__).resolve().parents[1]

DEFAULT_COMMAND = (
    "foot -o font=monospace:size=22 -- sh -c 'i=0; while true; do "
    "i=$((i+1)); printf \"\\033[H\\033[2JLive window in the Godot EDITOR\\nUpdate %s\\n\" \"$i\"; sleep 0.02; done'"
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=REPO / "godot/Godot_v4.7.2-stable_linux.x86_64")
    parser.add_argument("--project", type=Path, default=REPO / "godot-bridge/demo", help="source demo project to copy for this session")
    parser.add_argument("--vulkan", action="store_true", help="run the editor with the Mobile Vulkan renderer")
    parser.add_argument("--gpu-index", type=int, help="Vulkan GPU index")
    parser.add_argument("--nested", action="store_true", help="use a nested compositor (default in interactive mode)")
    parser.add_argument("--command", help="application shell command to run inside nested dwl; requires --app-id")
    parser.add_argument("--app-id", help="exact native Wayland app ID to capture; foot by default")
    parser.add_argument("--fps", type=int, choices=range(1, 31), default=10, metavar="1..30")
    parser.add_argument("--auto-exit", action="store_true", help="run the finite automated test instead of waiting for Ctrl+C")
    args = parser.parse_args()
    if args.gpu_index is not None and not args.vulkan:
        parser.error("--gpu-index requires --vulkan")
    if args.command is not None and not args.app_id:
        parser.error("--command requires --app-id so dwl captures the intended application")
    if args.app_id and not args.command:
        parser.error("--app-id requires --command; the built-in command uses foot")
    folder = Path(tempfile.mkdtemp(prefix="dwl-godot-editor-"))
    print(f"Artifacts: {folder}", flush=True)
    project = folder / "project"
    shutil.copytree(args.project.resolve(), project, ignore=shutil.ignore_patterns(".godot", "bin", "addons"))
    (project / "bin").mkdir()
    (project / "bin/libdwl_bridge.so").symlink_to(REPO / "godot-bridge/demo/bin/libdwl_bridge.so")
    shutil.copytree(REPO / "tests/fixtures/editor_live", project / "addons/editor_live")
    with (project / "project.godot").open("a") as file:
        file.write('\n[editor_plugins]\nenabled=PackedStringArray("res://addons/editor_live/plugin.cfg")\n')
    env = os.environ.copy()
    env.update(DWL_BRIDGE_SOCKET=str(folder / "frame.sock"), BRIDGE_TEST_REPORT=str(folder),
               XDG_DATA_HOME=str(folder), XDG_CONFIG_HOME=str(folder), XDG_CACHE_HOME=str(folder))
    use_nested = args.nested or args.command is not None or not args.auto_exit
    if not args.auto_exit:
        env["BRIDGE_TEST_HOLD"] = "1"
    if use_nested:
        env.update(BRIDGE_TEST_NESTED="1", DWL_BRIDGE_LIVE_APP_ID=args.app_id or "foot",
                   DWL_BRIDGE_FPS=str(args.fps))
    # Seed extension discovery; the graphical editor does the normal project scan.
    (project / ".godot").mkdir()
    (project / ".godot/extension_list.cfg").write_text("res://bridge.gdextension\n")
    engine = str(args.engine.resolve())
    renderer_options = ["--rendering-method", "mobile", "--rendering-driver", "vulkan"] if args.vulkan else []
    if args.gpu_index is not None:
        renderer_options += ["--gpu-index", str(args.gpu_index)]
    with (folder / "editor.log").open("w+") as log:
        proc = subprocess.Popen([engine, "--editor", "--path", str(project),
                                 "--display-driver", "wayland", "--audio-driver", "Dummy", *renderer_options],
                                env=env, stdout=log, stderr=subprocess.STDOUT)
        nested = None
        nested_log = (folder / "dwl.log").open("w")
        try:
            path = folder / "frame.sock"
            for cycle in range(2 if args.auto_exit else 1):
                wait_for(lambda: (folder / f"ready{cycle}").exists() and path.exists(), proc)
                if use_nested:
                    if nested is None:
                        nested_env = env.copy()
                        nested_env["WLR_BACKENDS"] = "wayland"
                        nested = subprocess.Popen([str(REPO / "dwl"), "-d", "-s", args.command or DEFAULT_COMMAND],
                            cwd=folder, env=nested_env, stdout=subprocess.DEVNULL, stderr=nested_log)
                else:
                    for number in (1, 2, 3):
                        send(path, number * 60, width=3 if number == 3 else 2)
                        wait_for(lambda: (folder / f"frame{cycle}_{number}").exists(), proc)
                if not args.auto_exit:
                    print(f"Capturing app ID: {args.app_id or 'foot'} at up to {args.fps} FPS", flush=True)
                    print(f"Application command: {args.command or DEFAULT_COMMAND}", flush=True)
                    print("Preview stays open. Press Ctrl+C in this terminal to stop it.", flush=True)
                    while proc.poll() is None:
                        if nested is not None and nested.poll() is not None:
                            raise RuntimeError(f"Nested compositor exited; inspect {folder / 'dwl.log'}")
                        time.sleep(.1)
                    print(f"Godot editor closed (exit {proc.returncode}).", flush=True)
                    return
                wait_for(lambda: (folder / f"closed{cycle}").exists(), proc)
                # The next cycle may already have rebound the path. At the final close it must be gone.
                if cycle == 1:
                    assert not path.exists()
            proc.wait(timeout=10)
            log.seek(0)
            text = log.read()
            assert proc.returncode == 0 and "EDITOR_LIVE_OK" in text and "SCRIPT ERROR" not in text, text
            assert (folder / "editor.png").stat().st_size > 1000
            print("PASS: real editor preview, uploaded pixels, texture reuse, resize and listener cleanup across scene close/reopen")
        except KeyboardInterrupt:
            print("\nStopping the preview and its nested compositor.", flush=True)
        finally:
            if nested is not None and nested.poll() is None:
                nested.terminate()
                try:
                    nested.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    nested.kill()
                    nested.wait()
            nested_log.close()
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    main()
