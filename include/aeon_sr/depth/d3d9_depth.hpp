#pragma once

#include <d3d9.h>

#include <cstdint>
#include <string>

namespace aeon_sr {

class D3D9DepthToFloat {
public:
	D3D9DepthToFloat() = default;
	~D3D9DepthToFloat() { release(); }
	D3D9DepthToFloat(const D3D9DepthToFloat &) = delete;
	D3D9DepthToFloat &operator=(const D3D9DepthToFloat &) = delete;

	static bool wants(IDirect3DBaseTexture9 *depth) noexcept;

	IDirect3DTexture9 *convert(IDirect3DDevice9 *device, IDirect3DTexture9 *depth, std::wstring &why);

	void release() noexcept;

private:
	bool ensure_shaders(IDirect3DDevice9 *device, std::wstring &why);

	IDirect3DDevice9 *device_ = nullptr;
	IDirect3DTexture9 *target_ = nullptr;
	IDirect3DSurface9 *target_surface_ = nullptr;
	uint32_t width_ = 0, height_ = 0;
	IDirect3DVertexShader9 *vs_ = nullptr;
	IDirect3DPixelShader9 *ps_ = nullptr;
	IDirect3DVertexDeclaration9 *decl_ = nullptr;
};

}
