#include "aeon_sr/motion/guide_builder.hpp"

#include "aeon_sr/interop/native_d3d.hpp"

namespace aeon_sr {
namespace {

constexpr D3D12_RESOURCE_STATES kMaskState = GuideBuilder::kMaskState;

}

void GuideBuilder::Mask::release()
{
	if (t12 != nullptr) {
		t12->Release();
		t12 = nullptr;
	}
	w = h = 0;
}

void GuideBuilder::release()
{
	bias_.release();
}

reshade::api::resource GuideBuilder::bias_mask(const PassContext &ctx, const Settings &s, const FrameInputs &in)
{
	if (ctx.runtime == nullptr)
		return { 0 };
	if (!in.have_motion_confidence || in.engine.confidence == nullptr || !in.engine.ready())
		return { 0 };
	(void)s;
	return draw(ctx, bias_, in, Kind::BiasFromConfidence, 1.0f, false);
}

reshade::api::resource GuideBuilder::draw(const PassContext &ctx, Mask &mask, const FrameInputs &in,
	Kind kind, float strength, bool invert)
{
	if (ctx.pipelines == nullptr)
		return { 0 };

	ID3D12Device *const dev12 = in.engine.device;
	ID3D12GraphicsCommandList *const cmd12 = in.engine.cmd;
	ID3D12Resource *const src12 = in.engine.confidence;
	if (dev12 == nullptr || cmd12 == nullptr || src12 == nullptr)
		return { 0 };

	const D3D12_RESOURCE_DESC cd = in.engine.color != nullptr
		? in.engine.color->GetDesc() : D3D12_RESOURCE_DESC{};
	const uint32_t w = static_cast<uint32_t>(cd.Width);
	const uint32_t h = cd.Height;
	if (w == 0 || h == 0)
		return { 0 };

	if (mask.t12 != nullptr && (mask.w != w || mask.h != h)) {
		mask.t12->Release();
		mask.t12 = nullptr;
	}
	if (mask.t12 == nullptr) {
		if (!create_tex12(dev12, w, h, DXGI_FORMAT_R8_UNORM, false, kMaskState, &mask.t12))
			return { 0 };
		mask.w = w;
		mask.h = h;
	}
	BlitPipelineD3D12 &blit = ctx.pipelines->d3d12;
	if (!blit.ensure(dev12))
		return { 0 };

	(void)kind;
	(void)invert;
	const uint32_t mode = 5u;
	barrier12(cmd12, mask.t12, kMaskState, D3D12_RESOURCE_STATE_RENDER_TARGET);
	const bool ok = blit.draw_fullscreen(dev12, cmd12, src12,
		mask.t12, DXGI_FORMAT_R8_UNORM, w, h, strength, mode);
	barrier12(cmd12, mask.t12, D3D12_RESOURCE_STATE_RENDER_TARGET, kMaskState);
	return ok ? reshade::api::resource{ reinterpret_cast<uintptr_t>(mask.t12) } : reshade::api::resource{ 0 };
}

}
