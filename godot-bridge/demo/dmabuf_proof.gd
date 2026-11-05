extends Node

var receiver: BridgeDMABufReceiver
var probe: BridgeVulkanProbe
var texture: Texture2DRD
var preview: TextureRect
var window_material: StandardMaterial3D
var window_quad: QuadMesh
var socket_path := ""
var artifacts := ""
var expected_frames := 1
var copied_frames := 0
var copy_busy := false
var finishing := false
var current_rid := RID()
var verify_pixels := false
var gpu_pending := false
var sync_copy := false
var cancel_acquire := false
var cancel_submit := false
var wait_started := 0
var wait_frames := 0
var compositor_mode := false
var live := false
var title: Label
var frame_size := Vector2i()

func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--socket="): socket_path = arg.trim_prefix("--socket=")
		if arg.begins_with("--artifacts="): artifacts = arg.trim_prefix("--artifacts=")
		if arg == "--compositor": compositor_mode = true
		if arg == "--live": live = true
		if arg == "--verify-readback": verify_pixels = true
		if arg == "--sync-copy": sync_copy = true
		if arg == "--cancel-acquire": cancel_acquire = true
		if arg == "--cancel-submit": cancel_submit = true
		if arg.begins_with("--frames="): expected_frames = arg.trim_prefix("--frames=").to_int()
	receiver = BridgeDMABufReceiver.new()
	probe = BridgeVulkanProbe.new()
	add_child(probe)
	texture = Texture2DRD.new()
	preview = TextureRect.new()
	preview.position = Vector2(20, 100)
	preview.size = Vector2(480, 280)
	preview.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	preview.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	preview.texture = texture
	add_child(preview)
	title = Label.new()
	title.text = "Application preview — waiting for a selected window" if compositor_mode else "DMA-BUF proof — GPU-generated pattern, imported and GPU-copied in Godot"
	add_child(title)
	var camera := Camera3D.new()
	camera.position = Vector3(0, 0, 4)
	add_child(camera)
	var mesh := MeshInstance3D.new()
	window_quad = QuadMesh.new()
	window_quad.size = Vector2(2.7, 1.35)
	mesh.mesh = window_quad
	mesh.position = Vector3(1.2, 0, 0)
	mesh.rotation_degrees = Vector3(-8, -15, 0)
	window_material = StandardMaterial3D.new()
	window_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	window_material.albedo_texture = texture
	mesh.material_override = window_material
	add_child(mesh)
	RenderingServer.call_on_render_thread(initialize_gpu)

func write_json(name: String, value: Dictionary) -> void:
	var file := FileAccess.open(artifacts.path_join(name), FileAccess.WRITE)
	file.store_string(JSON.stringify(value, "\t"))
	file.close()

func initialize_gpu() -> void:
	var rd := RenderingServer.get_rendering_device()
	if RenderingServer.get_current_rendering_driver_name() != "vulkan" or rd == null or not rd.has_method("texture_external_snapshot_submit"):
		call_deferred("failed", "Patched Vulkan snapshot-copy engine required")
		return
	var enabled: PackedStringArray = rd.call("get_device_enabled_extensions")
	for name in ["VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd"]:
		if not name in enabled:
			call_deferred("failed", "Required extension not enabled: " + name)
			return
	var physical := rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_PHYSICAL_DEVICE, RID(), 0)
	var report := probe.inspect(physical)
	if compositor_mode:
		if not "VK_EXT_queue_family_foreign" in enabled:
			call_deferred("failed", "Foreign queue ownership extension not enabled")
			return
		var configured := receiver.configure_compositor(physical)
		if configured.has("error"):
			call_deferred("failed", configured.error)
			return
		report.merge(configured)
	call_deferred("start_receiver", report)

func start_receiver(report: Dictionary) -> void:
	if not receiver.start(socket_path):
		failed(receiver.get_status())
		return
	write_json("ready.json", report)

func _process(_delta: float) -> void:
	if not finishing and not copy_busy:
		var status := receiver.get_status()
		if status == "Waiting for acquire fence":
			wait_frames += 1
			if wait_started == 0: wait_started = Time.get_ticks_msec()
			if cancel_acquire and Time.get_ticks_msec() - wait_started > 150:
				finishing = true
				var start := Time.get_ticks_msec()
				receiver.stop()
				write_json("result.json", {"success": true, "cancelled_acquire": true, "copied": copied_frames, "wait_frames": wait_frames, "stop_ms": Time.get_ticks_msec() - start})
				shutdown(0)
				return
		if status not in ["Disconnected", "Listening", "Waiting for acquire fence", "Frame pending GPU copy", "GPU copy completed"]:
			failed(status)
			return
	if not finishing and not copy_busy and (gpu_pending or receiver.has_frame()):
		copy_busy = true
		RenderingServer.call_on_render_thread(copy_frame)

