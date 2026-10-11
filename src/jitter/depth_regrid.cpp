#include "aeon_sr/jitter/depth_regrid.hpp"

#include "aeon_sr/core/diagnostics.hpp"

#include <d3dcompiler.h>

#include <cstring>
#include <string>

namespace aeon_sr {

const char kDepthRegridHlsl[] = R"(
Texture2D<float> depth_in : register(t0);
RWTexture2D<float> depth_out : register(u0);
cbuffer C : register(b0)
{
	float2 offset;
	int2 size;
};

float depth_at(int2 p)
{
	return any(p < 0) || any(p >= size) ? -1.0 : depth_in.Load(int3(p, 0));
}

bool surface(float z)
{
	return z > 0.0 && z < 1.0;
}

float side_step(float here, float next, float beyond, out bool ok)
{
	const float a = next - here;
	const float tol = abs(a) * 0.02 + abs(here) * 2e-6;
	ok = surface(next) && surface(beyond) && abs((beyond - next) - a) <= tol;
	return a;
}

float slope(int2 p, int2 axis, float here, float toward)
{
	bool fwd_ok, back_ok;
	const float fwd = side_step(here, depth_at(p + axis), depth_at(p + 2 * axis), fwd_ok);
	const float back = -side_step(here, depth_at(p - axis), depth_at(p - 2 * axis), back_ok);
	if (toward >= 0.0 ? fwd_ok : back_ok)
		return toward >= 0.0 ? fwd : back;
	return fwd_ok ? fwd : back_ok ? back : 0.0;
}

[numthreads(8, 8, 1)]
void CSRegrid(uint3 id : SV_DispatchThreadID)
{
	const int2 p = int2(id.xy);
	if (any(p >= size))
		return;
	const float z = depth_in.Load(int3(p, 0));
	if (!surface(z)) {
		depth_out[p] = z;
		return;
	}
	const float moved = z + slope(p, int2(1, 0), z, offset.x) * offset.x + slope(p, int2(0, 1), z, offset.y) * offset.y;
	depth_out[p] = surface(moved) ? moved : z;
}
)";

namespace {

template <typename T>
void rel(T *&p) noexcept
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

}

bool depth_regrid_reads_depth(DXGI_FORMAT f) noexcept
{
	switch (f) {
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
	case DXGI_FORMAT_R16_UNORM:
		return true;
	default:
		return false;
	}
}

void DepthRegridD3D11::release() noexcept
{
	rel(srv_);
	rel(uav_);
	rel(tex_);
	rel(cb_);
	rel(cs_);
	device_ = nullptr;
	w_ = h_ = 0;
	failed_ = false;
	failed_w_ = failed_h_ = 0;
}

bool DepthRegridD3D11::ensure(ID3D11Device *device, uint32_t w, uint32_t h)
{
	if (device != device_) {
		release();
		device_ = device;
	}
	if (failed_)
		return false;
	if (cs_ == nullptr) {
		ID3DBlob *blob = nullptr, *err = nullptr;
		if (FAILED(D3DCompile(kDepthRegridHlsl, std::strlen(kDepthRegridHlsl), "aeon_depth_regrid", nullptr, nullptr,
				"CSRegrid", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err)) ||
			FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &cs_))) {
			std::wstring what = L"depth regrid: the compute shader did not build";
			if (err != nullptr) {
				const char *t = static_cast<const char *>(err->GetBufferPointer());
				what += L": " + std::wstring(t, t + std::strlen(t));
			}
			diag_error("jitter", what);
			rel(blob);
			rel(err);
			failed_ = true;
			return false;
		}
		rel(blob);
		rel(err);
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = 16;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &cb_))) {
			failed_ = true;
			return false;
		}
	}
	if (tex_ != nullptr && w_ == w && h_ == h)
		return true;
	if (failed_w_ == w && failed_h_ == h)
		return false;
	rel(srv_);
	rel(uav_);
	rel(tex_);
	D3D11_TEXTURE2D_DESC td{};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R32_FLOAT;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	if (FAILED(device->CreateTexture2D(&td, nullptr, &tex_)) ||
		FAILED(device->CreateShaderResourceView(tex_, nullptr, &srv_)) ||
		FAILED(device->CreateUnorderedAccessView(tex_, nullptr, &uav_))) {
		rel(srv_);
		rel(uav_);
		rel(tex_);
		w_ = h_ = 0;
		failed_w_ = w;
		failed_h_ = h;
		return false;
	}
	w_ = w;
	h_ = h;
	return true;
}

