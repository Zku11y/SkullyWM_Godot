"""Real GPU session tests. Godot owns all nested compositor/application startup.

Default: runtime start/stop/restart and editor Play/Stop/Play. --runtime-only
skips the editor. Diagnostic readbacks/screenshots occur only in these fixtures.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge"
WORLD = REPO / "godot-bridge/world"
LIBC = ctypes.CDLL(None)


def permit_test_debugger():
    # Yama: allow this test parent and its gdb child to inspect only our engine.
    # Test-only pre-exec setup; does not alter the shipped engine or desktop.
    LIBC.prctl(0x59616D61, os.getppid(), 0, 0, 0)  # PR_SET_PTRACER

FOOT = ["foot", "--app-id=bridge-world-test", "-o", "font=monospace:size=22", "--", "sh", "-c",
        'i=0; while true; do i=$((i+1)); printf "\\033[H\\033[2JGodot-owned session\\nUpdate %s\\n\\033[31mRED \\033[32mGREEN \\033[34mBLUE\\033[0m\\n" "$i"; sleep 0.04; done']


def wait_for(predicate, proc=None, timeout=40):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, "Timed out"
        assert proc is None or proc.poll() is None, f"Engine exited {proc.returncode}"
        time.sleep(.03)


def read_json(path):
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return {}


def descendants(pid):
    """Capture the supervisor's tree before stop, for explicit cleanup checks."""
    found = [int(pid)]
    for parent in found:
        try:
            children = Path(f"/proc/{parent}/task/{parent}/children").read_text().split()
        except FileNotFoundError:
            continue
        found.extend(int(child) for child in children)
    return found


def stop_test_engine(proc):
    # On a fixture error the editor may still have a child game with no report.
    # Kill only this test's Godot descendants; their supervisors clean dwl/apps.
    import signal
    for pid in descendants(proc.pid)[1:]:
        try:
            command = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")[0]
            if command == os.fsencode(ENGINE):
                descriptor = os.pidfd_open(pid)
                signal.pidfd_send_signal(descriptor, signal.SIGKILL)
                os.close(descriptor)
        except (ProcessLookupError, FileNotFoundError):
            pass
    proc.kill()
    proc.wait()


def wait_engine_exit(proc, folder):
    try:
        proc.wait(timeout=12)
    except subprocess.TimeoutExpired:
        # Diagnose the previously observed intermittent exit stall before the
        # test's finally block kills its own engine. No attach to other apps.
        with (folder / "exit-stack.log").open("w") as file:
            try:
                subprocess.run(["gdb", "-q", "-batch", "-p", str(proc.pid),
                                "-ex", "set pagination off", "-ex", "thread apply all bt 12"],
                               stdout=file, stderr=subprocess.STDOUT, timeout=20,
                               env={**os.environ, "DEBUGINFOD_URLS": ""})
            except subprocess.TimeoutExpired:
                pass
        raise


