#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"

#include <d3dcompiler.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace aeon_sr {

namespace {

struct JournalEntry {
	const wchar_t *note = nullptr;
	ID3D12Resource *res = nullptr;
	uint32_t w = 0, h = 0, fmt = 0, flags = 0;
	uint32_t before = 0, after = 0;
	uint32_t tid = 0;
	uint64_t seq = 0;
};

constexpr uint32_t kJournalSlots = 64;
JournalEntry g_journal[kJournalSlots];
std::atomic<uint64_t> g_journal_seq{ 0 };

void journal_put(const JournalEntry &e) noexcept
{
	const uint64_t seq = g_journal_seq.fetch_add(1, std::memory_order_relaxed) + 1;
	JournalEntry &slot = g_journal[seq % kJournalSlots];
	slot = e;
	slot.tid = GetCurrentThreadId();
	slot.seq = seq;
}

}

void engine_journal_note(const wchar_t *note) noexcept
{
	JournalEntry e;
	e.note = note;
	journal_put(e);
}

void engine_journal_dump() noexcept
{
	const uint64_t last = g_journal_seq.load(std::memory_order_relaxed);
	const uint64_t first = last > kJournalSlots ? last - kJournalSlots + 1 : 1;
	for (uint64_t seq = first; seq <= last; ++seq) {
		const JournalEntry e = g_journal[seq % kJournalSlots];
		if (e.seq != seq)
			continue;
		if (e.note != nullptr)
			diag_logf(DiagLevel::Error, "crash", L"  #%llu t%u -- %s", static_cast<unsigned long long>(seq),
				e.tid, e.note);
		else
			diag_logf(DiagLevel::Error, "crash", L"  #%llu t%u barrier %p %ux%u fmt %u flags 0x%x: 0x%x -> 0x%x",
				static_cast<unsigned long long>(seq), e.tid, static_cast<void *>(e.res), e.w, e.h, e.fmt,
				e.flags, e.before, e.after);
	}
}

bool reads_like_copy12(ID3D12Resource *res, DXGI_FORMAT as) noexcept
{
	if (res == nullptr)
		return false;
	const D3D12_RESOURCE_DESC d = res->GetDesc();
	return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count == 1 &&
		d.DepthOrArraySize == 1 && (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0 && d.Format == as;
}

void barrier12(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	if (before == after) return;
	{
		JournalEntry e;
		e.res = res;
		if (res != nullptr) {
			const D3D12_RESOURCE_DESC d = res->GetDesc();
			e.w = static_cast<uint32_t>(d.Width);
			e.h = d.Height;
			e.fmt = static_cast<uint32_t>(d.Format);
			e.flags = static_cast<uint32_t>(d.Flags);
		}
		e.before = static_cast<uint32_t>(before);
		e.after = static_cast<uint32_t>(after);
		journal_put(e);
	}
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	cmd->ResourceBarrier(1, &b);
}

bool create_tex12(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
	bool need_uav, D3D12_RESOURCE_STATES initial, ID3D12Resource **out)
{
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w; d.Height = h;
	d.DepthOrArraySize = 1; d.MipLevels = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if (need_uav)
		d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, initial, nullptr, IID_PPV_ARGS(out));
	if (FAILED(hr)) {
		wchar_t buf[128]{};
		_snwprintf_s(buf, _TRUNCATE, L"CreateCommittedResource failed hr=0x%08X fmt=%u", static_cast<unsigned>(hr), static_cast<unsigned>(fmt));
		diag_error("d3d12", buf);
		return false;
	}
	return true;
}

namespace {

D3D12_GRAPHICS_PIPELINE_STATE_DESC base_pso_desc(ID3D12RootSignature *rs, ID3DBlob *vs, ID3DBlob *ps, DXGI_FORMAT rtv)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
	d.pRootSignature = rs;
	d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
	d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
	d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	d.SampleMask = UINT_MAX;
	d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	d.RasterizerState.DepthClipEnable = TRUE;
	d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	d.NumRenderTargets = 1;
	d.RTVFormats[0] = rtv;
	d.SampleDesc.Count = 1;
	return d;
}

