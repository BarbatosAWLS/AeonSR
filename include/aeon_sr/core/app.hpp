#pragma once

#include "aeon_sr/upscalers/upscaler_capture.hpp"
#include "aeon_sr/core/imgui_reshade.hpp"
#include "aeon_sr/core/present_drive.hpp"

#include <algorithm>
#include <mutex>
#include <vector>
#include "aeon_sr/upscalers/backend_dlss.hpp"
#include "aeon_sr/upscalers/backend_fsr.hpp"
#include "aeon_sr/upscalers/backend_vsr.hpp"
#include "aeon_sr/upscalers/backend_xess.hpp"
#include "aeon_sr/core/debug_view.hpp"
#include "aeon_sr/core/frame_clock.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/core/frame_plan.hpp"
#include "aeon_sr/core/flight_recorder.hpp"
#include "aeon_sr/core/report_bundle.hpp"
#include "aeon_sr/jitter/scene_snapshot.hpp"
#include "aeon_sr/jitter/frame_shift.hpp"
#include "aeon_sr/core/gpu_vendor.hpp"
#include "aeon_sr/core/frame_observer.hpp"
#include "aeon_sr/core/game_render_scale.hpp"
#include "aeon_sr/games/game_integration.hpp"
#include "aeon_sr/games/scale_driver.hpp"
#include "aeon_sr/jitter/hud_restore.hpp"
#include "aeon_sr/jitter/jitter_landing.hpp"
#include "aeon_sr/jitter/picture_hold.hpp"
#include "aeon_sr/motion/game_jitter_memory.hpp"
#include "aeon_sr/motion/game_jitter_probe.hpp"
#include "aeon_sr/motion/guide_builder.hpp"
#include "aeon_sr/core/input_probes.hpp"
#include "aeon_sr/interop/interop.hpp"
#include "aeon_sr/upscalers/native_dlss.hpp"
#include "aeon_sr/interop/engine_route.hpp"
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
#include "aeon_sr/core/display_pacer.hpp"
#include "aeon_sr/core/latency_governor.hpp"
#include "aeon_sr/core/reflex.hpp"
#if AEONSR_FRAME_GENERATION
#include "aeon_sr/framegen/display_clock.hpp"
#endif
#if AEONSR_FRAME_GENERATION
#include "aeon_sr/framegen/frame_generation.hpp"
#endif

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
	void on_init_effect_runtime(reshade::api::effect_runtime *runtime);
	void on_destroy_effect_runtime(reshade::api::effect_runtime *runtime);
	void on_game_present(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain);
	void on_finish_present(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain);

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

	void persist_settings();
	void shutdown();
	void at_process_exit() noexcept
	{
		keep_game_jitter_memory();
		flight_.dump_at_exit();
	}

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
	unsigned int latency_mode_now() const noexcept;
	static double latency_clock_ms() noexcept;
	void latency_apply(unsigned int mode, reshade::api::swapchain *swapchain);
	void latency_wait_until(double t_ms);
	void latency_queue(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain);
	void latency_queue_off() noexcept;
	void *latency_dxgi_ = nullptr;
	uint32_t latency_dxgi_was_ = 0;
	LatencyGovernor latency_;
	Reflex reflex_;
	void *latency_hwnd_ = nullptr;
	uint64_t latency_hwnd_tick_ = 0;
	bool latency_presented_ = false;
	bool latency_bridged_ = false;
	bool reflex_game_own_ = false;
	bool reflex_waiting_said_ = false;
	unsigned int latency_applied_ = 0;
	bool reflex_refused_ = false;
	uint64_t reflex_frame_ = 1;
	void *latency_timer_ = nullptr;
	struct LatencyWindow {
		uint64_t since = 0;
		uint32_t frames = 0, waited_frames = 0;
		double waited = 0.0;
	} latency_window_;
#if AEONSR_FRAME_GENERATION
	void pace_to_display(reshade::api::effect_runtime *runtime);
	void pace_off() noexcept;
	void pace_gpu_tail(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain);
	void pace_tail_release() noexcept;
	struct GpuDone {
		void *event = nullptr, *wait = nullptr;
		uint64_t value = 0;
		double sent_ms = 0.0;
		std::atomic<double> done_ms{ -1.0 };
	};
	static constexpr uint32_t kGpuDoneRing = 4;
	GpuDone pace_done_[kGpuDoneRing];
	void *pace_fence_ = nullptr, *pace_fence_device_ = nullptr;
	uint64_t pace_fence_value_ = 0;
	double pace_tails_[32] = {};
	uint32_t pace_tail_n_ = 0, pace_tail_at_ = 0;
	DisplayPacer display_pacer_;
	DisplayClock pace_clock_;
	uintptr_t pace_runtime_ = 0;
	uint64_t pace_runtime_tick_ = 0;
	void *pace_timer_ = nullptr;
	struct PaceWindow {
		uint64_t since = 0;
		uint32_t presents = 0, paced = 0, late = 0, misses_at = 0;
		double waited = 0.0;
	} pace_window_;
