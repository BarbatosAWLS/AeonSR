#pragma once

#include "aeon_sr/framegen/display_clock.hpp"
#include "aeon_sr/framegen/frame_interpolator.hpp"
#include "aeon_sr/framegen/overlay_scheduler.hpp"
#include "aeon_sr/framegen/vendor_frame_gen.hpp"
#include "aeon_sr/framegen/window_host_link.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aeon_sr {

inline constexpr uint32_t kOverlayMaxMade = 5;

bool compositor_clock(double *vblank_ms, double *period_ms) noexcept;

double monitor_refresh_hz(HMONITOR m) noexcept;

struct FenceReached {
	double at = -1.0;
	bool waited = false;
	bool exact = false;
};
inline constexpr double kFenceSlackMs = 0.5;
FenceReached fence_reached(ID3D12Fence *fence, uint64_t value, HANDLE event, HANDLE quit, double timeout_ms,
	double since) noexcept;

bool overlay_creating_device() noexcept;
bool overlay_on_own_thread() noexcept;
void overlay_note_own_device(uint64_t native) noexcept;
bool overlay_forget_own_device(uint64_t native) noexcept;

std::wstring overlay_window_host_beside_addon();

struct OverlayFeed {
	static constexpr D3D12_RESOURCE_STATES kRestState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
	ID3D12Resource *color = nullptr;
	ID3D12Resource *motion = nullptr;
	ID3D12Resource *global = nullptr;
	ID3D12Resource *made[kOverlayMaxMade] = {};
	uint32_t made_count = 0;
	DXGI_FORMAT made_format = DXGI_FORMAT_UNKNOWN;
};

struct OverlayStats {
	bool running = false;
	bool visible = false;
	double latency_ms = 0.0;
	double usable_lag_ms = 0.0;
	uint64_t reals = 0, skipped = 0, generated = 0, held = 0;
	uint64_t repeated = 0;
	uint64_t made_shown = 0;
	double real_per_s = 0.0, generated_per_s = 0.0;
	double period_ms = 0.0;
	bool compositor_paced = false;
	double screen_ms_p50 = 0.0, screen_real_ms_p50 = 0.0, screen_real_ms_p90 = 0.0;
	uint32_t screen_samples = 0;
	double held_real_ms_p50 = 0.0;
	double ready_wait_p90 = 0.0, ready_wait_max = 0.0;
	double ready_wait_long = 0.0;
	double thread_waitable_p90 = 0.0, thread_waitable_max = 0.0;
	double thread_late_p90 = 0.0, thread_late_max = 0.0;
	double thread_render_p90 = 0.0, thread_render_max = 0.0;
	double thread_present_p90 = 0.0, thread_present_max = 0.0;
	double gpu_d3d12_p90 = 0.0, gpu_d3d11_p90 = 0.0;
	double monitor_hz = 0.0;
	double lead_ms = 0.0;
	double work_ms = 0.0;
	uint64_t work_seen = 0, work_found = 0, work_untimed = 0;
	uint64_t late = 0;
	uint64_t missed = 0;
	uint64_t missed_late_work = 0;
	double offset_p50_ms = 0.0, offset_min_ms = 0.0;
	static constexpr uint8_t kPacedByReleases = 0, kPacedByMonitor = 1, kPacedByCompositor = 2;
	uint8_t paced_by = kPacedByReleases;
	bool in_host = false;
	uint32_t host_pid = 0;
	int host_gpu_class = -1;
	std::wstring note;
};

class OverlayPresenter {
public:
	static constexpr uint32_t kSlots = OverlayScheduler::kKeep;

	~OverlayPresenter();

	void set_window_host(const std::wstring &exe)
	{
		if (exe != host_exe_)
			host_exe_ = exe;
	}

	bool ensure(ID3D12Device *device, HWND game_window, uint32_t w, uint32_t h, DXGI_FORMAT frame_format,
		VendorColor color_space, DisplayClock *clock, std::wstring *why);
	bool running() const noexcept { return thread_.joinable(); }

