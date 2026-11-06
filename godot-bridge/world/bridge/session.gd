class_name BridgeSession
extends Node
## One configured application per session. No @tool execution during editing.

signal status_changed(message: String)
signal frame_ready(size: Vector2i, updates: int)
signal texture_changed(value: Texture2DRD)
signal stopped

const GPUState = preload("res://bridge/session_gpu.gd")

@export var auto_start := true
@export_file var dwl_executable := "../../dwl"
@export_file var supervisor_executable := "res://bin/bridge_session_host"
#@export var application_arguments := PackedStringArray(["foot", "--app-id=bridge-world-test", "-o", "font=monospace:size=18"])
#@export var application_app_id := "bridge-world-test"
@export var application_arguments := PackedStringArray(["mpv", "/home/skully/movies/Limitless/Limitless.mp4"])
@export var application_app_id := "mpv"
@export_range(1, 60) var maximum_fps := 60
@export_range(1, 60) var first_frame_timeout_seconds := 20

var texture := Texture2DRD.new()
var updates := 0
var status := "Stopped"
var runtime_directory := ""
var device := ""
var _processes := BridgeSessionProcess.new()
var _gpu: GPUState
var _state := "stopped"
var _busy := false
var _retired := true
var _started_at := 0
var _stop_started_at := 0
var _warned := false
var _generation := 0

func _ready() -> void:
	# This node continues supervising even if the game scene is paused.
	process_mode = Node.PROCESS_MODE_ALWAYS
	if auto_start: start()

func _absolute(path: String) -> String:
	if path.begins_with("res://") or path.begins_with("user://"):
		return ProjectSettings.globalize_path(path)
	if path.is_absolute_path(): return path
	return ProjectSettings.globalize_path("res://").path_join(path).simplify_path()

func _set_status(message: String) -> void:
	status = message
	status_changed.emit(message)

func start() -> void:
	if _state != "stopped": return
	updates = 0
	if not _processes.prepare(_absolute(supervisor_executable), _absolute(dwl_executable), application_arguments, application_app_id, maximum_fps):
		_set_status(_processes.poll().error)
		return
	runtime_directory = _processes.poll().runtime_directory
	print("BRIDGE_SESSION_DIRECTORY ", runtime_directory)
	# Clearing Texture2DRD destroys its RenderingServer RID. Materials cache that
	# RID, so a restarted session publishes a new Resource and binding explicitly.
	texture = Texture2DRD.new()
	texture_changed.emit(texture)
	_gpu = GPUState.new()
	_generation += 1
	_gpu.completed.connect(_gpu_completed.bind(_generation))
	_state = "initializing"
	_retired = false
	_busy = true
	_started_at = Time.get_ticks_msec()
	_set_status("Checking Vulkan device…")
	RenderingServer.call_on_render_thread(_gpu.initialize)

func stop() -> void:
	if _state in ["stopped", "stopping"]: return
	_state = "stopping"
	_stop_started_at = Time.get_ticks_msec()
	_warned = false
	_set_status("Stopping session…")
	_processes.stop()
	# Stop socket work before discarding the GPU ticket. Engine retains imports
	# until completion; compositor exports remain abandoned rather than reused.
	_gpu.receiver.stop()
	texture.texture_rd_rid = RID()
	_busy = true
	RenderingServer.call_on_render_thread(_gpu.shutdown)

func _fail(message: String) -> void:
	stop()
	_set_status(message + " — " + runtime_directory.path_join("dwl.log"))

func _gpu_completed(result: Dictionary, generation: int) -> void:
	if generation != _generation: return
	_busy = false
	if _state == "stopping":
		if result.has("retired"): _retired = result.retired
		return
	if _state == "stopped": return
	if result.has("error"):
		_fail(result.error)
		return
	if result.has("initialized"):
		device = result.device
		if not _gpu.receiver.start(_processes.poll().socket_path):
			_fail(_gpu.receiver.get_status())
			return
		if not _processes.start():
			_fail(_processes.poll().error)
			return
		_state = "running"
		_set_status("Waiting for app ID '%s' on %s…" % [application_app_id, device])
	elif result.has("texture_rd_rid"):
		texture.texture_rd_rid = result.texture_rd_rid
		RenderingServer.call_on_render_thread(_gpu.acknowledge_frame)
		updates += 1
		var size := Vector2i(result.width, result.height)
		_set_status("%s — %d × %d — %d updates" % [application_app_id, size.x, size.y, updates])
		frame_ready.emit(size, updates)

func _process(_delta: float) -> void:
	var process_info := _processes.poll()
	if _state == "stopping":
		if not _busy and not _retired:
			_busy = true
			RenderingServer.call_on_render_thread(_gpu.check_retirement)
		if _retired and process_info.supervisor_pid <= 0:
			_gpu = null
			_state = "stopped"
			if status == "Stopping session…": _set_status("Stopped")
			stopped.emit()
		elif not _warned and Time.get_ticks_msec() - _stop_started_at > 5000:
			_warned = true
			_set_status("Waiting for GPU/process cleanup — " + runtime_directory)
		return
	if _state not in ["running", "initializing"]: return
	if Time.get_ticks_msec() - _started_at > first_frame_timeout_seconds * 1000 and updates == 0:
		_fail("No frame: check the native app ID, matching GPU, and application startup")
		return
	if _state == "running":
		if process_info.state == "error":
			_fail(process_info.error)
			return
		var receiver_status: String = _gpu.receiver.get_status()
		if receiver_status not in ["Listening", "Waiting for acquire fence", "Frame pending GPU copy", "GPU copy completed"]:
			_fail(receiver_status)
			return
		if not _busy:
			_busy = true
			RenderingServer.call_on_render_thread(_gpu.step)

func is_stopped() -> bool:
	return _state == "stopped"

func _exit_tree() -> void:
	# F8/SIGKILL never reaches here; the supervisor lease covers that path.
	_processes.stop()
	if _gpu != null:
		_gpu.receiver.stop()
		texture.texture_rd_rid = RID()
		RenderingServer.call_on_render_thread(_gpu.shutdown)
