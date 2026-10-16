#pragma once

#include "aeon_sr/jitter/shader_offset.hpp"

#include <cstddef>
#include <cstdint>

namespace aeon_sr {

JitterVertexClass classify_spirv_vertex_shader(const void *code, size_t bytes, const char *entry_point = nullptr);

struct SpirvWTerms {
	static constexpr uint32_t kMax = 16u;
	enum Kind : uint8_t { Buffer = 0, Push = 1 };
	uint32_t count = 0;
	uint8_t kind[kMax]{};
	uint32_t set[kMax]{};
	uint32_t binding[kMax]{};
	uint32_t offset[kMax]{};
};
bool spirv_position_w_terms(const void *code, size_t bytes, const char *entry_point, SpirvWTerms *out);

using SpirvConstantRead = bool (*)(void *user, const SpirvWTerms &terms, uint32_t index, float *out);
int spirv_w_terms_perspective(const SpirvWTerms &terms, SpirvConstantRead read, void *user) noexcept;

JitterVertexClass classify_glsl_vertex_shader(const char *source, size_t length);

JitterVertexClass classify_gl_fixed_function(const float projection[16], const float modelview[16]) noexcept;

bool gl_fixed_function_perspective(const float projection[16], const float modelview[16]) noexcept;

}
