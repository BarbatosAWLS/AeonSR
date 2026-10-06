#include "aeon_sr/depth/depth_normalize.hpp"

#include <d3dcompiler.h>

#include <cstring>

namespace aeon_sr {
namespace {

template <class T>
void release(T *&p) noexcept
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

constexpr uint32_t kRingSize = 256;
constexpr uint32_t kTableSize = 2;

const char *kHlsl = R"HLSL(
cbuffer Params : register(b0)
{
	uint2  dst;
	float2 inv_dst;
	uint   upside_down;
	uint   mirrored;
	uint   reversed;
	uint   logarithmic;
	float  multiplier;
	float  x_scale;
	float  y_scale;
	float  x_offset;
	float  y_offset;
	float3 pad;
	float4 valid;
};

Texture2D<float>    Src : register(t0);
RWTexture2D<float>  Out : register(u0);
SamplerState LinearClamp : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= dst))
		return;

	float2 uv = (float2(id.xy) + 0.5) * inv_dst;
	if (valid.z > valid.x)
		uv = clamp(uv, valid.xy, valid.zw);
	if (upside_down != 0u)
		uv.y = 1.0 - uv.y;
	if (mirrored != 0u)
		uv.x = 1.0 - uv.x;
	uv.x /= x_scale;
	uv.y /= y_scale;
	uv.x -= x_offset / 2.000000001;
	uv.y += y_offset / 2.000000001;

	float d = Src.SampleLevel(LinearClamp, uv, 0) * multiplier;

	if (logarithmic != 0u) {
		const float C = 0.01;
		d = (exp(d * log(C + 1.0)) - 1.0) / C;
	}
	if (reversed != 0u)
		d = 1.0 - d;

	Out[id.xy] = saturate(d);
}
)HLSL";

}

DepthNormalizeD3D12::~DepthNormalizeD3D12()
{
	release();
}

void DepthNormalizeD3D12::release_texture()
{
	::aeon_sr::release(out_);
	width_ = height_ = 0;
	out_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

void DepthNormalizeD3D12::release()
{
	release_texture();
	::aeon_sr::release(heap_);
	::aeon_sr::release(pso_);
	::aeon_sr::release(root_);
	device_ = nullptr;
	ring_ = 0;
}

bool DepthNormalizeD3D12::ensure(ID3D12Device *device, uint32_t width, uint32_t height,
	std::wstring *error)
{
	if (device == nullptr || width == 0 || height == 0)
		return false;
	if (device_ == device && width_ == width && height_ == height && out_ != nullptr)
		return true;
	if (device_ == device && root_ != nullptr) {
		release_texture();
		width_ = width;
		height_ = height;
		return make_output(error);
	}
	release();
	device_ = device;
	width_ = width;
	height_ = height;
	stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = 1;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = 1;
	ranges[1].OffsetInDescriptorsFromTableStart = 1;

	D3D12_ROOT_PARAMETER params[2]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.Num32BitValues = sizeof(Constants) / 4;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 2;
	params[1].DescriptorTable.pDescriptorRanges = ranges;

	D3D12_STATIC_SAMPLER_DESC sampler{};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;

	D3D12_ROOT_SIGNATURE_DESC rs{};
	rs.NumParameters = 2;
	rs.pParameters = params;
	rs.NumStaticSamplers = 1;
	rs.pStaticSamplers = &sampler;

	ID3DBlob *blob = nullptr, *err = nullptr;
	if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
		FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
			IID_PPV_ARGS(&root_)))) {
		::aeon_sr::release(blob);
		::aeon_sr::release(err);
		if (error != nullptr)
			*error = L"depth: root signature failed";
		release();
		return false;
	}
	::aeon_sr::release(blob);
	::aeon_sr::release(err);

	ID3DBlob *cs = nullptr, *cerr = nullptr;
	if (FAILED(D3DCompile(kHlsl, std::strlen(kHlsl), "aeon_depth", nullptr, nullptr, "main",
			"cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &cerr))) {
		::aeon_sr::release(cs);
		::aeon_sr::release(cerr);
		if (error != nullptr)
			*error = L"depth: shader failed to compile";
		release();
		return false;
	}
	::aeon_sr::release(cerr);
	D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
	pd.pRootSignature = root_;
	pd.CS.pShaderBytecode = cs->GetBufferPointer();
	pd.CS.BytecodeLength = cs->GetBufferSize();
	const HRESULT pso_ok = device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_));
	::aeon_sr::release(cs);
	if (FAILED(pso_ok)) {
		if (error != nullptr)
			*error = L"depth: pipeline state failed";
		release();
		return false;
	}

	D3D12_DESCRIPTOR_HEAP_DESC hd{};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = kRingSize;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) {
		if (error != nullptr)
			*error = L"depth: descriptor heap failed";
		release();
		return false;
	}

	return make_output(error);
}

