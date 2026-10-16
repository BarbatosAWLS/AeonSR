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

enum class JitterVertexClass : uint8_t {
	Unknown = 0,
	Screen = 1,
	Transformed = 2,
};

JitterVertexClass classify_d3d9_vertex_shader(const uint32_t *code, size_t dwords);
JitterVertexClass classify_dxbc_vertex_shader(const void *code, size_t bytes);
JitterVertexClass classify_dxil_vertex_shader(const void *code, size_t bytes);

struct D3D9WTerms {
	static constexpr uint32_t kMax = 16u;
	uint32_t count = 0;
	uint16_t reg[kMax]{};
	uint8_t lane[kMax]{};
};

bool d3d9_position_w_terms(const uint32_t *code, size_t dwords, D3D9WTerms *out);

bool d3d9_w_terms_perspective(const D3D9WTerms &terms, const float *constants, uint32_t registers) noexcept;

struct DxbcWTerms {
	static constexpr uint32_t kMax = 16u;
	uint32_t count = 0;
	uint32_t slot[kMax]{};
	uint32_t space[kMax]{};
	uint16_t reg[kMax]{};
	uint8_t lane[kMax]{};
};
bool dxbc_position_w_terms(const void *code, size_t bytes, DxbcWTerms *out);
bool dxil_position_w_terms(const void *code, size_t bytes, DxbcWTerms *out);

using DxbcConstantRead = bool (*)(void *user, uint32_t slot, uint32_t reg, float out[4]);
int dxbc_w_terms_perspective(const DxbcWTerms &terms, DxbcConstantRead read, void *user) noexcept;
using DxbcSpacedConstantRead = bool (*)(void *user, uint32_t space, uint32_t slot, uint32_t reg, float out[4]);
int dxbc_w_terms_perspective_spaced(const DxbcWTerms &terms, DxbcSpacedConstantRead read, void *user) noexcept;

size_t d3d9_shader_dwords(const uint32_t *code, size_t max_dwords) noexcept;

struct ClipOffset {
	float x = 0.0f, y = 0.0f;
};
ClipOffset jitter_clip_offset(float tx, float ty, float vw, float vh) noexcept;

void offset_projection(const float in[16], ClipOffset o, float out[16]) noexcept;

}
