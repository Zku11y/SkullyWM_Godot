#!/usr/bin/env python3
"""Receive one DWLB v1 RGB snapshot. Python standard library only."""
import argparse
import os
from pathlib import Path
import socket
import stat
import struct
import tempfile

MAX_DIM = 8192
MAX_BYTES = 64 * 1024 * 1024
HEADER = struct.Struct("!8I")


def read_exact(conn, count):
    data = bytearray()
    while len(data) < count:
        part = conn.recv(min(count - len(data), 65536))
        if not part:
            raise ValueError("truncated frame")
        data.extend(part)
    return data


def receive(conn, output):
    magic, version, header_size, width, height, fmt, stride, size = HEADER.unpack(
        read_exact(conn, HEADER.size))
    if (magic != 0x44574C42 or version != 1 or header_size != HEADER.size
            or fmt != 1 or not 0 < width <= MAX_DIM or not 0 < height <= MAX_DIM
            or stride != width * 3 or size != stride * height or size > MAX_BYTES):
        raise ValueError("invalid frame header")
    # Stream to a temporary file; publish only after the complete payload and EOF.
    output = Path(output)
    name = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as file:
            name = file.name
            file.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
            remaining = size
            while remaining:
                part = conn.recv(min(remaining, 65536))
                if not part:
                    raise ValueError("truncated pixels")
                file.write(part)
                remaining -= len(part)
            if conn.recv(1):
                raise ValueError("unexpected trailing bytes")
        os.replace(name, output)
        name = None
    finally:
        if name:
            os.unlink(name)
    return width, height


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, help="absolute path in a private directory")
    parser.add_argument("--output", default="bridge-received.ppm")
    args = parser.parse_args()
    path = Path(args.socket)
    if not path.is_absolute() or len(os.fsencode(path)) >= 108:
        parser.error("socket path must be absolute and shorter than 108 bytes")
    info = path.parent.lstat()
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid()
            or stat.S_IMODE(info.st_mode) != 0o700):
        parser.error("socket parent must be your own directory with mode 0700")
    # Never remove an existing socket: it may belong to another receiver.
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(str(path))
        try:
            os.chmod(path, 0o600)
            server.listen(1)
            print(f"Listening on {path}", flush=True)
            conn, _ = server.accept()
            with conn:
                conn.settimeout(5)
                width, height = receive(conn, args.output)
            print(f"Saved {args.output}: {width}x{height}")
        finally:
            path.unlink()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(f"bridge receiver: {error}")
