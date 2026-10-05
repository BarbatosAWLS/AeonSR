#include "aeon_sr/jitter/hud_restore.hpp"

#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/dxgi_format_util.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace aeon_sr {

namespace {

constexpr uint32_t kConstantCount = 16;
static_assert(kHudRestoreCap <= 255u && kHudRestoreNeed <= 255u, "each fits a byte of the state");

constexpr const char kHudHlsl[] = R"(
cbuffer C : register(b0)
{
	float2 size;
	float2 inv_size;
	float2 shift;
	float gain;
	float have_prev;
	float need;
	float cost;
	float cap;
	float flat;
	float ring;
	float drawn;
	float split;
	float history;
};

Texture2D<float4> frame_now : register(t0);
Texture2D<float4> frame_before : register(t1);
Texture2D<float> mask_in : register(t2);
Texture2D<uint> state_in : register(t3);
Texture2D<float4> stable_in : register(t4);
RWTexture2D<uint> state_out : register(u0);
RWTexture2D<unorm float> mask_out : register(u1);
RWTexture2D<float2> motion_out : register(u2);
RWTexture2D<float4> stable_out : register(u3);
Texture2D<float4> trans_in : register(t5);
Texture2D<float4> scene_now : register(t6);
Texture2D<float4> scene_prev : register(t7);
RWTexture2D<float4> trans_out : register(u4);
SamplerState linear_clamp : register(s0);

static const uint kRun = 0xFFu;
static const uint kBudget = 0xFF00u;
static const uint kMember = 1u << 16;
static const uint kSame = 1u << 17;

float luma(float3 c)
{
	return dot(c, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)]
void CSEvidence(uint3 id : SV_DispatchThreadID)
{
	const int2 dim = int2(size);
	if (id.x >= (uint)dim.x || id.y >= (uint)dim.y)
		return;
	const int2 p = int2(id.xy);
	if (have_prev < 0.5) {
		state_out[p] = 0u;
		return;
	}
	const uint s = state_in.Load(int3(p, 0));
	uint run = s & kRun;
	uint budget = (s & kBudget) >> 8;
	bool member = (s & kMember) != 0;
	const float4 now = frame_now.Load(int3(p, 0));
	const bool same = all(now.rgb == frame_before.Load(int3(p, 0)).rgb);
	if (same) {
		run = min(run + (uint)gain, 255u);
		if (member)
			budget = min(budget + (uint)gain, (uint)cap);
	} else {
		run = 0u;
		const bool back = member && all(now.rgb == stable_in.Load(int3(p, 0)).rgb);
		const bool changed_before = (s & kSame) == 0;
		if (!back) {
			budget = budget > (uint)cost && !changed_before ? budget - (uint)cost : 0u;
			member = member && budget > 0u;
		}
	}
	state_out[p] = run | (budget << 8) | (member ? kMember : 0u) | (same ? kSame : 0u);
}

bool candidate(uint s)
{
	return (s & kSame) != 0 && (s & kRun) >= (uint)need;
}

[numthreads(8, 8, 1)]
void CSMask(uint3 id : SV_DispatchThreadID)
{
	const int2 dim = int2(size);
	if (id.x >= (uint)dim.x || id.y >= (uint)dim.y)
		return;
	const int2 p = int2(id.xy);
	const uint s = state_in.Load(int3(p, 0));
	bool member = (s & kMember) != 0;
	uint budget = (s & kBudget) >> 8;
	if (!member && candidate(s)) {
		const float l = luma(frame_now.Load(int3(p, 0)).rgb);
		[unroll] for (int y = -1; y <= 1; ++y) {
			[unroll] for (int x = -1; x <= 1; ++x) {
				const int2 q = p + int2(x, y);
				if ((x == 0 && y == 0) || any(q < 0) || any(q >= dim))
					continue;
				const uint t = state_in.Load(int3(q, 0));
				if (((t & kMember) != 0 || candidate(t)) && abs(luma(frame_now.Load(int3(q, 0)).rgb) - l) > flat)
					member = true;
			}
		}
		if (member)
			budget = (uint)cap;
	}
	state_out[p] = (s & (kRun | kSame)) | (budget << 8) | (member ? kMember : 0u);
	if ((s & kSame) != 0 && (gain > 0.5 || drawn < 0.5))
		stable_out[p] = frame_now.Load(int3(p, 0));
	const bool shown = (s & kSame) != 0 && ((s & kRun) != 0 || drawn < 0.5);
	const bool interface_drawn = split > 0.5 && any(frame_now.Load(int3(p, 0)).rgb != frame_before.Load(int3(p, 0)).rgb);
	mask_out[p] = interface_drawn ? 1.0 : !member ? 0.0 : (shown ? 1.0 : 128.0 / 255.0);
}

static const float kTransDecay = 0.85;

[numthreads(8, 8, 1)]
void CSTrans(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= (uint)size.x || id.y >= (uint)size.y)
		return;
	const int3 p = int3(id.xy, 0);
	if (history < 0.5) {
		trans_out[id.xy] = 0.0;
		return;
	}
	const float3 df = frame_now.Load(p).rgb - frame_before.Load(p).rgb;
	const float3 ds = scene_now.Load(p).rgb - scene_prev.Load(p).rgb;
	trans_out[id.xy] = trans_in.Load(p) * kTransDecay + float4(dot(df, ds), dot(ds, ds), dot(df, df), 0.0);
}

