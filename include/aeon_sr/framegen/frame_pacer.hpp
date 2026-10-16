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

struct PacerPlan {
	bool generate = false;
	double gen_at = 0.0;
	double real_at = 0.0;
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

	void reset() noexcept;

	bool begin(double now) noexcept;
	PacerPlan plan(double now, bool have_generated, double limit_ms, const PacerGrid &display = {}) noexcept;
	double after_generated(double real_at, double gen_done, const PacerGrid &display) noexcept;
	void returned(double t) noexcept;

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
	double hitches_[kHitchesToAdopt] = {};
	uint32_t hitches_in_a_row_ = 0;
};

}
