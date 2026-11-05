#pragma once
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace proof {
template <typename T> T structure(VkStructureType type) {
	T t{};
	t.sType = type;
	return t;
}
inline void check(VkResult r, const char *operation) {
	if (r != VK_SUCCESS)
		throw std::runtime_error(std::string(operation) + ": VkResult " + std::to_string(r));
}
inline uint32_t memory_type(VkPhysicalDevice gpu, uint32_t mask) {
	VkPhysicalDeviceMemoryProperties p;
	vkGetPhysicalDeviceMemoryProperties(gpu, &p);
	for (uint32_t i = 0; i < p.memoryTypeCount; i++)
		if (mask & (1u << i))
			return i;
	throw std::runtime_error("No compatible Vulkan memory type");
}
inline VkImageCreateInfo image_info(uint32_t w, uint32_t h) {
	auto p = structure<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
	p.imageType = VK_IMAGE_TYPE_2D;
	p.format = VK_FORMAT_R8G8B8A8_UNORM;
	p.extent = {w, h, 1};
	p.mipLevels = 1;
	p.arrayLayers = 1;
	p.samples = VK_SAMPLE_COUNT_1_BIT;
	p.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	p.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			  VK_IMAGE_USAGE_STORAGE_BIT;
	p.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	return p;
}
inline std::array<uint8_t, 16> gpu_uuid(VkPhysicalDevice gpu, bool driver = false) {
	auto id = structure<VkPhysicalDeviceIDProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
	auto p = structure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
	p.pNext = &id;
	vkGetPhysicalDeviceProperties2(gpu, &p);
	std::array<uint8_t, 16> out;
	std::memcpy(out.data(), driver ? id.driverUUID : id.deviceUUID, 16);
	return out;
}
} // namespace proof
