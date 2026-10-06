#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <cstdint>

namespace aeon_sr {

struct BlitPipelineD3D11 {
	ID3D11Device *device = nullptr;
	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps_blit = nullptr;
	ID3D11PixelShader *ps_debug = nullptr;
	ID3D11SamplerState *sampler = nullptr;
	ID3D11RasterizerState *rs = nullptr;
	ID3D11DepthStencilState *dss = nullptr;
	ID3D11BlendState *bs = nullptr;
	ID3D11Buffer *cb = nullptr;
	bool ready = false;
	int last_stage = 0;

	bool ensure(ID3D11Device *dev);
	void release();

	bool blit(ID3D11DeviceContext *ctx, ID3D11Resource *src, ID3D11Resource *dst, float sharpness = 0.0f,
		float jitter_u = 0.0f, float jitter_v = 0.0f);
	bool composite(ID3D11DeviceContext *ctx, ID3D11Resource *effect,
		ID3D11Resource *dst, float sharpness = 0.0f,
		float jitter_u = 0.0f, float jitter_v = 0.0f);

	bool mask_pass(ID3D11DeviceContext *ctx, ID3D11Resource *src,
		ID3D11Resource *dst, float strength, float mode);

	bool bias_mask(ID3D11DeviceContext *ctx, ID3D11Resource *confidence,
		ID3D11Resource *dst, float strength);
};

}
