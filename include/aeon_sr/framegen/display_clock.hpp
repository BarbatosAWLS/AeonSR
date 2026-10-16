#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace aeon_sr {

double qpc_ms() noexcept;

struct DisplayGrid {
	bool valid = false;
	double period_ms = 0.0;
	double vblank_ms = 0.0;
};

class DisplayClock {
public:
	~DisplayClock();

	void watch(HWND window);
	void stop() noexcept;

	DisplayGrid grid() const;
	double measured_period_ms() const;
	double spread() const;

private:
	void run(HMONITOR monitor);

	HMONITOR monitor_ = nullptr;
	std::thread thread_;
	std::atomic<bool> running_{ false };
	static constexpr uint64_t kRetryMs = 2000;
	std::atomic<uint64_t> ended_tick_{ 0 };
	mutable std::mutex mutex_;
	static constexpr uint32_t kRing = 64;
	double times_[kRing] = {};
	uint32_t count_ = 0;
	DisplayGrid grid_;
	double period_ = 0.0, spread_ = 0.0;
};

}
