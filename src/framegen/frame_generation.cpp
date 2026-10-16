#include "aeon_sr/framegen/frame_generation.hpp"
#include "aeon_sr/framegen/present_sync.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/interop/interop_bridges.hpp"

#include <Windows.h>
#include <d3d10.h>
#include <d3d11.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace aeon_sr {

using namespace reshade::api;

namespace {

double now_ms() noexcept
{
	return qpc_ms();
}

constexpr uint32_t kPresentGiveUp = 30;

bool dumpable(format f) noexcept
{
	switch (f) {
	case format::r8g8b8a8_unorm:
	case format::r8g8b8a8_unorm_srgb:
	case format::r8g8b8x8_unorm:
	case format::r8g8b8x8_unorm_srgb:
	case format::b8g8r8a8_unorm:
	case format::b8g8r8a8_unorm_srgb:
	case format::b8g8r8x8_unorm:
	case format::b8g8r8x8_unorm_srgb:
		return true;
	default:
		return false;
	}
}

std::atomic<uint32_t> g_vulkan_present_mode{ present_sync::kUnknown };

bool is_dxgi(device_api a) noexcept
{
	return a == device_api::d3d10 || a == device_api::d3d11 || a == device_api::d3d12;
}

void wait_for_gpu(effect_runtime *runtime)
{
	const device_api api = runtime->get_device()->get_api();
	const double until = qpc_ms() + 100.0;
	if (api == device_api::d3d11) {
		auto *const ctx = reinterpret_cast<ID3D11DeviceContext *>(static_cast<uintptr_t>(runtime->get_command_queue()->get_native()));
		ID3D11Device *dev = nullptr;
		ctx->GetDevice(&dev);
		D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
		ID3D11Query *q = nullptr;
		if (dev != nullptr && SUCCEEDED(dev->CreateQuery(&qd, &q))) {
			ctx->End(q);
			ctx->Flush();
			while (ctx->GetData(q, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE && qpc_ms() < until)
				bridge_util::short_wait();
			q->Release();
		}
		if (dev != nullptr)
			dev->Release();
		return;
	}
	if (api == device_api::d3d10) {
		auto *const dev = reinterpret_cast<ID3D10Device *>(static_cast<uintptr_t>(runtime->get_device()->get_native()));
		D3D10_QUERY_DESC qd{ D3D10_QUERY_EVENT, 0 };
		ID3D10Query *q = nullptr;
		if (SUCCEEDED(dev->CreateQuery(&qd, &q))) {
			q->End();
			dev->Flush();
			while (q->GetData(nullptr, 0, D3D10_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE && qpc_ms() < until)
				bridge_util::short_wait();
			q->Release();
		}
		return;
	}
	runtime->get_command_queue()->wait_idle();
}

bool bgr(format f) noexcept
{
	return f == format::b8g8r8a8_unorm || f == format::b8g8r8a8_unorm_srgb || f == format::b8g8r8x8_unorm ||
		f == format::b8g8r8x8_unorm_srgb;
}

}

void FrameGeneration::note_vulkan_present_mode(uint32_t mode) noexcept
{
	g_vulkan_present_mode.store(mode, std::memory_order_relaxed);
}

bool FrameGeneration::presents_wait_for_vblank(effect_runtime *runtime)
{
	const device_api api = runtime->get_device()->get_api();
	if (is_dxgi(api)) {
		auto *const sc = reinterpret_cast<IDXGISwapChain *>(static_cast<uintptr_t>(runtime->get_native()));
		DXGI_SWAP_CHAIN_DESC desc{};
		blt_present_ = sc != nullptr && SUCCEEDED(sc->GetDesc(&desc)) &&
			(desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD || desc.SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL);
		if (!sync_watched_ && !sync_watch_failed_) {
			sync_watched_ = present_sync::watch(reinterpret_cast<void *>(static_cast<uintptr_t>(runtime->get_native())));
			sync_watch_failed_ = !sync_watched_;
			if (!sync_watched_)
				diag_logf(DiagLevel::Warn, "framegen", L"frame generation: the game's presents could not be watched for "
					L"their vsync; the frames are spaced on the CPU's clock");
		}
		return sync_watched_;
	}
	if (api == device_api::d3d9) {
		auto *const sc = reinterpret_cast<IDirect3DSwapChain9 *>(static_cast<uintptr_t>(runtime->get_native()));
		D3DPRESENT_PARAMETERS pp{};
		const bool ok = sc != nullptr && SUCCEEDED(sc->GetPresentParameters(&pp));
		blt_present_ = ok && pp.Windowed != FALSE && pp.SwapEffect != D3DSWAPEFFECT_FLIPEX;
		return ok && (pp.PresentationInterval == D3DPRESENT_INTERVAL_DEFAULT || pp.PresentationInterval == D3DPRESENT_INTERVAL_ONE);
	}
	if (api == device_api::opengl) {
		blt_present_ = true;
		using GetProc = PROC(WINAPI *)(LPCSTR);
		using GetInterval = int(WINAPI *)();
		static const GetInterval get_interval = [] {
			const HMODULE gl = GetModuleHandleW(L"opengl32.dll");
			const auto get_proc = gl != nullptr ? reinterpret_cast<GetProc>(GetProcAddress(gl, "wglGetProcAddress")) : nullptr;
			return get_proc != nullptr ? reinterpret_cast<GetInterval>(get_proc("wglGetSwapIntervalEXT")) : nullptr;
		}();
		if (get_interval == nullptr)
			return false;
		const int interval = get_interval();
		return interval == 1 || interval == -1;
	}
	if (api == device_api::vulkan) {
		blt_present_ = false;
		const uint32_t mode = g_vulkan_present_mode.load(std::memory_order_relaxed);
		return mode == 2u || mode == 3u || mode == 1000361000u;
	}
	return false;
}

FrameGeneration::~FrameGeneration()
{
	release_timer();
	if (timer_handle_ != nullptr)
		CloseHandle(static_cast<HANDLE>(timer_handle_));
}

void FrameGeneration::sleep_until(double t_ms)
{
	double left = t_ms - now_ms();
	if (left <= 0.0)
		return;
	if (timer_handle_ == nullptr) {
		timer_handle_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (timer_handle_ == nullptr)
			timer_handle_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
	}
	if (timer_handle_ == nullptr) {
		Sleep(static_cast<DWORD>(left));
		return;
	}
	LARGE_INTEGER due{};
	due.QuadPart = -static_cast<LONGLONG>(left * 10000.0);
	if (SetWaitableTimer(static_cast<HANDLE>(timer_handle_), &due, 0, nullptr, nullptr, FALSE))
		WaitForSingleObject(static_cast<HANDLE>(timer_handle_), 1000);
}

void FrameGeneration::note_blocked(const std::wstring &why)
{
	note_ = why;
	overlay_.stop();
	overlay_active_ = false;
	if (sync_watched_)
		present_sync::force_vsync(false);
}

void FrameGeneration::off()
{
	if (sync_watched_)
		present_sync::forget();
	sync_watched_ = sync_watch_failed_ = false;
	if (runtime_ == nullptr)
		return;
	if (presenter_.ready() || dump_rb_[0].handle != 0)
		runtime_->get_command_queue()->wait_idle();
	overlay_.stop();
	overlay_active_ = overlay_refused_ = false;
	overlay_refusal_.clear();
	release_dumps();
	presenter_.release();
	stage_.reset();
	pacer_.reset();
	clock_.stop();
	present_sync::forget();
	sync_watched_ = sync_watch_failed_ = false;
	display_paced_ = false;
	runtime_ = nullptr;
	route_given_up_ = false;
	note_.clear();
}

void FrameGeneration::swapchain_gone(swapchain *sc, EngineDevice &engine)
{
	if (runtime_ == nullptr || sc == nullptr || sc->get_native() != runtime_->get_native())
		return;
	overlay_.stop();
	overlay_active_ = overlay_refused_ = false;
	release_dumps();
	presenter_.release();
	if (stage_.has_vendors()) {
		if (engine.ready())
			engine.wait_cpu(engine.last_submitted());
		stage_.release_vendors();
	}
	stage_.reset();
	pacer_.reset();
	clock_.stop();
	present_sync::forget();
	sync_watched_ = sync_watch_failed_ = false;
	display_paced_ = false;
	runtime_ = nullptr;
	route_given_up_ = false;
}

void FrameGeneration::device_gone(device *dev)
{
	if (runtime_ == nullptr || dev == nullptr || runtime_->get_device() != dev)
		return;
	for (resource &r : dump_rb_)
		r = { 0 };
	prev_round_ = ~0ull;
	overlay_.stop();
	overlay_active_ = overlay_refused_ = false;
	presenter_.forget();
	stage_.reset();
	pacer_.reset();
	clock_.stop();
	present_sync::forget();
	sync_watched_ = sync_watch_failed_ = false;
	display_paced_ = false;
	runtime_ = nullptr;
	route_given_up_ = false;
}

void FrameGeneration::engine_gone()
{
	overlay_.stop();
	overlay_active_ = false;
	release_timer();
	stage_.release();
	pacer_.reset();
}

bool FrameGeneration::ensure_timer(ID3D12Device *device)
{
	if (timer_device_ == device && timer_heap_ != nullptr)
		return true;
	release_timer();
	if (device == nullptr)
		return false;
	D3D12_QUERY_HEAP_DESC qd{};
	qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	qd.Count = 2 * kTimerRing;
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC rd{};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = sizeof(uint64_t) * 2 * kTimerRing;
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&timer_heap_))) ||
		FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			IID_PPV_ARGS(&timer_rb_)))) {
		release_timer();
		return false;
	}
	timer_device_ = device;
	return true;
}

