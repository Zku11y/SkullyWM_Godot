"""Run with python3 tests/test_bridge.py /tmp/bridge-sender."""
import importlib.util
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location(
    "receiver", Path(__file__).resolve().parents[1] / "tools/bridge-receiver.py")
receiver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(receiver)
SENDER = sys.argv.pop(1)


class BridgeTests(unittest.TestCase):
    def test_sender_to_receiver(self):
        with tempfile.TemporaryDirectory() as tmp:
            path, output = str(Path(tmp) / "socket"), Path(tmp) / "frame.ppm"
            with socket.socket(socket.AF_UNIX) as server:
                server.bind(path)
                server.listen(1)
                proc = subprocess.Popen([SENDER, path], stdout=subprocess.PIPE)
                conn, _ = server.accept()
                with conn:
                    conn.settimeout(5)
                    self.assertEqual(receiver.receive(conn, output), (1024, 1024))
                stdout, _ = proc.communicate(timeout=5)
                self.assertEqual(proc.returncode, 0)
                self.assertEqual(output.read_bytes(), b"P6\n1024 1024\n255\n"
                                 + bytes(range(256)) * (1024 * 1024 * 3 // 256))
                print(stdout.decode().strip())

    def test_absent(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run([SENDER, str(Path(tmp) / "absent")], timeout=3)
            self.assertEqual(result.returncode, 3)

    def test_stall_and_disconnect(self):
        for stall in (False, True):
            with self.subTest(stall=stall), tempfile.TemporaryDirectory() as tmp:
                path = str(Path(tmp) / "socket")
                with socket.socket(socket.AF_UNIX) as server:
                    server.bind(path)
                    server.listen(1)
                    proc = subprocess.Popen([SENDER, path], stdout=subprocess.PIPE)
                    conn, _ = server.accept()
                    start = time.monotonic()
                    if not stall:
                        conn.close()
                    stdout, _ = proc.communicate(timeout=4)
                    conn.close()
                    self.assertEqual(proc.returncode, 0)
                    self.assertLess(time.monotonic() - start, 3)
                    if stall:
                        self.assertGreater(time.monotonic() - start, 1.5)
                        ticks = int(stdout.decode().split()[0].split("=")[1])
                        self.assertGreater(ticks, 50)
                    print(stdout.decode().strip())

    def feed(self, parts, valid):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "frame.ppm"
            output.write_bytes(b"previous image")
            a, b = socket.socketpair()
            def writer():
                with a:
                    try:
                        for part in parts:
                            a.sendall(part)
                    except BrokenPipeError:
                        pass
            thread = threading.Thread(target=writer)
            thread.start()
            with b:
                b.settimeout(3)
                if valid:
                    self.assertEqual(receiver.receive(b, output), (2, 1))
                    self.assertEqual(output.read_bytes(), b"P6\n2 1\n255\nabcdef")
                else:
                    with self.assertRaises(ValueError):
                        receiver.receive(b, output)
                    self.assertEqual(output.read_bytes(), b"previous image")
            thread.join()
            self.assertEqual(list(Path(tmp).iterdir()), [output])

    def test_partial_and_invalid_input(self):
        header = [0x44574C42, 1, 32, 2, 1, 1, 6, 6]
        message = receiver.HEADER.pack(*header) + b"abcdef"
        self.feed([bytes([x]) for x in message], True)
        self.feed([message[:-1]], False)
        self.feed([message + b"x"], False)
        self.feed([message[:12]], False)
        for field, value in ((0, 0), (1, 2), (2, 31), (3, 0), (4, 9000),
                             (5, 2), (6, 7), (7, 0xFFFFFFFF)):
            broken = header.copy()
            broken[field] = value
            self.feed([receiver.HEADER.pack(*broken)], False)
        self.feed([receiver.HEADER.pack(0x44574C42, 1, 32, 8192, 8192,
                                       1, 8192 * 3, 8192 * 8192 * 3)], False)


if __name__ == "__main__":
    unittest.main()
