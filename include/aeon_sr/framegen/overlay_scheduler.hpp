#pragma once

#include <cstdint>
#include <deque>

namespace aeon_sr {

struct OverlayChoice {
	enum class Kind : uint8_t {
		None = 0,
		Real,
		Generated,
		Hold,
	};
	Kind kind = Kind::None;
	uint64_t a = 0, b = 0;
	float t = 0.0f;
	uint32_t index = 0;
	bool made = false;
	double content_ms = -1.0;
	double held_ms = -1.0;
};

uint32_t auto_multiplier(double frame_ms, double period_ms, uint32_t current, uint32_t max) noexcept;

uint32_t even_cap_fps(double fps, double refresh_hz) noexcept;

bool refresh_agrees(double period_ms, double nominal_hz) noexcept;

class OverlayPace {
public:
	static constexpr double kLeadMs = 6.0;
	static constexpr double kMaxLeadShare = 0.85;
	static constexpr double kMinLeadMs = 3.0;
	static constexpr double kComposeMs = 2.0;
	static constexpr double kDeadlineMs = 1.0;
	static constexpr double kWorkWindowMs = 6000.0;
	static constexpr uint32_t kWorkRing = 1536;
	static constexpr uint32_t kWorkNeed = 16;
	static constexpr double kWorkShare = 1.0;
	static constexpr double kLeadFallMs = 0.02;
	static constexpr double kLeadSlackMs = 0.25;
	static constexpr double kLeadStepMs = 1.0;
	static constexpr uint32_t kMissWindow = 300;
	static constexpr uint32_t kMissWindowUntimed = 60;
	static constexpr uint32_t kMissesToGrow = 2;
	static constexpr uint32_t kCalmPlans = 300;
	static constexpr uint32_t kCalmPlansUntimed = 3600;
	static constexpr uint32_t kVblankRing = 16;

	double plan(double now_ms, double vblank_ms, double period_ms, double *present_at) noexcept;
	void presented() noexcept { presented_ = true; }
	void work_done(double planned_ms, double display_ms, double done_ms, bool exact = true) noexcept;
	double lead_ms() const noexcept { return lead_; }
	uint32_t misses() const noexcept { return misses_total_; }
	uint32_t late_work() const noexcept { return late_work_total_; }
	double work_ms() const noexcept { return work_q_; }
	void set_decide_early(bool early) noexcept { early_ = early; }
	void set_lead_cap(double share) noexcept
	{
		if (share > 0.1 && share < 1.0)
			lead_cap_ = share;
	}
	void set_lead(double ms) noexcept
	{
		if (ms > 0.0) {
			lead_ = ms;
			fixed_ = true;
		}
	}
	void reset() noexcept
	{
		last_ = 0.0;
		presented_ = false;
		squeezed_ = false;
		last_done_ = -1.0;
		vblank_n_ = vblank_at_ = 0;
		last_vblank_in_ = -1.0;
	}

private:
	double estimate_vblank(double vblank_ms, double period_ms) noexcept;
	void follow(double period_ms) noexcept;
	double last_ = 0.0;
	double lead_ = kLeadMs;
	bool falling_ = false;
	bool early_ = false;
	bool fixed_ = false;
	double lead_cap_ = kMaxLeadShare;
	bool presented_ = false;
	bool squeezed_ = false;
	double last_done_ = -1.0;
	struct Work {
		double planned;
		double span;
	};
	Work work_[kWorkRing] = {};
	uint32_t work_n_ = 0, work_at_ = 0;
	double work_q_ = 0.0;
	double newest_planned_ = 0.0;
	double extra_ = 0.0;
	double untimed_ = kLeadMs;
	uint8_t recent_[kMissWindow] = {};
	uint32_t recent_at_ = 0, calm_ = 0, misses_total_ = 0, late_work_total_ = 0;
	void note(bool missed, bool counted, double period_ms) noexcept;
	double vblanks_[kVblankRing] = {};
	uint32_t vblank_n_ = 0, vblank_at_ = 0;
	double last_vblank_in_ = -1.0;
};

class OverlayScheduler {
public:
	static constexpr uint32_t kKeep = 8;
	static constexpr uint32_t kIntervals = 32;
	static constexpr double kNeedWindowMs = 2000.0;
	static constexpr uint32_t kNeedSamples = 256;
	static constexpr double kGuardFloorMs = 0.5;
	static constexpr double kGuardShare = 0.2;
	static constexpr double kRoundingFraction = 0.25;
	static constexpr double kMarginMs = 1.5;
	static constexpr double kMaxLatencyMs = 150.0;
	static constexpr double kHitch = 3.0;
	static constexpr uint32_t kSlower = 8;
	static constexpr double kShrinkFraction = 0.01;
	static constexpr double kGrowFraction = 0.25;

	void reset() noexcept;

	void add(uint64_t id, double presented_ms, bool pairs_with_previous, double usable_ms, uint32_t steps = 0,
		uint32_t made = 0, bool vendor = false, double ready_slack_ms = -1.0, double ready_after_ms = -1.0) noexcept;

	OverlayChoice choose(double display_ms, double period_ms) noexcept;

	double latency_ms() const noexcept { return latency_; }
	double usable_lag_ms() const noexcept { return lag_stat(); }
	uint64_t reals_shown() const noexcept { return reals_shown_; }
	uint64_t reals_skipped() const noexcept { return reals_skipped_; }
	uint64_t generated() const noexcept { return generated_; }
	uint64_t held() const noexcept { return held_; }
	uint64_t repeated() const noexcept { return repeated_; }

private:
	struct Real {
		uint64_t id;
		double at;
		bool pairs;
		bool shown;
		uint32_t steps;
		uint32_t made;
		bool vendor;
		double shown_at;
		double usable;
	};
	double interval_stat(bool p90) const noexcept;
	double lag_stat() const noexcept;
	double desired_latency(double period_ms) const noexcept;
	double due_at(double at, double display_ms, double period_ms, double desired) const noexcept;
	static constexpr uint32_t kPlanAhead = 64;

	std::deque<Real> reals_;
	double intervals_[kIntervals] = {};
	uint32_t interval_n_ = 0, interval_at_ = 0;
	uint32_t long_run_ = 0;
	double lags_[kIntervals] = {};
	uint32_t lag_n_ = 0, lag_at_ = 0;
	struct Need {
		double at;
		double lag;
		double pair;
		double slack;
		double after;
	};
	Need needs_[kNeedSamples] = {};
	uint32_t need_n_ = 0, need_at_ = 0;
	double last_display_ = 0.0, last_period_ = 0.0;
	uint32_t skips_ = 0;
	double latency_ = -1.0;
	uint64_t reals_shown_ = 0, reals_skipped_ = 0, generated_ = 0, held_ = 0, repeated_ = 0;
	uint32_t last_index_ = 0;
	uint64_t last_index_of_ = 0;
	uint64_t plan_b_ = 0;
	double plan_at_ = 0.0, plan_from_ = 0.0, plan_t0_ = 0.0, plan_t_ = 0.0;
};

}
