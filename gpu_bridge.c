/* One compositor-owned DMA-BUF export slot. All waits use the Wayland event loop. */
#include "gpu_bridge.h"
#include "gpu_protocol.h"
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/sync_file.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

enum ipc_state { IPC_OFF, IPC_HELLO, IPC_IDLE, IPC_SEND, IPC_ACK, IPC_RELEASE };
static struct {
	struct wl_event_loop *loop;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wl_event_source *timer, *socket_event, *ready_event, *release_event;
	struct wl_listener unmap, destroy;
	struct wlr_surface *surface; /* guarded by unmap/destroy listeners */
	struct wlr_buffer *source, *target;
	struct wlr_drm_syncobj_timeline *timeline;
	struct wlr_dmabuf_attributes attrs; /* borrowed from our owned target */
	const char *path, *app_id, *reference;
	int socket_fd, ready_fd, send_fd, release_fd;
	enum ipc_state ipc;
	int dirty, produced, abandoned;
	uint32_t device_major, device_minor;
	uint64_t point, frame, producing_frame, deadline, next_connect;
	uint64_t next_frame_ns, interval_ns;
	int tick_ms;
	unsigned char header[GPU_FRAME_BYTES];
} gpu = {.socket_fd = -1, .ready_fd = -1, .send_fd = -1, .release_fd = -1};

static uint64_t
now_ns(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static uint64_t
now_ms(void)
{
	return now_ns() / 1000000u;
}

static void
remove_event(struct wl_event_source **event)
{
	if (*event)
		wl_event_source_remove(*event);
	*event = NULL;
}

static void
close_fd(int *fd)
{
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
}

static void
release_target(void)
{
	if (gpu.target)
		wlr_buffer_drop(gpu.target);
	gpu.target = NULL;
}

static void
disconnect_peer(const char *reason)
{
	wlr_log(WLR_INFO, "[gpu-bridge] DISCONNECT %s; old export will not be reused", reason);
	remove_event(&gpu.socket_event);
	remove_event(&gpu.release_event);
	close_fd(&gpu.socket_fd);
	close_fd(&gpu.send_fd);
	close_fd(&gpu.release_fd);
	gpu.ipc = IPC_OFF;
	gpu.abandoned = 1;
	gpu.next_connect = now_ms() + 500;
	/* Receiver may still hold/read its FD: drop our storage ownership rather
	 * than rewriting it. Our own outstanding render must finish first. */
	if (!gpu.source)
		release_target();
}

static void
surface_gone(struct wl_listener *listener, void *data)
{
	(void)listener;
	(void)data;
	wl_list_remove(&gpu.unmap.link);
	wl_list_remove(&gpu.destroy.link);
	gpu.surface = NULL;
	gpu.dirty = 0;
	wlr_log(WLR_INFO, "[gpu-bridge] WINDOW gone; last Godot image may remain visible");
}

/* Test-only reference readback of the exact export, never enabled by the
 * interactive runner. The normal bridge never maps or reads back pixels. */
static void
save_reference(void)
{
	struct wlr_texture *texture;
	unsigned char *pixels;
	size_t bytes;
	char filename[512];
	FILE *file;
	int ok;
	struct wlr_texture_read_pixels_options options = {0};
	if (!gpu.reference || !gpu.target)
		return;
	if (snprintf(filename, sizeof(filename), "%s/export-%" PRIu64 ".rgba", gpu.reference,
				 gpu.producing_frame) >= (int)sizeof(filename))
		return;
	bytes = (size_t)gpu.target->width * (size_t)gpu.target->height * 4;
	pixels = malloc(bytes);
	texture = wlr_texture_from_buffer(gpu.renderer, gpu.target);
	if (!pixels || !texture) {
		free(pixels);
		if (texture)
			wlr_texture_destroy(texture);
		return;
	}
	options.data = pixels;
	options.format = DRM_FORMAT_ABGR8888;
	options.stride = (uint32_t)gpu.target->width * 4u;
	ok = wlr_texture_read_pixels(texture, &options);
	if (ok && (file = fopen(filename, "wb"))) {
		ok = fwrite(pixels, 1, bytes, file) == bytes;
		if (fclose(file))
			ok = 0;
	} else
		ok = 0;
	wlr_texture_destroy(texture);
	free(pixels);
	wlr_log(WLR_INFO, "[gpu-bridge] REFERENCE frame=%" PRIu64 " saved=%d", gpu.producing_frame, ok);
}

static int
produced(int fd, uint32_t mask, void *data)
{
	struct sync_file_info info = {0};
	(void)data;
	if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) || info.status <= 0) {
		/* Keep source/target pinned on uncertain completion. Do not pretend a
		 * failed fence authorizes reuse. Normal engine/session teardown follows. */
		remove_event(&gpu.ready_event);
		disconnect_peer("producer fence failed");
		wlr_log(WLR_ERROR, "[gpu-bridge] GPU failure: export disabled until restart");
		return 0;
	}
	(void)mask;
	remove_event(&gpu.ready_event);
	close_fd(&gpu.ready_fd);
	gpu.produced = 1;
	if (gpu.source)
		wlr_buffer_unlock(gpu.source);
	gpu.source = NULL;
	if (gpu.abandoned)
		release_target();
	else
		save_reference();
	return 0;
}

