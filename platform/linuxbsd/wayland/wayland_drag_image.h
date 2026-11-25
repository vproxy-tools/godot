#pragma once

#include <cstdint>

// wl_shm ARGB8888 uses premultiplied color channels.
inline uint32_t wayland_drag_argb(const uint8_t *bgra) {
	const uint32_t alpha = bgra[3];
	return (alpha << 24) | (((bgra[2] * alpha + 127) / 255) << 16) |
			(((bgra[1] * alpha + 127) / 255) << 8) | ((bgra[0] * alpha + 127) / 255);
}
