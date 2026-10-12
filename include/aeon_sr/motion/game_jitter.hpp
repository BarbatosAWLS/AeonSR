#pragma once

#include <cmath>
#include <cstdint>

namespace aeon_sr {

struct GameJitterFit {
	uint64_t serial = 0;
	bool depth_ok = false, color_ok = false;
	float depth_x = 0.0f, depth_y = 0.0f;
	float color_x = 0.0f, color_y = 0.0f;
	uint32_t depth_pixels = 0, color_pixels = 0;
	float motion_px = -1.0f;
	bool carried_depth_ok = false, carried_color_ok = false;
	float carried_depth_x = 0.0f, carried_depth_y = 0.0f;
	float carried_color_x = 0.0f, carried_color_y = 0.0f;
	uint32_t carried_depth_pixels = 0, carried_color_pixels = 0;
	float motion_x = 0.0f, motion_y = 0.0f;
};

inline bool solve_shift(const float s[6], uint32_t min_pixels, float *tx, float *ty) noexcept
{
	const double xx = s[0], xy = s[1], yy = s[2], xd = s[3], yd = s[4], n = s[5];
	if (!(n >= static_cast<double>(min_pixels)) || !std::isfinite(xx + xy + yy + xd + yd))
		return false;
	const double det = xx * yy - xy * xy;
	if (!(det > 0.04 * n * n))
		return false;
	*tx = static_cast<float>(-(yy * xd - xy * yd) / det);
	*ty = static_cast<float>(-(xx * yd - xy * xd) / det);
	return std::isfinite(*tx) && std::isfinite(*ty);
}

inline bool withhold_offset(bool absorbed, bool measured, bool jitters) noexcept
{
	return measured ? jitters : absorbed;
}

inline bool depth_withholds(bool jitters, bool lands, bool corroborated) noexcept
{
	return jitters && !lands && corroborated;
}

class GameJitterDetector {
public:
	static constexpr uint32_t kWindow = 16u;
	static constexpr uint32_t kHold = 2u;
	static constexpr float kOn = 0.12f;
	static constexpr float kOff = 0.06f;
	static constexpr float kMaxMotion = 0.5f;
	static constexpr float kMaxColorShift = 1.0f;
	static constexpr float kMaxDepthShift = 2.0f;
	static constexpr float kColorAboveMotion = 0.3f;
	static constexpr float kStillMotion = 0.15f;
	static constexpr uint32_t kTrim = 2u;
	static constexpr float kOnMoving = 0.18f;
	static constexpr float kOffMoving = 0.10f;
	static constexpr float kTurnFloor = 2.0f;
	static constexpr float kTurnShare = 0.25f;
	static constexpr float kMaxCarriedColor = 1.0f;
	static constexpr float kMaxMovingMotion = 16.0f;

	bool moving = true;

	enum class Change : uint8_t { None, StartsJittering, StopsJittering, NotJittering };

	Change feed(const GameJitterFit &f, bool same_offset, float before_x = 0.0f, float before_y = 0.0f) noexcept
	{
		const bool turn_known = f.serial != 0 && last_serial_ + 1u == f.serial;
		const float turn = std::sqrt((f.motion_x - last_mx_) * (f.motion_x - last_mx_) +
			(f.motion_y - last_my_) * (f.motion_y - last_my_));
		last_serial_ = f.serial;
		last_mx_ = f.motion_x;
		last_my_ = f.motion_y;
		const float color_len = std::sqrt(f.color_x * f.color_x + f.color_y * f.color_y);
		const bool still = f.depth_ok && f.color_ok && f.motion_px >= 0.0f && f.motion_px <= kMaxMotion &&
			color_len <= f.motion_px + kColorAboveMotion && std::fabs(f.color_x) <= kMaxColorShift &&
			std::fabs(f.color_y) <= kMaxColorShift && std::fabs(f.depth_x) <= kMaxDepthShift &&
			std::fabs(f.depth_y) <= kMaxDepthShift;
		const float mean_len = std::sqrt(f.motion_x * f.motion_x + f.motion_y * f.motion_y);
		const bool carried = moving && !still && f.motion_px > kMaxMotion && f.motion_px <= kMaxMovingMotion &&
			f.carried_depth_ok && f.carried_color_ok &&
			turn_known && turn <= std::fmax(kTurnFloor, kTurnShare * mean_len) &&
			std::fabs(f.carried_color_x) <= kMaxCarriedColor && std::fabs(f.carried_color_y) <= kMaxCarriedColor &&
			std::fabs(f.carried_depth_x) <= kMaxDepthShift && std::fabs(f.carried_depth_y) <= kMaxDepthShift;
		if (!same_offset || (!still && !carried) || !std::isfinite(before_x) || !std::isfinite(before_y)) {
			++skipped_;
			return Change::None;
		}
		rx_[count_] = still ? f.depth_x - f.color_x : f.carried_depth_x - f.carried_color_x;
		ry_[count_] = still ? f.depth_y - f.color_y : f.carried_depth_y - f.carried_color_y;
		ax_[count_] = before_x;
		ay_[count_] = before_y;
		mo_[count_] = f.motion_px;
		moved_[count_] = !still;
		if (++count_ < kWindow)
			return Change::None;
		count_ = 0;
		bool in_motion = false;
		for (uint32_t i = 0; i < kWindow; ++i)
			in_motion = in_motion || moved_[i];
		last_rms_ = window_rms(in_motion);
		last_in_motion_ = in_motion;
		++windows_;
		const float on = in_motion ? kOnMoving : kOn, off = in_motion ? kOffMoving : kOff;
		const int vote = last_rms_ >= on ? 1 : last_rms_ <= off ? -1 : 0;
		if (!measured_) {
			if (vote == 0) {
				agree_ = 0;
				return Change::None;
			}
			agree_ = vote == agree_vote_ ? agree_ + 1u : 1u;
			agree_vote_ = vote;
			if (agree_ < kHold)
				return Change::None;
			agree_ = 0;
			measured_ = true;
			confirmed_ = true;
			jitters_ = vote > 0;
			remembered_ = false;
			return jitters_ ? Change::StartsJittering : Change::NotJittering;
		}
		const int against = jitters_ ? -1 : 1;
		if (!confirmed_) {
			if (vote == 0)
				return Change::None;
			if (vote == against) {
				agree_ = 1;
				jitters_ = !jitters_;
				remembered_ = false;
				return jitters_ ? Change::StartsJittering : Change::StopsJittering;
			}
			if (++agree_ >= kHold) {
				agree_ = 0;
				confirmed_ = true;
				return jitters_ ? Change::None : Change::NotJittering;
			}
			return Change::None;
		}
		static_assert(kHold >= 2u, "one odd window never turns a confirmed verdict");
		if (vote != against) {
			streak_ = 0;
			return Change::None;
		}
		if (++streak_ < kHold)
			return Change::None;
		streak_ = 0;
		jitters_ = !jitters_;
		remembered_ = false;
		return jitters_ ? Change::StartsJittering : Change::StopsJittering;
	}

