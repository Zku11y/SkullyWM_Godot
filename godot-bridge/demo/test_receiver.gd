extends SceneTree

var receiver: BridgeReceiver
var socket_path := ""
var output_path := ""
var reject := false
var cancel := false

func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--socket="): socket_path = arg.trim_prefix("--socket=")
		if arg.begins_with("--output="): output_path = arg.trim_prefix("--output=")
		if arg == "--reject": reject = true
		if arg == "--cancel": cancel = true
	call_deferred("run")

func run() -> void:
	receiver = BridgeReceiver.new()
	root.add_child(receiver)
	if not receiver.start_listening(socket_path):
		push_error(receiver.get_status())
		quit(1)
		return
	var deadline := Time.get_ticks_msec() + 7500
	while Time.get_ticks_msec() < deadline:
		await create_timer(0.01).timeout
		if cancel:
			receiver.stop()
			print("RECEIVER_CANCELLED")
			quit(0)
			return
		var frame := receiver.take_frame()
		if not frame.is_empty():
			if reject:
				quit(1)
				return
			var file := FileAccess.open(output_path, FileAccess.WRITE)
			file.store_buffer(frame.pixels)
			file.close()
			print("RECEIVER_OK ", frame.width, "x", frame.height)
			quit(0)
			return
		var status := receiver.get_status()
		if not status.begins_with("Listening:") and status != "Receiving one snapshot":
			print("RECEIVER_REJECTED ", status)
			quit(0 if reject else 1)
			return
	push_error("Receiver test deadline exceeded")
	quit(1)

func _finalize() -> void:
	if is_instance_valid(receiver): receiver.stop()
