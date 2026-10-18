#include "aeon_sr/core/app.hpp"

#include "aeon_sr/jitter/scene_snapshot.hpp"
#include "aeon_sr/jitter/vulkan_constants.hpp"
#include "aeon_sr/jitter/vulkan_viewport_hook.hpp"
#include "aeon_sr/core/diag_report.hpp"
#include "aeon_sr/jitter/gl_jitter.hpp"
#include "aeon_sr/jitter/scene_jitter_shaders.hpp"
#include "aeon_sr/interop/interop_bridges.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/interop/native_d3d.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/core/game_render_scale.hpp"
#include "aeon_sr/core/i18n.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/ngx/ngx_override.hpp"
#include "aeon_sr/core/runtime_search.hpp"
#include "aeon_sr/ngx/neural_hardware.hpp"
#include "aeon_sr/upscalers/runtime_load_check.hpp"

#include <limits>
#include <d3d11.h>
#include <d3d12.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
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
	vsr11_.engine = &engine_;
	fsr11_.addon_dir = addon_dir;
	fsr12_.addon_dir = addon_dir;
	xess12_.addon_dir = addon_dir;
	neural12_.addon_dir = addon_dir;
	neural12_.data_dir = data_dir;

	ini_path_ = join_path(addon_dir, L"AeonSR.ini");
	load_settings(settings_, ini_path_);
	diag_set_verbose(settings_.verbose_log);
	set_jitter_shaders_wanted(settings_.spatial_jitter);
	flight_.set_log([](const std::wstring &line) { diag_info("flight", line); });
	flight_.set_dir(join_path(addon_dir, L"AeonSR_flight"));
	if (!parse_flight_fault(settings_.flight_fault, &flight_fault_))
		diag_logf(DiagLevel::Warn, "flight", L"FlightFault=%hs is not a fault this build knows; none injected",
			settings_.flight_fault.c_str());
	else if (flight_fault_.count != 0)
		diag_logf(DiagLevel::Warn, "flight", L"FlightFault: %hs fired on purpose", settings_.flight_fault.c_str());
	game_ = games::integration_for_process();
	if (game_ != nullptr) {
		std::wstring why;
		const std::wstring title = widen(game_->entry().title);
		if (game_->attach(&why)) {
			game_note_ = game_->note();
			diag_info("game", title + L": " + game_note_);
		} else {
			game_note_ = why;
			diag_logf(DiagLevel::Warn, "game", L"%ls: its render scale is left to the game (%ls)", title.c_str(),
				why.c_str());
		}
	}
	update_game_scale_hooks();
}

