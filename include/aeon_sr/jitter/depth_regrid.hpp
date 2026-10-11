#pragma once

#include <d3d11.h>

#include <cstdint>
#include <mutex>

namespace aeon_sr {

bool depth_regrid_reads_depth(DXGI_FORMAT view_format) noexcept;

extern const char kDepthRegridHlsl[];

class DepthRegridD3D11 {
public:
	~DepthRegridD3D11() { release(); }

	ID3D11ShaderResourceView *regrid(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *depth, float dx, float dy);

	ID3D11ShaderResourceView *view() const noexcept { return srv_; }
	ID3D11Texture2D *texture() const noexcept { return tex_; }
	ID3D11Device *device() const noexcept { return device_; }
	void release() noexcept;

private:
	bool ensure(ID3D11Device *device, uint32_t w, uint32_t h);

	ID3D11Device *device_ = nullptr;
	ID3D11ComputeShader *cs_ = nullptr;
	ID3D11Buffer *cb_ = nullptr;
	ID3D11Texture2D *tex_ = nullptr;
	ID3D11ShaderResourceView *srv_ = nullptr;
	ID3D11UnorderedAccessView *uav_ = nullptr;
	uint32_t w_ = 0, h_ = 0;
	bool failed_ = false;
	uint32_t failed_w_ = 0, failed_h_ = 0;
};

}
