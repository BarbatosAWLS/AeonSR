#pragma once

#include <cstdint>

namespace aeon_sr {

enum class SceneState : uint8_t {
	Moving = 1,
	Cut = 2,
};

inline constexpr float kCameraCutFloorPx = 12.0f;

bool camera_cut_detected(bool have_flow, float motion_px, float prev_motion_px) noexcept;

const char *scene_state_label(SceneState s) noexcept;

}