void FrameGeneration::release_timer() noexcept
{
	if (timer_heap_ != nullptr)
		timer_heap_->Release();
	if (timer_rb_ != nullptr)
		timer_rb_->Release();
	timer_heap_ = nullptr;
	timer_rb_ = nullptr;
	timer_device_ = nullptr;
	for (uint32_t i = 0; i < kTimerRing; ++i) {
		timer_used_[i] = false;
		timer_fence_[i] = 0;
	}
	timer_at_ = 0;
}

void FrameGeneration::read_timer(EngineDevice &engine)
{
	const uint32_t s = timer_at_;
	if (!timer_used_[s] || timer_rb_ == nullptr)
		return;
	if (!engine.borrowed() && engine.fence() != nullptr && engine.fence()->GetCompletedValue() < timer_fence_[s])
		return;
	UINT64 freq = 0;
	if (engine.queue() == nullptr || FAILED(engine.queue()->GetTimestampFrequency(&freq)) || freq == 0)
		return;
	const D3D12_RANGE range{ s * 16u, s * 16u + 16u };
	uint64_t *t = nullptr;
	if (FAILED(timer_rb_->Map(0, &range, reinterpret_cast<void **>(&t))))
		return;
	const uint64_t a = t[s * 2], b = t[s * 2 + 1];
	const D3D12_RANGE none{ 0, 0 };
	timer_rb_->Unmap(0, &none);
	if (b > a) {
		const double ms = static_cast<double>(b - a) * 1000.0 / static_cast<double>(freq);
		gpu_ms_ = gpu_ms_ == 0.0 ? ms : gpu_ms_ + 0.1 * (ms - gpu_ms_);
	}
}