ID3D11ShaderResourceView *DepthRegridD3D11::regrid(ID3D11DeviceContext *ctx, ID3D11ShaderResourceView *depth, float dx,
	float dy)
{
	if (ctx == nullptr || depth == nullptr)
		return nullptr;
	D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
	depth->GetDesc(&vd);
	if (vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || vd.Texture2D.MostDetailedMip != 0 ||
		!depth_regrid_reads_depth(vd.Format))
		return nullptr;
	ID3D11Resource *res = nullptr;
	depth->GetResource(&res);
	ID3D11Texture2D *tex = nullptr;
	const bool is_tex = res != nullptr && SUCCEEDED(res->QueryInterface(IID_PPV_ARGS(&tex)));
	rel(res);
	if (!is_tex)
		return nullptr;
	D3D11_TEXTURE2D_DESC td{};
	tex->GetDesc(&td);
	rel(tex);
	if (td.SampleDesc.Count != 1)
		return nullptr;
	ID3D11Device *device = nullptr;
	ctx->GetDevice(&device);
	const bool ready = device != nullptr && ensure(device, td.Width, td.Height);
	rel(device);
	if (!ready)
		return nullptr;

	D3D11_MAPPED_SUBRESOURCE m{};
	if (FAILED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
		return nullptr;
	const float off[2] = { dx, dy };
	const int32_t size[2] = { static_cast<int32_t>(td.Width), static_cast<int32_t>(td.Height) };
	std::memcpy(m.pData, off, sizeof(off));
	std::memcpy(static_cast<uint8_t *>(m.pData) + 8, size, sizeof(size));
	ctx->Unmap(cb_, 0);

	ID3D11ComputeShader *old_cs = nullptr;
	ID3D11ClassInstance *old_ci[256]{};
	UINT old_ci_count = 256;
	ctx->CSGetShader(&old_cs, old_ci, &old_ci_count);
	ID3D11ShaderResourceView *old_srv = nullptr;
	ctx->CSGetShaderResources(0, 1, &old_srv);
	ID3D11UnorderedAccessView *old_uav = nullptr;
	ctx->CSGetUnorderedAccessViews(0, 1, &old_uav);
	ID3D11Buffer *old_cb = nullptr;
	ctx->CSGetConstantBuffers(0, 1, &old_cb);

	ctx->CSSetShader(cs_, nullptr, 0);
	ctx->CSSetShaderResources(0, 1, &depth);
	ctx->CSSetUnorderedAccessViews(0, 1, &uav_, nullptr);
	ctx->CSSetConstantBuffers(0, 1, &cb_);
	ctx->Dispatch((td.Width + 7u) / 8u, (td.Height + 7u) / 8u, 1u);

	ID3D11ShaderResourceView *null_srv = nullptr;
	ctx->CSSetShaderResources(0, 1, old_srv != nullptr ? &old_srv : &null_srv);
	const UINT keep = ~0u;
	ctx->CSSetUnorderedAccessViews(0, 1, &old_uav, &keep);
	ctx->CSSetConstantBuffers(0, 1, &old_cb);
	ctx->CSSetShader(old_cs, old_ci, old_ci_count);
	rel(old_cs);
	for (UINT i = 0; i < old_ci_count; ++i)
		rel(old_ci[i]);
	rel(old_srv);
	rel(old_uav);
	rel(old_cb);
	return srv_;
}

}
