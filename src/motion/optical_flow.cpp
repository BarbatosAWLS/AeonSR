#include "aeon_sr/motion/optical_flow.hpp"

#include <d3dcompiler.h>
#include <string>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "optical_flow_shaders.inl"

namespace aeon_sr {
namespace {

template <class T>
void safe_release(T *&p) noexcept
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

constexpr uint32_t kMaxLumaMips = 8;
constexpr uint32_t kRingSize = 8192;
constexpr uint32_t kSrvCount = 11;
constexpr uint32_t kUavCount = 3;
constexpr uint32_t kTableSize = kSrvCount + kUavCount;

enum : uint32_t {
	kSlotColor = 0,
	kSlotDepth,
	kSlotNullSrv,
	kSlotNullUav = kSlotNullSrv + kSrvCount,
	kSlotLumaSrv = kSlotNullUav + kUavCount,
	kSlotLumaMipSrv = kSlotLumaSrv + 2,
	kSlotLumaUav = kSlotLumaMipSrv + 2 * kMaxLumaMips,
	kSlotFlow = kSlotLumaUav + 2 * kMaxLumaMips,
	kStagingCount = kSlotFlow + 50
};

std::wstring widen_ascii(const char *text)
{
	std::wstring out;
	if (text == nullptr)
		return out;
	constexpr size_t kMax = 400;
	for (size_t i = 0; text[i] != 0 && i < kMax; ++i) {
		const unsigned char c = static_cast<unsigned char>(text[i]);
		const bool eol = (c == 13) || (c == 10);
		out.push_back(eol ? L' ' : (c < 128 ? static_cast<wchar_t>(c) : L'?'));
	}
	return out;
}

const char *entry_of(uint32_t shader)
{
	switch (shader) {
	case 0: return "CSLuma";
	case 1: return "CSDownsample";
	case 2: return "CSCoarseTop";
	case 3: return "CSCoarse";
	case 4: return "CSMedian";
	case 5: return "CSRefine";
	case 6: return "CSGlobal";
	case 7: return "CSConfidence";
	case 8: return "CSExport";
	case 9: return "CSCopyFlow";
	case 10: return "CSStructure";
	case 11: return "CSModelTerms";
	case 12: return "CSModelReduce";
	case 13: return "CSModelSolve";
	case 14: return "CSDecision";
	case 15: return "CSFuse";
	case 16: return "CSPhotoTerms";
	default: return "CSThetaPublish";
	}
}

constexpr DXGI_FORMAT kSrvRegisterFormat[kSrvCount] = {
	DXGI_FORMAT_R8G8B8A8_UNORM,
	DXGI_FORMAT_R16_FLOAT,
	DXGI_FORMAT_R16_FLOAT,
	DXGI_FORMAT_R16G16_FLOAT,
	DXGI_FORMAT_R16G16_FLOAT,
	DXGI_FORMAT_R16G16_FLOAT,
	DXGI_FORMAT_R32_FLOAT,
	DXGI_FORMAT_R16_FLOAT,
	DXGI_FORMAT_R32G32B32A32_FLOAT,
	DXGI_FORMAT_R32_FLOAT,
	DXGI_FORMAT_R32_FLOAT,
};
constexpr DXGI_FORMAT kUavRegisterFormat[kUavCount] = {
	DXGI_FORMAT_R16G16_FLOAT,
	DXGI_FORMAT_R16_FLOAT,
	DXGI_FORMAT_R32G32B32A32_FLOAT,
};

constexpr uint32_t kModelStats = 76;
static_assert(kModelStats % 4u == 0u, "store_terms reduces the terms in four equal parts (kTermParts)");
constexpr uint32_t kPartialRows = 4096;
static_assert(kPartialRows <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, "one column of partial rows");
constexpr uint32_t kModelIterations = 4;
constexpr uint32_t kPhotoIterations = 4;
constexpr uint32_t kPublishedFits = 2;

uint32_t groups(uint32_t n, uint32_t size)
{
	return (n + size - 1u) / size;
}

uint32_t level_size(uint32_t n, uint32_t shift)
{
	return (std::max)(1u, n >> shift);
}

void barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	cmd->ResourceBarrier(1, &b);
}

}

OpticalFlowD3D12::~OpticalFlowD3D12()
{
	release();
}

void OpticalFlowD3D12::release_textures()
{
	Tex *const all[] = { &luma_[0], &luma_[1], &l4_, &l3_[0], &l3_[1], &l2_[0], &l2_[1],
		&l1_[0], &l1_[1], &l0_[0], &l0_[1], &dense_[0], &dense_[1], &dense_m_,
		&prev_flow_, &global_, &conf_quarter_, &prev_conf_, &motion_, &confidence_,
		&struct_q_, &partials_, &sums_, &theta_[0], &theta_[1], &alpha_q_, &theta_pub_ };
	for (Tex *t : all) {
		safe_release(t->res);
		*t = Tex{};
	}
	safe_release(staging_);
	width_ = height_ = 0;
	device_bytes_ = 0;
	frames_ = 0;
	prev_jitter_x_ = 0.0f;
	prev_jitter_y_ = 0.0f;
	parity_ = 0;
	model_cold_ = true;
	model_ran_ = false;
	model_textures_ = false;
	model_retry_ms_ = 0;
}

void OpticalFlowD3D12::release()
{
	model_cancel_.store(true, std::memory_order_relaxed);
	if (model_builder_.joinable())
		model_builder_.join();
	model_cancel_.store(false, std::memory_order_relaxed);
	model_state_.store(kModelNone, std::memory_order_relaxed);
	textures_failed_on_ = nullptr;
	release_textures();
	for (uint32_t q = 0; q < 2; ++q)
		for (uint32_t i = 0; i < kShaderCount; ++i)
			safe_release(pso_[q][i]);
	safe_release(heap_);
	safe_release(root_);
	safe_release(prof_heap_);
	safe_release(prof_readback_);
	prof_on_ = false;
	prof_count_ = 0;
	quality_ok[0] = quality_ok[1] = false;
	device_ = nullptr;
	ring_ = 0;
}

const char *OpticalFlowD3D12::kernel_name(uint32_t shader) noexcept
{
	static const char *const names[kShaderCount] = {
		"luma", "downsample", "coarse top", "coarse", "median", "refine",
		"global", "confidence", "export", "copy flow",
		"structure", "model terms", "model reduce", "model solve", "decision", "fuse", "photo terms",
		"theta publish",
	};
	return shader < kShaderCount ? names[shader] : "?";
}

bool OpticalFlowD3D12::set_profiling(bool on, ID3D12CommandQueue *queue)
{
	prof_on_ = false;
	prof_count_ = 0;
	if (!on)
		return true;
	if (device_ == nullptr || queue == nullptr)
		return false;
	if (prof_freq_ == 0 && (FAILED(queue->GetTimestampFrequency(&prof_freq_)) || prof_freq_ == 0))
		return false;
	if (prof_heap_ == nullptr) {
		D3D12_QUERY_HEAP_DESC qd{};
		qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		qd.Count = kMaxProfile + 1u;
		if (FAILED(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(&prof_heap_))))
			return false;
	}
	if (prof_readback_ == nullptr) {
		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC rd{};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width = sizeof(UINT64) * (kMaxProfile + 1u);
		rd.Height = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.SampleDesc.Count = 1;
		rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
				D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&prof_readback_))))
			return false;
	}
	prof_on_ = true;
	return true;
}

