#include "protocol.h"
#include "vulkan_helpers.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using namespace proof;
struct OwnedFD {
	int value = -1;
	OwnedFD() = default;
	OwnedFD(const OwnedFD &) = delete;
	~OwnedFD() {
		if (value >= 0)
			close(value);
	}
};
struct Producer {
	Producer() = default;
	Producer(const Producer &) = delete;
	Producer &operator=(const Producer &) = delete;
	VkInstance instance{};
	VkPhysicalDevice gpu{};
	VkDevice device{};
	VkQueue queue{};
	uint32_t family{};
	VkImage image{};
	VkDeviceMemory memory{};
	VkImageView view{};
	VkShaderModule shader{};
	VkDescriptorSetLayout set_layout{};
	VkDescriptorPool descriptors{};
	VkDescriptorSet set{};
	VkPipelineLayout pipeline_layout{};
	VkPipeline pipeline{};
	VkCommandPool pool{};
	VkCommandBuffer command{};
	VkFence fence{};
	VkSemaphore ready_semaphore{}, release_semaphore{};
	bool wait_release = false, legacy = false;
	Frame frame;
	int socket_fd = -1;
	~Producer() {
		if (socket_fd >= 0)
			close(socket_fd);
		if (device) {
			vkDeviceWaitIdle(device);
			if (ready_semaphore)
				vkDestroySemaphore(device, ready_semaphore, nullptr);
			if (release_semaphore)
				vkDestroySemaphore(device, release_semaphore, nullptr);
			if (fence)
				vkDestroyFence(device, fence, nullptr);
			if (pool)
				vkDestroyCommandPool(device, pool, nullptr);
			if (pipeline)
				vkDestroyPipeline(device, pipeline, nullptr);
			if (pipeline_layout)
				vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
			if (descriptors)
				vkDestroyDescriptorPool(device, descriptors, nullptr);
			if (set_layout)
				vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
			if (shader)
				vkDestroyShaderModule(device, shader, nullptr);
			if (view)
				vkDestroyImageView(device, view, nullptr);
			if (image)
				vkDestroyImage(device, image, nullptr);
			if (memory)
				vkFreeMemory(device, memory, nullptr);
			vkDestroyDevice(device, nullptr);
		}
		if (instance)
			vkDestroyInstance(instance, nullptr);
	}
	void initialize(const std::array<uint8_t, 16> &uuid) {
		auto app = structure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
		app.pApplicationName = "dwl controlled DMA-BUF producer";
		app.apiVersion = VK_API_VERSION_1_2;
		auto create = structure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
		create.pApplicationInfo = &app;
		check(vkCreateInstance(&create, nullptr, &instance), "vkCreateInstance");
		uint32_t count = 0;
		check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPUs");
		std::vector<VkPhysicalDevice> devices(count);
		check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate GPUs");
		for (auto candidate : devices)
			if (gpu_uuid(candidate) == uuid)
				gpu = candidate;
		if (!gpu)
			throw std::runtime_error("Requested device UUID not available");
		frame.device_uuid = gpu_uuid(gpu);
		frame.driver_uuid = gpu_uuid(gpu, true);
		vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
		for (family = 0; family < count; family++)
			if (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)
				break;
		if (family == count)
			throw std::runtime_error("No compute queue");
		float priority = 1;
		auto q = structure<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
		q.queueFamilyIndex = family;
		q.queueCount = 1;
		q.pQueuePriorities = &priority;
		const char *extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
									VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
									VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
									VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
		auto d = structure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
		d.queueCreateInfoCount = 1;
		d.pQueueCreateInfos = &q;
		d.enabledExtensionCount = 4;
		d.ppEnabledExtensionNames = extensions;
		check(vkCreateDevice(gpu, &d, nullptr, &device), "vkCreateDevice");
		vkGetDeviceQueue(device, family, 0, &queue);
		uint64_t linear = 0;
		auto external = structure<VkExternalMemoryImageCreateInfo>(
			VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
		external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		auto modifiers = structure<VkImageDrmFormatModifierListCreateInfoEXT>(
			VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT);
		modifiers.pNext = &external;
		modifiers.drmFormatModifierCount = 1;
		modifiers.pDrmFormatModifiers = &linear;
		auto info = image_info(frame.width, frame.height);
		info.pNext = &modifiers;
		check(vkCreateImage(device, &info, nullptr, &image), "create export image");
		VkMemoryRequirements requirements;
		vkGetImageMemoryRequirements(device, image, &requirements);
		if (requirements.size > 64 * 1024 * 1024)
			throw std::runtime_error("Producer allocation too large");
		auto export_info =
			structure<VkExportMemoryAllocateInfo>(VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
		export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		auto dedicated = structure<VkMemoryDedicatedAllocateInfo>(
			VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
		dedicated.image = image;
		dedicated.pNext = &export_info;
		auto allocation = structure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
		allocation.pNext = &dedicated;
		allocation.allocationSize = requirements.size;
		allocation.memoryTypeIndex = memory_type(gpu, requirements.memoryTypeBits);
		check(vkAllocateMemory(device, &allocation, nullptr, &memory), "allocate export memory");
		check(vkBindImageMemory(device, image, memory, 0), "bind export image");
		VkImageSubresource subresource{VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
		VkSubresourceLayout layout{};
		vkGetImageSubresourceLayout(device, image, &subresource, &layout);
		frame.offset = layout.offset;
		frame.stride = layout.rowPitch;
		auto actual = structure<VkImageDrmFormatModifierPropertiesEXT>(
			VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT);
		auto get_modifier = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
			vkGetDeviceProcAddr(device, "vkGetImageDrmFormatModifierPropertiesEXT"));
		if (!get_modifier)
			throw std::runtime_error("Missing modifier entrypoint");
		check(get_modifier(device, image, &actual), "query actual modifier");
		frame.modifier = actual.drmFormatModifier;
		decode(encode(frame).data(),
			   HEADER_BYTES); // enforce the same bounds as receiver
		auto vi = structure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
		vi.image = image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = VK_FORMAT_R8G8B8A8_UNORM;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		check(vkCreateImageView(device, &vi, nullptr, &view), "create storage view");
		std::ifstream file(PATTERN_SPV, std::ios::binary | std::ios::ate);
		if (!file)
			throw std::runtime_error("Shader missing");
		size_t size = static_cast<size_t>(file.tellg());
		std::vector<uint32_t> code(size / 4);
		file.seekg(0);
		file.read(reinterpret_cast<char *>(code.data()), size);
		auto si = structure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
		si.codeSize = size;
		si.pCode = code.data();
		check(vkCreateShaderModule(device, &si, nullptr, &shader), "create shader");
		VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
											 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
		auto sli = structure<VkDescriptorSetLayoutCreateInfo>(
			VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
		sli.bindingCount = 1;
		sli.pBindings = &binding;
		check(vkCreateDescriptorSetLayout(device, &sli, nullptr, &set_layout), "set layout");
		VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
		auto pli =
			structure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
		pli.setLayoutCount = 1;
		pli.pSetLayouts = &set_layout;
		pli.pushConstantRangeCount = 1;
		pli.pPushConstantRanges = &push;
		check(vkCreatePipelineLayout(device, &pli, nullptr, &pipeline_layout), "pipeline layout");
		auto pi =
			structure<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
		pi.layout = pipeline_layout;
		pi.stage = structure<VkPipelineShaderStageCreateInfo>(
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
		pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		pi.stage.module = shader;
		pi.stage.pName = "main";
		check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline),
			  "compute pipeline");
		VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
		auto dpi =
			structure<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
		dpi.maxSets = 1;
		dpi.poolSizeCount = 1;
		dpi.pPoolSizes = &pool_size;
		check(vkCreateDescriptorPool(device, &dpi, nullptr, &descriptors), "descriptor pool");
		auto dai =
			structure<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
		dai.descriptorPool = descriptors;
		dai.descriptorSetCount = 1;
		dai.pSetLayouts = &set_layout;
		check(vkAllocateDescriptorSets(device, &dai, &set), "descriptor set");
		VkDescriptorImageInfo descriptor{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
		auto write = structure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
		write.dstSet = set;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		write.pImageInfo = &descriptor;
		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
		auto cp = structure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
		cp.queueFamilyIndex = family;
		check(vkCreateCommandPool(device, &cp, nullptr, &pool), "command pool");
		auto ca =
			structure<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
		ca.commandPool = pool;
		ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ca.commandBufferCount = 1;
		check(vkAllocateCommandBuffers(device, &ca, &command), "command buffer");
		auto fi = structure<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
		check(vkCreateFence(device, &fi, nullptr, &fence), "fence");
		auto export_semaphore =
			structure<VkExportSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO);
		export_semaphore.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		auto semaphore = structure<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
		semaphore.pNext = &export_semaphore;
		check(vkCreateSemaphore(device, &semaphore, nullptr, &ready_semaphore), "ready semaphore");
		semaphore.pNext = nullptr;
		check(vkCreateSemaphore(device, &semaphore, nullptr, &release_semaphore),
			  "release semaphore");
	}

	void render(uint32_t number) {
		if (number)
			check(vkGetFenceStatus(device, fence),
				  "previous source use must be complete before reuse");
		check(vkResetCommandPool(device, pool, 0), "reset command pool");
		check(vkResetFences(device, 1, &fence), "reset fence");
		auto begin =
			structure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		check(vkBeginCommandBuffer(command, &begin), "begin compute");
		auto b = structure<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
		b.image = image;
		b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		b.oldLayout = number ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
		b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		b.srcQueueFamilyIndex = number ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = number ? family : VK_QUEUE_FAMILY_IGNORED;
		b.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
							 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
							 &b);
		vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1,
								&set, 0, nullptr);
		vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &number);
		vkCmdDispatch(command, (frame.width + 15) / 16, (frame.height + 15) / 16, 1);
		b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		b.dstAccessMask = 0;
		b.srcQueueFamilyIndex = family;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
							 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
							 &b);
		check(vkEndCommandBuffer(command), "end compute");
		auto submit = structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &command;
		VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		if (!legacy) {
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &ready_semaphore;
			if (wait_release) {
				submit.waitSemaphoreCount = 1;
				submit.pWaitSemaphores = &release_semaphore;
				submit.pWaitDstStageMask = &wait_stage;
			}
		}
		check(vkQueueSubmit(queue, 1, &submit, fence), "submit compute");
		if (legacy)
			check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX),
				  "legacy producer completion");
		if (vkGetFenceStatus(device, fence) == VK_NOT_READY)
			std::cout << "PRODUCER_PENDING_BEFORE_SEND" << std::endl;
		frame.frame_id = number;
	}
	void send_frame(const std::string &fault) {
		auto get_fd =
			reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
		if (!get_fd)
			throw std::runtime_error("No memory FD entrypoint");
		auto fi = structure<VkMemoryGetFdInfoKHR>(VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);
		fi.memory = memory;
		fi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		OwnedFD buffer_fd, acquire;
		check(get_fd(device, &fi, &buffer_fd.value), "export DMA-BUF");
		if (!legacy) {
			auto export_ready = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
				vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"));
			auto semaphore =
				structure<VkSemaphoreGetFdInfoKHR>(VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR);
			semaphore.semaphore = ready_semaphore;
			semaphore.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
			check(export_ready(device, &semaphore, &acquire.value), "export acquire sync_file");
			if (fault == "bad-fence") {
				if (acquire.value >= 0)
					close(acquire.value);
				acquire.value = dup(buffer_fd.value);
			}
		}
		frame.acquire_fd = acquire.value >= 0;
		if (!legacy)
			std::cout << "ACQUIRE_SYNC_EXPORTED " << frame.frame_id << std::endl;
		std::vector<uint8_t> packet;
		if (legacy) {
			auto header = encode(frame);
			packet.assign(header.begin(), header.end());
		} else {
			auto header = encode_async(frame);
			packet.assign(header.begin(), header.end());
		}
		if (fault == "size")
			put32(packet.data() + 12, 0xffffffff);
		if (fault == "uuid")
			packet[64] ^= 0xff;
		int fds[3] = {buffer_fd.value, acquire.value, buffer_fd.value};
		size_t count = 1 + unsigned(acquire.value >= 0);
		if (fault == "extra-fd")
			fds[count++] = buffer_fd.value;
		alignas(cmsghdr) char control[CMSG_SPACE(sizeof(fds))]{};
		iovec iov{packet.data(), fault == "empty"		? size_t(0)
								 : fault == "truncated" ? size_t(8)
														: packet.size()};
		msghdr message{};
		message.msg_iov = &iov;
		message.msg_iovlen = 1;
		message.msg_control = control;
		message.msg_controllen = CMSG_SPACE(count * sizeof(int));
		auto c = CMSG_FIRSTHDR(&message);
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(count * sizeof(int));
		std::memcpy(CMSG_DATA(c), fds, count * sizeof(int));
		ssize_t sent = sendmsg(socket_fd, &message, MSG_NOSIGNAL);
		if (sent != static_cast<ssize_t>(iov.iov_len))
			throw std::runtime_error("sendmsg failed");
		if (fault == "disconnect")
			return;
		pollfd p{socket_fd, POLLIN, 0};
		if (poll(&p, 1, 10000) <= 0)
			throw std::runtime_error("Timed out waiting for copy completion");
		uint8_t ack[20]{};
		alignas(cmsghdr) char response_control[CMSG_SPACE(4 * sizeof(int))]{};
		iovec response_iov{ack, sizeof(ack)};
		msghdr response{};
		response.msg_iov = &response_iov;
		response.msg_iovlen = 1;
		response.msg_control = response_control;
		response.msg_controllen = sizeof(response_control);
		ssize_t n = recvmsg(socket_fd, &response, MSG_CMSG_CLOEXEC);
		OwnedFD release;
		unsigned received = 0;
		for (auto ancillary = CMSG_FIRSTHDR(&response); ancillary;
			 ancillary = CMSG_NXTHDR(&response, ancillary)) {
			if (ancillary->cmsg_level != SOL_SOCKET || ancillary->cmsg_type != SCM_RIGHTS)
				continue;
			auto values = reinterpret_cast<int *>(CMSG_DATA(ancillary));
			for (size_t i = 0; i < (ancillary->cmsg_len - CMSG_LEN(0)) / sizeof(int); ++i) {
				if (received++ == 0)
					release.value = values[i];
				else
					close(values[i]);
			}
		}
		if ((response.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || n != (legacy ? 16 : 20) ||
			get32(ack) != (legacy ? 0x41434b31u : 0x41434b32u) || get32(ack + 4) != 0 ||
			get64(ack + 8) != frame.frame_id ||
			(!legacy && (get32(ack + 16) > 1 || get32(ack + 16) != received)))
			throw std::runtime_error("Receiver rejected/disconnected; source will not be reused");
		if (!legacy) {
			// Observe our own submission fence before replacing a semaphore payload.
			// The remote completion is not a Vulkan host synchronization operation.
			check(vkGetFenceStatus(device, fence), "producer fence before release import");
			auto import_release = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
				vkGetDeviceProcAddr(device, "vkImportSemaphoreFdKHR"));
			auto info = structure<VkImportSemaphoreFdInfoKHR>(
				VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR);
			info.semaphore = release_semaphore;
			info.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
			info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
			info.fd = release.value; // -1 is Vulkan's already-signalled sync-FD sentinel
			check(import_release(device, &info), "import release sync_file");
			release.value = -1; // Vulkan consumed the owned FD
			wait_release = true;
			std::cout << "RELEASE_SYNC_IMPORTED " << frame.frame_id << std::endl;
		}
		std::cout << "GPU_COPY_ACK " << frame.frame_id << std::endl;
	}
};