namespace {

FrameSignals signals_of(const FrameInputs &inputs, NeuralColorSpace color_space,
	const JitterFrame &drawn, bool jitter_withheld, bool camera_moving) noexcept
{
	FrameSignals s;
	s.camera_moving = camera_moving;
	s.color_space = color_space;
	s.jitter_drawn = drawn.valid;
	s.jitter_drawn_index = drawn.index;
	s.jitter_drawn_x = drawn.input_x;
	s.jitter_drawn_y = drawn.input_y;
	s.jitter_drawn_ambiguous = drawn.ambiguous;
	s.jitter_drawn_empty = drawn.empty;
	s.jitter_withheld = jitter_withheld;
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
	if (!settings_.spatial_jitter) {
		landing_.clear_absorbed();
		flow_.reset_landing();
		depth_proof_.reset();
	}

	invalidate_temporal_history();
	save_settings(settings_, ini_path_);
}

void App::invalidate_temporal_history() noexcept
{
	history_.temporal_history_valid = false;
}

void App::keep_game_jitter_memory() noexcept
{
	if (!memory_unsaved_ || game_memory_path_.empty())
		return;
	try {
		if (save_game_jitter_memory(game_memory_path_, game_memory_))
			memory_unsaved_ = false;
	} catch (...) {
	}
}

void App::shutdown()
{
	keep_game_jitter_memory();
	reflex_.detach();
	latency_queue_off();
	if (latency_timer_ != nullptr) {
		CloseHandle(static_cast<HANDLE>(latency_timer_));
		latency_timer_ = nullptr;
	}
#if AEONSR_FRAME_GENERATION
	pace_off();
	if (pace_timer_ != nullptr) {
		CloseHandle(static_cast<HANDLE>(pace_timer_));
		pace_timer_ = nullptr;
	}
#endif
	if (game_ != nullptr)
		game_->detach();
	flight_.shutdown();
	{
		const FlightRecorder::Cost c = flight_.cost();
		if (c.frames != 0)
			diag_logf(DiagLevel::Info, "flight", L"flight recorder: %llu frames recorded at %.2f us a frame on average, "
				L"%.1f us at most; %u dumps, the largest copy into one %.3f ms",
				static_cast<unsigned long long>(c.frames), c.total_us / static_cast<double>(c.frames), c.max_us, c.dumps,
				c.copy_ms_max);
	}
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
	game_probe_.release();
	depth_.release();
	shutdown_neural();
	ngx_.shutdown_device();
	ngx12_.shutdown_device();
#if AEONSR_FRAME_GENERATION
	framegen_.off();
#endif
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
	++remote_extras_.neural_restart_epoch;
}

bool App::neural_remote_now() const noexcept
{
	if (!remote_.ready())
		return false;
	if constexpr (sizeof(void *) == 4)
		return true;
	return engine_route_now().neural_in_host;
}

void App::request_neural_capture()
{

	SYSTEMTIME t{};
	GetLocalTime(&t);
	wchar_t stamp[32]{};
	_snwprintf_s(stamp, _TRUNCATE, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	const std::wstring dir = join_path(join_path(neural12_.addon_dir, L"captures"), stamp);
	neural12_.request_capture(dir);
	remote_extras_.capture_dir = dir;
	remote_extras_.capture_frames = kNeuralCaptureBurst;
	++remote_extras_.capture_epoch;
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
	bool unknown = false;
	const uint32_t planes = scope_planes_from_names(settings_.scope_planes, &unknown);
	if (unknown)
		diag_warn("capture", std::wstring(L"ScopePlanes has a name that is no plane (drawn, upscaler_in, upscaled, "
			L"final, mask, neural, motion, depth, separated by commas); ") +
			(planes == 0 ? L"every plane is kept" : L"the planes it names are kept"));
	upscaler_capture_.request_scope(dir, settings_.scope_frames, UpscalerCaptureD3D12::kScopeMaxBytes,
		planes != 0 ? planes : UpscalerCaptureD3D12::kScopeAllPlanes);
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

namespace {

void neural_files_beside(const std::wstring &dir, bool *dll, bool *shim)
{
	const std::vector<std::wstring> found = neural_runtime_candidates(dir, dir);
	*dll = !found.empty();
	const auto shim_in = [](const std::wstring &d) {
		return !d.empty() && GetFileAttributesW(join_path(join_path(d, L"ngxshim"), L"nvngx.dll").c_str()) !=
			INVALID_FILE_ATTRIBUTES;
	};
	std::wstring runtime_dir;
	if (*dll) {
		const size_t cut = found.front().find_last_of(L"\\/");
		if (cut != std::wstring::npos)
			runtime_dir = found.front().substr(0, cut);
	}
	*shim = shim_in(dir) || shim_in(runtime_dir);
}

}

const NeuralRenderCommon &App::neural() const noexcept
{
	return static_cast<const NeuralRenderCommon &>(neural12_);
}

bool App::neural_runtime_present() noexcept
{

	if (!gpu_.may_be_nvidia())
		return false;
	return neural12_.probe_dll_present();
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

void App::restore_interface(const FrameInputs &inputs)
{
	if (!hud_detected_ || interface_restored_)
		return;
	interface_restored_ = true;
	const StageScope restore_time(stage_times_, StageTimings::Stage::Restore, inputs);
	engine_journal_note(L"interface");
	(void)hud_restore_.composite(inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
		inputs.engine.color_state);
}

void App::run_engine_work(reshade::api::effect_runtime *runtime, const FrameInputs &inputs,
	const FramePlan &plan, UpscalerBackend *backend, const UpscalerParams &params)
{
	scope_list_lost_ = false;
	interface_restored_ = false;
	bool registered = false;
	if (hud_detected_ && !interface_split_ && backend != nullptr && backend != &fsr11_) {
		const StageScope register_time(stage_times_, StageTimings::Stage::Register, inputs);
		registered = hud_restore_.register_input(inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
			inputs.engine.color_state);
	}
	trace_cur_.set(ScopeColumn::Register, registered ? 1.0 : 0.0);
	run_engine_passes(runtime, inputs, plan, backend, params);
	restore_interface(inputs);
	if (observed_) {
		hold_drawn_[observe_serial_ % kHoldDrawn].split = interface_split_;
		const StageScope observe_end_time(stage_times_, StageTimings::Stage::ObserveEnd, inputs);
		observer_.end(inputs.engine.device, inputs.engine.cmd, inputs.engine.color, inputs.engine.color_state,
			hud_detected_ ? hud_restore_.mask() : nullptr, HudRestoreD3D12::kMaskState,
			inputs.have_motion_vectors ? inputs.engine.motion : nullptr, inputs.engine.motion_state, observe_serial_);
	}
	observed_ = false;
	hud_detected_ = false;
	interface_split_ = false;
	const FrameInputs &shown = game_scale_.active && game_scale_.done ? game_scale_shown_ : inputs;
	debug_view_.apply(settings_.debug_view, shown, depth_how_);
	if (upscaler_capture_.scope_recording() && !scope_list_lost_)
		upscaler_capture_.scope_copy(ScopePlane::Final, shown.engine.device, shown.engine.cmd, shown.engine.color,
			shown.engine.color_state);
}

void App::note_picture_hold()
{
	FrameEdges e;
	while (observer_.take_edges(&e)) {
		const HoldDrawn &a = hold_drawn_[e.serial % kHoldDrawn], &b = hold_drawn_[(e.serial - 1u) % kHoldDrawn];
		if (a.serial == e.serial && a.split && !picture_hold_.held())
			split_cover_.feed(e.shown, e.pixels);
		const bool moved = e.serial > 1u && a.serial == e.serial && b.serial + 1u == e.serial && a.scene && b.scene &&
			(a.x != b.x || a.y != b.y);
		const bool still = e.edges >= PictureHold::kMinEdges &&
			static_cast<float>(e.changed) <= PictureHold::kHeldShare * static_cast<float>(e.edges);
		held_frames_ += moved && still ? 1u : 0u;
		if (moved) {
			moved_frames_ += 1u;
			moved_edges_ += e.edges;
			moved_changed_ += e.changed;
		}
		const PictureHold::Change change = picture_hold_.feed(moved, e.edges, e.changed);
		if (change == PictureHold::Change::None)
			continue;
		landing_.clear_absorbed();
		flow_.reset_landing();
		if (change == PictureHold::Change::Moves) {
			history_.reset_pending = true;
			invalidate_temporal_history();
		}
		diag_logf(DiagLevel::Info, "interface", change == PictureHold::Change::Holds
			? L"interface: the picture holds still while the offset drawn into the scene moves (%u of %u edge pixels "
				L"changed): the scene does not reach the window, and the frame is shown as the game drew it"
			: L"interface: the picture moves again (%u of %u edge pixels changed)", e.changed, e.edges);
	}
}

void App::detect_interface(const FrameInputs &inputs)
{
	hud_detected_ = false;
	interface_game_ = false;
	if (!settings_.keep_interface || inputs.engine.device == nullptr || inputs.engine.cmd == nullptr ||
		inputs.engine.color == nullptr)
		return;
	const JitterFrame &drawn = jitter_.drawn();
	if (settings_.spatial_jitter) {
		HudRestoreFrame f;
		f.drawn = drawn.valid;
		f.x = drawn.valid ? drawn.x : 0.0f;
		f.y = drawn.valid ? drawn.y : 0.0f;
		f.game_jitters = !drawn.valid && offset_withheld() && !fault_.withheld_no_restore;
		f.scene_moved = camera_moving_;
		f.scene = inputs.engine.scene;
		f.scene_state = inputs.engine.scene_state;
		f.alone = jitter_interface_alone(drawn, depth_proof_.armed_proven) || picture_hold_.held();
		if (f.alone)
			flight_.cur().hud |= picture_hold_.held() ? kFlightHeld : kFlightAlone;
		diag_state("interface_alone", DiagLevel::Info, "interface", f.alone
			? std::wstring(L"interface: the frame draws no scene (a menu, a loading screen); it is shown as the game "
				L"drew it")
			: std::wstring(L"interface: the frame draws the scene"));
		engine_journal_note(L"interface detect");
		hud_detected_ = hud_restore_.detect(inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
			inputs.engine.color_state, f);
		interface_game_ = hud_detected_ && f.game_jitters;
	} else {
		hud_restore_.reset();
		interface_game_ = false;
	}
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
			flight_.cur().hud |= kFlightSplit;
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
			L"vertices or fewer, %u other; elsewhere %u not of the scene (%u of six or fewer, %u into the interface's own "
			L"layer), most into %ux%u", census.window_scene, census.window_quads, census.window_other, census.elsewhere,
			census.elsewhere_quads, census.layer, census.elsewhere_w, census.elsewhere_h);
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
				L"a quarter pixel, %.3f px on average; the game's frame changed %.2f%% of its edge pixels (%.2f%% on "
				L"the %u frames the offset drawn moved on), and held still on %u of those", o.frames,
				100.0 * o.hud / o.pixels,
				100.0 * o.held / o.pixels, per(o.out_change_interface, o.hud), per(o.out_change_rest, rest),
				per(o.drawn_change_interface, o.hud), per(o.drawn_change_rest, rest),
				100.0 * per(o.out_flicker_interface, o.hud), 100.0 * per(o.out_flicker_rest, rest),
				100.0 * per(o.motion_moving, o.motion_pixels), per(o.motion_rest, o.motion_pixels),
				100.0 * per(o.edges_changed, o.edges), 100.0 * per(moved_changed_, moved_edges_), moved_frames_,
				held_frames_);
			held_frames_ = moved_frames_ = 0;
			moved_edges_ = moved_changed_ = 0.0;
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
		inputs.engine.ready();
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
		inputs.engine.ready();
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
	const auto capture_input = [&]() {
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
	};
	if (game_scale_.active && (backend == &vsr11_ || backend == &fsr11_))
		backend = nullptr;
	if (remote_.ready() && runs_remote(backend)) {
		if (capturing)
			capture_input();
		if (scope)
			upscaler_capture_.scope_copy(ScopePlane::UpscalerIn, inputs.engine.device, inputs.engine.cmd,
				inputs.engine.color, inputs.engine.color_state);
		const bool neural_here = sizeof(void *) != 4 && !engine_route_now().neural_in_host;
		if (plan.neural.run && !neural_here && hud_detected_ && !interface_restored_ && !game_scale_.active) {
			FramePlan upscale_only = plan;
			upscale_only.neural.run = false;
			const bool upscaled = run_remote_engine_work(runtime, inputs, upscale_only, backend, params, false);
			if (!scope_list_lost_) {
				const FrameInputs &shown = game_scale_.active && game_scale_.done ? game_scale_shown_ : inputs;
				restore_interface(shown);
				engine_journal_note(L"neural pass");
				run_remote_engine_work(runtime, shown, plan, nullptr, UpscalerParams{}, false, upscaled);
			}
		} else {
			run_remote_engine_work(runtime, inputs, plan, backend, params, neural_here);
		}
		stage_times_.cancel(upscaler_stage);
		stage_times_.cancel(StageTimings::Stage::Total);
		stage_times_.note_remote_frame();
		if (scope_list_lost_)
			stage_times_.abandon_frame();
		return;
	}
	if (backend != nullptr && must_run_in_host(backend)) {
		backend->status = UpscalerStatus::InitFailed;
		const wchar_t *const why = sizeof(void *) == 4
			? L"a 32-bit game runs its upscaler in AeonSRHost.exe"
			: upscaler_layer_seen()
			? L"another program answers upscaler calls in this game (OptiScaler), so Aeon SR's upscaler runs in "
			  L"AeonSRHost.exe, apart from it"
			: L"the game runs its own DLSS, so Aeon SR's runs in AeonSRHost.exe";
		backend->last_error = remote_.last_error.empty()
			? std::wstring(why) + L", which is starting"
			: std::wstring(why) + L", which could not start: " + remote_.last_error;
		backend = nullptr;
	}
	if (backend != nullptr)
		engine_journal_note(L"upscaler");
	if (capturing && backend != nullptr)
		capture_input();
	if (scope && backend != nullptr)
		upscaler_capture_.scope_copy(ScopePlane::UpscalerIn, inputs.engine.device, inputs.engine.cmd,
			inputs.engine.color, inputs.engine.color_state);
	vsr11_.list_lost = false;
	const bool ran = backend != nullptr && backend->run(runtime, inputs, params);
	if (vsr11_.list_lost) {
		hud_detected_ = false;
		scope_list_lost_ = true;
		stage_times_.abandon_frame();
		return;
	}
	stage_times_.end(upscaler_stage, inputs.engine.cmd);
	trace_cur_.set(ScopeColumn::Ran, ran ? 1.0 : 0.0);
	if (game_scale_.active) {
		game_scale_.done = ran;
		if (!ran) {
			if (backend != nullptr && upscaler_waiting(backend->status))
				jitter_.note_waiting();
			return;
		}
	}
	const FrameInputs &shown = game_scale_.active ? game_scale_shown_ : inputs;
	if (scope && ran)
		upscaler_capture_.scope_copy(ScopePlane::Upscaled, inputs.engine.device, inputs.engine.cmd,
			inputs.engine.color, inputs.engine.color_state);
	if (ran && backend != &fsr11_)
		jitter_.note_upscaled();
	else if (backend != nullptr && upscaler_waiting(backend->status))
		jitter_.note_waiting();
	float depth_u = 0.0f, depth_v = 0.0f;
	jitter_.depth_uv(&depth_u, &depth_v);
	if (plan.neural.run && remote_.ready()) {
		const EngineRoute r = engine_route_now();
		if (neural_crosses_alone(r, backend)) {
			restore_interface(shown);
			engine_journal_note(L"neural pass");
			run_remote_engine_work(runtime, shown, plan, nullptr, UpscalerParams{}, false, ran);
			stage_times_.cancel(StageTimings::Stage::Total);
			stage_times_.note_remote_frame();
			if (scope_list_lost_)
				stage_times_.abandon_frame();
			return;
		}
	}
	restore_interface(shown);
	engine_journal_note(L"neural pass");
	const StageScope neural_time(stage_times_, StageTimings::Stage::Neural, shown);
	run_neural_render(runtime, shown, plan.neural, depth_u, depth_v);
}

bool App::run_remote_engine_work(reshade::api::effect_runtime *runtime, const FrameInputs &inputs,
	const FramePlan &plan, UpscalerBackend *backend, const UpscalerParams &params,
	bool neural_here, bool upscaled_here)
{
	const BackendChoice choice = choice_of(backend);

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
	rq.upscaled_here = upscaled_here;
	remote_extras_.fsr_provider = settings_.fsr_provider;
	rq.extras = remote_extras_;

	RemoteEngine::Reply rp;
	(void)remote_.run(engine_, rq, rp);
	const bool ok = rp.step == remote::RemoteStep::Ok;
	if (rp.info.valid)
		remote_info_ = rp.info;
	{
		FlightRecord &fr = flight_.cur();
		fr.host = rp.host_busy ? FlightHost::Busy
			: rp.step == remote::RemoteStep::Timeout ? FlightHost::Timeout
			: rp.step == remote::RemoteStep::Lost ? FlightHost::Lost
			: rp.step == remote::RemoteStep::NoHost || rp.step == remote::RemoteStep::Share ? FlightHost::Failed
			: FlightHost::Answered;
		fr.host_ms = rp.wait_ms;
	}
	if (!rp.list_recording) {
		hud_detected_ = false;
		scope_list_lost_ = true;
	}
	const bool host_ran = ok && rp.backend_ran == choice && rp.upscaler_status == UpscalerStatus::Ready;
	if (!upscaled_here)
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
		if (rp.neural_ran >= 0)
			neural12_.set_status(UpscalerStatus::Ready, neural12_.text().last_error);
		else
			neural12_.set_status(UpscalerStatus::EvaluateFailed, rp.message);
		neural12_.model_width = rp.neural_model_width;
		neural12_.model_height = rp.neural_model_height;
		neural12_.last_eval_rows = rp.neural_eval_rows;
		neural12_.created_passes = rp.neural_passes_built;
		neural12_.timer.last_ms = rp.neural_gpu_ms;
	}

	if (rp.step == remote::RemoteStep::Lost) {
		remote_tried_ = false;
		remote_info_ = RemoteEngine::Info{};
	}

	if (ok && rp.backend_ran == choice && rp.upscaler_status == UpscalerStatus::Ready)
		jitter_.note_upscaled();
	if (game_scale_.active && !upscaled_here)
		game_scale_.done = host_ran && rp.list_recording;
	if (!neural_here)
		return host_ran;
	const FrameInputs &shown = game_scale_.active && game_scale_.done ? game_scale_shown_ : inputs;
	restore_interface(shown);
	float depth_u = 0.0f, depth_v = 0.0f;
	jitter_.depth_uv(&depth_u, &depth_v);
	run_neural_render(runtime, shown, plan.neural, depth_u, depth_v);
	return host_ran;
}

bool App::remote_wanted() const noexcept
{
	if constexpr (sizeof(void *) == 4)
		return true;
	const EngineRoute r = engine_route_now();
	return r.dlss_in_host || r.all_in_host || r.neural_in_host;
}

EngineRoute App::engine_route_now() const noexcept
{
	const unsigned b = settings_.backend;
	const EngineRoute r = engine_route(sizeof(void *) == 4, native_dlss_seen_, upscaler_layer_seen(),
		settings_.enabled && b == static_cast<unsigned>(BackendChoice::Dlss),
		settings_.enabled && (b == static_cast<unsigned>(BackendChoice::Dlss) ||
			b == static_cast<unsigned>(BackendChoice::Fsr) || b == static_cast<unsigned>(BackendChoice::Xess)),
		settings_.neural_render && gpu_.may_be_nvidia());
	return with_layer_dlss(with_ngx_override(r, dlss_override()), !layer_dlss_path().empty(), game_dlss_for_layer(),
		settings_.enabled && b == static_cast<unsigned>(BackendChoice::Dlss),
		settings_.neural_render && gpu_.may_be_nvidia());
}

bool App::game_dlss_for_layer() const noexcept
{
	if (!layer_dlss_started_)
		return native_dlss_seen_;
	return native_before_layer_ || native_dlss_.game_marker;
}

const std::wstring &App::layer_dlss_path() const noexcept
{
	static const std::wstring none;
	if constexpr (sizeof(void *) == 4)
		return none;
	return settings_.optiscaler_upscale ? ngx_layer_table_ : none;
}

void App::sync_dlss_layer()
{
	if constexpr (sizeof(void *) == 4)
		return;
	const EngineRoute r = engine_route_now();
	const std::wstring want = r.dlss_to_layer ? layer_dlss_path() : std::wstring();
	if (!want.empty() && !layer_dlss_started_) {
		layer_dlss_started_ = true;
		native_before_layer_ = native_dlss_seen_;
	}
	if (want == ngx12_.layer_path)
		return;
	dlss12_.shutdown();
	ngx12_.layer_path = want;
	ngx12_.status = dlss12_.status = UpscalerStatus::Idle;
	ngx12_.last_error.clear();
	dlss12_.last_error.clear();
	diag_info("dlss", want.empty()
		? std::wstring(L"DLSS no longer goes to OptiScaler")
		: L"DLSS goes to " + want + L" as its DLSS input; OptiScaler runs the upscaler picked in its own menu");
}

bool App::dlss_override() const noexcept
{
	if constexpr (sizeof(void *) == 4)
		return false;
	return !ngx_override_path(ngx12_.addon_dir).empty();
}

bool App::runs_remote(const UpscalerBackend *backend) const noexcept
{
	if (backend != nullptr && !crosses(backend))
		return false;
	if constexpr (sizeof(void *) == 4)
		return true;
	const EngineRoute r = engine_route_now();
	if (backend == nullptr)
		return r.neural_in_host;
	return backend == static_cast<const UpscalerBackend *>(&dlss12_) ? !r.dlss_to_layer : r.all_in_host;
}

bool App::neural_crosses_alone(const EngineRoute &r, const UpscalerBackend *backend) const noexcept
{
	return neural_alone_after_local(r, backend != nullptr && !runs_remote(backend) &&
		backend != static_cast<const UpscalerBackend *>(&fsr11_) && backend != static_cast<const UpscalerBackend *>(&dlss11_));
}

bool App::must_run_in_host(const UpscalerBackend *backend) const noexcept
{
	if (backend == nullptr || !crosses(backend))
		return false;
	if constexpr (sizeof(void *) == 4)
		return backend == static_cast<const UpscalerBackend *>(&dlss12_) ||
			backend == static_cast<const UpscalerBackend *>(&vsr11_);
	const EngineRoute r = engine_route_now();
	return backend == static_cast<const UpscalerBackend *>(&dlss12_) ? r.dlss_in_host : r.all_in_host;
}

bool App::upscaler_layer_seen() const noexcept
{
	return !ngx_layers_.empty() || runtime_load_redirected();
}

bool App::crosses(const UpscalerBackend *backend) const noexcept
{
	if constexpr (sizeof(void *) == 4) {
		if (backend == static_cast<const UpscalerBackend *>(&vsr11_))
			return true;
	}
	return backend == static_cast<const UpscalerBackend *>(&dlss12_) ||
		backend == static_cast<const UpscalerBackend *>(&fsr12_) ||
		backend == static_cast<const UpscalerBackend *>(&xess12_);
}

void App::ensure_remote()
{
	if (!remote_wanted()) {
		if (remote_.ready()) {
			remote_.stop();
			remote_info_ = RemoteEngine::Info{};
			diag_info("remote", L"the engine host was closed, nothing here needs it any more");
		}
		remote_tried_ = false;
		return;
	}
	if (remote_tried_ || remote_.ready() || !engine_.ready())
		return;
	remote_tried_ = true;
	remote_.host_gpu_priority_high = settings_.host_gpu_priority;
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
	const EngineRoute route = engine_route_now();
	if (sizeof(void *) != 4 && route.dlss_to_layer && route.neural_in_host) {
		if (remote_tried_ && !remote_.ready())
			neural12_.set_status(UpscalerStatus::InitFailed, L"OptiScaler does the upscale in this game, so DLSS "
				L"neural rendering runs in AeonSRHost.exe, which did not start: " + remote_.last_error);
		else
			neural12_.set_status(UpscalerStatus::Loading, L"OptiScaler does the upscale in this game, so DLSS "
				L"neural rendering runs in AeonSRHost.exe, which is starting");
		return;
	}
	if (sizeof(void *) != 4 && route.all_in_host) {
		if (active_ != nullptr && !crosses(active_) && active_ != static_cast<const UpscalerBackend *>(&vsr11_))
			neural12_.set_status(UpscalerStatus::InitFailed, L"another program answers DLSS calls in this game, so "
				L"DLSS neural rendering runs in AeonSRHost.exe, after an upscaler that runs there or RTX VSR: DLSS, FSR, "
				L"XeSS or RTX VSR. The upscaler running now writes the game's own texture.");
		else if (remote_tried_ && !remote_.ready())
			neural12_.set_status(UpscalerStatus::InitFailed, L"another program answers DLSS calls in this game, so "
				L"DLSS neural rendering runs in AeonSRHost.exe, which did not start: " + remote_.last_error);
		else
			neural12_.set_status(UpscalerStatus::Loading, L"another program answers DLSS calls in this game, so "
				L"DLSS neural rendering runs in AeonSRHost.exe, which is starting");
		return;
	}
	if (fsr12_.fsr_device_busy() || xess12_.xess_loading() || dlss12_.dlss_loading())
		return;

	if (!gpu_.may_be_nvidia()) {
		if (neural12_.text().status != UpscalerStatus::UnsupportedGpu) {
			neural12_.set_status(UpscalerStatus::UnsupportedGpu, L"DLSS neural rendering needs an NVIDIA GPU: its "
				L"runtime refuses to initialise on any other adapter.");
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
	if (neural_last_ > 0 && upscaler_capture_.scope_recording())
		upscaler_capture_.scope_copy(ScopePlane::Neural, inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
			inputs.engine.color_state);
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
	if (!drawn.valid || picture_hold_.held() || inputs.engine.color == nullptr || inputs.engine.cmd == nullptr)
		return;
	const FrameValidRect band = frame_valid_rect(drawn.x, drawn.y, inputs.width, inputs.height, viewport_clip(),
		jitter_scene_w_, jitter_scene_h_);
	if (!band.any)
		return;
	(void)frame_shift_.fill_edges(inputs.engine.device, inputs.engine.queue, frame_shift_fence_, inputs.engine.cmd,
		frame_shift_blit_, inputs.engine.color, inputs.engine.color_state, band);
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

void App::update_ngx_layers()
{
	if (!ngx_layers_.empty() && (!ngx_layer_table_.empty() || ngx_layers_rescans_ >= 30))
		return;
	const ULONGLONG now = GetTickCount64();
	if (ngx_layers_scanned_ms_ != 0 && now - ngx_layers_scanned_ms_ < 1000)
		return;
	ngx_layers_scanned_ms_ = now;
	if (!ngx_layers_.empty())
		++ngx_layers_rescans_;
	ngx_layers_ = scan_ngx_layers(module_);
	if (dlss_override()) {
		const std::wstring &own = ngx_override_path(ngx12_.addon_dir);
		std::erase_if(ngx_layers_, [&own](const NgxLayer &l) { return ngx_same_file(l.path, own); });
	}
	if (ngx_layers_.empty())
		return;
	std::wstring names;
	for (const NgxLayer &l : ngx_layers_)
		names += (names.empty() ? L"" : L", ") + ngx_layer_label(l) + L" at " + l.path;
	for (const NgxLayer &l : ngx_layers_) {
		const HMODULE m = l.optiscaler ? GetModuleHandleW(l.path.c_str()) : nullptr;
		if (m != nullptr && ngx12_has_table(m)) {
			ngx_layer_table_ = l.path;
			break;
		}
	}
	diag_state("ngx-layer", DiagLevel::Info, "aeonsr", names + L" is loaded in this game and answers upscaler "
		L"calls in it; Aeon SR's DLSS, FSR, XeSS and DLSS neural rendering run in AeonSRHost.exe, apart from it, "
		L"whenever they are on, unless the panel hands the upscale to OptiScaler" +
		std::wstring(ngx_layer_table_.empty() ? L" (none of them can take it: no NGX Direct3D 12 table)." : L"."));
}

void App::update_native_dlss()
{
	if (native_dlss_seen_ && !(layer_dlss_started_ && !native_before_layer_))
		return;
	const ULONGLONG now = GetTickCount64();
	const bool deciding = sizeof(void *) == 8 && settings_.enabled &&
		settings_.backend == static_cast<unsigned>(BackendChoice::Dlss) &&
		!own_ngx_started() && !(!ngx12_.layer_path.empty() && ngx12_.status == UpscalerStatus::InitFailed);
	if (native_dlss_.queried && !deciding && now - native_dlss_scanned_ms_ < 1000)
		return;
	native_dlss_scanned_ms_ = now;
	std::wstring dir = find_ngx_runtime_dir(ngx12_.addon_dir);
	if (dlss_override()) {
		const std::wstring own = ngx_override_dlss_dir(ngx_override_path(ngx12_.addon_dir));
		if (!own.empty())
			dir = own;
	}
	native_dlss_ = scan_native_dlss(dir.empty() ? std::wstring() : ngx_dll_path(dir));
	native_dlss_seen_ = native_dlss_.native;
	if (native_dlss_seen_ && layer_dlss_started_ && !game_dlss_for_layer()) {
		diag_state("native-dlss", DiagLevel::Info, "aeonsr",
			L"a DLSS module was loaded after Aeon SR's call went to OptiScaler, most likely OptiScaler's own to serve "
			L"it: " + native_dlss_.source + L" from " + native_dlss_.dlss_path);
	} else if (native_dlss_seen_) {
		diag_state("native-dlss", DiagLevel::Info, "aeonsr",
			L"the game runs NVIDIA's DLSS stack itself: " + native_dlss_.source + L" from " + native_dlss_.dlss_path +
			L" (loaded: " + native_dlss_.modules + L")");
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

	const OpticalFlowD3D12::Quality quality = OpticalFlowD3D12::Quality::High;

	flow_.set_depth_logarithmic(depth_how_.logarithmic);
	flow_.set_colour_space(static_cast<uint32_t>(color_space_));
	stage_times_.begin(StageTimings::Stage::Flow, dev12, inputs.engine.queue, cmd12);
	ID3D12Resource *flow_color = inputs.engine.color;
	D3D12_RESOURCE_STATES flow_color_state = inputs.engine.color_state;
	if (interface_game_ && !interface_split_ && camera_moving_ && !fault_.withheld_no_fill) {
		ID3D12Resource *filled = nullptr;
		if (hud_restore_.fill_held(dev12, cmd12, &filled)) {
			flow_color = filled;
			flow_color_state = HudRestoreD3D12::kMaskState;
		}
	}
	const bool recorded = flow_.record(cmd12, flow_color, flow_color_state,
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
	if (depth12 != nullptr && upscaler_capture_.scope_recording())
		upscaler_capture_.scope_copy(ScopePlane::Depth, dev12, cmd12, depth12, inputs.engine.depth_state);

	game_depth_ = depth12 != nullptr;
	if (depth12 != nullptr && settings_.spatial_jitter && !game_probe_failed_) {
		if (!game_probe_.ensure(dev12, inputs.width, inputs.height, &game_probe_error_)) {
			game_probe_failed_ = true;
			diag_logf(DiagLevel::Warn, "jitter", L"jitter: whether the game jitters its own frame cannot be measured: %ls",
				game_probe_error_.c_str());
		} else if (const StageScope probe_time(stage_times_, StageTimings::Stage::Probe, inputs);
			game_probe_.record(cmd12, inputs.engine.color, inputs.engine.color_state, depth12,
				inputs.engine.depth_state, flow_.motion(), OpticalFlowD3D12::kPublishedState, &game_probe_error_)) {
			const JitterFrame &d = jitter_.drawn();
			const float nan = std::numeric_limits<float>::quiet_NaN();
			probe_drawn_[game_probe_.serial() % 8u] = { game_probe_.serial(), d.valid ? d.x : d.ambiguous ? nan : 0.0f,
				d.valid ? d.y : d.ambiguous ? nan : 0.0f, d.valid && d.moved != 0 };
		}
	}
	{
		GameJitterFit gf;
		if (game_probe_.take(&gf)) {
			++game_fits_;
			game_fits_seen_at_ = trace_presents_;
			if (gf.motion_px >= 0.0f)
				camera_moving_ = camera_gate_.step(gf.motion_px);
			if (!gf.depth_ok) {
				if (gf.depth_pixels < GameJitterProbeD3D12::kMinPixels)
					++game_depth_few_;
				else
					++game_depth_one_way_;
			}
			const ProbeDrawn &a = probe_drawn_[gf.serial % 8u], &b = probe_drawn_[(gf.serial - 1u) % 8u];
			const ProbeDrawn &c = probe_drawn_[(gf.serial - 2u) % 8u];
			const bool same = a.serial == gf.serial && b.serial + 1u == gf.serial && a.x == b.x && a.y == b.y;
			const bool have_before = c.serial + 2u == gf.serial;
			game_jitter_.moving = !fault_.game_jitter_still;
			const GameJitterDetector::Change change = game_jitter_.feed(gf, same && !picture_hold_.held(),
				have_before ? b.x - c.x : 0.0f, have_before ? b.y - c.y : 0.0f);
			if (a.serial == gf.serial && b.serial + 1u == gf.serial && a.scene && b.scene && !move_all_) {
				const float dx = a.x - b.x, dy = a.y - b.y;
				const bool still = gf.color_ok && gf.motion_px >= 0.0f &&
					gf.motion_px <= 0.5f + std::sqrt(dx * dx + dy * dy);
				const bool carried = !still && gf.carried_color_ok && gf.motion_px <= GameJitterDetector::kMaxMovingMotion &&
					std::fabs(gf.carried_color_x) <= 2.0f && std::fabs(gf.carried_color_y) <= 2.0f;
				if (landing_.note_response(still ? gf.color_x : gf.carried_color_x, still ? gf.color_y : gf.carried_color_y,
						dx, dy, (still || carried) && !picture_hold_.held()) == JitterLanding::Verdict::Absorbed) {
					flow_.reset_landing();
					diag_logf(DiagLevel::Info, "jitter", L"jitter: the picture held still while the offset drawn into it "
						L"moved (%.2f of it across, %.2f down); the game's frame does not move with it",
						landing_.response_x(), landing_.response_y());
				}
			}
			{
				FlightRecord &fr = flight_.cur();
				if (gf.depth_ok) {
					fr.game_depth_x = gf.depth_x;
					fr.game_depth_y = gf.depth_y;
				}
				if (gf.color_ok) {
					fr.game_color_x = gf.color_x;
					fr.game_color_y = gf.color_y;
				}
				fr.game_rms = game_jitter_.last_rms();
			}
			if (game_jitter_.jitters() != verdict_jitters_ || game_jitter_.confirmed() != verdict_confirmed_) {
				verdict_jitters_ = game_jitter_.jitters();
				verdict_confirmed_ = game_jitter_.confirmed();
				verdict_since_ = GetTickCount64();
			}
			const bool settled = GetTickCount64() >= verdict_since_ + kVerdictSettleMs;
			if (!fault_.game_jitter_forget && game_jitter_.confirmed() && settled && memory_w_ != 0 &&
					(!game_jitter_.jitters() || depth_withholds(true, landing_.lands(), landing_.takes_out())) &&
					game_memory_.remember(game_exe_, memory_w_, memory_h_, game_jitter_.jitters())) {
				memory_unsaved_ = true;
				diag_logf(DiagLevel::Info, "jitter", game_jitter_.jitters()
					? L"jitter: kept for the next launch: the game jitters its own frame at %ux%u"
					: L"jitter: kept for the next launch: the game does not jitter its own frame at %ux%u",
					memory_w_, memory_h_);
			}
			const int overruled = !game_jitter_.measured() || !game_jitter_.jitters() || depth_withholds_offset() ? 0
				: landing_.lands() ? 1 : 2;
			if (overruled != overruled_said_) {
				overruled_said_ = overruled;
				if (overruled == 1)
					diag_logf(DiagLevel::Info, "jitter", L"jitter: the game's depth reads as jittering (%.2f px), but its "
						L"picture moves with the offset drawn into it (%.2f of it across, %.2f down): drawn on, not kept",
						game_jitter_.last_rms(), landing_.response_x(), landing_.response_y());
				else if (overruled == 2)
					diag_logf(DiagLevel::Info, "jitter", L"jitter: the game's depth reads as jittering (%.2f px); drawn on "
						L"until the picture shows whether the offset is taken out of it", game_jitter_.last_rms());
			}
			if (memory_unsaved_ && GetTickCount64() >= memory_retry_at_) {
				keep_game_jitter_memory();
				memory_retry_at_ = GetTickCount64() + 250u;
				if (memory_unsaved_)
					diag_state("game_jitter_memory", DiagLevel::Warn, "jitter",
						L"jitter: what was measured of the game's own jitter could not be written for the next launch yet");
			}
			if (gf.serial % 120u == 0u)
				diag_logf(DiagLevel::Trace, "jitter", L"jitter: game jitter probe: depth %ls (%.3f, %.3f) on %u px, "
					L"colour %ls (%.3f, %.3f) on %u px, camera %.2f px, same offset %d; %u windows, last %.3f px, %u frames "
					L"not measured", gf.depth_ok ? L"fit" : L"no fit", gf.depth_x, gf.depth_y, gf.depth_pixels,
					gf.color_ok ? L"fit" : L"no fit", gf.color_x, gf.color_y, gf.color_pixels, gf.motion_px, same ? 1 : 0,
					game_jitter_.windows(), game_jitter_.last_rms(), game_jitter_.skipped());
			if (change != GameJitterDetector::Change::None) {
				landing_.clear_absorbed();
				if (change != GameJitterDetector::Change::StartsJittering)
					landing_.forget_response();
				flow_.reset_landing();
				diag_logf(DiagLevel::Info, "jitter", change == GameJitterDetector::Change::StartsJittering
					? L"jitter: the game jitters its own frame (%.2f px from frame to frame)"
					: change == GameJitterDetector::Change::StopsJittering
					? L"jitter: the game no longer jitters its own frame (%.2f px from frame to frame)"
					: L"jitter: the game does not jitter its own frame (%.2f px from frame to frame)",
					game_jitter_.last_rms());
			}
		}
	}

	if (!flow_.has_history()) {
		flow_note_benign_ = true;
		flow_note_ = L"warming up: the estimator needs two frames of the same size";
		note_flow_state();
		return;
	}

	{
		float sums[4];
		const JitterLanding::Verdict verdict = flow_.landing(sums) ? landing_.update(sums, !move_all_ &&
			!picture_hold_.held())
			: JitterLanding::Verdict::None;
		if (verdict != JitterLanding::Verdict::None)
			flow_.reset_landing();
		const uint32_t thrown = landing_.discarded();
		if (thrown != landing_discards_logged_ && (thrown == 1u || thrown == 10u || thrown == 100u || thrown % 1000u == 0u)) {
			landing_discards_logged_ = thrown;
			diag_logf(DiagLevel::Info, "jitter", L"jitter: %u reading(s) of which way the picture moves set aside as "
				L"the camera's, not the drawing's (the last %.2f across, %.2f down)", thrown, landing_.rejected_x(),
				landing_.rejected_y());
		}
		if (verdict == JitterLanding::Verdict::Turned) {
			wchar_t text[200]{};
			_snwprintf_s(text, _TRUNCATE, L"jitter: the picture moved %.2f times the offset across and %.2f down; "
				L"drawn turned over %ls", landing_.factor_x(), landing_.factor_y(),
				landing_.flip_x() && landing_.flip_y() ? L"on both axes"
				: landing_.flip_x() ? L"across" : landing_.flip_y() ? L"down" : L"on neither axis");
			diag_info("jitter", text);
		}
		if (verdict == JitterLanding::Verdict::Absorbed) {
			wchar_t text[200]{};
			_snwprintf_s(text, _TRUNCATE, L"jitter: the picture moved %.2f times the offset across and %.2f down; "
				L"the game's frame does not move with it", landing_.factor_x(), landing_.factor_y());
			diag_info("jitter", text);
		}
	}

	if (settings_.spatial_jitter && GetTickCount64() >= jitter_measure_logged_at_ + 10000u) {
		jitter_measure_logged_at_ = GetTickCount64();
		const uint32_t measured = game_fits_ - (game_jitter_.skipped() - game_skipped_at_);
		diag_logf(DiagLevel::Info, "jitter", L"jitter: last 10 s %u reading(s) of which way the picture moves, %u "
			L"with too little change of offset to judge, the last %.2f across and %.2f down; the game's own jitter "
			L"%ls, measured on %u of %u frames (the last window %.3f px); the depth fitted on %u, too few of its pixels in "
			L"a plane on %u, all sloping one way on %u; %u reading(s) of the colour against the offset, the last %.2f "
			L"across and %.2f down",
			landing_.readings() - landing_reads_at_, landing_.short_readings() - landing_short_at_, landing_.factor_x(),
			landing_.factor_y(), !game_depth_ && !game_jitter_.measured() ? L"cannot be measured: no depth"
				: !game_jitter_.measured() ? L"not measured yet"
				: !game_jitter_.confirmed() && game_jitter_.jitters() ? L"taken as there (remembered or one window), not confirmed yet"
				: !game_jitter_.confirmed() ? L"taken as not there (remembered or one window), not confirmed yet"
				: game_jitter_.jitters() ? L"is there" : L"is not there",
			measured, game_fits_, game_jitter_.last_rms(), game_fits_ - game_depth_few_ - game_depth_one_way_,
			game_depth_few_, game_depth_one_way_, landing_.responses() - responses_at_, landing_.response_x(),
			landing_.response_y());
		responses_at_ = landing_.responses();
		game_depth_few_ = game_depth_one_way_ = 0;
		{
			const JitterFrame &f = jitter_.drawn();
			const JitterShaderCounts c = jitter_shader_counts();
			diag_logf(DiagLevel::Info, "jitter", L"jitter: last frame %u scene draws at the offset, %u on the grid (%u "
				L"with another depth buffer of the scene's size), %u left on the grid as interface by their depth test "
				L"(%u of those outside the window), %u with the scene's depth bound, %u outside the window; %u quads "
				L"over the scene at the offset, %u left on the grid as passes over the whole screen or as interface "
				L"into the window; most other "
				L"draws into %ux%u (%u), most with another depth buffer with one of %ux%u (%u); vertex shaders rewritten %u, left as they were %u, draws kept on the grid by an "
				L"indexed shader %u, already in screen space %u",
				f.moved, f.plain, f.plain_aliased, f.interface_kept, f.interface_kept_outside, f.depth_draws,
				f.outside_draws, f.quads_moved, f.quads_kept, f.other_w, f.other_h, f.other_draws, f.other_depth_w,
				f.other_depth_h, f.other_depth_draws, c.rewritten, c.kept, c.indexed_held,
				c.pretransformed);
			const auto proof = [&f](JitterProofWhy w) { return f.proof_why[static_cast<uint32_t>(w)]; };
			diag_logf(DiagLevel::Info, "jitter", L"jitter: last frame, tested meshes of the scene asked whether its "
				L"camera divides by depth: %u proven to, %u proven not to; not known: %u not asked (this frame's tries "
				L"spent), %u whose vertex shader's w was not read, %u placed by a later stage, %u on a deferred "
				L"context, %u whose constants are not known yet; the camera %hs",
				proof(JitterProofWhy::Perspective), proof(JitterProofWhy::Flat), proof(JitterProofWhy::NotAsked),
				proof(JitterProofWhy::NoTerms), proof(JitterProofWhy::Replaced), proof(JitterProofWhy::Deferred),
				proof(JitterProofWhy::NotRead), scene_jitter().perspective_seen() ? "has been proven to" :
				"has never been proven to");
		}
		if (last_api_ == reshade::api::device_api::vulkan && vulkan_constants().installed()) {
			const VulkanConstantStats v = vulkan_constants().take_stats();
			const auto missed = [&v](VulkanReadMiss m) { return v.missed[static_cast<size_t>(m)]; };
			diag_logf(DiagLevel::Info, "jitter", L"jitter: Vulkan constants, %u of %u calls hooked; read %u from push "
				L"constants, %u from descriptor sets, %u through a descriptor heap, %u through a descriptor buffer; not "
				L"read %u (command buffer set nothing seen), %u (nothing bound there), %u (memory not mapped), %u "
				L"(descriptor not known), %u (heap mapping not followed), %u (command buffer not submitted); %u buffers, "
				L"%u mappings, %u device addresses known%hs%hs",
				v.hooked, v.hookable, v.push, v.sets, v.heap, v.descriptor_buffer, missed(VulkanReadMiss::NoState),
				missed(VulkanReadMiss::NotBound), missed(VulkanReadMiss::NotMapped), missed(VulkanReadMiss::NoDescriptor),
				missed(VulkanReadMiss::NoMapping), missed(VulkanReadMiss::NotSubmitted), v.buffers, v.mappings, v.addresses,
				v.first_miss[0] != '\0' ? "; the first not read: " : "", v.first_miss);
		}
		landing_reads_at_ = landing_.readings();
		landing_short_at_ = landing_.short_readings();
		game_skipped_at_ = game_jitter_.skipped();
		game_fits_ = 0;
	}

	if (flow_.model_failed())
		diag_state("camera-model", DiagLevel::Warn, "flow", flow_.model_error());

	inputs.engine.motion = flow_.motion();
	inputs.engine.motion_state = DepthNormalizeD3D12::kPublished;
	if (upscaler_capture_.scope_recording())
		upscaler_capture_.scope_copy(ScopePlane::Motion, inputs.engine.device, inputs.engine.cmd, flow_.motion(),
			OpticalFlowD3D12::kPublishedState);
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
		const bool on_d3d11 = runtime != nullptr && runtime->get_device() != nullptr &&
			runtime->get_device()->get_api() == reshade::api::device_api::d3d11;
		const bool motion_read = on_d3d11 && ((settings_.mv_probe && probes_.motion_write_due()) ||
			fsr12_.status == UpscalerStatus::InitFailed || active_ == &fsr11_ || active_ == &dlss11_);
		publish_flow_to_game(inputs, motion_read);
	}

	flow_note_.clear();
	note_flow_state();
}

void App::publish_flow_to_game(FrameInputs &inputs, bool motion_read)
{
	inputs.motion_vectors = { 0 };
	inputs.global_flow = { 0 };

	if (inputs.engine.native) {
		inputs.motion_vectors = { reinterpret_cast<uintptr_t>(flow_.motion()) };
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

	if (motion_read) {
		carry(flow_game_motion_, flow_.motion(), flow_.width(), flow_.height(),
			DXGI_FORMAT_R16G16_FLOAT, inputs.motion_vectors);
		if (inputs.motion_vectors.handle != 0)
			probes_.note_motion_written();
	}
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
		const std::wstring error = nr.text().last_error;
		if (!error.empty())
			line += L" - " + error;
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
		if (landing_.absorbed()) {
			landing_.clear_absorbed();
			flow_.reset_landing();
		}
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
			diag_logf(DiagLevel::Info, "jitter", L"jitter paused (%hs): last frame %u scene draws moved, %u on the "
				L"grid (%u of those with the other depth buffer, of %u drawn with it), %u replayed; offsets "
				L"reaching the recording since the last pause: %u at a frame boundary, %u between frames, "
				L"%u mid-frame; vertex shaders rewritten %u (%u indexing through a0), left as they were "
				L"%u; draws kept on the grid so far: %u by an indexed shader whose register the game writes, "
				L"%u already in screen space",
				jitter_.pause_reason(), f.moved, f.plain, f.plain_aliased, f.aliased, f.replayed, pr.at_boundary,
				pr.between_frames, pr.forced, c.rewritten, c.indexed, c.kept, c.indexed_held, c.pretransformed);
		}
		return;
	}
	jitter_pause_logged_ = false;
	const generic_depth_data *const depth = runtime->get_private_data<generic_depth_data>();
	uint64_t scene_depth = 0;
	uint32_t sel_w = 0, sel_h = 0;
	if (depth != nullptr && depth->selected_depth_stencil.handle != 0) {
		const reshade::api::resource_desc desc =
			runtime->get_device()->get_resource_desc(depth->selected_depth_stencil);
		sel_w = desc.texture.width;
		sel_h = desc.texture.height;
		scene_depth = sel_w != 0 && sel_h != 0 ? depth->selected_depth_stencil.handle : 0;
	}
	const bool was_composite = scene_size_.by_composite;
	const uint64_t comp = scene_jitter().composite_size();
	scene_size_.find(scene_depth, sel_w, sel_h, inputs.width, inputs.height, jitter_.drawn(),
		static_cast<uint32_t>(comp >> 32), static_cast<uint32_t>(comp & 0xFFFFFFFFu));
	if (scene_size_.by_composite)
		scene_depth = 0;
	if (scene_size_.by_composite != was_composite)
		diag_logf(DiagLevel::Info, "jitter", scene_size_.by_composite
			? L"jitter: ReShade selected the window's depth (%ux%u) while the scene is drawn with one of %ux%u, the "
				L"size the game presents its window from; the scene is found by that size"
			: L"jitter: the scene is found by ReShade's selection again (%ux%u; it was found at %ux%u)",
			sel_w, sel_h, scene_size_.comp_w, scene_size_.comp_h);
	const uint32_t tw = scene_size_.w, th = scene_size_.h;
	if (tw == 0 || th == 0) {
		jitter_note_ = "waiting for the game's depth buffer";
		return;
	}
	{
		FlightRecord &fr = flight_.cur();
		fr.depth_id = scene_depth;
		fr.depth_w = static_cast<uint16_t>(std::min<uint32_t>(tw, 0xFFFFu));
		fr.depth_h = static_cast<uint16_t>(std::min<uint32_t>(th, 0xFFFFu));
		fr.scene = scene_depth != 0 ? FlightScene::Depth : FlightScene::Size;
	}
	if (tw != memory_w_ || th != memory_h_) {
		memory_w_ = tw;
		memory_h_ = th;
		game_jitter_.unconfirm();
		if (!game_memory_loaded_) {
			game_memory_loaded_ = true;
			game_exe_ = game_jitter_exe_path();
			const std::wstring dir = local_appdata_dir_w();
			if (!dir.empty()) {
				game_memory_path_ = join_path(dir, L"game_jitter.txt");
				(void)load_game_jitter_memory(game_memory_path_, &game_memory_);
			}
		}
		bool remembered = false;
		if (fault_.game_jitter_assume) {
			game_jitter_.seed(true);
			diag_logf(DiagLevel::Warn, "jitter", L"jitter: JitterFault game_jitter_assume: starting as a game that "
				L"jitters its own frame at %ux%u, and measuring on", tw, th);
		} else if (!fault_.game_jitter_forget && game_memory_.find(game_exe_, tw, th, &remembered)) {
			game_jitter_.seed(remembered);
			diag_logf(DiagLevel::Info, "jitter", remembered
				? L"jitter: an earlier launch measured that the game jitters its own frame at %ux%u; starting from that, "
					L"and measuring on"
				: L"jitter: an earlier launch measured that the game does not jitter its own frame at %ux%u; starting "
					L"from that, and measuring on", tw, th);
		}
	}
	{
		if (landing_.note_scene(tw, th, scene_depth)) {
			flow_.reset_landing();
			game_jitter_.restart_window();
			game_probe_.reset();
		}
		const bool withheld = offset_withheld();
		if (withheld != withheld_said_) {
			withheld_said_ = withheld;
			diag_info("jitter", withheld
				? L"jitter: not drawn: the game jitters its own frame"
				: L"jitter: drawn into the game's frame again");
		}
		if (withheld) {
			jitter_note_ = "the game jitters its own frame";
			return;
		}
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
	if (game_scale_.active) {
		arm.target_x = arm.x;
		arm.target_y = arm.y;
		jitter_scene_w_ = rw;
		jitter_scene_h_ = rh;
	}
	if (landing_.flip_x())
		arm.target_x = -arm.target_x;
	if (landing_.flip_y())
		arm.target_y = -arm.target_y;
	const bool moved_all = move_all_;
	arm.depth_proven = depth_proof_.arm(scene_size_.by_depth, tw, th, jitter_.drawn());
	move_all_ = jitter_move_all(move_all_, jitter_.drawn(), arm.depth_proven);
	if (move_all_ && !moved_all) {
		const JitterFrame &f = jitter_.drawn();
		diag_logf(DiagLevel::Info, "jitter", L"jitter: the frame that showed no scene: %u draws seen in all, %u at the "
			L"offset, %u on the grid, %u left on the grid as interface by their depth test, %u with the scene's depth "
			L"(%ux%u) bound; most other draws into %ux%u (%u), most with another depth buffer with one of %ux%u (%u)",
			f.all_draws, f.moved, f.plain, f.interface_kept, f.depth_draws, tw, th, f.other_w, f.other_h, f.other_draws,
			f.other_depth_w, f.other_depth_h, f.other_depth_draws);
	}
	scene_rules(settings_.jitter_scene_rule, move_all_, &arm.size_targets, &arm.move_window);
	if (arm.move_window)
		flight_.cur().scene = FlightScene::Everything;
	arm.tested_quads = settings_.jitter_tested_quads;
	arm.scene_quads = settings_.jitter_scene_quads;
	arm.depth_regrid = settings_.depth_regrid;
	arm.layers = settings_.keep_interface;
	diag_state("jitter_move_all", DiagLevel::Info, "jitter", arm.move_window
		? std::wstring(L"jitter: no draw shows the scene by its depth; everything of the scene's size takes the offset")
		: std::wstring(L"jitter: the scene is found by its depth"));
	scene_jitter().back_buffers_into(arm);
	if (game_scale_.active)
		jitter_arm_add_back_buffer(arm, game_scale_.stretch.target.handle);
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
	if (reflex_.attached() && reflex_.device() == static_cast<IUnknown *>(engine_.device())) {
		reflex_.detach();
		latency_applied_ = 0u;
	}

	engine_.wait_cpu(engine_.last_submitted());
	stage_times_.release();

	bridge_util::release_plane(flow_game_motion_);
	bridge_util::release_plane(flow_game_global_);
	engine_.set_borrowed_queue(nullptr);
	remote_.stop();
	remote_tried_ = false;

	upscaler_capture_.finish(engine_.device(), engine_.queue());
	retire_bridge(bridge_, how);
	bridge_device_ = nullptr;
	frame_shift_.release();
	frame_shift_blit_.release();
	release_game_scale();
	frame_shift_fence_.release();
	hud_restore_.release();
	observer_.release();
#if AEONSR_FRAME_GENERATION
	framegen_.engine_gone();
#endif
	hud_detected_ = false;
	engine_.shutdown();
}

void App::on_init_device(reshade::api::device *device)
{
	if (is_engine_device(device))
		return;

	const auto api = device ? device->get_api() : reshade::api::device_api::d3d11;

	if (api == reshade::api::device_api::vulkan && device != nullptr && settings_.jitter_scene_quads) {
		if (const HMODULE reshade = reshade::internal::get_reshade_module_handle()) {
			if (void *const gdpa = reinterpret_cast<void *>(GetProcAddress(reshade, "vkGetDeviceProcAddr")))
				vulkan_constants().install(reinterpret_cast<void *>(static_cast<uintptr_t>(device->get_native())), gdpa);
		}
	}

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
		if (gpu_.queried && gpu_.spoofed)
			diag_logf(DiagLevel::Info, "gpu", L"render device: %s (vendor 0x%04X, device 0x%04X) on %hs - DXGI "
				L"describes it as %s (vendor 0x%04X), a spoofed identity (OptiScaler, DXVK); the card is the "
				L"driver's own answer", gpu_.name.c_str(), gpu_.vendor_id, gpu_.device_id, api_label(api),
				gpu_.reported_name.c_str(), gpu_.reported_vendor_id);
		else if (gpu_.queried)
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
			vsr11_.status = UpscalerStatus::UnsupportedGpu;
			ngx_.status = UpscalerStatus::UnsupportedGpu;
			if (!dlss_override()) {
				dlss12_.status = UpscalerStatus::UnsupportedGpu;
				ngx12_.status = UpscalerStatus::UnsupportedGpu;
			}
		}
	}
	history_.reset_pending = true;
	invalidate_temporal_history();
}

void App::on_destroy_device(reshade::api::device *device)
{
	pending_devices_.remove(device);
	scene_snapshot().release(device);
	if (device != nullptr && device->get_api() == reshade::api::device_api::vulkan)
		vulkan_constants().forget_device(device->get_native());
	if (device != nullptr && device->get_api() == reshade::api::device_api::d3d11)
		GameStateGuard::release_cached();
	const std::optional<BridgeRelease> release =
		bridge_release_on_destroy(device, bridge_device_, device != nullptr && is_engine_device(device));
	if (!release)
		return;
	engine_.wait_cpu(engine_.last_submitted());
#if AEONSR_FRAME_GENERATION
	framegen_.device_gone(device);
#endif
	jitter_.forget();
	scene_size_.reset();
	depth_proof_.reset();
	move_all_ = false;
	vulkan_viewport_hook().forget();
	guides_.release();
	pipelines_.release();
	probes_.release();
	debug_view_.release();
	flow_.release();
	game_probe_.release();
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

void App::on_destroy_swapchain(reshade::api::swapchain *swapchain, bool )
{
#if AEONSR_FRAME_GENERATION
	framegen_.swapchain_gone(swapchain, engine_);
#endif
	if (swapchain != nullptr && swapchain->get_device() != nullptr &&
		swapchain->get_device()->get_api() == reshade::api::device_api::d3d9)
		scene_snapshot().release(swapchain->get_device());
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
	if (runtime != nullptr) {
		const uint64_t tick = GetTickCount64();
		if (latency_hwnd_ == nullptr || (runtime->get_hwnd() != latency_hwnd_ && tick - latency_hwnd_tick_ > 1000u)) {
			if (latency_hwnd_ != nullptr && latency_hwnd_ != runtime->get_hwnd()) {
				reflex_.detach();
				latency_queue_off();
				latency_.reset();
				latency_applied_ = 0u;
			}
			latency_hwnd_ = runtime->get_hwnd();
		}
		if (runtime->get_hwnd() == latency_hwnd_) {
			latency_hwnd_tick_ = tick;
			latency_presented_ = true;
		}
	}
	if (runtime != nullptr && latency_applied_ != 0u && runtime->get_hwnd() == latency_hwnd_) {
		latency_.ready(latency_clock_ms());
		reflex_.marker(ReflexMarker::RenderSubmitEnd, reflex_frame_);
	}
	drive_game_scale(game_scale_.attempted);
	game_scale_.attempted = false;
	set_jitter_shaders_wanted(settings_.spatial_jitter);
	if (runtime != nullptr) {
		RuntimeJitterCounts counts(runtime);
		jitter_.present(reinterpret_cast<uintptr_t>(runtime), counts);
		if (reinterpret_cast<uintptr_t>(runtime) == trace_runtime_) {
			flight_commit();
			trace_close();
		}
	}
	service_report();
#if AEONSR_FRAME_GENERATION
	run_frame_generation(runtime);
	pace_to_display(runtime);
#endif
	if (runtime != nullptr) {
		if (latency_applied_ != 0u && runtime->get_hwnd() == latency_hwnd_) {
			latency_.sent(latency_clock_ms());
			reflex_.marker(ReflexMarker::PresentStart, reflex_frame_);
		}
	}
}

double App::latency_clock_ms() noexcept
{
#if AEONSR_FRAME_GENERATION
	return qpc_ms();
#else
	static LARGE_INTEGER f{};
	if (f.QuadPart == 0)
		QueryPerformanceFrequency(&f);
	LARGE_INTEGER t{};
	QueryPerformanceCounter(&t);
	return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
#endif
}

unsigned int App::latency_mode_now() const noexcept
{
#if AEONSR_FRAME_GENERATION
	if (settings_.frame_generation && !framegen_.own_window_active())
		return 0u;
#endif
	const unsigned int m = settings_.low_latency <= 2u ? settings_.low_latency : 0u;
	return m == 2u && reflex_refused_ ? 1u : m;
}

void App::latency_apply(unsigned int mode, reshade::api::swapchain *swapchain)
{
	reshade::api::device *const sdev = swapchain != nullptr ? swapchain->get_device() : nullptr;
	if (sdev != nullptr && sdev->get_api() == reshade::api::device_api::vulkan)
		mode = 0u;
	static const int reflex32 = [] {
		char v[16] = {};
		const DWORD n = GetEnvironmentVariableA("AEONSR_REFLEX_32", v, sizeof(v));
		return n == 0 || n >= sizeof(v) ? 0 : std::strcmp(v, "engine") == 0 ? 1 : std::strcmp(v, "plain") == 0 ? 2 : 0;
	}();
	const int reflex32_here = sizeof(void *) == 4 ? reflex32 : 0;
	if (mode == 2u && sizeof(void *) == 4 && reflex32_here == 0 && sdev != nullptr &&
		sdev->get_api() != reshade::api::device_api::d3d11 && sdev->get_api() != reshade::api::device_api::d3d12) {
		if (!reflex_waiting_said_) {
			reflex_waiting_said_ = true;
			diag_info("latency", L"low latency: on a 32-bit game Reflex would run on the add-on's own device on this API, "
				L"where it held frames too long; AeonSR's own instead");
		}
		mode = 1u;
	}
	if (mode == 2u && sdev != nullptr && sdev->get_api() != reshade::api::device_api::d3d11 &&
		sdev->get_api() != reshade::api::device_api::d3d12 && !engine_.ready()) {
		if (!reflex_waiting_said_) {
			reflex_waiting_said_ = true;
			diag_info("latency", L"low latency: Reflex runs on the add-on's Direct3D 12 device on this API, which is not "
				L"up (the upscaler is off); AeonSR's own until it is");
		}
		mode = 1u;
	}
	if (mode == latency_applied_ && (mode != 2u || reflex_.attached()))
		return;
	if (mode != 2u && reflex_.attached())
		reflex_.detach();
	if (mode != 1u)
		latency_queue_off();
	latency_.reset();
	if (mode == 2u) {
		IUnknown *device = nullptr;
		const wchar_t *on = L"";
		reshade::api::device *const dev = swapchain != nullptr ? swapchain->get_device() : nullptr;
		const reshade::api::device_api api = dev != nullptr ? dev->get_api() : reshade::api::device_api::d3d11;
		if (dev != nullptr && (api == reshade::api::device_api::d3d11 || api == reshade::api::device_api::d3d12)) {
			device = reinterpret_cast<IUnknown *>(static_cast<uintptr_t>(dev->get_native()));
			on = L"the game's device";
			if (!reflex_game_own_ && reflex_.already_on(device)) {
				reflex_game_own_ = true;
				diag_info("latency", L"low latency: the game runs NVIDIA Reflex itself; it is left to it");
			}
			if (reflex_game_own_)
				device = nullptr;
		} else if (engine_.ready()) {
			device = engine_.device();
			on = L"the add-on's Direct3D 12 device, where the game's frame is carried";
		}
		if (device == nullptr) {
			latency_applied_ = 0u;
			return;
		}
		if (!reflex_.attach(device, 0u, device == static_cast<IUnknown *>(engine_.device()) &&
				!(api == reshade::api::device_api::d3d12) && reflex32_here != 2)) {
			reflex_refused_ = true;
			diag_logf(DiagLevel::Warn, "latency", L"low latency: %ls; AeonSR's own low latency instead",
				reflex_.why().c_str());
			mode = 1u;
		} else {
			diag_logf(DiagLevel::Info, "latency", L"low latency: NVIDIA Reflex on %ls (%ls); each frame starts after "
				L"Reflex's sleep", on, widen(api_label(api)).c_str());
		}
	} else if (mode == 1u) {
		diag_info("latency", L"low latency: AeonSR's own; each frame waits before the game reads its input instead "
			L"of after it");
	} else if (latency_applied_ != 0u) {
		diag_info("latency", L"low latency: off");
	}
	latency_applied_ = mode;
	latency_window_ = LatencyWindow{};
}

void App::latency_queue_off() noexcept
{
	if (latency_dxgi_ != nullptr) {
		auto *const d = static_cast<IDXGIDevice1 *>(latency_dxgi_);
		d->SetMaximumFrameLatency(latency_dxgi_was_ != 0u ? latency_dxgi_was_ : 3u);
		d->Release();
		latency_dxgi_ = nullptr;
	}
}

void App::latency_queue(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain)
{
	reshade::api::device *const dev = swapchain->get_device();
	if (dev == nullptr)
		return;
	if (dev->get_api() == reshade::api::device_api::d3d11) {
		if (latency_dxgi_ != nullptr)
			return;
		auto *const d3d = reinterpret_cast<IUnknown *>(static_cast<uintptr_t>(dev->get_native()));
		IDXGIDevice1 *dxgi = nullptr;
		if (d3d == nullptr || FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dxgi))) || dxgi == nullptr)
			return;
		UINT was = 0;
		dxgi->GetMaximumFrameLatency(&was);
		if (FAILED(dxgi->SetMaximumFrameLatency(1))) {
			dxgi->Release();
			return;
		}
		latency_dxgi_ = dxgi;
		latency_dxgi_was_ = was;
		diag_logf(DiagLevel::Info, "latency", L"low latency (AeonSR): the game's frames queued in the driver held to "
			L"one (it allowed %u)", was);
		return;
	}
}

void App::latency_wait_until(double t_ms)
{
	const double left = t_ms - latency_clock_ms();
	if (left <= 0.0)
		return;
	if (latency_timer_ == nullptr) {
		latency_timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 , TIMER_ALL_ACCESS);
		if (latency_timer_ == nullptr)
			latency_timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
	}
	LARGE_INTEGER due{};
	due.QuadPart = -static_cast<LONGLONG>(left * 10000.0);
	if (latency_timer_ != nullptr && SetWaitableTimer(static_cast<HANDLE>(latency_timer_), &due, 0, nullptr, nullptr,
			FALSE))
		WaitForSingleObject(static_cast<HANDLE>(latency_timer_), 100);
	else
		Sleep(static_cast<DWORD>(left));
}