void release_pso(BlitPipelineD3D12::PsoPair &pp)
{
	for (ID3D12PipelineState **s : { &pp.blit, &pp.debug, &pp.proxy, &pp.guide, &pp.delta,
			&pp.rebuild, &pp.smooth, &pp.slope, &pp.neural, &pp.debug_view }) {
		if (*s) { (*s)->Release(); *s = nullptr; }
	}
	pp.rtv = DXGI_FORMAT_UNKNOWN;
}

}

namespace {

constexpr const char *kBlitEntries[] = { "VSMain", "PSMain", "PSDebug", "PSNeuralProxy", "PSNeuralGuide",
	"PSNeuralDelta", "PSNeuralRebuild", "PSNeuralSmooth", "PSNeuralSlope", "PSNeuralComposite" };
constexpr size_t kBlitShaderCount = sizeof(kBlitEntries) / sizeof(kBlitEntries[0]);

std::mutex g_blit_shaders_lock;
ID3DBlob *g_blit_shaders[kBlitShaderCount] = {};

bool shared_blit_shaders(ID3DBlob *(&out)[kBlitShaderCount])
{
	std::lock_guard<std::mutex> lock(g_blit_shaders_lock);
	if (g_blit_shaders[kBlitShaderCount - 1] == nullptr) {
		LARGE_INTEGER freq{}, begin{}, end{};
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&begin);
		ID3DBlob *made[kBlitShaderCount] = {};
		ID3DBlob *errors[kBlitShaderCount] = {};
		HRESULT results[kBlitShaderCount] = {};
		const auto compile_from = [&](size_t first, size_t step) {
			for (size_t i = first; i < kBlitShaderCount; i += step)
				results[i] = D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "aeon_blit12", nullptr, nullptr, kBlitEntries[i],
					i == 0 ? "vs_5_0" : "ps_5_0", 0, 0, &made[i], &errors[i]);
		};
		constexpr size_t kThreads = 4;
		std::thread helpers[kThreads - 1];
		for (size_t t = 1; t < kThreads; ++t) {
			try {
				helpers[t - 1] = std::thread(compile_from, t, kThreads);
			} catch (...) {
			}
		}
		compile_from(0, kThreads);
		for (size_t t = 1; t < kThreads; ++t) {
			if (helpers[t - 1].joinable())
				helpers[t - 1].join();
			else
				compile_from(t, kThreads);
		}
		bool all = true;
		for (size_t i = 0; i < kBlitShaderCount; ++i) {
			if (FAILED(results[i]) || made[i] == nullptr) {
				all = false;
				diag_logf(DiagLevel::Error, "shader", L"d3d12 blit shader %hs failed to compile%hs%hs", kBlitEntries[i],
					errors[i] != nullptr ? ": " : "",
					errors[i] != nullptr ? static_cast<const char *>(errors[i]->GetBufferPointer()) : "");
			}
			if (errors[i] != nullptr)
				errors[i]->Release();
		}
		if (!all) {
			for (ID3DBlob *b : made)
				if (b != nullptr)
					b->Release();
			return false;
		}
		for (size_t i = 0; i < kBlitShaderCount; ++i)
			g_blit_shaders[i] = made[i];
		QueryPerformanceCounter(&end);
		diag_logf(DiagLevel::Info, "shader", L"d3d12 blit: %zu shaders compiled in %.0f ms, once for every upscaler",
			kBlitShaderCount, freq.QuadPart != 0
				? 1000.0 * static_cast<double>(end.QuadPart - begin.QuadPart) / static_cast<double>(freq.QuadPart)
				: 0.0);
	}
	for (size_t i = 0; i < kBlitShaderCount; ++i) {
		out[i] = g_blit_shaders[i];
		out[i]->AddRef();
	}
	return true;
}

}