float transparency(int2 p)
{
	const float4 m = trans_in.Load(int3(p, 0));
	if (m.y <= 2e-4 || m.x <= 0.0 || m.x * m.x < 0.81 * m.y * m.z)
		return 0.0;
	return saturate(m.x / m.y);
}

[numthreads(8, 8, 1)]
void CSClear(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= (uint)size.x || id.y >= (uint)size.y)
		return;
	if (mask_in.Load(int3(id.xy, 0)) > 0.25)
		motion_out[id.xy] = 0.0;
}

struct VSOut
{
	float4 pos : SV_Position;
};

VSOut VSMain(uint i : SV_VertexID)
{
	VSOut o;
	const float2 t = float2((i << 1) & 2, i & 2);
	o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
	return o;
}

float4 PSComposite(VSOut i) : SV_Target
{
	const int2 p = int2(i.pos.xy);
	const float m = mask_in.Load(int3(p, 0));
	if (m < 0.25)
		discard;
	if (m <= 0.75)
		return float4(stable_in.Load(int3(p, 0)).rgb, 0.0);
	const float3 f = frame_now.Load(int3(p, 0)).rgb;
	if (split < 0.5)
		return float4(f, 0.0);
	const float t = transparency(p);
	return float4(f - t * scene_now.Load(int3(p, 0)).rgb, t);
}

float4 PSRegister(VSOut i) : SV_Target
{
	const int2 p = int2(i.pos.xy);
	float m = mask_in.Load(int3(p, 0));
	if (ring > 0.5) {
		const int2 dim = int2(size);
		[unroll] for (int y = -1; y <= 1; ++y) {
			[unroll] for (int x = -1; x <= 1; ++x)
				m = max(m, mask_in.Load(int3(clamp(p + int2(x, y), int2(0, 0), dim - 1), 0)));
		}
	}
	if (m < 0.75)
		discard;
	return frame_now.SampleLevel(linear_clamp, (i.pos.xy - shift) * inv_size, 0.0);
}

float4 PSClear(VSOut i) : SV_Target
{
	if (mask_in.Load(int3(int2(i.pos.xy), 0)) < 0.25)
		discard;
	return 0.0;
}
)";

const char *const kEntry[] = { "PSComposite", "PSRegister", "PSClear" };