void App::on_finish_present(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain)
{
#if AEONSR_FRAME_GENERATION
	if (swapchain != nullptr && pace_runtime_ != 0 && settings_.pace_to_display && !settings_.frame_generation &&
		swapchain->get_hwnd() == reinterpret_cast<reshade::api::effect_runtime *>(pace_runtime_)->get_hwnd())
		pace_gpu_tail(queue, swapchain);
#endif
	if (swapchain == nullptr || latency_hwnd_ == nullptr || swapchain->get_hwnd() != latency_hwnd_)
		return;
	if (!latency_presented_)
		return;
	latency_presented_ = false;
	const bool bridged = latency_bridged_;
	latency_bridged_ = false;
	if (latency_applied_ == 2u && reflex_.attached() && reflex_.device() != static_cast<IUnknown *>(engine_.device()) &&
		swapchain->get_device() != nullptr && swapchain->get_device()->get_api() != reshade::api::device_api::d3d11 &&
		swapchain->get_device()->get_api() != reshade::api::device_api::d3d12) {
		reflex_.detach();
		latency_applied_ = 0u;
	}
	latency_apply(latency_mode_now(), swapchain);
	if (latency_applied_ == 0u)
		return;
	const double now = latency_clock_ms();
	const reshade::api::device_api api = swapchain->get_device() != nullptr ? swapchain->get_device()->get_api()
		: reshade::api::device_api::d3d11;
	const bool drained = bridged && (api == reshade::api::device_api::d3d9 || api == reshade::api::device_api::d3d10 ||
		api == reshade::api::device_api::d3d11 || api == reshade::api::device_api::opengl);
	double slot = -1.0;
#if AEONSR_FRAME_GENERATION
	if (drained)
		slot = display_pacer_.next_slot();
#endif
	if (latency_applied_ == 2u) {
		reflex_.marker(ReflexMarker::PresentEnd, reflex_frame_);
		reflex_.sleep();
		if (slot >= 0.0)
			latency_wait_until(latency_.frame_start(latency_clock_ms(), slot));
		++reflex_frame_;
		reflex_.marker(ReflexMarker::SimulationStart, reflex_frame_);
		reflex_.marker(ReflexMarker::SimulationEnd, reflex_frame_);
		reflex_.marker(ReflexMarker::RenderSubmitStart, reflex_frame_);
	} else {
		latency_queue(queue, swapchain);
		bool paced_elsewhere = false;
#if AEONSR_FRAME_GENERATION
		paced_elsewhere = !drained && display_pacer_.engaged();
#endif
		if (!paced_elsewhere)
			latency_wait_until(latency_.frame_start(latency_clock_ms(), slot));
	}
	const double started = latency_clock_ms();
	latency_.started(started);
#if AEONSR_FRAME_GENERATION
	display_pacer_.frame_started(started);
#endif
	LatencyWindow &w = latency_window_;
	const uint64_t tick = GetTickCount64();
	if (w.since == 0)
		w.since = tick;
	++w.frames;
	if (started - now > 0.05) {
		++w.waited_frames;
		w.waited += started - now;
	}
	if (tick - w.since >= 10000u) {
		if (latency_applied_ == 2u) {
			ReflexLatency r;
			if (reflex_.latency(&r))
				diag_logf(DiagLevel::Info, "latency", L"low latency (Reflex): last 10 s %u frames, Reflex held %u of them "
					L"%.2f ms on average before they started; the driver's reports (%u frames): %.2f ms from a frame's "
					L"start to the GPU finishing it, %.2f ms of it waiting in the queue, %.2f ms on the GPU", w.frames,
					w.waited_frames, w.waited_frames != 0u ? w.waited / w.waited_frames : 0.0, r.frames, r.pc_ms,
					r.queue_ms, r.gpu_ms);
			else
				diag_logf(DiagLevel::Info, "latency", L"low latency (Reflex): last 10 s %u frames, Reflex held %u of them "
					L"%.2f ms on average before they started (no driver reports)", w.frames, w.waited_frames,
					w.waited_frames != 0u ? w.waited / w.waited_frames : 0.0);
		} else {
			diag_logf(DiagLevel::Info, "latency", L"low latency (AeonSR): last 10 s %u frames, %u held before they "
				L"started (%.2f ms on average); a frame's own time %.2f ms (90th percentile), held in Present %.2f ms "
				L"(median), %u let-gos for a lower frame rate", w.frames, w.waited_frames,
				w.waited_frames != 0u ? w.waited / w.waited_frames : 0.0, latency_.work_p90_ms(), latency_.hold_ms(),
				latency_.cooldowns());
		}
		w = LatencyWindow{};
		w.since = tick;
	}
}

