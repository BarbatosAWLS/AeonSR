#include "aeon_sr/core/frame_observer.hpp"

#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/dxgi_format_util.hpp"

#include <d3dcompiler.h>

#include <cstring>

namespace aeon_sr {

namespace {

enum : uint32_t {
	kPixels = 0, kInterface, kHeld, kFlickerInterface, kFlickerRest, kMoving, kMotionPixels,
	kOutInterface = 8, kOutRest = 10, kDrawnInterface = 12, kDrawnRest = 14, kMotionRest = 16,
};
constexpr double kFixed = 16.0;

constexpr const char kObserveHlsl[] = R"(
cbuffer C : register(b0)
{
	float2 size;
	float have_prev;
	float have_mask;
	float have_motion;
	float3 pad;
};

Texture2D<float4> drawn_now : register(t0);
Texture2D<float4> drawn_prev : register(t1);
Texture2D<float4> out_now : register(t2);
Texture2D<float4> out_prev : register(t3);
Texture2D<float> mask : register(t4);
Texture2D<float2> motion : register(t5);
RWByteAddressBuffer sums : register(u0);

groupshared uint g_count[7];
groupshared uint g_sum[5];

float luma(float3 c)
{
	return dot(saturate(c), float3(0.2126, 0.7152, 0.0722)) * 255.0;
}

void add64(uint word, uint v)
{
	if (v == 0u)
		return;
	uint was;
	sums.InterlockedAdd(word * 4u, v, was);
	if (was + v < was)
		sums.InterlockedAdd(word * 4u + 4u, 1u);
}

[numthreads(16, 1, 1)]
void CSClear(uint3 id : SV_DispatchThreadID)
{
	sums.Store(id.x * 4u, 0u);
	sums.Store((id.x + 16u) * 4u, 0u);
}

[numthreads(8, 8, 1)]
void CSObserve(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
	if (gi < 7u)
		g_count[gi] = 0u;
	if (gi < 5u)
		g_sum[gi] = 0u;
	GroupMemoryBarrierWithGroupSync();
	if (id.x < (uint)size.x && id.y < (uint)size.y && have_prev > 0.5) {
		const int3 p = int3(id.xy, 0);
		const float m = have_mask > 0.5 ? mask.Load(p) : 0.0;
		const bool hud = m > 0.75, held = m > 0.25 && !hud;
		const float dout = abs(luma(out_now.Load(p).rgb) - luma(out_prev.Load(p).rgb));
		const float ddrawn = abs(luma(drawn_now.Load(p).rgb) - luma(drawn_prev.Load(p).rgb));
		InterlockedAdd(g_count[0], 1u);
		if (hud)
			InterlockedAdd(g_count[1], 1u);
		if (held)
			InterlockedAdd(g_count[2], 1u);
		if (dout > 2.0)
			InterlockedAdd(g_count[hud ? 3 : 4], 1u);
		InterlockedAdd(g_sum[hud ? 0 : 1], (uint)(dout * 16.0 + 0.5));
		InterlockedAdd(g_sum[hud ? 2 : 3], (uint)(ddrawn * 16.0 + 0.5));
		if (!hud && have_motion > 0.5) {
			uint mw, mh;
			motion.GetDimensions(mw, mh);
			const int3 q = int3(min(uint2(float2(id.xy) * float2(mw, mh) / size), uint2(mw - 1u, mh - 1u)), 0);
			const float len = length(motion.Load(q) * size);
			InterlockedAdd(g_count[6], 1u);
			if (len > 0.25)
				InterlockedAdd(g_count[5], 1u);
			InterlockedAdd(g_sum[4], (uint)(min(len, 4096.0) * 16.0 + 0.5));
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (gi < 7u && g_count[gi] != 0u)
		sums.InterlockedAdd(gi * 4u, g_count[gi]);
	if (gi < 5u)
		add64(8u + gi * 2u, g_sum[gi]);
}
)";

ID3DBlob *compile(const char *entry)
{
	ID3DBlob *blob = nullptr, *err = nullptr;
	const HRESULT hr = D3DCompile(kObserveHlsl, sizeof(kObserveHlsl) - 1, "aeon_frame_observer", nullptr, nullptr, entry,
		"cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
	if (FAILED(hr)) {
		std::wstring what = L"frame observer: shader " + std::wstring(entry, entry + std::strlen(entry)) +
			L" did not compile";
		if (err != nullptr) {
			const char *text = static_cast<const char *>(err->GetBufferPointer());
			what += L": " + std::wstring(text, text + std::strlen(text));
		}
		diag_error("hud", what);
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

ID3D12Resource *make_buffer(ID3D12Device *device, uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
	D3D12_RESOURCE_STATES state)
{
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = heap;
	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	d.Width = bytes;
	d.Height = 1;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.SampleDesc.Count = 1;
	d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	d.Flags = flags;
	ID3D12Resource *r = nullptr;
	if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
		return nullptr;
	return r;
}

constexpr D3D12_RESOURCE_STATES kSrv =
	D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint32_t kBytes = FrameObserverD3D12::kCounters * 4u;

}

bool FrameObserverD3D12::ensure(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT format)
{
	if (failed_)
		return false;
	if (root_ == nullptr) {
		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 6;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 1;
		ranges[1].OffsetInDescriptorsFromTableStart = 6;
		D3D12_ROOT_PARAMETER rp[2]{};
		rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		rp[0].Constants.Num32BitValues = 8;
		rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rp[1].DescriptorTable.NumDescriptorRanges = 2;
		rp[1].DescriptorTable.pDescriptorRanges = ranges;
		D3D12_ROOT_SIGNATURE_DESC rs{};
		rs.NumParameters = 2;
		rs.pParameters = rp;
		ID3DBlob *sig = nullptr, *sig_err = nullptr;
		bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sig_err));
		rel(sig_err);
		ok = ok && SUCCEEDED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
			IID_PPV_ARGS(&root_)));
		rel(sig);
		ID3DBlob *cs[2] = { ok ? compile("CSObserve") : nullptr, ok ? compile("CSClear") : nullptr };
		ID3D12PipelineState **const outs[2] = { &observe_, &clear_ };
		for (uint32_t k = 0; k < 2; ++k) {
			D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
			cd.pRootSignature = root_;
			if (cs[k] != nullptr)
				cd.CS = { cs[k]->GetBufferPointer(), cs[k]->GetBufferSize() };
			ok = ok && cs[k] != nullptr && SUCCEEDED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(outs[k])));
		}
		rel(cs[0]);
		rel(cs[1]);
		D3D12_DESCRIPTOR_HEAP_DESC hd{};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = kSlots * kSlotDescriptors;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		ok = ok && SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)));
		sums_ = ok ? make_buffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS) : nullptr;
		readback_ = sums_ != nullptr ? make_buffer(device, kBytes * kReadback, D3D12_HEAP_TYPE_READBACK,
			D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST) : nullptr;
		void *map = nullptr;
		ok = readback_ != nullptr && SUCCEEDED(readback_->Map(0, nullptr, &map));
		if (!ok) {
			diag_error("hud", L"frame observer: its pipeline could not be made, the picture is not observed");
			release();
			failed_ = true;
			return false;
		}
		mapped_ = static_cast<uint32_t *>(map);
		stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}
	if (w == w_ && h == h_ && format == format_ && drawn_[0] != nullptr)
		return true;
	ID3D12Resource **const planes[] = { &drawn_[0], &drawn_[1], &out_[0], &out_[1] };
	for (ID3D12Resource **r : planes)
		rel(*r);
	w_ = w;
	h_ = h;
	format_ = format;
	have_prev_ = false;
	for (ID3D12Resource **r : planes) {
		if (!create_tex12(device, w, h, format, false, kSrv, r)) {
			for (ID3D12Resource **q : planes)
				rel(*q);
			w_ = h_ = 0;
			return false;
		}
	}
	return true;
}

