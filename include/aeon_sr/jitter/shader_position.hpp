#pragma once

#include "aeon_sr/jitter/shader_offset.hpp"

#include <cstddef>
#include <cstdint>

namespace aeon_sr {

JitterVertexClass classify_spirv_vertex_shader(const void *code, size_t bytes, const char *entry_point = nullptr);

JitterVertexClass classify_glsl_vertex_shader(const char *source, size_t length);

JitterVertexClass classify_gl_fixed_function(const float projection[16], const float modelview[16]) noexcept;

bool gl_fixed_function_perspective(const float projection[16], const float modelview[16]) noexcept;

}
