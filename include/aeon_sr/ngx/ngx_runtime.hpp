#pragma once

#include "aeon_sr/interop/blit_d3d11.hpp"
#include "aeon_sr/ngx/ngx_session.hpp"

#include <d3d11.h>

#include <cstdint>

namespace aeon_sr {

struct NgxRuntime : NgxSession {
	NgxRuntime() noexcept : NgxSession(L"") {}

	ID3D11Device *device = nullptr;

	ID3D11Texture2D *output_tex = nullptr;
	ID3D11Texture2D *color_copy = nullptr;
	ID3D11Texture2D *depth_scratch = nullptr;

	ID3D11Texture2D *bias_scratch = nullptr;
	ID3D11Texture2D *color_full = nullptr;

	BlitPipelineD3D11 blit_;
	bool jitter_failed = false;
	std::wstring jitter_note;

	bool init_device(ID3D11Device *dev, uint32_t w, uint32_t h);
	void shutdown_device();
	bool ensure_feature(ID3D11DeviceContext *ctx, uint32_t w, uint32_t h, DXGI_FORMAT color_format);
	void destroy_feature();
	void release_scratch();

	bool run(ID3D11DeviceContext *ctx,
		ID3D11Resource *color,
		ID3D11Resource *motion_vectors,
		ID3D11Resource *depth,
		ID3D11Resource *bias_mask);

private:
	bool blit_texture(ID3D11DeviceContext *ctx, ID3D11Resource *src, ID3D11Resource *dst, float sharpness = 0.0f,
		float jitter_u = 0.0f, float jitter_v = 0.0f);
};

}
