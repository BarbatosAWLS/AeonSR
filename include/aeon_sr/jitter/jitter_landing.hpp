#pragma once

#include <cmath>
#include <cstdint>

namespace aeon_sr {

class JitterLanding {
public:
	static constexpr float kEvidence = 1.5f;
	static constexpr float kTurnedOver = -0.4f;
	static constexpr uint32_t kMaxTurns = 4u;

	bool update(const float sums[4]) noexcept
	{
		bool toggled = false;
		for (int a = 0; a < 2; ++a) {
			const float lm = sums[a * 2], mm = sums[a * 2 + 1];
			if (!std::isfinite(lm) || !std::isfinite(mm) || !(mm >= kEvidence))
				continue;
			factor_[a] = 1.0f - lm / mm;
			measured_[a] = true;
			if (factor_[a] < kTurnedOver && turns_ < kMaxTurns) {
				flip_[a] = !flip_[a];
				++turns_;
				toggled = true;
			}
		}
		return toggled;
	}

	bool flip_x() const noexcept { return flip_[0]; }
	bool flip_y() const noexcept { return flip_[1]; }
	float factor_x() const noexcept { return factor_[0]; }
	float factor_y() const noexcept { return factor_[1]; }
	bool measured_x() const noexcept { return measured_[0]; }
	bool measured_y() const noexcept { return measured_[1]; }
	uint32_t turns() const noexcept { return turns_; }

	void reset() noexcept { *this = JitterLanding{}; }

private:
	bool flip_[2] = { false, false };
	bool measured_[2] = { false, false };
	float factor_[2] = { 1.0f, 1.0f };
	uint32_t turns_ = 0;
};

}
