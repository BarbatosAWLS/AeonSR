#pragma once

#include <cstdint>

namespace aeon_sr::games {

struct ScaleDriverInput {
	bool attached = false;
	bool upscaling = false;
	float mode_scale = 1.0f;
	bool stretch_ran = false;
	bool upscaled = false;
	float applied = 0.0f;
};

enum class ScaleDriverEvent : uint8_t {
	None,
	Asked,
	HandedBack,
	NotTaken,
	NotFollowed,
	Retried,
};

struct ScaleDriver {
	static constexpr uint32_t kGiveUp = 300;
	static constexpr uint32_t kRest = 600;
	static constexpr uint32_t kMismatches = 60;
	static constexpr uint32_t kHold = 30;

	uint64_t presents = 0;
	uint64_t asked_at = 0;
	uint64_t last_upscaled = 0;
	uint64_t rest_until = 0;
	uint32_t mismatches = 0;
	float answer = 0.0f;
	float asked = 0.0f;

	float step(const ScaleDriverInput &in, ScaleDriverEvent *event) noexcept;
	bool holds_present(bool at_full) const noexcept;
};

}
