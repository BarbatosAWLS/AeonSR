#pragma once

#include "aeon_sr/framegen/display_clock.hpp"
#include "aeon_sr/framegen/frame_gen_stage.hpp"
#include "aeon_sr/framegen/frame_pacer.hpp"
#include "aeon_sr/framegen/frame_presenter.hpp"
#include "aeon_sr/framegen/overlay_presenter.hpp"
#include "aeon_sr/interop/interop.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aeon_sr {

struct Settings;

struct FrameGenReadout {
	bool on = false;
	bool generating = false;
	PacerState state = PacerState::Measuring;
	std::wstring status;
	double real_fps = 0.0;
	double shown_fps = 0.0;
	double added_latency_ms = 0.0;
	double screen_latency_ms = 0.0;
	uint32_t even_cap_fps = 0;
	double gpu_ms = 0.0;
	double refresh_hz = 0.0;
	bool vsync_forced = false;
	bool own_window = false;
	double own_real_per_s = 0.0, own_generated_per_s = 0.0;
	uint32_t own_steps = 0;
	bool own_auto = true;
	uint32_t own_steps_max = 0;
	uint32_t own_asked = 0;
	double screen_hz = 0.0;
	bool through_game_present = false;
	uint64_t generated = 0;
	std::string route;
	FrameGenerator generator = FrameGenerator::AeonSR;
	std::wstring generator_version;
};

class FrameGeneration {
public:
	~FrameGeneration();

	void on_present(reshade::api::effect_runtime *runtime, FrameBridge &bridge, EngineDevice &engine,
		const Settings &settings);
	void note_blocked(const std::wstring &why);
	void set_color_space(VendorColor c) noexcept { color_space_ = c; }

	void off();
	void swapchain_gone(reshade::api::swapchain *swapchain, EngineDevice &engine);
	void engine_gone();
	void device_gone(reshade::api::device *device);

	bool active() const noexcept { return runtime_ != nullptr; }
	bool own_window_active() const noexcept { return overlay_active_; }
	static void note_vulkan_present_mode(uint32_t mode) noexcept;
	FrameGenReadout readout() const;

private:
	bool round(reshade::api::effect_runtime *runtime, FrameBridge &bridge, EngineDevice &engine,
		const Settings &settings);
	bool round_own_window(reshade::api::effect_runtime *runtime, FrameBridge &bridge, EngineDevice &engine,
		double presented_ms, const Settings &settings);
	bool own_window_ready(reshade::api::effect_runtime *runtime, EngineDevice &engine, std::wstring *why,
		bool *permanent);
	void on_present_own_window(reshade::api::effect_runtime *runtime, FrameBridge &bridge, EngineDevice &engine,
		double entry, double limit_ms, const Settings &settings);
	bool ensure_timer(ID3D12Device *device);
	void read_timer(EngineDevice &engine);
	void release_timer() noexcept;
	void dump_prepare(reshade::api::effect_runtime *runtime, const Settings &settings);
	void dump_take(reshade::api::effect_runtime *runtime, const Settings &settings, bool restored_check);
	void release_dumps() noexcept;
	void sleep_until(double t_ms);
	bool presents_wait_for_vblank(reshade::api::effect_runtime *runtime);

	reshade::api::effect_runtime *runtime_ = nullptr;
	VendorColor color_space_ = VendorColor::Sdr;
	FrameGenStage stage_;
	FramePacer pacer_;
	DisplayClock clock_;
	FramePresenter presenter_;
	std::wstring note_;
	uint64_t generated_ = 0;
	bool route_given_up_ = false;
	bool sync_watched_ = false;
	bool sync_watch_failed_ = false;
	bool blt_present_ = false;
	OverlayPresenter overlay_;
	bool overlay_refused_ = false;
	std::wstring overlay_refusal_;
	bool overlay_active_ = false;
	uint32_t own_steps_ = 0;
	uint32_t own_vendor_steps_ = 0;
	OverlayStats own_logged_;
	uint32_t own_steps_max_ = 0;
	uint32_t own_asked_ = 0;
	bool display_paced_ = false;
	static constexpr uint32_t kCostFrames = 60;
	double cost_ring_[kCostFrames] = {};
	double own_ring_[kCostFrames] = {};
	uint32_t cost_n_ = 0, cost_at_ = 0;
	double slow_cost_ms_ = 0.0, slow_own_ms_ = 0.0;
	bool route_too_slow_ = false;
	void check_route_cost(double cost, double own);
	void *timer_handle_ = nullptr;

	static constexpr uint32_t kTimerRing = 4;
	ID3D12Device *timer_device_ = nullptr;
	ID3D12QueryHeap *timer_heap_ = nullptr;
	ID3D12Resource *timer_rb_ = nullptr;
	uint64_t timer_fence_[kTimerRing] = {};
	bool timer_used_[kTimerRing] = {};
	uint32_t timer_at_ = 0;
	double gpu_ms_ = 0.0;

	reshade::api::resource dump_rb_[4] = {};
	uint64_t rounds_ = 0;
	uint64_t prev_round_ = ~0ull;
	uint32_t dump_seq_ = 0;
	uint32_t restore_checked_ = 0, restore_mismatched_ = 0;

	struct Window {
		uint32_t frames = 0, generated = 0, failed = 0, reentries = 0;
		double uneven_sum = 0.0;
		uint32_t pairs = 0;
		std::vector<double> real, round, begin, record, end, waits;
		std::vector<double> gpu_waits;
	} window_;
	double part_begin_ = 0.0, part_record_ = 0.0, part_end_ = 0.0;
	double window_start_ = 0.0;
	double last_return_ = -1.0;
	void log_window(double now);
};

}