ID3DBlob *compile(const char *entry, const char *target)
{
	ID3DBlob *blob = nullptr, *err = nullptr;
	const HRESULT hr = D3DCompile(kHudHlsl, sizeof(kHudHlsl) - 1, "aeon_hud_restore", nullptr, nullptr, entry, target,
		D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
	if (FAILED(hr)) {
		std::wstring what = L"interface restore: shader " + std::wstring(entry, entry + std::strlen(entry)) +
			L" did not compile";
		if (err != nullptr) {
			const char *text = static_cast<const char *>(err->GetBufferPointer());
			what += L": " + std::wstring(text, text + std::strlen(text));
			err->Release();
		}
		diag_error("hud", what);
		return nullptr;
	}
	if (err != nullptr)
		err->Release();
	return blob;
}

template <typename T>
void rel(T *&p) noexcept
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

void release_blob(void *&b) noexcept
{
	if (b != nullptr) {
		static_cast<ID3DBlob *>(b)->Release();
		b = nullptr;
	}
}

bool typeless(DXGI_FORMAT f) noexcept
{
	switch (f) {
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
	case DXGI_FORMAT_R32G32B32_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R32G32_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R16G16_TYPELESS:
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		return true;
	default:
		return false;
	}
}

bool single_sample_target(const D3D12_RESOURCE_DESC &d) noexcept
{
	return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
		(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0 && d.SampleDesc.Count == 1 &&
		d.MipLevels == 1 && d.DepthOrArraySize == 1 && !typeless(view_format_for(d.Format));
}

bool is_srgb(DXGI_FORMAT f) noexcept
{
	return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
		f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

DXGI_FORMAT stable_format(DXGI_FORMAT f) noexcept
{
	if (is_srgb(f))
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	switch (format_family(f)) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		return DXGI_FORMAT_R32G32B32A32_FLOAT;
	default:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
}

}

bool HudRestoreD3D12::ensure_pipeline(ID3D12Device *device)
{
	if (root_ != nullptr && evidence_ != nullptr && mask_ != nullptr && clear_ != nullptr && trans_pso_ != nullptr)
		return true;
	if (pipeline_failed_ || device == nullptr)
		return false;

	D3D12_DESCRIPTOR_RANGE srv{};
	srv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv.NumDescriptors = kSrvCount;
	D3D12_DESCRIPTOR_RANGE uav{};
	uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	uav.NumDescriptors = kUavCount;
	D3D12_ROOT_PARAMETER rp[3]{};
	rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rp[0].Constants.Num32BitValues = kConstantCount;
	rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rp[1].DescriptorTable.NumDescriptorRanges = 1;
	rp[1].DescriptorTable.pDescriptorRanges = &srv;
	rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rp[2].DescriptorTable.NumDescriptorRanges = 1;
	rp[2].DescriptorTable.pDescriptorRanges = &uav;
	D3D12_STATIC_SAMPLER_DESC samp{};
	samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.MaxLOD = D3D12_FLOAT32_MAX;
	D3D12_ROOT_SIGNATURE_DESC rs{};
	rs.NumParameters = 3;
	rs.pParameters = rp;
	rs.NumStaticSamplers = 1;
	rs.pStaticSamplers = &samp;
	ID3DBlob *sig = nullptr, *sig_err = nullptr;
	bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sig_err));
	rel(sig_err);
	ok = ok && SUCCEEDED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
		IID_PPV_ARGS(&root_)));
	rel(sig);

	ID3DBlob *cs[4]{};
	ID3DBlob *vs = nullptr;
	ID3DBlob *ps[KindCount]{};
	if (ok) {
		cs[0] = compile("CSEvidence", "cs_5_0");
		cs[1] = compile("CSMask", "cs_5_0");
		cs[2] = compile("CSClear", "cs_5_0");
		cs[3] = compile("CSTrans", "cs_5_0");
		vs = compile("VSMain", "vs_5_0");
		ok = cs[0] != nullptr && cs[1] != nullptr && cs[2] != nullptr && cs[3] != nullptr && vs != nullptr;
		for (uint32_t k = 0; ok && k < KindCount; ++k) {
			ps[k] = compile(kEntry[k], "ps_5_0");
			ok = ps[k] != nullptr;
		}
	}
	ID3D12PipelineState **const outs[4] = { &evidence_, &mask_, &clear_, &trans_pso_ };
	for (uint32_t k = 0; ok && k < 4; ++k) {
		D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
		cd.pRootSignature = root_;
		cd.CS = { cs[k]->GetBufferPointer(), cs[k]->GetBufferSize() };
		ok = SUCCEEDED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(outs[k])));
	}
	for (ID3DBlob *&b : cs)
		rel(b);
	if (ok) {
		vs_ = vs;
		for (uint32_t k = 0; k < KindCount; ++k)
			ps_[k] = ps[k];
		vs = nullptr;
	} else {
		rel(vs);
		for (ID3DBlob *&b : ps)
			rel(b);
	}

	D3D12_DESCRIPTOR_HEAP_DESC hd{};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = kRingSlots * kSlotDescriptors;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ok = ok && SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap_)));
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hd.NumDescriptors = kRtvSlots;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	ok = ok && SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)));
	if (!ok) {
		diag_error("hud", L"interface restore: its pipeline could not be made, the interface is left to the upscaler");
		release();
		pipeline_failed_ = true;
		return false;
	}
	srv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	rtv_stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	return true;
}