void FrameObserverD3D12::collect() noexcept
{
	for (uint32_t k = 0; k < kReadback; ++k) {
		if (!written_[k] || ++age_[k] < kReadback - 1u)
			continue;
		written_[k] = false;
		const uint32_t *c = mapped_ + k * kCounters;
		const auto wide = [c](uint32_t at) {
			return (static_cast<double>(c[at + 1]) * 4294967296.0 + static_cast<double>(c[at])) / kFixed;
		};
		if (c[kPixels] == 0u)
			continue;
		++acc_.frames;
		acc_.pixels += c[kPixels];
		acc_.hud += c[kInterface];
		acc_.held += c[kHeld];
		acc_.out_flicker_interface += c[kFlickerInterface];
		acc_.out_flicker_rest += c[kFlickerRest];
		acc_.motion_moving += c[kMoving];
		acc_.motion_pixels += c[kMotionPixels];
		acc_.out_change_interface += wide(kOutInterface);
		acc_.out_change_rest += wide(kOutRest);
		acc_.drawn_change_interface += wide(kDrawnInterface);
		acc_.drawn_change_rest += wide(kDrawnRest);
		acc_.motion_rest += wide(kMotionRest);
	}
}

bool FrameObserverD3D12::begin(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
	D3D12_RESOURCE_STATES state)
{
	begun_ = false;
	if (device == nullptr || cmd == nullptr || color == nullptr)
		return false;
	const D3D12_RESOURCE_DESC d = color->GetDesc();
	if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count != 1 || d.MipLevels != 1 ||
		d.DepthOrArraySize != 1 || !ensure(device, static_cast<uint32_t>(d.Width), d.Height, d.Format))
		return false;
	collect();
	const uint32_t now = cur_ ^ 1u;
	barrier12(cmd, color, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier12(cmd, drawn_[now], kSrv, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->CopyResource(drawn_[now], color);
	barrier12(cmd, drawn_[now], D3D12_RESOURCE_STATE_COPY_DEST, kSrv);
	barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
	begun_ = true;
	return true;
}

void FrameObserverD3D12::end(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
	D3D12_RESOURCE_STATES state, ID3D12Resource *mask, D3D12_RESOURCE_STATES mask_state, ID3D12Resource *motion,
	D3D12_RESOURCE_STATES motion_state)
{
	if (!begun_ || device == nullptr || cmd == nullptr || color == nullptr)
		return;
	begun_ = false;
	const D3D12_RESOURCE_DESC d = color->GetDesc();
	if (static_cast<uint32_t>(d.Width) != w_ || d.Height != h_ || d.Format != format_)
		return;
	const uint32_t now = cur_ ^ 1u;
	barrier12(cmd, color, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier12(cmd, out_[now], kSrv, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->CopyResource(out_[now], color);
	barrier12(cmd, out_[now], D3D12_RESOURCE_STATE_COPY_DEST, kSrv);
	barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, state);

	if (mask != nullptr) {
		const D3D12_RESOURCE_DESC md = mask->GetDesc();
		if (static_cast<uint32_t>(md.Width) != w_ || md.Height != h_)
			mask = nullptr;
	}
	if (motion != nullptr) {
		const D3D12_RESOURCE_DESC vd = motion->GetDesc();
		if (vd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || vd.SampleDesc.Count != 1)
			motion = nullptr;
	}

	slot_ = (slot_ + 1u) % kSlots;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
	cpu.ptr += static_cast<SIZE_T>(slot_) * kSlotDescriptors * stride_;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
	gpu.ptr += static_cast<UINT64>(slot_) * kSlotDescriptors * stride_;
	ID3D12Resource *const srvs[6] = { drawn_[now], drawn_[cur_], out_[now], out_[cur_], mask, motion };
	const DXGI_FORMAT fallback[6] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16G16_FLOAT };
	for (uint32_t i = 0; i < 6; ++i) {
		D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = srvs[i] != nullptr ? view_format_for(srvs[i]->GetDesc().Format) : fallback[i];
		sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sd.Texture2D.MipLevels = 1;
		D3D12_CPU_DESCRIPTOR_HANDLE at = cpu;
		at.ptr += static_cast<SIZE_T>(i) * stride_;
		device->CreateShaderResourceView(srvs[i], &sd, at);
	}
	D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
	ud.Format = DXGI_FORMAT_R32_TYPELESS;
	ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	ud.Buffer.NumElements = kCounters;
	ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	D3D12_CPU_DESCRIPTOR_HANDLE at = cpu;
	at.ptr += static_cast<SIZE_T>(6) * stride_;
	device->CreateUnorderedAccessView(sums_, nullptr, &ud, at);

	const float c[8] = { static_cast<float>(w_), static_cast<float>(h_), have_prev_ ? 1.0f : 0.0f,
		mask != nullptr ? 1.0f : 0.0f, motion != nullptr ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
	if (mask != nullptr)
		barrier12(cmd, mask, mask_state, kSrv);
	if (motion != nullptr)
		barrier12(cmd, motion, motion_state, kSrv);
	cmd->SetComputeRootSignature(root_);
	cmd->SetDescriptorHeaps(1, &heap_);
	cmd->SetComputeRoot32BitConstants(0, 8, c, 0);
	cmd->SetComputeRootDescriptorTable(1, gpu);
	D3D12_RESOURCE_BARRIER ub{};
	ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	ub.UAV.pResource = sums_;
	cmd->SetPipelineState(clear_);
	cmd->Dispatch(1u, 1u, 1u);
	cmd->ResourceBarrier(1, &ub);
	cmd->SetPipelineState(observe_);
	cmd->Dispatch((w_ + 7u) / 8u, (h_ + 7u) / 8u, 1u);
	cmd->ResourceBarrier(1, &ub);
	if (mask != nullptr)
		barrier12(cmd, mask, kSrv, mask_state);
	if (motion != nullptr)
		barrier12(cmd, motion, kSrv, motion_state);

	barrier12(cmd, sums_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	ring_ = (ring_ + 1u) % kReadback;
	cmd->CopyBufferRegion(readback_, static_cast<UINT64>(ring_) * kBytes, sums_, 0, kBytes);
	barrier12(cmd, sums_, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	written_[ring_] = have_prev_;
	age_[ring_] = 0;
	cur_ = now;
	have_prev_ = true;
}

FrameObservation FrameObserverD3D12::take() noexcept
{
	const FrameObservation r = acc_;
	acc_ = FrameObservation{};
	return r;
}

void FrameObserverD3D12::release() noexcept
{
	if (readback_ != nullptr && mapped_ != nullptr)
		readback_->Unmap(0, nullptr);
	mapped_ = nullptr;
	ID3D12Resource **const all[] = { &drawn_[0], &drawn_[1], &out_[0], &out_[1], &sums_, &readback_ };
	for (ID3D12Resource **r : all)
		rel(*r);
	rel(observe_);
	rel(clear_);
	rel(root_);
	rel(heap_);
	for (uint32_t k = 0; k < kReadback; ++k)
		written_[k] = false;
	w_ = h_ = 0;
	format_ = DXGI_FORMAT_UNKNOWN;
	have_prev_ = begun_ = failed_ = false;
	acc_ = FrameObservation{};
}

}