static int
released(int fd, uint32_t mask, void *data)
{
	struct sync_file_info info = {0};
	(void)data;
	(void)mask;
	if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) || info.status <= 0) {
		disconnect_peer("release fence failed");
		return 0;
	}
	remove_event(&gpu.release_event);
	close_fd(&gpu.release_fd);
	gpu.ipc = IPC_IDLE;
	++gpu.frame;
	return 0;
}

static int
socket_read(int fd, uint32_t mask, void *data)
{
	unsigned char packet[GPU_HELLO_BYTES];
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(4 * sizeof(int))];
	} control;
	struct iovec iov = {.iov_base = packet, .iov_len = sizeof(packet)};
	struct msghdr message = {.msg_iov = &iov,
							 .msg_iovlen = 1,
							 .msg_control = control.bytes,
							 .msg_controllen = sizeof(control.bytes)};
	struct cmsghdr *cmsg;
	int received = -1, count = 0, unknown = 0;
	ssize_t size;
	size_t i, n;
	int *fds;
	(void)data;
	if (!(mask & WL_EVENT_READABLE)) {
		disconnect_peer("socket hangup/error");
		return 0;
	}
	size = recvmsg(fd, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
	if (size < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return 0;
		goto failed;
	}
	for (cmsg = CMSG_FIRSTHDR(&message); cmsg; cmsg = CMSG_NXTHDR(&message, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
			unknown = 1;
			continue;
		}
		n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		fds = (int *)CMSG_DATA(cmsg);
		for (i = 0; i < n; ++i) {
			if (count++ == 0)
				received = fds[i];
			else
				close(fds[i]);
		}
	}
	if (size <= 0 || unknown || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
		goto failed;
	if (gpu.ipc == IPC_HELLO) {
		if (count || size != GPU_HELLO_BYTES || gpu_get32(packet) != GPU_HELLO_MAGIC ||
			gpu_get32(packet + 4) != 3 || gpu_get32(packet + 8) != GPU_HELLO_BYTES ||
			gpu_get32(packet + 12) != GPU_FORMAT ||
			gpu_get64(packet + 16) != DRM_FORMAT_MOD_LINEAR ||
			gpu_get32(packet + 24) != gpu.device_major ||
			gpu_get32(packet + 28) != gpu.device_minor)
			goto failed;
		gpu.ipc = IPC_IDLE;
		gpu.frame = 0;
		gpu.dirty = gpu.surface != NULL;
		wlr_log(WLR_INFO, "[gpu-bridge] CONNECT matched DRM device %u:%u", gpu.device_major,
				gpu.device_minor);
		return 0;
	}
	if (gpu.ipc != IPC_ACK || size != 20 || gpu_get32(packet) != GPU_ACK_MAGIC ||
		gpu_get32(packet + 4) != 0 || gpu_get64(packet + 8) != gpu.frame ||
		gpu_get32(packet + 16) > 1 || gpu_get32(packet + 16) != (uint32_t)count)
		goto failed;
	wlr_log(WLR_INFO, "[gpu-bridge] ACK frame=%" PRIu64 " release-fds=%d", gpu.frame, count);
	if (received >= 0) {
		struct sync_file_info info = {0};
		if (ioctl(received, SYNC_IOC_FILE_INFO, &info) || info.status < 0)
			goto failed;
		gpu.release_fd = received;
		received = -1;
		gpu.release_event =
			wl_event_loop_add_fd(gpu.loop, gpu.release_fd, WL_EVENT_READABLE, released, NULL);
		if (!gpu.release_event)
			goto failed;
		gpu.ipc = IPC_RELEASE;
	} else {
		gpu.ipc = IPC_IDLE;
		++gpu.frame;
	}
	return 0;
failed:
	if (received >= 0)
		close(received);
	disconnect_peer("invalid reply, mismatch or peer closed");
	return 0;
}

static void
connect_peer(void)
{
	struct sockaddr_un address = {.sun_family = AF_UNIX};
	int result;
	gpu.socket_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (gpu.socket_fd < 0)
		return;
	memcpy(address.sun_path, gpu.path, strlen(gpu.path) + 1);
	result = connect(gpu.socket_fd, (struct sockaddr *)&address, sizeof(address));
	if (result && errno != EINPROGRESS) {
		close_fd(&gpu.socket_fd);
		gpu.next_connect = now_ms() + 500;
		return;
	}
	gpu.socket_event =
		wl_event_loop_add_fd(gpu.loop, gpu.socket_fd, WL_EVENT_READABLE, socket_read, NULL);
	if (!gpu.socket_event) {
		disconnect_peer("event allocation");
		return;
	}
	gpu.ipc = IPC_HELLO;
	gpu.deadline = now_ms() + 5000;
}

static void
capture(void)
{
	struct wlr_surface *surface = gpu.surface;
	struct wlr_texture *texture;
	struct wlr_render_pass *pass;
	struct wlr_linux_drm_syncobj_surface_v1_state *sync;
	uint64_t linear = DRM_FORMAT_MOD_LINEAR;
	struct wlr_drm_format format = {
		.format = DRM_FORMAT_ABGR8888, .len = 1, .capacity = 1, .modifiers = &linear};
	struct wlr_buffer_pass_options options = {0};
	struct wlr_render_texture_options sample = {0};
	struct stat info;
	int width, height;
	uint64_t capture_ns = now_ns();
	/* Keep a fractional-millisecond cadence (60 Hz is 16.667 ms). Skip missed
	 * deadlines rather than queueing catch-up copies after backpressure. */
	if (!gpu.next_frame_ns)
		gpu.next_frame_ns = capture_ns;
	gpu.next_frame_ns +=
		((capture_ns - gpu.next_frame_ns) / gpu.interval_ns + 1) * gpu.interval_ns;
	if (!surface || !surface->mapped || !surface->buffer ||
		!(texture = wlr_surface_get_texture(surface)))
		return;
	width = surface->current.width;
	height = surface->current.height;
	if (width <= 0 || height <= 0 || width > (int)GPU_MAX_DIM || height > (int)GPU_MAX_DIM) {
		gpu.dirty = 0;
		wlr_log(WLR_ERROR, "[gpu-bridge] Unsupported dimensions %dx%d (maximum %u)", width, height,
				GPU_MAX_DIM);
		return;
	}
	if (gpu.target && (gpu.target->width != width || gpu.target->height != height))
		release_target();
	if (!gpu.target) {
		gpu.target = wlr_allocator_create_buffer(gpu.allocator, width, height, &format);
		if (!gpu.target || !wlr_buffer_get_dmabuf(gpu.target, &gpu.attrs) ||
			gpu.attrs.n_planes != 1 || gpu.attrs.width != width || gpu.attrs.height != height ||
			gpu.attrs.format != GPU_FORMAT ||
			gpu.attrs.modifier != linear || gpu.attrs.stride[0] < (uint32_t)width * 4 ||
			gpu.attrs.stride[0] > 65536 || fstat(gpu.attrs.fd[0], &info) || info.st_size <= 0 ||
			info.st_size > GPU_MAX_BYTES ||
			(uint64_t)gpu.attrs.offset[0] + (uint64_t)gpu.attrs.stride[0] * (uint32_t)height >
				(uint64_t)info.st_size) {
			release_target();
			gpu.dirty = 0;
			wlr_log(WLR_ERROR, "[gpu-bridge] No compatible LINEAR ABGR8888 export allocation");
			return;
		}
		wlr_log(WLR_INFO, "[gpu-bridge] ALLOC %dx%d stride=%u", width, height, gpu.attrs.stride[0]);
	}
	/* Retain the applied wrapper (not its diagnostic source fallback). This
	 * prevents mutable SHM texture updates and keeps GPU textures/imports alive. */
	gpu.source = wlr_buffer_lock(&surface->buffer->base);
	gpu.produced = 0;
	gpu.producing_frame = gpu.frame;
	gpu.abandoned = 0;
	options.signal_timeline = gpu.timeline;
	options.signal_point = ++gpu.point;
	pass = wlr_renderer_begin_buffer_pass(gpu.renderer, gpu.target, &options);
	if (!pass) {
		wlr_buffer_unlock(gpu.source);
		gpu.source = NULL;
		disconnect_peer("begin pass");
		return;
	}
	sample.texture = texture;
	sample.dst_box = (struct wlr_box){.width = width, .height = height};
	wlr_surface_get_buffer_source_box(surface, &sample.src_box);
	sample.transform = wlr_output_transform_invert(surface->current.transform);
	sample.filter_mode = WLR_SCALE_FILTER_NEAREST;
	sample.blend_mode = WLR_RENDER_BLEND_MODE_NONE;
	sync = wlr_linux_drm_syncobj_v1_get_surface_state(surface);
	if (sync) {
		sample.wait_timeline = sync->acquire_timeline;
		sample.wait_point = sync->acquire_point;
	}
	wlr_render_pass_add_texture(pass, &sample);
	if (!wlr_render_pass_submit(pass)) {
		/* Uncertain GPU work: pin source/target until renderer/session teardown. */
		disconnect_peer("render submit failed (slot pinned)");
		return;
	}
	gpu.ready_fd = wlr_drm_syncobj_timeline_export_sync_file(gpu.timeline, gpu.point);
	if (gpu.ready_fd < 0 || (gpu.send_fd = fcntl(gpu.ready_fd, F_DUPFD_CLOEXEC, 0)) < 0) {
		disconnect_peer("cannot export producer fence (slot pinned)");
		return;
	}
	gpu.ready_event =
		wl_event_loop_add_fd(gpu.loop, gpu.ready_fd, WL_EVENT_READABLE, produced, NULL);
	if (!gpu.ready_event) {
		disconnect_peer("producer event failed (slot pinned)");
		return;
	}
	memset(gpu.header, 0, sizeof(gpu.header));
	gpu_put32(gpu.header, GPU_FRAME_MAGIC);
	gpu_put32(gpu.header + 4, 3);
	gpu_put32(gpu.header + 8, GPU_FRAME_BYTES);
	gpu_put32(gpu.header + 12, (uint32_t)width);
	gpu_put32(gpu.header + 16, (uint32_t)height);
	gpu_put32(gpu.header + 20, GPU_FORMAT);
	gpu_put32(gpu.header + 24, 1);
	gpu_put32(gpu.header + 28, 1); /* Vulkan TRANSFER_SRC only */
	gpu_put64(gpu.header + 32, gpu.attrs.offset[0]);
	gpu_put64(gpu.header + 40, gpu.attrs.stride[0]);
	gpu_put64(gpu.header + 48, gpu.attrs.modifier);
	gpu_put64(gpu.header + 56, gpu.frame);
	gpu_put32(gpu.header + 96, 1);
	gpu_put32(gpu.header + 104, gpu.device_major);
	gpu_put32(gpu.header + 108, gpu.device_minor);
	gpu.ipc = IPC_SEND;
	gpu.deadline = now_ms() + 10000;
	gpu.dirty = 0;
}

static void
send_frame(void)
{
	int fds[2] = {gpu.attrs.fd[0], gpu.send_fd};
	union {
		struct cmsghdr align;
		char bytes[CMSG_SPACE(sizeof(fds))];
	} control;
	struct iovec iov = {.iov_base = gpu.header, .iov_len = sizeof(gpu.header)};
	struct msghdr message = {.msg_iov = &iov,
							 .msg_iovlen = 1,
							 .msg_control = control.bytes,
							 .msg_controllen = sizeof(control.bytes)};
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
	ssize_t size;
	memset(control.bytes, 0, sizeof(control.bytes));
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(fds));
	memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));
	size = sendmsg(gpu.socket_fd, &message, MSG_NOSIGNAL);
	if (size < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (size != sizeof(gpu.header)) {
		disconnect_peer("send failed");
		return;
	}
	close_fd(&gpu.send_fd);
	gpu.ipc = IPC_ACK;
	wlr_log(WLR_INFO, "[gpu-bridge] SEND frame=%" PRIu64 " size=%ux%u", gpu.frame,
			gpu_get32(gpu.header + 12), gpu_get32(gpu.header + 16));
}

