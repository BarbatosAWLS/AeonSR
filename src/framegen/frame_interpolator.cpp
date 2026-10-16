#include "aeon_sr/framegen/frame_interpolator.hpp"

#include "aeon_sr/core/dxgi_format_util.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"

#include "frame_interpolator_shaders.inl"

#include <d3dcompiler.h>

#include <cstring>
#include <initializer_list>
#include <utility>

namespace aeon_sr {

namespace {

constexpr uint32_t kStatStride = 8;
constexpr uint32_t kGroup = 8;
constexpr uint32_t kSearchSpan = 13;
constexpr uint32_t kTile = 8;
constexpr uint32_t kConstantCount = 12;

const char *const kEntries[] = { "StatsMain", "SearchMain", "ReduceMain", "RefineMain", "LayersMain", "InterpolateMain" };

struct Constants {
	uint32_t width, height;
	float inv_width, inv_height;
	float t;
	uint32_t has_depth;
	uint32_t stat_width, stat_height;
	uint32_t tile_width, tile_height;
	uint32_t encode_srgb;
	uint32_t is_float;
};
static_assert(sizeof(Constants) == kConstantCount * 4, "root constants and the HLSL cbuffer disagree");

uint32_t groups(uint32_t extent, uint32_t per) noexcept
{
	return (extent + per - 1) / per;
}

template <typename T>
void rel(T *&p) noexcept
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

bool is_srgb(DXGI_FORMAT f) noexcept
{
	return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
		f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

bool is_float_family(DXGI_FORMAT family) noexcept
{
	return family == DXGI_FORMAT_R16G16B16A16_TYPELESS || family == DXGI_FORMAT_R32G32B32A32_TYPELESS;
}

std::wstring widen_ascii(const char *s, size_t n)
{
	std::wstring w;
	w.reserve(n);
	for (size_t i = 0; i < n && s[i] != 0; ++i)
		w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(s[i])));
	return w;
}

void srv_2d(ID3D12Device *device, ID3D12Resource *res, DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE at)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC d{};
	d.Format = fmt;
	d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	d.Texture2D.MipLevels = 1;
	device->CreateShaderResourceView(res, &d, at);
}

void uav_2d(ID3D12Device *device, ID3D12Resource *res, DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE at)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
	d.Format = fmt;
	d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	device->CreateUnorderedAccessView(res, nullptr, &d, at);
}

}

DXGI_FORMAT interpolator_read_format(DXGI_FORMAT f) noexcept
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	default: return f;
	}
}

DXGI_FORMAT interpolator_write_format(DXGI_FORMAT f) noexcept
{
	switch (format_family(f)) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

FrameInterpolatorD3D12::~FrameInterpolatorD3D12()
{
	release();
}

void FrameInterpolatorD3D12::release() noexcept
{
	if (builder_.joinable())
		builder_.join();
	release_textures();
	for (ID3D12PipelineState *&p : pso_)
		rel(p);
	rel(root_);
	rel(heap_);
	device_ = nullptr;
	state_.store(kIdle, std::memory_order_release);
	build_error_.clear();
}

bool FrameInterpolatorD3D12::copies_into(DXGI_FORMAT f) const noexcept
{
	return format_family(f) == family_;
}

void FrameInterpolatorD3D12::release_textures() noexcept
{
	ID3D12Resource **const all[] = { &output_, &tile_stats_, &search_errors_, &summary_, &refined_, &layers_,
		&layer_depth_ };
	for (ID3D12Resource **r : all)
		rel(*r);
	w_ = h_ = 0;
	color_format_ = family_ = input_family_ = uav_format_ = DXGI_FORMAT_UNKNOWN;
	textures_failed_ = false;
}

void FrameInterpolatorD3D12::build_kernels(ID3D12Device *device)
{
	std::wstring error;
	bool ok = true;

	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = kSrvCount;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = kUavCount;
	D3D12_ROOT_PARAMETER rp[3]{};
	rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rp[0].Constants.Num32BitValues = kConstantCount;
	rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rp[1].DescriptorTable.NumDescriptorRanges = 1;
	rp[1].DescriptorTable.pDescriptorRanges = &ranges[0];
	rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rp[2].DescriptorTable.NumDescriptorRanges = 1;
	rp[2].DescriptorTable.pDescriptorRanges = &ranges[1];
	D3D12_STATIC_SAMPLER_DESC samp{};
	samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	samp.MaxLOD = D3D12_FLOAT32_MAX;
	D3D12_ROOT_SIGNATURE_DESC rs{};
	rs.NumParameters = 3;
	rs.pParameters = rp;
	rs.NumStaticSamplers = 1;
	rs.pStaticSamplers = &samp;
	ID3DBlob *sig = nullptr, *sig_err = nullptr;
	ID3D12RootSignature *root = nullptr;
	ok = SUCCEEDED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sig_err)) &&
		SUCCEEDED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root)));
	rel(sig);
	rel(sig_err);
	if (!ok)
		error = L"frame generation: the interpolator's root signature was refused";

	ID3D12PipelineState *psos[kPasses] = {};
	for (uint32_t k = 0; ok && k < kPasses; ++k) {
		ID3DBlob *code = nullptr, *messages = nullptr;
		const HRESULT hr = D3DCompile(framegen_shaders::kInterpolatorHlsl, sizeof(framegen_shaders::kInterpolatorHlsl) - 1,
			"aeon_frame_interpolator", nullptr, nullptr, kEntries[k], "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
			&messages);
		if (FAILED(hr) || code == nullptr) {
			error = L"frame generation: kernel " + widen_ascii(kEntries[k], std::strlen(kEntries[k])) +
				L" did not compile";
			if (messages != nullptr)
				error += L": " + widen_ascii(static_cast<const char *>(messages->GetBufferPointer()),
					messages->GetBufferSize());
			ok = false;
		} else {
			D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
			cd.pRootSignature = root;
			cd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
			if (FAILED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&psos[k])))) {
				error = L"frame generation: the device refused kernel " +
					widen_ascii(kEntries[k], std::strlen(kEntries[k]));
				ok = false;
			}
		}
		rel(code);
		rel(messages);
	}

	ID3D12DescriptorHeap *heap = nullptr;
	if (ok) {
		D3D12_DESCRIPTOR_HEAP_DESC hd{};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		hd.NumDescriptors = kRingSlots * kSlotDescriptors;
		hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		ok = SUCCEEDED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)));
		if (!ok)
			error = L"frame generation: the interpolator's descriptor heap was refused";
	}

	const std::lock_guard<std::mutex> lock(build_mutex_);
	if (ok) {
		root_ = root;
		for (uint32_t k = 0; k < kPasses; ++k)
			pso_[k] = psos[k];
		heap_ = heap;
		stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		state_.store(kReady, std::memory_order_release);
	} else {
		rel(root);
		for (ID3D12PipelineState *&p : psos)
			rel(p);
		rel(heap);
		build_error_ = error;
		state_.store(kFailed, std::memory_order_release);
	}
}

