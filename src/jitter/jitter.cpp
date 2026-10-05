#include "aeon_sr/jitter/jitter.hpp"

#include <atomic>

namespace aeon_sr {

namespace {
std::atomic<uint32_t> g_phases{ kJitterPhases };
}

uint32_t jitter_phases() noexcept
{
	return g_phases.load();
}

void set_jitter_phases(uint32_t phases) noexcept
{
	g_phases.store(phases >= 2u ? phases : kJitterPhases);
}

float radical_inverse(uint32_t index, uint32_t base) noexcept
{
	if (base < 2u)
		return 0.0f;
	float result = 0.0f;
	float fraction = 1.0f / static_cast<float>(base);
	while (index > 0u) {
		result += static_cast<float>(index % base) * fraction;
		index /= base;
		fraction /= static_cast<float>(base);
	}
	return result;
}

Jitter halton_jitter(uint32_t index) noexcept
{
	const uint32_t phase = (index % jitter_phases()) + 1u;
	Jitter j;
	j.x = radical_inverse(phase, 2u) - 0.5f;
	j.y = radical_inverse(phase, 3u) - 0.5f;
	return j;
}

Jitter halton_jitter(uint32_t index, float amount) noexcept
{
	if (!(amount > 0.0f) || !(amount <= 1.0f))
		amount = amount > 1.0f ? 1.0f : 0.0f;
	Jitter j = halton_jitter(index);
	j.x *= amount;
	j.y *= amount;
	return j;
}

}
