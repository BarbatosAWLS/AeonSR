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
	static constexpr float kAbsorbed = 0.4f;
	static constexpr uint32_t kAbsorbConfirm = 3u;

	enum class Verdict : uint8_t {
		None,
		Restart,
		Turned,
		Absorbed,
	};

	Verdict update(const float sums[4], bool counts_absorbed = true) noexcept
	{
		bool restart = false, toggled = false;
		int judged = 0, near_zero = 0;
		++readings_;
		if (!(sums[1] >= kEvidence) && !(sums[3] >= kEvidence))
			++short_;
		for (int a = 0; a < 2; ++a) {
			const float lm = sums[a * 2], mm = sums[a * 2 + 1];
			if (!std::isfinite(lm) || !std::isfinite(mm) || !(mm >= kEvidence))
				continue;
			const float f = 1.0f - lm / mm;
			if (!(std::fabs(f) <= kPlausible)) {
				++discarded_;
				rejected_[a] = f;
				pending_[a] = 0;
				absorb_pending_ = 0;
				restart = true;
				continue;
			}
			factor_[a] = f;
			measured_[a] = true;
			++judged;
			near_zero += std::fabs(f) < kAbsorbed ? 1 : 0;
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
		if (!counts_absorbed) {
			absorb_pending_ = 0;
		} else if (judged == 2 && near_zero == 2) {
			restart = true;
			if (++absorb_pending_ >= kAbsorbConfirm && !absorbed_) {
				absorbed_ = true;
				absorb_pending_ = 0;
				return Verdict::Absorbed;
			}
		} else if (judged > near_zero) {
			absorb_pending_ = 0;
		}
		return toggled ? Verdict::Turned : restart ? Verdict::Restart : Verdict::None;
	}

	bool absorbed() const noexcept { return absorbed_; }
	void clear_absorbed() noexcept
	{
		if (absorbed_ && takes_streak_ < kResponseConfirm)
			takes_streak_ = kResponseConfirm;
		absorbed_ = false;
		absorb_pending_ = 0;
		for (float &v : resp_)
			v = 0.0f;
		resp_pending_ = 0;
	}
	void forget_response() noexcept { takes_streak_ = part_streak_ = 0; }

	static constexpr float kResponseEvidence = 1.0f;
	static constexpr float kResponseAbsorbed = 0.3f;
	static constexpr float kResponseLands = 0.6f;
	static constexpr float kResponseMinStep = 0.1f;
	Verdict note_response(float cx, float cy, float dx, float dy, bool counts) noexcept
	{
		if (!counts || absorbed_ || !std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(dx) || !std::isfinite(dy))
			return Verdict::None;
		if (std::fabs(dx) >= kResponseMinStep) {
			resp_[0] += cx * dx;
			resp_[1] += dx * dx;
		}
		if (std::fabs(dy) >= kResponseMinStep) {
			resp_[2] += cy * dy;
			resp_[3] += dy * dy;
		}
		if (resp_[1] < kResponseEvidence || resp_[3] < kResponseEvidence)
			return Verdict::None;
		const float kx = std::fabs(resp_[0] / resp_[1]), ky = std::fabs(resp_[2] / resp_[3]);
		for (float &v : resp_)
			v = 0.0f;
		last_response_[0] = kx;
		last_response_[1] = ky;
		++responses_;
		takes_streak_ = kx < kResponseTakesOut && ky < kResponseTakesOut ? takes_streak_ + 1u : 0u;
		part_streak_ = kx < kResponseLands && ky < kResponseLands ? part_streak_ + 1u : 0u;
		if (kx < kResponseAbsorbed && ky < kResponseAbsorbed) {
			lands_ = false;
			lands_pending_ = 0;
			if (++resp_pending_ >= kResponseConfirm) {
				resp_pending_ = 0;
				absorbed_ = true;
				absorb_pending_ = 0;
				return Verdict::Absorbed;
			}
		} else if (kx > kResponseLands || ky > kResponseLands) {
			resp_pending_ = 0;
			if (++lands_pending_ >= kResponseConfirm)
				lands_ = true;
		}
		return Verdict::None;
	}
	bool lands() const noexcept { return lands_; }
	static constexpr uint32_t kResponseConfirm = 2u;
	static constexpr float kResponseTakesOut = 0.45f;
	static constexpr uint32_t kPartConfirm = 4u;
	bool takes_out() const noexcept
	{
		return absorbed_ || takes_streak_ >= kResponseConfirm || part_streak_ >= kPartConfirm;
	}
	float response_x() const noexcept { return last_response_[0]; }
	float response_y() const noexcept { return last_response_[1]; }
	uint32_t responses() const noexcept { return responses_; }
	bool note_scene(uint32_t w, uint32_t h, uint64_t depth) noexcept
	{
		if (w == scene_w_ && h == scene_h_ && depth == scene_depth_)
			return false;
		scene_w_ = w;
		scene_h_ = h;
		scene_depth_ = depth;
		clear_absorbed();
		forget_response();
		lands_ = false;
		lands_pending_ = 0;
		last_response_[0] = last_response_[1] = 1.0f;
		return true;
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
	uint32_t readings() const noexcept { return readings_; }
	uint32_t short_readings() const noexcept { return short_; }
	float rejected_x() const noexcept { return rejected_[0]; }
	float rejected_y() const noexcept { return rejected_[1]; }

	void reset() noexcept { *this = JitterLanding{}; }

private:
	bool flip_[2] = { false, false };
	bool measured_[2] = { false, false };
	float factor_[2] = { 1.0f, 1.0f };
	uint32_t turns_[2] = { 0, 0 };
	uint32_t pending_[2] = { 0, 0 };
	uint32_t absorb_pending_ = 0;
	bool absorbed_ = false;
	uint32_t scene_w_ = 0, scene_h_ = 0;
	uint64_t scene_depth_ = 0;
	uint32_t discarded_ = 0;
	uint32_t readings_ = 0, short_ = 0;
	float rejected_[2] = { 0.0f, 0.0f };
	float resp_[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	uint32_t resp_pending_ = 0;
	uint32_t lands_pending_ = 0;
	bool lands_ = false;
	uint32_t responses_ = 0;
	uint32_t takes_streak_ = 0, part_streak_ = 0;
	float last_response_[2] = { 1.0f, 1.0f };
};

}