bool OpticalFlowD3D12::read_profile(std::vector<ProfileEntry> *out) const
{
	if (out == nullptr || prof_readback_ == nullptr || prof_count_ == 0 || prof_freq_ == 0)
		return false;
	const D3D12_RANGE range{ 0, sizeof(UINT64) * (prof_count_ + 1u) };
	void *mapped = nullptr;
	if (FAILED(prof_readback_->Map(0, &range, &mapped)) || mapped == nullptr)
		return false;
	const UINT64 *ts = static_cast<const UINT64 *>(mapped);
	out->clear();
	for (uint32_t i = 0; i < prof_count_; ++i) {
		const double ms = ts[i + 1] > ts[i]
			? static_cast<double>(ts[i + 1] - ts[i]) * 1000.0 / static_cast<double>(prof_freq_) : 0.0;
		out->push_back({ kernel_name(prof_shader_[i]), ms });
	}
	const D3D12_RANGE none{ 0, 0 };
	prof_readback_->Unmap(0, &none);
	return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE OpticalFlowD3D12::staging(uint32_t slot) const
{
	D3D12_CPU_DESCRIPTOR_HANDLE h = staging_->GetCPUDescriptorHandleForHeapStart();
	h.ptr += static_cast<SIZE_T>(slot) * stride_;
	return h;
}

bool OpticalFlowD3D12::make_tex(Tex &t, uint32_t w, uint32_t h, DXGI_FORMAT fmt, uint32_t mips,
	std::wstring *error)
{
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = w;
	desc.Height = h;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = static_cast<UINT16>(mips);
	desc.Format = fmt;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	ID3D12Resource *res = nullptr;
	if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res)))) {
		if (error != nullptr)
			*error = L"optical flow: could not create a working texture";
		return false;
	}
	const D3D12_RESOURCE_ALLOCATION_INFO info = device_->GetResourceAllocationInfo(0, 1, &desc);
	device_bytes_ += info.SizeInBytes;
	t.res = res;
	t.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	t.w = w;
	t.h = h;
	t.mips = mips;
	t.fmt = fmt;
	return true;
}

bool OpticalFlowD3D12::make_pipelines(std::wstring *error)
{
	D3D12_DESCRIPTOR_RANGE ranges[2]{};
	ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[0].NumDescriptors = kSrvCount;
	ranges[0].BaseShaderRegister = 0;
	ranges[0].OffsetInDescriptorsFromTableStart = 0;
	ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[1].NumDescriptors = kUavCount;
	ranges[1].BaseShaderRegister = 0;
	ranges[1].OffsetInDescriptorsFromTableStart = kSrvCount;

	D3D12_ROOT_PARAMETER params[2]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	params[0].Constants.Num32BitValues = sizeof(Constants) / 4;
	params[0].Constants.ShaderRegister = 0;
	params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	params[1].DescriptorTable.NumDescriptorRanges = 2;
	params[1].DescriptorTable.pDescriptorRanges = ranges;

	D3D12_STATIC_SAMPLER_DESC samplers[2]{};
	for (uint32_t i = 0; i < 2; ++i) {
		samplers[i].Filter = (i == 0) ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
		                              : D3D12_FILTER_MIN_MAG_MIP_POINT;
		samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
		samplers[i].ShaderRegister = i;
	}

	D3D12_ROOT_SIGNATURE_DESC rs{};
	rs.NumParameters = 2;
	rs.pParameters = params;
	rs.NumStaticSamplers = 2;
	rs.pStaticSamplers = samplers;

	ID3DBlob *blob = nullptr, *err = nullptr;
	if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
		safe_release(err);
		if (error != nullptr)
			*error = L"optical flow: root signature failed";
		return false;
	}
	const HRESULT hr = device_->CreateRootSignature(0, blob->GetBufferPointer(),
		blob->GetBufferSize(), IID_PPV_ARGS(&root_));
	safe_release(blob);
	safe_release(err);
	if (FAILED(hr)) {
		if (error != nullptr)
			*error = L"optical flow: root signature could not be created";
		return false;
	}

	D3D12_DESCRIPTOR_HEAP_DESC ring{};
	ring.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	ring.NumDescriptors = kRingSize;
	ring.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device_->CreateDescriptorHeap(&ring, IID_PPV_ARGS(&heap_)))) {
		if (error != nullptr)
			*error = L"optical flow: descriptor heap failed";
		return false;
	}

	std::wstring first_error;
	for (uint32_t q = 0; q < 2; ++q) {
		quality_ok[q] = true;
		const D3D_SHADER_MACRO defines[] = {
			{ "AEON_FLOW_QUALITY", q == 0 ? "1" : "2" },
			{ nullptr, nullptr }
		};
		for (uint32_t i = 0; i < kFirstModelShader; ++i) {
			ID3DBlob *cs = nullptr, *cerr = nullptr;
			if (FAILED(D3DCompile(kFlowHlsl, std::strlen(kFlowHlsl), "aeon_flow", defines,
					nullptr, entry_of(i), "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &cerr))) {
				std::wstring detail = L"optical flow: ";
				detail += widen_ascii(entry_of(i));
				detail += q == 0 ? L" (balanced)" : L" (high)";
				detail += L" failed to compile";
				if (cerr != nullptr && cerr->GetBufferPointer() != nullptr) {
					detail += L": ";
					detail += widen_ascii(static_cast<const char *>(cerr->GetBufferPointer()));
				}
				safe_release(cs);
				safe_release(cerr);
				if (first_error.empty())
					first_error = detail;
				quality_ok[q] = false;
				break;
			}
			safe_release(cerr);
			D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
			pd.pRootSignature = root_;
			pd.CS.pShaderBytecode = cs->GetBufferPointer();
			pd.CS.BytecodeLength = cs->GetBufferSize();
			const HRESULT ok = device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_[q][i]));
			safe_release(cs);
			if (FAILED(ok)) {
				if (first_error.empty()) {
					wchar_t buf[192]{};
					_snwprintf_s(buf, _TRUNCATE, L"optical flow: %s (%s) pipeline state refused, hr=0x%08X",
						widen_ascii(entry_of(i)).c_str(), q == 0 ? L"balanced" : L"high",
						static_cast<unsigned>(ok));
					first_error = buf;
				}
				quality_ok[q] = false;
				break;
			}
		}
	}

	if (!quality_ok[0] && !quality_ok[1]) {
		if (error != nullptr)
			*error = first_error.empty() ? std::wstring(L"optical flow: no kernels could be built")
			                             : first_error;
		return false;
	}

	model_state_.store(kModelBuilding, std::memory_order_relaxed);
	try {
		model_builder_ = std::thread([this]() { build_model_pipelines(); });
	} catch (...) {
		model_build_error_ = L"camera model: no thread to build it on";
		model_state_.store(kModelFailed, std::memory_order_release);
	}
	return true;
}