func copy_frame() -> void:
	var rd := RenderingServer.get_rendering_device()
	var result: Dictionary
	if gpu_pending:
		result = receiver.poll_copy(rd)
	elif sync_copy:
		result = receiver.copy_next(rd,
			rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_PHYSICAL_DEVICE, RID(), 0),
			rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0))
	else:
		result = receiver.submit_next(rd,
			rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_PHYSICAL_DEVICE, RID(), 0),
			rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0))
		if result.has("pending") and cancel_submit:
			receiver.discard_copy(rd)
			result = {"cancelled_submit": true}
	gpu_pending = result.has("pending")
	if result.has("texture_rd_rid") and verify_pixels:
		# Test-only GPU readback. The display path does not upload these bytes.
		var pixels := rd.texture_get_data(result.texture_rd_rid, 0)
		var file := FileAccess.open(artifacts.path_join("frame-%d.rgba" % result.frame_id), FileAccess.WRITE)
		file.store_buffer(pixels)
		file.close()
	call_deferred("display_frame", result)

func display_frame(result: Dictionary) -> void:
	copy_busy = false
	if result.has("pending"): return
	if result.has("cancelled_submit"):
		finishing = true
		receiver.stop()
		wait_for_retirement()
		return
	if result.has("error"):
		failed(result.error)
		return
	if result.is_empty(): return
	var previous := current_rid
	current_rid = result.texture_rd_rid
	texture.texture_rd_rid = current_rid
	if previous.is_valid():
		RenderingServer.call_on_render_thread(func(): RenderingServer.get_rendering_device().free_rid(previous))
	copied_frames += 1
	frame_size = Vector2i(result.width, result.height)
	var mesh_width := minf(2.7, 2.4 * float(result.width) / float(result.height))
	var mesh_size := Vector2(mesh_width, mesh_width * float(result.height) / float(result.width))
	if window_quad.size != mesh_size: window_quad.size = mesh_size
	if compositor_mode:
		title.text = "Application preview — %d × %d — %d updates" % [result.width, result.height, copied_frames]
	print("DMABUF_GPU_COPY ", result.frame_id)
	write_json("progress.json", {"copied": copied_frames, "width": result.width, "height": result.height})
	if not live and copied_frames >= expected_frames:
		finishing = true
		finish_run()

func finish_run() -> void:
	# Tests must render even if the window is covered and Wayland stops callbacks.
	for _frame in range(4):
		await get_tree().process_frame
		RenderingServer.force_draw()
	get_viewport().get_texture().get_image().save_png(artifacts.path_join("screenshot.png"))
	write_json("result.json", {"success": true, "copied": copied_frames, "verification_readback": verify_pixels, "wait_frames": wait_frames, "asynchronous": not sync_copy, "compositor": compositor_mode, "width": frame_size.x, "height": frame_size.y})
	shutdown(0)

func wait_for_retirement() -> void:
	RenderingServer.call_on_render_thread(func():
		var rd := RenderingServer.get_rendering_device()
		var count: int = rd.call("texture_external_snapshot_pending_count")
		call_deferred("retirement_checked", count))
	RenderingServer.force_sync()

func retirement_checked(count: int) -> void:
	if count > 0:
		await get_tree().process_frame
		wait_for_retirement()
		return
	write_json("result.json", {"success": true, "cancelled_submit": true, "copied": copied_frames, "pending_copies": count})
	shutdown(0)

func failed(message: String) -> void:
	print("DMABUF_REJECTED ", message)
	write_json("result.json", {"success": false, "error": message, "copied": copied_frames})
	finishing = true
	shutdown(2)

func shutdown(code: int) -> void:
	print("DMABUF_STOP_WORKER")
	receiver.stop()
	texture.texture_rd_rid = RID()
	var final_rid := current_rid
	current_rid = RID()
	RenderingServer.call_on_render_thread(func():
		receiver.discard_copy(RenderingServer.get_rendering_device())
		if final_rid.is_valid(): RenderingServer.get_rendering_device().free_rid(final_rid))
	# Rejection can leave no visible changes to trigger another render submission.
	print("DMABUF_FLUSH_CLEANUP")
	RenderingServer.force_sync()
	print("DMABUF_CLEANUP_COMPLETE")
	get_tree().quit(code)

func _exit_tree() -> void:
	if is_instance_valid(receiver): receiver.stop()
