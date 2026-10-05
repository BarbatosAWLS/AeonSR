#include "aeon_sr/core/frame_clock.hpp"

#include <Windows.h>

namespace aeon_sr {

void FrameClock::tick() noexcept
{
	if (freq_ == 0) {
		LARGE_INTEGER f{};
		if (QueryPerformanceFrequency(&f))
			freq_ = f.QuadPart;
	}
	LARGE_INTEGER now{};
	if (freq_ > 0 && QueryPerformanceCounter(&now))
		advance(now.QuadPart);
}

void FrameClock::advance(long long now) noexcept
{
	if (freq_ <= 0)
		return;
	if (last_ != 0) {
		const double ms = 1000.0 * static_cast<double>(now - last_) / static_cast<double>(freq_);
		if (ms > 0.0 && ms < kMaxIntervalMs)
			interval_ms_ = static_cast<float>(ms);
	}
	last_ = now;
}

}
