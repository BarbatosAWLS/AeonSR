#include "aeon_sr/depth/d3d9_depth.hpp"

#include <d3dcompiler.h>

#include <cstring>

namespace aeon_sr {
namespace {

template <typename T>
void safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

constexpr D3DFORMAT kFormatIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

const char kHlsl[] = R"(
sampler2D depthSamp : register(s0);
struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(VSIn i) { VSOut o; o.pos = float4(i.pos, 1.0); o.uv = i.uv; return o; }
float4 PSDepth(float2 uv : TEXCOORD0) : COLOR { return tex2D(depthSamp, uv).r; }
)";

struct QuadVertex {
	float x, y, z;
	float u, v;
};

ID3DBlob *compile(const char *entry, const char *target)
{
	ID3DBlob *code = nullptr, *errors = nullptr;
	D3DCompile(kHlsl, std::strlen(kHlsl), "aeon_d3d9_depth", nullptr, nullptr, entry, target, 0, 0, &code, &errors);
	safe_release(errors);
	return code;
}

}

bool D3D9DepthToFloat::wants(IDirect3DBaseTexture9 *depth) noexcept
{
	IDirect3DTexture9 *tex = nullptr;
	if (depth == nullptr || FAILED(depth->QueryInterface(IID_PPV_ARGS(&tex))) || tex == nullptr)
		return false;
	D3DSURFACE_DESC d{};
	const bool intz = SUCCEEDED(tex->GetLevelDesc(0, &d)) && d.Format == kFormatIntz;
	tex->Release();
	return intz;
}

void D3D9DepthToFloat::release() noexcept
{
	safe_release(target_surface_);
	safe_release(target_);
	safe_release(decl_);
	safe_release(ps_);
	safe_release(vs_);
	device_ = nullptr;
	width_ = height_ = 0;
}

bool D3D9DepthToFloat::ensure_shaders(IDirect3DDevice9 *device, std::wstring &why)
{
	if (device_ != device)
		release();
	device_ = device;
	if (vs_ != nullptr && ps_ != nullptr && decl_ != nullptr)
		return true;
	D3DCAPS9 caps{};
	if (FAILED(device->GetDeviceCaps(&caps)) || caps.PixelShaderVersion < D3DPS_VERSION(3, 0) ||
		caps.VertexShaderVersion < D3DVS_VERSION(3, 0)) {
		why = L"this graphics card does not run Direct3D 9 shader model 3, which the depth conversion needs";
		return false;
	}
	ID3DBlob *vs = compile("VSMain", "vs_3_0");
	ID3DBlob *ps = compile("PSDepth", "ps_3_0");
	const D3DVERTEXELEMENT9 elements[] = {
		{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		D3DDECL_END(),
	};
	const bool ok = vs != nullptr && ps != nullptr &&
		SUCCEEDED(device->CreateVertexShader(static_cast<const DWORD *>(vs->GetBufferPointer()), &vs_)) &&
		SUCCEEDED(device->CreatePixelShader(static_cast<const DWORD *>(ps->GetBufferPointer()), &ps_)) &&
		SUCCEEDED(device->CreateVertexDeclaration(elements, &decl_));
	safe_release(vs);
	safe_release(ps);
	if (!ok) {
		safe_release(decl_);
		safe_release(ps_);
		safe_release(vs_);
		why = L"the small shader that converts the game's depth buffer would not build on this driver";
	}
	return ok;
}

IDirect3DTexture9 *D3D9DepthToFloat::convert(IDirect3DDevice9 *device, IDirect3DTexture9 *depth, std::wstring &why)
{
	D3DSURFACE_DESC dd{};
	if (device == nullptr || depth == nullptr || FAILED(depth->GetLevelDesc(0, &dd))) {
		why = L"the game's depth buffer could not be described";
		return nullptr;
	}
	if (!ensure_shaders(device, why))
		return nullptr;
	if (target_ == nullptr || width_ != dd.Width || height_ != dd.Height) {
		safe_release(target_surface_);
		safe_release(target_);
		if (FAILED(device->CreateTexture(dd.Width, dd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F,
				D3DPOOL_DEFAULT, &target_, nullptr)) ||
			target_ == nullptr || FAILED(target_->GetSurfaceLevel(0, &target_surface_))) {
			safe_release(target_);
			why = L"the device refused a float render target the size of the game's depth buffer";
			return nullptr;
		}
		width_ = dd.Width;
		height_ = dd.Height;
	}

	IDirect3DStateBlock9 *saved = nullptr;
	if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved)) || saved == nullptr) {
		why = L"the Direct3D 9 device would not save its render state, so nothing is drawn on it";
		return nullptr;
	}
	D3DCAPS9 caps{};
	device->GetDeviceCaps(&caps);
	const DWORD targets = caps.NumSimultaneousRTs > 0 && caps.NumSimultaneousRTs <= 4 ? caps.NumSimultaneousRTs : 1;
	IDirect3DSurface9 *rts[4]{};
	for (DWORD i = 0; i < targets; ++i)
		device->GetRenderTarget(i, &rts[i]);
	IDirect3DSurface9 *ds = nullptr;
	device->GetDepthStencilSurface(&ds);
	D3DVIEWPORT9 vp_saved{};
	device->GetViewport(&vp_saved);
	const bool opened_scene = SUCCEEDED(device->BeginScene());

	bool ok = SUCCEEDED(device->SetRenderTarget(0, target_surface_));
	for (DWORD i = 1; i < targets; ++i)
		device->SetRenderTarget(i, nullptr);
	device->SetDepthStencilSurface(nullptr);
	D3DVIEWPORT9 vp{};
	vp.Width = width_;
	vp.Height = height_;
	vp.MaxZ = 1.0f;
	device->SetViewport(&vp);
	device->SetRenderState(D3DRS_ZENABLE, FALSE);
	device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
	device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
	device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	device->SetRenderState(D3DRS_FOGENABLE, FALSE);
	device->SetRenderState(D3DRS_CLIPPING, FALSE);
	device->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
	device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
	device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	device->SetTexture(0, depth);
	device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
	ok = ok && SUCCEEDED(device->SetVertexDeclaration(decl_)) && SUCCEEDED(device->SetVertexShader(vs_)) &&
		SUCCEEDED(device->SetPixelShader(ps_));
	const float ox = -1.0f / static_cast<float>(width_);
	const float oy = 1.0f / static_cast<float>(height_);
	const QuadVertex quad[4] = {
		{ -1.0f + ox, 1.0f + oy, 0.0f, 0.0f, 0.0f },
		{ 1.0f + ox, 1.0f + oy, 0.0f, 1.0f, 0.0f },
		{ -1.0f + ox, -1.0f + oy, 0.0f, 0.0f, 1.0f },
		{ 1.0f + ox, -1.0f + oy, 0.0f, 1.0f, 1.0f },
	};
	ok = ok && SUCCEEDED(device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex)));
	if (opened_scene)
		device->EndScene();

	saved->Apply();
	saved->Release();
	for (DWORD i = 0; i < targets; ++i) {
		if (i != 0 || rts[0] != nullptr)
			device->SetRenderTarget(i, rts[i]);
		safe_release(rts[i]);
	}
	device->SetDepthStencilSurface(ds);
	safe_release(ds);
	device->SetViewport(&vp_saved);
	if (!ok) {
		why = L"the Direct3D 9 device turned down the draw that converts the game's depth buffer";
		return nullptr;
	}
	return target_;
}

}