	bool jitters() const noexcept { return jitters_; }
	bool measured() const noexcept { return measured_; }
	bool confirmed() const noexcept { return confirmed_; }
	float last_rms() const noexcept { return last_rms_; }
	bool last_in_motion() const noexcept { return last_in_motion_; }
	uint32_t windows() const noexcept { return windows_; }
	uint32_t skipped() const noexcept { return skipped_; }
	void restart_window() noexcept
	{
		count_ = 0;
		streak_ = 0;
		agree_ = 0;
	}
	void reset() noexcept { *this = GameJitterDetector{}; }
	void unconfirm() noexcept
	{
		confirmed_ = false;
		agree_ = 0;
		remembered_ = false;
	}
	void seed(bool jitters) noexcept
	{
		measured_ = true;
		confirmed_ = false;
		jitters_ = jitters;
		streak_ = 0;
		agree_ = 0;
		seeded_ = true;
		remembered_ = true;
	}
	bool seeded() const noexcept { return seeded_; }
	bool remembered() const noexcept { return remembered_; }

private:
	float window_rms(bool in_motion) const noexcept
	{
		double ex[kWindow], ey[kWindow];
		double rxa = 0.0, aa = 0.0, rya = 0.0, bb = 0.0;
		float most = 0.0f;
		for (uint32_t i = 0; i < kWindow; ++i) {
			rxa += static_cast<double>(rx_[i]) * ax_[i];
			aa += static_cast<double>(ax_[i]) * ax_[i];
			rya += static_cast<double>(ry_[i]) * ay_[i];
			bb += static_cast<double>(ay_[i]) * ay_[i];
			most = mo_[i] > most ? mo_[i] : most;
		}
		const double kx = aa > 1e-3 ? rxa / aa : 0.0, ky = bb > 1e-3 ? rya / bb : 0.0;
		double mx = 0.0, my = 0.0;
		for (uint32_t i = 0; i < kWindow; ++i) {
			ex[i] = rx_[i] - kx * ax_[i];
			ey[i] = ry_[i] - ky * ay_[i];
			mx += ex[i];
			my += ey[i];
		}
		if (most <= kStillMotion) {
			mx = my = 0.0;
		} else {
			mx /= kWindow;
			my /= kWindow;
		}
		double d[kWindow];
		uint32_t n = kWindow;
		if (in_motion) {
			n = kWindow - 1u;
			for (uint32_t i = 0; i < n; ++i)
				d[i] = 0.5 * ((ex[i + 1] - ex[i]) * (ex[i + 1] - ex[i]) + (ey[i + 1] - ey[i]) * (ey[i + 1] - ey[i]));
		} else {
			for (uint32_t i = 0; i < n; ++i)
				d[i] = (ex[i] - mx) * (ex[i] - mx) + (ey[i] - my) * (ey[i] - my);
		}
		for (uint32_t t = 0; t < kTrim; ++t) {
			uint32_t at = 0;
			for (uint32_t i = 1; i < n; ++i)
				at = d[i] > d[at] ? i : at;
			d[at] = -1.0;
		}
		double e = 0.0;
		for (uint32_t i = 0; i < n; ++i)
			e += d[i] >= 0.0 ? d[i] : 0.0;
		return static_cast<float>(std::sqrt(e / (n - kTrim)));
	}

	float rx_[kWindow]{}, ry_[kWindow]{};
	float ax_[kWindow]{}, ay_[kWindow]{}, mo_[kWindow]{};
	bool moved_[kWindow]{};
	bool last_in_motion_ = false;
	uint64_t last_serial_ = 0;
	float last_mx_ = 0.0f, last_my_ = 0.0f;
	uint32_t count_ = 0;
	uint32_t streak_ = 0;
	uint32_t windows_ = 0;
	uint32_t skipped_ = 0;
	float last_rms_ = 0.0f;
	bool jitters_ = false;
	bool measured_ = false;
	bool confirmed_ = false;
	bool seeded_ = false;
	bool remembered_ = false;
	uint32_t agree_ = 0;
	int agree_vote_ = 0;
};

}