	bool begin_feed(ID3D12CommandQueue *queue, OverlayFeed *slot, uint32_t made = 0,
		DXGI_FORMAT made_format = DXGI_FORMAT_UNKNOWN);
	void end_feed(ID3D12CommandQueue *queue, double presented_ms, bool pairs, uint32_t steps = 0, uint32_t made = 0,
		bool vendor = false);
	void abandon_feed(ID3D12CommandQueue *queue, bool made_dirty = false) noexcept;
	void reset_frames() noexcept;

	void stop() noexcept;

	OverlayStats stats() const;

private:
	struct Slot {
		ID3D12Resource *color = nullptr, *motion = nullptr, *global = nullptr;
		ID3D12Resource *made[kOverlayMaxMade] = {};
		DXGI_FORMAT made_format = DXGI_FORMAT_UNKNOWN;
		uint64_t id = 0;
		uint64_t read_until = 0;
	};
	struct Pending {
		uint64_t id;
		double presented_ms;
		uint64_t fence_value;
		bool pairs;
		uint32_t steps;
		uint32_t made;
		bool vendor;
	};
	struct Retired {
		ID3D12Resource *res;
		uint64_t read_until;
		uint64_t feed_until;
	};
	static constexpr uint32_t kMadeBackoff = 240;
	uint32_t made_backoff_ = 0;
	uint64_t feed_read_until_ = 0;
	void free_retired(bool all) noexcept;

	bool make_ring(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT family, std::wstring *why);
	void release_ring() noexcept;
	void run();
	void ready_run();
	double ready_time(uint64_t fence_value);
	bool thread_init(std::wstring *why);
	bool init_host(std::wstring *why);
	bool init_local(std::wstring *why);
	bool start_sink();
	void release_sink(bool gpu_idle = true) noexcept;
	void wait_sink_done() noexcept;
	void lose_host(uint64_t promised) noexcept;
	bool switch_to_local();
	void thread_shutdown() noexcept;
	void follow_window();
	bool render(const OverlayChoice &c);
	void wait_until(double t_ms) noexcept;

	mutable std::mutex mutex_;
	Slot slots_[kSlots];
	std::vector<Pending> pending_;
	bool reset_pending_ = false;
	OverlayStats stats_;

	ID3D12Device *device_ = nullptr;
	HWND game_window_ = nullptr;
	uint32_t w_ = 0, h_ = 0;
	DXGI_FORMAT family_ = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT swap_format_ = DXGI_FORMAT_UNKNOWN;
	VendorColor color_space_ = VendorColor::Sdr;
	DisplayClock *clock_ = nullptr;
	ID3D12Fence *feed_fence_ = nullptr;
	uint64_t feed_value_ = 0;
	ID3D12Fence *read_fence_ = nullptr;
	uint64_t next_id_ = 1;
	bool feed_open_ = false;
	std::vector<Retired> retired_;

	std::thread thread_;
	std::atomic<bool> quit_{ false };
	std::thread ready_thread_;
	HANDLE ready_event_ = nullptr;
	static constexpr uint32_t kReadyRing = 64;
	struct ReadyAt {
		uint64_t value;
		double at;
	};
	ReadyAt ready_at_[kReadyRing] = {};
	std::mutex ready_mutex_;
	std::atomic<bool> ready_alive_{ false };
	std::atomic<bool> init_done_{ false };
	std::atomic<bool> init_ok_{ false };
	std::atomic<bool> sink_failed_{ false };
	std::wstring init_error_;

