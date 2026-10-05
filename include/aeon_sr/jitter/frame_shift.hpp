#pragma once

#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/interop/gpu12.hpp"

#include <d3d12.h>

#include <cstdint>

namespace aeon_sr {

struct FrameValidRect {
	float uv[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	uint32_t left = 0, right = 0, top = 0, bottom = 0;
	bool any = false;
};

enum class ViewportClip {
	Centres,
	WholePixels,
};

FrameValidRect frame_valid_rect(float shift_x, float shift_y, uint32_t w, uint32_t h,
	ViewportClip clip = ViewportClip::Centres, uint32_t scene_w = 0, uint32_t scene_h = 0) noexcept;

class FrameShiftD3D12 {
public:
	bool apply(ID3D12Device *device, ID3D12CommandQueue *queue, Gpu12Fence &fence, ID3D12GraphicsCommandList *cmd,
		BlitPipelineD3D12 &blit, ID3D12Resource *color, D3D12_RESOURCE_STATES color_state, float u, float v,
		const float *valid_uv = nullptr, bool catmull_rom = false);
	void release() noexcept;

private:
	static constexpr D3D12_RESOURCE_STATES kState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	ID3D12Resource *scratch_ = nullptr;
	DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
	uint32_t w_ = 0, h_ = 0;
	bool failed_ = false;
};

}
