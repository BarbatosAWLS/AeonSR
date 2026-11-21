#pragma once

#include <cstdint>

namespace aeon_sr {

enum class PacerState : uint8_t {
	Measuring = 0,
	Engaged,
};

const char *pacer_state_label(PacerState s) noexcept;

struct PacerGrid {
	double period_ms = 0.0;
	double vblank_ms = 0.0;
};

inline constexpr uint32_t kPacerMaxMade = 5;

class StepFit {
public:
	static constexpr uint32_t kWindow = 8;
	static constexpr double kRoomShare = 0.97;
	static constexpr double kHeldShare = 1.03;
	static constexpr uint32_t kProbeEvery = 150;

	void reset() noexcept { *this = StepFit{}; }
	uint32_t next(double interval_ms, double period_ms, uint32_t asked) noexcept;
	uint32_t steps() const noexcept { return steps_; }
	bool probing() const noexcept { return probing_; }

private:
	double median() const noexcept;
	void restart() noexcept
	{
		n_ = at_ = 0;
		since_ = 0;
	}
	uint32_t steps_ = 2;
	uint32_t base_ = 2;
	bool probing_ = false;
	double iv_[kWindow] = {};
	uint32_t n_ = 0, at_ = 0;
	uint32_t since_ = 0;
};

void pick_steps(uint32_t made_steps, uint32_t shown, uint32_t *out) noexcept;

bool route_too_costly(double cost_ms, double own_ms, uint32_t made) noexcept;

struct PacerPlan;
void queue_spread(PacerPlan *p, double now, double last_real, double period_ms) noexcept;
bool queue_spreads(bool queue_fixed, bool display_paced, bool have_last_real) noexcept;
uint32_t route_cost_made(uint32_t shown, uint32_t made_at_once) noexcept;

struct PacerPlan {
	bool generate = false;
	double gen_at = 0.0;
	double real_at = 0.0;
	uint32_t made = 0;
	double made_at[kPacerMaxMade] = {};
};

class FramePacer {
public:
	static constexpr uint32_t kStartFrames = 8;
	static constexpr uint32_t kWindow = 30;
	static constexpr double kHitch = 3.0;
	static constexpr uint32_t kHitchesToAdopt = 10;
	static constexpr double kMaxWaitMs = 100.0;
	static constexpr double kLateAnchorMs = 2.0;
	static constexpr double kSlotPhase = 0.5;
	static constexpr double kSlotLeadMs = 0.3;
	static constexpr double kAfterVblankMs = 2.0;
	static constexpr double kStartShare = 1.1;
	static constexpr double kFitShare = 0.95;
	static constexpr double kResumeShare = 1.2;
	static constexpr uint32_t kOwnFrames = 30;
	static constexpr uint32_t kFastFrames = 20;
	static constexpr double kProvenMs = 10000.0;
	static constexpr double kRetryMs = 8000.0;
	static constexpr uint32_t kMaxFailedTries = 2;
	static constexpr uint32_t kMaxResumes = 4;
	static constexpr double kRefreshHoldMs = 2000.0;
	static constexpr double kProbeEveryMs = 15000.0;
	static constexpr double kHeldLow = 1.9, kHeldHigh = 2.1;

	void reset() noexcept;

	bool begin(double now) noexcept;
	PacerPlan plan(double now, bool have_generated, double limit_ms, const PacerGrid &display = {},
		uint32_t steps = 2) noexcept;
	double after_generated(double real_at, double gen_done, const PacerGrid &display) noexcept;
	void returned(double t) noexcept;
	void set_refresh(double period_ms, double now_ms = 0.0) noexcept
	{
		if (period_ms > 0.0) {
			refresh_ = period_ms;
			refresh_seen_ = now_ms;
		} else if (now_ms - refresh_seen_ > kRefreshHoldMs) {
			refresh_ = 0.0;
		}
	}
	void set_window_route(bool on) noexcept { window_route_ = on; }
	void set_fixed(bool fixed) noexcept;
	bool fixed() const noexcept { return fixed_; }
	void note_made(bool made) noexcept { last_generated_ = made; }
	bool as_fast_as_screen() const noexcept { return as_fast_ && confirmed_; }
	bool trying() const noexcept { return as_fast_ && !start_probe_ && !confirmed_; }
	bool as_fast_given_up() const noexcept { return given_up_; }
	bool holding_off() const noexcept { return as_fast_; }
	double own_ms() const noexcept { return own_median_; }
	double refresh_ms() const noexcept { return refresh_; }

	PacerState state() const noexcept { return state_; }
	double real_interval_ms() const noexcept { return reference_; }
	bool display_paced() const noexcept { return display_paced_; }

private:
	double median(const double *v, uint32_t n) const noexcept;

	PacerState state_ = PacerState::Measuring;
	double last_return_ = -1.0;
	double anchor_ = 0.0;
	double reference_ = 0.0;
	bool frame_hitched_ = false;
	bool generating_ = false;
	bool display_paced_ = false;
	double planned_real_ = 0.0;
	double ideal_real_ = -1.0;
	bool planned_wait_ = false;

	double window_[kWindow] = {};
	uint32_t window_n_ = 0, window_at_ = 0;
	double refresh_ = 0.0;
	bool as_fast_ = false, given_up_ = false, last_generated_ = false, start_probe_ = false;
	bool window_route_ = false, try_from_start_ = false, confirmed_ = false, fixed_ = false;
	double engaged_at_ = 0.0, refresh_seen_ = 0.0, limit_ = 0.0;
	uint32_t resumes_ = 0;
	double own_[kOwnFrames] = {};
	uint32_t own_n_ = 0, own_at_ = 0;
	static_assert(kOwnFrames <= kWindow && kFastFrames <= kWindow, "median() sorts in a buffer of kWindow");
	double own_median_ = 0.0;
	double fast_[kFastFrames] = {};
	uint32_t fast_n_ = 0, fast_at_ = 0;
	double as_fast_since_ = 0.0, next_try_ = 0.0;
	uint32_t failed_tries_ = 0;
	double hitches_[kHitchesToAdopt] = {};
	uint32_t hitches_in_a_row_ = 0;
};

}
