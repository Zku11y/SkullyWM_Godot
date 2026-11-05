#include "vulkan_probe.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace godot;
namespace {
template <typename T> T vk_structure(VkStructureType type) {
	T value{};
	value.sType = type;
	return value;
}
String hex64(uint64_t value) {
	char text[19];
	std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(value));
	return String(text);
}
String version(uint32_t value) {
	return String::num_int64(VK_VERSION_MAJOR(value)) + "." +
		String::num_int64(VK_VERSION_MINOR(value)) + "." + String::num_int64(VK_VERSION_PATCH(value));
}
String uuid(const uint8_t *bytes) {
	char text[VK_UUID_SIZE * 2 + 1];
	for (size_t i = 0; i < VK_UUID_SIZE; ++i) std::snprintf(text + i * 2, 3, "%02x", bytes[i]);
	return String(text);
}

Dictionary query_image(VkPhysicalDevice device, VkFormat format, uint64_t modifier, VkImageUsageFlags usage) {
	auto external = vk_structure<VkPhysicalDeviceExternalImageFormatInfo>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
	external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	auto layout = vk_structure<VkPhysicalDeviceImageDrmFormatModifierInfoEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT);
	layout.pNext = &external;
	layout.drmFormatModifier = modifier;
	layout.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	auto info = vk_structure<VkPhysicalDeviceImageFormatInfo2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2);
	info.pNext = &layout;
	info.format = format;
	info.type = VK_IMAGE_TYPE_2D;
	info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	info.usage = usage;
	auto external_properties = vk_structure<VkExternalImageFormatProperties>(VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
	auto properties = vk_structure<VkImageFormatProperties2>(VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2);
	properties.pNext = &external_properties;
	VkResult result = vkGetPhysicalDeviceImageFormatProperties2(device, &info, &properties);
	Dictionary report;
	report["vk_result"] = int(result);
	if (result == VK_SUCCESS) {
		auto flags = external_properties.externalMemoryProperties.externalMemoryFeatures;
		report["importable"] = bool(flags & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
		report["dedicated_only"] = bool(flags & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
		report["max_width"] = properties.imageFormatProperties.maxExtent.width;
		report["max_height"] = properties.imageFormatProperties.maxExtent.height;
	}
	return report;
}
}

void BridgeVulkanProbe::_bind_methods() {
	ClassDB::bind_method(D_METHOD("inspect", "physical_device"), &BridgeVulkanProbe::inspect);
}

Dictionary BridgeVulkanProbe::inspect(uint64_t physical_device) {
	Dictionary report;
	if (!physical_device) { report["error"] = "No Vulkan physical device"; return report; }
	VkPhysicalDevice device = reinterpret_cast<VkPhysicalDevice>(physical_device);
	uint32_t count = 0;
	VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
	if (result != VK_SUCCESS || count > 4096) {
		report["error"] = "Cannot enumerate device extensions"; return report;
	}
	std::vector<VkExtensionProperties> extensions(count);
	result = vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
	if (result != VK_SUCCESS) { report["error"] = "Device extension enumeration changed/failed"; return report; }
	PackedStringArray names;
	auto supports = [&](const char *name) {
		return std::any_of(extensions.begin(), extensions.end(), [&](const auto &entry) { return !std::strcmp(entry.extensionName, name); });
	};
	for (const auto &entry : extensions) names.append(entry.extensionName);
	names.sort();
	report["supported_device_extensions"] = names;
	auto id = vk_structure<VkPhysicalDeviceIDProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
	auto drm = vk_structure<VkPhysicalDeviceDrmPropertiesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT);
	if (supports(VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) id.pNext = &drm;
	auto properties = vk_structure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
	properties.pNext = &id;
	vkGetPhysicalDeviceProperties2(device, &properties);
	report["device_name"] = properties.properties.deviceName;
	report["device_uuid"] = uuid(id.deviceUUID);
	report["driver_uuid"] = uuid(id.driverUUID);
	report["vendor_id"] = properties.properties.vendorID;
	report["device_id"] = properties.properties.deviceID;
	report["api_version"] = version(properties.properties.apiVersion);
	report["driver_version_raw"] = properties.properties.driverVersion;
	if (drm.hasRender) {
		report["render_major"] = drm.renderMajor;
		report["render_minor"] = drm.renderMinor;
	}
	if (drm.hasPrimary) {
		report["primary_major"] = drm.primaryMajor;
		report["primary_minor"] = drm.primaryMinor;
	}
	auto sync_info = vk_structure<VkPhysicalDeviceExternalSemaphoreInfo>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO);
	sync_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
	auto sync_properties = vk_structure<VkExternalSemaphoreProperties>(VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES);
	vkGetPhysicalDeviceExternalSemaphoreProperties(device, &sync_info, &sync_properties);
	report["sync_fd_import_supported"] = bool(sync_properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
	report["sync_fd_export_supported"] = bool(sync_properties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT);
	Array formats;
	if (supports(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) && supports(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
		for (VkFormat format : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
			auto modifiers = vk_structure<VkDrmFormatModifierPropertiesListEXT>(VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT);
			auto format_properties = vk_structure<VkFormatProperties2>(VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2);
			format_properties.pNext = &modifiers;
			vkGetPhysicalDeviceFormatProperties2(device, format, &format_properties);
			if (modifiers.drmFormatModifierCount > 4096) continue;
			std::vector<VkDrmFormatModifierPropertiesEXT> entries(modifiers.drmFormatModifierCount);
			modifiers.pDrmFormatModifierProperties = entries.data();
			vkGetPhysicalDeviceFormatProperties2(device, format, &format_properties);
			for (size_t i = 0; i < std::min<size_t>(entries.size(), modifiers.drmFormatModifierCount); ++i) {
				const auto &entry = entries[i];
				Dictionary row;
				row["vk_format"] = int(format);
				row["vk_format_name"] = format == VK_FORMAT_B8G8R8A8_UNORM ? "B8G8R8A8_UNORM" : "R8G8B8A8_UNORM";
				row["modifier"] = hex64(entry.drmFormatModifier);
				row["memory_plane_count"] = entry.drmFormatModifierPlaneCount;
				row["tiling_features"] = hex64(entry.drmFormatModifierTilingFeatures);
				row["transfer_src"] = query_image(device, format, entry.drmFormatModifier, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
				row["sampled"] = query_image(device, format, entry.drmFormatModifier, VK_IMAGE_USAGE_SAMPLED_BIT);
				formats.append(row);
			}
		}
	}
	report["modifier_queries"] = formats;
	report["scope"] = "Physical-device support queries only; no image import, allocation, submission or synchronization performed";
	return report;
}
