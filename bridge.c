/* One bounded RGB snapshot, sent without blocking the compositor. */
#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "bridge.h"

static struct {
	int fd;
	struct wl_event_source *writable, *timer;
	unsigned char *rgb;
	uint32_t header[8];
	size_t offset, size;
} transfer = {.fd = -1};

int
bridge_busy(void)
{
	return transfer.fd != -1;
}

void
bridge_finish(void)
{
	if (transfer.writable)
		wl_event_source_remove(transfer.writable);
	if (transfer.timer)
		wl_event_source_remove(transfer.timer);
	if (transfer.fd != -1)
		close(transfer.fd);
	free(transfer.rgb);
	memset(&transfer, 0, sizeof(transfer));
	transfer.fd = -1;
}

static int
expired(void *data)
{
	wlr_log(WLR_ERROR, "[bridge] SEND timed out; snapshot discarded");
	bridge_finish();
	return 0;
}

static int
writeframe(int fd, uint32_t mask, void *data)
{
	size_t budget = 64 * 1024, count, header_size = sizeof(transfer.header);
	const unsigned char *bytes;
	ssize_t n;
	int error = 0;
	socklen_t len = sizeof(error);

	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error ||
			(mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)))
		goto failed;
	while (budget && transfer.offset < header_size + transfer.size) {
		if (transfer.offset < header_size) {
			bytes = (const unsigned char *)transfer.header + transfer.offset;
			count = header_size - transfer.offset;
		} else {
			bytes = transfer.rgb + transfer.offset - header_size;
			count = header_size + transfer.size - transfer.offset;
		}
		if (count > budget)
			count = budget;
		n = send(fd, bytes, count, MSG_NOSIGNAL);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			return 0;
		if (n <= 0)
			goto failed;
		transfer.offset += (size_t)n;
		budget -= (size_t)n;
	}
	if (transfer.offset == header_size + transfer.size) {
		wlr_log(WLR_INFO, "[bridge] SEND snapshot handed to socket (%zu bytes)", transfer.size);
		bridge_finish();
	}
	return 0;
failed:
	wlr_log(WLR_ERROR, "[bridge] SEND disconnected/failed; snapshot discarded");
	bridge_finish();
	return 0;
}

int
bridge_send(struct wl_event_loop *loop, const char *path,
		unsigned char *rgb, uint32_t width, uint32_t height)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	int fd;
	uint32_t fields[8];
	size_t i, size;

	if (bridge_busy() || !path || path[0] != '/' || strlen(path) >= sizeof(addr.sun_path)
			|| !rgb || !width || !height || width > BRIDGE_MAX_DIM || height > BRIDGE_MAX_DIM)
		return 0;
	size = (size_t)width * height * 3;
	if (size > BRIDGE_MAX_BYTES)
		return 0;
	memcpy(addr.sun_path, path, strlen(path) + 1);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
		close(fd);
		wlr_log(WLR_ERROR, "[bridge] SEND receiver unavailable; request retained");
		return 0;
	}
	transfer.fd = fd;
	transfer.size = size;
	/* All fields are network-order u32: DWLB, version, header bytes,
	 * width, height, RGB888=1, packed row stride, payload bytes. */
	fields[0] = 0x44574c42; fields[1] = 1; fields[2] = 32;
	fields[3] = width; fields[4] = height; fields[5] = 1;
	fields[6] = width * 3; fields[7] = (uint32_t)size;
	for (i = 0; i < 8; i++)
		transfer.header[i] = htonl(fields[i]);
	transfer.writable = wl_event_loop_add_fd(loop, fd, WL_EVENT_WRITABLE, writeframe, NULL);
	transfer.timer = wl_event_loop_add_timer(loop, expired, NULL);
	if (!transfer.writable || !transfer.timer || wl_event_source_timer_update(transfer.timer, 2000) < 0) {
		bridge_finish();
		return 0;
	}
	transfer.rgb = rgb;
	return 1;
}