#if AEONSR_FRAME_GENERATION
namespace {
VOID CALLBACK on_pace_gpu_done(PVOID context, BOOLEAN)
{
	static_cast<std::atomic<double> *>(context)->store(qpc_ms(), std::memory_order_release);
}
}

void App::pace_tail_release() noexcept
{
	for (GpuDone &g : pace_done_) {
		if (g.wait != nullptr) {
			UnregisterWaitEx(static_cast<HANDLE>(g.wait), INVALID_HANDLE_VALUE);
			g.wait = nullptr;
		}
		if (g.event != nullptr) {
			CloseHandle(static_cast<HANDLE>(g.event));
			g.event = nullptr;
		}
		g.done_ms.store(-1.0);
	}
	if (pace_fence_ != nullptr) {
		static_cast<ID3D12Fence *>(pace_fence_)->Release();
		pace_fence_ = nullptr;
	}
	pace_fence_device_ = nullptr;
	pace_fence_value_ = 0;
	pace_tail_n_ = pace_tail_at_ = 0;
	display_pacer_.set_gpu_tail(0.0);
}

void App::pace_gpu_tail(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain)
{
	reshade::api::device *const dev = swapchain != nullptr ? swapchain->get_device() : nullptr;
	if (dev == nullptr || queue == nullptr || dev->get_api() != reshade::api::device_api::d3d12)
		return;
	auto *const device = reinterpret_cast<ID3D12Device *>(static_cast<uintptr_t>(dev->get_native()));
	auto *const q = reinterpret_cast<ID3D12CommandQueue *>(static_cast<uintptr_t>(queue->get_native()));
	if (device == nullptr || q == nullptr)
		return;
	if (pace_fence_ != nullptr && pace_fence_device_ != device)
		pace_tail_release();
	if (pace_fence_ == nullptr) {
		ID3D12Fence *fence = nullptr;
		if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || fence == nullptr)
			return;
		pace_fence_ = fence;
		pace_fence_device_ = device;
	}
	bool fed = false;
	for (GpuDone &g : pace_done_) {
		const double done = g.done_ms.exchange(-1.0, std::memory_order_acq_rel);
		if (done >= 0.0 && done >= g.sent_ms && done - g.sent_ms < 100.0) {
			pace_tails_[pace_tail_at_] = done - g.sent_ms;
			pace_tail_at_ = (pace_tail_at_ + 1u) % 32u;
			pace_tail_n_ = std::min(pace_tail_n_ + 1u, 32u);
			fed = true;
		}
	}
	if (fed && pace_tail_n_ >= 8u) {
		double v[32];
		std::copy(pace_tails_, pace_tails_ + pace_tail_n_, v);
		std::sort(v, v + pace_tail_n_);
		display_pacer_.set_gpu_tail(v[(pace_tail_n_ * 9u) / 10u]);
	}
	auto *const fence = static_cast<ID3D12Fence *>(pace_fence_);
	q->Signal(fence, ++pace_fence_value_);
	GpuDone &slot = pace_done_[pace_fence_value_ % kGpuDoneRing];
	if (slot.wait != nullptr && fence->GetCompletedValue() < slot.value)
		return;
	if (slot.wait != nullptr) {
		UnregisterWaitEx(static_cast<HANDLE>(slot.wait), INVALID_HANDLE_VALUE);
		slot.wait = nullptr;
	}
	slot.value = pace_fence_value_;
	if (slot.event == nullptr)
		slot.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	slot.sent_ms = display_pacer_.last_return_ms();
	slot.done_ms.store(-1.0, std::memory_order_release);
	HANDLE wait = nullptr;
	if (slot.event != nullptr &&
		SUCCEEDED(fence->SetEventOnCompletion(pace_fence_value_, static_cast<HANDLE>(slot.event))) &&
		RegisterWaitForSingleObject(&wait, static_cast<HANDLE>(slot.event), on_pace_gpu_done, &slot.done_ms, INFINITE,
			WT_EXECUTEONLYONCE | WT_EXECUTEINWAITTHREAD))
		slot.wait = wait;
}

