"""Graphical integration checks for the isolated engine's extension request API."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[1]
BASE_EXTENSIONS = (
    "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=REPO / "engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge")
    parser.add_argument("--matching-gpu-index", type=int, default=0)
    parser.add_argument("--other-gpu-index", type=int, help="optional second GPU to check enablement separately from device matching")
    args = parser.parse_args()
    folder = Path(tempfile.mkdtemp(prefix="dwl-engine-capabilities-"))
    print(f"Artifacts: {folder}", flush=True)
    stock = REPO / "godot/Godot_v4.7.2-stable_linux.x86_64"
    engine = args.engine.resolve()
    cases = [
        ("stock-requested", stock, args.matching_gpu_index, ["--request-bridge-extensions"], False, False),
        ("patched-default", engine, args.matching_gpu_index, [], True, False),
        ("patched-amd-requested", engine, args.matching_gpu_index, ["--request-bridge-extensions", "--validation"], True, True),
        ("patched-optional-requests", engine, args.matching_gpu_index,
         ["--request-bridge-extensions", "--validation", "--request-extension", "VK_KHR_external_memory_fd",
          "--request-extension", "", "--request-extension", "VK_DWL_BRIDGE_intentionally_unsupported"], True, True),
    ]
    if args.other_gpu_index is not None:
        cases.append(("patched-other-gpu", engine, args.other_gpu_index,
                      ["--request-bridge-extensions", "--validation"], True, True))
    results = []
    for name, binary, index, extra, has_api, requested_enabled in cases:
        print(f"Running {name}...", flush=True)
        command = ["python3", str(REPO / "tools/probe_vulkan.py"), "--engine", str(binary),
                   "--gpu-index", str(index), "--output", str(folder / name), *extra]
        with (folder / f"{name}.log").open("w") as log:
            subprocess.run(command, cwd=REPO, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=90)
        report = json.loads((folder / name / "report.json").read_text())
        assert report["engine_api"]["enabled_extension_inspection"] == has_api, name
        assert report["engine_api_matches_capture"] == (True if has_api else None), name
        enabled = report["enabled_device_extensions"]
        assert (all(extension in enabled for extension in BASE_EXTENSIONS)) == requested_enabled, name
        assert "VK_DWL_BRIDGE_intentionally_unsupported" not in enabled and "" not in enabled, name
        if name != "patched-other-gpu":
            assert report["same_drm_device_as_nested_dwl"] is True, name
        else:
            assert report["same_drm_device_as_nested_dwl"] is False, name
        if "--validation" in extra:
            assert report["validation_layer_observed"] and not report["validation_error_observed"], name
        results.append({"case": name, "passed": True, "report": str(folder / name / "report.json")})
        print(f"PASS {name}", flush=True)
    (folder / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print("PASS: engine API agrees with vkCreateDevice; defaults, requests and optional failures verified")


if __name__ == "__main__":
    main()