void blit12_warm_shaders()
{
	ID3DBlob *shaders[kBlitShaderCount] = {};
	if (shared_blit_shaders(shaders))
		for (ID3DBlob *b : shaders)
			b->Release();
}

bool BlitPipelineD3D12::ensure(ID3D12Device *device)
{
	if (blit_ready)
		return true;
	if (device == nullptr)
		return false;

	ID3DBlob *shaders[kBlitShaderCount] = {};
	if (!shared_blit_shaders(shaders))
		return false;
	vs_blob = shaders[0]; ps_blit_blob = shaders[1]; ps_debug_blob = shaders[2];
	ps_proxy_blob = shaders[3]; ps_guide_blob = shaders[4]; ps_delta_blob = shaders[5];
	ps_rebuild_blob = shaders[6]; ps_smooth_blob = shaders[7]; ps_slope_blob = shaders[8];
	ps_neural_blob = shaders[9];

	D3D12_DESCRIPTOR_RANGE range{};
	range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	range.NumDescriptors = kSrvPerSlot;
	range.BaseShaderRegister = 0;

	D3D12_ROOT_PARAMETER rp[3]{};
	rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rp[0].Constants.ShaderRegister = 0;
	rp[0].Constants.Num32BitValues = kBlitConstantCount;
	rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rp[1].DescriptorTable.NumDescriptorRanges = 1;
	rp[1].DescriptorTable.pDescriptorRanges = &range;
	rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rp[2].Constants.ShaderRegister = 1;
	rp[2].Constants.Num32BitValues = kNeuralConstantCount;
	rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC samp{};
	samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	samp.MaxLOD = D3D12_FLOAT32_MAX;
	samp.ShaderRegister = 0;
	samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rs{};
	rs.NumParameters = 3; rs.pParameters = rp;
	rs.NumStaticSamplers = 1; rs.pStaticSamplers = &samp;

	ID3DBlob *sig = nullptr, *sig_err = nullptr;
	if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sig_err))) {
		if (sig_err) sig_err->Release();
		release();
		return false;
	}
	const HRESULT rs_hr = device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_sig));
	sig->Release();
	if (FAILED(rs_hr)) { release(); return false; }

	D3D12_DESCRIPTOR_HEAP_DESC hd{};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	hd.NumDescriptors = kD3D12RingSlots * kSrvPerSlot;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap)))) { release(); return false; }
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hd.NumDescriptors = kD3D12RingSlots;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)))) { release(); return false; }
	srv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	blit_ready = true;
	return true;
}

void BlitPipelineD3D12::release()
{
	for (PsoPair &pp : pso_cache)
		release_pso(pp);
	auto rel_blob = [](void *&b) { if (b) { static_cast<ID3DBlob *>(b)->Release(); b = nullptr; } };
	rel_blob(vs_blob); rel_blob(ps_blit_blob); rel_blob(ps_debug_blob);
	rel_blob(ps_proxy_blob); rel_blob(ps_guide_blob); rel_blob(ps_delta_blob);
	rel_blob(ps_rebuild_blob); rel_blob(ps_smooth_blob); rel_blob(ps_neural_blob);
	rel_blob(ps_slope_blob); rel_blob(ps_debug_view_blob);
	if (root_sig) { root_sig->Release(); root_sig = nullptr; }
	if (srv_heap) { srv_heap->Release(); srv_heap = nullptr; }
	if (rtv_heap) { rtv_heap->Release(); rtv_heap = nullptr; }
	ring_index = 0;
	blit_ready = false;
}

BlitPipelineD3D12::PsoPair *BlitPipelineD3D12::pso_pair(ID3D12Device *device, DXGI_FORMAT rtv_fmt)
{
	(void)device;
	PsoPair *pair = nullptr;
	for (PsoPair &pp : pso_cache) {
		if (pp.rtv == rtv_fmt) { pair = &pp; break; }
		if (pp.rtv == DXGI_FORMAT_UNKNOWN && pair == nullptr) pair = &pp;
	}
	if (pair == nullptr)
		return nullptr;
	pair->rtv = rtv_fmt;
	return pair;
}