void App::pace_off() noexcept
{
	pace_tail_release();
	if (display_pacer_.engaged())
		diag_info("pacing", L"pacing: let go, the game's presents go out as it makes them");
	display_pacer_.reset();
	pace_clock_.stop();
	pace_runtime_ = 0;
	pace_window_ = PaceWindow{};
}

void App::pace_to_display(reshade::api::effect_runtime *runtime)
{
	if (!settings_.pace_to_display || settings_.frame_generation || runtime == nullptr) {
		if (pace_runtime_ != 0)
			pace_off();
		return;
	}
	const uintptr_t rt = reinterpret_cast<uintptr_t>(runtime);
	const uint64_t now_tick = GetTickCount64();
	if (pace_runtime_ != rt && (pace_runtime_ == 0 || now_tick - pace_runtime_tick_ > 1000u)) {
		display_pacer_.reset();
		pace_tail_release();
		pace_runtime_ = rt;
	}
	if (pace_runtime_ != rt)
		return;
	pace_runtime_tick_ = now_tick;
	pace_clock_.watch(static_cast<HWND>(runtime->get_hwnd()));
	const DisplayGrid grid = pace_clock_.grid();
	const double now = qpc_ms();
	const bool was = display_pacer_.engaged();
	const double at = display_pacer_.plan(now, grid.valid ? grid.period_ms : 0.0, grid.vblank_ms);
	if (display_pacer_.engaged() != was)
		diag_logf(DiagLevel::Info, "pacing", display_pacer_.engaged()
			? L"pacing: the game's frames take %.2f ms on a %.2f ms refresh; one goes out each refresh, just before "
				L"its vertical blank, so that every refresh shows a frame one refresh after the last"
			: L"pacing: let go, frames came after the refresh they were due for (%.2f ms each on a %.2f ms refresh)",
			display_pacer_.work_ms(), grid.period_ms);
	if (at > now) {
		if (reshade::api::command_queue *const queue = runtime->get_command_queue())
			queue->flush_immediate_command_list();
		if (pace_timer_ == nullptr) {
			pace_timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 , TIMER_ALL_ACCESS);
			if (pace_timer_ == nullptr)
				pace_timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
		}
		LARGE_INTEGER due{};
		due.QuadPart = -static_cast<LONGLONG>((at - now) * 10000.0);
		if (pace_timer_ != nullptr && SetWaitableTimer(static_cast<HANDLE>(pace_timer_), &due, 0, nullptr, nullptr, FALSE))
			WaitForSingleObject(static_cast<HANDLE>(pace_timer_), 100);
		else
			Sleep(static_cast<DWORD>(at - now));
	}
	const double out = qpc_ms();
	display_pacer_.returned(out);
	PaceWindow &w = pace_window_;
	const uint64_t tick = GetTickCount64();
	if (w.since == 0)
		w.since = tick;
	++w.presents;
	if (at > now) {
		++w.paced;
		w.waited += out - now;
		w.late += out - at > 2.0 ? 1u : 0u;
	}
	if (tick - w.since >= 10000u && w.paced != 0u) {
		diag_logf(DiagLevel::Info, "pacing", L"pacing: last 10 s %u presents, %u placed on a refresh (waits %.2f ms on "
			L"average, %u woke more than 2 ms late, %u frames came after their refresh); the game's frames %.2f ms, "
			L"the refresh %.2f ms, presents %.2f ms before its blank%ls", w.presents, w.paced,
			w.paced != 0u ? w.waited / w.paced : 0.0, w.late,
			display_pacer_.misses() >= w.misses_at ? display_pacer_.misses() - w.misses_at : display_pacer_.misses(),
			display_pacer_.work_ms(), grid.period_ms, display_pacer_.lead_ms(),
			grid.valid ? L"" : L" (no fixed refresh seen)");
		w = PaceWindow{};
		w.since = tick;
		w.misses_at = display_pacer_.misses();
	} else if (tick - w.since >= 10000u) {
		w = PaceWindow{};
		w.since = tick;
		w.misses_at = display_pacer_.misses();
	}
}
#endif

#if AEONSR_FRAME_GENERATION
void App::run_frame_generation(reshade::api::effect_runtime *runtime)
{
	if (!settings_.frame_generation) {
		framegen_.off();
		return;
	}
	reshade::api::device *const device = runtime != nullptr ? runtime->get_device() : nullptr;
	if (device == nullptr || is_engine_device(device))
		return;
	if (pending_devices_.take(device))
		adopt_device(device);
	ensure_bridge(device);
	if (bridge_ == nullptr || !bridge_->ready() || bridge_device_ != device) {
		framegen_.note_blocked(L"frame generation: no route from " + widen(api_label(device->get_api())) +
			L" to Direct3D 12" + (bridge_error_.empty() ? L"" : L": " + bridge_error_));
		return;
	}
	framegen_.set_color_space(color_space_ == NeuralColorSpace::Pq ? VendorColor::Pq
		: color_space_ == NeuralColorSpace::ScrgbLinear ? VendorColor::Scrgb : VendorColor::Sdr);
	framegen_.on_present(runtime, *bridge_, engine_, settings_);
}
#endif

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
	bool taken = false;
	{
		const std::lock_guard<std::mutex> lock(runtimes_lock_);
		for (RuntimeDrive &d : runtimes_)
			if (d.runtime == runtime) {
				d.drive.effects_event();
				taken = d.drive.frame_taken();
				break;
			}
	}
	if (taken)
		return;
	if (runtime == nullptr || !runtime->get_effects_state())
		return;
	const EffectsFrame frame = effects_frame_of(event, settings_.upscale_effects);
	if (frame.runs && !game_scale_.attempted && !game_scale_holds_present())
		run_frame(runtime, cmd_list, rtv, frame.after_effects);
}

namespace {

std::string reshade_base_path()
{
	const HMODULE m = reshade::internal::get_reshade_module_handle();
	if (m == nullptr)
		return {};
	using GetBasePath = void (*)(char *, size_t *);
	if (const auto get = reinterpret_cast<GetBasePath>(GetProcAddress(m, "ReShadeGetBasePath"))) {
		size_t size = 0;
		get(nullptr, &size);
		if (size > 1) {
			std::string path(size, '\0');
			get(path.data(), &size);
			path.resize(size);
			return path;
		}
	}
	const std::wstring dir = module_directory(m);
	const int n = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string out(n > 0 ? static_cast<size_t>(n - 1) : 0u, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, out.data(), n, nullptr, nullptr);
	return out;
}

bool effect_search_paths(reshade::api::effect_runtime *runtime, std::vector<std::string> *paths)
{
	size_t size = 0;
	if (!reshade::get_config_value(runtime, "GENERAL", "EffectSearchPaths", nullptr, &size))
		return false;
	std::string listed(size, '\0');
	if (size != 0 && !reshade::get_config_value(runtime, "GENERAL", "EffectSearchPaths", listed.data(), &size))
		return false;
	listed.resize(size);
	for (size_t from = 0; from < listed.size();) {
		const size_t end = listed.find('\0', from);
		const std::string one = listed.substr(from, end == std::string::npos ? std::string::npos : end - from);
		if (!one.empty())
			paths->push_back(one);
		if (end == std::string::npos)
			break;
		from = end + 1;
	}
	return true;
}

}

