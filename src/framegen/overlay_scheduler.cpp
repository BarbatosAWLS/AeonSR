#include "aeon_sr/framegen/overlay_scheduler.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>

namespace aeon_sr {

uint32_t auto_multiplier(double frame_ms, double period_ms, uint32_t current, uint32_t max) noexcept
{
	max = std::max(1u, max);
	if (!(frame_ms > 0.0) || !(period_ms > 0.0))
		return current >= 1 ? std::min(current, max) : std::min(2u, max);
	const double frames = frame_ms / period_ms;
	if (current >= 1 && current <= max && frames > static_cast<double>(current - 1) + 0.02 &&
		frames <= static_cast<double>(current) + 0.1)
		return current;
	const double want = std::ceil(frames - 0.05);
	return static_cast<uint32_t>(std::clamp(want, 1.0, static_cast<double>(max)));
}

bool refresh_agrees(double period_ms, double nominal_hz) noexcept
{
	if (nominal_hz <= 1.0)
		return true;
	if (period_ms <= 0.0)
		return false;
	return std::fabs(1000.0 / period_ms - nominal_hz) <= std::max(0.04 * nominal_hz, 1.0);
}

uint32_t even_cap_fps(double fps, double refresh_hz) noexcept
{
	if (!(fps > 1.0) || !(refresh_hz > 1.0) || fps >= refresh_hz * 0.97)
		return 0;
	const double ratio = refresh_hz / fps;
	if (std::fabs(ratio - std::round(ratio)) < 0.06)
		return 0;
	for (uint32_t k = static_cast<uint32_t>(std::ceil(ratio)); k <= 8; ++k) {
		const double cap = refresh_hz / static_cast<double>(k);
		if (std::fabs(cap - std::round(cap)) < 0.05 && cap >= 10.0)
			return static_cast<uint32_t>(std::lround(cap));
	}
	return 0;
}

void OverlayPace::note(bool missed, double period_ms) noexcept
{
	recent_misses_ -= recent_[recent_at_];
	recent_[recent_at_] = missed ? 1 : 0;
	recent_misses_ += recent_[recent_at_];
	recent_at_ = (recent_at_ + 1) % kMissWindow;
	if (missed) {
		++misses_total_;
		calm_ = 0;
		if (recent_misses_ >= kMissesToGrow) {
			lead_ = std::min(lead_ + kLeadStepMs, lead_cap_ * period_ms);
			for (uint8_t &r : recent_)
				r = 0;
			recent_misses_ = 0;
		}
	} else if (++calm_ >= kCalmPlans) {
		calm_ = 0;
		lead_ = std::max(kMinLeadMs, lead_ - 0.25 * kLeadStepMs);
	}
}

double OverlayPace::estimate_vblank(double vblank_ms, double period_ms) noexcept
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
	return best;
}

double OverlayPace::plan(double now_ms, double vblank_ms, double period_ms, double *present_at) noexcept
{
	const double vblank = estimate_vblank(vblank_ms, period_ms);
	if (early_) {
		double next = vblank + std::ceil((now_ms - vblank) / period_ms) * period_ms;
		if (next <= now_ms + 1.5)
			next += period_ms;
		*present_at = now_ms;
		if (last_ > 0.0 && next < last_ + 0.5 * period_ms) {
			next = last_ + period_ms;
			*present_at = last_ + 0.5;
		}
		last_ = next;
		return next + period_ms;
	}
	const double lead = std::min(lead_, lead_cap_ * period_ms);
	const double least = std::min({ kMinLeadMs, lead, 0.5 * period_ms });
	double next = vblank + std::ceil((now_ms + least - vblank) / period_ms) * period_ms;
	if (next < now_ms + least)
		next += period_ms;
	if (presented_ && last_ > 0.0 && !fixed_)
		note(now_ms > last_ + period_ms, period_ms);
	presented_ = false;
	if (last_ > 0.0 && next < last_ + 0.5 * period_ms)
		next = last_ + period_ms;
	*present_at = std::max(now_ms, next - lead);
	last_ = next;
	return next;
}

void OverlayScheduler::reset() noexcept
{
	*this = OverlayScheduler{};
}

double OverlayScheduler::interval_stat(bool p90) const noexcept
{
	if (interval_n_ == 0)
		return 0.0;
	double s[kIntervals];
	std::copy(intervals_, intervals_ + interval_n_, s);
	std::sort(s, s + interval_n_);
	return p90 ? s[std::min(interval_n_ - 1, interval_n_ * 9 / 10)] : s[interval_n_ / 2];
}