ID3D12PipelineState *BlitPipelineD3D12::pso_of(ID3D12Device *device, PsoPair &pair,
	ID3D12PipelineState *PsoPair::*slot, void *ps_blob)
{
	ID3D12PipelineState *&pso = pair.*slot;
	if (pso == nullptr && ps_blob != nullptr) {
		const D3D12_GRAPHICS_PIPELINE_STATE_DESC d = base_pso_desc(root_sig,
			static_cast<ID3DBlob *>(vs_blob), static_cast<ID3DBlob *>(ps_blob), pair.rtv);
		if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) {
			pso = nullptr;
			bool any = false;
			for (ID3D12PipelineState *p : { pair.blit, pair.debug, pair.proxy, pair.guide, pair.delta,
					pair.rebuild, pair.smooth, pair.slope, pair.neural, pair.debug_view })
				any = any || p != nullptr;
			if (!any)
				pair.rtv = DXGI_FORMAT_UNKNOWN;
		}
	}
	return pso;
}

bool BlitPipelineD3D12::prepare(ID3D12Device *device, std::initializer_list<DXGI_FORMAT> targets)
{
	if (!ensure(device))
		return false;
	for (const DXGI_FORMAT f : targets) {
		if (f == DXGI_FORMAT_UNKNOWN)
			continue;
		PsoPair *const pair = pso_pair(device, view_format_for(f));
		if (pair == nullptr ||
			pso_of(device, *pair, &PsoPair::blit, ps_blit_blob) == nullptr ||
			pso_of(device, *pair, &PsoPair::debug, ps_debug_blob) == nullptr ||
			pso_of(device, *pair, &PsoPair::proxy, ps_proxy_blob) == nullptr ||
			pso_of(device, *pair, &PsoPair::guide, ps_guide_blob) == nullptr ||
			pso_of(device, *pair, &PsoPair::delta, ps_delta_blob) == nullptr ||
			pso_of(device, *pair, &PsoPair::rebuild, ps_rebuild_blob) == nullptr ||
			pso_of(device, *pair, &PsoPair::smooth, ps_smooth_blob) == nullptr ||
			pso_of(device, *pair, &PsoPair::slope, ps_slope_blob) == nullptr ||
			pso_of(device, *pair, &PsoPair::neural, ps_neural_blob) == nullptr)
			return false;
	}
	return true;
}

BlitPipelineD3D12::PsoPair *BlitPipelineD3D12::prepare_draw(ID3D12Device *device,
	ID3D12GraphicsCommandList *cmd, ID3D12Resource *t0, ID3D12Resource *t1, ID3D12Resource *t2, ID3D12Resource *t3,
	ID3D12Resource *dst, DXGI_FORMAT dst_format, uint32_t dst_w, uint32_t dst_h)
{
	const DXGI_FORMAT rtv_fmt = view_format_for(dst_format);
	PsoPair *const pair = pso_pair(device, rtv_fmt);
	if (pair == nullptr)
		return nullptr;

	ring_index = (ring_index + 1) % kD3D12RingSlots;
	D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
	srv_cpu.ptr += static_cast<SIZE_T>(ring_index) * kSrvPerSlot * srv_stride;
	D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
	srv_gpu.ptr += static_cast<UINT64>(ring_index) * kSrvPerSlot * srv_stride;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv_cpu = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	rtv_cpu.ptr += static_cast<SIZE_T>(ring_index) * rtv_stride;

	auto write_srv = [device](D3D12_CPU_DESCRIPTOR_HANDLE at, ID3D12Resource *res) {
		D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = res ? view_format_for(res->GetDesc().Format) : DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sd.Texture2D.MipLevels = 1;
		device->CreateShaderResourceView(res, &sd, at);
	};
	D3D12_CPU_DESCRIPTOR_HANDLE h = srv_cpu;
	write_srv(h, t0); h.ptr += srv_stride;
	write_srv(h, t1); h.ptr += srv_stride;
	write_srv(h, t2); h.ptr += srv_stride;
	write_srv(h, t3);

	D3D12_RENDER_TARGET_VIEW_DESC rd{};
	rd.Format = rtv_fmt;
	rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(dst, &rd, rtv_cpu);

	const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(dst_w), static_cast<float>(dst_h), 0.0f, 1.0f };
	const D3D12_RECT sc{ 0, 0, static_cast<LONG>(dst_w), static_cast<LONG>(dst_h) };

	cmd->SetGraphicsRootSignature(root_sig);
	cmd->SetDescriptorHeaps(1, &srv_heap);
	cmd->SetGraphicsRootDescriptorTable(1, srv_gpu);
	cmd->OMSetRenderTargets(1, &rtv_cpu, FALSE, nullptr);
	cmd->RSSetViewports(1, &vp);
	cmd->RSSetScissorRects(1, &sc);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	return pair;
}

