#include "aeon_sr/core/latency_governor.hpp"

#include <algorithm>

namespace aeon_sr {

double LatencyGovernor::median(const double *v, uint32_t n) noexcept
{
	return percentile(v, n, 1u, 2u);
}

double LatencyGovernor::percentile(const double *v, uint32_t n, uint32_t num, uint32_t den) noexcept
{
	if (n == 0u)
		return 0.0;
	double s[kWindow];
	std::copy(v, v + n, s);
	std::sort(s, s + n);
	return s[std::min(n - 1u, (n * num) / den)];
}

double LatencyGovernor::recent_interval() const noexcept
{
	double v[kProbeWindow];
	for (uint32_t i = 0; i < kProbeWindow; ++i)
		v[i] = intervals_[(interval_at_ + kWindow - 1u - i) % kWindow];
	std::sort(v, v + kProbeWindow);
	return v[kProbeWindow / 2u];
}

double LatencyGovernor::frame_start(double now, double paced_slot) noexcept
{
	if (sent_ >= 0.0 && now >= sent_) {
		holds_[hold_at_] = now - sent_;
		hold_at_ = (hold_at_ + 1u) % kBlockWindow;
		hold_n_ = std::min(hold_n_ + 1u, kBlockWindow);
		hold_med_ = median(holds_, hold_n_);
		hold_min_ = *std::min_element(holds_, holds_ + hold_n_);
		sent_ = -1.0;
	}
	const double interval = interval_n_ >= kWindow / 2u ? median(intervals_, interval_n_) : 0.0;
	double at = now;
	if (paced_slot >= 0.0) {
		wait_ = 0.0;
		if (work_n_ >= kWindow / 2u)
			at = std::max(now, paced_slot - work_p90_ - kMarginMs);
	} else if (cooldown_ != 0u) {
		--cooldown_;
		wait_ = 0.0;
	} else if (hold_n_ >= kBlockWindow && interval > 0.0) {
		if (wait_ <= 0.0)
			interval_at_start_ = interval;
		const bool missed = wait_ > 0.0 && interval_at_start_ > 0.0 && last_interval_ > kMissRatio * interval_at_start_;
		misses_ = (misses_ << 1) | (missed ? 1u : 0u);
		uint32_t recent = 0;
		for (uint32_t m = misses_; m != 0; m &= m - 1u)
			++recent;
		if (interval_at_start_ > 0.0 && wait_ > 0.0 &&
			(interval > kSlowdown * interval_at_start_ || recent >= kMissesToCool)) {
			wait_ = 0.0;
			cooldown_ = kCooldownFrames;
			++cooldowns_;
			hold_n_ = 0u;
			misses_ = 0u;
		} else if (missed) {
			wait_ = std::max(0.0, wait_ - std::max(kMissStepMs, 0.25 * wait_));
			hold_n_ = 0u;
			probing_ = false;
		} else if (hold_med_ < kProbeHoldMs && work_med_ > kBoundShare * interval && interval_n_ >= kProbeWindow) {
			const double recent = recent_interval();
			if (rest_ != 0u) {
				--rest_;
			} else if (!probing_) {
				probing_ = true;
				probe_base_ = recent;
				probe_count_ = 0u;
			} else if (recent > (1.0 + kProbeRise) * probe_base_) {
				wait_ = std::max(0.0, wait_ - kProbeBackOffMs);
				probing_ = false;
				rest_ = kRestFrames;
			} else if (++probe_count_ % kProbeEvery == 0u) {
				wait_ += kProbeStepMs;
			}
			wait_ = std::clamp(wait_, 0.0, std::min(kMaxWaitMs, interval));
		} else {
			probing_ = false;
			wait_ += kGain * (hold_min_ - kTargetHoldMs);
			wait_ = std::clamp(wait_, 0.0, std::min(kMaxWaitMs, interval));
		}
		at = now + wait_;
	}
	last_wait_ = at - now;
	return at;
}

void LatencyGovernor::started(double t) noexcept
{
	started_ = t;
}

void LatencyGovernor::ready(double t) noexcept
{
	if (started_ >= 0.0 && t >= started_) {
		work_[work_at_] = t - started_;
		work_at_ = (work_at_ + 1u) % kWindow;
		work_n_ = std::min(work_n_ + 1u, kWindow);
		work_p90_ = percentile(work_, work_n_, 9u, 10u);
		work_med_ = median(work_, work_n_);
	}
}

void LatencyGovernor::sent(double t) noexcept
{
	if (last_sent_ >= 0.0 && t > last_sent_) {
		last_interval_ = t - last_sent_;
		intervals_[interval_at_] = t - last_sent_;
		interval_at_ = (interval_at_ + 1u) % kWindow;
		interval_n_ = std::min(interval_n_ + 1u, kWindow);
	}
	last_sent_ = t;
	sent_ = t;
}

}
