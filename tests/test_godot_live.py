"""Native live mailbox/recovery checks through the actual headless engine."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]


def wait_for(predicate, proc):
    deadline = time.monotonic() + 10
    while not predicate():
        assert proc.poll() is None and time.monotonic() < deadline, "Engine failed or deadline expired"
        time.sleep(.01)


def send(path, value, width=2):
    with socket.socket(socket.AF_UNIX) as conn:
        conn.connect(str(path))
        conn.sendall(struct.pack("!8I", 0x44574C42, 1, 32, width, 1, 1, width * 3, width * 3)
                     + bytes([value]) * (width * 3))
        conn.shutdown(socket.SHUT_WR)
        # Ensure the receiver has closed this completed snapshot connection.
        conn.settimeout(3)
        assert conn.recv(1) == b""


def main():
    with tempfile.TemporaryDirectory(prefix="dwl-godot-live-") as tmp:
        folder = Path(tmp)
        path, report = folder / "frame.sock", folder / "report"
        env = os.environ.copy()
        env.update(XDG_DATA_HOME=tmp, XDG_CONFIG_HOME=tmp, XDG_CACHE_HOME=tmp)
        with (folder / "engine.log").open("w+") as log:
            proc = subprocess.Popen([
                str(REPO / "godot/Godot_v4.7.2-stable_linux.x86_64"), "--headless",
                "--path", str(REPO / "godot-bridge/demo"), "--script", "res://test_live_receiver.gd",
                "--", f"--socket={path}", f"--report={report}",
            ], env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                wait_for(path.exists, proc)
                for value in (1, 2, 3):
                    send(path, value)
                wait_for(lambda: report.exists() and report.read_text() == "restarted", proc)
                with socket.socket(socket.AF_UNIX) as conn:
                    conn.connect(str(path))
                    conn.sendall(b"bad")
                send(path, 4, width=3)
                wait_for(lambda: report.read_text() == "cancel_partial", proc)
                with socket.socket(socket.AF_UNIX) as conn:
                    conn.connect(str(path))
                    conn.sendall(b"D")
                    proc.wait(timeout=5)
                log.seek(0)
                text = log.read()
                assert proc.returncode == 0 and "LIVE_RECEIVER_OK" in text and "SCRIPT ERROR" not in text, text
                assert not path.exists()
                print(text.strip())
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    main()