double OverlayScheduler::lag_stat() const noexcept
{
	if (lag_n_ == 0)
		return 0.0;
	double s[kIntervals];
	std::copy(lags_, lags_ + lag_n_, s);
	std::sort(s, s + lag_n_);
	return s[std::min(lag_n_ - 1, lag_n_ * 9 / 10)];
}

double OverlayScheduler::desired_latency(double period_ms) const noexcept
{
	if (need_n_ == 0)
		return 0.0;
	const uint32_t first = (need_at_ + kNeedSamples - need_n_) % kNeedSamples;
	const double newest = needs_[(need_at_ + kNeedSamples - 1) % kNeedSamples].at;
	const auto mad = [](double *v, uint32_t n) {
		std::nth_element(v, v + n / 2, v + n);
		const double med = v[n / 2];
		for (uint32_t i = 0; i < n; ++i)
			v[i] = std::fabs(v[i] - med);
		std::nth_element(v, v + n / 2, v + n);
		return 1.4826 * v[n / 2];
	};
	double afters[kNeedSamples];
	uint32_t known = 0;
	for (uint32_t k = 0; k < need_n_; ++k) {
		const Need &n = needs_[(first + k) % kNeedSamples];
		if (newest - n.at <= kNeedWindowMs && n.slack >= 0.0 && n.after >= 0.0)
			afters[known++] = n.after;
	}
	const bool timed = known >= 8 && interval_n_ >= 8;
	double spread = 0.0;
	if (timed) {
		double iv[kIntervals];
		std::copy(intervals_, intervals_ + interval_n_, iv);
		const double presents = mad(iv, interval_n_) / std::sqrt(2.0);
		const double gpu = mad(afters, known);
		spread = std::min(kRoundingFraction * period_ms, std::sqrt(presents * presents + gpu * gpu));
	}
	const double guard = std::min(kGuardShare * period_ms, kGuardFloorMs + 2.0 * spread);
	double need = 0.0;
	for (uint32_t k = 0; k < need_n_; ++k) {
		const Need &n = needs_[(first + k) % kNeedSamples];
		if (newest - n.at > kNeedWindowMs)
			continue;
		const double round = !timed || n.slack < 0.0 ? kRoundingFraction * period_ms
			: n.slack < guard ? period_ms : 0.0;
		need = std::max(need, n.lag + round - 0.5 * period_ms);
		if (n.pair >= 0.0)
			need = std::max(need, n.pair + round - 1.5 * period_ms);
	}
	return std::min(kMaxLatencyMs, need + kMarginMs + (timed ? spread : 0.0));
}

void OverlayScheduler::add(uint64_t id, double presented_ms, bool pairs_with_previous, double usable_ms, uint32_t steps,
	uint32_t made, bool vendor, double ready_slack_ms, double ready_after_ms) noexcept
{
	if (!reals_.empty()) {
		const Real &last = reals_.back();
		if (id <= last.id || presented_ms <= last.at)
			return;
	}
	const double lag = std::max(0.0, usable_ms - presented_ms);
	if (lag < kMaxLatencyMs) {
		lags_[lag_at_] = lag;
		lag_at_ = (lag_at_ + 1) % kIntervals;
		lag_n_ = std::min(lag_n_ + 1, kIntervals);
	}
	if (!reals_.empty()) {
		const Real &last = reals_.back();
		const double interval = presented_ms - last.at;
		const double median = interval_stat(false);
		if (median <= 0.0 || interval < kHitch * median) {
			long_run_ = 0;
		} else if (++long_run_ >= kSlower) {
			interval_n_ = interval_at_ = 0;
			long_run_ = 0;
		}
		if (long_run_ == 0) {
			intervals_[interval_at_] = interval;
			interval_at_ = (interval_at_ + 1) % kIntervals;
			interval_n_ = std::min(interval_n_ + 1, kIntervals);
		}
		pairs_with_previous = pairs_with_previous && id == last.id + 1;
	} else {
		pairs_with_previous = false;
	}
	const bool after_skip = last_display_ > 0.0 && usable_ms - last_display_ > 1.5 * last_period_ &&
		std::bitset<16>(skips_).count() < 8;
	if (lag < kMaxLatencyMs && !after_skip) {
		const double pair = pairs_with_previous && long_run_ == 0 && !reals_.empty() ? usable_ms - reals_.back().at
			: -1.0;
		needs_[need_at_] = Need{ presented_ms, lag, pair < kMaxLatencyMs ? pair : -1.0, ready_slack_ms,
			ready_slack_ms >= 0.0 ? ready_after_ms : -1.0 };
		need_at_ = (need_at_ + 1) % kNeedSamples;
		need_n_ = std::min(need_n_ + 1, kNeedSamples);
	}
	reals_.push_back(Real{ id, presented_ms, pairs_with_previous, false, steps >= 2 ? steps : 0u, made, vendor, 0.0,
		usable_ms });
	while (reals_.size() > kKeep)
		reals_.pop_front();
}