void OpticalFlowD3D12::build_model_pipelines()
{
	ID3D12PipelineState *built[kShaderCount]{};
	std::wstring why;
	bool cancelled = false;
	for (uint32_t i = kFirstModelShader; i < kShaderCount && why.empty(); ++i) {
		if (model_cancel_.load(std::memory_order_relaxed)) {
			cancelled = true;
			break;
		}
		ID3DBlob *cs = nullptr, *cerr = nullptr;
		if (FAILED(D3DCompile(kFlowHlsl, std::strlen(kFlowHlsl), "aeon_flow", nullptr,
				nullptr, entry_of(i), "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &cerr))) {
			why = L"camera model: ";
			why += widen_ascii(entry_of(i));
			why += L" failed to compile";
			if (cerr != nullptr && cerr->GetBufferPointer() != nullptr) {
				why += L": ";
				why += widen_ascii(static_cast<const char *>(cerr->GetBufferPointer()));
			}
			safe_release(cs);
			safe_release(cerr);
			break;
		}
		safe_release(cerr);
		D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
		pd.pRootSignature = root_;
		pd.CS.pShaderBytecode = cs->GetBufferPointer();
		pd.CS.BytecodeLength = cs->GetBufferSize();
		const HRESULT ok = device_->CreateComputePipelineState(&pd, IID_PPV_ARGS(&built[i]));
		safe_release(cs);
		if (FAILED(ok)) {
			wchar_t buf[160]{};
			_snwprintf_s(buf, _TRUNCATE, L"camera model: %s pipeline state refused, hr=0x%08X",
				widen_ascii(entry_of(i)).c_str(), static_cast<unsigned>(ok));
			why = buf;
		}
	}
	if (cancelled || !why.empty()) {
		for (ID3D12PipelineState *&p : built)
			safe_release(p);
		if (cancelled)
			return;
		model_build_error_ = why;
		model_state_.store(kModelFailed, std::memory_order_release);
		return;
	}
	for (uint32_t i = kFirstModelShader; i < kShaderCount; ++i) {
		pso_[0][i] = built[i];
		pso_[1][i] = built[i];
		pso_[1][i]->AddRef();
	}
	model_state_.store(kModelReady, std::memory_order_release);
}

bool OpticalFlowD3D12::wait_for_camera_model()
{
	if (model_builder_.joinable())
		model_builder_.join();
	return model_ok();
}

bool OpticalFlowD3D12::make_textures(std::wstring *error)
{
	if (fail_textures_for_test > 0) {
		--fail_textures_for_test;
		if (error != nullptr)
			*error = L"optical flow: could not create a working texture";
		return false;
	}
	const uint32_t w = width_, h = height_;
	uint32_t mips = 1;
	while (mips < kMaxLumaMips && (std::max(w, h) >> mips) > 0u)
		++mips;

	if (!make_tex(luma_[0], w, h, DXGI_FORMAT_R16_FLOAT, mips, error) ||
		!make_tex(luma_[1], w, h, DXGI_FORMAT_R16_FLOAT, mips, error) ||
		!make_tex(l4_, level_size(w, 7), level_size(h, 7), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(l3_[0], level_size(w, 6), level_size(h, 6), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(l3_[1], level_size(w, 6), level_size(h, 6), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(l2_[0], level_size(w, 5), level_size(h, 5), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(l2_[1], level_size(w, 5), level_size(h, 5), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(l1_[0], level_size(w, 4), level_size(h, 4), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(l1_[1], level_size(w, 4), level_size(h, 4), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(l0_[0], level_size(w, 3), level_size(h, 3), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(l0_[1], level_size(w, 3), level_size(h, 3), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(dense_[0], level_size(w, 2), level_size(h, 2), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(dense_[1], level_size(w, 2), level_size(h, 2), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(dense_m_, level_size(w, 2), level_size(h, 2), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(prev_flow_, level_size(w, 2), level_size(h, 2), DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(global_, 1, 1, DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(conf_quarter_, level_size(w, 2), level_size(h, 2), DXGI_FORMAT_R16_FLOAT, 1, error) ||
		!make_tex(prev_conf_, level_size(w, 2), level_size(h, 2), DXGI_FORMAT_R16_FLOAT, 1, error) ||
		!make_tex(motion_, w, h, DXGI_FORMAT_R16G16_FLOAT, 1, error) ||
		!make_tex(confidence_, w, h, DXGI_FORMAT_R16_FLOAT, 1, error))
		return false;
	make_model_textures();

	D3D12_DESCRIPTOR_HEAP_DESC hd{};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = kStagingCount;
	if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&staging_))))
		return false;

	for (uint32_t i = 0; i < kSrvCount; ++i) {
		D3D12_SHADER_RESOURCE_VIEW_DESC nd{};
		nd.Format = kSrvRegisterFormat[i];
		nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		nd.Texture2D.MipLevels = 1;
		device_->CreateShaderResourceView(nullptr, &nd, staging(kSlotNullSrv + i));
	}
	for (uint32_t i = 0; i < kUavCount; ++i) {
		D3D12_UNORDERED_ACCESS_VIEW_DESC nd{};
		nd.Format = kUavRegisterFormat[i];
		nd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device_->CreateUnorderedAccessView(nullptr, nullptr, &nd, staging(kSlotNullUav + i));
	}

	for (uint32_t i = 0; i < 2; ++i) {
		D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = DXGI_FORMAT_R16_FLOAT;
		sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sd.Texture2D.MipLevels = luma_[i].mips;
		device_->CreateShaderResourceView(luma_[i].res, &sd, staging(kSlotLumaSrv + i));
		for (uint32_t m = 0; m < kMaxLumaMips; ++m) {
			const uint32_t clamped = std::min(m, luma_[i].mips - 1u);
			D3D12_SHADER_RESOURCE_VIEW_DESC md = sd;
			md.Texture2D.MostDetailedMip = clamped;
			md.Texture2D.MipLevels = 1;
			device_->CreateShaderResourceView(luma_[i].res, &md,
				staging(kSlotLumaMipSrv + i * kMaxLumaMips + m));
			D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
			ud.Format = DXGI_FORMAT_R16_FLOAT;
			ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			ud.Texture2D.MipSlice = clamped;
			device_->CreateUnorderedAccessView(luma_[i].res, nullptr, &ud,
				staging(kSlotLumaUav + i * kMaxLumaMips + m));
		}
	}

	make_flow_views();
	return true;
}

void OpticalFlowD3D12::make_flow_views()
{
	Tex *const flow[] = { &l4_, &l3_[0], &l3_[1], &l2_[0], &l2_[1], &l1_[0], &l1_[1],
		&l0_[0], &l0_[1], &dense_[0], &dense_[1], &dense_m_, &prev_flow_, &global_,
		&conf_quarter_, &prev_conf_, &motion_, &confidence_,
		&struct_q_, &partials_, &sums_, &theta_[0], &theta_[1], &alpha_q_, &theta_pub_ };
	static_assert(sizeof(flow) / sizeof(flow[0]) * 2 == kStagingCount - kSlotFlow,
		"every flow texture needs an SRV and a UAV slot");
	for (uint32_t i = 0; i < sizeof(flow) / sizeof(flow[0]); ++i) {
		flow[i]->slot = i;
		if (flow[i]->res == nullptr)
			continue;
		const DXGI_FORMAT fmt = flow[i]->fmt;
		D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = fmt;
		sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sd.Texture2D.MipLevels = 1;
		device_->CreateShaderResourceView(flow[i]->res, &sd, staging(kSlotFlow + i * 2));
		D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
		ud.Format = fmt;
		ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device_->CreateUnorderedAccessView(flow[i]->res, nullptr, &ud,
			staging(kSlotFlow + i * 2 + 1));
	}
}

void OpticalFlowD3D12::make_model_textures()
{
	const uint32_t w = width_, h = height_;
	const uint32_t rows = groups(level_size(w, 1), 16) * groups(level_size(h, 1), 16);
	const uint64_t bytes_before = device_bytes_;
	std::wstring error;
	const bool fail_for_test = fail_model_textures_for_test > 0;
	if (fail_for_test)
		--fail_model_textures_for_test;
	model_textures_ = !fail_for_test &&
		make_tex(struct_q_, level_size(w, 2), level_size(h, 2), DXGI_FORMAT_R32G32B32A32_FLOAT, 1, &error) &&
		make_tex(partials_, kModelStats * groups(rows, kPartialRows), (std::min)(rows, kPartialRows),
			DXGI_FORMAT_R32_FLOAT, 1, &error) &&
		make_tex(sums_, 80, 1, DXGI_FORMAT_R32_FLOAT, 1, &error) &&
		make_tex(theta_[0], 16, 2, DXGI_FORMAT_R32_FLOAT, 1, &error) &&
		make_tex(theta_[1], 16, 2, DXGI_FORMAT_R32_FLOAT, 1, &error) &&
		make_tex(alpha_q_, level_size(w, 2), level_size(h, 2), DXGI_FORMAT_R16_FLOAT, 1, &error) &&
		make_tex(theta_pub_, 16, 8, DXGI_FORMAT_R32_FLOAT, 1, &error);
	if (model_textures_) {
		model_retry_ms_ = 0;
		return;
	}
	model_textures_failed_at_ = GetTickCount64();
	model_retry_ms_ = model_retry_ms_ == 0 ? texture_retry_ms : (std::min)(2u * model_retry_ms_, 60000u);
	for (Tex *t : { &struct_q_, &partials_, &sums_, &theta_[0], &theta_[1], &alpha_q_, &theta_pub_ }) {
		safe_release(t->res);
		*t = Tex{};
	}
	device_bytes_ = bytes_before;
	model_textures_error_ = L"camera model: its textures could not be created at this size";
}

uint32_t OpticalFlowD3D12::records_per_ring() const noexcept
{
	return dispatches_ != 0 ? kRingSize / (dispatches_ * kTableSize) : 0u;
}

bool OpticalFlowD3D12::ensure(ID3D12Device *device, uint32_t width, uint32_t height,
	std::wstring *error)
{
	if (device == nullptr) {
		if (error != nullptr)
			*error = L"optical flow: no D3D12 device yet (the bridge has not come up)";
		return false;
	}
	if (width == 0 || height == 0) {
		if (error != nullptr)
			*error = L"optical flow: the frame has no size yet";
		return false;
	}
	if (device_ == device && width_ == width && height_ == height && motion_.res != nullptr) {
		if (!model_textures_ && GetTickCount64() - model_textures_failed_at_ >= model_retry_ms_) {
			make_model_textures();
			if (model_textures_) {
				make_flow_views();
				model_cold_ = true;
			}
		}
		return true;
	}

	if (pipelines_failed_on == device) {
		if (error != nullptr)
			*error = pipelines_error;
		return false;
	}
	if (textures_failed_on_ == device && textures_failed_w_ == width && textures_failed_h_ == height &&
		GetTickCount64() - textures_failed_at_ < texture_retry_ms) {
		if (error != nullptr)
			*error = textures_error_;
		return false;
	}

	const bool same_device = device_ == device && root_ != nullptr;
	if (same_device) {
		release_textures();
	} else {
		release();
		device_ = device;
		stride_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		if (!make_pipelines(error)) {
			pipelines_failed_on = device;
			pipelines_error = error != nullptr ? *error : L"optical flow: kernels failed to build";
			release();
			return false;
		}
	}
	width_ = width;
	height_ = height;
	if (!make_textures(error)) {
		if (error != nullptr && error->empty())
			*error = L"optical flow: working textures could not be created";
		textures_failed_on_ = device;
		textures_failed_w_ = width;
		textures_failed_h_ = height;
		textures_failed_at_ = GetTickCount64();
		textures_error_ = error != nullptr ? *error : L"optical flow: working textures could not be created";
		release_textures();
		return false;
	}
	textures_failed_on_ = nullptr;
	return true;
}

void OpticalFlowD3D12::to_state(ID3D12GraphicsCommandList *cmd, Tex &t, D3D12_RESOURCE_STATES want)
{
	if (t.res == nullptr || t.state == want)
		return;
	barrier(cmd, t.res, t.state, want);
	t.state = want;
}

void OpticalFlowD3D12::dispatch(ID3D12GraphicsCommandList *cmd, Shader shader, const Constants &c,
	const D3D12_CPU_DESCRIPTOR_HANDLE *srvs, uint32_t srv_count,
	const D3D12_CPU_DESCRIPTOR_HANDLE *uavs, uint32_t uav_count,
	uint32_t groups_x, uint32_t groups_y)
{
	if (ring_ + kTableSize > kRingSize)
		ring_ = 0;

	D3D12_CPU_DESCRIPTOR_HANDLE dst = heap_->GetCPUDescriptorHandleForHeapStart();
	dst.ptr += static_cast<SIZE_T>(ring_) * stride_;
	for (uint32_t i = 0; i < kTableSize; ++i) {
		const bool is_uav = i >= kSrvCount;
		D3D12_CPU_DESCRIPTOR_HANDLE src;
		if (is_uav) {
			const uint32_t u = i - kSrvCount;
			src = (u < uav_count) ? uavs[u] : D3D12_CPU_DESCRIPTOR_HANDLE{};
			if (src.ptr == 0)
				src = staging(kSlotNullUav + u);
		} else {
			src = (i < srv_count) ? srvs[i] : D3D12_CPU_DESCRIPTOR_HANDLE{};
			if (src.ptr == 0)
				src = staging(kSlotNullSrv + i);
		}
		D3D12_CPU_DESCRIPTOR_HANDLE at = dst;
		at.ptr += static_cast<SIZE_T>(i) * stride_;
		device_->CopyDescriptorsSimple(1, at, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}
	D3D12_GPU_DESCRIPTOR_HANDLE table = heap_->GetGPUDescriptorHandleForHeapStart();
	table.ptr += static_cast<UINT64>(ring_) * stride_;
	ring_ += kTableSize;

	ID3D12DescriptorHeap *heaps[] = { heap_ };
	cmd->SetDescriptorHeaps(1, heaps);
	cmd->SetComputeRootSignature(root_);
	cmd->SetPipelineState(pso_[quality_index_][shader]);
	cmd->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
	cmd->SetComputeRootDescriptorTable(1, table);
	cmd->Dispatch(groups_x, groups_y, 1);
	++dispatches_;

	D3D12_RESOURCE_BARRIER uav{};
	uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
	cmd->ResourceBarrier(1, &uav);

	if (prof_on_ && prof_count_ < kMaxProfile) {
		prof_shader_[prof_count_] = static_cast<uint8_t>(shader);
		cmd->EndQuery(prof_heap_, D3D12_QUERY_TYPE_TIMESTAMP, ++prof_count_);
	}
}

void OpticalFlowD3D12::level(ID3D12GraphicsCommandList *cmd, Tex &parent, Tex &a, Tex &b,
	uint32_t mip1, uint32_t mip2, uint32_t first)
{
	Constants c{};
	c.dst_w = a.w;
	c.dst_h = a.h;
	c.inv_dst_x = 1.0f / static_cast<float>(a.w);
	c.inv_dst_y = 1.0f / static_cast<float>(a.h);
	c.inv_src_x = 1.0f / static_cast<float>(parent.w);
	c.inv_src_y = 1.0f / static_cast<float>(parent.h);
	c.texel1_x = static_cast<float>(1u << mip1) / static_cast<float>(width_);
	c.texel1_y = static_cast<float>(1u << mip1) / static_cast<float>(height_);
	c.texel2_x = static_cast<float>(1u << mip2) / static_cast<float>(width_);
	c.texel2_y = static_cast<float>(1u << mip2) / static_cast<float>(height_);
	c.mip1 = mip1;
	c.mip2 = mip2;
	c.first = first;
	c.has_depth = have_depth_ ? 1u : 0u;
	c.inv_full_x = 1.0f / static_cast<float>(width_);
	c.inv_full_y = 1.0f / static_cast<float>(height_);

	to_state(cmd, parent, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	to_state(cmd, a, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[8] = {
			staging(kSlotColor), staging(kSlotLumaSrv + parity_),
			staging(kSlotLumaSrv + (parity_ ^ 1u)), srv_of(parent),
			srv_of(prev_flow_), srv_of(global_),
			staging(kSlotDepth), D3D12_CPU_DESCRIPTOR_HANDLE{}
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[2] = { uav_of(a), D3D12_CPU_DESCRIPTOR_HANDLE{} };
		dispatch(cmd, kCoarse, c, srvs, 8, uavs, 2, groups(a.w, 8), groups(a.h, 8));
	}

	to_state(cmd, a, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	to_state(cmd, b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	{
		Constants m = c;
		m.inv_src_x = c.inv_dst_x;
		m.inv_src_y = c.inv_dst_y;
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[8] = {
			staging(kSlotColor), staging(kSlotLumaSrv + parity_),
			staging(kSlotLumaSrv + (parity_ ^ 1u)), srv_of(a),
			srv_of(prev_flow_), srv_of(global_),
			staging(kSlotDepth), D3D12_CPU_DESCRIPTOR_HANDLE{}
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[2] = { uav_of(b), D3D12_CPU_DESCRIPTOR_HANDLE{} };
		dispatch(cmd, kMedian, m, srvs, 8, uavs, 2, groups(b.w, 8), groups(b.h, 8));
	}
}

void OpticalFlowD3D12::record_camera_model(ID3D12GraphicsCommandList *cmd, const Constants &base)
{
	const D3D12_RESOURCE_STATES read_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	const D3D12_RESOURCE_STATES write_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	const D3D12_CPU_DESCRIPTOR_HANDLE none{};
	const uint32_t qw = struct_q_.w, qh = struct_q_.h;

	Constants m = base;
	m.dst_w = qw;
	m.dst_h = qh;
	m.inv_dst_x = 1.0f / static_cast<float>(qw);
	m.inv_dst_y = 1.0f / static_cast<float>(qh);
	m.inv_src_x = m.inv_dst_x;
	m.inv_src_y = m.inv_dst_y;
	m.norm_x = 2.0f * static_cast<float>(width_) / static_cast<float>(height_);
	m.px_per_unit = 0.5f * static_cast<float>(height_);
	m.terms_gx = groups(qw, 16);
	m.terms_gy = groups(qh, 16);
	m.partial_rows = kPartialRows;
	m.model_flags = (model_cold_ ? 1u : 0u) | (base.model_flags & 6u);

	m.model_iter = 0;
	if (depth_logarithmic_)
		m.has_depth = 0u;

	to_state(cmd, struct_q_, write_state);
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[kSrvCount] = {
			none, staging(kSlotLumaSrv + parity_), none, none, none, none,
			staging(kSlotDepth), none, none, none, none
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[kUavCount] = { none, none, uav_of(struct_q_) };
		dispatch(cmd, kStructure, m, srvs, kSrvCount, uavs, kUavCount, groups(qw, 8), groups(qh, 8));
	}
	to_state(cmd, struct_q_, read_state);
	to_state(cmd, conf_quarter_, read_state);

	static_assert((kModelIterations + kPhotoIterations) % 2u == 0u, "the fit has to end in theta_[0]");
	const uint32_t flow_it = kModelIterations, photo_it = kPhotoIterations;
	for (uint32_t k = 0; k < flow_it + photo_it; ++k) {
		Tex &cur = theta_[k & 1u];
		Tex &next = theta_[(k + 1u) & 1u];
		const bool photo = k >= flow_it;
		Constants t = m;
		t.model_iter = photo ? 4u + (k - flow_it) : k;
		if (photo) {
			const uint32_t mip = (k == flow_it) ? 2u : 1u;
			t.src_mip = mip;
			t.dst_w = level_size(width_, mip);
			t.dst_h = level_size(height_, mip);
			t.inv_dst_x = 1.0f / static_cast<float>(t.dst_w);
			t.inv_dst_y = 1.0f / static_cast<float>(t.dst_h);
			t.terms_gx = groups(t.dst_w, 16);
			t.terms_gy = groups(t.dst_h, 16);
		}
		to_state(cmd, cur, read_state);
		to_state(cmd, partials_, write_state);
		if (photo) {
			const D3D12_CPU_DESCRIPTOR_HANDLE srvs[kSrvCount] = {
				none, staging(kSlotLumaSrv + parity_), staging(kSlotLumaSrv + (parity_ ^ 1u)), none, none, none,
				staging(kSlotDepth), none, none, srv_of(cur), none
			};
			const D3D12_CPU_DESCRIPTOR_HANDLE uavs[kUavCount] = { none, uav_of(partials_), none };
			dispatch(cmd, kPhotoTerms, t, srvs, kSrvCount, uavs, kUavCount, t.terms_gx, t.terms_gy);
		} else {
			const D3D12_CPU_DESCRIPTOR_HANDLE srvs[kSrvCount] = {
				none, none, none, srv_of(dense_[1]), none, none,
				staging(kSlotDepth), srv_of(conf_quarter_), srv_of(struct_q_), srv_of(cur), none
			};
			const D3D12_CPU_DESCRIPTOR_HANDLE uavs[kUavCount] = { none, uav_of(partials_), none };
			dispatch(cmd, kModelTerms, t, srvs, kSrvCount, uavs, kUavCount, t.terms_gx, t.terms_gy);
		}
		to_state(cmd, partials_, read_state);
		to_state(cmd, sums_, write_state);
		{
			const D3D12_CPU_DESCRIPTOR_HANDLE srvs[kSrvCount] = {
				none, none, none, none, none, none, none, none, none, none, srv_of(partials_)
			};
			const D3D12_CPU_DESCRIPTOR_HANDLE uavs[kUavCount] = { none, uav_of(sums_), none };
			dispatch(cmd, kModelReduce, t, srvs, kSrvCount, uavs, kUavCount, kModelStats, 1);
		}
		to_state(cmd, sums_, read_state);
		to_state(cmd, next, write_state);
		{
			const D3D12_CPU_DESCRIPTOR_HANDLE srvs[kSrvCount] = {
				none, none, none, none, none, none, none, none, none, srv_of(cur), srv_of(sums_)
			};
			const D3D12_CPU_DESCRIPTOR_HANDLE uavs[kUavCount] = { none, uav_of(next), none };
			dispatch(cmd, kModelSolve, t, srvs, kSrvCount, uavs, kUavCount, 1, 1);
		}
	}
	to_state(cmd, theta_[0], read_state);

	to_state(cmd, theta_pub_, write_state);
	{
		Constants p = m;
		p.pad_colour = kPublishedFits;
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[kSrvCount] = {
			none, none, none, none, none, none, staging(kSlotDepth), none, none, srv_of(theta_[0]), none
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[kUavCount] = { none, uav_of(theta_pub_), none };
		dispatch(cmd, kThetaPublish, p, srvs, kSrvCount, uavs, kUavCount, 1, 1);
	}
	to_state(cmd, theta_pub_, read_state);

	to_state(cmd, alpha_q_, write_state);
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[kSrvCount] = {
			none, staging(kSlotLumaSrv + parity_), staging(kSlotLumaSrv + (parity_ ^ 1u)),
			srv_of(dense_[1]), none, none,
			staging(kSlotDepth), none, srv_of(struct_q_), srv_of(theta_pub_), none
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[kUavCount] = { none, uav_of(alpha_q_), none };
		dispatch(cmd, kDecision, m, srvs, kSrvCount, uavs, kUavCount, groups(qw, 8), groups(qh, 8));
	}
	to_state(cmd, alpha_q_, read_state);

	Constants f = m;
	f.dst_w = width_;
	f.dst_h = height_;
	f.inv_dst_x = base.inv_full_x;
	f.inv_dst_y = base.inv_full_y;
	to_state(cmd, motion_, write_state);
	to_state(cmd, confidence_, write_state);
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[kSrvCount] = {
			none, none, none, srv_of(dense_[1]), none, none,
			staging(kSlotDepth), srv_of(conf_quarter_), srv_of(struct_q_), srv_of(theta_pub_), srv_of(alpha_q_)
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[kUavCount] = { uav_of(motion_), uav_of(confidence_), none };
		dispatch(cmd, kFuse, f, srvs, kSrvCount, uavs, kUavCount, groups(width_, 8), groups(height_, 8));
	}
}

D3D12_CPU_DESCRIPTOR_HANDLE OpticalFlowD3D12::srv_of(const Tex &t) const
{
	return staging(kSlotFlow + t.slot * 2u);
}

D3D12_CPU_DESCRIPTOR_HANDLE OpticalFlowD3D12::uav_of(const Tex &t) const
{
	return staging(kSlotFlow + t.slot * 2u + 1u);
}

namespace {

void barrier_mip(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, uint32_t mip,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.Subresource = mip;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	cmd->ResourceBarrier(1, &b);
}

DXGI_FORMAT readable_depth(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	default: return f;
	}
}

DXGI_FORMAT readable_color(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return f;
	}
}

}

bool OpticalFlowD3D12::record(ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
	ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state,
	Quality quality, bool launcher_filters, std::wstring *error, bool camera_model,
	float jitter_x_px, float jitter_y_px, bool publish_want)
{
	if (cmd == nullptr || color == nullptr || !ready()) {
		if (error != nullptr)
			*error = L"optical flow: not initialised";
		return false;
	}
	const Quality use = usable(quality);
	quality_index_ = (use == Quality::High) ? 1u : 0u;
	dispatches_ = 0;

	const D3D12_RESOURCE_DESC cdesc = color->GetDesc();
	D3D12_SHADER_RESOURCE_VIEW_DESC csrv{};
	csrv.Format = readable_color(cdesc.Format);
	csrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	csrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	csrv.Texture2D.MipLevels = 1;
	device_->CreateShaderResourceView(color, &csrv, staging(kSlotColor));

	have_depth_ = false;
	if (depth != nullptr) {
		const D3D12_RESOURCE_DESC ddesc = depth->GetDesc();
		if ((ddesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0) {
			D3D12_SHADER_RESOURCE_VIEW_DESC dsrv{};
			dsrv.Format = readable_depth(ddesc.Format);
			dsrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			dsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			dsrv.Texture2D.MipLevels = 1;
			device_->CreateShaderResourceView(depth, &dsrv, staging(kSlotDepth));
			have_depth_ = true;
		}
	}
	if (!have_depth_) {
		D3D12_SHADER_RESOURCE_VIEW_DESC nd{};
		nd.Format = DXGI_FORMAT_R32_FLOAT;
		nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		nd.Texture2D.MipLevels = 1;
		device_->CreateShaderResourceView(nullptr, &nd, staging(kSlotDepth));
	}

	parity_ ^= 1u;
	if (frames_ < 4u)
		++frames_;
	const uint32_t first = has_history() ? 0u : 1u;

	Tex &cur = luma_[parity_];
	Tex &prev = luma_[parity_ ^ 1u];

	const D3D12_RESOURCE_STATES read_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	const D3D12_RESOURCE_STATES write_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

	if (color_state != read_state)
		barrier(cmd, color, color_state, read_state);
	if (have_depth_ && depth_state != read_state)
		barrier(cmd, depth, depth_state, read_state);

	to_state(cmd, prev, read_state);
	to_state(cmd, cur, write_state);

	prof_count_ = 0;
	if (prof_on_)
		cmd->EndQuery(prof_heap_, D3D12_QUERY_TYPE_TIMESTAMP, 0);

	Constants c{};
	c.inv_full_x = 1.0f / static_cast<float>(width_);
	c.inv_full_y = 1.0f / static_cast<float>(height_);
	c.has_depth = have_depth_ ? 1u : 0u;
	c.first = first;
	c.filters = launcher_filters ? (frames_ >= 3u ? 3u : 1u) : 0u;
	c.dejitter_x = jitter_x_px * c.inv_full_x;
	c.dejitter_y = jitter_y_px * c.inv_full_y;
	c.colour_space = colour_space_;
	const bool same_positions = jitter_x_px == prev_jitter_x_ && jitter_y_px == prev_jitter_y_;
	prev_jitter_x_ = jitter_x_px;
	prev_jitter_y_ = jitter_y_px;
	const bool publish = publish_want && (same_positions || skipped_ != 0u || first != 0u);
	const bool cross = publish_want && !publish && camera_model && model_ok() && !model_cold_;
	c.model_flags = same_positions ? 2u : (cross ? 4u : 0u);
	skipped_ = publish ? 0u : skipped_ + 1u;

	{
		Constants l = c;
		l.dst_w = width_;
		l.dst_h = height_;
		l.inv_dst_x = c.inv_full_x;
		l.inv_dst_y = c.inv_full_y;
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[8] = {
			staging(kSlotColor), D3D12_CPU_DESCRIPTOR_HANDLE{}, D3D12_CPU_DESCRIPTOR_HANDLE{},
			D3D12_CPU_DESCRIPTOR_HANDLE{}, D3D12_CPU_DESCRIPTOR_HANDLE{}, D3D12_CPU_DESCRIPTOR_HANDLE{},
			staging(kSlotDepth), D3D12_CPU_DESCRIPTOR_HANDLE{}
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[2] = {
			D3D12_CPU_DESCRIPTOR_HANDLE{}, staging(kSlotLumaUav + parity_ * kMaxLumaMips)
		};
		dispatch(cmd, kLuma, l, srvs, 8, uavs, 2, groups(width_, 8), groups(height_, 8));
	}

	for (uint32_t m = 1; m < cur.mips; ++m) {
		barrier_mip(cmd, cur.res, m - 1u, write_state, read_state);
		Constants d = c;
		d.dst_w = (std::max)(1u, width_ >> m);
		d.dst_h = (std::max)(1u, height_ >> m);
		d.inv_dst_x = 1.0f / static_cast<float>(d.dst_w);
		d.inv_dst_y = 1.0f / static_cast<float>(d.dst_h);
		d.src_mip = 0u;
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[8] = {
			staging(kSlotColor),
			staging(kSlotLumaMipSrv + parity_ * kMaxLumaMips + (m - 1u)),
			D3D12_CPU_DESCRIPTOR_HANDLE{}, D3D12_CPU_DESCRIPTOR_HANDLE{}, D3D12_CPU_DESCRIPTOR_HANDLE{},
			D3D12_CPU_DESCRIPTOR_HANDLE{}, staging(kSlotDepth), D3D12_CPU_DESCRIPTOR_HANDLE{}
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[2] = {
			D3D12_CPU_DESCRIPTOR_HANDLE{}, staging(kSlotLumaUav + parity_ * kMaxLumaMips + m)
		};
		dispatch(cmd, kDownsample, d, srvs, 8, uavs, 2, groups(d.dst_w, 8), groups(d.dst_h, 8));
	}
	barrier_mip(cmd, cur.res, cur.mips - 1u, write_state, read_state);
	cur.state = read_state;

	to_state(cmd, prev_flow_, read_state);
	to_state(cmd, global_, read_state);

	{
		Constants t = c;
		t.dst_w = l4_.w;
		t.dst_h = l4_.h;
		t.inv_dst_x = 1.0f / static_cast<float>(l4_.w);
		t.inv_dst_y = 1.0f / static_cast<float>(l4_.h);
		t.mip1 = 5u;
		t.mip2 = 5u;
		t.texel1_x = 32.0f * c.inv_full_x;
		t.texel1_y = 32.0f * c.inv_full_y;
		t.texel2_x = t.texel1_x;
		t.texel2_y = t.texel1_y;
		to_state(cmd, l4_, write_state);
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[8] = {
			staging(kSlotColor), staging(kSlotLumaSrv + parity_),
			staging(kSlotLumaSrv + (parity_ ^ 1u)), D3D12_CPU_DESCRIPTOR_HANDLE{},
			srv_of(prev_flow_), srv_of(global_), staging(kSlotDepth), D3D12_CPU_DESCRIPTOR_HANDLE{}
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[2] = { uav_of(l4_), D3D12_CPU_DESCRIPTOR_HANDLE{} };
		dispatch(cmd, kCoarseTop, t, srvs, 8, uavs, 2, groups(l4_.w, 8), groups(l4_.h, 8));
	}

	level(cmd, l4_, l3_[0], l3_[1], 4u, 4u, first);
	level(cmd, l3_[1], l2_[0], l2_[1], 3u, 3u, first);
	level(cmd, l2_[1], l1_[0], l1_[1], 2u, 2u, first);
	level(cmd, l1_[1], l0_[0], l0_[1], 1u, 1u, first);

	{
		Constants d = c;
		d.dst_w = dense_[0].w;
		d.dst_h = dense_[0].h;
		d.inv_dst_x = 1.0f / static_cast<float>(dense_[0].w);
		d.inv_dst_y = 1.0f / static_cast<float>(dense_[0].h);
		d.inv_src_x = 1.0f / static_cast<float>(l0_[1].w);
		d.inv_src_y = 1.0f / static_cast<float>(l0_[1].h);
		d.mip1 = 1u;
		d.mip2 = 0u;
		d.texel1_x = 2.0f * c.inv_full_x;
		d.texel1_y = 2.0f * c.inv_full_y;
		d.texel2_x = c.inv_full_x;
		d.texel2_y = c.inv_full_y;
		to_state(cmd, l0_[1], read_state);
		to_state(cmd, dense_[0], write_state);
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[8] = {
			staging(kSlotColor), staging(kSlotLumaSrv + parity_),
			staging(kSlotLumaSrv + (parity_ ^ 1u)), srv_of(l0_[1]),
			srv_of(prev_flow_), srv_of(global_), staging(kSlotDepth), D3D12_CPU_DESCRIPTOR_HANDLE{}
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[2] = { uav_of(dense_[0]), D3D12_CPU_DESCRIPTOR_HANDLE{} };
		dispatch(cmd, kCoarse, d, srvs, 8, uavs, 2, groups(d.dst_w, 8), groups(d.dst_h, 8));
	}

	Tex &dense_src = launcher_filters ? dense_m_ : dense_[0];
	if (launcher_filters) {
		Constants m = c;
		m.dst_w = dense_m_.w;
		m.dst_h = dense_m_.h;
		m.inv_dst_x = 1.0f / static_cast<float>(dense_m_.w);
		m.inv_dst_y = 1.0f / static_cast<float>(dense_m_.h);
		m.inv_src_x = m.inv_dst_x;
		m.inv_src_y = m.inv_dst_y;
		to_state(cmd, dense_[0], read_state);
		to_state(cmd, dense_m_, write_state);
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[8] = {
			staging(kSlotColor), staging(kSlotLumaSrv + parity_),
			staging(kSlotLumaSrv + (parity_ ^ 1u)), srv_of(dense_[0]),
			srv_of(prev_flow_), srv_of(global_), staging(kSlotDepth), D3D12_CPU_DESCRIPTOR_HANDLE{}
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[2] = { uav_of(dense_m_), D3D12_CPU_DESCRIPTOR_HANDLE{} };
		dispatch(cmd, kMedian, m, srvs, 8, uavs, 2, groups(m.dst_w, 8), groups(m.dst_h, 8));
	}

	{
		Constants f = c;
		f.dst_w = conf_quarter_.w;
		f.dst_h = conf_quarter_.h;
		f.inv_dst_x = 1.0f / static_cast<float>(conf_quarter_.w);
		f.inv_dst_y = 1.0f / static_cast<float>(conf_quarter_.h);
		f.inv_src_x = f.inv_dst_x;
		f.inv_src_y = f.inv_dst_y;
		to_state(cmd, dense_src, read_state);
		to_state(cmd, prev_conf_, read_state);
		to_state(cmd, conf_quarter_, write_state);
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[8] = {
			staging(kSlotColor), staging(kSlotLumaSrv + parity_),
			staging(kSlotLumaSrv + (parity_ ^ 1u)), srv_of(dense_src),
			srv_of(prev_flow_), srv_of(global_), staging(kSlotDepth), srv_of(prev_conf_)
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[2] = { D3D12_CPU_DESCRIPTOR_HANDLE{}, uav_of(conf_quarter_) };
		dispatch(cmd, kConfidence, f, srvs, 8, uavs, 2, groups(f.dst_w, 8), groups(f.dst_h, 8));
	}

	to_state(cmd, conf_quarter_, read_state);

	{
		Constants r = c;
		r.dst_w = dense_[1].w;
		r.dst_h = dense_[1].h;
		r.inv_dst_x = 1.0f / static_cast<float>(dense_[1].w);
		r.inv_dst_y = 1.0f / static_cast<float>(dense_[1].h);
		r.inv_src_x = r.inv_dst_x;
		r.inv_src_y = r.inv_dst_y;
		r.mip1 = 2u;
		r.texel1_x = 4.0f * c.inv_full_x;
		r.texel1_y = 4.0f * c.inv_full_y;
		to_state(cmd, dense_src, read_state);
		to_state(cmd, dense_[1], write_state);
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[8] = {
			staging(kSlotColor), staging(kSlotLumaSrv + parity_),
			staging(kSlotLumaSrv + (parity_ ^ 1u)), srv_of(dense_src),
			srv_of(prev_flow_), srv_of(global_), staging(kSlotDepth), srv_of(conf_quarter_)
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[2] = { uav_of(dense_[1]), D3D12_CPU_DESCRIPTOR_HANDLE{} };
		dispatch(cmd, kRefine, r, srvs, 8, uavs, 2, groups(r.dst_w, 8), groups(r.dst_h, 8));
	}

	{
		Constants g = c;
		g.dst_w = 1u;
		g.dst_h = 1u;
		g.inv_dst_x = 1.0f;
		g.inv_dst_y = 1.0f;
		to_state(cmd, global_, write_state);
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[8] = {
			staging(kSlotColor), staging(kSlotLumaSrv + parity_),
			staging(kSlotLumaSrv + (parity_ ^ 1u)), srv_of(l0_[1]),
			srv_of(prev_flow_), D3D12_CPU_DESCRIPTOR_HANDLE{}, staging(kSlotDepth), D3D12_CPU_DESCRIPTOR_HANDLE{}
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[2] = { uav_of(global_), D3D12_CPU_DESCRIPTOR_HANDLE{} };
		dispatch(cmd, kGlobal, g, srvs, 8, uavs, 2, 1, 1);
	}

	to_state(cmd, dense_[1], read_state);
	to_state(cmd, global_, read_state);

	model_ran_ = false;
	if (cross) {
		record_camera_model(cmd, c);
		model_ran_ = true;
	} else if (!publish) {
	} else if (camera_model && model_ok() && first == 0u) {
		record_camera_model(cmd, c);
		model_ran_ = true;
		model_cold_ = false;
	} else {
		model_cold_ = true;
		Constants e = c;
		e.dst_w = width_;
		e.dst_h = height_;
		e.inv_dst_x = c.inv_full_x;
		e.inv_dst_y = c.inv_full_y;
		to_state(cmd, motion_, write_state);
		to_state(cmd, confidence_, write_state);
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[8] = {
			staging(kSlotColor), staging(kSlotLumaSrv + parity_),
			staging(kSlotLumaSrv + (parity_ ^ 1u)), srv_of(dense_[1]),
			srv_of(prev_flow_), srv_of(global_), staging(kSlotDepth), srv_of(conf_quarter_)
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[2] = { uav_of(motion_), uav_of(confidence_) };
		dispatch(cmd, kExport, e, srvs, 8, uavs, 2, groups(width_, 8), groups(height_, 8));
	}

	{
		Constants h = c;
		h.dst_w = prev_flow_.w;
		h.dst_h = prev_flow_.h;
		h.inv_dst_x = 1.0f / static_cast<float>(prev_flow_.w);
		h.inv_dst_y = 1.0f / static_cast<float>(prev_flow_.h);
		to_state(cmd, prev_flow_, write_state);
		to_state(cmd, prev_conf_, write_state);
		const D3D12_CPU_DESCRIPTOR_HANDLE srvs[8] = {
			staging(kSlotColor), staging(kSlotLumaSrv + parity_),
			staging(kSlotLumaSrv + (parity_ ^ 1u)), srv_of(dense_[1]),
			D3D12_CPU_DESCRIPTOR_HANDLE{}, srv_of(global_), staging(kSlotDepth), srv_of(conf_quarter_)
		};
		const D3D12_CPU_DESCRIPTOR_HANDLE uavs[2] = { uav_of(prev_flow_), uav_of(prev_conf_) };
		dispatch(cmd, kCopyFlow, h, srvs, 8, uavs, 2, groups(h.dst_w, 8), groups(h.dst_h, 8));
	}

	to_state(cmd, motion_, kPublishedState);
	to_state(cmd, confidence_, kPublishedState);
	to_state(cmd, global_, kPublishedState);
	to_state(cmd, theta_[0], kPublishedState);
	to_state(cmd, theta_pub_, kPublishedState);
	to_state(cmd, alpha_q_, kPublishedState);

	if (have_depth_ && depth_state != read_state)
		barrier(cmd, depth, read_state, depth_state);
	if (color_state != read_state)
		barrier(cmd, color, read_state, color_state);
	if (prof_on_ && prof_count_ != 0)
		cmd->ResolveQueryData(prof_heap_, D3D12_QUERY_TYPE_TIMESTAMP, 0, prof_count_ + 1u, prof_readback_, 0);
	return true;
}

}
