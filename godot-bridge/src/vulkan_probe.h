#pragma once
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/dictionary.hpp>

// Read-only diagnostic. Call on Godot's render thread with its Vulkan device.
class BridgeVulkanProbe : public godot::Node {
	GDCLASS(BridgeVulkanProbe, godot::Node)
protected:
	static void _bind_methods();
public:
	godot::Dictionary inspect(uint64_t physical_device);
};
