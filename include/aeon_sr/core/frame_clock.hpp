#pragma once

namespace aeon_sr {

class FrameClock {
public:
	void tick() noexcept;

	void advance(long long now) noexcept;

	float frame_time_ms() const noexcept { return interval_ms_; }

	void set_frequency(long long ticks_per_second) noexcept { freq_ = ticks_per_second; }

private:
	static constexpr double kMaxIntervalMs = 1000.0;

	long long freq_ = 0;
	long long last_ = 0;
	float interval_ms_ = 0.0f;
};

}