static int
tick(void *data)
{
	uint64_t now = now_ms();
	(void)data;
	if (gpu.ipc == IPC_OFF && !gpu.source && now >= gpu.next_connect)
		connect_peer();
	if (gpu.ipc == IPC_IDLE && !gpu.source && gpu.dirty && now_ns() >= gpu.next_frame_ns)
		capture();
	if (gpu.ipc == IPC_SEND)
		send_frame();
	if (gpu.ipc != IPC_OFF && gpu.ipc != IPC_IDLE && now > gpu.deadline)
		disconnect_peer("timeout");
	wl_event_source_timer_update(gpu.timer, gpu.tick_ms);
	return 0;
}

void
gpu_bridge_commit(struct wlr_surface *surface, const char *app_id)
{
	struct wlr_dmabuf_attributes attrs;
	struct wlr_shm_attributes shm;
	const char *kind = "uploaded-texture";
	if (!gpu.timer || !surface->mapped || strcmp(app_id, gpu.app_id))
		return;
	if (!(surface->current.committed &
		  (WLR_SURFACE_STATE_BUFFER | WLR_SURFACE_STATE_SURFACE_DAMAGE |
		   WLR_SURFACE_STATE_BUFFER_DAMAGE | WLR_SURFACE_STATE_TRANSFORM | WLR_SURFACE_STATE_SCALE |
		   WLR_SURFACE_STATE_VIEWPORT)))
		return;
	if (!gpu.surface) {
		gpu.surface = surface;
		gpu.unmap.notify = surface_gone;
		gpu.destroy.notify = surface_gone;
		wl_signal_add(&surface->events.unmap, &gpu.unmap);
		wl_signal_add(&surface->events.destroy, &gpu.destroy);
		wlr_log(WLR_INFO, "[gpu-bridge] WINDOW selected app-id=%s", app_id);
	}
	if (gpu.surface != surface)
		return;
	if (surface->current.buffer) {
		if (wlr_buffer_get_dmabuf(surface->current.buffer, &attrs))
			kind = "DMA-BUF";
		else if (wlr_buffer_get_shm(surface->current.buffer, &shm))
			kind = "SHM";
	}
	gpu.dirty = 1; /* coalesce to the latest applied state, including a final idle update */
	wlr_log(WLR_DEBUG, "[gpu-bridge] APPLIED seq=%u input=%s", surface->current.seq, kind);
}

