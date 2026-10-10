#pragma once

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/core/gpu_vendor.hpp"
#include "aeon_sr/ngx/neural_params.hpp"
#include "aeon_sr/motion/scene_stability.hpp"
#include "aeon_sr/interop/interop.hpp"
#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aeon_sr {

struct PanelState {

	bool d3d12 = false;

	GpuVendor gpu_vendor = GpuVendor::Unknown;
	uint32_t gpu_vendor_id = 0;
	std::wstring gpu_name;
	const char *active_name = nullptr;
	std::string upscaler_label;
	UpscalerStatus active_status = UpscalerStatus::Idle;
	std::wstring active_error;
	bool active_is_dlss = false;
	bool active_is_fsr = false;

	bool native_dlss = false;
	std::wstring native_dlss_modules;
	std::wstring ngx_layer;
	bool ngx_layer_relevant = false;
	bool ngx_layer_nvidia = false;
	bool active_is_xess = false;
	bool active_is_vsr = false;
	bool active_on12 = false;
	const char *bridge_name = nullptr;
	const char *bridge_sync = nullptr;
	const char *api_name = nullptr;
	BridgeKind bridge_kind = BridgeKind::None;
	std::wstring bridge_error;
	bool vsr_possible = false;

	bool bridge_any_ready = false;
	bool bridge_fsr_adapter_matched = false;

	bool upscaler_dll_present = false;
	bool upscalers_remote = false;
	bool host_ready = false;
	std::wstring host_error;
	uint32_t host_pid = 0;
	bool upscaler_initialized = false;
	uint32_t upscaler_out_width = 0, upscaler_out_height = 0;
	uint32_t upscaler_render_width = 0, upscaler_render_height = 0;
	bool upscaler_bridge_ready = false;
	bool upscaler_bridge_adapter_matched = false;

	bool vsr_capable = false;
	bool vsr_enabled = false;
	bool vsr_in_use = false;
	uint32_t vsr_level = 0;

	std::wstring fsr_provider_version;
	std::vector<std::string> fsr_providers;
	bool fsr_dx12_dlls_present = false;
	std::wstring fsr11_loaded_dll_name;
	std::wstring game_fsr_modules;
	std::wstring game_xess_modules;
	std::wstring game_ngx_modules;

	bool ngx_dll_present = false;
	bool ngx_initialized = false;
	uint32_t ngx_width = 0, ngx_height = 0;
	uint32_t ngx_render_width = 0, ngx_render_height = 0;
	uint64_t ngx_eval_count = 0;
	uint32_t ngx_upscale_mode = 0;
	uint32_t ngx_render_preset = 0;

	bool neural_dll_found = false;
	bool neural_dll_present = false;
	bool neural_shim_present = false;
	bool neural_initialized = false;
	bool neural_crashed = false;
	bool neural_driver_too_old = false;
	UpscalerStatus neural_status = UpscalerStatus::Idle;
	std::wstring neural_error;
	uint32_t neural_driver_major = 0, neural_driver_minor = 0;
	uint32_t neural_required_driver_major = 0, neural_required_driver_minor = 0;
	bool neural_requirement_known = false;
	bool neural_adapter_unsupported = false;
	uint32_t neural_min_architecture = 0;
	uint32_t neural_gpu_architecture = 0;
	std::string neural_runtime_kernels;
	bool neural_runtime_serves_card = true;
	bool neural_allgpu_build_targets = false;
	std::wstring neural_runtime_file;
	std::string neural_runtime_version;
	bool neural_runtime_certificate = false;
	std::string neural_runtime_sha256;
	std::string neural_runtime_cards;
	std::vector<std::wstring> neural_runtime_candidates;
	uint32_t neural_runtime_minimum = 0;
	uint32_t neural_reported_architecture = 0;
	bool neural_feature_unsupported = false;
	uint64_t neural_eval_count = 0;
	int neural_last = 0;
	bool neural_init_failed = false;
	bool neural_via_bridge = false;

	NeuralColorSpace neural_color_space = NeuralColorSpace::Sdr;
	uint32_t neural_model_width = 0, neural_model_height = 0;
	uint32_t neural_passes_built = 0;
	float neural_gpu_ms = 0.0f;
	uint32_t neural_eval_rows = 0;
	float neural_area_share = 0.0f;
	float frame_time_ms = 0.0f;

	std::wstring neural_capture_status;

	bool accum_running = false;
	bool accum_holding = false;
	uint32_t accum_done = 0;
	uint32_t accum_target = 0;

	MotionProvider motion_provider = MotionProvider::None;
	std::wstring flow_note;
	bool flow_note_benign = false;
	bool motion_wanted = false;
	DepthProvider depth_provider = DepthProvider::None;
	bool depth_reversed = false;
	bool depth_logarithmic = false;
	bool depth_upside_down = false;
	bool depth_mirrored = false;
	float depth_far_plane = 0.0f;
	std::string depth_overridden;
	bool have_global_flow = false;
	bool have_motion_confidence = false;
	TextureInfo color_info;
	TextureInfo motion_info;
	TextureInfo depth_info;

	bool probe_unsupported = false;
	bool probe_valid = false;
	bool probe_moving = false;
	float probe_nonzero_pct = 0.0f;
	float probe_mean_px = 0.0f;
	float probe_max_px = 0.0f;

	SceneState scene_state = SceneState::Moving;
	bool reset_this_frame = false;
	bool camera_cut = false;
	bool jitter_active = false;
	bool jitter_refused = false;
	bool jitter_in_frame = false;
	bool jitter_mixed = false;
	bool jitter_still = false;
	uint32_t jitter_moved_draws = 0;
	uint32_t jitter_plain_draws = 0;
	uint32_t jitter_elsewhere_draws = 0;
	uint32_t jitter_aliased_draws = 0;
	uint32_t jitter_replayed_draws = 0;
	uint32_t jitter_held_lists = 0;
	const char *jitter_note = "";
	bool scope_have = false;
	bool scope_computed_known = false, scope_drawn_known = false, scope_told_known = false;
	float scope_computed_x = 0.0f, scope_computed_y = 0.0f;
	float scope_drawn_x = 0.0f, scope_drawn_y = 0.0f;
	float scope_told_x = 0.0f, scope_told_y = 0.0f;
	uint64_t scope_armed_serial = 0, scope_drawn_serial = 0;
	std::string scope_plan;
	uint32_t scope_second_frames = 0, scope_lag_frames = 0, scope_empty_frames = 0;
	std::vector<float> scope_residual_x, scope_residual_y;
	bool scope_capture_enabled = false;
	std::wstring scope_status;
	std::string scope_fault;
	float last_motion_px = 0.0f;
	int debug_view_stage = 0;
	const char *debug_view_note = "";
	bool bias_mask_bound = false;

	const char *build_stamp = "";
	const char *version = "";

	std::wstring report_path;
	std::wstring report_error;
	std::wstring log_path;
};

struct PanelActions {
	bool persist = false;
	bool save_report = false;
	bool copy_report = false;
	bool reset = false;
	bool invalidate_history = false;
	bool neural_restart = false;
	bool neural_capture = false;
	bool accum_start = false;
	bool accum_stop = false;
	bool probe_reset = false;
	bool scope_capture = false;
};

namespace panel {

const char *format_label(uint32_t fmt);

void draw_overlay(const PanelState &state, Settings &settings, PanelActions &actions);

void draw_osd(const PanelState &state, const Settings &settings);

}

}
