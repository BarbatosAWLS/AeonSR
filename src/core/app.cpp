#include "aeon_sr/core/app.hpp"

#include "aeon_sr/jitter/scene_snapshot.hpp"
#include "aeon_sr/jitter/vulkan_viewport_hook.hpp"
#include "aeon_sr/core/diag_report.hpp"
#include "aeon_sr/jitter/gl_jitter.hpp"
#include "aeon_sr/jitter/scene_jitter_shaders.hpp"
#include "aeon_sr/interop/interop_bridges.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/interop/native_d3d.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/core/runtime_search.hpp"

#include <d3d11.h>
#include <d3d12.h>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>

namespace aeon_sr {
namespace {

std::unique_ptr<App> g_app;

std::wstring widen(const char *text)
{
	return text != nullptr ? std::wstring(text, text + strlen(text)) : std::wstring();
}

class StageScope {
public:
	StageScope(StageTimings &t, StageTimings::Stage s, const FrameInputs &inputs) noexcept
		: t_(t), s_(s), cmd_(inputs.engine.cmd)
	{
		t_.begin(s_, inputs.engine.device, inputs.engine.queue, cmd_);
	}
	~StageScope() { t_.end(s_, cmd_); }
	StageScope(const StageScope &) = delete;
	StageScope &operator=(const StageScope &) = delete;

private:
	StageTimings &t_;
	StageTimings::Stage s_;
	ID3D12GraphicsCommandList *cmd_;
};

}

App::App(HMODULE module)
	: module_(module)
{
	const std::wstring addon_dir = module_directory(module);
	const std::wstring data_dir = join_path(addon_dir, L"AeonSR_data");

	ngx_.addon_dir = addon_dir;
	ngx_.data_dir = data_dir;
	ngx12_.addon_dir = addon_dir;
	ngx12_.data_dir = data_dir;
	dlss11_.rt = &ngx_;
	dlss12_.rt = &ngx12_;
	fsr11_.addon_dir = addon_dir;
	fsr12_.addon_dir = addon_dir;
	xess12_.addon_dir = addon_dir;
	neural12_.addon_dir = addon_dir;
	neural12_.data_dir = data_dir;

	ini_path_ = join_path(addon_dir, L"AeonSR.ini");
	load_settings(settings_, ini_path_);
	diag_set_verbose(settings_.verbose_log);
	set_jitter_shaders_wanted(settings_.spatial_jitter);
}

namespace {

FrameSignals signals_of(const FrameInputs &inputs, NeuralColorSpace color_space,
	const JitterFrame &drawn) noexcept
{
	FrameSignals s;
	s.color_space = color_space;
	s.jitter_drawn = drawn.valid;
	s.jitter_drawn_index = drawn.index;
	s.jitter_drawn_x = drawn.input_x;
	s.jitter_drawn_y = drawn.input_y;
	s.jitter_drawn_ambiguous = drawn.ambiguous;
	s.jitter_drawn_empty = drawn.empty;
	s.have_motion_vectors = inputs.have_motion_vectors;
	s.have_depth = inputs.have_depth;
	s.have_global_flow = inputs.have_global_flow;
	s.have_motion_confidence = inputs.have_motion_confidence;
	s.depth_sense_known = inputs.depth_sense_known();
	return s;
}

}

void App::persist_settings()
{

	invalidate_temporal_history();
	save_settings(settings_, ini_path_);
}

void App::invalidate_temporal_history() noexcept
{
	history_.temporal_history_valid = false;
}

void App::shutdown()
{
	engine_.wait_cpu(engine_.last_submitted());
	dlss11_.shutdown();
	dlss12_.shutdown();
	fsr11_.shutdown();
	fsr12_.shutdown();
	xess12_.shutdown();
	vsr11_.shutdown();
	guides_.release();
	pipelines_.release();
	probes_.release();
	debug_view_.release();
	flow_.release();
	depth_.release();
	shutdown_neural();
	ngx_.shutdown_device();
	ngx12_.shutdown_device();
	release_bridge(BridgeRelease::Ordinary);
	history_.temporal_history_valid = false;
}

void App::shutdown_neural()
{
	neural12_.shutdown_device();
	neural_last_ = 0;
	neural_init_failed_ = false;
	neural_teardown_pending_ = false;
}

void App::service_neural_teardown()
{
	if (!neural_teardown_pending_)
		return;
	neural_teardown_pending_ = false;

	neural12_.destroy_feature();
	neural_last_ = 0;
	neural_init_failed_ = false;
}

void App::request_neural_capture()
{

	SYSTEMTIME t{};
	GetLocalTime(&t);
	wchar_t stamp[32]{};
	_snwprintf_s(stamp, _TRUNCATE, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	neural12_.request_capture(join_path(join_path(neural12_.addon_dir, L"captures"), stamp));
}

void App::request_upscaler_capture()
{
	SYSTEMTIME t{};
	GetLocalTime(&t);
	wchar_t stamp[48]{};
	_snwprintf_s(stamp, _TRUNCATE, L"upscaler-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour,
		t.wMinute, t.wSecond);
	upscaler_capture_.request(join_path(join_path(neural12_.addon_dir, L"captures"), stamp));
	diag_info("capture", L"upscaler capture started: " + std::to_wstring(kUpscalerCaptureFrames) + L" frames");
}

void App::request_scope_capture()
{
	if (upscaler_capture_.scope_recording())
		return;
	SYSTEMTIME t{};
	GetLocalTime(&t);
	wchar_t stamp[48]{};
	_snwprintf_s(stamp, _TRUNCATE, L"scope-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour,
		t.wMinute, t.wSecond);
	std::wstring root = join_path(neural12_.addon_dir, L"captures");
	if (!settings_.scope_dir.empty()) {
		const std::string &d = settings_.scope_dir;
		const int n = MultiByteToWideChar(CP_ACP, 0, d.c_str(), static_cast<int>(d.size()), nullptr, 0);
		std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
		if (n > 0)
			MultiByteToWideChar(CP_ACP, 0, d.c_str(), static_cast<int>(d.size()), w.data(), n);
		while (!w.empty() && (w.back() == L'\\' || w.back() == L'/'))
			w.pop_back();
		if (!w.empty())
			root = w;
	}
	const std::wstring dir = join_path(root, stamp);
	upscaler_capture_.request_scope(dir, settings_.scope_frames);
	diag_info("capture", L"scope capture started: " + std::to_wstring(settings_.scope_frames) + L" frames into " + dir);
}

uint32_t App::fault_clock(uint32_t ahead) const noexcept
{
	const uint64_t base = upscaler_capture_.scope_recording() ? upscaler_capture_.scope_next() : trace_presents_;
	return static_cast<uint32_t>(base + ahead);
}

void App::trace_begin_row()
{
	ScopeTraceRow &r = trace_cur_;
	r = ScopeTraceRow{};
	r.set(ScopeColumn::Frame, static_cast<double>(trace_presents_));
	LARGE_INTEGER now{}, freq{};
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	if (trace_t0_ == 0)
		trace_t0_ = static_cast<uint64_t>(now.QuadPart);
	r.set(ScopeColumn::TimeMs, freq.QuadPart > 0 ? static_cast<double>(static_cast<uint64_t>(now.QuadPart) - trace_t0_) *
		1000.0 / static_cast<double>(freq.QuadPart) : 0.0);
	r.plan = "none";
	r.fault = fault_text_;
	r.set(ScopeColumn::FaultActive, 0.0);
	r.set(ScopeColumn::ArmedSerial, static_cast<double>(jitter_.armed_serial()));
	if (trace_armed_.made) {
		r.set(ScopeColumn::Index, trace_armed_.index);
		r.set(ScopeColumn::PointX, trace_armed_.point_x);
		r.set(ScopeColumn::PointY, trace_armed_.point_y);
		r.set(ScopeColumn::ComputedDrawX, trace_armed_.x);
		r.set(ScopeColumn::ComputedDrawY, trace_armed_.y);
	}
	r.set(ScopeColumn::Ran, 0.0);
	trace_open_ = true;
}

void App::trace_close()
{
	if (!trace_open_)
		trace_begin_row();
	trace_open_ = false;
	trace_armed_ = jitter_.armed_serial() != 0 ? trace_earned_ : TraceArm{};
	trace_earned_ = TraceArm{};
	if (trace_armed_.made && trace_armed_.fault)
		faulted_serials_[faulted_next_++ % kFaultedSerials] = jitter_.armed_serial();
	if (trace_ring_.size() != kTraceRing)
		trace_ring_.assign(kTraceRing, ScopeTraceRow{});
	trace_ring_[trace_presents_ % kTraceRing] = trace_cur_;
	++trace_presents_;
	if (upscaler_capture_.scope_recording())
		upscaler_capture_.scope_end(trace_cur_, scope_header());
}

const NeuralRenderCommon &App::neural() const noexcept
{
	return static_cast<const NeuralRenderCommon &>(neural12_);
}

bool App::neural_runtime_present() noexcept
{

	if (!gpu_.may_be_nvidia())
		return false;
	return neural12_.ensure_dll_present();
}

const UpscalerBackend &App::backend_for_api(reshade::api::device_api) const noexcept
{
	return static_cast<const UpscalerBackend &>(dlss12_);
}

UpscalerStatus App::active_status() const noexcept
{
	if (active_ != nullptr)
		return active_->status;
	return backend_for_api(last_api_).status;
}

const std::wstring &App::active_error() const noexcept
{
	if (active_ != nullptr)
		return active_->last_error;
	return backend_for_api(last_api_).last_error;
}

bool App::active_is_dlss() const noexcept
{
	return active_ == static_cast<const UpscalerBackend *>(&dlss11_)
		|| active_ == static_cast<const UpscalerBackend *>(&dlss12_);
}

bool App::active_is_fsr() const noexcept
{
	return active_ == static_cast<const UpscalerBackend *>(&fsr11_)
		|| active_ == static_cast<const UpscalerBackend *>(&fsr12_);
}

bool App::active_is_xess() const noexcept
{
	return active_ == static_cast<const UpscalerBackend *>(&xess12_);
}

bool App::active_is_vsr() const noexcept
{
	return active_ == static_cast<const UpscalerBackend *>(&vsr11_);
}

void App::run_engine_work(reshade::api::effect_runtime *runtime, const FrameInputs &inputs,
	const FramePlan &plan, UpscalerBackend *backend, const UpscalerParams &params)
{
	scope_list_lost_ = false;
	bool registered = false;
	if (hud_detected_ && !interface_split_ && backend != nullptr && backend != &fsr11_ && backend != &vsr11_)
		registered = hud_restore_.register_input(inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
			inputs.engine.color_state);
	trace_cur_.set(ScopeColumn::Register, registered ? 1.0 : 0.0);
	run_engine_passes(runtime, inputs, plan, backend, params);
	const StageScope restore_time(stage_times_, StageTimings::Stage::Restore, inputs);
	if (hud_detected_) {
		engine_journal_note(L"interface");
		(void)hud_restore_.composite(inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
			inputs.engine.color_state);
	}
	if (observed_)
		observer_.end(inputs.engine.device, inputs.engine.cmd, inputs.engine.color, inputs.engine.color_state,
			hud_detected_ ? hud_restore_.mask() : nullptr, HudRestoreD3D12::kMaskState,
			inputs.have_motion_vectors ? inputs.engine.motion : nullptr, inputs.engine.motion_state);
	observed_ = false;
	hud_detected_ = false;
	interface_split_ = false;
	debug_view_.apply(settings_.debug_view, inputs, depth_how_);
	if (upscaler_capture_.scope_recording() && !scope_list_lost_)
		upscaler_capture_.scope_copy(ScopePlane::Final, inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
			inputs.engine.color_state);
}

void App::detect_interface(const FrameInputs &inputs)
{
	hud_detected_ = false;
	if (!settings_.keep_interface || inputs.engine.device == nullptr || inputs.engine.cmd == nullptr ||
		inputs.engine.color == nullptr)
		return;
	const JitterFrame &drawn = jitter_.drawn();
	HudRestoreFrame f;
	f.drawn = drawn.valid;
	f.x = drawn.valid ? drawn.x : 0.0f;
	f.y = drawn.valid ? drawn.y : 0.0f;
	f.scene = inputs.engine.scene;
	f.scene_state = inputs.engine.scene_state;
	engine_journal_note(L"interface detect");
	hud_detected_ = hud_restore_.detect(inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
		inputs.engine.color_state, f);
	interface_split_ = false;
	if (hud_detected_ && inputs.engine.scene != nullptr) {
		const D3D12_RESOURCE_DESC cd = inputs.engine.color->GetDesc(), sd = inputs.engine.scene->GetDesc();
		if (cd.Width == sd.Width && cd.Height == sd.Height && cd.Format == sd.Format) {
			engine_journal_note(L"interface split");
			ID3D12GraphicsCommandList *const cmd = inputs.engine.cmd;
			barrier12(cmd, inputs.engine.color, inputs.engine.color_state, D3D12_RESOURCE_STATE_COPY_DEST);
			barrier12(cmd, inputs.engine.scene, inputs.engine.scene_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
			cmd->CopyResource(inputs.engine.color, inputs.engine.scene);
			barrier12(cmd, inputs.engine.scene, D3D12_RESOURCE_STATE_COPY_SOURCE, inputs.engine.scene_state);
			barrier12(cmd, inputs.engine.color, D3D12_RESOURCE_STATE_COPY_DEST, inputs.engine.color_state);
			interface_split_ = true;
		}
	}
	const SceneSnapshot::Census census = scene_snapshot().take_census();
	const int kind = drawn.valid ? 0 : drawn.empty ? 1 : 2;
	(kind == 0 ? seen_drawn_ : kind == 1 ? seen_empty_ : seen_other_) += 1u;
	seen_changes_ += seen_last_ >= 0 && seen_last_ != kind ? 1u : 0u;
	seen_last_ = kind;
	seen_split_ += interface_split_ ? 1u : 0u;
	if (GetTickCount64() >= census_logged_at_ + 10000u) {
		census_logged_at_ = GetTickCount64();
		diag_logf(DiagLevel::Info, "interface", L"interface: last 10 s %u frames drawn at the offset, %u with nothing "
			L"taking it, %u otherwise, %u changes between them; the window before the interface used on %u (%hs)",
			seen_drawn_, seen_empty_, seen_other_, seen_changes_, seen_split_, scene_snapshot().note());
		seen_drawn_ = seen_empty_ = seen_other_ = seen_changes_ = seen_split_ = 0;
		const SceneSnapshot::Tally t = scene_snapshot().take_tally();
		diag_logf(DiagLevel::Info, "interface", L"interface: last 10 s frames ended by the stream %u, by the present "
			L"%u; copies %u; presents with a copy %u, without %u, with no frame finished %u", t.in_stream, t.at_present,
			t.copies, t.taken, t.none, t.empty);
		diag_logf(DiagLevel::Info, "interface", L"interface: last frame into the window %u scene draws, %u of six "
			L"vertices or fewer, %u other; elsewhere %u not of the scene (%u of six or fewer), most into %ux%u",
			census.window_scene, census.window_quads, census.window_other, census.elsewhere, census.elsewhere_quads,
			census.elsewhere_w, census.elsewhere_h);
		if (census.order_len != 0)
			diag_logf(DiagLevel::Info, "interface", L"interface: the window's draws in order: %hs", census.order);
		const FrameObservation o = observer_.take();
		if (o.frames != 0 && o.pixels > 0.0) {
			const double rest = o.pixels - o.hud;
			const auto per = [](double sum, double n) { return n > 0.0 ? sum / n : 0.0; };
			diag_logf(DiagLevel::Info, "interface", L"picture: last 10 s, %u frames observed: the interface restore "
				L"shows %.1f%% of the window as drawn and holds %.1f%%; frame to frame the picture sent back changes by "
				L"%.3f code values on the interface and %.3f on the rest (the game's own frame by %.3f and %.3f), "
				L"%.2f%% and %.2f%% of those pixels by more than 2; the motion field moves %.1f%% of the rest by over "
				L"a quarter pixel, %.3f px on average", o.frames, 100.0 * o.hud / o.pixels,
				100.0 * o.held / o.pixels, per(o.out_change_interface, o.hud), per(o.out_change_rest, rest),
				per(o.drawn_change_interface, o.hud), per(o.drawn_change_rest, rest),
				100.0 * per(o.out_flicker_interface, o.hud), 100.0 * per(o.out_flicker_rest, rest),
				100.0 * per(o.motion_moving, o.motion_pixels), per(o.motion_rest, o.motion_pixels));
		}
	}
	diag_state("interface_split", DiagLevel::Info, "interface", interface_split_
		? std::wstring(L"interface: the upscaler takes the window before the interface; the interface is put back as drawn")
		: std::wstring(L"interface: the window before the interface is not used this frame; the restore puts back what "
			L"holds still"));
}

void App::run_engine_passes(reshade::api::effect_runtime *runtime, const FrameInputs &inputs,
	const FramePlan &plan, UpscalerBackend *backend, const UpscalerParams &planned)
{
	UpscalerParams params = planned;
	engine_journal_note(L"engine frame");
	upscaler_capture_.begin_frame(inputs.engine.device, inputs.engine.queue);
	const bool capturing = upscaler_capture_.recording() && backend != nullptr && backend != &fsr11_ &&
		backend != &vsr11_ && inputs.engine.ready() && !(remote_.ready() && runs_remote(backend));
	bool split_moved = false;
	float split_u = 0.0f, split_v = 0.0f;
	StageTimings::Stage upscaler_stage = StageTimings::Stage::Upscaler;
	stage_times_.begin(upscaler_stage, inputs.engine.device, inputs.engine.queue, inputs.engine.cmd);
	if (backend != nullptr && (plan.split_shift_x != 0.0f || plan.split_shift_y != 0.0f)) {
		engine_journal_note(L"frame shift");
		uint32_t rw = 0, rh = 0;
		if (capturing && backend->status == UpscalerStatus::Ready)
			upscaler_capture_.record_drawn(inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
				inputs.engine.color_state);
		const bool moved = backend->status == UpscalerStatus::Ready && active_render_size(inputs, &rw, &rh) &&
			frame_shift_.apply(inputs.engine.device, inputs.engine.queue, frame_shift_fence_, inputs.engine.cmd,
				frame_shift_blit_, inputs.engine.color, inputs.engine.color_state,
				plan.split_shift_x / static_cast<float>(rw), plan.split_shift_y / static_cast<float>(rh), nullptr,
				split_uses_catmull_rom(settings_.split_catmull_rom,
					active_is_dlss() ? BackendChoice::Dlss : BackendChoice::None));
		split_moved = moved;
		if (moved) {
			split_u = plan.split_shift_x / static_cast<float>(rw);
			split_v = plan.split_shift_y / static_cast<float>(rh);
		}
		if (!moved) {
			params.jitter_x += plan.split_shift_x;
			params.jitter_y += plan.split_shift_y;
		}
	}
	const bool scope = upscaler_capture_.scope_recording() && backend != nullptr && backend != &fsr11_ &&
		backend != &vsr11_ && inputs.engine.ready();
	if (backend != nullptr) {
		trace_cur_.set(ScopeColumn::ToldX, params.jitter_x);
		trace_cur_.set(ScopeColumn::ToldY, params.jitter_y);
		trace_cur_.set(ScopeColumn::InFrame, params.jitter_in_frame ? 1.0 : 0.0);
		trace_cur_.set(ScopeColumn::SplitApplied, split_moved ? 1.0 : 0.0);
		uint32_t rw = 0, rh = 0;
		if (active_render_size(inputs, &rw, &rh)) {
			trace_cur_.set(ScopeColumn::RenderW, rw);
			trace_cur_.set(ScopeColumn::RenderH, rh);
		}
	}
	if (remote_.ready() && runs_remote(backend)) {
		if (scope)
			upscaler_capture_.scope_copy(ScopePlane::UpscalerIn, inputs.engine.device, inputs.engine.cmd,
				inputs.engine.color, inputs.engine.color_state);
		run_remote_engine_work(runtime, inputs, plan, backend, params, sizeof(void *) != 4);
		stage_times_.cancel(upscaler_stage);
		stage_times_.cancel(StageTimings::Stage::Total);
		stage_times_.note_remote_frame();
		if (scope_list_lost_)
			stage_times_.abandon_frame();
		return;
	}
	if (backend == &dlss12_ && dlss_needs_host()) {
		dlss12_.status = UpscalerStatus::InitFailed;
		const wchar_t *const why = sizeof(void *) == 4
			? L"a 32-bit game runs its upscaler in AeonSRHost.exe"
			: L"the game runs its own DLSS, so Aeon SR's runs in AeonSRHost.exe";
		dlss12_.last_error = remote_.last_error.empty()
			? std::wstring(why) + L", which is starting"
			: std::wstring(why) + L", which could not start: " + remote_.last_error;
		backend = nullptr;
	}
	if (backend != nullptr)
		engine_journal_note(L"upscaler");
	if (capturing) {
		CapturedFrameInfo info;
		info.upscaler = backend->name();
		info.fsr_provider = fsr12_.want_provider;
		info.params = params;
		info.params.bias_mask = 0;
		if (split_moved) {
			info.split_shift_x = plan.split_shift_x;
			info.split_shift_y = plan.split_shift_y;
			info.split_u = split_u;
			info.split_v = split_v;
		}
		upscaler_capture_.record(inputs.engine.device, inputs.engine.cmd,
			inputs.engine.color, inputs.engine.color_state, inputs.engine.motion, inputs.engine.motion_state,
			inputs.depth_provider == DepthProvider::Normalized ? inputs.engine.depth : nullptr,
			inputs.engine.depth_state, std::move(info));
	}
	if (scope && backend != nullptr)
		upscaler_capture_.scope_copy(ScopePlane::UpscalerIn, inputs.engine.device, inputs.engine.cmd,
			inputs.engine.color, inputs.engine.color_state);
	const bool ran = backend != nullptr && backend->run(runtime, inputs, params);
	stage_times_.end(upscaler_stage, inputs.engine.cmd);
	trace_cur_.set(ScopeColumn::Ran, ran ? 1.0 : 0.0);
	if (scope && ran)
		upscaler_capture_.scope_copy(ScopePlane::Upscaled, inputs.engine.device, inputs.engine.cmd,
			inputs.engine.color, inputs.engine.color_state);
	if (ran && backend != &fsr11_)
		jitter_.note_upscaled();
	else if (backend != nullptr && upscaler_waiting(backend->status))
		jitter_.note_waiting();
	float depth_u = 0.0f, depth_v = 0.0f;
	jitter_.depth_uv(&depth_u, &depth_v);
	engine_journal_note(L"neural pass");
	const StageScope neural_time(stage_times_, StageTimings::Stage::Neural, inputs);
	run_neural_render(runtime, inputs, plan.neural, depth_u, depth_v);
}

void App::run_remote_engine_work(reshade::api::effect_runtime *runtime, const FrameInputs &inputs,
	const FramePlan &plan, UpscalerBackend *backend, const UpscalerParams &params,
	bool neural_here)
{
	const BackendChoice choice = choice_of(backend);
	if (choice == BackendChoice::Vsr) {
		if (backend != nullptr) {
			backend->status = UpscalerStatus::UnsupportedApi;
			backend->last_error = L"RTX VSR is a Direct3D 11 feature of the game's own device, "
				L"so it cannot run in the helper process this game needs. Pick DLSS, FSR or XeSS.";
		}
		neural_last_ = 0;
		return;
	}

	RemoteEngine::Request rq;
	rq.inputs = &inputs;
	rq.backend = choice;
	rq.upscaler = params;
	rq.run_neural = plan.neural.run && !neural_here;
	rq.game_queue = runtime != nullptr ? runtime->get_command_queue() : nullptr;
	rq.neural_want_depth = plan.neural.want_depth;
	rq.neural = plan.neural.params;
	rq.guide = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(params.bias_mask));
	jitter_.drawn_uv(&rq.depth_jitter_u, &rq.depth_jitter_v);

	RemoteEngine::Reply rp;
	(void)remote_.run(engine_, rq, rp);
	const bool ok = rp.step == remote::RemoteStep::Ok;
	if (!rp.list_recording) {
		hud_detected_ = false;
		scope_list_lost_ = true;
	}
	const bool host_ran = ok && rp.backend_ran == choice && rp.upscaler_status == UpscalerStatus::Ready;
	trace_cur_.set(ScopeColumn::Ran, host_ran ? 1.0 : 0.0);
	if (host_ran && rp.list_recording && upscaler_capture_.scope_recording())
		upscaler_capture_.scope_copy(ScopePlane::Upscaled, inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
			inputs.engine.color_state);

	if (backend != nullptr) {
		const RemoteEngine::Row row = RemoteEngine::upscaler_row(rp, inputs);
		backend->status = row.status;
		backend->last_error = row.message;
		if (upscaler_waiting(row.status))
			jitter_.note_waiting();
	}
	neural_last_ = ok ? rp.neural_ran : 0;
	if (ok && rp.out_width != 0) {
		remote_out_width_ = rp.out_width;
		remote_out_height_ = rp.out_height;
	}
	if (ok && rp.in_width != 0 && rp.in_height != 0) {
		remote_in_width_ = rp.in_width;
		remote_in_height_ = rp.in_height;
	}
	if (!neural_here && plan.neural.run && RemoteEngine::reports_neural(rp)) {
		neural12_.status = rp.neural_ran >= 0 ? UpscalerStatus::Ready : UpscalerStatus::EvaluateFailed;
		if (rp.neural_ran < 0)
			neural12_.last_error = rp.message;
		neural12_.model_width = rp.neural_model_width;
		neural12_.model_height = rp.neural_model_height;
		neural12_.last_eval_rows = rp.neural_eval_rows;
		neural12_.created_passes = rp.neural_passes_built;
		neural12_.timer.last_ms = rp.neural_gpu_ms;
	}

	if (rp.step == remote::RemoteStep::Lost)
		remote_tried_ = false;

	if (ok && rp.backend_ran == choice && rp.upscaler_status == UpscalerStatus::Ready)
		jitter_.note_upscaled();
	if (!neural_here)
		return;
	float depth_u = 0.0f, depth_v = 0.0f;
	jitter_.depth_uv(&depth_u, &depth_v);
	run_neural_render(runtime, inputs, plan.neural, depth_u, depth_v);
}

bool App::remote_wanted() const noexcept
{
	if constexpr (sizeof(void *) == 4)
		return true;
	return dlss_needs_host();
}

bool App::dlss_needs_host() const noexcept
{
	if constexpr (sizeof(void *) == 4)
		return true;
	return native_dlss_seen_ && settings_.enabled &&
		settings_.backend == static_cast<unsigned>(BackendChoice::Dlss);
}

bool App::runs_remote(const UpscalerBackend *backend) const noexcept
{
	if constexpr (sizeof(void *) == 4)
		return true;
	return backend == static_cast<const UpscalerBackend *>(&dlss12_);
}

void App::ensure_remote()
{
	if (!remote_wanted()) {
		if (remote_.ready()) {
			remote_.stop();
			diag_info("remote", L"the engine host was closed, nothing here needs it any more");
		}
		remote_tried_ = false;
		return;
	}
	if (remote_tried_ || remote_.ready() || !engine_.ready())
		return;
	remote_tried_ = true;
	if (!remote_.start(engine_, module_, last_api_))
		diag_state("remote", DiagLevel::Error, "remote", remote_.last_error);
	else
		diag_info("remote", remote_.describe());
}

BackendChoice App::choice_of(const UpscalerBackend *backend) const noexcept
{
	if (backend == static_cast<const UpscalerBackend *>(&dlss12_))
		return BackendChoice::Dlss;
	if (backend == static_cast<const UpscalerBackend *>(&fsr12_))
		return BackendChoice::Fsr;
	if (backend == static_cast<const UpscalerBackend *>(&xess12_))
		return BackendChoice::Xess;
	if (backend == static_cast<const UpscalerBackend *>(&vsr11_))
		return BackendChoice::Vsr;
	return BackendChoice::None;
}

BackendAvailability App::availability_of(UpscalerBackend *backend, BackendChoice choice)
{
	if (backend == nullptr)
		return BackendAvailability{ false, false, UpscalerStatus::Idle };
	const bool present = remote_.ready() && runs_remote(backend)
		? remote_.backend_present(choice)
		: backend->runtime_present();
	return BackendAvailability{ present, backend->crashed, backend->status };
}

void App::run_neural_render(reshade::api::effect_runtime *runtime,
	const FrameInputs &inputs, const NeuralPlan &neural,
	float depth_jitter_u, float depth_jitter_v)
{
	neural_last_ = 0;
	if (!neural.run || runtime == nullptr)
		return;
	if (!inputs.engine.ready() || neural12_.crashed)
		return;
	if (fsr12_.fsr_device_busy() || xess12_.xess_loading() || dlss12_.dlss_loading())
		return;

	if (!gpu_.may_be_nvidia()) {
		if (neural12_.status != UpscalerStatus::UnsupportedGpu) {
			neural12_.status = UpscalerStatus::UnsupportedGpu;
			neural12_.last_error = L"DLSS neural rendering needs an NVIDIA GPU: its runtime refuses to "
				L"initialise on any other adapter.";
		}
		neural_last_ = -1;
		return;
	}

	const bool want_depth = neural.want_depth;
	const NeuralRenderParams np = neural.params;

	ID3D12Device *const dev12 = engine_.device();
	if (dev12 == nullptr)
		return;

	if (neural12_.initialized && neural12_.device != dev12)
		neural12_.shutdown_device();

	if (!neural12_.initialized) {
		if (neural_init_failed_) {
			neural_last_ = -1;
			return;
		}
		if (!neural12_.init_device(dev12, inputs.width, inputs.height)) {
			neural_init_failed_ = true;
			neural_last_ = -1;
			return;
		}
	}
	neural12_.set_command_queue(engine_.queue());

	neural_last_ = neural12_.run(inputs.engine.cmd, inputs.engine.color, inputs.engine.color_state,
		inputs.engine.motion, want_depth ? inputs.engine.depth : nullptr, np,
		depth_jitter_u, depth_jitter_v) ? 1 : -1;
}

void App::refresh_depth_convention(reshade::api::effect_runtime *runtime)
{
	if (depth_how_read_ || runtime == nullptr)
		return;

	std::vector<DepthEffect> effects;
	runtime->enumerate_techniques(nullptr,
		[&effects](reshade::api::effect_runtime *rt, reshade::api::effect_technique technique) {
			char name[MAX_PATH] = {};
			rt->get_technique_effect_name(technique, name);
			const bool on = rt->get_technique_state(technique);
			for (DepthEffect &e : effects) {
				if (e.name == name) {
					e.active = e.active || on;
					return;
				}
			}
			effects.push_back({ name, on });
		});

	DepthDefinitions defs;
	defs.own = [runtime](const std::string &effect, const char *name, std::string &value) {
		char text[64] = {};
		if (!runtime->get_preprocessor_definition_for_effect(effect.c_str(), name, text))
			return false;
		value = text;
		return true;
	};
	defs.shared = [runtime](const char *name, std::string &value) {
		char text[64] = {};
		if (!runtime->get_preprocessor_definition(name, text))
			return false;
		value = text;
		return true;
	};
	depth_resolved_ = resolve_depth_convention(defs, effects);
	depth_how_ = depth_resolved_.how;
	depth_how_read_ = true;
	diag_state("depth-convention", depth_resolved_.overridden.empty() ? DiagLevel::Info : DiagLevel::Warn,
		"depth", describe_depth_convention(depth_resolved_));
}

ViewportClip App::viewport_clip() const noexcept
{
	return last_api_ == reshade::api::device_api::d3d11 || last_api_ == reshade::api::device_api::d3d12
		? ViewportClip::WholePixels : ViewportClip::Centres;
}

void App::fill_uncovered_edges(FrameInputs &inputs)
{
	const JitterFrame &drawn = jitter_.drawn();
	if (!drawn.valid || inputs.engine.color == nullptr || inputs.engine.cmd == nullptr)
		return;
	const FrameValidRect band = frame_valid_rect(drawn.x, drawn.y, inputs.width, inputs.height, viewport_clip(),
		jitter_scene_w_, jitter_scene_h_);
	if (!band.any)
		return;
	(void)frame_shift_.apply(inputs.engine.device, inputs.engine.queue, frame_shift_fence_, inputs.engine.cmd,
		frame_shift_blit_, inputs.engine.color, inputs.engine.color_state, 0.0f, 0.0f, band.uv);
}

void App::normalize_depth(FrameInputs &inputs)
{
	if (!inputs.have_depth || depth_failed_ || !inputs.engine.ready())
		return;
	if (inputs.engine.depth == nullptr) {
		return;
	}
	ID3D12Device *const dev12 = engine_.device();
	ID3D12GraphicsCommandList *const cmd12 = inputs.engine.cmd;
	if (dev12 == nullptr)
		return;

	std::wstring error;
	if (!depth_.ensure(dev12, inputs.width, inputs.height, &error)) {
		depth_failed_ = true;
		return;
	}
	const DepthConvention &how = depth_how_;

	const D3D12_RESOURCE_DESC src_desc = inputs.engine.depth->GetDesc();
	const JitterFrame &drawn = jitter_.drawn();
	const float sx = drawn.valid && inputs.width != 0
		? drawn.x * static_cast<float>(src_desc.Width) / static_cast<float>(inputs.width) : 0.0f;
	const float sy = drawn.valid && inputs.height != 0
		? drawn.y * static_cast<float>(src_desc.Height) / static_cast<float>(inputs.height) : 0.0f;
	const FrameValidRect band = frame_valid_rect(sx, sy, static_cast<uint32_t>(src_desc.Width), src_desc.Height,
		viewport_clip());
	if (!depth_.record(cmd12, inputs.engine.depth, inputs.engine.depth_state, how, &error,
			band.any ? band.uv : nullptr)) {
		depth_failed_ = true;
		return;
	}
	inputs.engine.depth = depth_.depth();
	inputs.engine.depth_state = DepthNormalizeD3D12::kPublished;
	inputs.depth_provider = DepthProvider::Normalized;
	inputs.depth_info = { depth_.width(), depth_.height(),
		static_cast<uint32_t>(DXGI_FORMAT_R32_FLOAT) };
}

namespace {

const char *api_label(reshade::api::device_api api) noexcept
{
	switch (api) {
	case reshade::api::device_api::d3d9: return "Direct3D 9";
	case reshade::api::device_api::d3d10: return "Direct3D 10";
	case reshade::api::device_api::d3d11: return "Direct3D 11";
	case reshade::api::device_api::d3d12: return "Direct3D 12";
	case reshade::api::device_api::opengl: return "OpenGL";
	case reshade::api::device_api::vulkan: return "Vulkan";
	}
	return "an unknown graphics API";
}

}

void App::update_native_dlss()
{
	native_dlss_ = scan_native_dlss();
	if (native_dlss_seen_)
		return;
	const bool ours = ngx_.ngx_initialized || ngx12_.ngx_initialized || dlss12_.dlss_starting() ||
		neural12_.initialized;
	native_dlss_seen_ = native_dlss_.streamline ||
		(!ours && (native_dlss_.dlss || native_dlss_.frame_generation ||
			native_dlss_.ray_reconstruction));
	if (native_dlss_seen_) {
		diag_state("native-dlss", DiagLevel::Warn, "aeonsr",
			L"the game brings its own DLSS: " + native_dlss_.modules +
			(native_dlss_.dlss_path.empty() ? L"" : (L" from " + native_dlss_.dlss_path)));
	}
}

void App::run_internal_flow(reshade::api::effect_runtime *runtime, FrameInputs &inputs)
{
	const bool anyone_wants_motion =
		(settings_.enabled && settings_.backend != static_cast<unsigned>(BackendChoice::None)) ||
		settings_.neural_render;
	motion_wanted_ = anyone_wants_motion;
	flow_note_benign_ = false;
	const bool wanted = anyone_wants_motion && !flow_failed_ &&
		inputs.engine.ready() && inputs.motion_provider != MotionProvider::Internal;
	if (!wanted) {
		flow_note_ = !anyone_wants_motion ? L"nothing needs motion this frame"
			: flow_failed_ ? flow_note_
			: std::wstring();
		flow_note_benign_ = !anyone_wants_motion;
		note_flow_state();
		return;
	}
	(void)runtime;

	ID3D12Device *const dev12 = engine_.device();
	ID3D12GraphicsCommandList *const cmd12 = inputs.engine.cmd;
	if (dev12 == nullptr || cmd12 == nullptr || inputs.engine.color == nullptr)
		return;
	if (inputs.width == 0 || inputs.height == 0)
		return;

	if (!flow_.ensure(dev12, inputs.width, inputs.height, &flow_error_)) {
		flow_failed_ = flow_.failed_permanently();
		flow_note_ = flow_error_;
		note_flow_state();
		return;
	}

	ID3D12Resource *const depth12 = inputs.depth_provider == DepthProvider::Normalized
		? inputs.engine.depth : nullptr;

	const OpticalFlowD3D12::Quality quality = settings_.internal_flow_quality != 0u
		? OpticalFlowD3D12::Quality::High
		: OpticalFlowD3D12::Quality::Balanced;

	flow_.set_depth_logarithmic(depth_how_.logarithmic);
	flow_.set_colour_space(static_cast<uint32_t>(color_space_));
	stage_times_.begin(StageTimings::Stage::Flow, dev12, inputs.engine.queue, cmd12);
	const bool recorded = flow_.record(cmd12, inputs.engine.color, inputs.engine.color_state,
		depth12, inputs.engine.depth_state,
		quality, true, &flow_error_, true,
		jitter_.drawn().valid ? jitter_.drawn().x : 0.0f,
		jitter_.drawn().valid ? jitter_.drawn().y : 0.0f);
	stage_times_.end(StageTimings::Stage::Flow, cmd12);
	if (!recorded) {
		flow_failed_ = true;
		flow_note_ = flow_error_;
		note_flow_state();
		return;
	}

	if (!flow_.has_history()) {
		flow_note_benign_ = true;
		flow_note_ = L"warming up: the estimator needs two frames of the same size";
		note_flow_state();
		return;
	}

	{
		float sums[4];
		if (flow_.landing(sums) && landing_.update(sums)) {
			flow_.reset_landing();
			wchar_t text[200]{};
			_snwprintf_s(text, _TRUNCATE, L"jitter: the picture moved %.2f times the offset across and %.2f down; "
				L"drawn turned over %ls", landing_.factor_x(), landing_.factor_y(),
				landing_.flip_x() && landing_.flip_y() ? L"on both axes"
				: landing_.flip_x() ? L"across" : landing_.flip_y() ? L"down" : L"on neither axis");
			diag_info("jitter", text);
		}
	}

	if (flow_.model_failed())
		diag_state("camera-model", DiagLevel::Warn, "flow", flow_.model_error());

	inputs.engine.motion = flow_.motion();
	inputs.engine.motion_state = DepthNormalizeD3D12::kPublished;
	inputs.engine.confidence = flow_.confidence();
	inputs.engine.global_flow = flow_.global_flow();
	inputs.have_motion_vectors = true;
	inputs.motion_provider = MotionProvider::Internal;
	inputs.motion_info = { flow_.width(), flow_.height(),
		static_cast<uint32_t>(DXGI_FORMAT_R16G16_FLOAT) };
	inputs.have_motion_confidence = flow_.confidence() != nullptr;
	inputs.have_global_flow = flow_.global_flow() != nullptr;

	{
		const StageScope publish_time(stage_times_, StageTimings::Stage::Publish, inputs);
		publish_flow_to_game(inputs);
	}

	flow_note_.clear();
	note_flow_state();
}

void App::publish_flow_to_game(FrameInputs &inputs)
{
	inputs.motion_vectors = { 0 };
	inputs.motion_confidence = { 0 };
	inputs.global_flow = { 0 };

	if (inputs.engine.native) {
		inputs.motion_vectors = { reinterpret_cast<uintptr_t>(flow_.motion()) };
		if (flow_.confidence() != nullptr)
			inputs.motion_confidence = { reinterpret_cast<uintptr_t>(flow_.confidence()) };
		if (flow_.global_flow() != nullptr)
			inputs.global_flow = { reinterpret_cast<uintptr_t>(flow_.global_flow()) };
		return;
	}
	if (bridge_ == nullptr || inputs.engine.cmd == nullptr)
		return;

	const auto carry = [&](SharedPlane &plane, ID3D12Resource *src, uint32_t w, uint32_t h,
			DXGI_FORMAT fmt, reshade::api::resource &out) {
		out = { 0 };
		if (src == nullptr || w == 0 || h == 0)
			return;
		if (!bridge_->request_readable(plane, w, h, fmt) || plane.engine == nullptr)
			return;
		if (plane.game.handle == 0)
			return;

		D3D12_RESOURCE_BARRIER b[2]{};
		for (int i = 0; i < 2; ++i) {
			b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		}
		b[0].Transition.pResource = plane.engine;
		b[0].Transition.StateBefore = SharedPlane::kState;
		b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		b[1].Transition.pResource = src;
		b[1].Transition.StateBefore = DepthNormalizeD3D12::kPublished;
		b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		inputs.engine.cmd->ResourceBarrier(2, b);
		inputs.engine.cmd->CopyResource(plane.engine, src);
		for (int i = 0; i < 2; ++i)
			std::swap(b[i].Transition.StateBefore, b[i].Transition.StateAfter);
		inputs.engine.cmd->ResourceBarrier(2, b);

		out = plane.game;
	};

	carry(flow_game_motion_, flow_.motion(), flow_.width(), flow_.height(),
		DXGI_FORMAT_R16G16_FLOAT, inputs.motion_vectors);
	carry(flow_game_confidence_, flow_.confidence(), flow_.width(), flow_.height(),
		DXGI_FORMAT_R16_FLOAT, inputs.motion_confidence);
	carry(flow_game_global_, flow_.global_flow(), 1u, 1u,
		DXGI_FORMAT_R16G16_FLOAT, inputs.global_flow);
}

void App::note_frame_blocked(const std::wstring &why)
{
	motion_wanted_ = settings_.neural_render ||
		(settings_.enabled && settings_.backend != static_cast<unsigned>(BackendChoice::None));
	flow_note_benign_ = false;
	flow_note_ = L"the frame never reached the estimator: " + why;
	neural_last_ = 0;
	note_flow_state();
}

void App::note_flow_state()
{
	if (flow_note_ == logged_flow_note_)
		return;
	logged_flow_note_ = flow_note_;
	if (flow_note_.empty())
		diag_info("flow", L"producing motion vectors");
	else if (flow_note_benign_)
		diag_info("flow", L"no motion vectors - " + flow_note_);
	else
		diag_warn("flow", L"no motion vectors - " + flow_note_);
}

void App::note_state()
{
	if (active_ != nullptr) {
		const bool healthy = active_->status == UpscalerStatus::Ready ||
			active_->status == UpscalerStatus::Idle;
		const std::string provider = fsr12_.fsr_loading()
			? std::string() : diag_narrow(fsr12_.provider_version);
		std::wstring line = diag_widen(upscaler_display_name(active_->name(),
				active_is_fsr(), provider).c_str()) + L": " +
			diag_widen(status_label(active_->status));
		if (!active_->last_error.empty())
			line += L" - " + active_->last_error;
		diag_state("upscaler", healthy ? DiagLevel::Info : DiagLevel::Warn, "upscaler", line);
	}

	const NeuralRenderCommon &nr = neural();
	if (settings_.neural_render) {
		std::wstring line = std::wstring(L"neural pass: ") +
			(nr.crashed ? L"faulted"
				: neural_last_ == 1 ? L"running"
				: neural_last_ == -1 ? L"failed" : L"idle");
		if (!nr.last_error.empty())
			line += L" - " + nr.last_error;
		diag_state("neural", neural_last_ == 1 ? DiagLevel::Info : DiagLevel::Warn,
			"neural", line);
	}

	diag_state("depth", DiagLevel::Info, "inputs",
		std::wstring(L"depth: ") + (last_inputs_.depth_provider == DepthProvider::None
			? L"none - ReShade has not selected a depth buffer for this game"
			: last_inputs_.depth_provider == DepthProvider::Normalized
				? L"normalised" : L"the game's own buffer"));

	if (last_plan_.jitter_active) {
		if (jitter_.drawn().aliased != 0)
			diag_state("jitter_alias", DiagLevel::Info, "jitter", L"jitter: the game draws part of its "
				L"scene with another depth buffer of the scene's size; those draws are moved with the scene");
		if (jitter_.drawn().by_target != 0)
			diag_state("jitter_by_target", DiagLevel::Info, "jitter", L"jitter: the game draws part of its "
				L"scene with no depth buffer into the target its depth was drawn into; those draws are moved "
				L"with the scene");
		diag_state("jitter", DiagLevel::Info, "jitter", jitter_.drawn().valid
			? std::wstring(L"jitter: drawn into the game's frame")
			: jitter_.drawn().empty
				? std::wstring(L"jitter: nothing the game drew took the offset; the frame goes to the upscaler "
					L"as drawn, with none") + (jitter_note_[0] != '\0' ? L" - " + widen(jitter_note_) : std::wstring())
			: last_plan_.params.jitter_in_frame
				? std::wstring(L"jitter: drawn into the game's frame, mostly without the offset, "
					L"the upscaler told none")
				: std::wstring(L"jitter: resampled from the finished frame") +
					(jitter_note_[0] != '\0' ? L" - " + widen(jitter_note_) : std::wstring()));
	}
}

void App::arm_scene_jitter(reshade::api::effect_runtime *runtime, const FrameInputs &inputs)
{
	set_jitter_shaders_wanted(settings_.spatial_jitter);
	set_jitter_phases(settings_.backend == static_cast<unsigned>(BackendChoice::Fsr) &&
		static_cast<UpscaleMode>(settings_.upscale_mode) == UpscaleMode::Dlaa ? 8u : kJitterPhases);
	if (!settings_.spatial_jitter) {
		jitter_note_ = "";
		return;
	}
	if (active_ == nullptr) {
		jitter_note_ = "no upscaler is running";
		return;
	}
	if (active_ == &vsr11_) {
		jitter_note_ = "RTX VSR has no history to gather samples into";
		return;
	}
	switch (last_api_) {
	case reshade::api::device_api::d3d11:
	case reshade::api::device_api::d3d12:
	case reshade::api::device_api::vulkan:
		break;
	case reshade::api::device_api::opengl:
		if (!gl_jitter_usable()) {
			jitter_note_ = gl_jitter_note();
			return;
		}
		break;
	case reshade::api::device_api::d3d9:
	case reshade::api::device_api::d3d10: {
		const JitterShaderCounts shaders = jitter_shader_counts();
		if (shaders.rewritten == 0 && shaders.unwanted != 0) {
			jitter_note_ = "the game made its shaders while this was off: restart the game";
			return;
		}
		break;
	}
	default:
		jitter_note_ = "this game's graphics API cannot draw it";
		return;
	}
	if (active_ == &fsr11_) {
		jitter_note_ = "FSR 3.1 on Direct3D 11 cannot take it";
		return;
	}
	if (last_api_ == reshade::api::device_api::vulkan && !vulkan_viewport_hook().installed()) {
		if (reshade::api::device *const dev = runtime->get_device()) {
			void *gdpa = nullptr;
			if (const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll"))
				gdpa = reinterpret_cast<void *>(GetProcAddress(loader, "vkGetDeviceProcAddr"));
			vulkan_viewport_hook().install(
				reinterpret_cast<void *>(static_cast<uintptr_t>(dev->get_native())), gdpa);
		}
	}
	if (last_api_ == reshade::api::device_api::vulkan && settings_.keep_interface && !vulkan_rendering_hook_installed()) {
		if (reshade::api::device *const dev = runtime->get_device()) {
			void *gdpa = nullptr;
			if (const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll"))
				gdpa = reinterpret_cast<void *>(GetProcAddress(loader, "vkGetDeviceProcAddr"));
			(void)vulkan_rendering_hook_install(reinterpret_cast<void *>(static_cast<uintptr_t>(dev->get_native())), gdpa);
		}
	}
	if (const char *const why = jitter_.refusal()) {
		jitter_note_ = why;
		if (!jitter_.gate_open() && !jitter_pause_logged_) {
			jitter_pause_logged_ = true;
			const JitterFrame &f = jitter_.drawn();
			const JitterShaderCounts c = jitter_shader_counts();
			const JitterPromotions pr = scene_jitter().take_promotions();
			diag_logf(DiagLevel::Info, "jitter", L"jitter paused: last frame %u scene draws moved, %u on the "
				L"grid (%u of those with the other depth buffer, of %u drawn with it), %u replayed; offsets "
				L"reaching the recording since the last pause: %u at a frame boundary, %u between frames, "
				L"%u mid-frame; vertex shaders rewritten %u (%u indexing through a0), left as they were "
				L"%u; draws kept on the grid so far: %u by an indexed shader whose register the game writes, "
				L"%u already in screen space",
				f.moved, f.plain, f.plain_aliased, f.aliased, f.replayed, pr.at_boundary, pr.between_frames,
				pr.forced, c.rewritten, c.indexed, c.kept, c.indexed_held, c.pretransformed);
		}
		return;
	}
	jitter_pause_logged_ = false;
	const generic_depth_data *const depth = runtime->get_private_data<generic_depth_data>();
	uint64_t scene_depth = 0;
	uint32_t tw = 0, th = 0;
	if (depth != nullptr && depth->selected_depth_stencil.handle != 0) {
		const reshade::api::resource_desc desc =
			runtime->get_device()->get_resource_desc(depth->selected_depth_stencil);
		tw = desc.texture.width;
		th = desc.texture.height;
		scene_depth = tw != 0 && th != 0 ? depth->selected_depth_stencil.handle : 0;
	}
	if (learned_scene_dw_ != inputs.width || learned_scene_dh_ != inputs.height || scene_depth != 0) {
		learned_scene_w_ = learned_scene_h_ = 0;
		learned_scene_dw_ = inputs.width;
		learned_scene_dh_ = inputs.height;
	}
	if (scene_depth != 0) {
		depth_scene_w_ = tw;
		depth_scene_h_ = th;
		depth_scene_dw_ = inputs.width;
		depth_scene_dh_ = inputs.height;
	} else {
		(void)jitter_learn_scene_size(jitter_.drawn(), inputs.width, inputs.height, &learned_scene_w_,
			&learned_scene_h_);
		if (learned_scene_w_ != 0) {
			tw = learned_scene_w_;
			th = learned_scene_h_;
		} else if (depth_scene_w_ != 0 && depth_scene_dw_ == inputs.width && depth_scene_dh_ == inputs.height) {
			tw = depth_scene_w_;
			th = depth_scene_h_;
		} else {
			tw = inputs.width;
			th = inputs.height;
		}
	}
	if (tw == 0 || th == 0) {
		jitter_note_ = "waiting for the game's depth buffer";
		return;
	}
	diag_state("jitter_scene", DiagLevel::Info, "jitter", scene_depth != 0
		? std::wstring(L"jitter: the scene is the depth buffer ReShade selected")
		: L"jitter: no depth buffer selected; the scene is what the game draws at " + std::to_wstring(tw) + L"x" +
			std::to_wstring(th));
	if (!scene_jitter().saw_viewports() && !vulkan_viewport_hook().saw_viewports()) {
		const char *const why = vulkan_viewport_hook().note();
		jitter_note_ = last_api_ != reshade::api::device_api::vulkan
			? "the game has not bound a viewport the add-on can move"
			: why[0] != '\0' ? why
			: "waiting for the game to set a viewport";
		return;
	}
	uint32_t rw = 0, rh = 0;
	if (!active_render_size(inputs, &rw, &rh)) {
		jitter_note_ = "waiting for the upscaler to report its input size";
		return;
	}
	jitter_note_ = jitter_.note();
	jitter_scene_w_ = tw;
	jitter_scene_h_ = th;
	const Jitter j = halton_jitter(history_.jitter_index / 2u, kJitterAmount);
	const JitterReach reach = JitterReach::SceneTargets;
	JitterArm arm = make_jitter_arm(history_.jitter_index, j.x, j.y, inputs.width, inputs.height,
		rw, rh, tw, th, scene_depth, runtime->get_back_buffer(0).handle, reach);
	if (landing_.flip_x())
		arm.target_x = -arm.target_x;
	if (landing_.flip_y())
		arm.target_y = -arm.target_y;
	move_all_ = jitter_move_all(move_all_, jitter_.drawn());
	scene_rules(settings_.jitter_scene_rule, move_all_, &arm.size_targets, &arm.move_window);
	arm.tested_quads = settings_.jitter_tested_quads;
	diag_state("jitter_move_all", DiagLevel::Info, "jitter", arm.move_window
		? std::wstring(L"jitter: no draw shows the scene by its depth; everything of the scene's size takes the offset")
		: std::wstring(L"jitter: the scene is found by its depth"));
	scene_jitter().back_buffers_into(arm);
	if (last_api_ == reshade::api::device_api::opengl && gl_clip_origin_upper_left())
		arm.target_y = -arm.target_y;
	TraceArm made;
	made.made = true;
	made.index = arm.index;
	made.point_x = j.x;
	made.point_y = j.y;
	made.x = arm.x;
	made.y = arm.y;
	made.fault = apply_draw_fault(fault_, fault_clock(1), &arm);
	if (jitter_.earn(arm))
		trace_earned_ = made;
}

bool App::active_render_size(const FrameInputs &inputs, uint32_t *w, uint32_t *h)
{
	*w = 0;
	*h = 0;
	if (remote_.ready() && runs_remote(active_)) {
		if (remote_in_width_ != 0 && remote_in_height_ != 0) {
			*w = remote_in_width_;
			*h = remote_in_height_;
		} else if (static_cast<UpscaleMode>(settings_.upscale_mode) == UpscaleMode::Dlaa) {
			*w = inputs.width;
			*h = inputs.height;
		}
	} else if (active_ == &dlss12_) {
		*w = ngx12_.render_width;
		*h = ngx12_.render_height;
	} else if (active_ == &fsr12_) {
		*w = fsr12_.fsr_render_width();
		*h = fsr12_.fsr_render_height();
	} else if (active_ == &fsr11_) {
		*w = fsr11_.fsr_render_width();
		*h = fsr11_.fsr_render_height();
	} else if (active_ == &xess12_) {
		*w = xess12_.xess_render_width();
		*h = xess12_.xess_render_height();
	}
	return *w != 0 && *h != 0 && inputs.width != 0 && inputs.height != 0;
}

bool App::is_engine_device(reshade::api::device *device) const noexcept
{
	if (device == nullptr)
		return false;
	if (building_bridge_)
		return true;
	return engine_.device() != nullptr && !engine_.borrowed() &&
		device->get_api() == reshade::api::device_api::d3d12 &&
		reinterpret_cast<ID3D12Device *>(device->get_native()) == engine_.device();
}

void App::ensure_bridge(reshade::api::device *device)
{
	if (device == nullptr || building_bridge_)
		return;
	if (bridge_ != nullptr && bridge_->ready() && bridge_device_ == device)
		return;
	release_bridge(BridgeRelease::Ordinary);

	diag_logf(DiagLevel::Info, "interop", L"bridging %hs to Direct3D 12", api_label(device->get_api()));

	building_bridge_ = true;
	bridge_error_.clear();
	bridge_ = make_bridge(device, engine_, &bridge_error_, settings_.force_system_memory);
	building_bridge_ = false;
	bridge_device_ = bridge_ != nullptr ? device : nullptr;
	if (bridge_ == nullptr) {
		diag_error("interop", L"this graphics API could not be bridged to Direct3D 12: " +
			(bridge_error_.empty() ? std::wstring(L"no reason given") : bridge_error_));
		return;
	}
	diag_logf(DiagLevel::Info, "interop", L"%hs, synchronised by %hs%s",
		bridge_->name(), bridge_->sync_name(),
		engine_.borrowed() ? L"" : L" (engine device of our own)");
}

void App::release_bridge(BridgeRelease how)
{
	if (building_bridge_)
		return;
	building_bridge_ = true;
	struct Done {
		bool &flag;
		~Done() { flag = false; }
	} done{ building_bridge_ };

	engine_.wait_cpu(engine_.last_submitted());
	stage_times_.release();

	bridge_util::release_plane(flow_game_motion_);
	bridge_util::release_plane(flow_game_confidence_);
	bridge_util::release_plane(flow_game_global_);
	engine_.set_borrowed_queue(nullptr);
	remote_.stop();
	remote_tried_ = false;

	upscaler_capture_.finish(engine_.device(), engine_.queue());
	retire_bridge(bridge_, how);
	bridge_device_ = nullptr;
	frame_shift_.release();
	frame_shift_blit_.release();
	frame_shift_fence_.release();
	hud_restore_.release();
	observer_.release();
	hud_detected_ = false;
	engine_.shutdown();
}

void App::on_init_device(reshade::api::device *device)
{
	if (is_engine_device(device))
		return;

	const auto api = device ? device->get_api() : reshade::api::device_api::d3d11;

	if (!adopt_on_init(api, bridge_device_)) {
		pending_devices_.add(device);
		return;
	}
	last_api_ = api;
	adopt_device(device);
}

void App::adopt_device(reshade::api::device *device)
{
	const auto api = device ? device->get_api() : reshade::api::device_api::d3d11;

	{
		last_api_ = api;
		if (api != reshade::api::device_api::d3d12)
			xess12_.status = UpscalerStatus::UnsupportedApi;

		ensure_bridge(device);

		gpu_ = GpuInfo{};
		if (engine_.ready())
			query_gpu_info(engine_.device(), &gpu_);
		else if (api == reshade::api::device_api::d3d12)
			query_gpu_info(native_d3d12_device(device), &gpu_);
		else if (api == reshade::api::device_api::d3d11)
			query_gpu_info(native_d3d11_device(device), &gpu_);
		if (gpu_.queried)
			diag_logf(DiagLevel::Info, "gpu", L"render device: %s (vendor 0x%04X, device 0x%04X) on %hs%s",
				gpu_.name.c_str(), gpu_.vendor_id, gpu_.device_id, api_label(api),
				gpu_.vendor_id_disputed
					? L" - those ids are spoofed by a translation layer; the card is read off its name"
					: L"");
		if (settings_.backend >= kBackendChoiceCount) {
			settings_.backend = static_cast<unsigned int>(backend_choice(settings_.backend, gpu_.vendor));
			persist_settings();
		}
		if (!gpu_.may_be_nvidia()) {

			dlss11_.status = UpscalerStatus::UnsupportedGpu;
			dlss12_.status = UpscalerStatus::UnsupportedGpu;
			vsr11_.status = UpscalerStatus::UnsupportedGpu;
			ngx_.status = UpscalerStatus::UnsupportedGpu;
			ngx12_.status = UpscalerStatus::UnsupportedGpu;
		}
	}
	history_.reset_pending = true;
	invalidate_temporal_history();
}

void App::on_destroy_device(reshade::api::device *device)
{
	pending_devices_.remove(device);
	scene_snapshot().release(device);
	const std::optional<BridgeRelease> release =
		bridge_release_on_destroy(device, bridge_device_, device != nullptr && is_engine_device(device));
	if (!release)
		return;
	engine_.wait_cpu(engine_.last_submitted());
	jitter_.forget();
	vulkan_viewport_hook().forget();
	guides_.release();
	pipelines_.release();
	probes_.release();
	debug_view_.release();
	flow_.release();
	depth_.release();

	neural12_.shutdown_device();
	dlss12_.shutdown();
	fsr12_.shutdown();
	xess12_.shutdown();

	if (device->get_api() == reshade::api::device_api::d3d11) {
		if (native_d3d11_device(device) == ngx_.device)
			dlss11_.shutdown();
		fsr11_.shutdown();
		vsr11_.shutdown();
	}

	release_bridge(*release);
	flow_failed_ = false;
	depth_failed_ = false;
	depth_how_read_ = false;
	invalidate_temporal_history();
}

void App::on_init_swapchain(reshade::api::swapchain *swapchain, bool resize)
{
	if (swapchain == nullptr)
		return;
	if (is_engine_device(swapchain->get_device()))
		return;

	flow_failed_ = false;
	depth_failed_ = false;

	reshade::api::device *const device = swapchain->get_device();
	if (device == nullptr)
		return;

	reshade::api::resource_desc desc;
	const bool have_surface = swapchain_surface_of(swapchain, &desc);
	const uint32_t w = have_surface ? desc.texture.width : 0u;
	const uint32_t h = have_surface ? desc.texture.height : 0u;

	const wchar_t *color_space_name = L"SDR";
	switch (swapchain->get_color_space()) {
	case reshade::api::color_space::scrgb:
		color_space_ = NeuralColorSpace::ScrgbLinear;
		color_space_name = L"scRGB";
		break;
	case reshade::api::color_space::hdr10_pq:
		color_space_ = NeuralColorSpace::Pq;
		color_space_name = L"HDR10 PQ";
		break;
	default:
		color_space_ = NeuralColorSpace::Sdr;
		break;
	}

	last_api_ = device->get_api();
	++swapchain_inits_;
	const uint32_t count = swapchain->get_back_buffer_count();
	const uint32_t current = swapchain->get_current_back_buffer_index();
	{
		uint64_t images[kJitterBackBuffers]{};
		uint32_t n = 0;
		for (uint32_t i = 0; i < count && n < kJitterBackBuffers; ++i)
			images[n++] = swapchain->get_back_buffer(i).handle;
		scene_jitter().watch_swapchain(images, n);
		scene_jitter().set_present_lags(last_api_ == reshade::api::device_api::vulkan);
	}
	if (have_surface)
		diag_logf(DiagLevel::Info, "swapchain", L"chain %u%s: %ux%u %hs on %hs, %s, %u images, current index %u",
			swapchain_inits_, resize ? L" (resized)" : L"", w, h,
			panel::format_label(static_cast<uint32_t>(desc.texture.format)),
			api_label(last_api_), color_space_name, count, current);
	else
		diag_logf(DiagLevel::Warn, "swapchain", L"chain %u%s: ReShade named no image, or a zero-sized one, "
			L"for the swap chain on %hs (%u images, current index %u); the display size is read from the "
			L"first frame instead",
			swapchain_inits_, resize ? L" (resized)" : L"", api_label(last_api_), count, current);

	if (have_surface && ngx12_.ngx_initialized && ngx12_.device == engine_.device()) {
		ngx12_.width = w;
		ngx12_.height = h;
	}
	if (have_surface && last_api_ == reshade::api::device_api::d3d11) {
		ID3D11Device *const d3d = native_d3d11_device(device);
		if (d3d != nullptr && ngx_.ngx_initialized && ngx_.device == d3d) {
			ngx_.width = w;
			ngx_.height = h;
		}
	}

	history_.reset_pending = true;
	invalidate_temporal_history();
}

void App::on_destroy_swapchain(reshade::api::swapchain * , bool )
{
	jitter_.forget();
	if (bridge_ != nullptr)
		bridge_->on_swapchain_reset();
	dlss11_.on_destroy_swapchain();
	dlss12_.on_destroy_swapchain();
	fsr11_.on_destroy_swapchain();
	fsr12_.on_destroy_swapchain();
	xess12_.on_destroy_swapchain();
	vsr11_.on_destroy_swapchain();
	neural12_.destroy_feature();
	history_.reset_pending = true;
	invalidate_temporal_history();
}

void App::on_reloaded_effects(reshade::api::effect_runtime *)
{
	depth_how_read_ = false;
	history_.reset_pending = true;
	invalidate_temporal_history();
}

void App::on_present(reshade::api::effect_runtime *runtime)
{
	clock_.tick();
	set_jitter_shaders_wanted(settings_.spatial_jitter);
	if (runtime != nullptr) {
		RuntimeJitterCounts counts(runtime);
		jitter_.present(reinterpret_cast<uintptr_t>(runtime), counts);
		if (reinterpret_cast<uintptr_t>(runtime) == trace_runtime_)
			trace_close();
	}
}

void App::on_begin_effects(
	reshade::api::effect_runtime *runtime,
	reshade::api::command_list *cmd_list,
	reshade::api::resource_view rtv,
	reshade::api::resource_view )
{
	run_effects_frame(reshade::addon_event::reshade_begin_effects, runtime, cmd_list, rtv);
}

void App::on_finish_effects(
	reshade::api::effect_runtime *runtime,
	reshade::api::command_list *cmd_list,
	reshade::api::resource_view rtv,
	reshade::api::resource_view )
{
	run_effects_frame(reshade::addon_event::reshade_finish_effects, runtime, cmd_list, rtv);
}

void App::run_effects_frame(reshade::addon_event event, reshade::api::effect_runtime *runtime,
	reshade::api::command_list *cmd_list, reshade::api::resource_view rtv)
{
	const EffectsFrame frame = effects_frame_of(event, settings_.upscale_effects);
	if (frame.runs)
		run_frame(runtime, cmd_list, rtv, frame.after_effects);
}

void App::run_frame(
	reshade::api::effect_runtime *runtime,
	reshade::api::command_list *cmd_list,
	reshade::api::resource_view rtv,
	bool after_effects)
{
	if (runtime == nullptr)
		return;

	{
		RuntimeJitterCounts counts(runtime);
		jitter_.begin_frame(reinterpret_cast<uintptr_t>(runtime), counts);
	}
	jitter_note_ = jitter_.note();

	if (!fault_read_) {
		fault_read_ = true;
		if (!parse_jitter_fault(settings_.jitter_fault, &fault_))
			diag_logf(DiagLevel::Warn, "jitter", L"JitterFault=%hs is not a fault this build knows; none injected",
				settings_.jitter_fault.c_str());
		fault_text_ = jitter_fault_text(fault_);
		if (jitter_fault_any(fault_))
			diag_logf(DiagLevel::Warn, "jitter", L"JitterFault: %hs injected on purpose", fault_text_.c_str());
	}
	trace_runtime_ = reinterpret_cast<uintptr_t>(runtime);
	trace_begin_row();
	trace_earned_ = TraceArm{};
	{
		const JitterFrame &d = jitter_.drawn();
		ScopeTraceRow &r = trace_cur_;
		r.set(ScopeColumn::DrawnValid, d.valid ? 1.0 : 0.0);
		if (d.valid) {
			r.set(ScopeColumn::DrawnX, d.x);
			r.set(ScopeColumn::DrawnY, d.y);
			r.set(ScopeColumn::DrawnSerial, static_cast<double>(d.serial));
			r.set(ScopeColumn::DrawnIndex, d.index);
			for (uint64_t s : faulted_serials_) {
				if (s != 0 && s == d.serial)
					r.set(ScopeColumn::FaultActive, 1.0);
			}
		}
		r.set(ScopeColumn::MovedDraws, d.moved);
		r.set(ScopeColumn::PlainDraws, d.plain);
		r.set(ScopeColumn::ElsewhereDraws, d.elsewhere);
		r.set(ScopeColumn::UnknownDraws, d.unknown);
		r.set(ScopeColumn::Empty, d.empty ? 1.0 : 0.0);
		r.set(ScopeColumn::ArmedPending, d.armed_pending ? 1.0 : 0.0);
	}
	if (settings_.scope_capture && settings_.scope_key != 0 && runtime->is_key_pressed(settings_.scope_key))
		request_scope_capture();
	if (!upscaler_capture_.scope_recording()) {
		const std::wstring st = upscaler_capture_.scope_status();
		if (st != scope_logged_ && st.rfind(L"writing", 0) != 0) {
			scope_logged_ = st;
			if (!st.empty())
				diag_info("capture", L"scope capture " + st);
		}
	}

	if (reshade::api::device *const presenting = runtime->get_device();
		presenting != nullptr && pending_devices_.take(presenting))
		adopt_device(presenting);
	diag_log_environment_gpu();

	if (settings_.upscaler_capture && runtime->is_key_pressed(kUpscalerCaptureKey))
		request_upscaler_capture();
	if (!upscaler_capture_.recording()) {
		const std::wstring st = upscaler_capture_.status();
		if (st != upscaler_capture_logged_ && st.rfind(L"writing", 0) != 0) {
			upscaler_capture_logged_ = st;
			if (!st.empty())
				diag_info("capture", L"upscaler capture " + st);
		}
	}

	if (settings_.neural_toggle_key != 0 && runtime->is_key_pressed(settings_.neural_toggle_key)) {
		settings_.neural_render = !settings_.neural_render;
		if (!settings_.neural_render)
			neural_teardown_pending_ = true;
		persist_settings();
	}

	service_neural_teardown();

	if (!settings_.neural_render) {
		neural12_.accum.stop();
	}

	if (!settings_.enabled && !settings_.neural_render)
		return;

	reshade::api::device *const device = runtime->get_device();
	if (device == nullptr)
		return;
	const reshade::api::device_api api = device->get_api();
	if (is_engine_device(device))
		return;
	last_api_ = api;
	scene_jitter().watch_device(reinterpret_cast<uintptr_t>(device));

	ensure_bridge(device);
	if (bridge_ == nullptr || !bridge_->ready()) {
		const std::wstring why = std::wstring(L"no route from ") + widen(api_label(api)) +
			L" to Direct3D 12" + (bridge_error_.empty() ? L"" : L": " + bridge_error_);
		UpscalerBackend *const all[] = { &dlss11_, &dlss12_, &fsr11_, &fsr12_, &xess12_, &vsr11_ };
		for (UpscalerBackend *b : all) {
			b->status = UpscalerStatus::UnsupportedApi;
			b->last_error = why;
		}
		ngx_.status = UpscalerStatus::UnsupportedApi;
		ngx12_.status = UpscalerStatus::UnsupportedApi;
		note_frame_blocked(why);
		return;
	}

	ensure_remote();

	const PassContext pctx{ runtime, cmd_list, &pipelines_ };

	FrameInputs inputs;
	if (!resolve_frame_inputs(runtime, rtv, inputs))
		return;

	BridgeInputs bin = bridge_inputs_of(inputs, after_effects);
	SceneSnapshotTaken snap;
	if (settings_.keep_interface)
		scene_snapshot().prepare(runtime->get_device());
	if (settings_.keep_interface) {
		scene_snapshot().end_frame_at_present();
		const bool have = scene_snapshot().take(jitter_.drawn().serial, &snap) && jitter_.drawn().valid;
		if (split_gate_.step(have)) {
			bin.scene = snap.resource;
			bin.scene_state = snap.state;
		}
	}
	BridgeFrame bframe;
	const BridgeStep step = bridge_->begin(runtime, cmd_list, bin, bframe);
	if (step != BridgeStep::Ok) {
		const std::wstring why = std::wstring(L"the frame could not be carried to the engine "
			L"device, it stopped at ") + widen(bridge_step_label(step)) +
			(bridge_->last_error.empty() ? L"" : L": " + bridge_->last_error);
		UpscalerBackend *const all[] = { &dlss12_, &fsr12_, &xess12_ };
		for (UpscalerBackend *b : all) {
			b->status = UpscalerStatus::EvaluateFailed;
			b->last_error = why;
		}
		engine_.abandon_list();
		note_frame_blocked(why);
		return;
	}
	inputs.engine.device = engine_.device();
	inputs.engine.queue = engine_.queue();
	inputs.engine.cmd = bframe.cmd;
	inputs.engine.color = bframe.color;
	inputs.engine.depth = bframe.depth;
	if (bridge_ != nullptr)
		take_bridge_depth_note(inputs, bridge_->depth_note);
	inputs.engine.motion = bframe.motion_vectors;
	inputs.engine.color_state = bframe.color_state;
	inputs.engine.depth_state = bframe.depth_state;
	inputs.engine.motion_state = bframe.motion_state;
	inputs.engine.scene = bframe.scene;
	inputs.engine.scene_state = bframe.scene_state;
	inputs.engine.native = bframe.native;
	if (bframe.width != 0)
		inputs.width = bframe.width;
	if (bframe.height != 0)
		inputs.height = bframe.height;
	trace_cur_.set(ScopeColumn::Seq, static_cast<double>(++engine_frames_));
	stage_times_.enabled = settings_.gpu_timings;
	stage_times_.start_frame();
	if (stage_times_.enabled) {
		std::wstring line;
		if (stage_times_.take_report(GetTickCount64(), 10000u, &line))
			diag_info("perf", L"GPU time per frame on the engine, last 10 s, mean (frames): " + line);
	}
	stage_times_.begin(StageTimings::Stage::Total, inputs.engine.device, inputs.engine.queue, inputs.engine.cmd);
	upscaler_capture_.scope_begin(inputs.engine.device, inputs.engine.queue);
	if (upscaler_capture_.scope_recording())
		upscaler_capture_.scope_copy(ScopePlane::Drawn, inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
			inputs.engine.color_state);
	{
		const StageScope edges_time(stage_times_, StageTimings::Stage::Edges, inputs);
		fill_uncovered_edges(inputs);
	}
	{
		const StageScope observe_time(stage_times_, StageTimings::Stage::Observe, inputs);
		observed_ = observer_.begin(inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
			inputs.engine.color_state);
	}
	{
		const StageScope interface_time(stage_times_, StageTimings::Stage::Interface, inputs);
		detect_interface(inputs);
	}
	if (settings_.keep_interface)
		trace_cur_.set(ScopeColumn::Restore, hud_detected_ ? 1.0 : 0.0);
	if (hud_detected_ && upscaler_capture_.scope_recording())
		upscaler_capture_.scope_copy(ScopePlane::Mask, inputs.engine.device, inputs.engine.cmd, hud_restore_.mask(),
			HudRestoreD3D12::kMaskState);

	refresh_depth_convention(runtime);
	{
		const StageScope depth_time(stage_times_, StageTimings::Stage::Depth, inputs);
		normalize_depth(inputs);
	}
	update_native_dlss();
	run_internal_flow(runtime, inputs);
	if (hud_detected_ && inputs.have_motion_vectors && inputs.engine.motion != nullptr)
		(void)hud_restore_.clear_motion(inputs.engine.device, inputs.engine.cmd, inputs.engine.motion,
			inputs.engine.motion_state);
	last_inputs_ = inputs;
	probes_.tick_motion(pctx, settings_, inputs, history_.last_motion_px);

	last_bias_mask_bound_ = false;

	const bool is12 = (api == reshade::api::device_api::d3d12);
	UpscalerBackend *const dlss = &dlss12_;
	UpscalerBackend *fsr = &fsr12_;
	UpscalerBackend *const xess = &xess12_;
	UpscalerBackend *const vsr = &vsr11_;
	const bool vsr_possible = (api == reshade::api::device_api::d3d11);

	const bool nv = gpu_.may_be_nvidia();
	BackendSlots slots;
	slots.is_d3d12 = is12;
	slots.vendor = gpu_.vendor;

	slots.vsr = nv
		? BackendAvailability{ vsr_possible && vsr->runtime_present(), vsr->crashed, vsr->status }
		: BackendAvailability{ false, false, UpscalerStatus::UnsupportedGpu };
	const float frame_time_ms = clock_.frame_time_ms();
	if (inputs.have_motion_vectors) {
		slots.dlss = nv
			? availability_of(dlss, BackendChoice::Dlss)
			: BackendAvailability{ false, false, UpscalerStatus::UnsupportedGpu };
		slots.fsr = availability_of(fsr, BackendChoice::Fsr);
		if (xess != nullptr)
			slots.xess = availability_of(xess, BackendChoice::Xess);
		if (api == reshade::api::device_api::d3d11 &&
			fsr12_.status == UpscalerStatus::InitFailed) {
			slots.fsr_bridge_failed = true;
			slots.fsr11 = BackendAvailability{ fsr11_.runtime_present(), fsr11_.crashed, fsr11_.status };
		}
	}

	const FramePlan plan = plan_frame(settings_, signals_of(inputs, color_space_, jitter_.drawn()),
		slots, history_, frame_time_ms);
	history_ = plan.next;
	last_plan_ = plan;
	if (plan.use_fsr11)
		fsr = &fsr11_;
	trace_cur_.plan = jitter_decision_name(plan.jitter_decision);
	trace_cur_.set(ScopeColumn::InFrame, plan.params.jitter_in_frame ? 1.0 : 0.0);
	trace_cur_.set(ScopeColumn::ShiftX, plan.split_shift_x);
	trace_cur_.set(ScopeColumn::ShiftY, plan.split_shift_y);
	const bool told_prev_set = told_prev_set_;
	const float told_prev_x = told_prev_x_, told_prev_y = told_prev_y_;
	told_prev_set_ = true;
	told_prev_x_ = plan.params.jitter_x;
	told_prev_y_ = plan.params.jitter_y;

	if (plan.outcome == UpscalerOutcome::NoMotionVectors) {

		UpscalerBackend *const waiting = nv ? dlss : fsr;
		waiting->status = UpscalerStatus::NeedMotionVectors;
		jitter_.note_waiting();
		waiting->last_error = flow_note_;
		active_ = waiting;

		run_engine_work(runtime, inputs, plan, nullptr, UpscalerParams{});
		finish_frame(runtime, cmd_list, inputs);
		return;
	}

	if (plan.outcome == UpscalerOutcome::NoBackend) {
		active_ = nv ? dlss : fsr;
		if (plan.no_upscaler_wanted)
			active_->status = UpscalerStatus::Idle;

		run_engine_work(runtime, inputs, plan, nullptr, UpscalerParams{});
		finish_frame(runtime, cmd_list, inputs);
		return;
	}

	active_ = plan.backend_index == 0 ? dlss
		: (plan.backend_index == 1 ? fsr : (plan.backend_index == 2 ? xess : vsr));

	if (!nv && (plan.backend_index == 0 || plan.backend_index == 3)) {
		active_->status = UpscalerStatus::UnsupportedGpu;
		active_->last_error = plan.backend_index == 0
			? L"DLSS needs an NVIDIA GPU. Pick FSR or XeSS, or leave the upscaler on Auto."
			: L"RTX VSR is an NVIDIA driver feature. Without it the video processor would "
			  L"still shrink and re-enlarge the frame for nothing, so the frame is left alone.";
		run_engine_work(runtime, inputs, plan, nullptr, UpscalerParams{});
		finish_frame(runtime, cmd_list, inputs);
		return;
	}

	fsr12_.want_provider = settings_.fsr_provider;

	UpscalerParams params = plan.params;
	if (jitter_fault_any(fault_) && (plan.jitter_decision == JitterDecision::Drawn ||
			plan.jitter_decision == JitterDecision::Split) &&
		apply_told_fault(fault_, fault_clock(0), plan.jitter_decision == JitterDecision::Split, told_prev_set,
			told_prev_x, told_prev_y, &params.jitter_x, &params.jitter_y))
		trace_cur_.set(ScopeColumn::FaultActive, 1.0);

	if (active_is_dlss()) {
		const StageScope mask_time(stage_times_, StageTimings::Stage::Mask, inputs);
		params.bias_mask = guides_.bias_mask(pctx, settings_, inputs).handle;
		last_bias_mask_bound_ = params.bias_mask != 0;
	}

	run_engine_work(runtime, inputs, plan, active_, params);

	if (finish_frame(runtime, cmd_list, inputs))
		jitter_.note_reached_game();
	arm_scene_jitter(runtime, inputs);

	float motion_px = 0.0f;
	if (probes_.sample_flow(pctx, inputs, motion_px))
		history_.last_motion_px = motion_px;

	note_state();
}

bool App::finish_frame(reshade::api::effect_runtime *runtime,
	reshade::api::command_list *cmd_list, const FrameInputs &inputs)
{
	if (bridge_ == nullptr || !inputs.engine.ready())
		return false;
	stage_times_.end(StageTimings::Stage::Total, inputs.engine.cmd);
	const BridgeStep step = bridge_->end(runtime, cmd_list, inputs.color);
	if (step == BridgeStep::Ok)
		return true;
	engine_.abandon_list();
	diag_state("interop", DiagLevel::Warn, "interop",
		L"the upscaled frame did not reach the game: " + widen(bridge_step_label(step)) +
		(bridge_->last_error.empty() ? L"" : L" - " + bridge_->last_error));
	return false;
}

std::string App::scope_header()
{
	std::string h = "source=addon\n";
	const auto put = [&h](const char *k, const std::string &v) { h += std::string(k) + "=" + v + "\n"; };
	put("api", api_label(last_api_));
	put("display_w", std::to_string(last_inputs_.width));
	put("display_h", std::to_string(last_inputs_.height));
	uint32_t rw = 0, rh = 0;
	if (!active_render_size(last_inputs_, &rw, &rh)) {
		rw = last_inputs_.width;
		rh = last_inputs_.height;
	}
	put("render_w", std::to_string(rw));
	put("render_h", std::to_string(rh));
	put("resolver", active_is_fsr() ? "fsr" : active_is_dlss() ? "dlss" : active_is_xess() ? "xess"
		: active_is_vsr() ? "vsr" : "none");
	put("fsr_provider", settings_.fsr_provider);
	put("upscale_mode", std::to_string(settings_.upscale_mode));
	put("no_depth", last_inputs_.depth_provider == DepthProvider::None ? "1" : "0");
	put("restore", settings_.keep_interface ? "on" : "off");
	put("jitter_phases", std::to_string(jitter_phases()));
	put("fault", fault_text_);
	put("fault_params", settings_.jitter_fault);
	put("label", "");
	return h;
}

PanelState App::panel_state(bool for_overlay)
{
	const bool d3d12 = last_api_ == reshade::api::device_api::d3d12;
	PanelState s;
	s.d3d12 = d3d12;
	s.build_stamp = __DATE__ " " __TIME__;
	s.version = AEONSR_VERSION_STR;
	s.report_path = report_path_;
	s.report_error = report_error_;
	s.log_path = diag_log_path();

	s.native_dlss = native_dlss_seen_;
	s.native_dlss_modules = native_dlss_.modules;
	s.gpu_vendor = gpu_.vendor;
	s.gpu_vendor_id = gpu_.vendor_id;
	s.gpu_name = gpu_.name;
	s.active_name = active_ != nullptr ? active_->name() : nullptr;
	s.active_status = active_status();
	s.active_error = active_error();
	s.active_is_dlss = active_is_dlss();
	s.active_is_fsr = active_is_fsr();
	s.active_is_xess = active_is_xess();
	s.active_is_vsr = active_is_vsr();
	s.bridge_name = bridge_ != nullptr ? bridge_->name() : nullptr;
	s.bridge_sync = bridge_ != nullptr ? bridge_->sync_name() : nullptr;
	s.bridge_kind = bridge_ != nullptr ? bridge_->kind() : BridgeKind::None;
	s.bridge_error = bridge_error_;
	s.api_name = api_label(last_api_);
	s.bridge_any_ready = bridge_ != nullptr && bridge_->ready();
	s.bridge_fsr_adapter_matched = engine_.adapter_matched();
	s.active_on12 = bridge_ != nullptr && bridge_->kind() != BridgeKind::Native12;
	s.vsr_possible = last_api_ == reshade::api::device_api::d3d11;

	const bool probe = for_overlay || settings_.debug_info;
	const bool fsr_is_11 = active_ == static_cast<const UpscalerBackend *>(&fsr11_);
	if (s.active_is_fsr) {
		UpscalerBackend &fsr = fsr_is_11
			? static_cast<UpscalerBackend &>(fsr11_) : static_cast<UpscalerBackend &>(fsr12_);
		s.upscaler_dll_present = probe ? fsr.runtime_present() : fsr.status != UpscalerStatus::MissingRuntime;
		s.upscaler_initialized = fsr_is_11 ? fsr11_.fsr_initialized() : fsr12_.fsr_initialized();
		s.upscaler_out_width = fsr_is_11 ? fsr11_.out_width() : fsr12_.out_width();
		s.upscaler_out_height = fsr_is_11 ? fsr11_.out_height() : fsr12_.out_height();
		s.upscaler_render_width = fsr_is_11 ? fsr11_.fsr_render_width() : fsr12_.fsr_render_width();
		s.upscaler_render_height = fsr_is_11 ? fsr11_.fsr_render_height() : fsr12_.fsr_render_height();
		s.upscaler_bridge_ready = s.bridge_any_ready;
		s.upscaler_bridge_adapter_matched = engine_.adapter_matched();

		if (!fsr_is_11) {
			if (!fsr12_.fsr_loading()) {
				s.fsr_provider_version = fsr12_.provider_version;
				for (const auto &pv : fsr12_.providers)
					s.fsr_providers.push_back(pv.name);
			}
		} else if (for_overlay) {
			s.fsr_dx12_dlls_present = false;
			for (const std::wstring &p : runtime_candidates(
					fsr_search_dirs(module_directory(module_), exe_directory_w()),
					L"amd_fidelityfx_upscaler_dx12.dll")) {
				if (file_exists_w(p)) {
					s.fsr_dx12_dlls_present = true;
					break;
				}
			}
			s.fsr11_loaded_dll_name = fsr11_.loaded_dll_name;
		}
	} else if (s.active_is_xess) {
		UpscalerBackend &xess = xess12_;
		s.upscaler_dll_present = probe ? xess.runtime_present() : xess.status != UpscalerStatus::MissingRuntime;
		s.upscaler_initialized = xess12_.xess_initialized();
		s.upscaler_out_width = xess12_.out_width();
		s.upscaler_out_height = xess12_.out_height();
		s.upscaler_render_width = xess12_.xess_render_width();
		s.upscaler_render_height = xess12_.xess_render_height();
		s.upscaler_bridge_ready = s.bridge_any_ready;
	} else if (s.active_is_vsr) {

		s.upscaler_dll_present = !d3d12;
		s.upscaler_initialized = vsr11_.vsr_initialized();
		s.upscaler_out_width = vsr11_.out_width();
		s.upscaler_out_height = vsr11_.out_height();
		s.upscaler_render_width = vsr11_.vsr_render_width();
		s.upscaler_render_height = vsr11_.vsr_render_height();
		const VsrProcessorStatus &v = vsr11_.vsr_status();
		s.vsr_capable = v.capable;
		s.vsr_enabled = v.enabled;
		s.vsr_in_use = v.in_use;
		s.vsr_level = v.level;
	}
	if (active_ == static_cast<const UpscalerBackend *>(&fsr11_))
		s.game_fsr_modules = fsr11_.game_fsr_modules;
	else if (!fsr12_.fsr_loading())
		s.game_fsr_modules = fsr12_.game_fsr_modules;
	s.game_xess_modules = xess12_.game_xess_modules;
	if (!dlss12_.dlss_loading())
		s.game_ngx_modules = ngx12_.game_ngx_modules;

	const NgxSession &session = active_ == static_cast<const UpscalerBackend *>(&dlss11_)
		? static_cast<const NgxSession &>(ngx_) : static_cast<const NgxSession &>(ngx12_);
	s.ngx_dll_present = session.dll_present;
	s.ngx_initialized = session.ngx_initialized;
	s.ngx_width = session.width;
	s.ngx_height = session.height;
	s.ngx_render_width = session.render_width;
	s.ngx_render_height = session.render_height;
	s.ngx_eval_count = session.eval_count;
	s.ngx_upscale_mode = session.cfg.quality_mode;
	s.ngx_render_preset = session.cfg.render_preset;

	{
		const NeuralRenderCommon &nr = neural();

		s.neural_dll_found = nr.dll_present && nr.shim_present;
		s.neural_dll_present = nr.dll_present;
		s.neural_shim_present = nr.shim_present;
		s.neural_initialized = nr.initialized;
		s.neural_crashed = nr.crashed;
		s.neural_driver_too_old = nr.driver_too_old();
		s.neural_status = nr.status;
		s.neural_error = nr.last_error;
		s.neural_driver_major = nr.driver_major;
		s.neural_driver_minor = nr.driver_minor;
		s.neural_required_driver_major = nr.required_driver_major;
		s.neural_required_driver_minor = nr.required_driver_minor;
		s.neural_requirement_known = nr.requirement_known;
		s.neural_adapter_unsupported = nr.adapter_unsupported();
		s.neural_min_architecture = nr.min_architecture;
		s.neural_gpu_architecture = nr.card_architecture;
		s.neural_runtime_serves_card = nr.runtime_serves_card();
		s.neural_allgpu_build_targets = neural_allgpu_build_targets(nr.card_architecture);
		s.neural_runtime_kernels.clear();
		s.neural_runtime_cards.clear();
		if (nr.runtime_targets_known) {
			s.neural_runtime_kernels = neural_kernels_label(nr.runtime_kernels);
			s.neural_runtime_cards = neural_kernels_cards(nr.runtime_kernels);
			s.neural_runtime_file = nr.dll_path;
			s.neural_runtime_version = nr.runtime_identity.file_version;
			s.neural_runtime_certificate = nr.runtime_identity.has_certificate;
			s.neural_runtime_sha256 = nr.runtime_sha256;
		}
		s.neural_runtime_candidates = nr.runtime_candidates;
		s.neural_runtime_minimum = nr.runtime_minimum_architecture;
		s.neural_reported_architecture = nr.reported_architecture;
		s.neural_feature_unsupported = nr.feature_unsupported;
		s.neural_eval_count = nr.eval_count;
		s.neural_last = neural_last_;
		s.neural_init_failed = neural_init_failed_;
		s.neural_via_bridge = last_api_ == reshade::api::device_api::d3d11;
		s.neural_color_space = color_space_;
		const NeuralRenderD3D12 &nr12 = neural12_;
		s.neural_model_width = nr12.model_width;
		s.neural_model_height = nr12.model_height;
		s.neural_passes_built = nr12.created_passes;
		s.neural_gpu_ms = static_cast<float>(nr12.timer.last_ms);
		s.neural_eval_rows = nr12.last_eval_rows;
		s.neural_area_share = nr12.last_area_share;
		s.neural_capture_status = nr12.capture.last;
		s.accum_running = nr12.accum.running;
		s.accum_holding = nr12.accum.step() == AccumStep::Hold;
		s.accum_done = nr12.accum.done;
		s.accum_target = nr12.accum.target;
	}

	s.motion_provider = last_inputs_.motion_provider;
	s.flow_note = flow_note_;
	s.flow_note_benign = flow_note_benign_;
	s.motion_wanted = motion_wanted_;
	s.depth_provider = last_inputs_.depth_provider;
	s.depth_reversed = depth_how_.reversed;
	s.depth_logarithmic = depth_how_.logarithmic;
	s.depth_far_plane = depth_how_.far_plane;
	s.depth_upside_down = depth_how_.upside_down;
	s.depth_mirrored = depth_how_.mirrored;
	s.depth_overridden.clear();
	for (const std::string &name : depth_resolved_.overridden)
		s.depth_overridden += (s.depth_overridden.empty() ? "" : ", ") + name;
	s.have_global_flow = last_inputs_.have_global_flow;
	s.have_motion_confidence = last_inputs_.have_motion_confidence;
	s.color_info = last_inputs_.color_info;
	s.motion_info = last_inputs_.motion_info;
	s.depth_info = last_inputs_.depth_info;
	s.probe_unsupported = probes_.mv.unsupported;
	s.probe_valid = probes_.mv.valid;
	s.probe_moving = probes_.mv_moving;
	s.probe_nonzero_pct = probes_.mv.nonzero_pct;
	s.probe_mean_px = probes_.mv.mean_px;
	s.probe_max_px = probes_.mv.max_px;
	s.scene_state = last_plan_.scene_state;
	s.reset_this_frame = last_plan_.params.reset;
	s.camera_cut = last_plan_.camera_cut;
	s.frame_time_ms = last_plan_.params.frame_time_ms;
	s.jitter_active = last_plan_.jitter_active;
	s.jitter_refused = dlss11_.rt != nullptr && dlss11_.rt->jitter_failed;
	s.jitter_in_frame = last_plan_.params.jitter_in_frame;
	s.jitter_mixed = jitter_.drawn().ambiguous;
	s.jitter_still = jitter_.drawn().empty;
	s.jitter_moved_draws = jitter_.drawn().moved;
	s.jitter_plain_draws = jitter_.drawn().plain;
	s.jitter_elsewhere_draws = jitter_.drawn().elsewhere;
	s.jitter_aliased_draws = jitter_.drawn().aliased;
	s.jitter_replayed_draws = jitter_.drawn().replayed;
	s.jitter_held_lists = scene_jitter().live_holds();
	s.jitter_note = jitter_note_;
	s.scope_capture_enabled = settings_.scope_capture;
	s.scope_status = upscaler_capture_.scope_status();
	s.scope_fault = fault_text_;
	if (trace_presents_ != 0 && trace_ring_.size() == kTraceRing) {
		const auto at = [this](uint64_t back) -> const ScopeTraceRow & {
			return trace_ring_[(trace_presents_ - 1u - back) % kTraceRing];
		};
		const float dw = static_cast<float>(last_inputs_.width);
		const auto display_scale = [dw](const ScopeTraceRow &r) {
			const double rw = r.get(ScopeColumn::RenderW);
			return r.has(ScopeColumn::RenderW) && rw > 0.0 && dw > 0.0 ? dw / static_cast<float>(rw) : 1.0f;
		};
		const ScopeTraceRow &last = at(0);
		s.scope_have = true;
		s.scope_plan = last.plan;
		s.scope_computed_known = last.has(ScopeColumn::ComputedDrawX);
		s.scope_computed_x = static_cast<float>(last.get(ScopeColumn::ComputedDrawX));
		s.scope_computed_y = static_cast<float>(last.get(ScopeColumn::ComputedDrawY));
		s.scope_drawn_known = last.has(ScopeColumn::DrawnX);
		s.scope_drawn_x = static_cast<float>(last.get(ScopeColumn::DrawnX));
		s.scope_drawn_y = static_cast<float>(last.get(ScopeColumn::DrawnY));
		s.scope_told_known = last.has(ScopeColumn::ToldX);
		s.scope_told_x = static_cast<float>(last.get(ScopeColumn::ToldX)) * display_scale(last);
		s.scope_told_y = static_cast<float>(last.get(ScopeColumn::ToldY)) * display_scale(last);
		s.scope_armed_serial = last.has(ScopeColumn::ArmedSerial)
			? static_cast<uint64_t>(last.get(ScopeColumn::ArmedSerial)) : 0u;
		s.scope_drawn_serial = last.has(ScopeColumn::DrawnSerial)
			? static_cast<uint64_t>(last.get(ScopeColumn::DrawnSerial)) : 0u;
		const double now = last.get(ScopeColumn::TimeMs);
		const uint64_t rows = trace_presents_ < kTraceRing ? trace_presents_ : kTraceRing;
		for (uint64_t k = 0; k < rows; ++k) {
			const ScopeTraceRow &r = at(k);
			if (!(now - r.get(ScopeColumn::TimeMs) < 1000.0))
				break;
			++s.scope_second_frames;
			if (r.get(ScopeColumn::DrawnValid) == 1.0 && r.get(ScopeColumn::DrawnSerial) != r.get(ScopeColumn::ArmedSerial))
				++s.scope_lag_frames;
			if (r.get(ScopeColumn::Empty) == 1.0 || r.plan == "empty")
				++s.scope_empty_frames;
		}
		const uint64_t plotted = rows < 120u ? rows : 120u;
		s.scope_residual_x.reserve(static_cast<size_t>(plotted));
		s.scope_residual_y.reserve(static_cast<size_t>(plotted));
		for (uint64_t k = plotted; k-- > 0;) {
			const ScopeTraceRow &r = at(k);
			float rx = 0.0f, ry = 0.0f;
			if ((r.plan == "drawn" || r.plan == "split") && r.has(ScopeColumn::ToldX) && r.has(ScopeColumn::DrawnX)) {
				const float sc = display_scale(r);
				const float moved = r.get(ScopeColumn::SplitApplied) == 1.0 ? 1.0f : 0.0f;
				rx = static_cast<float>(r.get(ScopeColumn::ToldX)) * sc -
					(static_cast<float>(r.get(ScopeColumn::DrawnX)) - moved * static_cast<float>(r.get(ScopeColumn::ShiftX)) * sc);
				ry = static_cast<float>(r.get(ScopeColumn::ToldY)) * sc -
					(static_cast<float>(r.get(ScopeColumn::DrawnY)) - moved * static_cast<float>(r.get(ScopeColumn::ShiftY)) * sc);
			}
			s.scope_residual_x.push_back(rx);
			s.scope_residual_y.push_back(ry);
		}
	}
	s.last_motion_px = history_.last_motion_px;
	s.debug_view_stage = debug_view_.last;
	s.debug_view_note = debug_view_.note;
	s.bias_mask_bound = last_bias_mask_bound_;

#if defined(AEONSR_NO_VENDOR)
	s.upscalers_remote = true;
#endif
	s.host_ready = remote_.ready();

	if (s.upscalers_remote && !s.host_ready) {
		s.neural_dll_found = true;
		s.neural_dll_present = true;
		s.neural_shim_present = true;
		s.upscaler_dll_present = true;
	}

	if (s.upscalers_remote && s.host_ready) {
		s.host_pid = remote_.host_pid();
		const bool neural_there = remote_.neural_present();
		s.neural_dll_found = neural_there;
		s.neural_dll_present = neural_there;
		s.neural_shim_present = neural_there;
		if (s.active_is_dlss)
			s.upscaler_dll_present = remote_.backend_present(BackendChoice::Dlss);
		else if (s.active_is_fsr)
			s.upscaler_dll_present = remote_.backend_present(BackendChoice::Fsr);
		else if (s.active_is_xess)
			s.upscaler_dll_present = remote_.backend_present(BackendChoice::Xess);
		if (remote_out_width_ != 0) {
			s.upscaler_out_width = remote_out_width_;
			s.upscaler_out_height = remote_out_height_;
		}
	}

	s.upscaler_label = upscaler_display_name(s.active_name, s.active_is_fsr,
		diag_narrow(s.fsr_provider_version));
	return s;
}

void App::apply_panel_actions(const PanelActions &actions)
{
	if (actions.neural_restart)
		neural_teardown_pending_ = true;
	if (actions.neural_capture)
		request_neural_capture();

	if (actions.accum_start || actions.accum_stop) {
		NeuralRenderD3D12 &nr = neural12_;
		if (actions.accum_start) {
			nr.accum.start(clamp_accum_iterations(settings_.accum_iterations));
			diag_logf(DiagLevel::Info, "neural",
				L"screenshot accumulation: %u passes over one frozen frame",
				nr.accum.target);
		} else {
			diag_logf(DiagLevel::Info, "neural",
				L"screenshot accumulation released after %u passes", nr.accum.done);
			nr.accum.stop();
			nr.delta_valid = false;
		}
	}
	if (actions.probe_reset)
		probes_.reset_motion();
	if (actions.scope_capture && settings_.scope_capture)
		request_scope_capture();
	if (actions.reset)
		history_.reset_pending = true;
	if (actions.invalidate_history)
		invalidate_temporal_history();
	if (actions.persist) {
		diag_set_verbose(settings_.verbose_log);
		persist_settings();
	}
}

void App::draw_overlay(reshade::api::effect_runtime *)
{
	neural_runtime_present();
	const PanelState state = panel_state(true);
	PanelActions actions;
	panel::draw_overlay(state, settings_, actions);
	if (actions.save_report || actions.copy_report)
		write_report(state, actions.copy_report);
	apply_panel_actions(actions);
}

void App::write_report(const PanelState &state, bool to_clipboard)
{
	diag_collect_environment(module_);
	if (to_clipboard) {
		ImGui::SetClipboardText(diag_narrow(diag_report_text(state, settings_)).c_str());
		report_path_ = L"copied to the clipboard";
		report_error_.clear();
		return;
	}
	report_path_ = diag_write_report(state, settings_, &report_error_);
}

void App::draw_osd(reshade::api::effect_runtime *)
{
	if (!settings_.show_osd)
		return;
	panel::draw_osd(panel_state(false), settings_);
}

App *app() noexcept
{
	return g_app.get();
}

void create_app(HMODULE module)
{
	g_app = std::make_unique<App>(module);
}

void destroy_app()
{
	if (g_app)
		g_app->shutdown();
	g_app.reset();
}

}