void
gpu_bridge_init(struct wl_event_loop *loop, struct wlr_renderer *renderer,
				struct wlr_allocator *allocator)
{
	struct stat info;
	struct sockaddr_un address;
	char parent[sizeof(address.sun_path)], *slash, *end;
	const char *rate;
	long fps = 10;
	int drm_fd;
	gpu.path = getenv("DWL_BRIDGE_GPU_SOCKET");
	gpu.app_id = getenv("DWL_BRIDGE_GPU_APP_ID");
	gpu.reference = getenv("DWL_BRIDGE_GPU_REFERENCE_DIR");
	if (!gpu.path || !gpu.app_id)
		return;
	if (!*gpu.app_id || !strcmp(gpu.app_id, "Godot") ||
		!strcmp(gpu.app_id, "org.godotengine.Godot") || gpu.path[0] != '/' ||
		strlen(gpu.path) >= sizeof(address.sun_path))
		goto invalid;
	memcpy(parent, gpu.path, strlen(gpu.path) + 1);
	slash = strrchr(parent, '/');
	*slash = '\0';
	if (lstat(parent, &info) || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
		(info.st_mode & 0777) != 0700)
		goto invalid;
	rate = getenv("DWL_BRIDGE_FPS");
	if (rate) {
		fps = strtol(rate, &end, 10);
		if (!*rate || *end || fps < 1 || fps > 60)
			goto invalid;
	}
	drm_fd = wlr_renderer_get_drm_fd(renderer);
	if (!renderer->features.timeline || !(allocator->buffer_caps & WLR_BUFFER_CAP_DMABUF) ||
		drm_fd < 0 || fstat(drm_fd, &info) || !S_ISCHR(info.st_mode))
		goto invalid;
	gpu.timeline = wlr_drm_syncobj_timeline_create(drm_fd);
	if (!gpu.timeline)
		goto invalid;
	gpu.device_major = major(info.st_rdev);
	gpu.device_minor = minor(info.st_rdev);
	gpu.loop = loop;
	gpu.renderer = renderer;
	gpu.allocator = allocator;
	gpu.interval_ns = (1000000000u + (uint64_t)fps - 1) / (uint64_t)fps;
	gpu.tick_ms = fps > 30 ? 1 : 10;
	gpu.timer = wl_event_loop_add_timer(loop, tick, NULL);
	if (!gpu.timer)
		goto invalid;
	wl_event_source_timer_update(gpu.timer, 1);
	wlr_log(WLR_INFO, "[gpu-bridge] ENABLED app-id=%s fps=%ld DRM=%u:%u", gpu.app_id, fps,
			gpu.device_major, gpu.device_minor);
	return;
invalid:
	wlr_log(WLR_ERROR, "[gpu-bridge] Cannot enable: private path, selector, timeline-capable "
					   "renderer and DMA-BUF allocator required");
}

void
gpu_bridge_finish(void)
{
	remove_event(&gpu.timer);
	remove_event(&gpu.socket_event);
	remove_event(&gpu.ready_event);
	remove_event(&gpu.release_event);
	close_fd(&gpu.socket_fd);
	close_fd(&gpu.send_fd);
	close_fd(&gpu.release_fd);
	close_fd(&gpu.ready_fd);
	if (gpu.surface) {
		wl_list_remove(&gpu.unmap.link);
		wl_list_remove(&gpu.destroy.link);
		gpu.surface = NULL;
	}
	/* Do not unlock an uncertain in-flight source on compositor teardown.
	 * The renderer/device teardown, then process exit, owns that rare case. */
	if (!gpu.source)
		release_target();
	wlr_drm_syncobj_timeline_unref(gpu.timeline);
	gpu.timeline = NULL;
}