def project(folder):
    target = folder / "project"
    shutil.copytree(WORLD, target, ignore=shutil.ignore_patterns("bin", ".godot", "*.uid"))
    (target / "bin").mkdir()
    for binary in ("libdwl_bridge.so", "bridge_session_host"):
        (target / "bin" / binary).symlink_to(WORLD / "bin" / binary)
    (target / ".godot").mkdir()
    (target / ".godot/extension_list.cfg").write_text("res://bridge.gdextension\n")
    # Avoid relying on a prior editor import to discover the named script class.
    (target / ".godot/global_script_class_cache.cfg").write_text('list=[{\n"base": &"Node",\n"class": &"BridgeSession",\n"icon": "",\n"is_abstract": false,\n"is_tool": false,\n"language": &"GDScript",\n"path": "res://bridge/session.gd"\n}]\n')
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-only", action="store_true")
    parser.add_argument("--editor-only", action="store_true")
    parser.add_argument("--shutdown-only", action="store_true", help="ten empty Wayland engine exits, without a bridge session")
    args = parser.parse_args()
    folder = Path(tempfile.mkdtemp(prefix="dwl-bridge-session-test-"))
    print(f"Artifacts: {folder}", flush=True)
    target = project(folder)
    (folder / "vk_layer_settings.txt").write_text("khronos_validation.validate_sync = true\n")
    env = os.environ.copy()
    env.update(XDG_DATA_HOME=str(folder / "data"), XDG_CONFIG_HOME=str(folder / "config"),
               XDG_CACHE_HOME=str(folder / "cache"), BRIDGE_SESSION_TEST_REPORT=str(folder),
               VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation", VK_LOADER_DEBUG="layer",
               VK_LAYER_SETTINGS_PATH=str(folder))
    flags = [str(ENGINE), "--path", str(target), "--display-driver", "wayland", "--gpu-index", "0",
             "--audio-driver", "Dummy"]
    if args.shutdown_only:
        (target / "empty.gd").write_text('extends SceneTree\nfunc _initialize():\n    quit()\n')
        for cycle in range(10):
            with (folder / f"empty-{cycle}.log").open("w") as log:
                proc = subprocess.Popen([*flags, "--script", "res://empty.gd"], env=env,
                    stdout=log, stderr=subprocess.STDOUT, preexec_fn=permit_test_debugger)
                try:
                    wait_engine_exit(proc, folder)
                    assert proc.returncode == 0
                finally:
                    if proc.poll() is None: stop_test_engine(proc)
        print("PASS: ten empty Wayland engine exits", flush=True)
        return
    if args.editor_only:
        editor_test(folder, target, flags, env)
        return
    runtime_script = '''extends Node
var world: Node
var session: Node
var cycle := 0
var stopping := false
var checking := false
var report := OS.get_environment("BRIDGE_SESSION_TEST_REPORT")
func _ready():
    world = load("res://development_world.tscn").instantiate()
    session = world.get_node("BridgeSession")
    session.dwl_executable = %s
    session.application_arguments = PackedStringArray(%s)
    add_child(world)
func _process(_delta):
    if session.updates >= 12 and not stopping and not checking:
        checking = true
        # Freeze new frame scheduling while a diagnostic reads the published RID.
        session.set_process(false)
        var info: Dictionary = session._processes.poll()
        info["updates"] = session.updates
        info["device"] = session.device
        var file := FileAccess.open(report.path_join("running-%%d.json" %% cycle), FileAccess.WRITE)
        file.store_string(JSON.stringify(info))
        file.close()
        RenderingServer.call_on_render_thread(check_pixels)
    if stopping and session.is_stopped():
        var file := FileAccess.open(report.path_join("stopped-%%d" %% cycle), FileAccess.WRITE)
        file.store_string("stopped")
        file.close()
        cycle += 1
        if cycle == 2:
            print("BRIDGE_SESSION_RUNTIME_PASS")
            get_tree().quit()
        else:
            session.application_arguments = PackedStringArray(["weston-simple-egl"])
            session.application_app_id = "org.freedesktop.weston.simple-egl"
            stopping = false
            checking = false
            session.start()
func check_pixels():
    var rd := RenderingServer.get_rendering_device()
    var pixels := rd.texture_get_data(session.texture.texture_rd_rid, 0)
    var file := FileAccess.open(report.path_join("pixels-%%d.rgba" %% cycle), FileAccess.WRITE)
    file.store_buffer(pixels)
    file.close()
    call_deferred("checked")
func checked():
    for _frame in range(3):
        await get_tree().process_frame
        RenderingServer.force_draw()
    var image := get_viewport().get_texture().get_image()
    image.save_png(report.path_join("world-%%d.png" %% cycle))
    # Source/readback success alone misses stale material bindings on restart.
    var camera: Camera3D = world.get_node("Camera")
    var mesh: MeshInstance3D = world.get_node("ApplicationWindow")
    var pixel := image.get_pixelv(Vector2i(camera.unproject_position(mesh.global_position)))
    assert(not (pixel.r > 0.98 and pixel.g > 0.98 and pixel.b > 0.98), "Mesh lost its texture binding")
    assert(pixel.r + pixel.g + pixel.b > 0.05, "Mesh is blank")
    # Let the Python side snapshot owned processes before requesting stop.
    while not FileAccess.file_exists(report.path_join("inspected-%%d" %% cycle)):
        await get_tree().process_frame
    stopping = true
    session.set_process(true)
    session.stop()
'''
    (target / "test_runtime.gd").write_text(runtime_script % (json.dumps(str(REPO / "dwl")), json.dumps(FOOT)))
    (target / "test_runtime.tscn").write_text('[gd_scene load_steps=2 format=3]\n[ext_resource type="Script" path="res://test_runtime.gd" id="1"]\n[node name="Test" type="Node"]\nscript=ExtResource("1")\n')
    all_pids = []
    with (folder / "runtime.log").open("w") as log:
        proc = subprocess.Popen([*flags, "res://test_runtime.tscn"], env=env, stdout=log, stderr=subprocess.STDOUT,
                                preexec_fn=permit_test_debugger)
        try:
            for cycle in range(2):
                record = folder / f"running-{cycle}.json"
                wait_for(lambda: bool(read_json(record)), proc)
                info = read_json(record)
                owned = descendants(info["supervisor_pid"])
                all_pids.extend(owned)
                (folder / f"inspected-{cycle}").touch()
                wait_for((folder / f"stopped-{cycle}").exists, proc)
                for pid in owned:
                    wait_for(lambda: not Path(f"/proc/{pid}").exists())
                assert not Path(info["socket_path"]).exists()
                pixels = (folder / f"pixels-{cycle}.rgba").read_bytes()
                assert len(pixels) > 10000 and len(set(pixels)) > 8, "Texture contains no application content"
                assert (folder / f"world-{cycle}.png").stat().st_size > 1000
            wait_engine_exit(proc, folder)
            assert proc.returncode == 0
        finally:
            if proc.poll() is None:
                stop_test_engine(proc)
    text = (folder / "runtime.log").read_text()
    assert "BRIDGE_SESSION_RUNTIME_PASS" in text and all(word not in text for word in (
        "SCRIPT ERROR", "Validation Error", "VUID-", "SYNC-HAZARD", "ObjectDB instances leaked", "RID allocations")), text
    print("PASS: real SHM foot and DMA-BUF EGL sessions, GPU textures, stop/restart and normal engine exit", flush=True)
    if not args.runtime_only:
        editor_test(folder, target, flags, env)
    (folder / "result.json").write_text(json.dumps({"success": True, "runtime_cycles": 2,
        "editor_play_stop": not args.runtime_only, "owned_pids": all_pids}, indent=2) + "\n")


