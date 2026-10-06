#include "aeon_sr/interop/blit_d3d11.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace aeon_sr {
namespace {

struct TexInfo {
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	uint32_t width = 0;
	uint32_t height = 0;
};

TexInfo tex_info(ID3D11Resource *res)
{
	TexInfo info;
	ID3D11Texture2D *t = nullptr;
	if (res != nullptr &&
		SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t))) && t) {
		D3D11_TEXTURE2D_DESC d{};
		t->GetDesc(&d);
		info.format = d.Format;
		info.width = d.Width;
		info.height = d.Height;
		t->Release();
	}
	return info;
}

bool compile_entry(const char *entry, const char *target, ID3DBlob **out)
{
	ID3DBlob *err = nullptr;
	const HRESULT hr = D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "aeon_blit", nullptr, nullptr, entry, target,
		0, 0, out, &err);
	if (err != nullptr)
		err->Release();
	if (FAILED(hr)) {
		wchar_t line[96]{};
		_snwprintf_s(line, _TRUNCATE, L"blit: %hs (%hs) compile failed hr=0x%08X", entry, target, static_cast<unsigned>(hr));
		diag_error("shader", line);
		return false;
	}
	return true;
}

bool create_tex2d(ID3D11Device *device, uint32_t w, uint32_t h, DXGI_FORMAT format, bool need_uav,
	ID3D11Texture2D **out)
{
	if (device == nullptr || out == nullptr)
		return false;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	if (need_uav)
		desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

	return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, out));
}

}

void BlitPipelineD3D11::release()
{
	if (vs) { vs->Release(); vs = nullptr; }
	if (ps_blit) { ps_blit->Release(); ps_blit = nullptr; }
	if (ps_debug) { ps_debug->Release(); ps_debug = nullptr; }
	if (sampler) { sampler->Release(); sampler = nullptr; }
	if (rs) { rs->Release(); rs = nullptr; }
	if (dss) { dss->Release(); dss = nullptr; }
	if (bs) { bs->Release(); bs = nullptr; }
	if (cb) { cb->Release(); cb = nullptr; }
	ready = false;
	device = nullptr;
}

bool BlitPipelineD3D11::ensure(ID3D11Device *dev)
{
	if (ready && device == dev)
		return true;
	release();
	device = dev;
	if (device == nullptr)
		return false;

	struct Entry { const char *name; const char *target; ID3DBlob *blob; };
	Entry entries[] = {
		{ "VSMain", "vs_5_0", nullptr },
		{ "PSMain", "ps_5_0", nullptr },
		{ "PSDebug", "ps_5_0", nullptr },
	};
	bool compiled = true;
	for (Entry &e : entries)
		compiled = compiled && compile_entry(e.name, e.target, &e.blob);
	if (compiled) {
		auto ps = [&](ID3DBlob *blob, ID3D11PixelShader **out) {
			return SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out));
		};
		compiled = SUCCEEDED(device->CreateVertexShader(entries[0].blob->GetBufferPointer(),
			entries[0].blob->GetBufferSize(), nullptr, &vs)) &&
			ps(entries[1].blob, &ps_blit) && ps(entries[2].blob, &ps_debug);
	}
	for (Entry &e : entries)
		if (e.blob != nullptr)
			e.blob->Release();
	if (!compiled) {
		release();
		return false;
	}

	D3D11_SAMPLER_DESC sd{};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	if (FAILED(device->CreateSamplerState(&sd, &sampler))) {
		release();
		return false;
	}

	D3D11_RASTERIZER_DESC rd{};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	if (FAILED(device->CreateRasterizerState(&rd, &rs))) {
		release();
		return false;
	}

	D3D11_DEPTH_STENCIL_DESC dd{};
	dd.DepthEnable = FALSE;
	dd.StencilEnable = FALSE;
	if (FAILED(device->CreateDepthStencilState(&dd, &dss))) {
		release();
		return false;
	}

	D3D11_BLEND_DESC bd{};
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(device->CreateBlendState(&bd, &bs))) {
		release();
		return false;
	}

	D3D11_BUFFER_DESC cbd{};
	cbd.ByteWidth = 64;
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(device->CreateBuffer(&cbd, nullptr, &cb))) {
		release();
		return false;
	}

	ready = true;
	return true;
}