bool FrameGeneration::round(effect_runtime *runtime, FrameBridge &bridge, EngineDevice &engine,
	const Settings &settings)
{
	const double t0 = now_ms();
	part_begin_ = part_record_ = part_end_ = 0.0;
	if (!presenter_.ensure(runtime, &note_))
		return false;
	++rounds_;
	presenter_.copy_back_buffer(runtime);
	dump_prepare(runtime, settings);

	command_list *const cmd = runtime->get_command_queue()->get_immediate_command_list();
	if (!engine.use_ring(EngineDevice::Ring::FrameGeneration)) {
		note_ = L"frame generation: the engine's list was still open";
		return false;
	}
	BridgeInputs in;
	in.color = presenter_.real_copy();
	BridgeFrame f;
	const BridgeStep step = bridge.begin(runtime, cmd, in, f);
	if (step != BridgeStep::Ok) {
		engine.abandon_list();
		engine.use_ring(EngineDevice::Ring::Frame);
		note_ = L"frame generation: the frame could not be carried to the engine device (" +
			std::wstring(bridge_step_label(step), bridge_step_label(step) + std::strlen(bridge_step_label(step))) +
			(bridge.last_error.empty() ? L")" : L": " + bridge.last_error + L")");
		stage_.reset();
		return false;
	}
	const double t1 = now_ms();
	part_begin_ = t1 - t0;
	ID3D12Device *const device = engine.device();
	ID3D12GraphicsCommandList *const list = f.cmd;
	ID3D12Resource *const dst = f.native ? reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(presenter_.generated().handle)) : f.color;
	const D3D12_RESOURCE_STATES dst_state = f.native ? D3D12_RESOURCE_STATE_RENDER_TARGET : f.color_state;

	read_timer(engine);
	const bool timed = ensure_timer(device);
	const uint32_t slot = timer_at_;
	if (timed)
		list->EndQuery(timer_heap_, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
	FrameGenOptions o;
	o.generator = settings.frame_generator < kFrameGeneratorCount
		? static_cast<FrameGenerator>(settings.frame_generator) : FrameGenerator::AeonSR;
	const double real_ms = pacer_.real_interval_ms();
	o.frame_ms = real_ms > 0.0 ? real_ms : 16.7;
	o.window = static_cast<HWND>(runtime->get_hwnd());
	o.color_space = color_space_;
	o.queue = engine.queue();
	bool list_lost = false;
	o.flush = [&]() -> bool {
		if (f.native) {
			runtime->get_command_queue()->flush_immediate_command_list();
			return true;
		}
		list_lost = !engine.submit_list() || engine.begin_list() != list;
		return !list_lost;
	};
	std::wstring why;
	const FrameGenResult r = stage_.record(device, list, f.color, f.color_state, dst, dst_state, o, &why);
	if (list_lost) {
		engine.use_ring(EngineDevice::Ring::Frame);
		note_ = why.empty() ? L"frame generation: the engine's list could not be reopened" : why;
		part_record_ = now_ms() - t1;
		return false;
	}
	if (timed) {
		list->EndQuery(timer_heap_, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
		list->ResolveQueryData(timer_heap_, D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, timer_rb_, slot * 16u);
	}
	const double t2 = now_ms();
	part_record_ = t2 - t1;
	const BridgeStep end = bridge.end(runtime, cmd, presenter_.generated());
	if (end != BridgeStep::Ok)
		engine.abandon_list();
	engine.use_ring(EngineDevice::Ring::Frame);
	part_end_ = now_ms() - t2;
	if (end != BridgeStep::Ok) {
		note_ = L"frame generation: the generated frame did not come back from the engine device (" +
			std::wstring(bridge_step_label(end), bridge_step_label(end) + std::strlen(bridge_step_label(end))) +
			(bridge.last_error.empty() ? L")" : L": " + bridge.last_error + L")");
		stage_.reset();
		return false;
	}
	if (timed) {
		timer_used_[slot] = true;
		timer_fence_[slot] = engine.last_submitted();
		timer_at_ = (timer_at_ + 1) % kTimerRing;
	}
	if (r == FrameGenResult::Failed) {
		note_ = why;
		return false;
	}
	if (r != FrameGenResult::Generated) {
		note_ = std::wstring(L"frame generation: ") + (r == FrameGenResult::Building
			? L"building its shaders, a moment" : L"warming up");
		return false;
	}
	note_.clear();
	return true;
}

bool FrameGeneration::own_window_ready(effect_runtime *runtime, EngineDevice &engine, std::wstring *why,
	bool *permanent)
{
	*permanent = true;
	const device_api api = runtime->get_device()->get_api();
	if (is_dxgi(api)) {
		auto *const sc = reinterpret_cast<IDXGISwapChain *>(static_cast<uintptr_t>(runtime->get_native()));
		BOOL fullscreen = FALSE;
		if (sc != nullptr && SUCCEEDED(sc->GetFullscreenState(&fullscreen, nullptr)) && fullscreen) {
			*why = L"the game is in exclusive fullscreen, where no window can show over it";
			return false;
		}
	} else if (api == device_api::d3d9) {
		auto *const sc = reinterpret_cast<IDirect3DSwapChain9 *>(static_cast<uintptr_t>(runtime->get_native()));
		D3DPRESENT_PARAMETERS pp{};
		if (sc != nullptr && SUCCEEDED(sc->GetPresentParameters(&pp)) && !pp.Windowed) {
			*why = L"the game is in exclusive fullscreen, where no window can show over it";
			return false;
		}
	}
	*permanent = false;
	if (!presenter_.ensure(runtime, why))
		return false;
	if (engine.device() == nullptr) {
		*why = L"frame generation: no engine device for its own window yet";
		return false;
	}
	*permanent = true;
	const resource_desc d = runtime->get_device()->get_resource_desc(presenter_.real_copy());
	return overlay_.ensure(engine.device(), static_cast<HWND>(runtime->get_hwnd()), d.texture.width, d.texture.height,
		static_cast<DXGI_FORMAT>(d.texture.format), color_space_, &clock_, why);
}

bool FrameGeneration::round_own_window(effect_runtime *runtime, FrameBridge &bridge, EngineDevice &engine,
	double presented_ms, const Settings &settings)
{
	const double t0 = now_ms();
	part_begin_ = part_record_ = part_end_ = 0.0;
	++rounds_;
	presenter_.copy_back_buffer(runtime);
	command_list *const cmd = runtime->get_command_queue()->get_immediate_command_list();
	if (!engine.use_ring(EngineDevice::Ring::FrameGeneration)) {
		note_ = L"frame generation: the engine's list was still open";
		return false;
	}
	BridgeInputs in;
	in.color = presenter_.real_copy();
	BridgeFrame f;
	const BridgeStep step = bridge.begin(runtime, cmd, in, f);
	if (step != BridgeStep::Ok) {
		engine.abandon_list();
		engine.use_ring(EngineDevice::Ring::Frame);
		note_ = L"frame generation: the frame could not be carried to the engine device (" +
			std::wstring(bridge_step_label(step), bridge_step_label(step) + std::strlen(bridge_step_label(step))) +
			(bridge.last_error.empty() ? L")" : L": " + bridge.last_error + L")");
		stage_.reset();
		overlay_.reset_frames();
		return false;
	}
	const double t1 = now_ms();
	part_begin_ = t1 - t0;

	const FrameGenerator asked = settings.frame_generator < kFrameGeneratorCount
		? static_cast<FrameGenerator>(settings.frame_generator) : FrameGenerator::AeonSR;
	const uint32_t multiplier = settings.frame_generation_multiplier >= 2 &&
		settings.frame_generation_multiplier <= kFrameGenerationMultiplierMax ? settings.frame_generation_multiplier : 0u;
	const bool vendor_asked = asked != FrameGenerator::AeonSR && stage_.vendor_usable(asked);
	double vblank = 0.0, refresh = 0.0;
	const DisplayGrid monitor = clock_.grid();
	if (monitor.valid && monitor.period_ms > 2.0)
		refresh = monitor.period_ms;
	else if (!compositor_clock(&vblank, &refresh))
		refresh = 0.0;
	const double real_ms = pacer_.real_interval_ms();
	uint32_t steps = multiplier;
	if (vendor_asked) {
		own_steps_max_ = stage_.vendor_max_generated() + 1;
		steps = multiplier != 0 ? std::min(multiplier, own_steps_max_)
			: auto_multiplier(real_ms, refresh, own_vendor_steps_, own_steps_max_);
		own_vendor_steps_ = steps;
	} else {
		own_steps_max_ = kFrameGenerationMultiplierMax;
	}
	own_asked_ = multiplier;
	const uint32_t made_wanted = vendor_asked && steps >= 2 ? steps - 1 : 0u;
	const DXGI_FORMAT made_format = made_wanted > 0
		? FrameGenStage::vendor_output_format(engine.device(), f.color->GetDesc().Format) : DXGI_FORMAT_UNKNOWN;

	OverlayFeed slot;
	const bool slot_ok = overlay_.begin_feed(engine.queue(), &slot, made_wanted, made_format);
	FrameGenResult r = FrameGenResult::Failed;
	std::wstring why;
	bool list_lost = false;
	if (slot_ok) {
		FrameGenOptions o;
		o.feed = &slot;
		o.made = made_wanted;
		o.generator = asked;
		o.frame_ms = real_ms > 0.0 ? real_ms : 16.7;
		o.window = static_cast<HWND>(runtime->get_hwnd());
		o.color_space = color_space_;
		o.queue = engine.queue();
		o.flush = [&]() -> bool {
			if (f.native) {
				runtime->get_command_queue()->flush_immediate_command_list();
				return true;
			}
			list_lost = !engine.submit_list() || engine.begin_list() != f.cmd;
			return !list_lost;
		};
		r = stage_.record(engine.device(), f.cmd, f.color, f.color_state, f.color, f.color_state, o, &why);
	}
	if (list_lost) {
		engine.use_ring(EngineDevice::Ring::Frame);
		overlay_.abandon_feed(engine.queue(), true);
		overlay_.reset_frames();
		stage_.reset();
		note_ = why.empty() ? L"frame generation: the engine's list could not be reopened" : why;
		part_record_ = now_ms() - t1;
		return false;
	}
	const bool by_vendor = vendor_asked && stage_.active_generator() == asked;
	if (!by_vendor)
		steps = multiplier;
	else if (stage_.last_made() > 0)
		steps = stage_.last_made() + 1;
	own_steps_ = steps;
	const double t2 = now_ms();
	part_record_ = t2 - t1;
	const BridgeStep end = bridge.end(runtime, cmd, presenter_.generated());
	if (end != BridgeStep::Ok)
		engine.abandon_list();
	engine.use_ring(EngineDevice::Ring::Frame);
	part_end_ = now_ms() - t2;
	if (end != BridgeStep::Ok) {
		note_ = L"frame generation: the frame did not come back from the engine device (" +
			std::wstring(bridge_step_label(end), bridge_step_label(end) + std::strlen(bridge_step_label(end))) +
			(bridge.last_error.empty() ? L")" : L": " + bridge.last_error + L")");
		stage_.reset();
		overlay_.abandon_feed(engine.queue());
		overlay_.reset_frames();
		return false;
	}
	if (r != FrameGenResult::Generated && r != FrameGenResult::Warming) {
		if (f.native)
			runtime->get_command_queue()->flush_immediate_command_list();
		overlay_.abandon_feed(engine.queue());
		note_ = r == FrameGenResult::Building ? L"frame generation: building its shaders, a moment" : why;
		return false;
	}
	if (f.native)
		runtime->get_command_queue()->flush_immediate_command_list();
	const bool pairs = r == FrameGenResult::Generated && !(by_vendor && steps < 2);
	overlay_.end_feed(engine.queue(), presented_ms, pairs, steps >= 2 ? steps : 0u, stage_.last_made(),
		by_vendor && made_wanted > 0);
	note_.clear();
	return true;
}

void FrameGeneration::on_present_own_window(effect_runtime *runtime, FrameBridge &bridge, EngineDevice &engine,
	double entry, double limit_ms, const Settings &settings)
{
	if (!overlay_active_) {
		overlay_active_ = true;
		pacer_.reset();
		stage_.reset();
		if (sync_watched_)
			present_sync::force_vsync(false);
		diag_logf(DiagLevel::Info, "framegen", L"frame generation: shown through a window of its own over the game's, "
			L"every refresh of the display getting a picture; the game runs at its own rate");
	}
	const bool hitched = !pacer_.begin(entry) && pacer_.state() == PacerState::Engaged;
	if (hitched)
		overlay_.reset_frames();
	const bool fed = round_own_window(runtime, bridge, engine, entry, settings);
	const double planned = now_ms();
	window_.round.push_back(planned - entry);
	window_.begin.push_back(part_begin_);
	window_.record.push_back(part_record_);
	window_.end.push_back(part_end_);
	if (fed) {
		++generated_;
		++window_.generated;
	}
	const PacerPlan plan = pacer_.plan(planned, false, limit_ms);
	sleep_until(plan.real_at);
	const double ret = now_ms();
	pacer_.returned(ret);
	++window_.frames;
	window_.waits.push_back(ret - planned);
	if (last_return_ >= 0.0)
		window_.real.push_back(ret - last_return_);
	last_return_ = ret;
	if (window_start_ == 0.0)
		window_start_ = ret;
	if (ret - window_start_ >= 10000.0) {
		Window &w = window_;
		const auto med = [](std::vector<double> &v) {
			if (v.empty())
				return 0.0;
			std::sort(v.begin(), v.end());
			return v[v.size() / 2];
		};
		const OverlayStats s = overlay_.stats();
		const std::string by = frame_generator_label(stage_.active_generator());
		wchar_t steps[48];
		if (own_steps_ >= 2)
			std::swprintf(steps, 48, L"%ux%ls", own_steps_, own_asked_ == 0 ? L" (Auto)" : L"");
		else if (own_steps_ == 1)
			std::swprintf(steps, 48, L"1x (Auto: the game is as fast as the screen)");
		else
			std::swprintf(steps, 48, L"every refresh (Auto)");
		diag_logf(DiagLevel::Info, "framegen", L"frame generation (its own window): last %.1f s, %u real frames %.2f ms "
			L"apart, %u handed over; made by %hs at %ls; on the screen %.1f real and %.1f made frames a second at %.1f Hz "
			L"(%ls), %llu real frames left out, %llu refreshes held, %llu repeated by the multiplier, %.1f ms behind the "
			L"game (%.1f of it until a frame is usable); on the screen %.1f ms after the game's own time (real frames "
			L"%.1f, p90 %.1f, %u seen; held %.1f ms after ready), decided %.1f ms before its refresh, %llu missed, %llu late, shown %.1f ms after the blank planned (earliest %.1f); on the game's thread, median ms: round %.2f (begin %.2f, "
			L"estimator %.2f, end %.2f), wait %.2f; window %ls", (ret - window_start_) / 1000.0, w.frames, med(w.real),
			w.generated, by.c_str(), steps, s.real_per_s, s.generated_per_s, s.period_ms > 0.0 ? 1000.0 / s.period_ms : 0.0,
			s.paced_by == OverlayStats::kPacedByMonitor ? L"the monitor's vertical blanks"
				: s.paced_by == OverlayStats::kPacedByCompositor ? L"the compositor's clock" : L"the swap chain's releases",
			static_cast<unsigned long long>(s.skipped - std::min(s.skipped, own_logged_.skipped)),
			static_cast<unsigned long long>(s.held - std::min(s.held, own_logged_.held)),
			static_cast<unsigned long long>(s.repeated - std::min(s.repeated, own_logged_.repeated)), s.latency_ms,
			s.usable_lag_ms, s.screen_ms_p50, s.screen_real_ms_p50, s.screen_real_ms_p90, s.screen_samples, s.held_real_ms_p50, s.lead_ms,
			static_cast<unsigned long long>(s.missed - std::min(s.missed, own_logged_.missed)),
			static_cast<unsigned long long>(s.late - std::min(s.late, own_logged_.late)), s.offset_p50_ms, s.offset_min_ms,
			med(w.round), med(w.begin), med(w.record), med(w.end), med(w.waits),
			!s.visible ? L"hidden (the game is not in front)"
				: s.in_host ? L"shown, from AeonSRHost.exe" : L"shown, from the game's process");
		own_logged_ = s;
		window_ = Window{};
		window_start_ = ret;
	}
}

void FrameGeneration::on_present(effect_runtime *runtime, FrameBridge &bridge, EngineDevice &engine,
	const Settings &settings)
{
	if (runtime == nullptr)
		return;
	static thread_local bool inside = false;
	if (inside) {
		++window_.reentries;
		return;
	}
	struct Inside {
		bool &flag;
		explicit Inside(bool &f) : flag(f) { flag = true; }
		~Inside() { flag = false; }
	} guard(inside);
	if (runtime_ != nullptr && runtime_ != runtime)
		return;
	if (runtime_ == nullptr) {
		runtime_ = runtime;
		pacer_.reset();
		stage_.reset();
		cost_n_ = cost_at_ = 0;
		route_too_slow_ = false;
	}
	clock_.watch(static_cast<HWND>(runtime->get_hwnd()));
	const double limit_ms = settings.frame_generation_limit != 0
		? 1000.0 / static_cast<double>(settings.frame_generation_limit) : 0.0;
	const double entry = now_ms();
	if (settings.frame_generation_own_window && !overlay_refused_ && !route_given_up_) {
		static const std::wstring beside = overlay_window_host_beside_addon(), none;
		overlay_.set_window_host(settings.frame_generation_window_host ? beside : none);
		std::wstring why;
		bool permanent = false;
		if (own_window_ready(runtime, engine, &why, &permanent)) {
			on_present_own_window(runtime, bridge, engine, entry, limit_ms, settings);
			return;
		}
		overlay_.stop();
		overlay_active_ = false;
		if (!permanent) {
			note_ = why;
			return;
		}
		overlay_refused_ = true;
		overlay_refusal_ = why;
		diag_logf(DiagLevel::Warn, "framegen", L"frame generation: %ls; the frames are presented from the game's own "
			L"present instead, one made frame per real one", why.c_str());
	}
	if (overlay_.running())
		overlay_.stop();
	overlay_active_ = false;
	const bool want = !route_given_up_ && !route_too_slow_ && pacer_.begin(entry);
	bool generated = false;
	if (want)
		generated = round(runtime, bridge, engine, settings);
	else
		stage_.reset();

	const double planned = now_ms();
	window_.round.push_back(planned - entry);
	if (want) {
		window_.begin.push_back(part_begin_);
		window_.record.push_back(part_record_);
		window_.end.push_back(part_end_);
	}
	const DisplayGrid display = clock_.grid();
	display_paced_ = display.valid && presents_wait_for_vblank(runtime);
	if (sync_watched_)
		present_sync::force_vsync(display_paced_ && !route_given_up_ && !route_too_slow_);
	presenter_.set_sync_interval(display_paced_ && is_dxgi(runtime->get_device()->get_api()) ? 1u : 0u);
	PacerGrid grid;
	if (display_paced_) {
		grid.period_ms = display.period_ms;
		grid.vblank_ms = display.vblank_ms;
	}
	const PacerPlan plan = pacer_.plan(planned, generated, limit_ms, grid);
	double real_at = plan.real_at;
	double gen_issued = -1.0;
	if (plan.generate) {
		sleep_until(plan.gen_at);
		std::wstring why;
		gen_issued = now_ms();
		const bool ok = presenter_.present_generated(runtime, &why);
		if (ok) {
			++generated_;
			++window_.generated;
		} else {
			++window_.failed;
			++present_failures_;
			note_ = why;
			if (presenter_.failures_in_a_row() >= kPresentGiveUp) {
				route_given_up_ = true;
				diag_logf(DiagLevel::Warn, "framegen", L"frame generation: %u generated frames in a row could not be "
					L"presented (%ls); given up on this swap chain", kPresentGiveUp, why.c_str());
			}
		}
		dump_take(runtime, settings, ok);
		if (ok && display_paced_ && blt_present_) {
			const double before = now_ms();
			wait_for_gpu(runtime);
			const double done = now_ms();
			window_.gpu_waits.push_back(done - before);
			real_at = pacer_.after_generated(real_at, done, grid);
		}
	}
	sleep_until(real_at);
	const double ret = now_ms();
	pacer_.returned(ret);
	if (generated && last_return_ >= 0.0)
		check_route_cost(planned - entry, entry - last_return_);

	++window_.frames;
	window_.waits.push_back(ret - planned);
	if (last_return_ >= 0.0) {
		window_.real.push_back(ret - last_return_);
		if (gen_issued >= 0.0) {
			const double a = gen_issued - last_return_, b = ret - gen_issued;
			if (!display_paced_ && a + b > 0.0) {
				window_.uneven_sum += std::fabs(a - b) / (a + b);
				++window_.pairs;
			}
		}
	}
	last_return_ = ret;
	if (window_start_ == 0.0)
		window_start_ = ret;
	if (ret - window_start_ >= 10000.0)
		log_window(ret);
}

void FrameGeneration::check_route_cost(double cost, double own)
{
	cost_ring_[cost_at_] = cost;
	own_ring_[cost_at_] = own;
	cost_at_ = (cost_at_ + 1) % kCostFrames;
	cost_n_ = std::min(cost_n_ + 1, kCostFrames);
	if (cost_n_ < kCostFrames)
		return;
	double c[kCostFrames], o[kCostFrames];
	std::copy(cost_ring_, cost_ring_ + kCostFrames, c);
	std::copy(own_ring_, own_ring_ + kCostFrames, o);
	std::nth_element(c, c + kCostFrames / 2, c + kCostFrames);
	std::nth_element(o, o + kCostFrames / 2, o + kCostFrames);
	const double mc = c[kCostFrames / 2], mo = o[kCostFrames / 2];
	if (mc > 6.0 && mc > 0.4 * mo) {
		route_too_slow_ = true;
		slow_cost_ms_ = mc;
		slow_own_ms_ = mo;
		stage_.reset();
		diag_logf(DiagLevel::Warn, "framegen", L"frame generation: each generated frame costs the game's thread "
			L"%.1f ms on this graphics route, against %.1f ms of the game's own work; it would cut the real frame "
			L"rate by more than it adds, so it is off for this window", mc, mo);
	}
}

void FrameGeneration::log_window(double now)
{
	Window &w = window_;
	const auto mp = [](std::vector<double> &v, double *m, double *p) {
		*m = *p = 0.0;
		if (v.empty())
			return;
		std::sort(v.begin(), v.end());
		*m = v[v.size() / 2];
		*p = v[std::min(v.size() - 1, v.size() * 9 / 10)];
	};
	double rm, rp, om, op, bm, bp, cm, cp, em, ep, wm, wp;
	mp(w.real, &rm, &rp);
	mp(w.round, &om, &op);
	mp(w.begin, &bm, &bp);
	mp(w.record, &cm, &cp);
	mp(w.end, &em, &ep);
	mp(w.waits, &wm, &wp);
	double gm, gp;
	mp(w.gpu_waits, &gm, &gp);
	wchar_t pacing[192];
	if (display_paced_ && blt_present_)
		std::swprintf(pacing, 192, L"paced by the display's vertical blanks, the real one after the refresh that takes "
			L"the generated one (its GPU work waited for %.2f/%.2f ms)", gm, gp);
	else if (display_paced_)
		std::swprintf(pacing, 192, L"paced by the display's vertical blanks");
	else
		std::swprintf(pacing, 192, L"paced on the CPU clock, the generated one %.1f%% off halfway on average",
			w.pairs ? 100.0 * w.uneven_sum / w.pairs : 0.0);
	wchar_t sync[64] = L"not seen";
	const uint32_t game_sync = sync_watched_ ? present_sync::game_sync_interval() : present_sync::kUnknown;
	if (game_sync != present_sync::kUnknown)
		std::swprintf(sync, 64, L"%u%ls", game_sync, display_paced_ && game_sync != 1 ? L", given 1 while generating" : L"");
	diag_logf(DiagLevel::Info, "framegen", L"frame generation: last %.1f s, %u real frames %.2f ms apart (p90 %.2f), "
		L"%u generated presented, %u failed, %u re-entries; %ls; on the "
		L"presenting thread, median/p90 ms: round %.2f/%.2f (begin %.2f/%.2f, record %.2f/%.2f, end %.2f/%.2f), waits "
		L"with the extra present %.2f/%.2f; %hs, GPU %.2f ms (%hs); display %.2f Hz (spread %.3f); the game's sync "
		L"interval %ls",
		(now - window_start_) / 1000.0, w.frames, rm, rp, w.generated, w.failed, w.reentries, pacing, om, op, bm, bp,
		cm, cp, em, ep, wm, wp, pacer_state_label(pacer_.state()), gpu_ms_,
		presenter_.route().empty() ? "no extra present yet" : presenter_.route().c_str(),
		clock_.measured_period_ms() > 0.0 ? 1000.0 / clock_.measured_period_ms() : 0.0, clock_.spread(), sync);
	window_ = Window{};
	window_start_ = now;
}

FrameGenReadout FrameGeneration::readout() const
{
	FrameGenReadout r;
	r.on = runtime_ != nullptr;
	r.generating = r.on && !route_given_up_ && !route_too_slow_;
	r.state = pacer_.state();
	r.generated = generated_;
	r.present_failures = present_failures_;
	r.route = presenter_.route();
	r.gpu_ms = gpu_ms_;
	r.generator = stage_.active_generator();
	r.generator_version = stage_.vendor_version();
	r.through_game_present = overlay_refused_;
	const DisplayGrid display = clock_.grid();
	const bool paced = display_paced_ && display.valid && display.period_ms > 0.0;
	r.refresh_hz = paced ? 1000.0 / display.period_ms : 0.0;
	const uint32_t game_sync = sync_watched_ ? present_sync::game_sync_interval() : present_sync::kUnknown;
	r.vsync_forced = paced && game_sync != present_sync::kUnknown && game_sync != 1;
	const double real = pacer_.real_interval_ms();
	if (real > 0.0) {
		r.real_fps = 1000.0 / real;
		r.shown_fps = pacer_.state() == PacerState::Engaged ? 2.0 * r.real_fps : r.real_fps;
		if (pacer_.state() == PacerState::Engaged)
			r.added_latency_ms = paced ? display.period_ms : 0.5 * real;
	}
	if (overlay_active_) {
		const OverlayStats s = overlay_.stats();
		r.own_window = true;
		r.own_real_per_s = s.real_per_s;
		r.own_generated_per_s = s.generated_per_s;
		r.shown_fps = s.real_per_s + s.generated_per_s;
		r.added_latency_ms = s.held_real_ms_p50;
		r.screen_latency_ms = s.screen_real_ms_p50;
		r.own_steps = own_steps_;
		r.own_auto = own_asked_ == 0;
		r.own_steps_max = own_steps_max_;
		r.own_asked = own_asked_;
		const DisplayGrid grid = clock_.grid();
		r.screen_hz = s.compositor_paced && s.period_ms > 0.0 ? 1000.0 / s.period_ms
			: grid.valid && grid.period_ms > 0.0 ? 1000.0 / grid.period_ms : 0.0;
		r.even_cap_fps = even_cap_fps(s.real_per_s, r.screen_hz);
		r.status = note_;
		if (r.status.empty() && s.real_per_s <= 0.0)
			r.status = L"Starting.";
		const std::wstring &fallback = stage_.fallback_note();
		if (!fallback.empty())
			r.status += (r.status.empty() ? L"" : L" ") + fallback + L".";
		return r;
	}
	if (route_given_up_) {
		r.status = L"Off for this window: its swap chain would not show the extra frames. " + note_;
	} else if (route_too_slow_) {
		wchar_t b[256];
		std::swprintf(b, 256, L"Off for this window: on this game's graphics route each made-up frame takes %.0f ms "
			L"of the game's own time (its work takes %.0f). Turn the option off and on to try again.", slow_cost_ms_,
			slow_own_ms_);
		r.status = b;
	} else if (!note_.empty()) {
		r.status = note_;
	} else if (overlay_refused_) {
		r.status = L"Not in its own window: " + overlay_refusal_ + L".";
	} else if (!stage_.fallback_note().empty()) {
		r.status = stage_.fallback_note() + L".";
	} else if (pacer_.state() == PacerState::Measuring) {
		r.status = L"Starting.";
	} else if (r.vsync_forced) {
		r.status = L"Vsync is on while generating.";
	}
	return r;
}

void FrameGeneration::release_dumps() noexcept
{
	device *const dev = runtime_ != nullptr ? runtime_->get_device() : nullptr;
	for (resource &r : dump_rb_) {
		if (r.handle != 0 && dev != nullptr)
			dev->destroy_resource(r);
		r = { 0 };
	}
	prev_round_ = ~0ull;
}

void FrameGeneration::dump_prepare(effect_runtime *runtime, const Settings &settings)
{
	if (settings.frame_generation_dump == 0)
		return;
	device *const dev = runtime->get_device();
	const resource_desc bd = dev->get_resource_desc(presenter_.real_copy());
	if (!dumpable(format_to_default_typed(bd.texture.format, 0)))
		return;
	if (dump_rb_[0].handle == 0) {
		const resource_desc rd(bd.texture.width, bd.texture.height, 1, 1, format_to_default_typed(bd.texture.format, 0),
			1, memory_heap::readback, resource_usage::copy_dest);
		for (resource &r : dump_rb_) {
			if (!dev->create_resource(rd, nullptr, resource_usage::copy_dest, &r)) {
				release_dumps();
				return;
			}
		}
	}
	if ((rounds_ + 1) % settings.frame_generation_dump != 0)
		return;
	command_list *const cmd = runtime->get_command_queue()->get_immediate_command_list();
	cmd->barrier(presenter_.real_copy(), resource_usage::render_target, resource_usage::copy_source);
	cmd->copy_texture_region(presenter_.real_copy(), 0, nullptr, dump_rb_[3], 0, nullptr);
	cmd->barrier(presenter_.real_copy(), resource_usage::copy_source, resource_usage::render_target);
	prev_round_ = rounds_;
}

void FrameGeneration::dump_take(effect_runtime *runtime, const Settings &settings, bool presented)
{
	if (settings.frame_generation_dump == 0 || dump_rb_[0].handle == 0 || !presented ||
		rounds_ % settings.frame_generation_dump != 0 || prev_round_ + 1 != rounds_)
		return;
	device *const dev = runtime->get_device();
	command_queue *const q = runtime->get_command_queue();
	command_list *const cmd = q->get_immediate_command_list();
	const bool vulkan = dev->get_api() == device_api::vulkan;
	cmd->barrier(presenter_.real_copy(), resource_usage::render_target, resource_usage::copy_source);
	cmd->copy_texture_region(presenter_.real_copy(), 0, nullptr, dump_rb_[1], 0, nullptr);
	cmd->barrier(presenter_.real_copy(), resource_usage::copy_source, resource_usage::render_target);
	cmd->barrier(presenter_.generated(), resource_usage::render_target, resource_usage::copy_source);
	cmd->copy_texture_region(presenter_.generated(), 0, nullptr, dump_rb_[2], 0, nullptr);
	cmd->barrier(presenter_.generated(), resource_usage::copy_source, resource_usage::render_target);
	const resource bb = current_back_buffer_of(runtime);
	if (!vulkan && bb.handle != 0) {
		cmd->barrier(bb, resource_usage::present, resource_usage::copy_source);
		cmd->copy_texture_region(bb, 0, nullptr, dump_rb_[0], 0, nullptr);
		cmd->barrier(bb, resource_usage::copy_source, resource_usage::present);
	}
	q->flush_immediate_command_list();
	q->wait_idle();

	const resource_desc d = dev->get_resource_desc(dump_rb_[1]);
	const bool swap_rb = bgr(format_to_default_typed(d.texture.format, 0));
	const uint32_t w = d.texture.width, h = d.texture.height;
	subresource_data m[4] = {};
	bool mapped[4] = {};
	for (int i = 0; i < 4; ++i)
		mapped[i] = dev->map_texture_region(dump_rb_[i], 0, nullptr, map_access::read_only, &m[i]) && m[i].data != nullptr;
	if (!vulkan && bb.handle != 0 && mapped[0] && mapped[1]) {
		uint32_t differ = 0;
		for (uint32_t y = 0; y < h; ++y)
			for (uint32_t x = 0; x < w; ++x)
				differ += std::memcmp(static_cast<const uint8_t *>(m[0].data) + y * m[0].row_pitch + x * 4,
					static_cast<const uint8_t *>(m[1].data) + y * m[1].row_pitch + x * 4, 3) != 0 ? 1u : 0u;
		++restore_checked_;
		restore_mismatched_ += differ != 0 ? 1u : 0u;
		diag_logf(differ != 0 ? DiagLevel::Warn : DiagLevel::Info, "framegen", L"frame generation: the frame the game "
			L"presents after the extra present differs from the real frame in %u pixels (%u of %u checks differed)",
			differ, restore_mismatched_, restore_checked_);
	}
	std::wstring dir = settings.frame_generation_dump_dir.empty() ? std::wstring()
		: std::wstring(settings.frame_generation_dump_dir.begin(), settings.frame_generation_dump_dir.end());
	if (dir.empty()) {
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&now_ms), &self);
		dir = join_path(module_directory(self), L"framegen-dumps");
	}
	CreateDirectoryW(dir.c_str(), nullptr);
	const auto write = [&](int slot, const wchar_t *what) {
		if (!mapped[slot])
			return;
		wchar_t name[64];
		std::swprintf(name, 64, L"\\fg_%05u_%ls.rgba", dump_seq_, what);
		FILE *f = _wfopen((dir + name).c_str(), L"wb");
		if (f == nullptr)
			return;
		std::fprintf(f, "AEONFG1 %u %u\n", w, h);
		std::vector<uint8_t> row(static_cast<size_t>(w) * 4);
		for (uint32_t y = 0; y < h; ++y) {
			const uint8_t *src = static_cast<const uint8_t *>(m[slot].data) + y * m[slot].row_pitch;
			for (uint32_t x = 0; x < w; ++x) {
				const uint8_t *p = src + x * 4;
				uint8_t *o = row.data() + x * 4;
				o[0] = swap_rb ? p[2] : p[0];
				o[1] = p[1];
				o[2] = swap_rb ? p[0] : p[2];
				o[3] = 255;
			}
			std::fwrite(row.data(), 1, row.size(), f);
		}
		std::fclose(f);
	};
	write(3, L"prev");
	write(1, L"real");
	write(2, L"gen");
	++dump_seq_;
	for (int i = 0; i < 4; ++i)
		if (mapped[i])
			dev->unmap_texture_region(dump_rb_[i], 0);
}

}
