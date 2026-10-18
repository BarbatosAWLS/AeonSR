#include "aeon_sr/motion/game_jitter_probe.hpp"
#include "aeon_sr/core/com_release.hpp"

#include <d3dcompiler.h>

#include <cmath>
#include <cstring>

namespace aeon_sr {
namespace {

constexpr uint32_t kSums = 28u;
constexpr uint32_t kResultUints = 30u;
constexpr uint32_t kTable = 9u;
constexpr uint32_t kHeap = 9u * 64u;

const char *kHlsl = R"HLSL(
cbuffer P : register(b0)
{
	uint2 size;
	uint groups_x;
	uint groups;
	uint serial;
	uint first;
	uint2 pad;
};

Texture2D<float> Dcur : register(t0);
Texture2D<float4> Ccur : register(t1);
Texture2D<float2> Mv : register(t2);
RWTexture2D<float> Dprev : register(u0);
RWTexture2D<float> Lprev : register(u1);
RWStructuredBuffer<float> Blocks : register(u2);
RWStructuredBuffer<uint> Result : register(u3);
RWTexture2D<float> Dnext : register(u4);
RWTexture2D<float> Lnext : register(u5);

#define K 28
#define PARTS 4
#define PART (K / PARTS)
groupshared float gs[PART * 256];
groupshared float gsum[K];

float luma_at(int2 p) { return dot(Ccur[p].rgb, float3(0.2126, 0.7152, 0.0722)); }

void reduce_group(uint gi, float v[K])
{
	[unroll] for (uint part = 0; part < PARTS; ++part) {
		[unroll] for (uint e = 0; e < PART; ++e)
			gs[e * 256u + gi] = v[part * PART + e];
		GroupMemoryBarrierWithGroupSync();
		for (uint s = 128u; s > 0u; s >>= 1) {
			for (uint j = gi; j < s * PART; j += 256u) {
				const uint e2 = j / s, i = j - e2 * s;
				gs[e2 * 256u + i] += gs[e2 * 256u + i + s];
			}
			GroupMemoryBarrierWithGroupSync();
		}
		if (gi < PART)
			gsum[part * PART + gi] = gs[gi * 256u];
		GroupMemoryBarrierWithGroupSync();
	}
}

void add_pixel(uint2 xy, inout float v[K])
{
	const int2 p = int2(xy);
	const bool inside = all(xy < size);
	float z = 0.0, l = 0.0;
	if (inside) {
		z = Dcur[p];
		l = luma_at(p);
	}
	if (inside && first == 0u && all(p >= 2) && all(p + 2 < int2(size))) {
		const float zl = Dcur[p - int2(1, 0)], zr = Dcur[p + int2(1, 0)];
		const float zu = Dcur[p - int2(0, 1)], zd = Dcur[p + int2(0, 1)];
		const float zp = Dprev[p];
		const float gx = 0.5 * (zr - zl), gy = 0.5 * (zd - zu);
		const float gm = sqrt(gx * gx + gy * gy);
		const bool planar = abs(zr - 2.0 * z + zl) <= 0.05 * abs(gx) + 1e-7 &&
			abs(zd - 2.0 * z + zu) <= 0.05 * abs(gy) + 1e-7;
		const bool live = z > 0.0 && z < 0.9999 && zp > 0.0 && zp < 0.9999 && gm > 1e-7;
		if (planar && live) {
			const float d = (z - zp) / gm;
			if (abs(d) <= 2.0) {
				const float ux = gx / gm, uy = gy / gm;
				v[0] += ux * ux; v[1] += ux * uy; v[2] += uy * uy; v[3] += ux * d; v[4] += uy * d; v[5] += 1.0;
			}
		}
		const float ll = luma_at(p - int2(1, 0)), lr = luma_at(p + int2(1, 0));
		const float lu = luma_at(p - int2(0, 1)), ld = luma_at(p + int2(0, 1));
		const float lp = Lprev[p];
		const float hx = 0.5 * (lr - ll), hy = 0.5 * (ld - lu);
		const float hm = sqrt(hx * hx + hy * hy);
		if (hm > 0.01) {
			const float d = (l - lp) / hm;
			if (abs(d) <= 2.0) {
				const float ux = hx / hm, uy = hy / hm;
				v[6] += ux * ux; v[7] += ux * uy; v[8] += uy * uy; v[9] += ux * d; v[10] += uy * d; v[11] += 1.0;
			}
		}
		const float2 mv = Mv[p] * float2(size);
		const float ml = length(mv);
		const float2 n = ml > 0.25 ? float2(-mv.y, mv.x) / ml : float2(0.0, 0.0);
		const float2 q = float2(p) + mv;
		if (all(q >= 0.0) && all(q <= float2(size) - 1.001)) {
			const int2 q0 = int2(floor(q));
			const float2 fq = q - float2(q0);
			const float zw = lerp(lerp(Dprev[q0], Dprev[q0 + int2(1, 0)], fq.x),
				lerp(Dprev[q0 + int2(0, 1)], Dprev[q0 + int2(1, 1)], fq.x), fq.y);
			const float lw = lerp(lerp(Lprev[q0], Lprev[q0 + int2(1, 0)], fq.x),
				lerp(Lprev[q0 + int2(0, 1)], Lprev[q0 + int2(1, 1)], fq.x), fq.y);
			if (planar && z > 0.0 && z < 0.9999 && zw > 0.0 && zw < 0.9999 && gm > 1e-7) {
				const float d = (z - zw) / gm;
				if (abs(d) <= 2.0) {
					const float ux = gx / gm, uy = gy / gm;
					const float w = ml > 0.25 ? (ux * n.x + uy * n.y) * (ux * n.x + uy * n.y) : 1.0;
					v[14] += w * ux * ux; v[15] += w * ux * uy; v[16] += w * uy * uy; v[17] += w * ux * d; v[18] += w * uy * d;
					v[19] += w;
				}
			}
			if (hm > 0.01) {
				const float d = (l - lw) / hm;
				if (abs(d) <= 2.0) {
					const float ux = hx / hm, uy = hy / hm;
					const float w = ml > 0.25 ? (ux * n.x + uy * n.y) * (ux * n.x + uy * n.y) : 1.0;
					v[20] += w * ux * ux; v[21] += w * ux * uy; v[22] += w * uy * uy; v[23] += w * ux * d; v[24] += w * uy * d;
					v[25] += w;
				}
			}
		}
	}
	if (inside) {
		const float2 m = Mv[p] * float2(size);
		v[12] += length(m);
		v[13] += 1.0;
		v[26] += -m.x;
		v[27] += -m.y;
	}
	if (inside) {
		Dnext[p] = z;
		Lnext[p] = l;
	}
}

[numthreads(16, 16, 1)]
void Accumulate(uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex, uint3 gid : SV_GroupID)
{
	float v[K];
	[unroll] for (uint k = 0; k < K; ++k)
		v[k] = 0.0;
	const uint2 base = gid.xy * 32u + gtid.xy;
	[loop] for (uint t = 0; t < 4u; ++t)
		add_pixel(base + uint2(t & 1u, t >> 1u) * 16u, v);
	reduce_group(gi, v);
	if (gi == 0u) {
		const uint at = (gid.y * groups_x + gid.x) * K;
		[unroll] for (uint k = 0; k < K; ++k)
			Blocks[at + k] = gsum[k];
	}
}

[numthreads(256, 1, 1)]
void Reduce(uint gi : SV_GroupIndex)
{
	float v[K];
	[unroll] for (uint k = 0; k < K; ++k)
		v[k] = 0.0;
	for (uint g = gi; g < groups; g += 256u) {
		[unroll] for (uint k = 0; k < K; ++k)
			v[k] += Blocks[g * K + k];
	}
	reduce_group(gi, v);
	if (gi == 0u) {
		[unroll] for (uint k = 0; k < K; ++k)
			Result[k] = asuint(gsum[k]);
		Result[K] = serial;
	}
}
)HLSL";

DXGI_FORMAT readable(DXGI_FORMAT f) noexcept
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
	default: return f;
	}
}

void barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
	if (a == b)
		return;
	D3D12_RESOURCE_BARRIER t{};
	t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	t.Transition.pResource = r;
	t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	t.Transition.StateBefore = a;
	t.Transition.StateAfter = b;
	cmd->ResourceBarrier(1, &t);
}

void uav_barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *r)
{
	D3D12_RESOURCE_BARRIER t{};
	t.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	t.UAV.pResource = r;
	cmd->ResourceBarrier(1, &t);
}

ID3D12Resource *make_buffer(ID3D12Device *dev, UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
	D3D12_RESOURCE_FLAGS flags)
{
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = type;
	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	d.Width = bytes;
	d.Height = 1;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.SampleDesc.Count = 1;
	d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	d.Flags = flags;
	ID3D12Resource *out = nullptr;
	if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out))))
		return nullptr;
	return out;
}

ID3D12Resource *make_texture(ID3D12Device *dev, uint32_t w, uint32_t h, DXGI_FORMAT f)
{
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = f;
	d.SampleDesc.Count = 1;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	ID3D12Resource *out = nullptr;
	if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			nullptr, IID_PPV_ARGS(&out))))
		return nullptr;
	return out;
}

constexpr double kRidge = 0.05;
bool solve_carried(const float s[6], uint32_t min_weight, float *tx, float *ty)
{
	const double n = s[5];
	if (!(n >= static_cast<double>(min_weight)) || !std::isfinite(static_cast<double>(s[0]) + s[1] + s[2] + s[3] + s[4]))
		return false;
	const double xx = s[0] + kRidge * n, xy = s[1], yy = s[2] + kRidge * n, xd = s[3], yd = s[4];
	const double det = xx * yy - xy * xy;
	if (!(det > 0.0))
		return false;
	*tx = static_cast<float>(-(yy * xd - xy * yd) / det);
	*ty = static_cast<float>(-(xx * yd - xy * xd) / det);
	return std::isfinite(*tx) && std::isfinite(*ty);
}

