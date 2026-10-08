#pragma once

#include "aeon_sr/upscalers/upscaler_capture.hpp"
#include "aeon_sr/core/imgui_reshade.hpp"
#include "aeon_sr/upscalers/backend_dlss.hpp"
#include "aeon_sr/upscalers/backend_fsr.hpp"
#include "aeon_sr/upscalers/backend_vsr.hpp"
#include "aeon_sr/upscalers/backend_xess.hpp"
#include "aeon_sr/core/debug_view.hpp"
#include "aeon_sr/core/frame_clock.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/core/frame_plan.hpp"
#include "aeon_sr/jitter/scene_snapshot.hpp"
#include "aeon_sr/jitter/frame_shift.hpp"
#include "aeon_sr/core/gpu_vendor.hpp"
#include "aeon_sr/core/frame_observer.hpp"
#include "aeon_sr/jitter/hud_restore.hpp"
#include "aeon_sr/jitter/jitter_landing.hpp"
#include "aeon_sr/motion/guide_builder.hpp"
#include "aeon_sr/core/input_probes.hpp"
#include "aeon_sr/interop/interop.hpp"
#include "aeon_sr/upscalers/native_dlss.hpp"
#include "aeon_sr/ngx/ngx_dlssnr.hpp"
#include "aeon_sr/ngx/ngx_runtime.hpp"
#include "aeon_sr/ngx/ngx_runtime_d3d12.hpp"
#include "aeon_sr/depth/depth_normalize.hpp"
#include "aeon_sr/motion/optical_flow.hpp"
#include "aeon_sr/core/panel.hpp"
#include "aeon_sr/jitter/scene_jitter_hooks.hpp"
#include "aeon_sr/interop/remote_engine.hpp"
#include "aeon_sr/motion/scene_stability.hpp"
#include "aeon_sr/core/settings.hpp"

namespace aeon_sr {

class App {
public:
	explicit App(HMODULE module);

	void on_init_device(reshade::api::device *device);
	void on_destroy_device(reshade::api::device *device);
	void on_init_swapchain(reshade::api::swapchain *swapchain, bool resize);
	void on_destroy_swapchain(reshade::api::swapchain *swapchain, bool resize);
	void on_begin_effects(
		reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list,
		reshade::api::resource_view rtv,
		reshade::api::resource_view rtv_srgb);
	void on_finish_effects(
		reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list,
		reshade::api::resource_view rtv,
		reshade::api::resource_view rtv_srgb);
	void on_reloaded_effects(reshade::api::effect_runtime *runtime);
	void on_present(reshade::api::effect_runtime *runtime);

	void run_frame(reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list, reshade::api::resource_view rtv, bool after_effects);

	void draw_overlay(reshade::api::effect_runtime *runtime);
	void draw_osd(reshade::api::effect_runtime *runtime);

	const GpuInfo &gpu() const noexcept { return gpu_; }

	Settings &settings() noexcept { return settings_; }
	const Settings &settings() const noexcept { return settings_; }

	const NeuralRenderCommon &neural() const noexcept;

	bool neural_runtime_present() noexcept;

	UpscalerStatus active_status() const noexcept;
	const std::wstring &active_error() const noexcept;

	void request_reset() noexcept { history_.reset_pending = true; history_.temporal_history_valid = false; }
	void persist_settings();
	void shutdown();

private:
	HMODULE module_ = nullptr;
	std::wstring ini_path_;
	Settings settings_;
	NgxRuntime ngx_;
	NgxRuntimeD3D12 ngx12_;
	NeuralRenderD3D12 neural12_;
	DlssD3D11Backend dlss11_;
	DlssD3D12Backend dlss12_;
	Fsr31D3D11Backend fsr11_;
	Fsr4D3D12Backend fsr12_;
	XessD3D12Backend xess12_;

	VsrD3D11Backend vsr11_;
	UpscalerBackend *active_ = nullptr;

	FrameHistory history_;
	FramePlan last_plan_;
	FrameClock clock_;
	reshade::api::device_api last_api_ = reshade::api::device_api::d3d11;

	EngineDevice engine_;
	RemoteEngine remote_;
	bool remote_tried_ = false;
	uint32_t remote_out_width_ = 0, remote_out_height_ = 0;
	uint32_t remote_in_width_ = 0, remote_in_height_ = 0;
	bool remote_wanted() const noexcept;
	bool runs_remote(const UpscalerBackend *backend) const noexcept;
	bool dlss_needs_host() const noexcept;
	void ensure_remote();
	void run_remote_engine_work(reshade::api::effect_runtime *runtime, const FrameInputs &inputs,
		const FramePlan &plan, UpscalerBackend *backend, const UpscalerParams &params,
		bool neural_here);
	BackendChoice choice_of(const UpscalerBackend *backend) const noexcept;
	BackendAvailability availability_of(UpscalerBackend *backend, BackendChoice choice);
	FrameBridge *bridge_ = nullptr;
	std::wstring bridge_error_;
	reshade::api::device *bridge_device_ = nullptr;
	bool building_bridge_ = false;
	PendingDevices pending_devices_;
	uint32_t swapchain_inits_ = 0;
	bool is_engine_device(reshade::api::device *device) const noexcept;
	void ensure_bridge(reshade::api::device *device);
	void adopt_device(reshade::api::device *device);
	void release_bridge(BridgeRelease how);
	void run_effects_frame(reshade::addon_event event, reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list, reshade::api::resource_view rtv);
	SharedPlane flow_game_motion_;
	SharedPlane flow_game_confidence_;
	SharedPlane flow_game_global_;
	void publish_flow_to_game(FrameInputs &inputs);
	bool finish_frame(reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list, const FrameInputs &inputs);

	GpuInfo gpu_;