int main(int argc, char **argv) {
	try {
		if (argc < 3)
			throw std::runtime_error(
				"Usage: dmabuf-producer SOCKET DEVICE_UUID [FRAMES=1] [FAULT]");
		std::string uuid = argv[2];
		if (uuid.size() != 32)
			throw std::runtime_error("UUID must have 32 hexadecimal digits");
		std::array<uint8_t, 16> bytes{};
		for (size_t i = 0; i < 16; i++)
			bytes[i] = static_cast<uint8_t>(std::stoul(uuid.substr(i * 2, 2), nullptr, 16));
		int frames = argc > 3 ? std::stoi(argv[3]) : 1;
		if (frames < 1 || frames > 32)
			throw std::runtime_error("Frames must be 1..32");
		std::string fault = argc > 4 ? argv[4] : "";
		Producer producer;
		producer.legacy = fault == "legacy";
		producer.initialize(bytes);
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		std::string path = argv[1];
		if (path.empty() || path[0] != '/' || path.size() >= sizeof(address.sun_path))
			throw std::runtime_error("Invalid socket path");
		std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
		producer.socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
		if (producer.socket_fd < 0 ||
			connect(producer.socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)))
			throw std::runtime_error("Cannot connect receiver");
		for (int i = 0; i < frames; i++) {
			producer.render(i);
			producer.send_frame(fault);
			if (!fault.empty() && fault != "legacy")
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "Producer: " << error.what() << std::endl;
		return 1;
	}
}
