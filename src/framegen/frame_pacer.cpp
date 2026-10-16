#include "aeon_sr/framegen/frame_pacer.hpp"

#include <algorithm>
#include <cmath>

namespace aeon_sr {

const char *pacer_state_label(PacerState s) noexcept
{
	return s == PacerState::Engaged ? "generating" : "starting";
}

void FramePacer::reset() noexcept
{
	*this = FramePacer{};
}

double FramePacer::median(const double *v, uint32_t n) const noexcept
{
	if (n == 0)
		return 0.0;
	double s[kWindow];
	std::copy(v, v + n, s);
	std::sort(s, s + n);
	return n % 2 != 0 ? s[n / 2] : 0.5 * (s[n / 2 - 1] + s[n / 2]);
}

bool FramePacer::begin(double now) noexcept
{
	generating_ = false;
	frame_hitched_ = false;
	if (last_return_ < 0.0)
		return false;
	frame_hitched_ = reference_ > 0.0 && now - last_return_ > kHitch * reference_;
	generating_ = state_ == PacerState::Engaged && !frame_hitched_;
	return generating_;
}

PacerPlan FramePacer::plan(double now, bool have_generated, double limit_ms, const PacerGrid &display) noexcept
{
	PacerPlan p;
	const double limit = limit_ms > 0.0 ? limit_ms : 0.0;
	const double base = last_return_ >= 0.0 ? anchor_ : now;
	const double period = display.period_ms;
	display_paced_ = period > 0.0;
	const double ideal = ideal_real_;
	ideal_real_ = -1.0;
	if (generating_ && have_generated && display_paced_) {
		p.generate = true;
		const double interval = std::max(limit, 2.0 * period);
		const double target = (ideal >= 0.0 && std::fabs(ideal - base) < period ? ideal : base) + interval;
		const auto slot = [&](double t) {
			return display.vblank_ms + (std::round((t - display.vblank_ms) / period - kSlotPhase) + kSlotPhase) * period;
		};
		const double real_slot = slot(target);
		if (now + kSlotLeadMs >= real_slot) {
			p.gen_at = p.real_at = now;
		} else {
			p.real_at = real_slot;
			p.gen_at = std::max(now, std::min(slot(target - 0.5 * interval), real_slot - period));
			ideal_real_ = target;
		}
	} else if (generating_ && have_generated) {
		const double half = std::max(reference_, limit) * 0.5;
		p.generate = true;
		p.gen_at = std::max(now, base + half);
		p.real_at = std::max(p.gen_at + half, base + limit);
	} else {
		p.gen_at = now;
		p.real_at = frame_hitched_ ? now : std::max(now, base + limit);
	}
	p.real_at = std::min(p.real_at, now + kMaxWaitMs);
	p.gen_at = std::min(p.gen_at, p.real_at);
	planned_real_ = p.real_at;
	planned_wait_ = p.real_at > now;
	return p;
}

double FramePacer::after_generated(double real_at, double gen_done, const PacerGrid &display) noexcept
{
	if (display.period_ms <= 0.0)
		return real_at;
	const double period = display.period_ms;
	const double blank = display.vblank_ms + std::ceil((gen_done - display.vblank_ms) / period) * period;
	const double t = std::min(std::max(real_at, blank + kAfterVblankMs), gen_done + kMaxWaitMs);
	if (t > planned_real_) {
		planned_real_ = t;
		planned_wait_ = true;
	}
	return t;
}

void FramePacer::returned(double t) noexcept
{
	const double prev = last_return_;
	const double prev_anchor = anchor_;
	last_return_ = t;
	const double late = planned_wait_ ? std::max(0.0, t - planned_real_) : 0.0;
	anchor_ = planned_wait_ && late < kLateAnchorMs ? planned_real_ : t;
	planned_wait_ = false;
	if (prev < 0.0)
		return;
	const double interval = (t - late) - prev_anchor;
	if (frame_hitched_ || (reference_ > 0.0 && interval > kHitch * reference_)) {
		hitches_[hitches_in_a_row_++] = interval;
		if (hitches_in_a_row_ < kHitchesToAdopt)
			return;
		std::copy(hitches_, hitches_ + kHitchesToAdopt, window_);
		window_n_ = kHitchesToAdopt;
		window_at_ = kHitchesToAdopt % kWindow;
		hitches_in_a_row_ = 0;
		reference_ = median(window_, window_n_);
		return;
	}
	hitches_in_a_row_ = 0;
	window_[window_at_] = interval;
	window_at_ = (window_at_ + 1) % kWindow;
	window_n_ = std::min(window_n_ + 1, kWindow);
	reference_ = median(window_, window_n_);
	if (state_ == PacerState::Measuring && window_n_ >= kStartFrames)
		state_ = PacerState::Engaged;
}

}