bool HudRestoreD3D12::ensure_targets(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT format)
{
	const bool same = w_ == w && h_ == h && format_ == format;
	if (same && frames_[0] != nullptr)
		return true;
	if (same && targets_failed_)
		return false;
	uint32_t room = 0;
	for (const Retired &r : retired_)
		room += r.res == nullptr ? 1u : 0u;
	if (room < kTargets)
		return false;
	retire(frames_[0]);
	retire(frames_[1]);
	retire(stable_);
	retire(state_[0]);
	retire(state_[1]);
	retire(core_);
	retire(scenes_[0]);
	retire(scenes_[1]);
	retire(trans_[0]);
	retire(trans_[1]);
	prev_split_ = false;
	w_ = w;
	h_ = h;
	format_ = format;
	have_prev_ = false;
	has_mask_ = false;
	targets_failed_ = !create_tex12(device, w, h, format, false, kSrv, &frames_[0]) ||
		!create_tex12(device, w, h, format, false, kSrv, &frames_[1]) ||
		!create_tex12(device, w, h, stable_format(format), true, kSrv, &stable_) ||
		!create_tex12(device, w, h, DXGI_FORMAT_R32_UINT, true, kSrv, &state_[0]) ||
		!create_tex12(device, w, h, DXGI_FORMAT_R32_UINT, true, kSrv, &state_[1]) ||
		!create_tex12(device, w, h, DXGI_FORMAT_R8_UNORM, true, kSrv, &core_) ||
		!create_tex12(device, w, h, format, false, kSrv, &scenes_[0]) ||
		!create_tex12(device, w, h, format, false, kSrv, &scenes_[1]) ||
		!create_tex12(device, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, true, kSrv, &trans_[0]) ||
		!create_tex12(device, w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, true, kSrv, &trans_[1]);
	if (targets_failed_) {
		ID3D12Resource **const all[] = { &frames_[0], &frames_[1], &stable_, &state_[0], &state_[1], &core_,
			&scenes_[0], &scenes_[1], &trans_[0], &trans_[1] };
		for (ID3D12Resource **r : all)
			rel(*r);
		diag_error("hud", L"interface restore: its textures could not be made, the interface is left to the upscaler");
		return false;
	}
	return true;
}

void HudRestoreD3D12::retire(ID3D12Resource *&res) noexcept
{
	if (res == nullptr)
		return;
	for (Retired &r : retired_) {
		if (r.res == nullptr) {
			r.res = res;
			r.frames = kRetireFrames;
			res = nullptr;
			return;
		}
	}
	res->Release();
	res = nullptr;
}

void HudRestoreD3D12::age_retired() noexcept
{
	for (Retired &r : retired_) {
		if (r.res != nullptr && --r.frames == 0)
			rel(r.res);
	}
}

