extends SceneTree

var receiver: BridgeReceiver
var path := ""
var report := ""

func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--socket="): path = arg.trim_prefix("--socket=")
		if arg.begins_with("--report="): report = arg.trim_prefix("--report=")
	call_deferred("run")

func mark(value: String) -> void:
	var file := FileAccess.open(report, FileAccess.WRITE)
	file.store_string(value)
	file.close()

func run() -> void:
	receiver = BridgeReceiver.new()
	root.add_child(receiver)
	assert(receiver.start_live(path))
	while receiver.get_stats().received < 3:
		await create_timer(0.01).timeout
	var stats := receiver.get_stats()
	assert(stats.received == 3 and stats.replaced == 2 and stats.pending_bytes == 6)
	var frame := receiver.take_frame()
	assert(frame.pixels == PackedByteArray([3, 3, 3, 3, 3, 3]))
	assert(receiver.take_frame().is_empty())
	receiver.stop()
	assert(receiver.start_live(path))
	mark("restarted")
	while receiver.get_stats().received < 1:
		await create_timer(0.01).timeout
	frame = receiver.take_frame()
	assert(frame.width == 3 and frame.pixels.size() == 9)
	mark("cancel_partial")
	while receiver.get_status() != "Receiving one snapshot":
		await create_timer(0.01).timeout
	receiver.stop()
	assert(not FileAccess.file_exists(path))
	print("LIVE_RECEIVER_OK: one pending frame, latest wins, reconnect, malformed recovery, partial-read cancellation")
	quit()

func _finalize() -> void:
	if is_instance_valid(receiver): receiver.stop()
