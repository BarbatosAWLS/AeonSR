#pragma once

#include "aeon_sr/jitter/jitter.hpp"
#include "aeon_sr/ngx/neural_params.hpp"
#include "aeon_sr/motion/scene_stability.hpp"
#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <cstdint>

namespace aeon_sr {

struct FrameSignals {
	bool have_motion_vectors = false;
	bool have_depth = false;
	bool have_global_flow = false;
	bool have_motion_confidence = false;
	bool depth_sense_known = false;

	bool jitter_drawn = false;
	uint32_t jitter_drawn_index = 0;
	float jitter_drawn_x = 0.0f, jitter_drawn_y = 0.0f;
	bool jitter_drawn_ambiguous = false;
	bool jitter_drawn_empty = false;

	NeuralColorSpace color_space = NeuralColorSpace::Sdr;
};

struct BackendSlots {
	bool is_d3d12 = false;

	GpuVendor vendor = GpuVendor::Unknown;
	BackendAvailability dlss;
	BackendAvailability fsr;
	BackendAvailability xess;

	BackendAvailability vsr;

	bool fsr_bridge_failed = false;
	BackendAvailability fsr11;
};

struct FrameHistory {
	bool reset_pending = true;
	bool temporal_history_valid = false;
	float last_motion_px = 0.0f;
	float prev_motion_px = 0.0f;

	uint32_t jitter_index = 0;

};

struct NeuralPlan {
	bool run = false;
	bool want_motion = true;
	bool want_depth = true;
	NeuralRenderParams params;
};

enum class UpscalerOutcome {
	NoMotionVectors,
	NoBackend,
	Run,
};

enum class JitterDecision : uint8_t {
	Off,
	Drawn,
	Split,
	Ambiguous,
	Empty,
	Resampled,
};
const char *jitter_decision_name(JitterDecision d) noexcept;

struct FramePlan {
	UpscalerOutcome outcome = UpscalerOutcome::NoMotionVectors;
	int backend_index = -1;
	bool use_fsr11 = false;
	bool no_upscaler_wanted = false;

	UpscalerParams params;
	bool camera_cut = false;
	bool jitter_active = false;
	JitterDecision jitter_decision = JitterDecision::Off;
	float split_shift_x = 0.0f, split_shift_y = 0.0f;
	SceneState scene_state = SceneState::Moving;
	bool invalidate_temporal_history = false;

	NeuralPlan neural;
	FrameHistory next;
};

Jitter split_pair_point(uint32_t drawn_index, float amount) noexcept;

bool split_uses_catmull_rom(bool setting, BackendChoice backend) noexcept;

FramePlan plan_frame(const Settings &settings, const FrameSignals &signals, const BackendSlots &slots,
	const FrameHistory &history, float frame_time_ms) noexcept;

NeuralRenderParams plan_neural_params(const Settings &settings, const FrameSignals &signals, bool reset,
	float frame_time_ms = 0.0f) noexcept;

void plan_vsr_params(const Settings &settings, const FrameSignals &signals, UpscalerParams &params) noexcept;

}
