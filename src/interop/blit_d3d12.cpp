#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"

#include <d3dcompiler.h>

#include <atomic>
#include <cstring>

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
			&pp.rebuild, &pp.smooth, &pp.neural, &pp.debug_view }) {
		if (*s) { (*s)->Release(); *s = nullptr; }
	}
	pp.rtv = DXGI_FORMAT_UNKNOWN;
}

}

bool BlitPipelineD3D12::ensure(ID3D12Device *device)
{
	if (blit_ready)
		return true;
	if (device == nullptr)
		return false;

	ID3DBlob *vs = nullptr, *psb = nullptr, *psd = nullptr, *err = nullptr;
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "aeon_blit12", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &err))) {
		if (err) { diag_error("shader", L"d3d12 blit VS compile failed"); err->Release(); }
		return false;
	}
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "aeon_blit12", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err))) {
		vs->Release();
		if (err) { diag_error("shader", L"d3d12 blit PS compile failed"); err->Release(); }
		return false;
	}
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "aeon_blit12", nullptr, nullptr, "PSDebug", "ps_5_0", 0, 0, &psd, &err))) {
		vs->Release(); psb->Release();
		if (err) { diag_error("shader", L"d3d12 debug PS compile failed"); err->Release(); }
		return false;
	}

	ID3DBlob *psp = nullptr, *psg = nullptr, *psn = nullptr;
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "aeon_blit12", nullptr, nullptr, "PSNeuralProxy", "ps_5_0", 0, 0, &psp, &err))) {
		vs->Release(); psb->Release(); psd->Release();
		if (err) { diag_error("shader", L"d3d12 neural proxy PS compile failed"); err->Release(); }
		return false;
	}
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "aeon_blit12", nullptr, nullptr, "PSNeuralGuide", "ps_5_0", 0, 0, &psg, &err))) {
		vs->Release(); psb->Release(); psd->Release(); psp->Release();
		if (err) { diag_error("shader", L"d3d12 neural guide PS compile failed"); err->Release(); }
		return false;
	}
	ID3DBlob *psdl = nullptr;
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "aeon_blit12", nullptr, nullptr, "PSNeuralDelta", "ps_5_0", 0, 0, &psdl, &err))) {
		vs->Release(); psb->Release(); psd->Release(); psp->Release(); psg->Release();
		if (err) { diag_error("shader", L"d3d12 neural delta PS compile failed"); err->Release(); }
		return false;
	}
	ID3DBlob *psrb = nullptr;
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "aeon_blit12", nullptr, nullptr, "PSNeuralRebuild", "ps_5_0", 0, 0, &psrb, &err))) {
		psdl->Release();
		vs->Release(); psb->Release(); psd->Release(); psp->Release(); psg->Release();
		if (err) { diag_error("shader", L"d3d12 neural rebuild PS compile failed"); err->Release(); }
		return false;
	}
	ID3DBlob *pssm = nullptr;
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "aeon_blit12", nullptr, nullptr, "PSNeuralSmooth", "ps_5_0", 0, 0, &pssm, &err))) {
		psdl->Release(); psrb->Release();
		vs->Release(); psb->Release(); psd->Release(); psp->Release(); psg->Release();
		if (err) { diag_error("shader", L"d3d12 neural smooth PS compile failed"); err->Release(); }
		return false;
	}
	if (FAILED(D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "aeon_blit12", nullptr, nullptr, "PSNeuralComposite", "ps_5_0", 0, 0, &psn, &err))) {
		psdl->Release(); psrb->Release(); pssm->Release();
		vs->Release(); psb->Release(); psd->Release(); psp->Release(); psg->Release();
		if (err) { diag_error("shader", L"d3d12 neural composite PS compile failed"); err->Release(); }
		return false;
	}
	vs_blob = vs; ps_blit_blob = psb; ps_debug_blob = psd;
	ps_proxy_blob = psp; ps_guide_blob = psg; ps_delta_blob = psdl;
	ps_rebuild_blob = psrb; ps_smooth_blob = pssm; ps_neural_blob = psn;

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
	rel_blob(ps_debug_view_blob);
	if (root_sig) { root_sig->Release(); root_sig = nullptr; }
	if (srv_heap) { srv_heap->Release(); srv_heap = nullptr; }
	if (rtv_heap) { rtv_heap->Release(); rtv_heap = nullptr; }
	ring_index = 0;
	blit_ready = false;
}

