#include "aeon_sr/motion/flow_probe.hpp"
#include "aeon_sr/core/half_float.hpp"

#include <cmath>

namespace aeon_sr {

bool FlowProbe11::ensure(ID3D11Device *dev)
{
	if (device == dev && staging[0] != nullptr && staging[1] != nullptr)
		return true;
	release();
	device = dev;
	if (device == nullptr)
		return false;

	D3D11_TEXTURE2D_DESC d{};
	d.Width = 1;
	d.Height = 1;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = DXGI_FORMAT_R16G16_FLOAT;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_STAGING;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	for (int i = 0; i < 2; ++i) {
		if (FAILED(device->CreateTexture2D(&d, nullptr, &staging[i]))) {
			release();
			return false;
		}
	}
	return true;
}

void FlowProbe11::release()
{
	for (ID3D11Texture2D *&t : staging) {
		if (t != nullptr) { t->Release(); t = nullptr; }
	}
	device = nullptr;
	have_prev = false;
	idx = 0;
}

bool FlowProbe11::sample(ID3D11DeviceContext *ctx, ID3D11Resource *flow,
	float screen_w, float screen_h, float &out_motion_px)
{
	if (ctx == nullptr || flow == nullptr || !ensure(device))
		return false;

	const int cur = idx;
	const int prev = 1 - idx;
	ctx->CopyResource(staging[cur], flow);
	idx = prev;

	bool ok = false;
	if (have_prev) {
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(staging[prev], 0, D3D11_MAP_READ, 0, &m)) && m.pData != nullptr) {
			const uint16_t *const h = static_cast<const uint16_t *>(m.pData);
			const float u = half_to_float(h[0]);
			const float v = half_to_float(h[1]);
			ctx->Unmap(staging[prev], 0);
			const float dx = u * screen_w;
			const float dy = v * screen_h;
			out_motion_px = std::sqrt(dx * dx + dy * dy);
			ok = true;
		}
	}
	have_prev = true;
	return ok;
}

}