ID3D12PipelineState *HudRestoreD3D12::graphics_pso(ID3D12Device *device, Kind kind, DXGI_FORMAT rtv)
{
	Pso *free_slot = nullptr;
	for (Pso &p : psos_) {
		if (p.pso != nullptr && p.format == rtv && p.kind == kind)
			return p.pso;
		if (p.pso == nullptr && free_slot == nullptr)
			free_slot = &p;
	}
	if (free_slot == nullptr)
		return nullptr;
	ID3DBlob *const vs = static_cast<ID3DBlob *>(vs_);
	ID3DBlob *const ps = static_cast<ID3DBlob *>(ps_[kind]);
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
	d.pRootSignature = root_;
	d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	if (kind == Composite) {
		D3D12_RENDER_TARGET_BLEND_DESC &b = d.BlendState.RenderTarget[0];
		b.BlendEnable = TRUE;
		b.SrcBlend = D3D12_BLEND_ONE;
		b.DestBlend = D3D12_BLEND_SRC_ALPHA;
		b.BlendOp = D3D12_BLEND_OP_ADD;
		b.SrcBlendAlpha = D3D12_BLEND_ZERO;
		b.DestBlendAlpha = D3D12_BLEND_ONE;
		b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	}
	d.SampleMask = UINT_MAX;
	d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	d.RasterizerState.DepthClipEnable = TRUE;
	d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d.NumRenderTargets = 1;
	d.RTVFormats[0] = rtv;
	d.SampleDesc.Count = 1;
	if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&free_slot->pso)))) {
		free_slot->pso = nullptr;
		return nullptr;
	}
	free_slot->format = rtv;
	free_slot->kind = kind;
	return free_slot->pso;
}

bool HudRestoreD3D12::matches(ID3D12Resource *res) const noexcept
{
	if (res == nullptr)
		return false;
	const D3D12_RESOURCE_DESC d = res->GetDesc();
	return single_sample_target(d) && static_cast<uint32_t>(d.Width) == w_ && d.Height == h_;
}

HudRestoreD3D12::Slot HudRestoreD3D12::next_slot() noexcept
{
	ring_ = (ring_ + 1u) % kRingSlots;
	Slot s;
	s.cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
	s.cpu.ptr += static_cast<SIZE_T>(ring_) * kSlotDescriptors * srv_stride_;
	s.srv_gpu = srv_heap_->GetGPUDescriptorHandleForHeapStart();
	s.srv_gpu.ptr += static_cast<UINT64>(ring_) * kSlotDescriptors * srv_stride_;
	s.uav_gpu = s.srv_gpu;
	s.uav_gpu.ptr += static_cast<UINT64>(kSrvCount) * srv_stride_;
	return s;
}

void HudRestoreD3D12::write_srv(ID3D12Device *device, const Slot &slot, uint32_t i, ID3D12Resource *res) const
{
	D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
	sd.Format = res != nullptr ? view_format_for(res->GetDesc().Format) : DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sd.Texture2D.MipLevels = 1;
	D3D12_CPU_DESCRIPTOR_HANDLE at = slot.cpu;
	at.ptr += static_cast<SIZE_T>(i) * srv_stride_;
	device->CreateShaderResourceView(res, &sd, at);
}

void HudRestoreD3D12::write_uav(ID3D12Device *device, const Slot &slot, uint32_t i, ID3D12Resource *res) const
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
	ud.Format = res != nullptr ? view_format_for(res->GetDesc().Format) : DXGI_FORMAT_R32_FLOAT;
	ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	D3D12_CPU_DESCRIPTOR_HANDLE at = slot.cpu;
	at.ptr += static_cast<SIZE_T>(kSrvCount + i) * srv_stride_;
	device->CreateUnorderedAccessView(res, nullptr, &ud, at);
}

void HudRestoreD3D12::constants(float *c) const noexcept
{
	const float v[kConstantCount] = {
		static_cast<float>(w_), static_cast<float>(h_), 1.0f / static_cast<float>(w_), 1.0f / static_cast<float>(h_),
		frame_.x, frame_.y, gain_ ? 1.0f : 0.0f, have_prev_ ? 1.0f : 0.0f,
		static_cast<float>(std::clamp(tuning.need, 1u, 255u)), static_cast<float>(tuning.cost),
		static_cast<float>(std::min(tuning.cap, 255u)), tuning.flat,
		tuning.register_ring ? 1.0f : 0.0f, frame_.drawn ? 1.0f : 0.0f, frame_.scene != nullptr ? 1.0f : 0.0f,
		trans_history_ ? 1.0f : 0.0f };
	std::memcpy(c, v, sizeof(v));
}

