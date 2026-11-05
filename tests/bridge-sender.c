/* Exercise the actual sender without starting a compositor. */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "bridge.h"

int
main(int argc, char **argv)
{
	struct wl_event_loop *loop = wl_event_loop_create();
	size_t i, size = 1024 * 1024 * 3;
	unsigned char *rgb = malloc(size);
	unsigned int ticks = 0;
	struct timespec start, end;
	double elapsed;
	if (argc != 2 || !loop || !rgb)
		return 2;
	for (i = 0; i < size; i++)
		rgb[i] = (unsigned char)i;
	clock_gettime(CLOCK_MONOTONIC, &start);
	if (!bridge_send(loop, argv[1], rgb, 1024, 1024)) {
		free(rgb);
		wl_event_loop_destroy(loop);
		return 3;
	}
	while (bridge_busy()) {
		if (wl_event_loop_dispatch(loop, 10) < 0)
			return 4;
		ticks++;
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	elapsed = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1e9;
	printf("dispatches=%u elapsed=%.3f\n", ticks, elapsed);
	wl_event_loop_destroy(loop);
	return 0;
}
