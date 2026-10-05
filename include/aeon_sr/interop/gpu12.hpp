#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace aeon_sr {

bool fence_wait(ID3D12Fence *fence, UINT64 target, DWORD timeout_ms) noexcept;

struct Gpu12Fence {
	ID3D12Fence *fence = nullptr;
	UINT64 value = 0;

	void release() noexcept;

	bool signal_and_wait(ID3D12Device *device, ID3D12CommandQueue *queue, DWORD timeout_ms = INFINITE) noexcept;
};

struct Gpu12Timer {
	static constexpr uint32_t kSlots = 4;

	ID3D12QueryHeap *heap = nullptr;
	ID3D12Resource *readback = nullptr;
	UINT64 frequency = 0;
	uint32_t slot = 0;
	uint32_t written = 0;
	bool open = false;

	static constexpr uint32_t kWindow = 15;
	double samples[kWindow] = {};
	uint32_t sample_count = 0;
	uint32_t sample_next = 0;
	double last_ms = 0.0;

	bool ensure(ID3D12Device *device, ID3D12CommandQueue *queue) noexcept;
	void begin(ID3D12GraphicsCommandList *cmd) noexcept;
	void end(ID3D12GraphicsCommandList *cmd) noexcept;
	void poll() noexcept;
	double median_ms() const noexcept;
	void release() noexcept;
};

struct StageTimings {
	enum class Stage : uint32_t {
		Total,
		Edges,
		Observe,
		Interface,
		Depth,
		Flow,
		Publish,
		Mask,
		Upscaler,
		Neural,
		Restore,
		Count
	};
	static const char *label(Stage s) noexcept;

	bool enabled = false;

	void begin(Stage s, ID3D12Device *device, ID3D12CommandQueue *queue, ID3D12GraphicsCommandList *cmd) noexcept;
	void end(Stage s, ID3D12GraphicsCommandList *cmd) noexcept;
	void start_frame() noexcept { suspended_ = false; }
	void cancel(Stage s) noexcept;
	void abandon_frame() noexcept;
	void note_remote_frame() noexcept { ++remote_frames_; }

	void add_sample(Stage s, double ms) noexcept;
	bool take_report(uint64_t now_ms, uint64_t period_ms, std::wstring *line);
	void release() noexcept;

private:
	static constexpr uint32_t kCount = static_cast<uint32_t>(Stage::Count);
	Gpu12Timer timers_[kCount];
	double sum_[kCount] = {};
	uint32_t n_[kCount] = {};
	uint64_t period_start_ms_ = 0;
	uint32_t remote_frames_ = 0;
	bool suspended_ = false;
};

struct Gpu12InitList {
	ID3D12CommandAllocator *alloc = nullptr;
	ID3D12GraphicsCommandList *list = nullptr;

	void release() noexcept;

	ID3D12GraphicsCommandList *begin(ID3D12Device *device) noexcept;

	void end(ID3D12Device *device, ID3D12CommandQueue *queue, Gpu12Fence &fence, bool execute) noexcept;
};

}
