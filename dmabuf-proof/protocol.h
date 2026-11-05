#pragma once
#include "../gpu_protocol.h"
#include <arpa/inet.h>
#include <array>
#include <cstdint>
#include <endian.h>
#include <stdexcept>

namespace proof {
// SOCK_SEQPACKET, one small metadata packet and one SCM_RIGHTS FD per frame.
// All integers network order. No native structs or Vulkan handles on the wire.
constexpr size_t HEADER_BYTES = 96;
constexpr size_t ASYNC_HEADER_BYTES = 104;
constexpr uint32_t MAGIC = 0x444d4231;	// DMB1
constexpr uint32_t FOURCC = 0x34324241; // DRM ABGR8888: RGBA bytes, little endian
constexpr uint32_t USAGE = 0x0f;		// Vulkan transfer src/dst, sampled, storage
struct Frame {
	uint32_t version = 1;
	bool acquire_fd = false;
	uint32_t drm_major = 0, drm_minor = 0;
	uint32_t width = 512, height = 256;
	uint64_t offset = 0, stride = 0, modifier = 0, frame_id = 0;
	std::array<uint8_t, 16> device_uuid{}, driver_uuid{};
};
inline void put32(uint8_t *p, uint32_t v) {
	v = htonl(v);
	__builtin_memcpy(p, &v, 4);
}
inline void put64(uint8_t *p, uint64_t v) {
	v = htobe64(v);
	__builtin_memcpy(p, &v, 8);
}
inline uint32_t get32(const uint8_t *p) {
	uint32_t v;
	__builtin_memcpy(&v, p, 4);
	return ntohl(v);
}
inline uint64_t get64(const uint8_t *p) {
	uint64_t v;
	__builtin_memcpy(&v, p, 8);
	return be64toh(v);
}
inline std::array<uint8_t, HEADER_BYTES> encode(const Frame &f) {
	std::array<uint8_t, HEADER_BYTES> p{};
	put32(p.data(), MAGIC);
	put32(p.data() + 4, 1);
	put32(p.data() + 8, HEADER_BYTES);
	put32(p.data() + 12, f.width);
	put32(p.data() + 16, f.height);
	put32(p.data() + 20, FOURCC);
	put32(p.data() + 24, 1);
	put32(p.data() + 28, USAGE);
	put64(p.data() + 32, f.offset);
	put64(p.data() + 40, f.stride);
	put64(p.data() + 48, f.modifier);
	put64(p.data() + 56, f.frame_id);
	__builtin_memcpy(p.data() + 64, f.device_uuid.data(), 16);
	__builtin_memcpy(p.data() + 80, f.driver_uuid.data(), 16);
	return p;
}
inline std::array<uint8_t, ASYNC_HEADER_BYTES> encode_async(const Frame &f) {
	std::array<uint8_t, ASYNC_HEADER_BYTES> p{};
	auto base = encode(f);
	__builtin_memcpy(p.data(), base.data(), base.size());
	put32(p.data() + 4, 2);
	put32(p.data() + 8, ASYNC_HEADER_BYTES);
	put32(p.data() + 96, f.acquire_fd ? 1 : 0);
	return p;
}
inline Frame decode(const uint8_t *p, size_t size) {
	if ((size != HEADER_BYTES && size != ASYNC_HEADER_BYTES && size != GPU_FRAME_BYTES) ||
		get32(p) != MAGIC ||
		!((get32(p + 4) == 1 && size == HEADER_BYTES) ||
		  (get32(p + 4) == 2 && size == ASYNC_HEADER_BYTES) ||
		  (get32(p + 4) == 3 && size == GPU_FRAME_BYTES)) ||
		get32(p + 8) != size || get32(p + 20) != FOURCC || get32(p + 24) != 1 ||
		get32(p + 28) != (get32(p + 4) == 3 ? 1u : USAGE))
		throw std::runtime_error("Invalid DMB1 header, format, plane count or usage");
	Frame f;
	f.version = get32(p + 4);
	if (f.version >= 2) {
		if (get32(p + 96) > 1 || get32(p + 100))
			throw std::runtime_error("Invalid acquire-fence flags");
		f.acquire_fd = get32(p + 96) == 1;
	}
	if (f.version == 3) {
		f.drm_major = get32(p + 104);
		f.drm_minor = get32(p + 108);
		if (!f.acquire_fd)
			throw std::runtime_error("Compositor frame requires an acquire FD");
	}
	f.width = get32(p + 12);
	f.height = get32(p + 16);
	f.offset = get64(p + 32);
	f.stride = get64(p + 40);
	f.modifier = get64(p + 48);
	f.frame_id = get64(p + 56);
	__builtin_memcpy(f.device_uuid.data(), p + 64, 16);
	__builtin_memcpy(f.driver_uuid.data(), p + 80, 16);
	if (!f.width || !f.height || f.width > 2048 || f.height > 2048 || f.modifier != 0 ||
		f.stride < uint64_t(f.width) * 4 || f.stride > 64 * 1024 || f.offset > 64 * 1024 * 1024 ||
		f.offset + f.stride * f.height > 64 * 1024 * 1024)
		throw std::runtime_error("Unsupported or oversized single-plane linear layout");
	return f;
}
} // namespace proof