BlitPipelineD3D12::PsoPair *BlitPipelineD3D12::prepare_draw(ID3D12Device *device,
	ID3D12GraphicsCommandList *cmd, ID3D12Resource *t0, ID3D12Resource *t1, ID3D12Resource *t2, ID3D12Resource *t3,
	ID3D12Resource *dst, DXGI_FORMAT dst_format, uint32_t dst_w, uint32_t dst_h)
{
	const DXGI_FORMAT rtv_fmt = view_format_for(dst_format);

	PsoPair *pair = nullptr;
	for (PsoPair &pp : pso_cache) {
		if (pp.rtv == rtv_fmt) { pair = &pp; break; }
		if (pp.rtv == DXGI_FORMAT_UNKNOWN && pair == nullptr) pair = &pp;
	}
	if (pair == nullptr)
		return nullptr;
	if (pair->rtv != rtv_fmt) {
		auto make = [&](void *ps, ID3D12PipelineState **out) {
			D3D12_GRAPHICS_PIPELINE_STATE_DESC d = base_pso_desc(root_sig,
				static_cast<ID3DBlob *>(vs_blob), static_cast<ID3DBlob *>(ps), rtv_fmt);
			return SUCCEEDED(device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(out)));
		};
		if (!make(ps_blit_blob, &pair->blit) ||
			!make(ps_debug_blob, &pair->debug) ||
			!make(ps_proxy_blob, &pair->proxy) ||
			!make(ps_guide_blob, &pair->guide) ||
			!make(ps_delta_blob, &pair->delta) ||
			!make(ps_rebuild_blob, &pair->rebuild) ||
			!make(ps_smooth_blob, &pair->smooth) ||
			!make(ps_neural_blob, &pair->neural)) {
			release_pso(*pair);
			return nullptr;
		}
		pair->rtv = rtv_fmt;
	}

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
	float sharpness, uint32_t debug_mode, float jitter_u, float jitter_v, const float *valid_uv, bool catmull_rom)
{
	if (!ensure(device) || cmd == nullptr || effect == nullptr || dst == nullptr)
		return false;

	PsoPair *const pair = prepare_draw(device, cmd, effect, nullptr, nullptr, nullptr, dst, dst_format, dst_w, dst_h);
	if (pair == nullptr)
		return false;

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

	PsoPair *const pair = prepare_draw(device, cmd, src, orig, proxy, aux, dst, dst_format, dst_w, dst_h);
	if (pair == nullptr)
		return false;

	static const float zero[kBlitConstantCount]{};
	static_assert(sizeof(NeuralDrawConstants) == kNeuralConstantCount * sizeof(float),
		"NeuralDrawConstants must stay the DWORDs the root signature reserves");
	cmd->SetGraphicsRoot32BitConstants(0, kBlitConstantCount, zero, 0);
	cmd->SetGraphicsRoot32BitConstants(2, kNeuralConstantCount, &c, 0);
	ID3D12PipelineState *pso = pair->neural;
	switch (pass) {
	case NeuralPass::Proxy: pso = pair->proxy; break;
	case NeuralPass::Guide: pso = pair->guide; break;
	case NeuralPass::Delta: pso = pair->delta; break;
	case NeuralPass::Rebuild: pso = pair->rebuild; break;
	case NeuralPass::Smooth: pso = pair->smooth; break;
	case NeuralPass::Composite: break;
	}
	cmd->SetPipelineState(pso);
	cmd->DrawInstanced(3, 1, 0, 0);
	return true;
}

}
