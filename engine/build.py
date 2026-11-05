#!/usr/bin/env python3
"""Apply the reviewed patches to the pinned checkout and build a separate editor."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "source"
BASE = "ed1daf0bf001b61586d9930840f2f1394092c079"
PATCHES = [ROOT / "patches/0001-optional-vulkan-extension-requests.patch",
           ROOT / "patches/0002-expose-enabled-device-extensions.patch",
           ROOT / "patches/0003-controlled-external-snapshot-copy.patch",
           ROOT / "patches/0004-async-external-copy-retirement.patch",
           ROOT / "patches/0005-foreign-compositor-image-ownership.patch"]
BINARY = SOURCE / "bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge"


def git(*args, **kwargs):
    return subprocess.run(["git", "-C", str(SOURCE), *map(str, args)], **kwargs)


def digest(path):
    with path.open("rb") as file:
        return hashlib.file_digest(file, "sha256").hexdigest()


def reviewed_sources_match():
    # Later patches can change earlier patches' context. Replay the series in a
    # small private tree instead of reverse-checking overlapping patches at once.
    paths = {line[6:] for patch in PATCHES for line in patch.read_text().splitlines()
             if line.startswith("+++ b/")}
    changed = set(git("diff", "--name-only", capture_output=True, text=True, check=True).stdout.splitlines())
    if not changed <= paths:
        return False
    with tempfile.TemporaryDirectory(prefix="dwl-engine-patches-") as directory:
        tree = Path(directory)
        subprocess.run(["git", "init", "-q", directory], check=True)
        for path in paths:
            target = tree / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(git("show", f"HEAD:{path}", capture_output=True, check=True).stdout)
        for patch in PATCHES:
            subprocess.run(["git", "-C", directory, "apply", str(patch)], check=True)
        return all((SOURCE / path).read_bytes() == (tree / path).read_bytes() for path in paths)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=6)
    parser.add_argument("--prepare-only", action="store_true", help="check/apply patches without compiling")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if not (SOURCE / ".git").exists():
        parser.error("Source checkout missing; follow engine/README.md first")
    revision = git("rev-parse", "HEAD", capture_output=True, text=True, check=True).stdout.strip()
    if revision != BASE:
        parser.error(f"Expected {BASE}; checkout is {revision}")
    applied = reviewed_sources_match()
    if applied:
        print("Reviewed engine patches are already applied.", flush=True)
    else:
        if git("status", "--porcelain", capture_output=True, text=True, check=True).stdout.strip():
            parser.error("Source has other changes or a partial patch; inspect it before applying")
        # reviewed_sources_match() already replayed and checked the full series.
        for patch in PATCHES:
            git("apply", str(patch), check=True)
        print("Applied engine patches.", flush=True)
    git("diff", "--check", check=True)
    if args.prepare_only:
        return
    scons = ROOT / "venv/bin/scons"
    if not scons.exists():
        parser.error("Private SCons environment missing; follow engine/README.md")
    logs = ROOT / "logs"
    logs.mkdir(exist_ok=True)
    command = [str(scons), "platform=linuxbsd", "target=editor", "use_llvm=yes", "linker=lld",
               "debug_symbols=no", "optimize=none", "lto=none", "extra_suffix=dwl_bridge", f"-j{args.jobs}"]
    print(f"Building in {SOURCE}; progress: {logs / 'build.log'}", flush=True)
    with (logs / "build.log").open("w") as log:
        subprocess.run(command, cwd=SOURCE, stdout=log, stderr=subprocess.STDOUT, check=True)
    diff = git("diff", "--binary", capture_output=True, check=True).stdout
    manifest = {
        "base_revision": BASE,
        "built_at_utc": datetime.now(timezone.utc).isoformat(),
        "command": command,
        "patch_sha256": {path.name: digest(path) for path in PATCHES},
        "working_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "binary": str(BINARY),
        "binary_sha256": digest(BINARY),
        "version": subprocess.check_output([str(BINARY), "--version"], text=True).strip(),
        "scons": subprocess.check_output([str(ROOT / "venv/bin/python"), "-c", "import SCons; print(SCons.__version__)"], text=True).strip(),
        "compiler": subprocess.check_output(["clang++", "--version"], text=True).splitlines()[0],
        "linker": subprocess.check_output(["ld.lld", "--version"], text=True).splitlines()[0],
    }
    (logs / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Built: {BINARY}")
    print(f"Manifest: {logs / 'manifest.json'}")


if __name__ == "__main__":
    main()
