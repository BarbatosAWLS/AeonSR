#include "aeon_sr/framegen/overlay_cover.hpp"

namespace aeon_sr {

void OverlayCover::reset() noexcept
{
	*this = OverlayCover{};
}

bool OverlayCover::due(double now) const noexcept
{
	return !covered_ && now >= next_;
}

bool OverlayCover::checked(double now, bool above) noexcept
{
	next_ = now + kCheckMs;
	if (covered_)
		return false;
	under_[at_] = !above;
	at_ = (at_ + 1) % kChecks;
	if (n_ < kChecks)
		++n_;
	if (above)
		return false;
	uint32_t under = 0;
	for (uint32_t i = 0; i < n_; ++i)
		under += under_[i] ? 1u : 0u;
	if (n_ == kChecks && under >= kUnder) {
		covered_ = true;
		return false;
	}
	++raised_;
	return true;
}

}
