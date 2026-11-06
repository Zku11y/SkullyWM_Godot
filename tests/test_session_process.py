"""Headless native-session tests: argv/env isolation, stubborn descendants, owner death.

The fake compositor deliberately uses setsid, like dwl's startup command. No
display/GPU is required. The test never enumerates or kills desktop processes.
"""
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[1]
ENGINE = REPO / "engine/source/bin/godot.linuxbsd.editor.x86_64.llvm.dwl_bridge"
WORLD = REPO / "godot-bridge/world"


def wait_for(predicate, proc=None, timeout=12):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline, "Timed out"
        assert proc is None or proc.poll() is None, f"Engine exited {proc.returncode}"
        time.sleep(.02)


def main():
    folder = Path(tempfile.mkdtemp(prefix="dwl-session-process-test-"))
    print(f"Artifacts: {folder}", flush=True)
    (folder / "bin").mkdir()
    for binary in ("libdwl_bridge.so", "bridge_session_host"):
        (folder / "bin" / binary).symlink_to(WORLD / "bin" / binary)
    (folder / "bridge.gdextension").write_text((WORLD / "bridge.gdextension").read_text())
    (folder / "project.godot").write_text('config_version=5\n[application]\nconfig/name="Session Process Test"\n')
    (folder / ".godot").mkdir()
    (folder / ".godot/extension_list.cfg").write_text("res://bridge.gdextension\n")
    backend = folder / "fake compositor.py"
    backend.write_text("""#!/usr/bin/python3
import os, signal, subprocess, sys, time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
subprocess.Popen(['/bin/sh', '-c', sys.argv[-1]], start_new_session=True)
while True: time.sleep(.1)
""")
    backend.chmod(0o700)
    app = folder / "fake app.py"
    app.write_text("""import json, os, signal, subprocess, sys, time
signal.signal(signal.SIGTERM, signal.SIG_IGN)
child = subprocess.Popen([sys.executable, '-c', 'import signal,time; signal.signal(signal.SIGTERM,signal.SIG_IGN); time.sleep(120)'], start_new_session=True)
inherited_sockets = []
for fd in os.listdir('/proc/self/fd'):
    try:
        if os.readlink('/proc/self/fd/' + fd).startswith('socket:'): inherited_sockets.append(fd)
    except FileNotFoundError: pass
with open(sys.argv[1], 'w') as file:
    json.dump({'pid':os.getpid(), 'grandchild':child.pid, 'argv':sys.argv[2:],
               'inherited_sockets':inherited_sockets,
               'env':{k:v for k,v in os.environ.items() if k.startswith(('DWL_BRIDGE_', 'WLR_'))}}, file)
while True: time.sleep(.1)
""")
    marker = folder / "shell-expansion-must-not-happen"
    literal = f"space ' quote \" newline\n$(touch {marker}) `false` $HOME"
    arguments = ["/usr/bin/python3", str(app), str(folder / "application.json"), literal, ""]
    template = '''extends SceneTree
var session := BridgeSessionProcess.new()
var started := 0
var stopping := false
var cycles := 0
var directory := ""
var reports: Array = []
func _initialize():
    assert(not session.prepare("/missing", "/missing", PackedStringArray(), "", 0))
    launch()
func launch():
    assert(session.prepare(%s, %s, PackedStringArray(%s), "native-test", 60), str(session.poll()))
    directory = session.poll().runtime_directory
    assert(session.start(), str(session.poll()))
    assert(OS.get_environment("DWL_BRIDGE_SOCKET") == "parent-marker")
    started = Time.get_ticks_msec()
    stopping = false
func _process(_delta):
    assert(Time.get_ticks_msec() - started < 10000, str(session.poll()))
    var info := session.poll()
    if not stopping and FileAccess.file_exists(%s):
        var app: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(%s))
        if info.dwl_pid <= 0: return false
        info["application"] = app
        reports.append(info)
        var file := FileAccess.open(%s, FileAccess.WRITE)
        file.store_string(JSON.stringify(reports))
        file.close()
        %s
    if stopping and info.state == "stopped":
        DirAccess.remove_absolute(%s)
        cycles += 1
        if cycles < 3: launch()
        else:
            print("SESSION_PROCESS_PASS")
            quit()
    return false
'''
    env = os.environ.copy()
    env.update(XDG_DATA_HOME=str(folder / "data"), XDG_CONFIG_HOME=str(folder / "config"),
               DWL_BRIDGE_SOCKET="parent-marker", WLR_BACKENDS="parent-marker")
    sentinel = subprocess.Popen(["sleep", "120"])
    try:
        for killed in (False, True):
            output = folder / ("kill-report.json" if killed else "normal-report.json")
            values = tuple(json.dumps(value) for value in (
                str(WORLD / "bin/bridge_session_host"), str(backend), arguments,
                str(folder / "application.json"), str(folder / "application.json"), str(output)))
            script = template % (*values, "stopping = true" if killed else "stopping = true\n        session.stop()",
                                  json.dumps(str(folder / "application.json")))
            (folder / "test.gd").write_text(script)
            with (folder / ("kill.log" if killed else "normal.log")).open("w") as log:
                proc = subprocess.Popen([str(ENGINE), "--headless", "--path", str(folder),
                                         "--script", "res://test.gd"], env=env, stdout=log, stderr=subprocess.STDOUT)
                try:
                    if killed:
                        wait_for(output.exists, proc)
                        proc.kill()  # The editor's Linux Stop path also uses SIGKILL.
                    proc.wait(timeout=30)
                finally:
                    if proc.poll() is None:
                        proc.kill()
                        proc.wait()
            text = (folder / ("kill.log" if killed else "normal.log")).read_text()
            assert "SCRIPT ERROR" not in text, text
            if not killed:
                assert proc.returncode == 0 and "SESSION_PROCESS_PASS" in text, text
            reports = json.loads(output.read_text())
            assert len(reports) == (1 if killed else 3), reports
            for report in reports:
                application = report["application"]
                assert application["argv"] == [literal, ""], application
                assert not application["inherited_sockets"], application
                child_env = application["env"]
                assert child_env == {"WLR_BACKENDS": "wayland", "WLR_RENDERER": "gles2",
                    "DWL_BRIDGE_GPU_SOCKET": report["socket_path"], "DWL_BRIDGE_GPU_APP_ID": "native-test",
                    "DWL_BRIDGE_FPS": "60"}, child_env
                assert Path(report["runtime_directory"]).stat().st_mode & 0o777 == 0o700
                for pid in (report["supervisor_pid"], report["dwl_pid"], application["pid"], application["grandchild"]):
                    pid = int(pid)
                    wait_for(lambda: not Path(f"/proc/{pid}").exists(), timeout=8)
            assert not marker.exists(), "Arguments were evaluated as shell code"
            assert sentinel.poll() is None, "Unrelated process was stopped"
        (folder / "application.json").unlink()
        (folder / "drop_owner.gd").write_text('''extends SceneTree
func _initialize():
    call_deferred("run")
func run():
    var controller := BridgeSessionProcess.new()
    assert(controller.prepare(%s, %s, PackedStringArray(%s), "native-test", 60))
    assert(controller.start())
    while not FileAccess.file_exists(%s): await process_frame
    var info := controller.poll()
    info["application"] = JSON.parse_string(FileAccess.get_file_as_string(%s))
    var file := FileAccess.open(%s, FileAccess.WRITE)
    file.store_string(JSON.stringify(info))
    file.close()
    controller = null # Unexpected removal while the engine remains alive.
    await create_timer(2.0).timeout
    print("SESSION_OWNER_DROP_PASS")
    quit()
''' % tuple(json.dumps(value) for value in (str(WORLD / "bin/bridge_session_host"), str(backend),
                  arguments, str(folder / "application.json"), str(folder / "application.json"), str(folder / "drop-report.json"))))
        with (folder / "drop.log").open("w") as log:
            proc = subprocess.Popen([str(ENGINE), "--headless", "--path", str(folder), "--script", "res://drop_owner.gd"],
                                     env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                proc.wait(timeout=12)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
        text = (folder / "drop.log").read_text()
        assert proc.returncode == 0 and "SESSION_OWNER_DROP_PASS" in text and "SCRIPT ERROR" not in text, text
        info = json.loads((folder / "drop-report.json").read_text())
        for pid in (info["supervisor_pid"], info["dwl_pid"], info["application"]["pid"], info["application"]["grandchild"]):
            pid = int(pid)
            wait_for(lambda: not Path(f"/proc/{pid}").exists())
        shutil.copytree(WORLD / "bridge", folder / "bridge")
        (folder / "scene_errors.gd").write_text('''extends SceneTree
const Session = preload("res://bridge/session.gd")
func _initialize():
    call_deferred("run")
func run():
    var session := Session.new()
    session.auto_start = false
    session.dwl_executable = %s
    session.supervisor_executable = %s
    root.add_child(session)
    session.start()
    while not session.is_stopped(): await process_frame
    assert(session.status.contains("patched Vulkan"), session.status)
    assert(session._processes.poll().supervisor_pid <= 0)
    session.start()
    session.stop() # Stop before initialize's deferred completion is delivered.
    while not session.is_stopped(): await process_frame
    session.dwl_executable = "/missing/compositor"
    session.start()
    assert(session.is_stopped() and session.status.contains("executable paths"), session.status)
    session.queue_free()
    for _frame in range(3): await process_frame
    print("SESSION_SCENE_FAILURES_PASS")
    quit()
''' % (json.dumps(str(backend)), json.dumps(str(WORLD / "bin/bridge_session_host"))))
        with (folder / "scene-errors.log").open("w") as log:
            proc = subprocess.Popen([str(ENGINE), "--headless", "--path", str(folder),
                                     "--script", "res://scene_errors.gd"], env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                proc.wait(timeout=15)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
        text = (folder / "scene-errors.log").read_text()
        assert proc.returncode == 0 and "SESSION_SCENE_FAILURES_PASS" in text and "SCRIPT ERROR" not in text, text
        print("PASS: three native start/stop cycles, literal argv, isolated environment, stubborn setsid descendants, SIGKILL owner cleanup", flush=True)
        print("PASS: owner removal, no inherited sockets, unsupported renderer, early cancellation and missing executable", flush=True)
    finally:
        sentinel.terminate()
        sentinel.wait()


if __name__ == "__main__":
    main()
