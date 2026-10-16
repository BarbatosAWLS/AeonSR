#include "aeon_sr/games/scale_driver.hpp"

#include <cmath>

namespace aeon_sr::games {

float ScaleDriver::step(const ScaleDriverInput &in, ScaleDriverEvent *event) noexcept
{
	*event = ScaleDriverEvent::None;
	++presents;
	if (in.upscaled)
		last_upscaled = presents;
	const float want = in.attached && in.upscaling ? in.mode_scale : 0.0f;
	if (want <= 0.0f) {
		if (answer > 0.0f || asked > 0.0f)
			*event = ScaleDriverEvent::HandedBack;
		answer = asked = 0.0f;
		rest_until = 0;
		mismatches = 0;
		return answer;
	}
	if (want != asked) {
		asked = want;
		answer = want;
		asked_at = presents;
		rest_until = 0;
		mismatches = 0;
		*event = ScaleDriverEvent::Asked;
		return answer;
	}
	if (want >= 1.0f) {
		answer = want;
		return answer;
	}
	if (answer <= 0.0f) {
		if (presents >= rest_until) {
			answer = want;
			asked_at = presents;
			mismatches = 0;
			*event = ScaleDriverEvent::Retried;
		}
		return answer;
	}
	if (in.stretch_ran && in.applied > 0.0f && std::fabs(in.applied - answer) > 0.002f) {
		if (++mismatches >= kMismatches) {
			answer = 0.0f;
			rest_until = presents + kRest;
			*event = ScaleDriverEvent::NotFollowed;
			return answer;
		}
	} else if (in.stretch_ran) {
		mismatches = 0;
	}
	const uint64_t since = asked_at > last_upscaled ? asked_at : last_upscaled;
	if (presents - since >= kGiveUp) {
		answer = 0.0f;
		rest_until = presents + kRest;
		*event = ScaleDriverEvent::NotTaken;
	}
	return answer;
}

bool ScaleDriver::holds_present(bool at_full) const noexcept
{
	if (answer <= 0.0f || (answer >= 1.0f && !at_full))
		return false;
	const uint64_t since = asked_at > last_upscaled ? asked_at : last_upscaled;
	return presents - since <= kHold;
}

}
