extends RefCounted
## Transient render-thread state. Queued Callables keep this object alive even
## if its scene disappears. Only publish() and emitted signals run on main.

signal completed(result: Dictionary)

var receiver := BridgeDMABufReceiver.new()
var physical := 0
var logical := 0
var pending := false
var current := RID()
var previous := RID()

func publish(result: Dictionary) -> void:
	completed.emit(result)

func initialize() -> void:
	var rd := RenderingServer.get_rendering_device()
	if RenderingServer.get_current_rendering_driver_name() != "vulkan" or rd == null:
		call_deferred("publish", {"error": "Open this project with the patched Vulkan engine."})
		return
	for method in ["get_device_enabled_extensions", "texture_external_snapshot_submit", "texture_external_snapshot_pending_count"]:
		if not rd.has_method(method):
			call_deferred("publish", {"error": "Patched engine API missing: " + method})
			return
	var enabled: PackedStringArray = rd.call("get_device_enabled_extensions")
	for extension in ["VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier", "VK_KHR_external_semaphore_fd", "VK_EXT_queue_family_foreign"]:
		if not extension in enabled:
			call_deferred("publish", {"error": "Vulkan extension not enabled: " + extension})
			return
	physical = rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_PHYSICAL_DEVICE, RID(), 0)
	logical = rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0)
	var result := receiver.configure_compositor(physical)
	result["initialized"] = true
	result["device"] = rd.get_device_name()
	call_deferred("publish", result)

func step() -> void:
	var rd := RenderingServer.get_rendering_device()
	var result: Dictionary = {}
	if pending:
		result = receiver.poll_copy(rd)
	elif receiver.has_frame():
		result = receiver.submit_next(rd, physical, logical)
	pending = result.has("pending")
	if result.has("texture_rd_rid"):
		previous = current
		current = result.texture_rd_rid
	call_deferred("publish", result)

func acknowledge_frame() -> void:
	# Main has replaced the Texture2DRD binding. Engine handles deferred GPU
	# destruction. The previous RID remains tracked if scene exit wins the race.
	if previous.is_valid():
		RenderingServer.get_rendering_device().free_rid(previous)
		previous = RID()

func shutdown() -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null and rd.has_method("texture_external_snapshot_discard"):
		receiver.discard_copy(rd)
		pending = false
		if previous.is_valid(): rd.free_rid(previous)
		if current.is_valid(): rd.free_rid(current)
	previous = RID()
	current = RID()
	check_retirement()

func check_retirement() -> void:
	var rd := RenderingServer.get_rendering_device()
	var remaining := 0
	if rd != null and rd.has_method("texture_external_snapshot_pending_count"):
		remaining = rd.call("texture_external_snapshot_pending_count")
	call_deferred("publish", {"retired": remaining == 0})
