#pragma once

#include <cstdint>

namespace aeon_sr {

inline constexpr uint32_t kJitterPhases = 64u;

uint32_t jitter_phases() noexcept;
void set_jitter_phases(uint32_t phases) noexcept;

struct Jitter {
	float x = 0.0f;
	float y = 0.0f;
};

float radical_inverse(uint32_t index, uint32_t base) noexcept;

Jitter halton_jitter(uint32_t index) noexcept;

Jitter halton_jitter(uint32_t index, float amount) noexcept;

}
