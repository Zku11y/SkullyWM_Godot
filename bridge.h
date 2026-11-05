#ifndef DWL_BRIDGE_H
#define DWL_BRIDGE_H
#include <stddef.h>
#include <stdint.h>
#include <wayland-server-core.h>

#define BRIDGE_MAX_DIM 8192u
#define BRIDGE_MAX_BYTES (64u * 1024u * 1024u)

int bridge_busy(void);
/* Takes ownership of rgb only on success. No wlroots buffer is retained. */
int bridge_send(struct wl_event_loop *loop, const char *path,
        unsigned char *rgb, uint32_t width, uint32_t height);
void bridge_finish(void);
#endif
