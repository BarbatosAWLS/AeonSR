#pragma once

#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/interop/gpu12.hpp"

#include <d3d12.h>

#include <cstdint>

namespace aeon_sr {

class DepthOnGridD3D12 {
public:
	static constexpr D3D12_RESOURCE_STATES kState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	ID3D12Resource *read(ID3D12Device *device, ID3D12CommandQueue *queue, Gpu12Fence &fence,
		ID3D12GraphicsCommandList *cmd, BlitPipelineD3D12 &blit, ID3D12Resource *depth, float u, float v);
	void release() noexcept;

	ID3D12Resource *texture() const noexcept { return tex_; }
	uint32_t creations() const noexcept { return creations_; }

private:
	ID3D12Resource *tex_ = nullptr;
	DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
	uint32_t w_ = 0, h_ = 0;
	bool failed_ = false;
	uint32_t creations_ = 0;
};

}