	ID3D12Device *native_ = nullptr;
	std::wstring host_exe_;
	std::unique_ptr<WindowHostLink> host_;
	bool host_lost_ = false;
	bool host_pending_ = false;
	uint32_t kicks_left_ = 0;
	double next_kick_ = 0.0;
	static constexpr uint32_t kHostTries = 2;
	uint32_t host_failures_ = 0;
	static constexpr uint64_t kDoneMargin = 1u << 20;
	WindowHostAnswer host_answer_;
	void *dpi_before_ = nullptr;
	HWND window_ = nullptr;
	void *dcomp_device_ = nullptr;
	void *dcomp_target_ = nullptr;
	void *dcomp_visual_ = nullptr;
	HMODULE dcomp_module_ = nullptr;
	HMODULE dxgi_module_ = nullptr;
	ID3D12CommandQueue *queue_ = nullptr;
	IDXGISwapChain3 *swap_ = nullptr;
	HMODULE d3d11_module_ = nullptr;
	void *d3d11_wrapper_ = nullptr;
	void *d3d11_ = nullptr;
	void *ctx11_ = nullptr;
	static constexpr uint32_t kShared = 3;
	ID3D12Resource *shared12_[kShared] = {};
	void *shared11_[kShared] = {};
	uint64_t shared_free_[kShared] = {};
	uint32_t shared_at_ = 0;
	ID3D12Fence *ready12_ = nullptr;
	void *ready11_ = nullptr;
	uint64_t ready_value_ = 0;
	void *done11_ = nullptr;
	ID3D12Fence *done12_ = nullptr;
	uint64_t done_value_ = 0;
	HANDLE waitable_ = nullptr;
	static constexpr uint32_t kLists = 3;
	ID3D12CommandAllocator *alloc_[kLists] = {};
	uint64_t alloc_done_[kLists] = {};
	uint32_t alloc_at_ = 0;
	ID3D12GraphicsCommandList *list_ = nullptr;
	uint64_t read_value_ = 0;
	HANDLE fence_event_ = nullptr;
	HANDLE timer_ = nullptr;
	FrameInterpolatorD3D12 interp_;
	BlitPipelineD3D12 blit_;
	OverlayScheduler scheduler_;
	std::vector<Pending> waiting_;
	enum class Last : uint8_t { None, Real, Generated, Made } last_ = Last::None;
	uint64_t last_id_ = 0;
	uint32_t last_index_ = 0;
	uint64_t made_shown_ = 0;
	struct ShownEntry {
		UINT count;
		double content_ms;
		bool real;
		double planned_ms;
	};
	static constexpr uint32_t kOffsets = 120;
	double offsets_[kOffsets] = {};
	uint32_t offset_n_ = 0, offset_at_ = 0;
	double planned_display_ = 0.0;
	uint64_t late_ = 0;
	double paced_period_ = 0.0;
	static constexpr uint32_t kShownRing = 64;
	ShownEntry shown_ring_[kShownRing] = {};
	uint32_t shown_ring_at_ = 0;
	UINT stats_count_ = 0;
	std::vector<double> screen_all_, screen_real_, held_real_, ready_wait_;
	uint32_t ready_wait_long_ = 0;
	std::vector<double> t_waitable_, t_wake_late_, t_render_, t_present_;
	double monitor_hz_ = 0.0, monitor_hz_at_ = -1.0e9, good_period_ = 0.0;
	HMONITOR monitor_ = nullptr;
	struct GpuTrace {
		ID3D12Fence *ready_fence, *done_fence;
		uint64_t ready, done;
		double submitted;
		double planned;
		double display;
		uint64_t seq;
	};
	struct GpuDone {
		double planned, display, done;
		bool exact;
	};
	std::thread trace_thread_;
	std::mutex trace_mutex_;
	std::condition_variable trace_cv_;
	HANDLE trace_quit_event_ = nullptr;
	std::vector<GpuTrace> trace_pending_;
	std::vector<GpuDone> trace_done_;
	std::vector<double> trace_d3d12_, trace_d3d11_;
	bool trace_quit_ = false;
	uint64_t trace_seq_ = 0, trace_void_ = 0;
	uint64_t work_seen_ = 0, work_found_ = 0, work_untimed_ = 0;
	void trace_run();
	double planned_at_ = 0.0;
	FILE *pace_csv_ = nullptr;
	void read_frame_statistics();
	bool showing_made_ = false;
	bool shown_new_ = false;
	OverlayPace pace_;
	RECT shown_rect_{};
	bool shown_ = false;
	bool had_content_ = false;
	static constexpr uint32_t kWakes = 32;
	double wakes_[kWakes] = {};
	uint32_t wake_n_ = 0, wake_at_ = 0;
	double last_wake_ = 0.0;
	double next_display_ = 0.0;
	double second_start_ = 0.0;
	uint32_t second_reals_ = 0, second_generated_ = 0;
};

}