bool FrameInterpolatorD3D12::wait_for_kernels()
{
	if (builder_.joinable())
		builder_.join();
	return state_.load(std::memory_order_acquire) == kReady;
}

bool FrameInterpolatorD3D12::ensure(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT color_format,
	bool *building, std::wstring *error)
{
	if (building != nullptr)
		*building = false;
	if (device == nullptr || w == 0 || h == 0) {
		if (error != nullptr)
			*error = L"frame generation: no device or no frame size";
		return false;
	}
	if (device_ != device) {
		release();
		device_ = device;
	}
	const uint32_t s = state_.load(std::memory_order_acquire);
	if (s == kIdle) {
		if (builder_.joinable())
			builder_.join();
		state_.store(kBuilding, std::memory_order_release);
		device->AddRef();
		builder_ = std::thread([this, device] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			build_kernels(device);
			device->Release();
		});
	}
	if (s == kIdle || s == kBuilding) {
		if (building != nullptr)
			*building = true;
		return false;
	}
	if (s == kFailed) {
		if (builder_.joinable())
			builder_.join();
		if (error != nullptr)
			*error = build_error_;
		return false;
	}
	if (builder_.joinable())
		builder_.join();
	return ensure_textures(device, w, h, color_format, error);
}

bool FrameInterpolatorD3D12::ensure_textures(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT color_format,
	std::wstring *error)
{
	if (w == w_ && h == h_ && color_format == color_format_) {
		if (textures_failed_ && error != nullptr)
			*error = L"frame generation: the interpolator's textures could not be made at this size";
		return !textures_failed_;
	}
	release_textures();
	w_ = w;
	h_ = h;
	color_format_ = color_format;
	input_family_ = format_family(color_format);
	const auto writable = [device](DXGI_FORMAT f) {
		D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{ f };
		return f != DXGI_FORMAT_UNKNOWN &&
			SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) &&
			(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
	};
	family_ = input_family_;
	uav_format_ = interpolator_write_format(color_format);
	if (!writable(uav_format_)) {
		const bool srgb = is_srgb(color_format);
		family_ = srgb ? DXGI_FORMAT_R16G16B16A16_TYPELESS : DXGI_FORMAT_R8G8B8A8_TYPELESS;
		uav_format_ = srgb ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
		if (!writable(uav_format_)) {
			textures_failed_ = true;
			if (error != nullptr)
				*error = L"frame generation: this GPU cannot write a frame from a compute pass";
			return false;
		}
	}
	stat_w_ = groups(groups(w, kStatStride), kGroup);
	stat_h_ = groups(groups(h, kStatStride), kGroup);
	tile_w_ = groups(w, kTile);
	tile_h_ = groups(h, kTile);
	textures_failed_ = !create_tex12(device, w, h, family_, true, kOutputState, &output_) ||
		!create_tex12(device, stat_w_, stat_h_, DXGI_FORMAT_R32G32_FLOAT, true, kOutputState, &tile_stats_) ||
		!create_tex12(device, kSearchSpan, 2 * kSearchSpan, DXGI_FORMAT_R32_FLOAT, true, kOutputState,
			&search_errors_) ||
		!create_tex12(device, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, true, kOutputState, &summary_) ||
		!create_tex12(device, w, h, DXGI_FORMAT_R16G16_FLOAT, true, kOutputState, &refined_) ||
		!create_tex12(device, tile_w_, tile_h_, DXGI_FORMAT_R16G16B16A16_FLOAT, true, kOutputState, &layers_) ||
		!create_tex12(device, tile_w_, tile_h_, DXGI_FORMAT_R16_FLOAT, true, kOutputState, &layer_depth_);
	if (textures_failed_) {
		const uint32_t keep_w = w_, keep_h = h_;
		const DXGI_FORMAT keep_f = color_format_;
		release_textures();
		w_ = keep_w;
		h_ = keep_h;
		color_format_ = keep_f;
		textures_failed_ = true;
		if (error != nullptr)
			*error = L"frame generation: the interpolator's textures could not be made at this size";
		return false;
	}
	return true;
}

