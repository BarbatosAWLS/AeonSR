#pragma once

#include <cstdint>

namespace aeon_sr {

class OverlayCover {
public:
	static constexpr double kCheckMs = 250.0;
	static constexpr uint32_t kChecks = 12;
	static constexpr uint32_t kUnder = 10;

	void reset() noexcept;
	bool due(double now) const noexcept;
	bool checked(double now, bool above) noexcept;

	bool covered() const noexcept { return covered_; }
	uint64_t raised() const noexcept { return raised_; }

private:
	double next_ = 0.0;
	bool under_[kChecks] = {};
	uint32_t at_ = 0, n_ = 0;
	uint64_t raised_ = 0;
	bool covered_ = false;
};

}