bool BlitPipelineD3D11::composite(ID3D11DeviceContext *ctx, ID3D11Resource *effect,
	ID3D11Resource *dst, float sharpness, float jitter_u, float jitter_v)
{
	last_stage = 0;
	if (ctx == nullptr || effect == nullptr || dst == nullptr || device == nullptr) {
		last_stage = -1;
		return false;
	}
	if (!ensure(device)) {
		last_stage = -2;
		return false;
	}

	const TexInfo dst_info = tex_info(dst);

	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {

			float cbd[16] = {
				0.0f, 0.0f,
				static_cast<float>(dst_info.width), static_cast<float>(dst_info.height),
				sharpness, 0.0f, 0.0f, 0.0f,
				jitter_u, jitter_v,
				0.0f, 0.0f };
			memcpy(mapped.pData, cbd, sizeof(cbd));
			ctx->Unmap(cb, 0);
		}
	}

	ID3D11ShaderResourceView *srv_fx = nullptr;
	ID3D11RenderTargetView *rtv = nullptr;

	auto make_srv = [this](ID3D11Resource *res, ID3D11ShaderResourceView **out) {
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		desc.Format = view_format_for(tex_info(res).format);
		desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		desc.Texture2D.MipLevels = 1;
		return SUCCEEDED(device->CreateShaderResourceView(res, &desc, out));
	};

	D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
	rtv_desc.Format = view_format_for(dst_info.format);
	rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

	bool ok = make_srv(effect, &srv_fx);
	if (!ok)
		last_stage = -3;
	if (ok) {
		ok = SUCCEEDED(device->CreateRenderTargetView(dst, &rtv_desc, &rtv));
		if (!ok)
			last_stage = -4;
	}

	if (!ok) {
		if (srv_fx) srv_fx->Release();
		if (rtv) rtv->Release();
		return false;
	}

	ID3D11RenderTargetView *old_rtv = nullptr;
	ID3D11DepthStencilView *old_dsv = nullptr;
	ctx->OMGetRenderTargets(1, &old_rtv, &old_dsv);

	D3D11_VIEWPORT vp{};
	vp.Width = static_cast<float>(dst_info.width);
	vp.Height = static_cast<float>(dst_info.height);
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);
	ctx->OMSetRenderTargets(1, &rtv, nullptr);
	ctx->OMSetBlendState(bs, nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(dss, 0);
	ctx->RSSetState(rs);
	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(vs, nullptr, 0);
	ctx->PSSetShader(ps_blit, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &cb);
	ID3D11ShaderResourceView *srvs[3] = { srv_fx, nullptr, nullptr };
	ctx->PSSetShaderResources(0, 3, srvs);
	ctx->PSSetSamplers(0, 1, &sampler);
	ctx->Draw(3, 0);

	ID3D11ShaderResourceView *null_srvs[3] = {};
	ctx->PSSetShaderResources(0, 3, null_srvs);
	ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);
	if (old_rtv) old_rtv->Release();
	if (old_dsv) old_dsv->Release();
	srv_fx->Release();
	rtv->Release();
	return true;
}

bool BlitPipelineD3D11::blit(ID3D11DeviceContext *ctx, ID3D11Resource *src, ID3D11Resource *dst, float sharpness,
	float jitter_u, float jitter_v)
{
	if (ctx == nullptr || src == nullptr || dst == nullptr)
		return false;

	const TexInfo src_info = tex_info(src);
	const TexInfo dst_info = tex_info(dst);

	const bool jittered = jitter_u != 0.0f || jitter_v != 0.0f;
	if (!jittered && sharpness <= 1e-4f && src_info.format == dst_info.format &&
		src_info.width == dst_info.width && src_info.height == dst_info.height) {
		last_stage = 1;
		ctx->CopyResource(dst, src);
		return true;
	}
	return composite(ctx, src, dst, sharpness, jitter_u, jitter_v);
}

bool BlitPipelineD3D11::bias_mask(ID3D11DeviceContext *ctx, ID3D11Resource *confidence,
	ID3D11Resource *dst, float strength)
{
	return mask_pass(ctx, confidence, dst, strength, 5.0f);
}

bool BlitPipelineD3D11::mask_pass(ID3D11DeviceContext *ctx, ID3D11Resource *src,
	ID3D11Resource *dst, float strength, float mode)
{
	if (ctx == nullptr || src == nullptr || dst == nullptr)
		return false;
	ID3D11Device *ctx_device = nullptr;
	ctx->GetDevice(&ctx_device);
	const bool pipeline_ready = ctx_device != nullptr && ensure(ctx_device);
	if (ctx_device != nullptr)
		ctx_device->Release();
	if (!pipeline_ready)
		return false;

	const TexInfo dst_info = tex_info(dst);

	{
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {

			float cbd[16] = {
				0.0f, 1.0f,
				static_cast<float>(dst_info.width), static_cast<float>(dst_info.height),
				strength, mode, 0.0f, 0.0f };
			memcpy(mapped.pData, cbd, sizeof(cbd));
			ctx->Unmap(cb, 0);
		}
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
	srv_desc.Format = view_format_for(tex_info(src).format);
	srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MipLevels = 1;
	ID3D11ShaderResourceView *srv = nullptr;
	if (FAILED(device->CreateShaderResourceView(src, &srv_desc, &srv)))
		return false;

	D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
	rtv_desc.Format = view_format_for(dst_info.format);
	rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	ID3D11RenderTargetView *rtv = nullptr;
	if (FAILED(device->CreateRenderTargetView(dst, &rtv_desc, &rtv))) {
		srv->Release();
		return false;
	}

	ID3D11RenderTargetView *old_rtv = nullptr;
	ID3D11DepthStencilView *old_dsv = nullptr;
	ctx->OMGetRenderTargets(1, &old_rtv, &old_dsv);

	D3D11_VIEWPORT vp{};
	vp.Width = static_cast<float>(dst_info.width);
	vp.Height = static_cast<float>(dst_info.height);
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);
	ctx->OMSetRenderTargets(1, &rtv, nullptr);
	ctx->OMSetBlendState(bs, nullptr, 0xffffffff);
	ctx->OMSetDepthStencilState(dss, 0);
	ctx->RSSetState(rs);
	ctx->IASetInputLayout(nullptr);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(vs, nullptr, 0);
	ctx->PSSetShader(ps_debug, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &cb);
	ID3D11ShaderResourceView *srvs[3] = { srv, nullptr, nullptr };
	ctx->PSSetShaderResources(0, 3, srvs);
	ctx->PSSetSamplers(0, 1, &sampler);
	ctx->Draw(3, 0);

	ID3D11ShaderResourceView *null_srvs[3] = {};
	ctx->PSSetShaderResources(0, 3, null_srvs);
	ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);
	if (old_rtv) old_rtv->Release();
	if (old_dsv) old_dsv->Release();
	srv->Release();
	rtv->Release();
	return true;
}

}