bool FrameInterpolatorD3D12::record(ID3D12GraphicsCommandList *cmd, const Inputs &in, std::wstring *error)
{
	if (state_.load(std::memory_order_acquire) != kReady || output_ == nullptr || cmd == nullptr ||
		in.previous == nullptr || in.current == nullptr || in.motion == nullptr || in.global_flow == nullptr) {
		if (error != nullptr)
			*error = L"frame generation: the interpolator is not ready or an input is missing";
		return false;
	}
	ID3D12Device *const device = device_;
	const DXGI_FORMAT cur_fmt = in.current->GetDesc().Format;
	const DXGI_FORMAT prev_fmt = in.previous->GetDesc().Format;
	if (format_family(cur_fmt) != input_family_ || format_family(prev_fmt) != input_family_) {
		if (error != nullptr)
			*error = L"frame generation: the two frames are not of the interpolator's format";
		return false;
	}
	const DXGI_FORMAT read = interpolator_read_format(cur_fmt);
	const bool srgb = is_srgb(read) && family_ == input_family_;

	const uint32_t slot = ring_;
	ring_ = (ring_ + 1) % kRingSlots;
	const D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = heap_->GetCPUDescriptorHandleForHeapStart();
	const D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = heap_->GetGPUDescriptorHandleForHeapStart();
	const auto cpu_at = [&](uint32_t i) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = cpu0;
		h.ptr += static_cast<SIZE_T>(slot * kSlotDescriptors + i) * stride_;
		return h;
	};
	const auto gpu_at = [&](uint32_t i) {
		D3D12_GPU_DESCRIPTOR_HANDLE h = gpu0;
		h.ptr += static_cast<UINT64>(slot * kSlotDescriptors + i) * stride_;
		return h;
	};
	const DXGI_FORMAT depth_fmt = in.depth != nullptr ? view_format_for(in.depth->GetDesc().Format) : DXGI_FORMAT_R32_FLOAT;
	srv_2d(device, in.previous, read, cpu_at(0));
	srv_2d(device, in.current, read, cpu_at(1));
	srv_2d(device, in.motion, DXGI_FORMAT_R16G16_FLOAT, cpu_at(2));
	srv_2d(device, nullptr, DXGI_FORMAT_R16_FLOAT, cpu_at(3));
	srv_2d(device, in.global_flow, DXGI_FORMAT_R16G16_FLOAT, cpu_at(4));
	srv_2d(device, in.depth, depth_fmt, cpu_at(5));
	srv_2d(device, tile_stats_, DXGI_FORMAT_R32G32_FLOAT, cpu_at(6));
	srv_2d(device, search_errors_, DXGI_FORMAT_R32_FLOAT, cpu_at(7));
	srv_2d(device, summary_, DXGI_FORMAT_R32G32B32A32_FLOAT, cpu_at(8));
	srv_2d(device, refined_, DXGI_FORMAT_R16G16_FLOAT, cpu_at(9));
	srv_2d(device, layers_, DXGI_FORMAT_R16G16B16A16_FLOAT, cpu_at(10));
	srv_2d(device, layer_depth_, DXGI_FORMAT_R16_FLOAT, cpu_at(11));
	uav_2d(device, output_, uav_format_, cpu_at(kSrvCount + 0));
	uav_2d(device, tile_stats_, DXGI_FORMAT_R32G32_FLOAT, cpu_at(kSrvCount + 1));
	uav_2d(device, search_errors_, DXGI_FORMAT_R32_FLOAT, cpu_at(kSrvCount + 2));
	uav_2d(device, summary_, DXGI_FORMAT_R32G32B32A32_FLOAT, cpu_at(kSrvCount + 3));
	uav_2d(device, refined_, DXGI_FORMAT_R16G16_FLOAT, cpu_at(kSrvCount + 4));
	uav_2d(device, layers_, DXGI_FORMAT_R16G16B16A16_FLOAT, cpu_at(kSrvCount + 5));
	uav_2d(device, layer_depth_, DXGI_FORMAT_R16_FLOAT, cpu_at(kSrvCount + 6));

	constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	struct In {
		ID3D12Resource *res;
		D3D12_RESOURCE_STATES state;
	};
	In ins[5] = { { in.previous, in.previous_state }, { in.current, in.current_state }, { in.motion, in.motion_state },
		{ in.global_flow, in.global_state }, { in.depth, in.depth_state } };
	D3D12_RESOURCE_BARRIER to_read[5]{};
	uint32_t n_read = 0;
	for (uint32_t i = 0; i < 5; ++i) {
		if (ins[i].res == nullptr || (ins[i].state & kRead) == kRead)
			continue;
		bool seen = false;
		for (uint32_t j = 0; j < i; ++j)
			seen = seen || ins[j].res == ins[i].res;
		if (seen)
			continue;
		D3D12_RESOURCE_BARRIER &b = to_read[n_read++];
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = ins[i].res;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = ins[i].state;
		b.Transition.StateAfter = kRead;
	}

	ID3D12Resource *const written[kPasses + 1] = { tile_stats_, search_errors_, summary_, refined_, layers_,
		layer_depth_, output_ };
	D3D12_RESOURCE_BARRIER to_write[kPasses + 1]{};
	for (uint32_t i = 0; i < kPasses + 1; ++i) {
		to_write[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		to_write[i].Transition.pResource = written[i];
		to_write[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		to_write[i].Transition.StateBefore = kOutputState;
		to_write[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	}
	engine_journal_note(L"frame generation");
	if (n_read != 0)
		cmd->ResourceBarrier(n_read, to_read);
	cmd->ResourceBarrier(kPasses + 1, to_write);

	const Constants c{ w_, h_, 1.0f / static_cast<float>(w_), 1.0f / static_cast<float>(h_), in.t,
		in.depth != nullptr ? 1u : 0u, stat_w_, stat_h_, tile_w_, tile_h_, srgb ? 1u : 0u,
		is_float_family(input_family_) ? 1u : 0u };
	ID3D12DescriptorHeap *const heaps[] = { heap_ };
	cmd->SetDescriptorHeaps(1, heaps);
	cmd->SetComputeRootSignature(root_);
	cmd->SetComputeRoot32BitConstants(0, kConstantCount, &c, 0);
	cmd->SetComputeRootDescriptorTable(1, gpu_at(0));
	cmd->SetComputeRootDescriptorTable(2, gpu_at(kSrvCount));

	const auto readable = [&](std::initializer_list<ID3D12Resource *> list) {
		D3D12_RESOURCE_BARRIER b[2]{};
		uint32_t n = 0;
		for (ID3D12Resource *r : list) {
			b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b[n].Transition.pResource = r;
			b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b[n].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
			b[n].Transition.StateAfter = kOutputState;
			++n;
		}
		cmd->ResourceBarrier(n, b);
	};
	const uint32_t px = groups(w_, kGroup), py = groups(h_, kGroup);
	cmd->SetPipelineState(pso_[0]);
	cmd->Dispatch(stat_w_, stat_h_, 1);
	cmd->SetPipelineState(pso_[1]);
	cmd->Dispatch(kSearchSpan, 2 * kSearchSpan, 1);
	readable({ tile_stats_, search_errors_ });
	cmd->SetPipelineState(pso_[2]);
	cmd->Dispatch(1, 1, 1);
	readable({ summary_ });
	cmd->SetPipelineState(pso_[3]);
	cmd->Dispatch(px, py, 1);
	readable({ refined_ });
	cmd->SetPipelineState(pso_[4]);
	cmd->Dispatch(groups(tile_w_, kGroup), groups(tile_h_, kGroup), 1);
	readable({ layers_, layer_depth_ });
	cmd->SetPipelineState(pso_[5]);
	cmd->Dispatch(px, py, 1);
	readable({ output_ });

	for (uint32_t i = 0; i < n_read; ++i)
		std::swap(to_read[i].Transition.StateBefore, to_read[i].Transition.StateAfter);
	if (n_read != 0)
		cmd->ResourceBarrier(n_read, to_read);
	return true;
}

}
