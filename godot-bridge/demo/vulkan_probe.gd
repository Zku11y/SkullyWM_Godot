extends SceneTree

var probe: BridgeVulkanProbe
var output_path := ""

func _initialize() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--report="): output_path = arg.trim_prefix("--report=")
	call_deferred("begin")

func begin() -> void:
	probe = BridgeVulkanProbe.new()
	root.add_child(probe)
	RenderingServer.call_on_render_thread(collect)

func collect() -> void:
	var driver := RenderingServer.get_current_rendering_driver_name()
	var rd := RenderingServer.get_rendering_device()
	var report := {
		"engine": Engine.get_version_info(),
		"rendering_driver": driver,
		"rendering_method": RenderingServer.get_current_rendering_method(),
		"on_render_thread": RenderingServer.is_on_render_thread(),
		"enabled_device_extensions": "unknown until correlated with vkCreateDevice capture",
	}
	if driver != "vulkan" or rd == null:
		report.error = "This run has no global Vulkan RenderingDevice"
	else:
		report.physical_device = probe.inspect(rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_PHYSICAL_DEVICE, RID(), 0))
		report.engine_api = {
			"wrap_external_image": rd.has_method("texture_create_from_extension"),
			"enabled_extension_inspection": rd.has_method("get_device_enabled_extensions"),
			"global_rd_submit_sync": "Not used: these APIs are for local devices",
		}
		if rd.has_method("get_device_enabled_extensions"):
			report.enabled_device_extensions = rd.call("get_device_enabled_extensions")
	call_deferred("finish", report)

func finish(report: Dictionary) -> void:
	var file := FileAccess.open(output_path, FileAccess.WRITE)
	if file == null:
		push_error("Cannot write capability report")
		quit(1)
		return
	file.store_string(JSON.stringify(report, "\t"))
	file.close()
	print("VULKAN_PROBE_REPORT ", output_path)
	quit(0 if not report.has("error") and not report.get("physical_device", {}).has("error") else 2)