bool HudRestoreD3D12::detect(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
	D3D12_RESOURCE_STATES color_state, const HudRestoreFrame &frame)
{
	has_mask_ = false;
	age_retired();
	if (device == nullptr || cmd == nullptr || color == nullptr)
		return false;
	const D3D12_RESOURCE_DESC cd = color->GetDesc();
	if (!single_sample_target(cd) || !ensure_pipeline(device) ||
		!ensure_targets(device, static_cast<uint32_t>(cd.Width), cd.Height, cd.Format))
		return false;

	const uint32_t now = cur_ ^ 1u;
	barrier12(cmd, color, color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier12(cmd, frames_[now], kSrv, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->CopyResource(frames_[now], color);
	barrier12(cmd, frames_[now], D3D12_RESOURCE_STATE_COPY_DEST, kSrv);
	barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, color_state);

	const float step = std::max(std::fabs(frame.x - prev_x_), std::fabs(frame.y - prev_y_));
	gain_ = have_prev_ && frame.drawn && prev_drawn_ && step >= kHudRestoreMinStep;
	frame_ = frame;
	if (frame_.scene != nullptr) {
		const D3D12_RESOURCE_DESC sd = frame_.scene->GetDesc();
		if (sd.Width != cd.Width || sd.Height != cd.Height || sd.Format != cd.Format || sd.SampleDesc.Count != 1)
			frame_.scene = nullptr;
	}
	const bool split = frame_.scene != nullptr;
	if (split) {
		barrier12(cmd, frame_.scene, frame_.scene_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier12(cmd, scenes_[now], kSrv, D3D12_RESOURCE_STATE_COPY_DEST);
		cmd->CopyResource(scenes_[now], frame_.scene);
		barrier12(cmd, scenes_[now], D3D12_RESOURCE_STATE_COPY_DEST, kSrv);
		barrier12(cmd, frame_.scene, D3D12_RESOURCE_STATE_COPY_SOURCE, frame_.scene_state);
	}
	trans_history_ = split && have_prev_ && prev_split_;
	float c[kConstantCount];
	constants(c);

	const Slot first = next_slot();
	write_srv(device, first, 0, frames_[now]);
	write_srv(device, first, 1, frames_[cur_]);
	write_srv(device, first, 2, nullptr);
	write_srv(device, first, 3, state_[0]);
	write_srv(device, first, 4, stable_);
	write_uav(device, first, 0, state_[1]);
	write_uav(device, first, 1, nullptr);
	write_uav(device, first, 2, nullptr);
	write_uav(device, first, 3, nullptr);
	for (uint32_t i = 5; i < kSrvCount; ++i)
		write_srv(device, first, i, nullptr);
	write_uav(device, first, 4, nullptr);
	const Slot second = next_slot();
	write_srv(device, second, 0, frames_[now]);
	write_srv(device, second, 1, split ? scenes_[now] : nullptr);
	write_srv(device, second, 2, nullptr);
	write_srv(device, second, 3, state_[1]);
	write_srv(device, second, 4, nullptr);
	write_uav(device, second, 0, state_[0]);
	write_uav(device, second, 1, core_);
	write_uav(device, second, 2, nullptr);
	write_uav(device, second, 3, stable_);
	for (uint32_t i = 5; i < kSrvCount; ++i)
		write_srv(device, second, i, nullptr);
	write_uav(device, second, 4, nullptr);

	const UINT gx = (w_ + 7u) / 8u, gy = (h_ + 7u) / 8u;
	cmd->SetComputeRootSignature(root_);
	cmd->SetDescriptorHeaps(1, &srv_heap_);
	cmd->SetComputeRoot32BitConstants(0, kConstantCount, c, 0);

	barrier12(cmd, state_[1], kSrv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	cmd->SetComputeRootDescriptorTable(1, first.srv_gpu);
	cmd->SetComputeRootDescriptorTable(2, first.uav_gpu);
	cmd->SetPipelineState(evidence_);
	cmd->Dispatch(gx, gy, 1u);
	barrier12(cmd, state_[1], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrv);

	barrier12(cmd, state_[0], kSrv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	barrier12(cmd, core_, kSrv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	barrier12(cmd, stable_, kSrv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	cmd->SetComputeRootDescriptorTable(1, second.srv_gpu);
	cmd->SetComputeRootDescriptorTable(2, second.uav_gpu);
	cmd->SetPipelineState(mask_);
	cmd->Dispatch(gx, gy, 1u);
	barrier12(cmd, state_[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrv);
	barrier12(cmd, core_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrv);
	barrier12(cmd, stable_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrv);

	if (split) {
		const Slot third = next_slot();
		write_srv(device, third, 0, frames_[now]);
		write_srv(device, third, 1, frames_[cur_]);
		write_srv(device, third, 2, nullptr);
		write_srv(device, third, 3, nullptr);
		write_srv(device, third, 4, nullptr);
		write_srv(device, third, 5, trans_[trans_cur_]);
		write_srv(device, third, 6, scenes_[now]);
		write_srv(device, third, 7, scenes_[cur_]);
		for (uint32_t i = 0; i < 4; ++i)
			write_uav(device, third, i, nullptr);
		write_uav(device, third, 4, trans_[trans_cur_ ^ 1u]);
		barrier12(cmd, trans_[trans_cur_ ^ 1u], kSrv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		cmd->SetComputeRootDescriptorTable(1, third.srv_gpu);
		cmd->SetComputeRootDescriptorTable(2, third.uav_gpu);
		cmd->SetPipelineState(trans_pso_);
		cmd->Dispatch(gx, gy, 1u);
		barrier12(cmd, trans_[trans_cur_ ^ 1u], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kSrv);
		trans_cur_ ^= 1u;
	}
	prev_split_ = split;

	cur_ = now;
	have_prev_ = true;
	prev_drawn_ = frame.drawn;
	prev_x_ = frame.x;
	prev_y_ = frame.y;
	has_mask_ = true;
	return true;
}

bool HudRestoreD3D12::draw(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, Kind kind, ID3D12Resource *dst,
	D3D12_RESOURCE_STATES dst_state)
{
	const DXGI_FORMAT rtv = view_format_for(dst->GetDesc().Format);
	ID3D12PipelineState *const pso = graphics_pso(device, kind, rtv);
	if (pso == nullptr)
		return false;

	const Slot slot = next_slot();
	write_srv(device, slot, 0, frames_[cur_]);
	write_srv(device, slot, 1, nullptr);
	write_srv(device, slot, 2, core_);
	write_srv(device, slot, 3, nullptr);
	write_srv(device, slot, 4, stable_);
	write_srv(device, slot, 5, frame_.scene != nullptr ? trans_[trans_cur_] : nullptr);
	write_srv(device, slot, 6, frame_.scene != nullptr ? scenes_[cur_] : nullptr);
	write_srv(device, slot, 7, nullptr);
	for (uint32_t i = 0; i < kUavCount; ++i)
		write_uav(device, slot, i, nullptr);
	rtv_next_ = (rtv_next_ + 1u) % kRtvSlots;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
	rtv_cpu.ptr += static_cast<SIZE_T>(rtv_next_) * rtv_stride_;
	D3D12_RENDER_TARGET_VIEW_DESC rd{};
	rd.Format = rtv;
	rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(dst, &rd, rtv_cpu);

	float c[kConstantCount];
	constants(c);
	const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(w_), static_cast<float>(h_), 0.0f, 1.0f };
	const D3D12_RECT sc{ 0, 0, static_cast<LONG>(w_), static_cast<LONG>(h_) };

	barrier12(cmd, dst, dst_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
	cmd->SetGraphicsRootSignature(root_);
	cmd->SetDescriptorHeaps(1, &srv_heap_);
	cmd->SetGraphicsRoot32BitConstants(0, kConstantCount, c, 0);
	cmd->SetGraphicsRootDescriptorTable(1, slot.srv_gpu);
	cmd->SetGraphicsRootDescriptorTable(2, slot.uav_gpu);
	cmd->OMSetRenderTargets(1, &rtv_cpu, FALSE, nullptr);
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &sc);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->SetPipelineState(pso);
	cmd->DrawInstanced(3, 1, 0, 0);
	barrier12(cmd, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, dst_state);
	return true;
}

bool HudRestoreD3D12::register_input(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
	D3D12_RESOURCE_STATES color_state)
{
	if (!has_mask_ || !frame_.drawn || (frame_.x == 0.0f && frame_.y == 0.0f) || device == nullptr ||
		cmd == nullptr || !matches(color))
		return false;
	return draw(device, cmd, Register, color, color_state);
}

bool HudRestoreD3D12::clear_motion(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *motion,
	D3D12_RESOURCE_STATES state)
{
	if (!has_mask_ || device == nullptr || cmd == nullptr || motion == nullptr)
		return false;
	const D3D12_RESOURCE_DESC d = motion->GetDesc();
	if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 ||
		static_cast<uint32_t>(d.Width) != w_ || d.Height != h_)
		return false;
	if ((d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
		return matches(motion) && draw(device, cmd, Clear, motion, state);

	const Slot slot = next_slot();
	for (uint32_t i = 0; i < kSrvCount; ++i)
		write_srv(device, slot, i, i == 2 ? core_ : nullptr);
	for (uint32_t i = 0; i < kUavCount; ++i)
		write_uav(device, slot, i, i == 2 ? motion : nullptr);
	float c[kConstantCount];
	constants(c);
	barrier12(cmd, motion, state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	cmd->SetComputeRootSignature(root_);
	cmd->SetDescriptorHeaps(1, &srv_heap_);
	cmd->SetComputeRoot32BitConstants(0, kConstantCount, c, 0);
	cmd->SetComputeRootDescriptorTable(1, slot.srv_gpu);
	cmd->SetComputeRootDescriptorTable(2, slot.uav_gpu);
	cmd->SetPipelineState(clear_);
	cmd->Dispatch((w_ + 7u) / 8u, (h_ + 7u) / 8u, 1u);
	if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
		D3D12_RESOURCE_BARRIER ub{};
		ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
		ub.UAV.pResource = motion;
		cmd->ResourceBarrier(1, &ub);
	}
	barrier12(cmd, motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, state);
	return true;
}

bool HudRestoreD3D12::composite(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
	D3D12_RESOURCE_STATES state)
{
	if (!has_mask_ || device == nullptr || cmd == nullptr || !matches(color))
		return false;
	return draw(device, cmd, Composite, color, state);
}

void HudRestoreD3D12::reset() noexcept
{
	have_prev_ = false;
	has_mask_ = false;
	prev_drawn_ = false;
	prev_x_ = prev_y_ = 0.0f;
	gain_ = false;
	prev_split_ = false;
}

void HudRestoreD3D12::release() noexcept
{
	for (Pso &p : psos_) {
		rel(p.pso);
		p.format = DXGI_FORMAT_UNKNOWN;
		p.kind = 0;
	}
	rel(evidence_);
	rel(mask_);
	rel(trans_pso_);
	rel(clear_);
	rel(root_);
	release_blob(vs_);
	for (void *&b : ps_)
		release_blob(b);
	rel(srv_heap_);
	rel(rtv_heap_);
	ID3D12Resource **const all[] = { &frames_[0], &frames_[1], &stable_, &state_[0], &state_[1], &core_,
		&scenes_[0], &scenes_[1], &trans_[0], &trans_[1] };
	for (ID3D12Resource **r : all)
		rel(*r);
	trans_cur_ = 0;
	for (Retired &r : retired_) {
		rel(r.res);
		r.frames = 0;
	}
	format_ = DXGI_FORMAT_UNKNOWN;
	w_ = h_ = 0;
	targets_failed_ = false;
	pipeline_failed_ = false;
	ring_ = rtv_next_ = 0;
	cur_ = 0;
	reset();
}

}
