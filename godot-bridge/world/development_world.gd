extends Node3D

@onready var session: BridgeSession = $BridgeSession
@onready var screen: MeshInstance3D = $ApplicationWindow
@onready var preview: TextureRect = $Interface/Preview
@onready var information: Label = $Interface/Status
var _quitting := false
var _material := StandardMaterial3D.new()

func _ready() -> void:
	get_tree().auto_accept_quit = false
	_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	screen.material_override = _material
	session.texture_changed.connect(_bind_texture)
	_bind_texture(session.texture)
	session.status_changed.connect(func(message: String): information.text = message)
	session.frame_ready.connect(_frame_ready)
	session.stopped.connect(_stopped)
	information.text = session.status
	$Interface/Start.pressed.connect(session.start)
	$Interface/Stop.pressed.connect(session.stop)

func _bind_texture(value: Texture2DRD) -> void:
	_material.albedo_texture = value
	preview.texture = value

func _frame_ready(size: Vector2i, _updates: int) -> void:
	var width := minf(3.8, 2.6 * float(size.x) / float(size.y))
	screen.mesh.size = Vector2(width, width * float(size.y) / float(size.x))

func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST:
		_quitting = true
		if session.is_stopped(): get_tree().quit()
		else: session.stop()

func _stopped() -> void:
	if _quitting: get_tree().quit()
