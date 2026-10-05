#include "aeon_sr/depth/depth_on_grid.hpp"

#include "aeon_sr/core/dxgi_format_util.hpp"

namespace aeon_sr {

ID3D12Resource *DepthOnGridD3D12::read(ID3D12Device *device, ID3D12CommandQueue *queue, Gpu12Fence &fence,
	ID3D12GraphicsCommandList *cmd, BlitPipelineD3D12 &blit, ID3D12Resource *depth, float u, float v)
{
	if (device == nullptr || cmd == nullptr || depth == nullptr)
		return nullptr;
	const D3D12_RESOURCE_DESC dd = depth->GetDesc();
	const uint32_t w = static_cast<uint32_t>(dd.Width);
	const uint32_t h = dd.Height;
	const DXGI_FORMAT want = view_format_for(dd.Format);
	const bool same = format_ == want && w_ == w && h_ == h;
	if (tex_ != nullptr && !same) {
		fence.signal_and_wait(device, queue);
		tex_->Release();
		tex_ = nullptr;
	}
	if (tex_ == nullptr) {
		if (same && failed_)
			return nullptr;
		format_ = want;
		w_ = w;
		h_ = h;
		++creations_;
		failed_ = !create_tex12(device, w, h, want, false, kState, &tex_);
		if (failed_)
			return nullptr;
	}
	NeuralDrawConstants c;
	c.jitter_u = u;
	c.jitter_v = v;
	barrier12(cmd, tex_, kState, D3D12_RESOURCE_STATE_RENDER_TARGET);
	const bool ok = blit.draw_neural(device, cmd, NeuralPass::Guide, depth, nullptr, nullptr, nullptr,
		tex_, want, w, h, c);
	barrier12(cmd, tex_, D3D12_RESOURCE_STATE_RENDER_TARGET, kState);
	return ok ? tex_ : nullptr;
}

void DepthOnGridD3D12::release() noexcept
{
	if (tex_ != nullptr) {
		tex_->Release();
		tex_ = nullptr;
	}
	format_ = DXGI_FORMAT_UNKNOWN;
	w_ = h_ = 0;
	failed_ = false;
}

}
