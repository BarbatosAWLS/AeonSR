#pragma once

#include <d3d12.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace aeon_sr {

class FrameInterpolatorD3D12 {
public:
	static constexpr D3D12_RESOURCE_STATES kOutputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	struct Inputs {
		ID3D12Resource *previous = nullptr;
		D3D12_RESOURCE_STATES previous_state = D3D12_RESOURCE_STATE_COMMON;
		ID3D12Resource *current = nullptr;
		D3D12_RESOURCE_STATES current_state = D3D12_RESOURCE_STATE_COMMON;
		ID3D12Resource *motion = nullptr;
		D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_COMMON;
		ID3D12Resource *global_flow = nullptr;
		D3D12_RESOURCE_STATES global_state = D3D12_RESOURCE_STATE_COMMON;
		ID3D12Resource *depth = nullptr;
		D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_COMMON;
		float t = 0.5f;
	};

	~FrameInterpolatorD3D12();

	bool ensure(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT color_format, bool *building,
		std::wstring *error);
	bool wait_for_kernels();
	bool failed_permanently() const noexcept { return state_.load(std::memory_order_acquire) == kFailed; }

	bool record(ID3D12GraphicsCommandList *cmd, const Inputs &in, std::wstring *error);

	ID3D12Resource *output() const noexcept { return output_; }
	uint32_t width() const noexcept { return w_; }
	uint32_t height() const noexcept { return h_; }
	DXGI_FORMAT family() const noexcept { return family_; }
	bool copies_into(DXGI_FORMAT f) const noexcept;

	void release() noexcept;

private:
	enum : uint32_t { kIdle = 0, kBuilding = 1, kReady = 2, kFailed = 3 };
	static constexpr uint32_t kPasses = 6;
	static constexpr uint32_t kSrvCount = 12;
	static constexpr uint32_t kUavCount = 7;
	static constexpr uint32_t kSlotDescriptors = kSrvCount + kUavCount;
	static constexpr uint32_t kRingSlots = 16;

	bool ensure_textures(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT color_format, std::wstring *error);
	void release_textures() noexcept;
	void build_kernels(ID3D12Device *device);

	std::atomic<uint32_t> state_{ kIdle };
	std::thread builder_;
	std::mutex build_mutex_;
	std::wstring build_error_;
	ID3D12Device *device_ = nullptr;
	ID3D12RootSignature *root_ = nullptr;
	ID3D12PipelineState *pso_[kPasses] = {};
	ID3D12DescriptorHeap *heap_ = nullptr;
	uint32_t stride_ = 0;
	uint32_t ring_ = 0;

	uint32_t w_ = 0, h_ = 0;
	DXGI_FORMAT color_format_ = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT family_ = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT input_family_ = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT uav_format_ = DXGI_FORMAT_UNKNOWN;
	uint32_t stat_w_ = 0, stat_h_ = 0, tile_w_ = 0, tile_h_ = 0;
	ID3D12Resource *output_ = nullptr;
	ID3D12Resource *tile_stats_ = nullptr;
	ID3D12Resource *search_errors_ = nullptr;
	ID3D12Resource *summary_ = nullptr;
	ID3D12Resource *refined_ = nullptr;
	ID3D12Resource *layers_ = nullptr;
	ID3D12Resource *layer_depth_ = nullptr;
	bool textures_failed_ = false;
};

DXGI_FORMAT interpolator_read_format(DXGI_FORMAT f) noexcept;
DXGI_FORMAT interpolator_write_format(DXGI_FORMAT f) noexcept;

}
