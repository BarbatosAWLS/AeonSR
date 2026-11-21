#include "aeon_sr/framegen/frame_marker.hpp"

#include <algorithm>

namespace aeon_sr {

namespace {

DXGI_FORMAT typed_for(DXGI_FORMAT f) noexcept
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
		return f;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

void transition(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES before,
	D3D12_RESOURCE_STATES after)
{
	if (before == after)
		return;
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	cmd->ResourceBarrier(1, &b);
}

}

uint32_t FrameMarker::side_for(uint32_t w, uint32_t h) noexcept
{
	const uint32_t side = std::clamp(h / 12u, 16u, 256u);
	return std::min(side, std::min(w, h));
}

void FrameMarker::release() noexcept
{
	retire();
	retire();
	device_ = nullptr;
	format_ = DXGI_FORMAT_UNKNOWN;
	side_ = 0;
}

void FrameMarker::retire() noexcept
{
	if (old_tex_ != nullptr)
		old_tex_->Release();
	if (old_heap_ != nullptr)
		old_heap_->Release();
	old_tex_ = tex_;
	old_heap_ = rtv_heap_;
	tex_ = nullptr;
	rtv_heap_ = nullptr;
}

bool FrameMarker::ensure(ID3D12Device *device, DXGI_FORMAT format, uint32_t side)
{
	if (device == device_ && format == format_ && side == side_ && tex_ != nullptr)
		return true;
	if (device != device_)
		release();
	else
		retire();
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = side;
	d.Height = side;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = format;
	d.SampleDesc.Count = 1;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	D3D12_DESCRIPTOR_HEAP_DESC hd{};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hd.NumDescriptors = 1;
	if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
			IID_PPV_ARGS(&tex_))) ||
		FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)))) {
		if (tex_ != nullptr)
			tex_->Release();
		if (rtv_heap_ != nullptr)
			rtv_heap_->Release();
		tex_ = nullptr;
		rtv_heap_ = nullptr;
		device_ = nullptr;
		return false;
	}
	device->CreateRenderTargetView(tex_, nullptr, rtv_heap_->GetCPUDescriptorHandleForHeapStart());
	device_ = device;
	format_ = format;
	side_ = side;
	return true;
}

bool FrameMarker::stamp(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst,
	D3D12_RESOURCE_STATES dst_state)
{
	if (device == nullptr || cmd == nullptr || dst == nullptr)
		return false;
	const D3D12_RESOURCE_DESC dd = dst->GetDesc();
	const DXGI_FORMAT typed = typed_for(dd.Format);
	if (typed == DXGI_FORMAT_UNKNOWN || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dd.SampleDesc.Count != 1)
		return false;
	const uint32_t side = side_for(static_cast<uint32_t>(dd.Width), dd.Height);
	if (side == 0 || !ensure(device, typed, side))
		return false;
	static const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
	cmd->ClearRenderTargetView(rtv_heap_->GetCPUDescriptorHandleForHeapStart(), red, 0, nullptr);
	transition(cmd, tex_, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
	transition(cmd, dst, dst_state, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_TEXTURE_COPY_LOCATION to{}, from{};
	to.pResource = dst;
	to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	from.pResource = tex_;
	from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
	transition(cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST, dst_state);
	transition(cmd, tex_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	return true;
}

}