bool BlitPipelineD3D12::draw_fullscreen(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *effect,
	ID3D12Resource *dst, DXGI_FORMAT dst_format, uint32_t dst_w, uint32_t dst_h,
	float sharpness, uint32_t debug_mode, float jitter_u, float jitter_v, const float *valid_uv, bool catmull_rom,
	const D3D12_RECT *scissor)
{
	if (!ensure(device) || cmd == nullptr || effect == nullptr || dst == nullptr)
		return false;

	{
		PsoPair *const target = pso_pair(device, view_format_for(dst_format));
		if (target == nullptr || (debug_mode != 0
				? pso_of(device, *target, &PsoPair::debug, ps_debug_blob)
				: pso_of(device, *target, &PsoPair::blit, ps_blit_blob)) == nullptr)
			return false;
	}
	PsoPair *const pair = prepare_draw(device, cmd, effect, nullptr, nullptr, nullptr, dst, dst_format, dst_w, dst_h);
	if (pair == nullptr)
		return false;
	if (scissor != nullptr)
		cmd->RSSetScissorRects(1, scissor);

	const float constants[kBlitConstantCount] = {
		0.0f, 0.0f,
		static_cast<float>(dst_w), static_cast<float>(dst_h),
		sharpness, static_cast<float>(debug_mode), catmull_rom ? 1.0f : 0.0f, 0.0f,
		jitter_u, jitter_v,
		0.0f, 0.0f,
		valid_uv != nullptr ? valid_uv[0] : 0.0f, valid_uv != nullptr ? valid_uv[1] : 0.0f,
		valid_uv != nullptr ? valid_uv[2] : 0.0f, valid_uv != nullptr ? valid_uv[3] : 0.0f };

	cmd->SetGraphicsRoot32BitConstants(0, kBlitConstantCount, constants, 0);
	cmd->SetPipelineState(debug_mode != 0 ? pair->debug : pair->blit);
	cmd->DrawInstanced(3, 1, 0, 0);
	return true;
}

