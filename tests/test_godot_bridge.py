"""Exercise the native extension through the actual engine (no GPU required)."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "godot/Godot_v4.7.2-stable_linux.x86_64"
DEMO = REPO / "godot-bridge/demo"


class GodotBridgeTests(unittest.TestCase):
    def run_case(self, data, reject=False, cancel=False, stall=False):
        with tempfile.TemporaryDirectory(prefix="dwl-godot-test-") as tmp:
            folder = Path(tmp)
            path = folder / "frame.sock"
            output = folder / "pixels.rgb"
            env = os.environ.copy()
            env.update(XDG_DATA_HOME=tmp, XDG_CONFIG_HOME=tmp, XDG_CACHE_HOME=tmp)
            args = [str(ENGINE), "--headless", "--path", str(DEMO),
                    "--script", "res://test_receiver.gd", "--",
                    f"--socket={path}", f"--output={output}"]
            if reject:
                args.append("--reject")
            if cancel:
                args.append("--cancel")
            with (folder / "engine.log").open("w+") as log:
                proc = subprocess.Popen(args, env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 8
                    while not path.exists() and proc.poll() is None and time.monotonic() < deadline:
                        time.sleep(.005)
                    if not cancel:
                        self.assertTrue(path.exists(), "engine did not listen")
                        with socket.socket(socket.AF_UNIX) as conn:
                            conn.settimeout(2)
                            conn.connect(str(path))
                            if stall:
                                # Receiver's total deadline must expire despite the open connection.
                                proc.wait(timeout=8)
                            else:
                                for part in data:
                                    conn.sendall(part)
                                conn.shutdown(socket.SHUT_WR)
                    proc.wait(timeout=10)
                    log.seek(0)
                    text = log.read()
                    self.assertEqual(proc.returncode, 0, text)
                    self.assertNotIn("SCRIPT ERROR", text)
                    self.assertNotIn("Leaked", text)
                    marker = "RECEIVER_CANCELLED" if cancel else "RECEIVER_REJECTED" if reject else "RECEIVER_OK"
                    self.assertIn(marker, text)
                    self.assertFalse(path.exists(), text)
                    if reject or cancel:
                        self.assertFalse(output.exists())
                    else:
                        self.assertEqual(output.read_bytes(), b"abcdef")
                finally:
                    if proc.poll() is None:
                        proc.kill()
                        proc.wait()

    def test_partial_header_and_pixels(self):
        data = struct.pack("!8I", 0x44574C42, 1, 32, 2, 1, 1, 6, 6) + b"abcdef"
        self.run_case([bytes([x]) for x in data])

    def test_rejected_frames(self):
        good = struct.pack("!8I", 0x44574C42, 1, 32, 2, 1, 1, 6, 6)
        oversized = struct.pack("!8I", 0x44574C42, 1, 32, 8192, 8192, 1, 24576, 201326592)
        for data in (good[:15], good + b"abc", good + b"abcdefx", oversized):
            with self.subTest(data=data[:16]):
                self.run_case([data], reject=True)

    def test_stalled_peer(self):
        self.run_case([], reject=True, stall=True)

    def test_stop_while_listening(self):
        self.run_case([], cancel=True)


if __name__ == "__main__":
    unittest.main()