bool DepthNormalizeD3D12::make_output(std::wstring *error)
{
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC td{};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = width_;
	td.Height = height_;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R32_FLOAT;
	td.SampleDesc.Count = 1;
	td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out_)))) {
		if (error != nullptr)
			*error = L"depth: could not create the normalised buffer";
		release();
		return false;
	}
	out_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	return true;
}

bool DepthNormalizeD3D12::record(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src,
	D3D12_RESOURCE_STATES src_state, const DepthConvention &how, std::wstring *error, const float *valid_uv)
{
	if (cmd == nullptr || src == nullptr || !ready()) {
		if (error != nullptr)
			*error = L"depth: not initialised";
		return false;
	}

	const D3D12_RESOURCE_DESC sd = src->GetDesc();
	DXGI_FORMAT read_as = sd.Format;
	switch (sd.Format) {
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT: read_as = DXGI_FORMAT_R32_FLOAT; break;
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT: read_as = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_D16_UNORM: read_as = DXGI_FORMAT_R16_UNORM; break;
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: read_as = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
	default: break;
	}
	if ((sd.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
		if (error != nullptr)
			*error = L"depth: the game's buffer cannot be read by a shader";
		return false;
	}

	if (ring_ + kTableSize > kRingSize)
		ring_ = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += static_cast<SIZE_T>(ring_) * stride_;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
	gpu.ptr += static_cast<UINT64>(ring_) * stride_;
	ring_ += kTableSize;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.Format = read_as;
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	device_->CreateShaderResourceView(src, &srv, cpu);

	D3D12_CPU_DESCRIPTOR_HANDLE uav_at = cpu;
	uav_at.ptr += stride_;
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.Format = DXGI_FORMAT_R32_FLOAT;
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	device_->CreateUnorderedAccessView(out_, nullptr, &uav, uav_at);

	const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	const D3D12_RESOURCE_STATES src_read = (src_state & read) == read ? src_state : read;
	const auto barrier = [cmd](ID3D12Resource *res, D3D12_RESOURCE_STATES a,
			D3D12_RESOURCE_STATES b) {
		if (a == b)
			return;
		D3D12_RESOURCE_BARRIER t{};
		t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		t.Transition.pResource = res;
		t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		t.Transition.StateBefore = a;
		t.Transition.StateAfter = b;
		cmd->ResourceBarrier(1, &t);
	};

	barrier(src, src_state, src_read);
	barrier(out_, out_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	out_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	Constants c{};
	c.dst_w = width_;
	c.dst_h = height_;
	c.inv_dst_x = 1.0f / static_cast<float>(width_);
	c.inv_dst_y = 1.0f / static_cast<float>(height_);
	c.upside_down = how.upside_down ? 1u : 0u;
	c.mirrored = how.mirrored ? 1u : 0u;
	c.reversed = how.reversed ? 1u : 0u;
	c.logarithmic = how.logarithmic ? 1u : 0u;
	c.multiplier = how.multiplier;
	c.x_scale = how.x_scale != 0.0f ? how.x_scale : 1.0f;
	c.y_scale = how.y_scale != 0.0f ? how.y_scale : 1.0f;
	c.x_offset = depth_x_offset(how, width_);
	c.y_offset = depth_y_offset(how, height_);
	for (int i = 0; i < 4; ++i)
		c.valid[i] = valid_uv != nullptr ? valid_uv[i] : 0.0f;

	ID3D12DescriptorHeap *heaps[] = { heap_ };
	cmd->SetDescriptorHeaps(1, heaps);
	cmd->SetComputeRootSignature(root_);
	cmd->SetPipelineState(pso_);
	cmd->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
	cmd->SetComputeRootDescriptorTable(1, gpu);
	cmd->Dispatch((width_ + 7u) / 8u, (height_ + 7u) / 8u, 1);

	barrier(out_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kPublished);
	out_state_ = kPublished;
	barrier(src, src_read, src_state);
	return true;
}

}