void App::on_init_effect_runtime(reshade::api::effect_runtime *runtime)
{
	if (runtime == nullptr)
		return;
	{
		const std::lock_guard<std::mutex> lock(runtimes_lock_);
		const bool known = std::any_of(runtimes_.begin(), runtimes_.end(),
			[runtime](const RuntimeDrive &d) { return d.runtime == runtime; });
		if (!known) {
			RuntimeDrive d;
			d.runtime = runtime;
			d.device = runtime->get_device();
			d.hwnd = runtime->get_hwnd();
			runtimes_.push_back(d);
		}
	}
	const std::wstring dir = module_directory(module_);
	if (dir.empty() || GetFileAttributesW(join_path(dir, L"AeonSR.addonfx").c_str()) == INVALID_FILE_ATTRIBUTES)
		return;
	std::vector<std::string> paths;
	if (!effect_search_paths(runtime, &paths) && !effect_search_paths(nullptr, &paths))
		paths.push_back(".\\");
	const int n = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string dir8(n > 0 ? static_cast<size_t>(n - 1) : 0u, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, dir8.data(), n, nullptr, nullptr);
	std::string base = reshade_base_path();
	if (base.empty())
		base = dir8;
	if (dir8.empty() || effect_paths_reach(paths, dir8, base))
		return;
	paths.push_back(dir8);
	std::string joined;
	for (const std::string &p : paths) {
		joined += p;
		joined += '\0';
	}
	reshade::set_config_value(runtime, "GENERAL", "EffectSearchPaths", joined.data(), joined.size());
	diag_info("aeonsr", L"added the add-on's folder (" + dir + L") to ReShade's effect search paths, so ReShade loads "
		L"AeonSR.addonfx and runs its effects frame and its depth support where no effect package is installed");
}

void App::on_destroy_effect_runtime(reshade::api::effect_runtime *runtime)
{
	if (runtime != nullptr && runtime->get_hwnd() == latency_hwnd_) {
		reflex_.detach();
		latency_queue_off();
		latency_.reset();
		latency_applied_ = 0u;
		latency_hwnd_ = nullptr;
	}
#if AEONSR_FRAME_GENERATION
	if (pace_runtime_ != 0 && pace_runtime_ == reinterpret_cast<uintptr_t>(runtime))
		pace_off();
#endif
	const std::lock_guard<std::mutex> lock(runtimes_lock_);
	runtimes_.erase(std::remove_if(runtimes_.begin(), runtimes_.end(),
		[runtime](const RuntimeDrive &d) { return d.runtime == runtime; }), runtimes_.end());
}

void App::on_game_present(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain)
{
	if (queue == nullptr || swapchain == nullptr)
		return;
	const bool wanted = settings_.enabled || settings_.neural_render;
	reshade::api::device *const device = swapchain->get_device();
	void *const hwnd = swapchain->get_hwnd();
	reshade::api::effect_runtime *runtime = nullptr;
	{
		const std::lock_guard<std::mutex> lock(runtimes_lock_);
		present_keys_.clear();
		for (const RuntimeDrive &d : runtimes_)
			present_keys_.push_back({ d.runtime->get_native(), d.device, d.hwnd });
		const size_t i = runtime_of_present(present_keys_.data(), present_keys_.size(),
			{ swapchain->get_native(), device, hwnd });
		if (i < runtimes_.size())
			runtime = runtimes_[i].runtime;
	}
	if (runtime == nullptr)
		return;
	const bool effects_on = runtime->get_effects_state();
	bool drive = false;
	uint64_t driven = 0;
	{
		const std::lock_guard<std::mutex> lock(runtimes_lock_);
		for (RuntimeDrive &d : runtimes_)
			if (d.runtime == runtime) {
				drive = d.drive.at_present(wanted, effects_on);
				driven = d.drive.driven();
				break;
			}
	}
	if (!drive)
		return;
	if (driven == 1)
		diag_info("aeonsr", L"ReShade is running no effect in this game (none loaded, or still compiling), so "
			L"the add-on takes each frame at the game's present instead");
	reshade::api::command_list *const cmd_list = queue->get_immediate_command_list();
	if (cmd_list == nullptr)
		return;
	if (game_scale_.attempted || game_scale_holds_present())
		return;
	const GameStateGuard keep(device, cmd_list);
	const reshade::api::resource back_buffer = current_back_buffer_of(swapchain);
	if (back_buffer.handle != 0)
		cmd_list->barrier(back_buffer, reshade::api::resource_usage::present, reshade::api::resource_usage::render_target);
	run_frame(runtime, cmd_list, reshade::api::resource_view{ 0 }, false);
	if (back_buffer.handle != 0)
		cmd_list->barrier(back_buffer, reshade::api::resource_usage::render_target, reshade::api::resource_usage::present);
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

	if (d3d9_device_lost(runtime->get_device())) {
		flight_.cur().blocked = FlightBlocked::DeviceLost;
		note_frame_blocked(L"the game's Direct3D 9 device is lost (switched away from fullscreen, or "
			L"changing mode); frames resume when the game resets it");
		return;
	}

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
	if (settings_.report_key != 0 && runtime->is_key_pressed(settings_.report_key)) {
		diag_collect_environment(module_);
		start_report(panel_state(true));
	}
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
		if (remote_extras_.accum_target != 0) {
			++remote_extras_.accum_epoch;
			remote_extras_.accum_target = 0;
		}
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
		flight_.cur().blocked = FlightBlocked::NoBridge;
		note_frame_blocked(why);
		return;
	}

	ensure_remote();

	const PassContext pctx{ runtime, cmd_list, &pipelines_ };

	FrameInputs inputs;
	if (!resolve_frame_inputs(runtime, rtv, inputs)) {
		wchar_t why[200]{};
		_snwprintf_s(why, _TRUNCATE, L"ReShade named no back buffer for this frame (%u back buffers, current %u, %ls)",
			runtime->get_back_buffer_count(), runtime->get_current_back_buffer_index(),
			rtv.handle != 0 ? L"a target was named but has no resource" : L"no target named");
		UpscalerBackend *const all[] = { &dlss12_, &fsr12_, &xess12_ };
		for (UpscalerBackend *b : all) {
			b->status = UpscalerStatus::EvaluateFailed;
			b->last_error = std::wstring(L"the frame never reached the upscaler: ") + why;
		}
		flight_.cur().blocked = FlightBlocked::NoBackBuffer;
		note_frame_blocked(why);
		return;
	}

	if (game_scale_.active) {
		const GameScaleStretch &gs = game_scale_.stretch;
		inputs.color = gs.source;
		const reshade::api::resource_desc cd = device->get_resource_desc(gs.source);
		inputs.color_info.width = cd.texture.width;
		inputs.color_info.height = cd.texture.height;
		inputs.color_info.format = static_cast<uint32_t>(cd.texture.format);
		if (gs.depth.handle != 0) {
			const reshade::api::resource_desc dd = device->get_resource_desc(gs.depth);
			inputs.depth = gs.depth;
			inputs.depth_view = reshade::api::resource_view{ 0 };
			inputs.depth_is_backup = false;
			inputs.depth_view_withheld = false;
			inputs.depth_note.clear();
			inputs.have_depth = true;
			inputs.depth_provider = DepthProvider::Generic;
			inputs.depth_info.width = dd.texture.width;
			inputs.depth_info.height = dd.texture.height;
			inputs.depth_info.format = static_cast<uint32_t>(dd.texture.format);
		}
	}

	BridgeInputs bin = bridge_inputs_of(inputs, after_effects);
	SceneSnapshotTaken snap;
	if (settings_.keep_interface)
		scene_snapshot().prepare(runtime->get_device());
	if (settings_.keep_interface) {
		scene_snapshot().end_frame_at_present();
		const bool took = scene_snapshot().take(jitter_.drawn().serial, &snap);
		const bool covered = split_cover_.present();
		if (split_cover_.started) {
			split_cover_.started = false;
			diag_logf(DiagLevel::Info, "interface", L"interface: the window before the interface left out for the next "
				L"%u frames: what it leaves to the restore covered more than half the window on %u of the last %u frames "
				L"that used it, a pass over the whole picture after the copy rather than an interface; the restore puts "
				L"back what holds still meanwhile", SplitCover::kRefuse, SplitCover::kOver, SplitCover::kWindow);
		}
		if (split_gate_present(split_gate_, jitter_.drawn().valid, took) && covered &&
			!game_scale_.active) {
			bin.scene = snap.resource;
			bin.scene_state = snap.state;
		}
	}
	BridgeFrame bframe;
	const BridgeStep step = bridge_->begin(runtime, cmd_list, bin, bframe);
	latency_bridged_ = step == BridgeStep::Ok;
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
		flight_.cur().blocked = FlightBlocked::BridgeFailed;
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
	if (game_scale_.active && !enter_game_scale(inputs)) {
		(void)bridge_->end(runtime, cmd_list, inputs.color);
		engine_.use_ring(EngineDevice::Ring::Frame);
		flight_.cur().blocked = FlightBlocked::BridgeFailed;
		note_frame_blocked(L"the game's render scale could not be taken: the add-on's copy of its frame was not made");
		return;
	}
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
	upscaler_capture_.scope_motion_size(flow_.width(), flow_.height());
	if (upscaler_capture_.scope_recording())
		upscaler_capture_.scope_copy(ScopePlane::Drawn, inputs.engine.device, inputs.engine.cmd, inputs.engine.color,
			inputs.engine.color_state);
	{
		const StageScope edges_time(stage_times_, StageTimings::Stage::Edges, inputs);
		fill_uncovered_edges(inputs);
	}
	{
		const StageScope observe_time(stage_times_, StageTimings::Stage::Observe, inputs);
		observed_ = !game_scale_.active && observer_.begin(inputs.engine.device, inputs.engine.cmd,
			inputs.engine.color, inputs.engine.color_state);
		if (observed_) {
			const JitterFrame &drawn = jitter_.drawn();
			HoldDrawn &h = hold_drawn_[++observe_serial_ % kHoldDrawn];
			h.serial = observe_serial_;
			h.split = false;
			h.scene = drawn.valid && drawn.moved != 0;
			h.x = drawn.x;
			h.y = drawn.y;
		}
		note_picture_hold();
	}
	{
		const StageScope interface_time(stage_times_, StageTimings::Stage::Interface, inputs);
		if (game_scale_.active) {
			hud_detected_ = false;
			interface_game_ = false;
			interface_split_ = false;
		} else {
			detect_interface(inputs);
		}
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
	update_ngx_layers();
	run_internal_flow(runtime, inputs);
	if (hud_detected_ && inputs.have_motion_vectors && inputs.engine.motion != nullptr) {
		const StageScope clear_time(stage_times_, StageTimings::Stage::Clear, inputs);
		(void)hud_restore_.clear_motion(inputs.engine.device, inputs.engine.cmd, inputs.engine.motion,
			inputs.engine.motion_state);
	}
	last_inputs_ = inputs;
	probes_.tick_motion(pctx, settings_, inputs, history_.last_motion_px);

	last_bias_mask_bound_ = false;

	const bool is12 = (api == reshade::api::device_api::d3d12);
	UpscalerBackend *const dlss = &dlss12_;
	UpscalerBackend *fsr = &fsr12_;
	UpscalerBackend *const xess = &xess12_;
	UpscalerBackend *const vsr = &vsr11_;
	const bool vsr_possible = engine_.ready();

	const bool nv = gpu_.may_be_nvidia();
	sync_dlss_layer();
	const bool dlss_ok = nv || dlss_override() || (!layer_dlss_path().empty() && !game_dlss_for_layer());
	BackendSlots slots;
	slots.is_d3d12 = is12;
	slots.vendor = gpu_.vendor;

	if (nv && vsr_possible) {
		slots.vsr = availability_of(vsr, BackendChoice::Vsr);
	} else {
		slots.vsr = BackendAvailability{ false, false,
			nv ? UpscalerStatus::UnsupportedApi : UpscalerStatus::UnsupportedGpu };
	}
	const float frame_time_ms = clock_.frame_time_ms();
	if (inputs.have_motion_vectors) {
		slots.dlss = dlss_ok
			? availability_of(dlss, BackendChoice::Dlss)
			: BackendAvailability{ false, false, UpscalerStatus::UnsupportedGpu };
		slots.fsr = availability_of(fsr, BackendChoice::Fsr);
		if (xess != nullptr)
			slots.xess = availability_of(xess, BackendChoice::Xess);
		if (api == reshade::api::device_api::d3d11 &&
			fsr12_.status == UpscalerStatus::InitFailed && !must_run_in_host(&fsr12_)) {
			slots.fsr_bridge_failed = true;
			slots.fsr11 = BackendAvailability{ fsr11_.runtime_present(), fsr11_.crashed, fsr11_.status };
		}
	}

	if (game_fits_seen_at_ + 30u < trace_presents_) {
		camera_gate_.reset();
		camera_moving_ = false;
	}
	FrameSignals sig = signals_of(inputs, color_space_, jitter_.drawn(), offset_withheld(), camera_moving_);
	sig.withheld_interface_learned = interface_game_ && hud_restore_.camera_learned();
	const FramePlan plan = plan_frame(settings_, sig, slots, history_, frame_time_ms);
	history_ = plan.next;
	last_plan_ = plan;
	{
		FlightRecord &fr = flight_.cur();
		fr.plan = static_cast<uint8_t>(plan.jitter_decision);
		fr.width = static_cast<uint16_t>(std::min<uint32_t>(inputs.width, 0xFFFFu));
		fr.height = static_cast<uint16_t>(std::min<uint32_t>(inputs.height, 0xFFFFu));
	}
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

		UpscalerBackend *const waiting = dlss_ok ? dlss : fsr;
		waiting->status = UpscalerStatus::NeedMotionVectors;
		jitter_.note_waiting();
		waiting->last_error = flow_note_;
		active_ = waiting;

		run_engine_work(runtime, inputs, plan, nullptr, UpscalerParams{});
		finish_frame(runtime, cmd_list, inputs);
		return;
	}

	if (plan.outcome == UpscalerOutcome::NoBackend) {
		active_ = dlss_ok ? dlss : fsr;
		if (plan.no_upscaler_wanted)
			active_->status = UpscalerStatus::Idle;

		run_engine_work(runtime, inputs, plan, nullptr, UpscalerParams{});
		finish_frame(runtime, cmd_list, inputs);
		return;
	}

	active_ = plan.backend_index == 0 ? dlss
		: (plan.backend_index == 1 ? fsr : (plan.backend_index == 2 ? xess : vsr));

	if ((!dlss_ok && plan.backend_index == 0) || (!nv && plan.backend_index == 3)) {
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
	if (game_scale_.active) {
		params.render_width = game_scale_.stretch.render_w;
		params.render_height = game_scale_.stretch.render_h;
	}
	if (jitter_fault_any(fault_) && (plan.jitter_decision == JitterDecision::Drawn ||
			plan.jitter_decision == JitterDecision::Split) &&
		apply_told_fault(fault_, fault_clock(0), plan.jitter_decision == JitterDecision::Split, told_prev_set,
			told_prev_x, told_prev_y, &params.jitter_x, &params.jitter_y))
		trace_cur_.set(ScopeColumn::FaultActive, 1.0);

	if (active_is_dlss() && !game_scale_.active) {
		const StageScope mask_time(stage_times_, StageTimings::Stage::Mask, inputs);
		params.bias_mask = guides_.bias_mask(pctx, settings_, inputs).handle;
		last_bias_mask_bound_ = params.bias_mask != 0;
	}

	ID3D12Resource *const render_motion = inputs.engine.motion;
	if (game_scale_.active)
		game_scale_motion(inputs);
	run_engine_work(runtime, inputs, plan, active_, params);
	inputs.engine.motion = render_motion;
	if (jitter_fault_hit(fault_.upscale_drop_every, fault_clock(0)))
		jitter_.drop_upscaled();

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
	const reshade::api::resource back = !game_scale_.active ? inputs.color
		: game_scale_.done ? game_scale_.stretch.target : game_scale_.stretch.source;
	const BridgeStep step = bridge_->end(runtime, cmd_list, back);
	if (step != BridgeStep::Ok && game_scale_.active)
		game_scale_.done = false;
	if (step == BridgeStep::Ok) {
		engine_.use_ring(EngineDevice::Ring::Frame);
		return true;
	}
	engine_.abandon_list();
	engine_.use_ring(EngineDevice::Ring::Frame);
	flight_.cur().blocked = FlightBlocked::BridgeFailed;
	diag_state("interop", DiagLevel::Warn, "interop",
		L"the upscaled frame did not reach the game: " + widen(bridge_step_label(step)) +
		(bridge_->last_error.empty() ? L"" : L" - " + bridge_->last_error));
	return false;
}

