"""Nested integration: absence, rejected negotiation, backpressure and abandoned BOs."""
import array
import os
from pathlib import Path
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO / "tools"))
from run_gpu_client import DEFAULT_COMMAND, stop


def main():
    folder = Path(tempfile.mkdtemp(prefix="dwl-gpu-backpressure-"))
    print(f"Artifacts: {folder}", flush=True)
    env = {key: value for key, value in os.environ.items() if not key.startswith("DWL_BRIDGE_")}
    env.update(WLR_BACKENDS="wayland", WLR_RENDERER="gles2", DWL_BRIDGE_GPU_SOCKET=str(folder / "gpu.sock"),
               DWL_BRIDGE_GPU_APP_ID="bridge-gpu-test", DWL_BRIDGE_FPS="10",
               XDG_DATA_HOME=str(folder / "data"), XDG_CONFIG_HOME=str(folder / "config"))
    server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    server.settimeout(5)
    peer = None
    owned_fds = []
    child = None
    with (folder / "dwl.log").open("w") as log:
        nested = subprocess.Popen([str(REPO / "dwl"), "-d", "-s", DEFAULT_COMMAND], cwd=folder,
                                  env=env, stdout=subprocess.DEVNULL, stderr=log, start_new_session=True)
        try:
            deadline = time.monotonic() + 10
            identity = None
            while identity is None or "[bridge] MAP" not in (folder / "dwl.log").read_text():
                assert nested.poll() is None and time.monotonic() < deadline
                text = (folder / "dwl.log").read_text()
                identity = re.search(r"ENABLED .*DRM=(\d+):(\d+)", text)
                time.sleep(.02)
            major, minor = map(int, identity.groups())
            time.sleep(.4)
            text = (folder / "dwl.log").read_text()
            assert "[gpu-bridge] ALLOC" not in text and "[gpu-bridge] SEND" not in text
            assert text.count("[gpu-bridge] APPLIED") >= 3
            server.bind(str(folder / "gpu.sock"))
            server.listen(1)
            peer, _ = server.accept()
            # A different physical GPU must be rejected before allocating/exporting.
            peer.send(struct.pack("!IIIIQII", 0x44474831, 3, 32, 0x34324241, 0, major, minor ^ 1))
            peer.settimeout(3)
            assert peer.recv(112) == b""
            peer.close()
            assert "[gpu-bridge] ALLOC" not in (folder / "dwl.log").read_text()

            def receive_frame():
                nonlocal peer
                peer, _ = server.accept()
                peer.settimeout(5)
                peer.send(struct.pack("!IIIIQII", 0x44474831, 3, 32, 0x34324241, 0, major, minor))
                data, control, flags, _ = peer.recvmsg(112, socket.CMSG_SPACE(8), socket.MSG_CMSG_CLOEXEC)
                fds = []
                for level, kind, values in control:
                    assert level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS
                    descriptors = array.array("i")
                    descriptors.frombytes(values)
                    fds.extend(descriptors)
                owned_fds.extend(fds)
                assert len(data) == 112 and len(fds) == 2 and not flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC)
                assert struct.unpack_from("!III", data) == (0x444d4231, 3, 112)
                poll = select.poll()
                poll.register(fds[1], select.POLLIN)
                assert poll.poll(5000), "Compositor GPU copy did not finish"
                return fds[0]

            old = receive_frame()
            before = (folder / "dwl.log").read_text().count("[gpu-bridge] APPLIED")
            time.sleep(1.2)  # Deliberately never ACK: dwl must keep handling commits.
            text = (folder / "dwl.log").read_text()
            assert nested.poll() is None
            assert text.count("[gpu-bridge] APPLIED") >= before + 5
            assert text.count("[gpu-bridge] SEND frame=") == 1
            assert text.count("[gpu-bridge] ALLOC") == 1
            peer.settimeout(.1)
            try:
                unexpected = peer.recv(112)
                raise AssertionError(f"Queued another image under backpressure: {unexpected!r}")
            except TimeoutError:
                pass
            peer.close()  # Retain the received storage FD after disconnect.
            new = receive_frame()
            assert os.fstat(old).st_ino != os.fstat(new).st_ino, "Abandoned export storage was reused"
            text = (folder / "dwl.log").read_text()
            assert text.count("[gpu-bridge] ALLOC") == 2
            print("PASS: absent/wrong-device receiver, one slot under backpressure, commits continue, disconnected export not reused", flush=True)
        finally:
            if peer is not None:
                peer.close()
            server.close()
            for fd in owned_fds:
                os.close(fd)
            # The startup shell is a direct child/session leader of this nested dwl.
            children = Path(f"/proc/{nested.pid}/task/{nested.pid}/children")
            if children.exists():
                for value in children.read_text().split():
                    child = int(value)
                    try:
                        if os.getpgid(child) == child:
                            os.killpg(child, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            stop(nested)


if __name__ == "__main__":
    main()
