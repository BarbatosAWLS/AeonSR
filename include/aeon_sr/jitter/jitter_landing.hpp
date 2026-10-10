#pragma once

#include <cmath>
#include <cstdint>

namespace aeon_sr {

class JitterLanding {
public:
	static constexpr float kEvidence = 1.5f;
	static constexpr float kTurnedOver = -0.4f;
	static constexpr uint32_t kMaxTurns = 2u;
	static constexpr float kPlausible = 1.6f;
	static constexpr uint32_t kConfirm = 2u;

	enum class Verdict : uint8_t {
		None,
		Restart,
		Turned,
	};

	Verdict update(const float sums[4]) noexcept
	{
		bool restart = false, toggled = false;
		for (int a = 0; a < 2; ++a) {
			const float lm = sums[a * 2], mm = sums[a * 2 + 1];
			if (!std::isfinite(lm) || !std::isfinite(mm) || !(mm >= kEvidence))
				continue;
			const float f = 1.0f - lm / mm;
			if (!(std::fabs(f) <= kPlausible)) {
				++discarded_;
				rejected_[a] = f;
				pending_[a] = 0;
				restart = true;
				continue;
			}
			factor_[a] = f;
			measured_[a] = true;
			if (f >= kTurnedOver) {
				pending_[a] = 0;
				continue;
			}
			restart = true;
			if (++pending_[a] < kConfirm || turns_[a] >= kMaxTurns)
				continue;
			pending_[a] = 0;
			flip_[a] = !flip_[a];
			++turns_[a];
			toggled = true;
		}
		return toggled ? Verdict::Turned : restart ? Verdict::Restart : Verdict::None;
	}

	bool flip_x() const noexcept { return flip_[0]; }
	bool flip_y() const noexcept { return flip_[1]; }
	float factor_x() const noexcept { return factor_[0]; }
	float factor_y() const noexcept { return factor_[1]; }
	bool measured_x() const noexcept { return measured_[0]; }
	bool measured_y() const noexcept { return measured_[1]; }
	uint32_t turns() const noexcept { return turns_[0] + turns_[1]; }
	uint32_t turns_x() const noexcept { return turns_[0]; }
	uint32_t turns_y() const noexcept { return turns_[1]; }
	uint32_t discarded() const noexcept { return discarded_; }
	float rejected_x() const noexcept { return rejected_[0]; }
	float rejected_y() const noexcept { return rejected_[1]; }

	void reset() noexcept { *this = JitterLanding{}; }

private:
	bool flip_[2] = { false, false };
	bool measured_[2] = { false, false };
	float factor_[2] = { 1.0f, 1.0f };
	uint32_t turns_[2] = { 0, 0 };
	uint32_t pending_[2] = { 0, 0 };
	uint32_t discarded_ = 0;
	float rejected_[2] = { 0.0f, 0.0f };
};

}