#endif
	reshade::api::device_api last_api_ = reshade::api::device_api::d3d11;

	EngineDevice engine_;
	RemoteEngine remote_;
	bool remote_tried_ = false;
	uint32_t remote_out_width_ = 0, remote_out_height_ = 0;
	uint32_t remote_in_width_ = 0, remote_in_height_ = 0;
	RemoteEngine::Extras remote_extras_;
	RemoteEngine::Info remote_info_;
	bool neural_remote_now() const noexcept;
	bool remote_wanted() const noexcept;
	bool runs_remote(const UpscalerBackend *backend) const noexcept;
	EngineRoute engine_route_now() const noexcept;
	bool dlss_override() const noexcept;
	bool crosses(const UpscalerBackend *backend) const noexcept;
	bool neural_crosses_alone(const EngineRoute &r, const UpscalerBackend *backend) const noexcept;
	bool must_run_in_host(const UpscalerBackend *backend) const noexcept;
	bool upscaler_layer_seen() const noexcept;
	const std::wstring &layer_dlss_path() const noexcept;
	bool game_dlss_for_layer() const noexcept;
	void sync_dlss_layer();
	std::wstring ngx_layer_table_;
	bool layer_dlss_started_ = false;
	bool native_before_layer_ = false;
	void ensure_remote();
	bool run_remote_engine_work(reshade::api::effect_runtime *runtime, const FrameInputs &inputs,
		const FramePlan &plan, UpscalerBackend *backend, const UpscalerParams &params,
		bool neural_here, bool upscaled_here = false);
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
	SharedPlane flow_game_global_;
	void publish_flow_to_game(FrameInputs &inputs, bool motion_read);
	bool finish_frame(reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list, const FrameInputs &inputs);

	GpuInfo gpu_;

	NativeDlss native_dlss_;
	unsigned long long native_dlss_scanned_ms_ = 0;
	uint32_t landing_discards_logged_ = 0;
	bool native_dlss_seen_ = false;
	unsigned long long ngx_layers_scanned_ms_ = 0;
	uint32_t ngx_layers_rescans_ = 0;
	std::vector<NgxLayer> ngx_layers_;
	void update_ngx_layers();
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
	PictureHold picture_hold_;
	uint64_t observe_serial_ = 0;
	struct HoldDrawn {
		uint64_t serial = 0;
		float x = 0.0f, y = 0.0f;
		bool scene = false;
		bool split = false;
	};
	static constexpr uint32_t kHoldDrawn = 16u;
	HoldDrawn hold_drawn_[kHoldDrawn]{};
	uint32_t held_frames_ = 0, moved_frames_ = 0;
	double moved_edges_ = 0.0, moved_changed_ = 0.0;
	void note_picture_hold();
	bool hud_detected_ = false;
	bool interface_split_ = false;
	bool interface_game_ = false;
	bool interface_restored_ = false;
	unsigned long long census_logged_at_ = 0;
	SplitGate split_gate_;
	SplitCover split_cover_;
	uint32_t seen_drawn_ = 0, seen_empty_ = 0, seen_other_ = 0, seen_changes_ = 0, seen_split_ = 0;
	int seen_last_ = -1;
	void detect_interface(const FrameInputs &inputs);
	struct RuntimeDrive {
		reshade::api::effect_runtime *runtime = nullptr;
		reshade::api::device *device = nullptr;
		void *hwnd = nullptr;
		PresentDrive drive;
	};
	std::mutex runtimes_lock_;
	std::vector<RuntimeDrive> runtimes_;
	std::vector<PresentRuntime> present_keys_;
#if AEONSR_FRAME_GENERATION
	FrameGeneration framegen_;
	void run_frame_generation(reshade::api::effect_runtime *runtime);
