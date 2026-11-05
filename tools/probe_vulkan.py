#!/usr/bin/env python3
"""Read-only Godot Vulkan capability audit plus nested dwl DRM identity."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]
REQUIRED = (
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier",
    "VK_KHR_external_semaphore_fd",
)
BRIDGE_REQUESTS = (*REQUIRED, "VK_EXT_queue_family_foreign")


def parse_creation_capture(text):
    """Refuse ambiguous/malformed evidence instead of guessing enabled features."""
    blocks = re.findall(r"^vkCreateDevice\([^\n]*\) returns VkResult VK_SUCCESS \(0\):\n(.*?)(?=\nThread |\Z)",
                        text, re.MULTILINE | re.DOTALL)
    if len(blocks) != 1:
        raise ValueError(f"Expected exactly one successful vkCreateDevice, found {len(blocks)}")
    count = re.search(r"enabledExtensionCount:\s+uint32_t = (\d+)", blocks[0])
    names = re.findall(r'ppEnabledExtensionNames\[\d+\]:\s+const char\* = "([^"]+)"', blocks[0])
    if count is None or int(count[1]) != len(names) or len(set(names)) != len(names):
        raise ValueError("Incomplete or ambiguous enabled-extension capture")
    return sorted(names)


def stop(proc):
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


def nested_identity(folder, env):
    env = env.copy()
    env["WLR_BACKENDS"] = "wayland"
    with (folder / "dwl.log").open("w") as log:
        proc = subprocess.Popen([str(REPO / "dwl"), "-d"], cwd=folder, env=env,
                                stdout=subprocess.DEVNULL, stderr=log)
        try:
            deadline = time.monotonic() + 10
            while proc.poll() is None and time.monotonic() < deadline:
                text = (folder / "dwl.log").read_text(errors="replace")
                match = re.search(r"\[bridge\] RENDER_DEVICE major=(\d+) minor=(\d+)", text)
                if match:
                    return {"major": int(match[1]), "minor": int(match[2]),
                            "source": "fstat(wlr_renderer_get_drm_fd), nested Wayland backend"}
                if "[bridge] RENDER_DEVICE unavailable" in text:
                    return {"error": "Nested renderer has no DRM device identity"}
                time.sleep(.05)
            return {"error": "Nested renderer failed/timed out; inspect dwl.log"}
        finally:
            stop(proc)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=REPO / "godot/Godot_v4.7.2-stable_linux.x86_64")
    parser.add_argument("--request-bridge-extensions", action="store_true", help="request the bridge extensions in an isolated project")
    parser.add_argument("--request-extension", action="append", default=[], help="additional optional request (repeatable)")
    parser.add_argument("--validation", action="store_true", help="also enable the SDK validation layer")
    parser.add_argument("--gpu-index", type=int, help="Godot GPU index; omit to audit its default selection")
    parser.add_argument("--output", type=Path, help="new artifact directory (must not already exist)")
    parser.add_argument("--api-dump-layer-dir", type=Path, help="directory containing VkLayer_api_dump.json")
    args = parser.parse_args()
    if args.gpu_index is not None and args.gpu_index < 0:
        parser.error("--gpu-index must be nonnegative")
    candidates = [args.api_dump_layer_dir] if args.api_dump_layer_dir else [
        Path(os.environ.get("VULKAN_SDK", "/nonexistent")) / "share/vulkan/explicit_layer.d",
        Path("/usr/share/vulkan/explicit_layer.d"),
    ]
    layer_dir = next((p for p in candidates if (p / "VkLayer_api_dump.json").is_file()), None)
    if layer_dir is None:
        parser.error("LunarG API-dump layer not found. Set VULKAN_SDK or pass --api-dump-layer-dir.")
    folder = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix="dwl-vulkan-audit-"))
    if args.output:
        folder.mkdir(mode=0o700, parents=True)
    print(f"Artifacts: {folder}", flush=True)
    env = os.environ.copy()
    # Isolate this diagnostic from caller-provided layers/capture settings.
    for key in list(env):
        if key.startswith("VK_"):
            del env[key]
    env.update(XDG_DATA_HOME=str(folder / "data"), XDG_CONFIG_HOME=str(folder / "config"),
               XDG_CACHE_HOME=str(folder / "cache"))
    renderer = nested_identity(folder, env)
    requests = list(BRIDGE_REQUESTS if args.request_bridge_extensions else ()) + args.request_extension
    project = REPO / "godot-bridge/demo"
    if requests:
        project = folder / "project"
        shutil.copytree(REPO / "godot-bridge/demo", project, ignore=shutil.ignore_patterns(".godot", "bin"))
        (project / "bin").mkdir()
        (project / "bin/libdwl_bridge.so").symlink_to(REPO / "godot-bridge/demo/bin/libdwl_bridge.so")
        (project / ".godot").mkdir()
        (project / ".godot/extension_list.cfg").write_text("res://bridge.gdextension\n")
        with (project / "project.godot").open("a") as config:
            config.write("\nrendering_device/vulkan/additional_device_extensions=PackedStringArray(" +
                         ", ".join(json.dumps(name) for name in requests) + ")\n")
    gpu_env = env.copy()
    capture = folder / "api.txt"
    gpu_env.update(VK_LAYER_PATH=str(layer_dir), VK_INSTANCE_LAYERS="VK_LAYER_LUNARG_api_dump" + (":VK_LAYER_KHRONOS_validation" if args.validation else ""),
                   VK_APIDUMP_LOG_FILENAME=str(capture), VK_APIDUMP_OUTPUT_FORMAT="text",
                   VK_APIDUMP_DETAILED="true", VK_APIDUMP_NO_ADDR="true", VK_LOADER_DEBUG="layer")
    engine = args.engine.resolve()
    command = [str(engine),
               "--path", str(project), "--display-driver", "wayland",
               "--rendering-method", "mobile", "--rendering-driver", "vulkan",
               "--audio-driver", "Dummy", "--script", "res://vulkan_probe.gd"]
    if args.gpu_index is not None:
        command += ["--gpu-index", str(args.gpu_index)]
    command += ["--", f"--report={folder / 'device.json'}"]
    with (folder / "godot.log").open("w") as log:
        subprocess.run(command, env=gpu_env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=45)
    report = json.loads((folder / "device.json").read_text())
    if report["rendering_driver"] != "vulkan" or not report["on_render_thread"]:
        raise ValueError("Probe did not execute on Godot's Vulkan render thread")
    enabled = parse_creation_capture(capture.read_text())
    engine_enabled = report["enabled_device_extensions"]
    report["engine_reported_enabled_extensions"] = engine_enabled
    report["engine_api_matches_capture"] = sorted(engine_enabled) == enabled if isinstance(engine_enabled, list) else None
    if report["engine_api_matches_capture"] is False:
        raise ValueError("Engine enabled-extension API disagrees with captured vkCreateDevice")
    report["enabled_device_extensions"] = enabled
    report["requested_additional_extensions"] = requests
    report["engine_binary"] = str(engine)
    with engine.open("rb") as binary:
        report["engine_binary_sha256"] = hashlib.file_digest(binary, "sha256").hexdigest()
    report["enabled_extension_evidence"] = {
        "source": "LunarG API-dump: sole successful vkCreateDevice in this probe run",
        "capture_sha256": hashlib.sha256(capture.read_bytes()).hexdigest(),
        "capture_file": str(capture),
    }
    log_text = (folder / "godot.log").read_text()
    report["captured_at_utc"] = datetime.now(timezone.utc).isoformat()
    report["requested_gpu_index"] = args.gpu_index
    selected = re.search(r"Using Device #(\d+)", log_text)
    report["selected_gpu_index"] = int(selected[1]) if selected else None
    if args.gpu_index is not None and report["selected_gpu_index"] != args.gpu_index:
        raise ValueError("Godot did not select the requested GPU; inspect godot.log")
    report["validation_requested"] = args.validation
    report["validation_layer_observed"] = bool(re.search(r'Insert instance layer "VK_LAYER_KHRONOS_validation"', log_text))
    report["validation_error_observed"] = "Validation Error" in log_text or "VUID-" in log_text
    if args.validation and not report["validation_layer_observed"]:
        raise ValueError("Validation requested but loader did not confirm it was inserted")
    if report["validation_error_observed"]:
        raise ValueError("Vulkan validation reported an issue; inspect godot.log")
    report["dwl_renderer"] = renderer
    physical = report["physical_device"]
    supported = physical["supported_device_extensions"]
    report["bridge_extensions"] = {name: {"supported": name in supported, "enabled": name in enabled} for name in REQUIRED}
    if "error" in renderer or "render_major" not in physical:
        same_gpu = None
    else:
        same_gpu = any((renderer["major"], renderer["minor"]) == (physical.get(f"{kind}_major"), physical.get(f"{kind}_minor"))
                       for kind in ("primary", "render"))
    report["same_drm_device_as_nested_dwl"] = same_gpu
    report["required_capabilities_present"] = same_gpu is True and all(name in enabled for name in REQUIRED)
    report["decision_scope"] = "Required capabilities for the proposed same-GPU DMA-BUF + sync-FD path only; no import or synchronization proof"
    report["repository_revision"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
    report["repository_dirty"] = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=REPO))
    report["wlroots_version"] = subprocess.check_output(["pkg-config", "--modversion", "wlroots-0.19"], text=True).strip()
    (folder / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Godot GPU: {physical['device_name']}")
    print(f"Same DRM device as nested dwl: {same_gpu}")
    for name, state in report["bridge_extensions"].items():
        print(f"{name}: supported={state['supported']}, enabled={state['enabled']}")
    print(f"Report: {folder / 'report.json'}")
    if not report["required_capabilities_present"]:
        print("The proposed direct DMA-BUF path is not ready; inspect the report before writing an importer.")


if __name__ == "__main__":
    main()
