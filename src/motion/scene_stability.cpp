#include "aeon_sr/motion/scene_stability.hpp"

#include <algorithm>
#include <cmath>

namespace aeon_sr {

bool camera_cut_detected(bool have_flow, float motion_px, float prev_motion_px) noexcept
{
	if (!have_flow)
		return false;
	const float motion = (std::max)(0.0f, motion_px);
	const float prev = (std::max)(0.0f, prev_motion_px);
	const float jump = std::fabs(motion - prev);
	return jump > kCameraCutFloorPx && jump > (std::max)(prev, motion) * 0.5f;
}

const char *scene_state_label(SceneState s) noexcept
{
	switch (s) {
	case SceneState::Moving: return "moving";
	case SceneState::Cut: return "cut";
	}
	return "unknown";
}

}