bool App::game_scale_backend_takes() const noexcept
{
	if (settings_.backend == static_cast<unsigned>(BackendChoice::Vsr))
		return false;
	return !(active_ == &fsr11_ || active_ == &vsr11_);
}

void App::update_game_scale_hooks()
{
	const bool want = game_integrated();
	if (want && !game_scale_hooks_) {
		game_scale_hooks_ = true;
		register_game_render_scale([this](const GameScaleStretch &s) { return run_game_scale(s); });
		diag_info("scale", L"render scale: the game's stretch is taken over (the game's integration sets its render scale)");
	}
	set_game_render_scale_enabled(want);
	set_game_render_scale_full(want && game_->routes_full_size());
}

void App::drive_game_scale(bool stretch_ran)
{
	if (game_ == nullptr || !game_->attached())
		return;
	games::ScaleDriverInput in;
	in.attached = true;
	in.upscaling = settings_.game_render_scale && settings_.enabled &&
		settings_.backend != static_cast<unsigned>(BackendChoice::None) && game_scale_backend_takes();
	in.mode_scale = games::render_scale_for_mode(settings_.upscale_mode);
	in.stretch_ran = stretch_ran;
	in.upscaled = stretch_ran && game_scale_.done;
	in.applied = stretch_ran ? game_->applied_render_scale() : 0.0f;
	games::ScaleDriverEvent ev = games::ScaleDriverEvent::None;
	const float answer = game_driver_.step(in, &ev);
	game_->set_render_scale(answer);
	const char *const title = game_->entry().title;
	switch (ev) {
	case games::ScaleDriverEvent::Asked:
		game_note_ = game_->note();
		diag_logf(DiagLevel::Info, "game", L"%hs: drawn at %.0f%% of its resolution (Upscaler input)", title,
			100.0 * answer);
		break;
	case games::ScaleDriverEvent::Retried:
		game_note_ = game_->note();
		diag_logf(DiagLevel::Info, "game", L"%hs: asked again to draw at %.0f%%", title, 100.0 * answer);
		break;
	case games::ScaleDriverEvent::HandedBack:
		diag_logf(DiagLevel::Info, "game", L"%hs: its own render scale setting is back (no upscaler)", title);
		break;
	case games::ScaleDriverEvent::NotTaken:
		game_note_ = L"the upscaler did not take the game's frame, so the game draws at its own setting for now";
		diag_logf(DiagLevel::Warn, "game", L"%hs: the upscaler took none of the game's frames for %u presents; the "
			L"game draws at its own setting and is asked again in %u", title, games::ScaleDriver::kGiveUp,
			games::ScaleDriver::kRest);
		break;
	case games::ScaleDriverEvent::NotFollowed:
		game_note_ = L"the game did not draw at the render scale asked of it, so it draws at its own setting for now";
		diag_logf(DiagLevel::Warn, "game", L"%hs: asked for %.3f, it drew at %.3f on %u frames in a row; the game draws "
			L"at its own setting and is asked again in %u presents", title, in.mode_scale, in.applied,
			games::ScaleDriver::kMismatches, games::ScaleDriver::kRest);
		break;
	default:
		break;
	}
}

bool App::run_game_scale(const GameScaleStretch &s)
{
	if (game_scale_.active || game_scale_.attempted || s.cmd == nullptr || !game_integrated() ||
		!settings_.enabled || settings_.backend == static_cast<unsigned>(BackendChoice::None) ||
		!game_scale_backend_takes())
		return false;
	reshade::api::device *const device = s.cmd->get_device();
	if (device == nullptr || is_engine_device(device))
		return false;
	const reshade::api::resource_desc sd = device->get_resource_desc(s.source);
	const reshade::api::resource_desc td = device->get_resource_desc(s.target);
	if (sd.texture.width != td.texture.width || sd.texture.height != td.texture.height ||
		sd.texture.format != td.texture.format || sd.texture.samples != 1 || td.texture.samples != 1 ||
		sd.texture.levels != 1 || td.texture.levels != 1 || sd.texture.depth_or_layers != 1 ||
		td.texture.depth_or_layers != 1) {
		diag_state("game_scale_shape", DiagLevel::Warn, "scale", L"render scale: the game stretches between two "
			L"textures that differ in size or format; its own stretch is left to draw the frame");
		return false;
	}
	reshade::api::effect_runtime *runtime = nullptr;
	{
		const std::lock_guard<std::mutex> lock(runtimes_lock_);
		for (const RuntimeDrive &d : runtimes_) {
			if (d.device == device) {
				runtime = d.runtime;
				break;
			}
		}
	}
	if (runtime == nullptr)
		return false;
	game_scale_.active = true;
	game_scale_.done = false;
	game_scale_.stretch = s;
	const bool before_aa = s.before.handle != 0 &&
		(game_integrated() ? game_->reads_before_game_aa() : settings_.game_render_scale_before_aa);
	if (before_aa)
		game_scale_.stretch.source = s.before;
	if (before_aa != game_scale_before_aa_said_) {
		game_scale_before_aa_said_ = before_aa;
		diag_info("scale", before_aa
			? L"render scale: the upscaler reads the game's frame from before its anti-aliasing"
			: L"render scale: the upscaler reads the game's frame as its last pass left it");
	}
	{
		const GameStateGuard keep(device, s.cmd);
		run_frame(runtime, s.cmd, reshade::api::resource_view{ 0 }, false);
	}
	game_scale_.active = false;
	game_scale_.attempted = true;
	(game_scale_.done ? game_scale_frames_ : game_scale_fallbacks_) += 1u;
	if (game_scale_.done != game_scale_said_) {
		game_scale_said_ = game_scale_.done;
		diag_logf(DiagLevel::Info, "scale", game_scale_.done
			? L"render scale: the game draws its scene at %ux%u of %ux%u (%.0f%%); the upscaler takes it there and the "
				L"game's interface is drawn over its picture at full size"
			: L"render scale: the game draws its scene at %ux%u of %ux%u (%.0f%%); the upscaler did not run, so the "
				L"game's own stretch draws the frame",
			s.render_w, s.render_h, s.width, s.height, 100.0 * s.render_w / (s.width != 0 ? s.width : 1u));
	}
	const uint64_t now = GetTickCount64();
	if (now >= game_scale_report_at_ + 10000u) {
		if (game_scale_report_at_ != 0)
			diag_logf(DiagLevel::Info, "scale", L"render scale: last 10 s %u frames upscaled from %ux%u to %ux%u, %u drawn "
				L"by the game's own stretch", game_scale_frames_, s.render_w, s.render_h, s.width, s.height,
				game_scale_fallbacks_);
		game_scale_report_at_ = now;
		game_scale_frames_ = game_scale_fallbacks_ = 0;
	}
	return game_scale_.done;
}