	NativeDlss native_dlss_;
	bool native_dlss_seen_ = false;
	void update_native_dlss();

	NeuralColorSpace color_space_ = NeuralColorSpace::Sdr;

	AddonPipelines pipelines_;
	GuideBuilder guides_;
	DebugView debug_view_;

	bool last_bias_mask_bound_ = false;

	int neural_last_ = 0;

	bool neural_init_failed_ = false;

	bool neural_teardown_pending_ = false;
	void service_neural_teardown();

	void normalize_depth(FrameInputs &inputs);
	void refresh_depth_convention(reshade::api::effect_runtime *runtime);
	void run_internal_flow(reshade::api::effect_runtime *runtime, FrameInputs &inputs);

	JitterDriver jitter_{ scene_jitter() };
	const char *jitter_note_ = "";
	bool jitter_pause_logged_ = false;
	void arm_scene_jitter(reshade::api::effect_runtime *runtime, const FrameInputs &inputs);
	bool active_render_size(const FrameInputs &inputs, uint32_t *w, uint32_t *h);

	FrameInputs last_inputs_;
	OpticalFlowD3D12 flow_;
	DepthNormalizeD3D12 depth_;
	DepthConvention depth_how_;
	ResolvedDepthConvention depth_resolved_;
	bool depth_how_read_ = false;
	bool depth_failed_ = false;
	bool flow_failed_ = false;
	std::wstring flow_note_;
	bool flow_note_benign_ = false;
	bool motion_wanted_ = false;
	std::wstring logged_flow_note_;
	uint64_t last_flow_frames_ = 0;
	void note_flow_state();
	void note_frame_blocked(const std::wstring &why);
	std::wstring flow_error_;
	InputProbes probes_;

	void fill_uncovered_edges(FrameInputs &inputs);
	ViewportClip viewport_clip() const noexcept;
	FrameShiftD3D12 frame_shift_;
	HudRestoreD3D12 hud_restore_;
	FrameObserverD3D12 observer_;
	bool observed_ = false;
	bool hud_detected_ = false;
	bool interface_split_ = false;
	unsigned long long census_logged_at_ = 0;
	SplitGate split_gate_;
	uint32_t seen_drawn_ = 0, seen_empty_ = 0, seen_other_ = 0, seen_changes_ = 0, seen_split_ = 0;
	int seen_last_ = -1;
	void detect_interface(const FrameInputs &inputs);
	UpscalerCaptureD3D12 upscaler_capture_;
	std::wstring upscaler_capture_logged_;
	std::wstring scope_logged_;

	static constexpr uint32_t kTraceRing = 512u;
	struct TraceArm {
		bool made = false;
		uint32_t index = 0;
		float point_x = 0.0f, point_y = 0.0f;
		float x = 0.0f, y = 0.0f;
		bool fault = false;
	};
	std::vector<ScopeTraceRow> trace_ring_;
	uint64_t trace_presents_ = 0;
	ScopeTraceRow trace_cur_;
	bool trace_open_ = false;
	uintptr_t trace_runtime_ = 0;
	uint64_t engine_frames_ = 0;
	TraceArm trace_earned_, trace_armed_;
	bool told_prev_set_ = false;
	float told_prev_x_ = 0.0f, told_prev_y_ = 0.0f;
	JitterFault fault_;
	std::string fault_text_ = "none";
	static constexpr uint32_t kFaultedSerials = 8u;
	uint64_t faulted_serials_[kFaultedSerials]{};
	uint32_t faulted_next_ = 0;
	bool fault_read_ = false;
	bool scope_list_lost_ = false;
	uint64_t trace_t0_ = 0;
	void trace_begin_row();
	void trace_close();
	uint32_t fault_clock(uint32_t ahead) const noexcept;
	std::string scope_header();
	void request_scope_capture();
	uint32_t jitter_scene_w_ = 0, jitter_scene_h_ = 0;
	uint32_t depth_scene_w_ = 0, depth_scene_h_ = 0;
	uint32_t depth_scene_dw_ = 0, depth_scene_dh_ = 0;
	uint32_t learned_scene_w_ = 0, learned_scene_h_ = 0;
	uint32_t learned_scene_dw_ = 0, learned_scene_dh_ = 0;
	bool move_all_ = false;
	JitterLanding landing_;
	BlitPipelineD3D12 frame_shift_blit_;
	Gpu12Fence frame_shift_fence_;
	StageTimings stage_times_;
	void run_engine_passes(reshade::api::effect_runtime *runtime, const FrameInputs &inputs,
		const FramePlan &plan, UpscalerBackend *backend, const UpscalerParams &params);
	void run_engine_work(reshade::api::effect_runtime *runtime, const FrameInputs &inputs,
		const FramePlan &plan, UpscalerBackend *backend, const UpscalerParams &params);
	void run_neural_render(reshade::api::effect_runtime *runtime,
		const FrameInputs &inputs, const NeuralPlan &neural,
		float depth_jitter_u, float depth_jitter_v);
	void shutdown_neural();

	void request_neural_capture();
	void request_upscaler_capture();

	PanelState panel_state(bool for_overlay);
	void apply_panel_actions(const PanelActions &actions);
	void note_state();
	void write_report(const PanelState &state, bool to_clipboard);
	std::wstring report_path_;
	std::wstring report_error_;
	void invalidate_temporal_history() noexcept;
	const UpscalerBackend &backend_for_api(reshade::api::device_api api) const noexcept;
	bool active_is_dlss() const noexcept;
	bool active_is_fsr() const noexcept;
	bool active_is_xess() const noexcept;
	bool active_is_vsr() const noexcept;
};

App *app() noexcept;
void create_app(HMODULE module);
void destroy_app();

}
