"""Graphical integration: temporary nested dwl/foot -> Godot 2D and 3D views.

Opens a Godot window and a nested compositor; stops only processes it starts.
Retains logs, source PPM, received RGB, and screenshot in a temporary directory.
"""
import argparse
import os
from pathlib import Path
import subprocess
import re
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]


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
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--vulkan", action="store_true", help="override the demo's OpenGL renderer for this test")
    parser.add_argument("--gpu-index", type=int, help="Godot Vulkan device index")
    args = parser.parse_args()
    if args.gpu_index is not None and not args.vulkan:
        parser.error("--gpu-index requires --vulkan")
    folder = Path(tempfile.mkdtemp(prefix="dwl-godot-visual-"))
    print(f"Artifacts: {folder}", flush=True)
    live = args.live
    env = os.environ.copy()
    env.update(DWL_BRIDGE_SOCKET=str(folder / "frame.sock"),
               XDG_DATA_HOME=str(folder), XDG_CONFIG_HOME=str(folder), XDG_CACHE_HOME=str(folder))
    if live:
        env.update(DWL_BRIDGE_LIVE_APP_ID="foot", DWL_BRIDGE_FPS="10")
    viewer = nested = None
    renderer_options = ["--rendering-method", "mobile", "--rendering-driver", "vulkan"] if args.vulkan else []
    if args.gpu_index is not None:
        renderer_options += ["--gpu-index", str(args.gpu_index)]
    with (folder / "godot.log").open("w") as godot_log, (folder / "dwl.log").open("w") as dwl_log:
        try:
            viewer = subprocess.Popen([
                str(REPO / "godot/Godot_v4.7.2-stable_linux.x86_64"),
                "--path", str(REPO / "godot-bridge/demo"), "--display-driver", "wayland",
                "--audio-driver", "Dummy", *renderer_options, "--",
                f"--capture-after={8 if live else 1}", f"--capture={folder / 'screenshot.png'}", f"--pixels-output={folder / 'received.rgb'}",
            ], env=env, stdout=godot_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 15
            while not (folder / "frame.sock").exists() and viewer.poll() is None and time.monotonic() < deadline:
                time.sleep(.02)
            assert (folder / "frame.sock").exists(), "Viewer did not listen; inspect godot.log"
            env["WLR_BACKENDS"] = "wayland"
            nested = subprocess.Popen([
                str(REPO / "dwl"), "-d", "-s",
                "foot -o font=monospace:size=18 -- sh -c 'while true; do "
                "printf \"\\033[H\\033[2JGODOT BRIDGE TEST\\nOne frame in two views\\n"
                "\\033[31mRED \\033[32mGREEN \\033[34mBLUE\\033[0m\\n\"; sleep 0.25; done'",
            ], cwd=folder, env=env, stdout=subprocess.DEVNULL, stderr=dwl_log)
            deadline = time.monotonic() + 10
            while "[bridge] MAP" not in (folder / "dwl.log").read_text(errors="replace"):
                assert nested.poll() is None and time.monotonic() < deadline, "Nested foot did not map"
                time.sleep(.02)
            # Capture populated content rather than foot's initial blank buffer.
            time.sleep(.5)
            if not live:
                (folder / "bridge-capture.request").touch()
                (folder / "bridge-send.request").touch()
            assert viewer.wait(timeout=25) == 0, "Viewer failed; inspect godot.log"
            if not live:
                raw = (folder / "bridge-frame.ppm").read_bytes().split(b"\n", 3)[3]
                assert raw == (folder / "received.rgb").read_bytes(), "Pixel mismatch"
            assert (folder / "screenshot.png").stat().st_size > 1000
            log = (folder / "godot.log").read_text()
            assert "BRIDGE_DISPLAYED" in log and "SCRIPT ERROR" not in log, log
            if args.vulkan:
                assert "Vulkan" in log and "OpenGL API" not in log, log
            if live:
                updates = re.findall(r"frames=(\d+) allocations=(\d+)", log)
                assert int(updates[-1][0]) >= 8 and int(updates[-1][1]) < int(updates[-1][0]), log
                times = re.findall(r"(\d+):(\d+):(\d+\.\d+) .*\[bridge\] SEND snapshot", (folder / "dwl.log").read_text())
                seconds = [int(h) * 3600 + int(m) * 60 + float(s) for h, m, s in times]
                assert len(seconds) >= 8 and all(b - a >= .095 for a, b in zip(seconds, seconds[1:])), seconds
                print("PASS: live nested SHM updates, rate limit and texture reuse; screenshot saved")
            else:
                print("PASS: nested SHM capture equals Godot's received RGB; rendered screenshot saved")
        finally:
            stop(viewer)
            stop(nested)


if __name__ == "__main__":
    main()
