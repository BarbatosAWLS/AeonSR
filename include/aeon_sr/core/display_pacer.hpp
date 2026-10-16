#pragma once

#include <cstdint>

namespace aeon_sr {

class DisplayPacer {
public:
	static constexpr double kLeadMs = 3.0;
	static constexpr double kLateMarginMs = 1.0;
	static constexpr double kRoomMs = 1.0;
	static constexpr double kMaxRatio = 2.5;
	static constexpr double kReleaseRatio = 2.9;
	static constexpr uint32_t kWindow = 30;
	static constexpr uint32_t kFitsToEngage = 30;
	static constexpr uint32_t kMissesToRelease = 3;
	static constexpr uint32_t kMaxBackoff = 8;
	static constexpr uint32_t kCalmFrames = 1500;
	static constexpr uint32_t kVblankRing = 16;
	static constexpr double kLateWakeMs = 2.0;
	static constexpr double kHitchPeriods = 4.0;

	double plan(double now, double period_ms, double vblank_ms) noexcept;
	void returned(double t) noexcept;
	void frame_started(double t) noexcept
	{
		if (t > last_return_)
			last_return_ = t;
	}
	void reset() noexcept { *this = DisplayPacer{}; }

	bool engaged() const noexcept { return engaged_; }
	double work_ms() const noexcept { return work_; }
	uint32_t misses() const noexcept { return missed_total_; }
	double vblank_ms() const noexcept { return vblank_; }
	void set_gpu_tail(double ms) noexcept { gpu_tail_ = ms > 0.0 ? ms : 0.0; }
	double lead_ms() const noexcept;
	double last_return_ms() const noexcept { return last_return_; }
	double next_slot() const noexcept { return engaged_ && last_slot_ >= 0.0 ? last_slot_ + period_ : -1.0; }

private:
	double estimate_vblank(double vblank_ms, double period_ms) noexcept;
	void release() noexcept;

	double last_return_ = -1.0;
	double last_slot_ = -1.0;
	double last_blank_ = -1.0;
	double planned_ = -1.0;
	double work_ = 0.0, work_p90_ = 0.0;
	double window_[kWindow] = {};
	uint32_t window_n_ = 0, window_at_ = 0;
	double vblanks_[kVblankRing] = {};
	uint32_t vblank_n_ = 0, vblank_at_ = 0;
	double vblank_ = 0.0, last_vblank_in_ = -1.0;
	double period_ = 0.0;
	double gpu_tail_ = 0.0;
	uint32_t fits_ = 0, backoff_ = 1, calm_ = 0;
	uint32_t missed_ = 0;
	uint32_t missed_total_ = 0;
	bool late_wake_ = false;
	bool engaged_ = false;
};

}
