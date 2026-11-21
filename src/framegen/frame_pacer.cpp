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

void FramePacer::set_fixed(bool fixed) noexcept
{
	if (fixed && !fixed_ && as_fast_) {
		as_fast_ = start_probe_ = confirmed_ = try_from_start_ = false;
		own_n_ = own_at_ = 0;
	} else if (!fixed && fixed_) {
		failed_tries_ = resumes_ = 0;
		given_up_ = false;
		next_try_ = 0.0;
		own_n_ = own_at_ = 0;
		if (last_return_ >= 0.0)
			engaged_at_ = as_fast_since_ = last_return_;
		if (state_ == PacerState::Engaged && refresh_ > 0.0 && limit_ <= kFitShare * refresh_) {
			as_fast_ = start_probe_ = true;
			confirmed_ = try_from_start_ = false;
			fast_n_ = fast_at_ = 0;
		}
	}
	fixed_ = fixed;
}

bool FramePacer::begin(double now) noexcept
{
	generating_ = false;
	frame_hitched_ = false;
	if (last_return_ < 0.0)
		return false;
	frame_hitched_ = reference_ > 0.0 && now - last_return_ > kHitch * reference_;
	if (last_generated_ && !frame_hitched_) {
		own_[own_at_] = now - last_return_;
		own_at_ = (own_at_ + 1) % kOwnFrames;
		own_n_ = std::min(own_n_ + 1, kOwnFrames);
		own_median_ = median(own_, own_n_);
	}
	const bool held_at_two = window_route_ && last_generated_ && window_n_ >= kWindow &&
		reference_ >= kHeldLow * refresh_ && reference_ <= kHeldHigh * refresh_ &&
		now >= std::max(next_try_, engaged_at_ + kProbeEveryMs);
	if (!fixed_ && !as_fast_ && !given_up_ && refresh_ > 0.0 && state_ == PacerState::Engaged && limit_ <= kFitShare * refresh_ &&
		((own_n_ >= kOwnFrames && now >= next_try_ && own_median_ <= kFitShare * refresh_) || held_at_two)) {
		as_fast_ = true;
		confirmed_ = false;
		as_fast_since_ = now;
		fast_n_ = fast_at_ = 0;
	} else if (as_fast_ && refresh_ <= 0.0) {
		as_fast_ = start_probe_ = confirmed_ = false;
		own_n_ = own_at_ = 0;
	}
	generating_ = state_ == PacerState::Engaged && !frame_hitched_ && !as_fast_;
	return generating_;
}

double StepFit::median() const noexcept
{
	double s[kWindow];
	std::copy(iv_, iv_ + n_, s);
	std::sort(s, s + n_);
	return s[n_ / 2];
}

uint32_t StepFit::next(double interval_ms, double period_ms, uint32_t asked) noexcept
{
	const uint32_t most = std::clamp(asked, 2u, kPacerMaxMade + 1u);
	if (period_ms <= 0.0)
		return most;
	if (steps_ > most) {
		steps_ = base_ = most;
		probing_ = false;
		restart();
	}
	if (interval_ms > 0.0) {
		iv_[at_] = interval_ms;
		at_ = (at_ + 1) % kWindow;
		n_ = std::min(n_ + 1, kWindow);
	}
	++since_;
	if (n_ < kWindow)
		return steps_;
	const double m = median();
	const double held = static_cast<double>(steps_) * period_ms * kHeldShare;
	if (probing_) {
		probing_ = false;
		if (m > held)
			steps_ = base_;
		base_ = steps_;
		restart();
		return steps_;
	}
	if (steps_ < most && m >= static_cast<double>(steps_ + 1) * period_ms * kRoomShare) {
		base_ = ++steps_;
		restart();
		return steps_;
	}
	if (steps_ > 2 && m <= held && since_ >= kProbeEvery) {
		base_ = steps_--;
		probing_ = true;
		restart();
	}
	return steps_;
}

void pick_steps(uint32_t made_steps, uint32_t shown, uint32_t *out) noexcept
{
	made_steps = std::clamp(made_steps, 2u, kPacerMaxMade + 1u);
	shown = std::clamp(shown, 2u, made_steps);
	uint32_t last = 0;
	for (uint32_t j = 1; j < shown; ++j) {
		const uint32_t want = static_cast<uint32_t>(std::lround(static_cast<double>(j) * made_steps / shown));
		last = std::clamp(want, last + 1u, made_steps - (shown - j));
		out[j - 1] = last;
	}
}

bool route_too_costly(double cost_ms, double own_ms, uint32_t made) noexcept
{
	return cost_ms > 6.0 && cost_ms > 0.4 * own_ms * static_cast<double>(std::max(1u, made));
}

void queue_spread(PacerPlan *p, double now, double last_real, double period_ms) noexcept
{
	if (p == nullptr || !p->generate || p->made == 0 || period_ms <= 0.0)
		return;
	const double start = std::max(now, last_real + period_ms);
	p->real_at = start + static_cast<double>(p->made) * period_ms;
	for (uint32_t k = 0; k < p->made; ++k)
		p->made_at[k] = start + static_cast<double>(k) * period_ms;
	p->gen_at = p->made_at[0];
}