def editor_test(folder, target, flags, env):
    # This fixture invokes actual EditorInterface Play/Stop, not a lookalike
    # process launcher. The child game uses the very same session script.
    addon = target / "addons/session_test"
    addon.mkdir(parents=True)
    (addon / "plugin.cfg").write_text('[plugin]\nname="Session lifecycle test"\ndescription="Temporary test only"\nauthor="dwl bridge"\nversion="1"\nscript="plugin.gd"\n')
    (addon / "plugin.gd").write_text('''@tool
extends EditorPlugin
var report := OS.get_environment("BRIDGE_SESSION_TEST_REPORT")
func _enter_tree():
    call_deferred("run")
func run():
    for _frame in range(30): await get_tree().process_frame
    for cycle in range(2):
        EditorInterface.play_custom_scene("res://test_editor_game.tscn")
        while not FileAccess.file_exists(report.path_join("editor-game.json")):
            await get_tree().process_frame
        while not FileAccess.file_exists(report.path_join("editor-inspected-%d" % cycle)):
            await get_tree().process_frame
        EditorInterface.stop_playing_scene()
        DirAccess.remove_absolute(report.path_join("editor-game.json"))
        var file := FileAccess.open(report.path_join("editor-stopped-%d" % cycle), FileAccess.WRITE)
        file.store_string("stopped")
        file.close()
        while not FileAccess.file_exists(report.path_join("editor-cleaned-%d" % cycle)):
            await get_tree().process_frame
    print("BRIDGE_SESSION_EDITOR_PASS")
    get_tree().quit()
''')
    (target / "test_editor_game.gd").write_text('''extends Node
var session: Node
var marked := false
func _ready():
    var world: Node = load("res://development_world.tscn").instantiate()
    session = world.get_node("BridgeSession")
    session.dwl_executable = %s
    session.application_arguments = PackedStringArray(%s)
    add_child(world)
func _process(_delta):
    if not marked and session.updates >= 8:
        marked = true
        var info: Dictionary = session._processes.poll()
        info["game_pid"] = OS.get_process_id()
        var file := FileAccess.open(OS.get_environment("BRIDGE_SESSION_TEST_REPORT").path_join("editor-game.json"), FileAccess.WRITE)
        file.store_string(JSON.stringify(info))
        file.close()
''' % (json.dumps(str(REPO / "dwl")), json.dumps(FOOT)))
    (target / "test_editor_game.tscn").write_text('[gd_scene load_steps=2 format=3]\n[ext_resource type="Script" path="res://test_editor_game.gd" id="1"]\n[node name="Game" type="Node"]\nscript=ExtResource("1")\n')
    with (target / "project.godot").open("a") as file:
        file.write('\n[editor_plugins]\nenabled=PackedStringArray("res://addons/session_test/plugin.cfg")\n')
    with (folder / "editor.log").open("w") as log:
        # The editor may choose its normal GPU. The saved project's Run
        # Arguments must select GPU 0 for the child, without a CLI override.
        editor_flags = list(flags)
        index = editor_flags.index("--gpu-index")
        del editor_flags[index:index + 2]
        proc = subprocess.Popen([*editor_flags, "--editor"], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            for cycle in range(2):
                wait_for(lambda: bool(read_json(folder / "editor-game.json")), proc, timeout=55)
                info = read_json(folder / "editor-game.json")
                (folder / f"editor-session-{cycle}.json").write_text(json.dumps(info, indent=2) + "\n")
                owned = descendants(info["supervisor_pid"])
                (folder / f"editor-inspected-{cycle}").touch()
                wait_for((folder / f"editor-stopped-{cycle}").exists, proc)
                for pid in [int(info["game_pid"]), *owned]:
                    wait_for(lambda: not Path(f"/proc/{pid}").exists(), proc, timeout=10)
                assert not Path(info["socket_path"]).exists()
                (folder / f"editor-cleaned-{cycle}").touch()
            proc.wait(timeout=15)
            assert proc.returncode == 0
        finally:
            if proc.poll() is None:
                stop_test_engine(proc)
    text = (folder / "editor.log").read_text()
    assert "BRIDGE_SESSION_EDITOR_PASS" in text and all(word not in text for word in (
        "SCRIPT ERROR", "Validation Error", "VUID-", "SYNC-HAZARD")), text
    print("PASS: real editor Play/Stop/Play; forced game stops cleaned both owned nested sessions", flush=True)


if __name__ == "__main__":
    main()
