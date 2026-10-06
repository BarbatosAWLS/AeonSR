#pragma once

#include "aeon_sr/depth/depth_convention.hpp"

#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>
#include <string>

namespace aeon_sr {

class DepthNormalizeD3D12 {
public:
	~DepthNormalizeD3D12();

	bool ensure(ID3D12Device *device, uint32_t width, uint32_t height, std::wstring *error);

	bool record(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src,
		D3D12_RESOURCE_STATES src_state, const DepthConvention &how, std::wstring *error,
		const float *valid_uv = nullptr);

	void release();
	void release_texture();

	bool ready() const noexcept { return device_ != nullptr && out_ != nullptr; }
	ID3D12Resource *depth() const noexcept { return out_; }
	uint32_t width() const noexcept { return width_; }
	uint32_t height() const noexcept { return height_; }

	static constexpr D3D12_RESOURCE_STATES kPublished =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

private:
	bool make_output(std::wstring *error);

	struct Constants {
		uint32_t dst_w, dst_h;
		float inv_dst_x, inv_dst_y;
		uint32_t upside_down, mirrored, reversed, logarithmic;
		float multiplier, x_scale, y_scale, x_offset;
		float y_offset, pad0, pad1, pad2;
		float valid[4];
	};

	ID3D12Device *device_ = nullptr;
	ID3D12RootSignature *root_ = nullptr;
	ID3D12PipelineState *pso_ = nullptr;
	ID3D12DescriptorHeap *heap_ = nullptr;
	uint32_t stride_ = 0;
	uint32_t ring_ = 0;
	ID3D12Resource *out_ = nullptr;
	D3D12_RESOURCE_STATES out_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	uint32_t width_ = 0, height_ = 0;
};

}
