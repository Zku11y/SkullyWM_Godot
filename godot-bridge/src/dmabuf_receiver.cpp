#include "dmabuf_receiver.h"
#include "../../dmabuf-proof/vulkan_helpers.h"
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <linux/sync_file.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace godot;
using namespace proof;
namespace {
struct FD {
	int value = -1;
	explicit FD(int descriptor = -1) : value(descriptor) {}
	FD(const FD &) = delete;
	FD &operator=(const FD &) = delete;
	~FD() {
		if (value >= 0)
			close(value);
	}
};
struct ImportedImage {
	VkDevice device;
	VkImage image{};
	VkDeviceMemory memory{};
	explicit ImportedImage(VkDevice p_device) : device(p_device) {}
	ImportedImage(const ImportedImage &) = delete;
	ImportedImage &operator=(const ImportedImage &) = delete;
	~ImportedImage() {
		if (image)
			vkDestroyImage(device, image, nullptr);
		if (memory)
			vkFreeMemory(device, memory, nullptr);
	}
};
bool readable(int fd, std::atomic<bool> &cancelled) {
	while (!cancelled) {
		pollfd p{fd, POLLIN, 0};
		int n = poll(&p, 1, 50);
		if (n > 0)
			return !(p.revents & (POLLERR | POLLNVAL));
		if (n < 0 && errno != EINTR)
			return false;
	}
	return false;
}
// Wait only on the socket worker. A stalled producer cannot block Godot's GPU
// queue.
void wait_acquire(int fd, std::atomic<bool> &cancelled) {
	sync_file_info info{};
	if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0)
		throw std::runtime_error("Acquire descriptor is not a sync_file");
	// Deterministic test hold: delay consuming a real sync_file, without placing
	// an artificial unresolved semaphore dependency on any GPU queue.
	const char *hold = std::getenv("DWL_BRIDGE_TEST_DELAY_ACQUIRE");
	if (hold && std::strcmp(hold, "1") == 0) {
		auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
		while (!cancelled && std::chrono::steady_clock::now() < until)
			poll(nullptr, 0, 25);
	}
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!cancelled) {
		pollfd event{fd, POLLIN, 0};
		int result = poll(&event, 1, 25);
		if (result > 0) {
			info = {};
			if ((event.revents & (POLLERR | POLLNVAL)) || ioctl(fd, SYNC_IOC_FILE_INFO, &info) ||
				info.status < 0)
				throw std::runtime_error("Acquire fence failed");
			if (info.status > 0)
				return;
		}
		if (result < 0 && errno != EINTR)
			throw std::runtime_error("Acquire poll failed");
		if (std::chrono::steady_clock::now() >= deadline)
			throw std::runtime_error("Acquire fence timed out");
	}
	throw std::runtime_error("Cancelled");
}
void query_compositor_import(VkPhysicalDevice gpu, uint32_t &major, uint32_t &minor) {
	auto drm = structure<VkPhysicalDeviceDrmPropertiesEXT>(
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT);
	auto properties =
		structure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
	properties.pNext = &drm;
	vkGetPhysicalDeviceProperties2(gpu, &properties);
	if (!drm.hasRender)
		throw std::runtime_error("Godot GPU has no DRM render-node identity");
	major = drm.renderMajor;
	minor = drm.renderMinor;
	auto external = structure<VkPhysicalDeviceExternalImageFormatInfo>(
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
	external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	auto modifier = structure<VkPhysicalDeviceImageDrmFormatModifierInfoEXT>(
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT);
	modifier.pNext = &external;
	modifier.drmFormatModifier = 0;
	modifier.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	auto info = structure<VkPhysicalDeviceImageFormatInfo2>(
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2);
	info.pNext = &modifier;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.type = VK_IMAGE_TYPE_2D;
	info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	auto memory = structure<VkExternalImageFormatProperties>(
		VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
	auto output = structure<VkImageFormatProperties2>(VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2);
	output.pNext = &memory;
	check(vkGetPhysicalDeviceImageFormatProperties2(gpu, &info, &output),
		  "query compositor import format");
	if (!(memory.externalMemoryProperties.externalMemoryFeatures &
		  VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
		throw std::runtime_error("LINEAR RGBA8 transfer import not supported on Godot GPU");
}
std::unique_ptr<ImportedImage> import_image(const Frame &f, int &fd, VkPhysicalDevice gpu,
											VkDevice device) {
	if (f.version == 3) {
		uint32_t major, minor;
		query_compositor_import(gpu, major, minor);
		if (major != f.drm_major || minor != f.drm_minor)
			throw std::runtime_error("Compositor DRM device differs from Godot");
	} else if (gpu_uuid(gpu) != f.device_uuid || gpu_uuid(gpu, true) != f.driver_uuid)
		throw std::runtime_error("Producer GPU/driver UUID differs from Godot");
	struct stat info{};
	if (fstat(fd, &info) || info.st_size <= 0 || uint64_t(info.st_size) > 64 * 1024 * 1024 ||
		f.offset + f.stride * f.height > uint64_t(info.st_size))
		throw std::runtime_error("DMA-BUF size does not cover the declared layout");
	auto source = std::make_unique<ImportedImage>(device);
	VkSubresourceLayout plane{};
	plane.offset = f.offset;
	plane.rowPitch = f.stride;
	auto external = structure<VkExternalMemoryImageCreateInfo>(
		VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	auto modifier = structure<VkImageDrmFormatModifierExplicitCreateInfoEXT>(
		VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);
	modifier.pNext = &external;
	modifier.drmFormatModifier = f.modifier;
	modifier.drmFormatModifierPlaneCount = 1;
	modifier.pPlaneLayouts = &plane;
	auto image = image_info(f.width, f.height);
	image.pNext = &modifier;
	if (f.version == 3)
		image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	check(vkCreateImage(device, &image, nullptr, &source->image), "create imported image");
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(device, source->image, &requirements);
	if (requirements.size > uint64_t(info.st_size) ||
		(f.version != 3 && requirements.size != uint64_t(info.st_size)))
		throw std::runtime_error("Export/import allocation requirements differ");
	auto properties =
		structure<VkMemoryFdPropertiesKHR>(VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR);
	auto get_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
		vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
	if (!get_properties)
		throw std::runtime_error("Memory FD extension entrypoint missing");
	check(get_properties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &properties),
		  "query memory FD types");
	auto import = structure<VkImportMemoryFdInfoKHR>(VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
	import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	import.fd = fd;
	auto dedicated =
		structure<VkMemoryDedicatedAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
	dedicated.image = source->image;
	dedicated.pNext = &import;
	auto allocation = structure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
	allocation.pNext = &dedicated;
	allocation.allocationSize = uint64_t(info.st_size);
	allocation.memoryTypeIndex =
		memory_type(gpu, requirements.memoryTypeBits & properties.memoryTypeBits);
	check(vkAllocateMemory(device, &allocation, nullptr, &source->memory), "import DMA-BUF memory");
	fd = -1; // successful Vulkan import consumed our owned FD
	check(vkBindImageMemory(device, source->image, source->memory, 0), "bind imported memory");
	return source;
}
} // namespace

BridgeDMABufReceiver::Slot::~Slot() {
	if (fd >= 0)
		close(fd);
}
void BridgeDMABufReceiver::_bind_methods() {
	ClassDB::bind_method(D_METHOD("configure_compositor", "physical"),
						 &BridgeDMABufReceiver::configure_compositor);
	ClassDB::bind_method(D_METHOD("start", "path"), &BridgeDMABufReceiver::start);
	ClassDB::bind_method(D_METHOD("stop"), &BridgeDMABufReceiver::stop);
	ClassDB::bind_method(D_METHOD("submit_next", "rd", "physical", "logical"),
						 &BridgeDMABufReceiver::submit_next);
	ClassDB::bind_method(D_METHOD("poll_copy", "rd"), &BridgeDMABufReceiver::poll_copy);
	ClassDB::bind_method(D_METHOD("discard_copy", "rd"), &BridgeDMABufReceiver::discard_copy);
	ClassDB::bind_method(D_METHOD("has_frame"), &BridgeDMABufReceiver::has_frame);
	ClassDB::bind_method(D_METHOD("get_status"), &BridgeDMABufReceiver::get_status);
	ClassDB::bind_method(D_METHOD("copy_next", "rd", "physical", "logical"),
						 &BridgeDMABufReceiver::copy_next);
}
Dictionary BridgeDMABufReceiver::configure_compositor(uint64_t physical) {
	Dictionary result;
	try {
		if (worker.joinable() || !physical)
			throw std::runtime_error("Configure before receiver start on render thread");
		uint32_t major, minor;
		query_compositor_import(reinterpret_cast<VkPhysicalDevice>(physical), major, minor);
		gpu_put32(hello.data(), GPU_HELLO_MAGIC);
		gpu_put32(hello.data() + 4, 3);
		gpu_put32(hello.data() + 8, GPU_HELLO_BYTES);
		gpu_put32(hello.data() + 12, GPU_FORMAT);
		gpu_put64(hello.data() + 16, 0);
		gpu_put32(hello.data() + 24, major);
		gpu_put32(hello.data() + 28, minor);
		compositor_mode = true;
		result["drm_major"] = major;
		result["drm_minor"] = minor;
	} catch (const std::exception &error) {
		result["error"] = String(error.what());
	}
	return result;
}
bool BridgeDMABufReceiver::start(const String &value) {
	stop();
	std::string path = value.utf8().get_data();
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	struct stat info{};
	std::string parent = path.substr(0, path.rfind('/'));
	if (path.empty() || path[0] != '/' || path.size() >= sizeof(addr.sun_path) ||
		lstat(parent.c_str(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
		(info.st_mode & 0777) != 0700) {
		std::lock_guard<std::mutex> lock(mutex);
		status = "Invalid private socket path";
		return false;
	}
	FD server{socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
	std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
	if (server.value < 0 || bind(server.value, reinterpret_cast<sockaddr *>(&addr), sizeof(addr))) {
		std::lock_guard<std::mutex> lock(mutex);
		status = "Cannot bind (existing paths are not replaced)";
		return false;
	}
	if (chmod(path.c_str(), 0600) || listen(server.value, 1)) {
		unlink(path.c_str());
		return false;
	}
	cancelled = false;
	{
		std::lock_guard<std::mutex> lock(mutex);
		status = "Listening";
	}
	try {
		worker = std::thread(&BridgeDMABufReceiver::receive, this, server.value, path);
	} catch (...) {
		unlink(path.c_str());
		return false;
	}
	server.value = -1;
	return true;
}
void BridgeDMABufReceiver::receive(int server_fd, const std::string &path) {
	FD server{server_fd}, peer;
	try {
		while (readable(server.value, cancelled)) {
			peer.value = accept4(server.value, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
			if (peer.value >= 0)
				break;
			if (errno != EAGAIN && errno != EINTR)
				throw std::runtime_error("Accept failed");
		}
		if (peer.value < 0)
			throw std::runtime_error("Cancelled");
		if (compositor_mode &&
			send(peer.value, hello.data(), hello.size(), MSG_NOSIGNAL) != ssize_t(hello.size()))
			throw std::runtime_error("Compositor handshake failed");
		for (uint64_t frame_number = 0; !cancelled && (compositor_mode || frame_number < 32);) {
			if (!readable(peer.value, cancelled))
				break;
			std::array<uint8_t, GPU_FRAME_BYTES> packet{};
			alignas(cmsghdr) char control[CMSG_SPACE(8 * sizeof(int))]{};
			iovec iov{packet.data(), packet.size()};
			msghdr message{};
			message.msg_iov = &iov;
			message.msg_iovlen = 1;
			message.msg_control = control;
			message.msg_controllen = sizeof(control);
			ssize_t size = recvmsg(peer.value, &message, MSG_CMSG_CLOEXEC);
			if (size < 0) {
				if (errno == EAGAIN || errno == EINTR) {
					continue;
				}
				throw std::runtime_error("recvmsg failed");
			}
			std::vector<std::unique_ptr<FD>> fds;
			bool unknown_control = false;
			for (auto c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
				if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
					size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
					auto values = reinterpret_cast<int *>(CMSG_DATA(c));
					for (size_t i = 0; i < count; i++) {
						auto owned = std::make_unique<FD>();
						owned->value = values[i];
						fds.push_back(std::move(owned));
					}
				} else
					unknown_control = true;
			}
			// A zero-byte seqpacket can still deliver SCM_RIGHTS. Own/close those
			// descriptors before treating an empty receive as a disconnect.
			if (size == 0 && fds.empty() && !unknown_control &&
				!(message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
				break;
			if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || unknown_control)
				throw std::runtime_error("Expected exactly one complete metadata packet and FD");
			auto slot = std::make_unique<Slot>();
			slot->frame = decode(packet.data(), size);
			if ((slot->frame.version == 3) != compositor_mode)
				throw std::runtime_error("Frame does not match receiver mode");
			if (slot->frame.frame_id != frame_number)
				throw std::runtime_error("Unexpected frame sequence");
			if (fds.size() != 1 + unsigned(slot->frame.acquire_fd))
				throw std::runtime_error("Unexpected descriptor count");
			if (slot->frame.acquire_fd) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					status = "Waiting for acquire fence";
				}
				wait_acquire(fds[1]->value, cancelled);
			}
			uint32_t version = slot->frame.version;
			slot->fd = fds[0]->value;
			fds[0]->value = -1;
			{
				std::unique_lock<std::mutex> lock(mutex);
				pending = std::move(slot);
				completed = false;
				status = "Frame pending GPU copy";
				condition.wait(lock, [&] { return completed || cancelled.load(); });
				if (cancelled)
					break;
			}
			FD release;
			{
				std::lock_guard<std::mutex> lock(mutex);
				release.value = release_fd;
				release_fd = -1;
			}
			uint8_t ack[20]{};
			put32(ack, version == 1 ? 0x41434b31 : 0x41434b32);
			put32(ack + 4, succeeded ? 0 : 1);
			put64(ack + 8, frame_number);
			put32(ack + 16, release.value >= 0 ? 1 : 0);
			iovec reply{ack, version == 1 ? size_t(16) : sizeof(ack)};
			alignas(cmsghdr) char rights[CMSG_SPACE(sizeof(int))]{};
			msghdr response{};
			response.msg_iov = &reply;
			response.msg_iovlen = 1;
			if (version >= 2 && release.value >= 0) {
				response.msg_control = rights;
				response.msg_controllen = sizeof(rights);
				auto control = CMSG_FIRSTHDR(&response);
				control->cmsg_level = SOL_SOCKET;
				control->cmsg_type = SCM_RIGHTS;
				control->cmsg_len = CMSG_LEN(sizeof(int));
				std::memcpy(CMSG_DATA(control), &release.value, sizeof(int));
			}
			if (sendmsg(peer.value, &response, MSG_NOSIGNAL) != ssize_t(reply.iov_len))
				throw std::runtime_error("Producer disconnected after copy");
			if (!succeeded)
				break;
			++frame_number;
		}
	} catch (const std::exception &error) {
		std::lock_guard<std::mutex> lock(mutex);
		status = error.what();
	}
	unlink(path.c_str());
}

Dictionary BridgeDMABufReceiver::copy_next(Object *rd, uint64_t physical, uint64_t logical) {
	Ref<BridgeDMABufReceiver> keep_alive(
		this); // source/worker state survives this render-thread call
	std::unique_ptr<Slot> slot;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!pending)
			return {};
		slot = std::move(pending);
	}
	Dictionary result;
	const Frame &f = slot->frame;
	result["frame_id"] = f.frame_id;
	result["width"] = f.width;
	result["height"] = f.height;
	bool success = false;
	try {
		if (f.version == 3)
			throw std::runtime_error("Compositor frames require asynchronous FOREIGN ownership");
		if (!rd || !physical || !logical ||
			!rd->has_method("texture_create_from_external_snapshot"))
			throw std::runtime_error("Engine snapshot copy hook is unavailable");
		auto gpu = reinterpret_cast<VkPhysicalDevice>(physical);
		auto device = reinterpret_cast<VkDevice>(logical);
		auto source = import_image(f, slot->fd, gpu, device);
		RID texture = rd->call("texture_create_from_external_snapshot", uint64_t(source->image),
							   f.width, f.height);
		if (!texture.is_valid())
			throw std::runtime_error("Engine external snapshot GPU copy failed");
		result["texture_rd_rid"] = texture;
		success = true;
		// source image and memory are destroyed here, after the copy fence
		// completed.
	} catch (const std::exception &error) {
		result["error"] = String(error.what());
	}
	complete_copy(success, success ? "GPU copy completed"
								   : std::string(String(result["error"]).utf8().get_data()));
	return result;
}
void BridgeDMABufReceiver::complete_copy(bool success, const std::string &message, int release) {
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (release_fd >= 0)
			close(release_fd);
		if (cancelled && release >= 0) {
			close(release);
			release = -1;
		}
		release_fd = release;
		completed = true;
		succeeded = success;
		status = message;
	}
	condition.notify_all();
}

Dictionary BridgeDMABufReceiver::submit_next(Object *rd, uint64_t physical, uint64_t logical) {
	Ref<BridgeDMABufReceiver> keep_alive(this);
	Dictionary result;
	if (copy_ticket) {
		result["error"] = "Copy already pending";
		return result;
	}
	std::unique_ptr<Slot> slot;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!pending || cancelled)
			return result;
		slot = std::move(pending);
	}
	try {
		if (!rd || !physical || !logical || !rd->has_method("texture_external_snapshot_submit"))
			throw std::runtime_error("Asynchronous engine hook required");
		auto source =
			import_image(slot->frame, slot->fd, reinterpret_cast<VkPhysicalDevice>(physical),
						 reinterpret_cast<VkDevice>(logical));
		copy_ticket = rd->call("texture_external_snapshot_submit", uint64_t(source->image),
							   uint64_t(source->memory), slot->frame.width, slot->frame.height,
							   slot->frame.version == 3);
		if (!copy_ticket)
			throw std::runtime_error("Asynchronous submission rejected");
		source->image = VK_NULL_HANDLE;
		source->memory = VK_NULL_HANDLE; // engine adopted both until GPU
										 // completion, including cancellation
		copying_frame = slot->frame;
		result["pending"] = true;
	} catch (const std::exception &error) {
		result["error"] = String(error.what());
		complete_copy(false, error.what());
	}
	return result;
}

Dictionary BridgeDMABufReceiver::poll_copy(Object *rd) {
	Ref<BridgeDMABufReceiver> keep_alive(this);
	if (!copy_ticket)
		return {};
	Dictionary result = rd->call("texture_external_snapshot_poll", copy_ticket);
	if (result.has("pending"))
		return result;
	copy_ticket = 0;
	if (result.has("texture_rd_rid")) {
		int release = int(result["release_fd"]);
		result.erase("release_fd"); // raw FD never exposed to GDScript
		result["frame_id"] = copying_frame.frame_id;
		result["width"] = copying_frame.width;
		result["height"] = copying_frame.height;
		complete_copy(true, "GPU copy completed", release);
	} else {
		complete_copy(false, std::string(String(result["error"]).utf8().get_data()));
	}
	return result;
}

void BridgeDMABufReceiver::discard_copy(Object *rd) {
	if (copy_ticket)
		rd->call("texture_external_snapshot_discard", copy_ticket);
	copy_ticket = 0; // engine retirement owns in-flight source, fence and destination
}

bool BridgeDMABufReceiver::has_frame() {
	std::lock_guard<std::mutex> lock(mutex);
	return bool(pending);
}
String BridgeDMABufReceiver::get_status() {
	std::lock_guard<std::mutex> lock(mutex);
	return String(status.c_str());
}
void BridgeDMABufReceiver::stop() {
	cancelled = true;
	condition.notify_all();
	if (worker.joinable())
		worker.join();
	std::lock_guard<std::mutex> lock(mutex);
	pending.reset();
	if (release_fd >= 0)
		close(release_fd);
	release_fd = -1;
}
BridgeDMABufReceiver::~BridgeDMABufReceiver() { stop(); }
