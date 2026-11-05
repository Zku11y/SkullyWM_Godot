@tool
extends Node3D

signal frame_displayed(texture: ImageTexture, width: int, height: int, pixels: PackedByteArray)

@export var socket_path := "":
	set(value):
		socket_path = value
		queue_connection()
@export var live := true:
	set(value):
		live = value
		queue_connection()
@export var enabled := false:
	set(value):
		enabled = value
		queue_connection()

var receiver: BridgeReceiver
var texture: ImageTexture
var frames_displayed := 0
var allocations := 0
var quad: QuadMesh
var window_material: StandardMaterial3D
var connection_queued := false

func _enter_tree() -> void:
	if not is_instance_valid(receiver):
		receiver = BridgeReceiver.new()
		add_child(receiver)
		var mesh := MeshInstance3D.new()
		quad = QuadMesh.new()
		quad.size = Vector2(2.4, 1.6)
		mesh.mesh = quad
		window_material = StandardMaterial3D.new()
		window_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		window_material.cull_mode = BaseMaterial3D.CULL_DISABLED
		mesh.material_override = window_material
		add_child(mesh)
	queue_connection()

func queue_connection() -> void:
	if is_inside_tree() and not connection_queued:
		connection_queued = true
		call_deferred("apply_connection")

func apply_connection() -> void:
	connection_queued = false
	if not is_inside_tree() or not is_instance_valid(receiver):
		return
	receiver.stop()
	if enabled and not socket_path.is_empty():
		if live:
			receiver.start_live(socket_path)
		else:
			receiver.start_listening(socket_path)

func start(path: String, continuously: bool) -> void:
	socket_path = path
	live = continuously
	enabled = true
	queue_connection()

func stop() -> void:
	enabled = false
	if is_instance_valid(receiver):
		receiver.stop()

func get_status() -> String:
	return receiver.get_status() if is_instance_valid(receiver) else "Disconnected"

func _process(_delta: float) -> void:
	if not is_instance_valid(receiver):
		return
	var frame := receiver.take_frame()
	if frame.is_empty():
		return
	var image := Image.create_from_data(frame.width, frame.height, false, Image.FORMAT_RGB8, frame.pixels)
	if texture == null:
		texture = ImageTexture.create_from_image(image)
		allocations += 1
	elif texture.get_width() != frame.width or texture.get_height() != frame.height:
		texture.set_image(image)
		allocations += 1
	else:
		texture.update(image)
	window_material.albedo_texture = texture
	quad.size = Vector2(2.4, 2.4 * float(frame.height) / float(frame.width))
	frames_displayed += 1
	frame_displayed.emit(texture, frame.width, frame.height, frame.pixels)

func _exit_tree() -> void:
	if is_instance_valid(receiver):
		receiver.stop()
