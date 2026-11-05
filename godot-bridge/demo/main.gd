extends Control

var window_view: Node3D
var live_toggle: CheckButton
var capture_after := 1
var preview: TextureRect
var status_label: Label
var path_edit: LineEdit
var capture_path := ""
var capturing := false
var pixels_output := ""

func _ready() -> void:
	var layout := VBoxContainer.new()
	layout.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(layout)
	var title := Label.new()
	title.text = "Window bridge — TextureRect (left) and 3D mesh (right)"
	layout.add_child(title)
	var controls := HBoxContainer.new()
	layout.add_child(controls)
	path_edit = LineEdit.new()
	path_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	path_edit.placeholder_text = "Absolute socket path in an owned mode-0700 directory"
	path_edit.text = OS.get_environment("DWL_BRIDGE_SOCKET")
	controls.add_child(path_edit)
	live_toggle = CheckButton.new()
	live_toggle.text = "Live"
	live_toggle.button_pressed = not OS.get_environment("DWL_BRIDGE_LIVE_APP_ID").is_empty()
	controls.add_child(live_toggle)
	var listen := Button.new()
	listen.text = "Start / reconnect"
	listen.pressed.connect(func(): window_view.start(path_edit.text, live_toggle.button_pressed))
	controls.add_child(listen)
	var stop := Button.new()
	stop.text = "Stop"
	stop.pressed.connect(func(): window_view.stop())
	controls.add_child(stop)
	status_label = Label.new()
	layout.add_child(status_label)
	var panels := HSplitContainer.new()
	panels.size_flags_vertical = Control.SIZE_EXPAND_FILL
	layout.add_child(panels)
	preview = TextureRect.new()
	preview.custom_minimum_size = Vector2(540, 400)
	preview.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	preview.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	preview.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	panels.add_child(preview)
	var world := SubViewportContainer.new()
	world.stretch = true
	world.custom_minimum_size = Vector2(540, 400)
	world.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panels.add_child(world)
	var viewport := SubViewport.new()
	viewport.size = Vector2i(540, 600)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	world.add_child(viewport)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 0, 3)
	viewport.add_child(camera)
	window_view = preload("res://window_preview.tscn").instantiate()
	window_view.rotation_degrees = Vector3(-8, -18, 0)
	viewport.add_child(window_view)
	window_view.frame_displayed.connect(display_frame)
	var note := Label.new()
	note.text = "SHM CPU upload. Live captures need DWL_BRIDGE_LIVE_APP_ID in nested dwl. Stop keeps the last image."
	layout.add_child(note)
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--capture-after="):
			capture_after = maxi(1, arg.trim_prefix("--capture-after=").to_int())
		if arg.begins_with("--capture="):
			capture_path = arg.trim_prefix("--capture=")
		if arg.begins_with("--pixels-output="):
			pixels_output = arg.trim_prefix("--pixels-output=")
	if not path_edit.text.is_empty():
		window_view.start(path_edit.text, live_toggle.button_pressed)

func _process(_delta: float) -> void:
	status_label.text = "%s — frames: %d, texture allocations: %d" % [window_view.get_status(), window_view.frames_displayed, window_view.allocations]

func display_frame(texture: ImageTexture, width: int, height: int, pixels: PackedByteArray) -> void:
	preview.texture = texture
	print("BRIDGE_DISPLAYED ", width, "x", height, " frames=", window_view.frames_displayed, " allocations=", window_view.allocations)
	if not pixels_output.is_empty():
		var file := FileAccess.open(pixels_output, FileAccess.WRITE)
		file.store_buffer(pixels)
		file.close()
	if not capture_path.is_empty() and not capturing and window_view.frames_displayed >= capture_after:
		capturing = true
		save_capture()

func save_capture() -> void:
	# A screenshot is an explicit test operation, outside the receiving path.
	for _frame in range(4):
		await RenderingServer.frame_post_draw
	var error := get_viewport().get_texture().get_image().save_png(capture_path)
	get_tree().quit(0 if error == OK else 1)

func _exit_tree() -> void:
	if is_instance_valid(window_view):
		window_view.stop()
