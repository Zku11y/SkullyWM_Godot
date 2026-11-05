#ifndef DWL_GPU_PROTOCOL_H
#define DWL_GPU_PROTOCOL_H
#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>
/* v3: a compositor-owned, foreign-API, one-plane LINEAR ABGR8888 image.
 * Exact layout is shared with the diagnostic C++ decoder. Integers big endian. */
#define GPU_FRAME_BYTES 112u
#define GPU_HELLO_BYTES 32u
#define GPU_HELLO_MAGIC 0x44474831u
#define GPU_FRAME_MAGIC 0x444d4231u
#define GPU_ACK_MAGIC 0x41434b32u
#define GPU_FORMAT 0x34324241u
#define GPU_MAX_DIM 2048u
#define GPU_MAX_BYTES (64u * 1024u * 1024u)
static inline void
gpu_put32(unsigned char *p, uint32_t value)
{
	value = htonl(value);
	memcpy(p, &value, 4);
}
static inline uint32_t
gpu_get32(const unsigned char *p)
{
	uint32_t value;
	memcpy(&value, p, 4);
	return ntohl(value);
}
static inline void
gpu_put64(unsigned char *p, uint64_t value)
{
	gpu_put32(p, (uint32_t)(value >> 32));
	gpu_put32(p + 4, (uint32_t)value);
}
static inline uint64_t
gpu_get64(const unsigned char *p)
{
	return ((uint64_t)gpu_get32(p) << 32) | gpu_get32(p + 4);
}
#endif