bool queue_spreads(bool queue_fixed, bool display_paced, bool have_last_real) noexcept
{
	return queue_fixed && !display_paced && have_last_real;
}

uint32_t route_cost_made(uint32_t shown, uint32_t made_at_once) noexcept
{
	return std::max(1u, std::min(shown, made_at_once));
}

PacerPlan FramePacer::plan(double now, bool have_generated, double limit_ms, const PacerGrid &display,
	uint32_t steps) noexcept
{
	const uint32_t n = std::clamp(steps, 2u, kPacerMaxMade + 1u);
	PacerPlan p;
	const double limit = limit_ms > 0.0 ? limit_ms : 0.0;
	limit_ = limit;
	const double base = last_return_ >= 0.0 ? anchor_ : now;
	const double period = display.period_ms;
	display_paced_ = period > 0.0;
	const double ideal = ideal_real_;
	ideal_real_ = -1.0;
	if (generating_ && have_generated && display_paced_) {
		p.generate = true;
		p.made = n - 1;
		const double interval = std::max(limit, static_cast<double>(n) * period);
		const double target = (ideal >= 0.0 && std::fabs(ideal - base) < period ? ideal : base) + interval;
		const auto slot = [&](double t) {
			return display.vblank_ms + (std::round((t - display.vblank_ms) / period - kSlotPhase) + kSlotPhase) * period;
		};
		const double real_slot = slot(target);
		if (now + kSlotLeadMs >= real_slot) {
			for (uint32_t k = 0; k < p.made; ++k)
				p.made_at[k] = now;
			p.real_at = now;
		} else {
			p.real_at = real_slot;
			double at = now;
			for (uint32_t k = 1; k < n; ++k) {
				const double back = static_cast<double>(n - k);
				at = std::max(at, std::min(slot(target - back / static_cast<double>(n) * interval), real_slot - back * period));
				p.made_at[k - 1] = at;
			}
			ideal_real_ = target;
		}
		p.gen_at = p.made_at[0];
	} else if (generating_ && have_generated) {
		const double step = std::max(reference_, limit) / static_cast<double>(n);
		p.generate = true;
		p.made = n - 1;
		double at = now;
		for (uint32_t k = 1; k < n; ++k) {
			at = std::max(at, base + static_cast<double>(k) * step);
			p.made_at[k - 1] = at;
		}
		p.gen_at = p.made_at[0];
		p.real_at = std::max(p.made_at[n - 2] + step, base + limit);
	} else {
		p.gen_at = now;
		p.real_at = frame_hitched_ ? now : std::max(now, base + limit);
	}
	p.real_at = std::min(p.real_at, now + kMaxWaitMs);
	for (uint32_t k = 0; k < p.made; ++k)
		p.made_at[k] = std::min(p.made_at[k], p.real_at);
	p.gen_at = std::min(p.gen_at, p.real_at);
	last_generated_ = p.generate;
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
	if (as_fast_) {
		fast_[fast_at_] = interval;
		fast_at_ = (fast_at_ + 1) % kFastFrames;
		fast_n_ = std::min(fast_n_ + 1, kFastFrames);
		if (fast_n_ >= kFastFrames) {
			double sum = 0.0;
			for (uint32_t i = 0; i < fast_n_; ++i)
				sum += fast_[i];
			const double mean = sum / static_cast<double>(fast_n_);
			const double retry = window_route_ ? kProbeEveryMs : kRetryMs;
			if (start_probe_) {
				start_probe_ = false;
				try_from_start_ = true;
				if (mean > kStartShare * refresh_) {
					as_fast_ = try_from_start_ = false;
					own_n_ = own_at_ = 0;
				} else {
					confirmed_ = true;
				}
			} else {
				const bool proven = t - as_fast_since_ >= kProvenMs;
				if (proven)
					confirmed_ = true;
				if (mean >= (proven ? kResumeShare : kStartShare) * refresh_) {
					as_fast_ = confirmed_ = false;
					own_n_ = own_at_ = 0;
					if (!proven && !try_from_start_) {
						if (++failed_tries_ >= kMaxFailedTries)
							given_up_ = true;
						next_try_ = t + retry * static_cast<double>(failed_tries_);
					} else {
						next_try_ = t + retry;
					}
					try_from_start_ = false;
					if (++resumes_ >= kMaxResumes)
						given_up_ = true;
				}
			}
		}
	}
	if (state_ == PacerState::Measuring && window_n_ >= kStartFrames) {
		state_ = PacerState::Engaged;
		engaged_at_ = t;
		if (refresh_ > 0.0 && !given_up_ && !fixed_ && limit_ <= kFitShare * refresh_) {
			as_fast_ = start_probe_ = true;
			as_fast_since_ = t;
			fast_n_ = fast_at_ = 0;
		}
	}
}

}