#endif
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
	JitterSceneSize scene_size_;
	bool move_all_ = false;
	JitterDepthProof depth_proof_;
	JitterLanding landing_;
	GameJitterProbeD3D12 game_probe_;
	GameJitterDetector game_jitter_;
	struct ProbeDrawn {
		uint64_t serial = 0;
		float x = 0.0f, y = 0.0f;
		bool scene = false;
	};
	ProbeDrawn probe_drawn_[8]{};
	std::wstring game_probe_error_;
	bool game_probe_failed_ = false;
	bool game_depth_ = false;
	bool withheld_said_ = false;
	GameJitterMemory game_memory_;
	std::wstring game_memory_path_;
	std::string game_exe_;
	bool game_memory_loaded_ = false;
	uint32_t memory_w_ = 0, memory_h_ = 0;
	bool memory_unsaved_ = false;
	unsigned long long memory_retry_at_ = 0;
	void keep_game_jitter_memory() noexcept;
	unsigned long long jitter_measure_logged_at_ = 0;
	uint32_t landing_reads_at_ = 0, landing_short_at_ = 0, game_fits_ = 0, game_skipped_at_ = 0;
	uint32_t game_depth_few_ = 0, game_depth_one_way_ = 0, responses_at_ = 0;
	bool camera_moving_ = false;
	CameraMotionGate camera_gate_;
	uint64_t game_fits_seen_at_ = 0;
	static constexpr unsigned long long kVerdictSettleMs = 5000u;
	unsigned long long verdict_since_ = 0;
	bool verdict_jitters_ = false, verdict_confirmed_ = false;
	int overruled_said_ = 0;
	bool depth_withholds_offset() const noexcept
	{
		return depth_withholds(game_jitter_.jitters(), landing_.lands(),
			game_jitter_.remembered() || landing_.takes_out());
	}
	bool offset_withheld() const noexcept
	{
		return withhold_offset(landing_.absorbed(), game_jitter_.measured(), depth_withholds_offset());
	}
	BlitPipelineD3D12 frame_shift_blit_;
	Gpu12Fence frame_shift_fence_;
	struct GameScaleFrame {
		bool active = false;
		bool attempted = false;
		bool done = false;
		GameScaleStretch stretch;
	} game_scale_;
	ID3D12Resource *game_scale_color_ = nullptr;
	ID3D12Resource *game_scale_depth_ = nullptr;
	ID3D12Resource *game_scale_motion_ = nullptr;
	BlitPipelineD3D12 game_scale_blit_;
	FrameInputs game_scale_shown_;
	bool game_scale_hooks_ = false;
	GameScaleWatch game_scale_watch_{};
	bool game_scale_watching_ = false;
	bool game_scale_said_ = false;
	bool game_scale_backend_takes() const noexcept;
	bool game_scale_before_aa_said_ = false;
	uint64_t game_scale_report_at_ = 0;
	uint32_t game_scale_frames_ = 0, game_scale_fallbacks_ = 0;
	games::GameIntegration *game_ = nullptr;
	std::wstring game_note_;
	games::ScaleDriver game_driver_;
	bool game_integrated() const noexcept
	{
		return settings_.game_render_scale && game_ != nullptr && game_->attached() && game_->replaces_stretch();
	}
	bool game_scale_holds_present() const noexcept
	{
		return game_integrated() && game_driver_.holds_present(game_->routes_full_size());
	}
	void drive_game_scale(bool stretch_ran);
	bool run_game_scale(const GameScaleStretch &s);
	bool enter_game_scale(FrameInputs &inputs);
	void game_scale_motion(FrameInputs &inputs);
	void update_game_scale_hooks();
	void release_game_scale() noexcept;
	StageTimings stage_times_;
	void run_engine_passes(reshade::api::effect_runtime *runtime, const FrameInputs &inputs,
		const FramePlan &plan, UpscalerBackend *backend, const UpscalerParams &params);
	void run_engine_work(reshade::api::effect_runtime *runtime, const FrameInputs &inputs,
		const FramePlan &plan, UpscalerBackend *backend, const UpscalerParams &params);
	void restore_interface(const FrameInputs &inputs);
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
	void start_report(const PanelState &state);
	void service_report();
	enum class ReportPhase { None, Frame, Writing };
	struct ReportJob {
		std::wstring zip_path, staging, frame_dir;
		std::string flight_csv, frame_note;
		bool frame_asked = false;
		unsigned long long deadline = 0;
	} report_job_;
	std::mutex report_lock_;
	ReportPhase report_phase_ = ReportPhase::None;
	std::wstring report_shown_, report_shown_error_;
	void flight_commit();
	FlightFault flight_fault_;
	uint32_t flight_landing_reads_ = 0;
	void invalidate_temporal_history() noexcept;
	const UpscalerBackend &backend_for_api(reshade::api::device_api api) const noexcept;
	bool active_is_dlss() const noexcept;
	bool active_is_fsr() const noexcept;
	bool active_is_xess() const noexcept;
	bool active_is_vsr() const noexcept;

	FlightRecorder flight_;
};

App *app() noexcept;
void create_app(HMODULE module);
void destroy_app();

}