OverlayChoice OverlayScheduler::choose(double display_ms, double period_ms) noexcept
{
	OverlayChoice c;
	skips_ = (skips_ << 1) |
		(last_display_ > 0.0 && display_ms - last_display_ > 1.5 * last_period_ ? 1u : 0u);
	skips_ &= 0xFFFFu;
	last_display_ = display_ms;
	last_period_ = period_ms;
	if (reals_.size() < 2 || interval_n_ == 0)
		return c;
	const double desired = desired_latency(period_ms);
	if (latency_ < 0.0)
		latency_ = std::min(kMaxLatencyMs, desired + 0.5 * period_ms);
	else if (latency_ > desired)
		latency_ = std::max(desired, latency_ - kShrinkFraction * period_ms);
	else if (latency_ < desired)
		latency_ = std::min(desired, latency_ + kGrowFraction * period_ms);
	double content = display_ms - latency_;
	if (reals_.front().at > content + 0.5 * period_ms) {
		latency_ = std::max(desired, display_ms - reals_.front().at);
		content = display_ms - latency_;
	}

	long due = -1;
	for (size_t i = 0; i < reals_.size(); ++i)
		if (reals_[i].at <= content + 0.5 * period_ms)
			due = static_cast<long>(i);
	if (due >= 0 && !reals_[static_cast<size_t>(due)].shown) {
		for (long i = 0; i < due; ++i) {
			if (!reals_[static_cast<size_t>(i)].shown) {
				reals_[static_cast<size_t>(i)].shown = true;
				++reals_skipped_;
			}
		}
		reals_[static_cast<size_t>(due)].shown = true;
		reals_[static_cast<size_t>(due)].shown_at = display_ms;
		++reals_shown_;
		c.kind = OverlayChoice::Kind::Real;
		c.b = reals_[static_cast<size_t>(due)].id;
		c.content_ms = reals_[static_cast<size_t>(due)].at;
		c.held_ms = std::max(0.0, display_ms - reals_[static_cast<size_t>(due)].usable);
		return c;
	}

	if (due >= 0 && static_cast<size_t>(due) + 1 < reals_.size()) {
		const Real &a = reals_[static_cast<size_t>(due)];
		const Real &b = reals_[static_cast<size_t>(due) + 1];
		const double span = b.at - a.at;
		const double median = interval_stat(false);
		if (b.pairs && span > 0.0 && (median <= 0.0 || span < kHitch * median)) {
			double t = std::clamp((content - a.at) / span, 0.0, 1.0);
			const double ratio = median > 0.0 ? median / period_ms : 0.0;
			const double whole = std::round(ratio);
			if (whole >= 2.0 && std::fabs(ratio - whole) < 0.03 && a.shown_at > 0.0) {
				const double b_at = display_ms +
					std::ceil((b.at + latency_ - 0.5 * period_ms - display_ms) / period_ms - 1e-6) * period_ms;
				if (b_at - a.shown_at > 1.5 * period_ms)
					t = std::clamp((display_ms - a.shown_at) / (b_at - a.shown_at), 0.0, 1.0);
			}
			c.a = a.id;
			c.b = b.id;
			if (b.steps >= 2) {
				const uint32_t k = static_cast<uint32_t>(std::lround(t * b.steps));
				const bool missing = b.vendor && k > b.made;
				if (k == 0 || k >= b.steps || missing || (k == last_index_ && b.id == last_index_of_)) {
					c.kind = OverlayChoice::Kind::Hold;
					if (missing)
						++held_;
					else
						++repeated_;
					return c;
				}
				last_index_ = k;
				last_index_of_ = b.id;
				c.index = k;
				c.made = b.vendor;
				c.t = static_cast<float>(static_cast<double>(k) / b.steps);
			} else {
				c.t = static_cast<float>(t);
			}
			c.kind = OverlayChoice::Kind::Generated;
			c.content_ms = a.at + static_cast<double>(c.t) * span;
			++generated_;
			return c;
		}
		c.kind = OverlayChoice::Kind::Hold;
		++held_;
		return c;
	}

	if (due >= 0) {
		latency_ = std::min(std::min(kMaxLatencyMs, desired + period_ms), latency_ + 0.25 * period_ms);
		c.kind = OverlayChoice::Kind::Hold;
		++held_;
		return c;
	}
	return c;
}

}
