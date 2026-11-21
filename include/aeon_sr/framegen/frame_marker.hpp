#pragma once

#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>

namespace aeon_sr {

class FrameMarker {
public:
	FrameMarker() = default;
	FrameMarker(const FrameMarker &) = delete;
	FrameMarker &operator=(const FrameMarker &) = delete;
	~FrameMarker() { release(); }

	static uint32_t side_for(uint32_t w, uint32_t h) noexcept;

	bool stamp(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst,
		D3D12_RESOURCE_STATES dst_state);

	void release() noexcept;

private:
	bool ensure(ID3D12Device *device, DXGI_FORMAT format, uint32_t side);
	void retire() noexcept;

	ID3D12Device *device_ = nullptr;
	ID3D12Resource *tex_ = nullptr;
	ID3D12DescriptorHeap *rtv_heap_ = nullptr;
	ID3D12Resource *old_tex_ = nullptr;
	ID3D12DescriptorHeap *old_heap_ = nullptr;
	DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
	uint32_t side_ = 0;
};

}
