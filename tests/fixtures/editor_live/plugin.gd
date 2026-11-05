@tool
extends EditorPlugin

var frame_count := 0
var texture_id := 0
var current_view: Node3D
var current_cycle := 0

func mark(name: String) -> void:
	var file := FileAccess.open(OS.get_environment("BRIDGE_TEST_REPORT") + "/" + name, FileAccess.WRITE)
	file.store_string(name)
	file.close()

func _enter_tree() -> void:
	call_deferred("run")

func run() -> void:
	# Wait for editor initialization, then exercise its real edited scene tree.
	for _frame in range(30):
		await get_tree().process_frame
	for cycle in range(2):
		current_cycle = cycle
		EditorInterface.open_scene_from_path("res://window_preview.tscn")
		for _frame in range(20):
			await get_tree().process_frame
		current_view = EditorInterface.get_edited_scene_root()
		assert(current_view != null and current_view.has_method("start"))
		assert(Engine.is_editor_hint())
		frame_count = 0
		texture_id = 0
		current_view.frame_displayed.connect(check_frame)
		current_view.start(OS.get_environment("DWL_BRIDGE_SOCKET"), true)
		# A fast producer can already be receiving before this coroutine resumes.
		while current_view.get_status() == "Disconnected":
			await get_tree().process_frame
		mark("ready%d" % cycle)
		if OS.get_environment("BRIDGE_TEST_HOLD") == "1":
			print("EDITOR_PREVIEW_READY: waiting until the Python runner is stopped")
			return
		while frame_count < 3:
			await get_tree().process_frame
		if cycle == 0:
			for _frame in range(5):
				await get_tree().process_frame
			RenderingServer.force_draw()
			get_viewport().get_texture().get_image().save_png(OS.get_environment("BRIDGE_TEST_REPORT") + "/editor.png")
		# Closing the scene must release its listener without an explicit stop call.
		assert(EditorInterface.close_scene() == OK)
		for _frame in range(10):
			await get_tree().process_frame
		mark("closed%d" % cycle)
	print("EDITOR_LIVE_OK: updates, texture reuse, resize and scene close/reopen")
	get_tree().quit()

func check_frame(texture: ImageTexture, width: int, height: int, pixels: PackedByteArray) -> void:
	frame_count += 1
	if OS.get_environment("BRIDGE_TEST_HOLD") == "1":
		if frame_count == 1:
			print("EDITOR_PREVIEW_FRAME ", width, "x", height)
		return
	if OS.get_environment("BRIDGE_TEST_NESTED") == "1":
		assert(width > 0 and height > 0 and texture.get_image().get_data() == pixels)
		mark("frame%d_%d" % [current_cycle, frame_count])
		return
	assert(width == (3 if frame_count == 3 else 2) and height == 1)
	if frame_count == 1:
		texture_id = texture.get_instance_id()
	assert(texture.get_instance_id() == texture_id)
	assert(current_view.allocations == (2 if frame_count == 3 else 1))
	assert(pixels[0] == frame_count * 60)
	# Explicit test readback verifies the uploaded texture, not just the socket data.
	assert(texture.get_image().get_data() == pixels)
	mark("frame%d_%d" % [current_cycle, frame_count])
