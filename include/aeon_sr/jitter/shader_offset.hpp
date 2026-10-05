#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aeon_sr {

inline constexpr uint32_t kD3D9OffsetConstants = 256u;
inline constexpr uint32_t kD3D10OffsetSlots = 14u;

bool offset_d3d9_vertex_shader(const uint32_t *code, size_t dwords, uint32_t max_constants,
	std::vector<uint32_t> &out, uint32_t *reg, bool *indexed = nullptr);

bool offset_dxbc_vertex_shader(const void *code, size_t bytes, std::vector<uint8_t> &out, uint32_t *slot);

void dxbc_checksum(const void *code, size_t bytes, uint32_t out[4]) noexcept;

size_t d3d9_shader_dwords(const uint32_t *code, size_t max_dwords) noexcept;

struct ClipOffset {
	float x = 0.0f, y = 0.0f;
};
ClipOffset jitter_clip_offset(float tx, float ty, float vw, float vh) noexcept;

void offset_projection(const float in[16], ClipOffset o, float out[16]) noexcept;

}