bool BlitPipelineD3D12::draw_debug_view(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *field, ID3D12Resource *frame, ID3D12Resource *dst, DXGI_FORMAT dst_format,
	uint32_t dst_w, uint32_t dst_h, uint32_t mode, float cell_px, float depth_far, bool depth_reversed)
{
	if (!ensure(device) || cmd == nullptr || field == nullptr || frame == nullptr || dst == nullptr || mode == 0)
		return false;
	if (ps_debug_view_blob == nullptr) {
		ID3DBlob *blob = nullptr, *err = nullptr;
		if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "aeon_blit12", nullptr, nullptr, "PSDebugView", "ps_5_0",
				0, 0, &blob, &err))) {
			if (err) { diag_error("shader", L"d3d12 debug view PS compile failed"); err->Release(); }
			return false;
		}
		ps_debug_view_blob = blob;
	}

	PsoPair *const pair = prepare_draw(device, cmd, field, frame, nullptr, nullptr, dst, dst_format, dst_w, dst_h);
	if (pair == nullptr)
		return false;
	if (pair->debug_view == nullptr) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC d = base_pso_desc(root_sig, static_cast<ID3DBlob *>(vs_blob),
			static_cast<ID3DBlob *>(ps_debug_view_blob), pair->rtv);
		if (FAILED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pair->debug_view))))
			return false;
	}

	const float constants[kBlitConstantCount] = {
		cell_px, 0.0f,
		static_cast<float>(dst_w), static_cast<float>(dst_h),
		0.0f, static_cast<float>(mode), 0.0f, 0.0f,
		0.0f, 0.0f,
		depth_far, depth_reversed ? 1.0f : 0.0f,
		0.0f, 0.0f, 0.0f, 0.0f };
	cmd->SetGraphicsRoot32BitConstants(0, kBlitConstantCount, constants, 0);
	cmd->SetPipelineState(pair->debug_view);
	cmd->DrawInstanced(3, 1, 0, 0);
	return true;
}

bool BlitPipelineD3D12::clear(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst,
	DXGI_FORMAT dst_format, const float rgba[4])
{
	if (!ensure(device) || cmd == nullptr || dst == nullptr || rgba == nullptr)
		return false;
	ring_index = (ring_index + 1) % kD3D12RingSlots;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += static_cast<SIZE_T>(ring_index) * rtv_stride;
	D3D12_RENDER_TARGET_VIEW_DESC rd{};
	rd.Format = view_format_for(dst_format);
	rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	device->CreateRenderTargetView(dst, &rd, rtv);
	cmd->ClearRenderTargetView(rtv, rgba, 0, nullptr);
	return true;
}

bool BlitPipelineD3D12::draw_neural(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, NeuralPass pass,
	ID3D12Resource *src, ID3D12Resource *orig, ID3D12Resource *proxy, ID3D12Resource *aux,
	ID3D12Resource *dst, DXGI_FORMAT dst_format, uint32_t dst_w, uint32_t dst_h,
	const NeuralDrawConstants &c)
{
	if (!ensure(device) || cmd == nullptr || src == nullptr || dst == nullptr)
		return false;

	ID3D12PipelineState *PsoPair::*slot = &PsoPair::neural;
	void *blob = ps_neural_blob;
	switch (pass) {
	case NeuralPass::Proxy: slot = &PsoPair::proxy; blob = ps_proxy_blob; break;
	case NeuralPass::Guide: slot = &PsoPair::guide; blob = ps_guide_blob; break;
	case NeuralPass::Delta: slot = &PsoPair::delta; blob = ps_delta_blob; break;
	case NeuralPass::Rebuild: slot = &PsoPair::rebuild; blob = ps_rebuild_blob; break;
	case NeuralPass::Smooth: slot = &PsoPair::smooth; blob = ps_smooth_blob; break;
	case NeuralPass::Slope: slot = &PsoPair::slope; blob = ps_slope_blob; break;
	case NeuralPass::Composite: break;
	}
	{
		PsoPair *const target = pso_pair(device, view_format_for(dst_format));
		if (target == nullptr || pso_of(device, *target, slot, blob) == nullptr)
			return false;
	}
	PsoPair *const pair = prepare_draw(device, cmd, src, orig, proxy, aux, dst, dst_format, dst_w, dst_h);
	if (pair == nullptr)
		return false;

	static const float zero[kBlitConstantCount]{};
	static_assert(sizeof(NeuralDrawConstants) == kNeuralConstantCount * sizeof(float),
		"NeuralDrawConstants must stay the DWORDs the root signature reserves");
	cmd->SetGraphicsRoot32BitConstants(0, kBlitConstantCount, zero, 0);
	cmd->SetGraphicsRoot32BitConstants(2, kNeuralConstantCount, &c, 0);
	cmd->SetPipelineState(pair->*slot);
	cmd->DrawInstanced(3, 1, 0, 0);
	return true;
}

}
