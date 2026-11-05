"""Graphical integration: temporary nested dwl/foot -> Godot 2D and 3D views.

Opens a Godot window and a nested compositor; stops only processes it starts.
Retains logs, source PPM, received RGB, and screenshot in a temporary directory.
"""
import os
from pathlib import Path
import subprocess
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
    folder = Path(tempfile.mkdtemp(prefix="dwl-godot-visual-"))
    print(f"Artifacts: {folder}", flush=True)
    env = os.environ.copy()
    env.update(DWL_BRIDGE_SOCKET=str(folder / "frame.sock"),
               XDG_DATA_HOME=str(folder), XDG_CONFIG_HOME=str(folder), XDG_CACHE_HOME=str(folder))
    viewer = nested = None
    with (folder / "godot.log").open("w") as godot_log, (folder / "dwl.log").open("w") as dwl_log:
        try:
            viewer = subprocess.Popen([
                str(REPO / "godot/Godot_v4.7.2-stable_linux.x86_64"),
                "--path", str(REPO / "godot-bridge/demo"), "--display-driver", "wayland",
                "--audio-driver", "Dummy", "--",
                f"--capture={folder / 'screenshot.png'}", f"--pixels-output={folder / 'received.rgb'}",
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
            (folder / "bridge-capture.request").touch()
            (folder / "bridge-send.request").touch()
            assert viewer.wait(timeout=25) == 0, "Viewer failed; inspect godot.log"
            raw = (folder / "bridge-frame.ppm").read_bytes().split(b"\n", 3)[3]
            assert raw == (folder / "received.rgb").read_bytes(), "Pixel mismatch"
            assert (folder / "screenshot.png").stat().st_size > 1000
            log = (folder / "godot.log").read_text()
            assert "BRIDGE_DISPLAYED" in log and "SCRIPT ERROR" not in log, log
            print("PASS: nested SHM capture equals Godot's received RGB; rendered screenshot saved")
        finally:
            stop(viewer)
            stop(nested)


if __name__ == "__main__":
    main()
