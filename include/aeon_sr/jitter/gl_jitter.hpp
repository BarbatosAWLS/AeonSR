#pragma once

#include "aeon_sr/jitter/scene_jitter.hpp"

#include <cstdint>

namespace aeon_sr {

inline constexpr int kGlJitterMinSubpixelBits = 4;

bool gl_jitter_usable() noexcept;
const char *gl_jitter_note() noexcept;
int gl_jitter_subpixel_bits() noexcept;

bool gl_jitter_bind(uint32_t count, const JitterViewport *viewports) noexcept;
bool gl_jitter_own_call() noexcept;

uint32_t gl_jitter_read(uint32_t count, JitterViewport *out) noexcept;
bool gl_jitter_bound_as(uint32_t count, const JitterViewport *expect) noexcept;

void gl_jitter_observe(uint64_t frame) noexcept;
bool gl_clip_origin_upper_left() noexcept;

class GlJitterQuiet {
public:
	GlJitterQuiet() noexcept;
	~GlJitterQuiet();
	GlJitterQuiet(const GlJitterQuiet &) = delete;
	GlJitterQuiet &operator=(const GlJitterQuiet &) = delete;

private:
	bool was_;
};

}
