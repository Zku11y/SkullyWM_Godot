#ifndef DWL_GPU_BRIDGE_H
#define DWL_GPU_BRIDGE_H
struct wl_event_loop;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_surface;
/* Optional, single selected native main surface. No client FD is exported. */
void gpu_bridge_init(struct wl_event_loop *loop, struct wlr_renderer *renderer,
					 struct wlr_allocator *allocator);
void gpu_bridge_commit(struct wlr_surface *surface, const char *app_id);
void gpu_bridge_finish(void);
#endif