struct Constants {
	uint32_t w, h, groups_x, groups, serial, first, pad0, pad1;
};

}

void GameJitterProbeD3D12::release()
{
	if (readback_ != nullptr && mapped_ != nullptr)
		readback_->Unmap(0, nullptr);
	mapped_ = nullptr;
	safe_release(readback_);
	safe_release(result_);
	safe_release(partials_);
	for (int i = 0; i < 2; ++i) {
		safe_release(prev_luma_[i]);
		safe_release(prev_depth_[i]);
	}
	safe_release(heap_);
	safe_release(reduce_);
	safe_release(accumulate_);
	safe_release(root_);
	device_ = nullptr;
	width_ = height_ = groups_x_ = groups_y_ = 0;
	reset();
}

void GameJitterProbeD3D12::reset() noexcept
{
	first_valid_ = serial_ + 2u;
	taken_ = serial_;
}

bool GameJitterProbeD3D12::ensure(ID3D12Device *device, uint32_t width, uint32_t height, std::wstring *error)
{
	if (device == nullptr || width == 0 || height == 0)
		return false;
	if (device == device_ && width == width_ && height == height_ && prev_depth_[0] != nullptr)
		return true;
	if (device != device_ || root_ == nullptr) {
		release();
		device_ = device;
		stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 3;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 6;
		ranges[1].OffsetInDescriptorsFromTableStart = 3;
		D3D12_ROOT_PARAMETER params[2]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.Num32BitValues = sizeof(Constants) / 4;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 2;
		params[1].DescriptorTable.pDescriptorRanges = ranges;
		D3D12_ROOT_SIGNATURE_DESC rs{};
		rs.NumParameters = 2;
		rs.pParameters = params;
		ID3DBlob *blob = nullptr, *err = nullptr;
		const bool rs_ok = SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) &&
			SUCCEEDED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
				IID_PPV_ARGS(&root_)));
		safe_release(blob);
		safe_release(err);
		bool ok = rs_ok;
		std::wstring why;
		for (int k = 0; ok && k < 2; ++k) {
			ID3DBlob *cs = nullptr, *cerr = nullptr;
			ok = SUCCEEDED(D3DCompile(kHlsl, std::strlen(kHlsl), "aeon_game_jitter", nullptr, nullptr,
				k == 0 ? "Accumulate" : "Reduce", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &cerr));
			if (!ok && error != nullptr && cerr != nullptr) {
				const char *msg = static_cast<const char *>(cerr->GetBufferPointer());
				why.assign(msg, msg + std::strlen(msg));
			}
			safe_release(cerr);
			if (ok) {
				D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
				pd.pRootSignature = root_;
				pd.CS.pShaderBytecode = cs->GetBufferPointer();
				pd.CS.BytecodeLength = cs->GetBufferSize();
				ok = SUCCEEDED(device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(k == 0 ? &accumulate_ : &reduce_)));
			}
			safe_release(cs);
		}
		D3D12_DESCRIPTOR_HEAP_DESC hd{};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = kHeap;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		ok = ok && SUCCEEDED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)));
		result_ = ok ? make_buffer(device_, kResultUints * 4u, D3D12_HEAP_TYPE_DEFAULT,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) : nullptr;
		readback_ = ok ? make_buffer(device_, kResultUints * 4u * kRing, D3D12_HEAP_TYPE_READBACK,
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE) : nullptr;
		ok = ok && result_ != nullptr && readback_ != nullptr &&
			SUCCEEDED(readback_->Map(0, nullptr, reinterpret_cast<void **>(&mapped_)));
		if (!ok) {
			if (error != nullptr)
				*error = L"game jitter probe: the pipeline could not be made" + (why.empty() ? std::wstring() : L": " + why);
			release();
			return false;
		}
		std::memset(mapped_, 0xFF, kResultUints * 4u * kRing);
	}
	for (int i = 0; i < 2; ++i) {
		safe_release(prev_luma_[i]);
		safe_release(prev_depth_[i]);
	}
	safe_release(partials_);
	width_ = width;
	height_ = height;
	groups_x_ = (width + 31u) / 32u;
	groups_y_ = (height + 31u) / 32u;
	bool made = true;
	for (int i = 0; i < 2; ++i) {
		prev_depth_[i] = make_texture(device_, width, height, DXGI_FORMAT_R32_FLOAT);
		prev_luma_[i] = make_texture(device_, width, height, DXGI_FORMAT_R32_FLOAT);
		made = made && prev_depth_[i] != nullptr && prev_luma_[i] != nullptr;
	}
	partials_ = make_buffer(device_, static_cast<UINT64>(groups_x_) * groups_y_ * kSums * 4u, D3D12_HEAP_TYPE_DEFAULT,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	if (!made || partials_ == nullptr) {
		if (error != nullptr)
			*error = L"game jitter probe: its textures could not be made";
		release();
		return false;
	}
	reset();
	return true;
}