namespace {

void copy_corner12(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src, D3D12_RESOURCE_STATES src_state,
	ID3D12Resource *dst, D3D12_RESOURCE_STATES dst_state, uint32_t w, uint32_t h)
{
	barrier12(cmd, src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier12(cmd, dst, dst_state, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_TEXTURE_COPY_LOCATION to{}, from{};
	to.pResource = dst;
	to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	from.pResource = src;
	from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	const D3D12_BOX box{ 0, 0, 0, w, h, 1 };
	cmd->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
	barrier12(cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST, dst_state);
	barrier12(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);
}

bool ensure_tex12(EngineDevice &engine, ID3D12Device *device, ID3D12Resource **tex, uint32_t w, uint32_t h,
	DXGI_FORMAT fmt, D3D12_RESOURCE_STATES state)
{
	if (*tex != nullptr) {
		const D3D12_RESOURCE_DESC d = (*tex)->GetDesc();
		if (d.Width == w && d.Height == h && d.Format == fmt)
			return true;
		engine.wait_cpu(engine.last_submitted());
		(*tex)->Release();
		*tex = nullptr;
	}
	return create_tex12(device, w, h, fmt, false, state, tex);
}

}

bool App::enter_game_scale(FrameInputs &inputs)
{
	const GameScaleStretch &gs = game_scale_.stretch;
	ID3D12Device *const dev = inputs.engine.device;
	ID3D12GraphicsCommandList *const cmd = inputs.engine.cmd;
	ID3D12Resource *const plane = inputs.engine.color;
	if (dev == nullptr || cmd == nullptr || plane == nullptr)
		return false;
	const D3D12_RESOURCE_DESC pd = plane->GetDesc();
	const uint32_t rw = gs.render_w, rh = gs.render_h;
	if (rw == 0 || rh == 0 || rw > pd.Width || rh > pd.Height)
		return false;
	if (!ensure_tex12(engine_, dev, &game_scale_color_, rw, rh, pd.Format, inputs.engine.color_state))
		return false;
	copy_corner12(cmd, plane, inputs.engine.color_state, game_scale_color_, inputs.engine.color_state, rw, rh);
	ID3D12Resource *depth = nullptr;
	if (inputs.engine.depth != nullptr) {
		const D3D12_RESOURCE_DESC dd = inputs.engine.depth->GetDesc();
		if (rw <= dd.Width && rh <= dd.Height &&
			ensure_tex12(engine_, dev, &game_scale_depth_, rw, rh, dd.Format, inputs.engine.depth_state)) {
			copy_corner12(cmd, inputs.engine.depth, inputs.engine.depth_state, game_scale_depth_,
				inputs.engine.depth_state, rw, rh);
			depth = game_scale_depth_;
		}
	}
	inputs.engine.output = plane;
	inputs.engine.output_state = inputs.engine.color_state;
	inputs.engine.color = game_scale_color_;
	inputs.engine.depth = depth;
	if (depth == nullptr)
		inputs.have_depth = false;
	inputs.engine.scene = nullptr;
	inputs.width = rw;
	inputs.height = rh;
	return true;
}

void App::game_scale_motion(FrameInputs &inputs)
{
	const GameScaleStretch &gs = game_scale_.stretch;
	ID3D12Device *const dev = inputs.engine.device;
	ID3D12GraphicsCommandList *const cmd = inputs.engine.cmd;
	if (inputs.have_motion_vectors && inputs.engine.motion != nullptr && dev != nullptr && cmd != nullptr) {
		const D3D12_RESOURCE_DESC md = inputs.engine.motion->GetDesc();
		if (game_scale_blit_.ensure(dev) &&
			ensure_tex12(engine_, dev, &game_scale_motion_, gs.width, gs.height, md.Format,
				inputs.engine.motion_state)) {
			barrier12(cmd, game_scale_motion_, inputs.engine.motion_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
			const bool drawn = game_scale_blit_.draw_fullscreen(dev, cmd, inputs.engine.motion, game_scale_motion_,
				md.Format, gs.width, gs.height);
			barrier12(cmd, game_scale_motion_, D3D12_RESOURCE_STATE_RENDER_TARGET, inputs.engine.motion_state);
			if (drawn)
				inputs.engine.motion = game_scale_motion_;
		}
	}
	FrameInputs &s = game_scale_shown_;
	s = inputs;
	s.engine.color = inputs.engine.output;
	s.engine.color_state = inputs.engine.output_state;
	s.engine.output = nullptr;
	s.width = gs.width;
	s.height = gs.height;
}

void App::release_game_scale() noexcept
{
	ID3D12Resource **const all[] = { &game_scale_color_, &game_scale_depth_, &game_scale_motion_ };
	for (ID3D12Resource **r : all) {
		if (*r != nullptr) {
			(*r)->Release();
			*r = nullptr;
		}
	}
	game_scale_blit_.release();
	game_scale_shown_ = FrameInputs{};
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
	if (game_ != nullptr) {
		s.game_title = game_->entry().title;
		s.game_integrated = game_->attached();
		s.game_note = game_note_;
	}
	{
		const GameScaleState g = game_render_scale_state();
		if (settings_.game_render_scale && !game_scale_watching_)
			game_scale_watch_ = GameScaleWatch{ g.presents, g.missed_corner };
		game_scale_watching_ = settings_.game_render_scale;
		const bool api_ok = last_api_ == reshade::api::device_api::d3d10 || last_api_ == reshade::api::device_api::d3d11;
		s.game_scale_verdict = game_scale_verdict(settings_.game_render_scale, api_ok, g, game_scale_watch_);
	}
	if (game_integrated()) {
		const GameScaleState g = game_render_scale_state();
		if (g.replaced != 0 && g.found_frame + 90u >= g.presents) {
			s.game_scale_render_w = g.render_w;
			s.game_scale_render_h = g.render_h;
			s.game_scale_w = g.width;
			s.game_scale_h = g.height;
		}
	}
	s.d3d12 = d3d12;
	s.build_stamp = __DATE__ " " __TIME__;
	s.version = AEONSR_VERSION_STR;
	{
		const std::lock_guard<std::mutex> lock(report_lock_);
		s.report_path = report_shown_.empty() ? report_path_ : report_shown_;
		s.report_error = report_shown_error_.empty() ? report_error_ : report_shown_error_;
	}
	s.log_path = diag_log_path();

	s.native_dlss = game_dlss_for_layer();
	s.native_dlss_modules = native_dlss_.source;
	s.ngx_layer.clear();
	for (const NgxLayer &l : ngx_layers_)
		s.ngx_layer += (s.ngx_layer.empty() ? L"" : L", ") + ngx_layer_label(l);
	if (s.ngx_layer.empty() && runtime_load_redirected())
		s.ngx_layer = L"a program that hooks upscaler runtimes";
	{
		const EngineRoute r = engine_route_now();
		s.ngx_layer_relevant = sizeof(void *) != 4 && upscaler_layer_seen() && (r.all_in_host || r.dlss_in_host);
		s.optiscaler_offered = sizeof(void *) != 4 && !ngx_layer_table_.empty();
		s.dlss_without_layer = gpu_.may_be_nvidia() || dlss_override();
		s.optiscaler_upscales = r.dlss_to_layer;
		s.optiscaler_held = PanelState::OptiscalerHeld::No;
		if (s.optiscaler_offered && settings_.optiscaler_upscale && !r.dlss_to_layer)
			s.optiscaler_held = !settings_.enabled || settings_.backend != static_cast<unsigned>(BackendChoice::Dlss)
				? PanelState::OptiscalerHeld::PickDlss : PanelState::OptiscalerHeld::GameDlss;
	}
	s.gpu_vendor = gpu_.vendor;
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

		if (!fsr_is_11) {
			if (!fsr12_.fsr_loading()) {
				s.fsr_provider_version = fsr12_.provider_version;
				for (const auto &pv : fsr12_.providers)
					s.fsr_providers.push_back(pv.name);
			}
		}
	} else if (s.active_is_xess) {
		UpscalerBackend &xess = xess12_;
		s.upscaler_dll_present = probe ? xess.runtime_present() : xess.status != UpscalerStatus::MissingRuntime;
		s.upscaler_initialized = xess12_.xess_initialized();
		s.upscaler_out_width = xess12_.out_width();
		s.upscaler_out_height = xess12_.out_height();
		s.upscaler_render_width = xess12_.xess_render_width();
		s.upscaler_render_height = xess12_.xess_render_height();
	} else if (s.active_is_vsr) {
		s.upscaler_dll_present = true;
		if (remote_.ready() && runs_remote(active_)) {
			const uint32_t v = remote_info_.vsr_status;
			s.upscaler_initialized = (v & remote::kVsrQueried) != 0;
			s.vsr_capable = (v & remote::kVsrCapable) != 0;
			s.vsr_enabled = (v & remote::kVsrEnabled) != 0;
			s.vsr_in_use = (v & remote::kVsrInUse) != 0;
			s.vsr_level = (v >> 8) & 0xFFu;
		} else {
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
	}

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
		const NeuralRenderCommon::Text nt = nr.text();

		s.neural_dll_found = nr.dll_present && nr.shim_present;
		s.neural_dll_present = nr.dll_present;
		s.neural_shim_present = nr.shim_present;
		s.neural_initialized = nr.initialized;
		s.neural_crashed = nr.crashed;
		s.neural_driver_too_old = nr.driver_too_old();
		s.neural_status = nt.status;
		s.neural_error = nt.last_error;
		s.neural_driver_major = nr.driver_major;
		s.neural_driver_minor = nr.driver_minor;
		s.neural_required_driver_major = nr.required_driver_major;
		s.neural_required_driver_minor = nr.required_driver_minor;
		s.neural_requirement_known = nr.requirement_known;
		s.neural_adapter_unsupported = nr.adapter_unsupported();
		s.neural_min_architecture = nr.min_architecture;
		s.neural_gpu_architecture = nr.card_architecture;
		s.neural_runtime_serves_card = nr.serves_card(nt);
		s.neural_allgpu_build_targets = neural_allgpu_build_targets(nr.card_architecture);
		s.neural_runtime_kernels.clear();
		s.neural_runtime_cards.clear();
		if (nt.targets_known) {
			s.neural_runtime_kernels = neural_kernels_label(nt.kernels);
			s.neural_runtime_cards = neural_kernels_cards(nt.kernels);
			s.neural_runtime_file = nt.dll_path;
			s.neural_runtime_version = nt.identity.file_version;
			s.neural_runtime_certificate = nt.identity.has_certificate;
			s.neural_runtime_sha256 = nt.sha256;
		}
		s.neural_runtime_candidates = nt.candidates;
		s.neural_runtime_minimum = nr.runtime_minimum_architecture;
		s.neural_reported_architecture = nr.reported_architecture;
		s.neural_feature_unsupported = nr.feature_unsupported;
		s.neural_eval_count = nr.eval_count;
		s.neural_last = neural_last_;
		s.neural_init_failed = neural_init_failed_;
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
	s.frame_time_ms = last_plan_.params.frame_time_ms;
	s.jitter_active = last_plan_.jitter_active;
	s.jitter_refused = dlss11_.rt != nullptr && dlss11_.rt->jitter_failed;
	s.jitter_in_frame = last_plan_.params.jitter_in_frame;
	s.jitter_mixed = jitter_.drawn().ambiguous;
	s.jitter_still = jitter_.drawn().empty;
	s.jitter_withheld = offset_withheld();
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
	if (!s.host_ready && remote_tried_ && remote_wanted())
		s.host_error = remote_.last_error.empty() ? std::wstring(L"it did not start") : remote_.last_error;

	if (s.upscalers_remote && !s.host_ready) {
		s.neural_dll_found = true;
		s.neural_dll_present = true;
		s.neural_shim_present = true;
		s.upscaler_dll_present = true;
	}

	if (s.host_ready && active_ != nullptr && runs_remote(active_)) {
		if (remote_out_width_ != 0) {
			s.upscaler_out_width = remote_out_width_;
			s.upscaler_out_height = remote_out_height_;
		}
		if (remote_in_width_ != 0) {
			s.upscaler_render_width = remote_in_width_;
			s.upscaler_render_height = remote_in_height_;
		}
		if (remote_info_.valid && active_ == static_cast<const UpscalerBackend *>(&fsr12_)) {
			s.fsr_providers = remote_info_.fsr_providers;
			s.fsr_provider_version = remote_info_.fsr_provider_version;
		}
	}
	if (remote_info_.valid && neural_remote_now()) {
		const RemoteEngine::Info &i = remote_info_;
		const auto has = [&](uint32_t bit) { return (i.neural_flags & bit) != 0; };
		s.neural_dll_present = has(remote::kNeuralDllPresent);
		s.neural_shim_present = has(remote::kNeuralShimPresent);
		s.neural_dll_found = s.neural_dll_present && s.neural_shim_present;
		s.neural_initialized = has(remote::kNeuralInitialized);
		s.neural_crashed = has(remote::kNeuralCrashed);
		s.neural_driver_too_old = has(remote::kNeuralDriverTooOld);
		s.neural_adapter_unsupported = has(remote::kNeuralAdapterUnsupported);
		s.neural_feature_unsupported = has(remote::kNeuralFeatureUnsupported);
		s.neural_runtime_serves_card = has(remote::kNeuralServesCard);
		s.neural_requirement_known = has(remote::kNeuralRequirementKnown);
		s.neural_init_failed = s.neural_init_failed || has(remote::kNeuralInitFailed);
		s.neural_driver_major = i.neural_driver_major;
		s.neural_driver_minor = i.neural_driver_minor;
		s.neural_required_driver_major = i.neural_required_major;
		s.neural_required_driver_minor = i.neural_required_minor;
		s.neural_min_architecture = i.neural_min_architecture;
		s.neural_gpu_architecture = i.neural_card_architecture;
		s.neural_allgpu_build_targets = neural_allgpu_build_targets(i.neural_card_architecture);
		s.neural_runtime_minimum = i.neural_runtime_minimum;
		s.neural_reported_architecture = i.neural_reported_architecture;
		s.neural_eval_count = i.neural_eval_count;
		if (has(remote::kNeuralTargetsKnown)) {
			s.neural_runtime_kernels = i.neural_runtime_kernels;
			s.neural_runtime_cards = i.neural_runtime_cards;
			s.neural_runtime_file = i.neural_runtime_file;
			s.neural_runtime_version = i.neural_runtime_version;
			s.neural_runtime_certificate = has(remote::kNeuralCertificate);
			s.neural_runtime_sha256 = i.neural_runtime_sha256;
		}
		s.neural_area_share = i.neural_area_share;
		if (!i.neural_capture_status.empty())
			s.neural_capture_status = i.neural_capture_status;
		s.accum_running = i.accum_running;
		s.accum_holding = i.accum_holding;
		s.accum_done = i.accum_done;
		s.accum_target = i.accum_target;
	}

	if (s.upscalers_remote && s.host_ready) {
		s.host_pid = remote_.host_pid();
		const bool neural_there = remote_.neural_present();
		bool dll = neural_there, shim = neural_there;
		if (!neural_there)
			neural_files_beside(module_directory(module_), &dll, &shim);
		s.neural_dll_found = neural_there;
		s.neural_dll_present = dll;
		s.neural_shim_present = shim;
		s.neural_host_refused = !neural_there && dll && shim;
		if (s.active_is_dlss)
			s.upscaler_dll_present = remote_.backend_present(BackendChoice::Dlss);
		else if (s.active_is_fsr)
			s.upscaler_dll_present = remote_.backend_present(BackendChoice::Fsr);
		else if (s.active_is_xess)
			s.upscaler_dll_present = remote_.backend_present(BackendChoice::Xess);
	}

	s.upscaler_label = upscaler_display_name(s.active_name, s.active_is_fsr,
		diag_narrow(s.fsr_provider_version));
	if (s.optiscaler_upscales && s.active_is_dlss)
		s.upscaler_label = "OptiScaler (DLSS input)";
#if AEONSR_FRAME_GENERATION
	s.framegen = framegen_.readout();
#endif
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
		++remote_extras_.accum_epoch;
		remote_extras_.accum_target = actions.accum_start ? clamp_accum_iterations(settings_.accum_iterations) : 0u;
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
		update_game_scale_hooks();
	}
}

void App::draw_overlay(reshade::api::effect_runtime *runtime)
{
	i18n::follow_reshade(runtime);
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
		const std::lock_guard<std::mutex> lock(report_lock_);
		report_shown_.clear();
		report_shown_error_.clear();
		return;
	}
	start_report(state);
}

namespace {

FlightApi flight_api_of(reshade::api::device_api api) noexcept
{
	switch (api) {
	case reshade::api::device_api::d3d9: return FlightApi::D3D9;
	case reshade::api::device_api::d3d10: return FlightApi::D3D10;
	case reshade::api::device_api::d3d11: return FlightApi::D3D11;
	case reshade::api::device_api::d3d12: return FlightApi::D3D12;
	case reshade::api::device_api::opengl: return FlightApi::OpenGL;
	case reshade::api::device_api::vulkan: return FlightApi::Vulkan;
	default: return FlightApi::Unknown;
	}
}

uint16_t flight_u16(double v) noexcept
{
	return v > 0.0 ? static_cast<uint16_t>(std::min(v, 65535.0)) : 0u;
}

std::wstring widen_utf8(const std::string &s)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(n > 0 ? static_cast<size_t>(n) : 0u, L'\0');
	if (n > 0)
		MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
	return w;
}

void remove_staging(const std::wstring &dir)
{
	for (const std::wstring &sub : { join_path(dir, L"frame"), dir }) {
		WIN32_FIND_DATAW fd{};
		const HANDLE h = FindFirstFileW(join_path(sub, L"*").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
					DeleteFileW(join_path(sub, fd.cFileName).c_str());
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		RemoveDirectoryW(sub.c_str());
	}
}

}

void App::flight_commit()
{
	FlightRecord &r = flight_.cur();
	LARGE_INTEGER now{}, freq{};
	QueryPerformanceCounter(&now);
	QueryPerformanceFrequency(&freq);
	const char *note = nullptr, *pause = nullptr;
	if (trace_open_) {
		const ScopeTraceRow &t = trace_cur_;
		r.api = flight_api_of(last_api_);
		if (t.has(ScopeColumn::ToldX)) {
			r.told_x = static_cast<float>(t.get(ScopeColumn::ToldX));
			r.told_y = static_cast<float>(t.get(ScopeColumn::ToldY));
		}
		if (t.has(ScopeColumn::Ran))
			r.ran = t.get(ScopeColumn::Ran) == 1.0 ? 1 : 0;
		if (t.has(ScopeColumn::RenderW)) {
			r.render_w = flight_u16(t.get(ScopeColumn::RenderW));
			r.render_h = flight_u16(t.get(ScopeColumn::RenderH));
		}
		if (t.get(ScopeColumn::Restore) == 1.0)
			r.hud |= kFlightRestore;
		if (t.get(ScopeColumn::Register) == 1.0)
			r.hud |= kFlightRegistered;
		const JitterFrame &d = jitter_.drawn();
		if (d.valid) {
			r.drawn_x = d.x;
			r.drawn_y = d.y;
			r.drawn_index = static_cast<uint16_t>(d.index);
		}
		r.moved = flight_u16(d.moved);
		r.plain = flight_u16(d.plain);
		if (r.plan != 0xFFu && active_ != nullptr) {
			const UpscalerBackend *const a = active_;
			r.upscaler = a == &dlss12_ ? FlightUpscaler::Dlss : a == &dlss11_ ? FlightUpscaler::DlssD3D11
				: a == &fsr12_ ? FlightUpscaler::Fsr : a == &fsr11_ ? FlightUpscaler::FsrD3D11
				: a == &xess12_ ? FlightUpscaler::Xess : a == &vsr11_ ? FlightUpscaler::Vsr : FlightUpscaler::None;
			r.status = static_cast<uint8_t>(a->status);
		}
		if (settings_.neural_render) {
			r.neural = static_cast<int8_t>(neural_last_);
			if (neural_last_ == 1)
				r.neural_ms = static_cast<float>(neural12_.timer.last_ms);
		}
		if (r.plan == 0xFFu)
			r.jitter = FlightJitter::Idle;
		else if (!settings_.spatial_jitter)
			r.jitter = FlightJitter::Off;
		else if (offset_withheld())
			r.jitter = FlightJitter::Withheld;
		else if (!jitter_.gate_open())
			r.jitter = FlightJitter::Paused;
		else switch (static_cast<JitterDecision>(r.plan)) {
			case JitterDecision::Drawn: r.jitter = FlightJitter::Drawn; break;
			case JitterDecision::Split: r.jitter = FlightJitter::Split; break;
			case JitterDecision::Ambiguous: r.jitter = FlightJitter::Ambiguous; break;
			case JitterDecision::Empty: r.jitter = FlightJitter::Empty; break;
			case JitterDecision::Resampled: r.jitter = FlightJitter::Resampled; break;
			case JitterDecision::Off: r.jitter = FlightJitter::NotDrawn; break;
		}
		note = jitter_note_;
		if (r.jitter == FlightJitter::Paused)
			pause = jitter_.pause_reason();
	}
	if (landing_.readings() != flight_landing_reads_) {
		flight_landing_reads_ = landing_.readings();
		r.landing_x = landing_.factor_x();
		r.landing_y = landing_.factor_y();
	}
	r.game_jitter = !game_jitter_.measured() ? FlightGameJitter::Unmeasured
		: game_jitter_.jitters() ? FlightGameJitter::Yes : FlightGameJitter::No;
	flight_.commit(now.QuadPart, freq.QuadPart, flight_fault_.at(trace_presents_), note, pause);
}

void App::start_report(const PanelState &state)
{
	report_path_ = diag_write_report(state, settings_, &report_error_);
	{
		const std::lock_guard<std::mutex> lock(report_lock_);
		if (report_phase_ != ReportPhase::None)
			return;
		report_phase_ = ReportPhase::Frame;
	}
	SYSTEMTIME t{};
	GetLocalTime(&t);
	wchar_t stamp[64]{};
	_snwprintf_s(stamp, _TRUNCATE, L"AeonSR_report-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour,
		t.wMinute, t.wSecond);
	const std::wstring dir = module_directory(module_);
	report_job_ = ReportJob{};
	report_job_.zip_path = join_path(dir, (std::wstring(stamp) + L".zip").c_str());
	report_job_.staging = join_path(dir, (std::wstring(stamp) + L".tmp").c_str());
	report_job_.flight_csv = flight_.csv_now(0.0);
	if (upscaler_capture_.scope_busy())
		report_job_.frame_note = "a jitter scope capture was already recording";
	else
		report_job_.frame_dir = join_path(report_job_.staging, L"frame");
	{
		const std::lock_guard<std::mutex> lock(report_lock_);
		report_shown_ = L"writing " + report_job_.zip_path;
		report_shown_error_.clear();
	}
	diag_info("report", L"report: writing " + report_job_.zip_path);
}

void App::service_report()
{
	{
		const std::lock_guard<std::mutex> lock(report_lock_);
		if (report_phase_ != ReportPhase::Frame)
			return;
	}
	ReportJob &j = report_job_;
	if (!j.frame_dir.empty() && !j.frame_asked) {
		upscaler_capture_.request_scope(j.frame_dir, 1u, 1ull << 30,
			(1u << static_cast<uint32_t>(ScopePlane::Drawn)) | (1u << static_cast<uint32_t>(ScopePlane::Final)));
		j.frame_asked = true;
		j.deadline = GetTickCount64() + 3000u;
		return;
	}
	if (j.frame_asked && upscaler_capture_.scope_busy()) {
		const unsigned long long now = GetTickCount64();
		if (now >= j.deadline && upscaler_capture_.scope_recording()) {
			upscaler_capture_.scope_cancel();
			j.frame_note = "no frame reached the add-on within 3 s";
		}
		if (now < j.deadline + 10000u)
			return;
		if (j.frame_note.empty())
			j.frame_note = "the frame was still being written after 13 s";
	}
	const bool pictures = j.frame_asked && j.frame_note.empty();
	const bool remove = !j.frame_asked || !upscaler_capture_.scope_busy();
	ReportBundle b;
	b.zip_path = j.zip_path;
	const std::wstring dir = module_directory(module_);
	std::wstring base = widen_utf8(reshade_base_path());
	if (base.empty())
		base = dir;
	b.files = {
		{ "AeonSR.log", diag_log_path() },
		{ "AeonSRHost.log", join_path(dir, L"AeonSRHost.log") },
		{ "AeonSRHost-window.log", join_path(dir, L"AeonSRHost-window.log") },
		{ "ReShade.log", join_path(base, L"ReShade.log") },
		{ "AeonSR.ini", ini_path_ },
		{ "ReShade.ini", join_path(base, L"ReShade.ini") },
	};
	if (report_error_.empty() && !report_path_.empty())
		b.files.push_back({ "AeonSR_report.txt", report_path_ });
	b.flight_csv = std::move(j.flight_csv);
	b.frame_dir = pictures ? j.frame_dir : std::wstring();
	b.frame_note = j.frame_note;
	SYSTEMTIME t{};
	GetLocalTime(&t);
	char summary[256]{};
	std::snprintf(summary, sizeof(summary), "Aeon SR %s report, %04u-%02u-%02u %02u:%02u:%02u.\n"
		"Attach this file when you report a problem.\n", AEONSR_VERSION_STR, t.wYear, t.wMonth, t.wDay, t.wHour,
		t.wMinute, t.wSecond);
	b.summary = summary;
	const std::wstring staging = remove ? j.staging : std::wstring();
	{
		const std::lock_guard<std::mutex> lock(report_lock_);
		report_phase_ = ReportPhase::Writing;
	}
	flight_.post([this, b = std::move(b), staging]() {
		uint64_t bytes = 0;
		std::wstring error;
		const bool ok = write_report_bundle(b, &bytes, &error);
		if (!staging.empty())
			remove_staging(staging);
		{
			const std::lock_guard<std::mutex> lock(report_lock_);
			report_phase_ = ReportPhase::None;
			report_shown_ = ok ? b.zip_path : std::wstring();
			report_shown_error_ = ok ? std::wstring() : L"the report could not be written: " + error;
		}
		if (ok)
			diag_logf(DiagLevel::Info, "report", L"report: wrote %ls (%.1f MB)", b.zip_path.c_str(),
				static_cast<double>(bytes) / (1024.0 * 1024.0));
		else
			diag_logf(DiagLevel::Warn, "report", L"report: %ls could not be written: %ls", b.zip_path.c_str(),
				error.c_str());
	});
}

void App::draw_osd(reshade::api::effect_runtime *runtime)
{
	if (!settings_.show_osd)
		return;
	i18n::follow_reshade(runtime);
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
