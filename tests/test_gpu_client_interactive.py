"""Exercise the user-facing continuous runner, no-readback path and Ctrl+C cleanup."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[1]


def main():
    proc = subprocess.Popen([sys.executable, "-B", str(REPO / "tools/run_gpu_client.py"), "--validation"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        line = proc.stdout.readline().strip()
        print(line, flush=True)
        assert line.startswith("Artifacts: "), line
        folder = Path(line.removeprefix("Artifacts: "))
        deadline = time.monotonic() + 25
        progress = {}
        while progress.get("copied", 0) < 35:
            assert proc.poll() is None and time.monotonic() < deadline, "Interactive runner did not continue beyond 32 frames"
            try:
                progress = json.loads((folder / "progress.json").read_text())
            except (FileNotFoundError, json.JSONDecodeError):
                pass
            time.sleep(.05)
        processes = json.loads((folder / "processes.json").read_text())
        startup = int((folder / "startup.pid").read_text())
        assert not list(folder.glob("frame-*.rgba")) and not list(folder.glob("export-*.rgba")), "Interactive pixel readback enabled"
        proc.send_signal(signal.SIGINT)
        output = proc.communicate(timeout=12)[0]
        assert proc.returncode == 0, output
        for pid in (*processes.values(), startup):
            assert not Path(f"/proc/{pid}").exists(), f"Runner left its own process {pid} alive"
        log = (folder / "godot.log").read_text()
        assert all(value not in log for value in ("Validation Error", "VUID-", "SYNC-HAZARD", "SCRIPT ERROR")), log
        assert 'Insert instance layer "VK_LAYER_KHRONOS_validation"' in log
        (folder / "interactive-test.json").write_text(json.dumps({"success": True, "frames": progress["copied"],
            "no_texture_readback": True, "ctrl_c_cleanup": True}, indent=2) + "\n")
        print("PASS: continuous real-client preview beyond 32 frames, no texture readbacks, Ctrl+C stopped its own processes", flush=True)
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.communicate(timeout=12)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate()


if __name__ == "__main__":
    main()
