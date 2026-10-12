#pragma once

#include "aeon_sr/motion/game_jitter.hpp"

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace aeon_sr {

class GameJitterProbeD3D12 {
public:
	static constexpr uint32_t kRing = 4u;
	static constexpr uint32_t kLatency = 3u;
	static constexpr uint32_t kMinPixels = 2000u;

	~GameJitterProbeD3D12() { release(); }

	bool ensure(ID3D12Device *device, uint32_t width, uint32_t height, std::wstring *error);
	bool record(ID3D12GraphicsCommandList *cmd, ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
		ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state, ID3D12Resource *motion,
		D3D12_RESOURCE_STATES motion_state, std::wstring *error);
	bool take(GameJitterFit *out) noexcept;
	uint64_t serial() const noexcept { return serial_; }
	void reset() noexcept;
	void release();

private:
	bool make_textures(std::wstring *error);

	ID3D12Device *device_ = nullptr;
	ID3D12RootSignature *root_ = nullptr;
	ID3D12PipelineState *accumulate_ = nullptr, *reduce_ = nullptr;
	ID3D12DescriptorHeap *heap_ = nullptr;
	uint32_t stride_ = 0;
	uint32_t ring_slot_ = 0;
	ID3D12Resource *prev_depth_[2]{}, *prev_luma_[2]{};
	ID3D12Resource *partials_ = nullptr, *result_ = nullptr, *readback_ = nullptr;
	uint8_t *mapped_ = nullptr;
	uint32_t width_ = 0, height_ = 0, groups_x_ = 0, groups_y_ = 0;
	uint64_t serial_ = 0;
	uint64_t first_valid_ = 0;
	uint64_t taken_ = 0;
};

}
