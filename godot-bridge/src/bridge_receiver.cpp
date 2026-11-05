#include "bridge_receiver.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace godot;
using Clock = std::chrono::steady_clock;

namespace {
constexpr uint32_t MAX_DIM = 8192;
constexpr uint64_t MAX_BYTES = 64 * 1024 * 1024;
struct OwnedFD {
	int fd;
	~OwnedFD() { if (fd >= 0) close(fd); }
};

// Poll in short intervals so stop()/destruction can join even with a stalled peer.
bool readable(int fd, std::atomic<bool> &cancelled, Clock::time_point deadline) {
	while (!cancelled && Clock::now() < deadline) {
		pollfd p{fd, POLLIN, 0};
		int result = poll(&p, 1, 50);
		if (result > 0) return !(p.revents & (POLLERR | POLLNVAL));
		if (result < 0 && errno != EINTR) return false;
	}
	return false;
}

bool read_exact(int fd, void *destination, size_t size, std::atomic<bool> &cancelled,
		Clock::time_point deadline) {
	auto *bytes = static_cast<uint8_t *>(destination);
	size_t offset = 0;
	while (offset < size) {
		if (!readable(fd, cancelled, deadline)) return false;
		ssize_t n = recv(fd, bytes + offset, size - offset, 0);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
		if (n <= 0) return false;
		offset += static_cast<size_t>(n);
	}
	return true;
}
}

void BridgeReceiver::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_listening", "path"), &BridgeReceiver::start_listening);
	ClassDB::bind_method(D_METHOD("start_live", "path"), &BridgeReceiver::start_live);
	ClassDB::bind_method(D_METHOD("get_stats"), &BridgeReceiver::get_stats);
	ClassDB::bind_method(D_METHOD("stop"), &BridgeReceiver::stop);
	ClassDB::bind_method(D_METHOD("get_status"), &BridgeReceiver::get_status);
	ClassDB::bind_method(D_METHOD("take_frame"), &BridgeReceiver::take_frame);
}

void BridgeReceiver::set_status(const std::string &value) {
	std::lock_guard<std::mutex> lock(mutex);
	status = value;
}

bool BridgeReceiver::start_listening(const String &value) { return start(value, false); }
bool BridgeReceiver::start_live(const String &value) { return start(value, true); }

bool BridgeReceiver::start(const String &value, bool live) {
	stop();
	std::string path = value.utf8().get_data();
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	if (path.empty() || path[0] != '/' || path.size() >= sizeof(address.sun_path)) {
		set_status("Socket path must be absolute and shorter than 108 bytes");
		return false;
	}
	struct stat info{};
	std::string parent = path.substr(0, path.rfind('/'));
	if (lstat(parent.c_str(), &info) || !S_ISDIR(info.st_mode)
			|| info.st_uid != getuid() || (info.st_mode & 0777) != 0700) {
		set_status("Socket parent must be your own directory with mode 0700");
		return false;
	}
	OwnedFD server{socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
	memcpy(address.sun_path, path.c_str(), path.size() + 1);
	if (server.fd < 0 || bind(server.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
		set_status("Cannot bind socket (an existing path is never replaced)");
		return false;
	}
	if (chmod(path.c_str(), 0600) || listen(server.fd, 1)) {
		unlink(path.c_str());
		set_status("Cannot prepare listening socket");
		return false;
	}
	received_count = replaced_count = 0;
	cancelled = false;
	set_status("Listening: " + path);
	try {
		worker = std::thread(&BridgeReceiver::receive_frames, this, server.fd, path, live);
	} catch (...) {
		unlink(path.c_str());
		set_status("Cannot start receiver thread");
		return false;
	}
	server.fd = -1; // worker now owns the socket
	return true;
}

void BridgeReceiver::receive_frames(int server_fd, const std::string &path, bool live) {
	OwnedFD server{server_fd};
	do {
		OwnedFD peer{-1};
		try {
			while (!cancelled) {
				if (!readable(server.fd, cancelled, Clock::now() + std::chrono::milliseconds(100))) continue;
				peer.fd = accept4(server.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
				if (peer.fd >= 0) break;
				if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
					throw std::runtime_error("Accept failed");
			}
			if (cancelled) { unlink(path.c_str()); return; }
			set_status("Receiving one snapshot");
			auto deadline = Clock::now() + std::chrono::seconds(5);
			uint32_t header[8];
			if (!read_exact(peer.fd, header, sizeof(header), cancelled, deadline))
				throw std::runtime_error("Incomplete header, timeout, or cancellation");
			for (auto &field : header) field = ntohl(field);
			const uint32_t w = header[3], h = header[4];
			const uint64_t size = uint64_t(w) * h * 3;
			if (header[0] != 0x44574c42 || header[1] != 1 || header[2] != 32
					|| !w || !h || w > MAX_DIM || h > MAX_DIM || header[5] != 1
					|| uint64_t(header[6]) != uint64_t(w) * 3 || header[7] != size || size > MAX_BYTES)
				throw std::runtime_error("Invalid or oversized DWLB v1 RGB888 header");
			std::vector<uint8_t> received(static_cast<size_t>(size));
			if (!read_exact(peer.fd, received.data(), received.size(), cancelled, deadline))
				throw std::runtime_error("Incomplete pixels, timeout, or cancellation");
			uint8_t extra;
			if (!readable(peer.fd, cancelled, deadline) || recv(peer.fd, &extra, 1, 0) != 0)
				throw std::runtime_error("Expected EOF after pixels");
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (!pixels.empty()) ++replaced_count;
				++received_count;
				pixels = std::move(received);
				width = w; height = h;
				status = "Frame ready";
			}
		} catch (const std::exception &error) {
			set_status(error.what());
		}
	} while (live && !cancelled);
	unlink(path.c_str());
}

Dictionary BridgeReceiver::get_stats() {
	std::lock_guard<std::mutex> lock(mutex);
	Dictionary result;
	result["received"] = received_count;
	result["replaced"] = replaced_count;
	result["pending_bytes"] = static_cast<int64_t>(pixels.size());
	return result;
}

Dictionary BridgeReceiver::take_frame() {
	std::vector<uint8_t> received;
	uint32_t w, h;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (pixels.empty()) return {};
		received = std::move(pixels);
		w = width; h = height;
	}
	PackedByteArray data;
	data.resize(received.size());
	memcpy(data.ptrw(), received.data(), received.size());
	Dictionary frame;
	frame["width"] = w; frame["height"] = h; frame["pixels"] = data;
	return frame;
}

String BridgeReceiver::get_status() {
	std::lock_guard<std::mutex> lock(mutex);
	return String(status.c_str());
}

void BridgeReceiver::stop() {
	cancelled = true;
	if (worker.joinable()) worker.join();
	std::lock_guard<std::mutex> lock(mutex);
	pixels.clear();
	status = "Disconnected";
}

BridgeReceiver::~BridgeReceiver() { stop(); }
