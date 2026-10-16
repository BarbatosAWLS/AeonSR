#include "aeon_sr/core/display_pacer.hpp"

#include <algorithm>
#include <cmath>

namespace aeon_sr {

double DisplayPacer::estimate_vblank(double vblank_ms, double period_ms) noexcept
{
	if (vblank_ms != last_vblank_in_) {
		last_vblank_in_ = vblank_ms;
		vblanks_[vblank_at_] = vblank_ms;
		vblank_at_ = (vblank_at_ + 1u) % kVblankRing;
		vblank_n_ = std::min(vblank_n_ + 1u, kVblankRing);
	}
	double best = vblank_ms;
	for (uint32_t i = 0; i < vblank_n_; ++i) {
		const double v = vblanks_[i];
		const double carried = v + std::round((vblank_ms - v) / period_ms) * period_ms;
		if (std::fabs(carried - vblank_ms) < 0.5 * period_ms)
			best = std::min(best, carried);
	}
	vblank_ = best;
	return best;
}

void DisplayPacer::release() noexcept
{
	engaged_ = false;
	fits_ = 0;
	missed_ = 0;
	calm_ = 0;
	last_slot_ = -1.0;
	last_blank_ = -1.0;
	backoff_ = std::min(backoff_ * 2u, kMaxBackoff);
}

double DisplayPacer::lead_ms() const noexcept
{
	const double most = period_ > 0.0 ? 0.5 * period_ : kLeadMs;
	return std::max(kLeadMs, std::min(kLeadMs + gpu_tail_, most));
}

double DisplayPacer::plan(double now, double period_ms, double vblank_ms) noexcept
{
	const bool hitch = last_return_ >= 0.0 && period_ms > 0.0 && now - last_return_ >= kHitchPeriods * period_ms;
	if (last_return_ >= 0.0 && now >= last_return_ && !hitch) {
		window_[window_at_] = now - last_return_;
		window_at_ = (window_at_ + 1u) % kWindow;
		window_n_ = std::min(window_n_ + 1u, kWindow);
		double sorted[kWindow];
		std::copy(window_, window_ + window_n_, sorted);
		std::sort(sorted, sorted + window_n_);
		work_ = sorted[window_n_ / 2u];
		work_p90_ = sorted[(window_n_ * 9u) / 10u];
	}
	if (period_ms <= 0.0) {
		engaged_ = false;
		fits_ = 0;
		missed_ = 0;
		last_slot_ = -1.0;
		last_blank_ = -1.0;
		late_wake_ = false;
		return now;
	}
	const double vblank = estimate_vblank(vblank_ms, period_ms);
	period_ = period_ms;
	const auto blank_after = [&](double t) { return vblank + std::ceil((t - vblank) / period_ms) * period_ms; };
	if (engaged_) {
		const bool missed = !hitch && last_blank_ >= 0.0 && now > last_blank_ + period_ms - kLateMarginMs;
		const bool miss = missed || late_wake_;
		late_wake_ = false;
		missed_ = (missed_ << 1) | (miss ? 1u : 0u);
		missed_total_ += miss ? 1u : 0u;
		uint32_t n = 0;
		for (uint32_t m = missed_ & ((1u << kWindow) - 1u); m != 0; m &= m - 1u)
			++n;
		if (n >= kMissesToRelease || (window_n_ >= kWindow / 2u && work_ * kReleaseRatio < period_ms)) {
			release();
			return now;
		}
		if (++calm_ >= kCalmFrames) {
			calm_ = 0;
			backoff_ = std::max(1u, backoff_ / 2u);
		}
	} else {
		late_wake_ = false;
		const bool fits = window_n_ >= kWindow / 2u && work_p90_ + kRoomMs <= period_ms &&
			work_ * kMaxRatio >= period_ms;
		fits_ = fits ? fits_ + 1u : 0u;
		if (fits_ < kFitsToEngage * backoff_)
			return now;
		engaged_ = true;
		missed_ = 0;
		calm_ = 0;
	}
	const double lead = lead_ms();
	double blank = blank_after(now + kLateMarginMs + (lead - kLeadMs));
	if (last_slot_ >= 0.0 && blank - lead < last_slot_ + 0.5 * period_ms)
		blank += period_ms;
	const double slot = std::max(now, blank - lead);
	last_slot_ = blank - lead;
	last_blank_ = blank;
	planned_ = slot > now ? slot : -1.0;
	return slot;
}

void DisplayPacer::returned(double t) noexcept
{
	last_return_ = t;
	if (engaged_ && planned_ >= 0.0 && t > planned_ + kLateWakeMs)
		late_wake_ = true;
	planned_ = -1.0;
}

}
