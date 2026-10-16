#pragma once

#include <cstdint>

namespace aeon_sr {

class LatencyGovernor {
public:
	static constexpr double kMarginMs = 1.0;
	static constexpr double kTargetHoldMs = 0.75;
	static constexpr double kGain = 0.35;
	static constexpr uint32_t kWindow = 32;
	static constexpr uint32_t kBlockWindow = 9;
	static constexpr double kSlowdown = 1.12;
	static constexpr uint32_t kCooldownFrames = 120;
	static constexpr double kMaxWaitMs = 50.0;
	static constexpr double kMissRatio = 1.4;
	static constexpr double kMissStepMs = 1.0;
	static constexpr uint32_t kMissesToCool = 4;
	static constexpr double kBoundShare = 0.7;
	static constexpr double kProbeHoldMs = 0.25;
	static constexpr uint32_t kProbeWindow = 12;
	static constexpr uint32_t kProbeEvery = 4;
	static constexpr double kProbeStepMs = 0.2;
	static constexpr double kProbeRise = 0.012;
	static constexpr double kProbeBackOffMs = 0.6;
	static constexpr uint32_t kRestFrames = 120;

	double frame_start(double now, double paced_slot) noexcept;
	void started(double t) noexcept;
	void ready(double t) noexcept;
	void sent(double t) noexcept;
	void reset() noexcept { *this = LatencyGovernor{}; }

	double wait_ms() const noexcept { return last_wait_; }
	double work_p90_ms() const noexcept { return work_p90_; }
	double hold_ms() const noexcept { return hold_med_; }
	uint32_t cooldowns() const noexcept { return cooldowns_; }

private:
	static double median(const double *v, uint32_t n) noexcept;
	static double percentile(const double *v, uint32_t n, uint32_t num, uint32_t den) noexcept;
	double recent_interval() const noexcept;

	double started_ = -1.0, sent_ = -1.0, last_sent_ = -1.0;
	double work_[kWindow] = {};
	uint32_t work_n_ = 0, work_at_ = 0;
	double work_p90_ = 0.0;
	double intervals_[kWindow] = {};
	uint32_t interval_n_ = 0, interval_at_ = 0;
	double holds_[kBlockWindow] = {};
	uint32_t hold_n_ = 0, hold_at_ = 0;
	double hold_med_ = 0.0, hold_min_ = 0.0;
	double last_interval_ = 0.0;
	uint32_t misses_ = 0;
	double work_med_ = 0.0;
	bool probing_ = false;
	double probe_base_ = 0.0;
	uint32_t probe_count_ = 0, rest_ = 0;
	double wait_ = 0.0, last_wait_ = 0.0;
	double interval_at_start_ = 0.0;
	uint32_t cooldown_ = 0, cooldowns_ = 0;
};

}