bool GameJitterProbeD3D12::record(ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
	D3D12_RESOURCE_STATES color_state, ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state,
	ID3D12Resource *motion, D3D12_RESOURCE_STATES motion_state, std::wstring *error)
{
	if (cmd == nullptr || color == nullptr || depth == nullptr || motion == nullptr || prev_depth_[0] == nullptr) {
		if (error != nullptr)
			*error = L"game jitter probe: not ready";
		return false;
	}
	const uint64_t serial = ++serial_;
	if (ring_slot_ + kTable > kHeap)
		ring_slot_ = 0;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += static_cast<SIZE_T>(ring_slot_) * stride_;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
	gpu.ptr += static_cast<UINT64>(ring_slot_) * stride_;
	ring_slot_ += kTable;

	D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
	srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2D.MipLevels = 1;
	srv.Format = readable(depth->GetDesc().Format);
	D3D12_CPU_DESCRIPTOR_HANDLE at = cpu;
	device_->CreateShaderResourceView(depth, &srv, at);
	at.ptr += stride_;
	srv.Format = readable(color->GetDesc().Format);
	device_->CreateShaderResourceView(color, &srv, at);
	at.ptr += stride_;
	srv.Format = DXGI_FORMAT_R16G16_FLOAT;
	device_->CreateShaderResourceView(motion, &srv, at);
	at.ptr += stride_;
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
	uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	uav.Format = DXGI_FORMAT_R32_FLOAT;
	const uint32_t rd = static_cast<uint32_t>(serial & 1u), wr = rd ^ 1u;
	device_->CreateUnorderedAccessView(prev_depth_[rd], nullptr, &uav, at);
	at.ptr += stride_;
	device_->CreateUnorderedAccessView(prev_luma_[rd], nullptr, &uav, at);
	at.ptr += stride_;
	D3D12_UNORDERED_ACCESS_VIEW_DESC buf{};
	buf.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	buf.Format = DXGI_FORMAT_UNKNOWN;
	buf.Buffer.StructureByteStride = 4;
	buf.Buffer.NumElements = groups_x_ * groups_y_ * kSums;
	device_->CreateUnorderedAccessView(partials_, nullptr, &buf, at);
	at.ptr += stride_;
	buf.Buffer.NumElements = kResultUints;
	device_->CreateUnorderedAccessView(result_, nullptr, &buf, at);
	at.ptr += stride_;
	device_->CreateUnorderedAccessView(prev_depth_[wr], nullptr, &uav, at);
	at.ptr += stride_;
	device_->CreateUnorderedAccessView(prev_luma_[wr], nullptr, &uav, at);

	const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	const D3D12_RESOURCE_STATES color_read = (color_state & read) == read ? color_state : read;
	const D3D12_RESOURCE_STATES depth_read = (depth_state & read) == read ? depth_state : read;
	const D3D12_RESOURCE_STATES motion_read = (motion_state & read) == read ? motion_state : read;
	barrier(cmd, color, color_state, color_read);
	barrier(cmd, depth, depth_state, depth_read);
	barrier(cmd, motion, motion_state, motion_read);

	Constants c{};
	c.w = width_;
	c.h = height_;
	c.groups_x = groups_x_;
	c.groups = groups_x_ * groups_y_;
	c.serial = static_cast<uint32_t>(serial);
	c.first = serial < first_valid_ ? 1u : 0u;
	ID3D12DescriptorHeap *heaps[] = { heap_ };
	cmd->SetDescriptorHeaps(1, heaps);
	cmd->SetComputeRootSignature(root_);
	cmd->SetComputeRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
	cmd->SetComputeRootDescriptorTable(1, gpu);
	cmd->SetPipelineState(accumulate_);
	cmd->Dispatch(groups_x_, groups_y_, 1);
	uav_barrier(cmd, partials_);
	cmd->SetPipelineState(reduce_);
	cmd->Dispatch(1, 1, 1);
	barrier(cmd, result_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmd->CopyBufferRegion(readback_, static_cast<UINT64>(serial % kRing) * kResultUints * 4u, result_, 0,
		kResultUints * 4u);
	barrier(cmd, result_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	barrier(cmd, motion, motion_read, motion_state);
	barrier(cmd, depth, depth_read, depth_state);
	barrier(cmd, color, color_read, color_state);
	return true;
}

bool GameJitterProbeD3D12::take(GameJitterFit *out) noexcept
{
	if (mapped_ == nullptr || serial_ <= kLatency)
		return false;
	const uint64_t want = serial_ - kLatency;
	if (want <= taken_ || want < first_valid_)
		return false;
	taken_ = want;
	uint32_t r[kResultUints];
	std::memcpy(r, mapped_ + (want % kRing) * kResultUints * 4u, sizeof(r));
	if (r[kSums] != static_cast<uint32_t>(want))
		return false;
	float s[kSums];
	std::memcpy(s, r, sizeof(s));
	GameJitterFit f;
	f.serial = want;
	f.depth_pixels = static_cast<uint32_t>(s[5] > 0.0f ? s[5] : 0.0f);
	f.color_pixels = static_cast<uint32_t>(s[11] > 0.0f ? s[11] : 0.0f);
	f.depth_ok = solve_shift(s, kMinPixels, &f.depth_x, &f.depth_y);
	f.color_ok = solve_shift(s + 6, kMinPixels, &f.color_x, &f.color_y);
	f.motion_px = s[13] > 0.0f ? s[12] / s[13] : -1.0f;
	f.carried_depth_ok = solve_carried(s + 14, kMinPixels / 4u, &f.carried_depth_x, &f.carried_depth_y);
	f.carried_color_ok = solve_carried(s + 20, kMinPixels / 4u, &f.carried_color_x, &f.carried_color_y);
	f.carried_depth_pixels = static_cast<uint32_t>(s[19] > 0.0f ? s[19] : 0.0f);
	f.carried_color_pixels = static_cast<uint32_t>(s[25] > 0.0f ? s[25] : 0.0f);
	f.motion_x = s[13] > 0.0f ? s[26] / s[13] : 0.0f;
	f.motion_y = s[13] > 0.0f ? s[27] / s[13] : 0.0f;
	*out = f;
	return true;
}

}
