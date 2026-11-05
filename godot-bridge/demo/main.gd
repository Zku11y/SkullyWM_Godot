extends Control

var receiver: BridgeReceiver
var preview: TextureRect
var window_material: StandardMaterial3D
var quad: QuadMesh
var status_label: Label
var path_edit: LineEdit
var capture_path := ""
var capturing := false
var pixels_output := ""

func _ready() -> void:
	receiver = BridgeReceiver.new()
	add_child(receiver)
	var layout := VBoxContainer.new()
	layout.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(layout)
	var title := Label.new()
	title.text = "One captured window — TextureRect (left) and 3D mesh (right)"
	layout.add_child(title)
	var controls := HBoxContainer.new()
	layout.add_child(controls)
	path_edit = LineEdit.new()
	path_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	path_edit.placeholder_text = "Absolute socket path in an owned mode-0700 directory"
	path_edit.text = OS.get_environment("DWL_BRIDGE_SOCKET")
	controls.add_child(path_edit)
	var listen := Button.new()
	listen.text = "Listen for one frame"
	listen.pressed.connect(func(): receiver.start_listening(path_edit.text))
	controls.add_child(listen)
	var stop := Button.new()
	stop.text = "Stop"
	stop.pressed.connect(receiver.stop)
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
	var mesh := MeshInstance3D.new()
	quad = QuadMesh.new()
	quad.size = Vector2(2.4, 1.6)
	mesh.mesh = quad
	mesh.rotation_degrees = Vector3(-8, -18, 0)
	window_material = StandardMaterial3D.new()
	window_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	window_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	mesh.material_override = window_material
	viewport.add_child(mesh)
	var note := Label.new()
	note.text = "SHM snapshot via CPU upload. Start the listener, request a frame in nested dwl, then update foot."
	layout.add_child(note)
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--capture="):
			capture_path = arg.trim_prefix("--capture=")
		if arg.begins_with("--pixels-output="):
			pixels_output = arg.trim_prefix("--pixels-output=")
	if not path_edit.text.is_empty():
		receiver.start_listening(path_edit.text)

func _process(_delta: float) -> void:
	status_label.text = receiver.get_status()
	var frame := receiver.take_frame()
	if frame.is_empty():
		return
	var image := Image.create_from_data(frame.width, frame.height, false, Image.FORMAT_RGB8, frame.pixels)
	var texture := ImageTexture.create_from_image(image)
	preview.texture = texture
	window_material.albedo_texture = texture
	quad.size = Vector2(2.4, 2.4 * float(frame.height) / float(frame.width))
	print("BRIDGE_DISPLAYED ", frame.width, "x", frame.height)
	if not pixels_output.is_empty():
		var file := FileAccess.open(pixels_output, FileAccess.WRITE)
		file.store_buffer(frame.pixels)
		file.close()
	if not capture_path.is_empty() and not capturing:
		capturing = true
		save_capture()

func save_capture() -> void:
	# A screenshot is an explicit test operation, outside the receiving path.
	for _frame in range(4):
		await RenderingServer.frame_post_draw
	var error := get_viewport().get_texture().get_image().save_png(capture_path)
	get_tree().quit(0 if error == OK else 1)

func _exit_tree() -> void:
	if is_instance_valid(receiver):
		receiver.stop()
