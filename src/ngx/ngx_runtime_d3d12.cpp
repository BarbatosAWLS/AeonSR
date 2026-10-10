#include "aeon_sr/ngx/ngx_runtime_d3d12.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/ngx/ngx_session_ngx.hpp"

#include <excpt.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <cstring>
#include <mutex>
#include <vector>

namespace aeon_sr {
namespace {

struct SharedNgx12 {
	ID3D12Device *device = nullptr;
	int count = 0;
};
std::mutex g_shared_mutex;
std::vector<SharedNgx12> g_shared;

NVSDK_NGX_Result ngx12_acquire(ID3D12Device *device, const wchar_t *data_dir,
	const NVSDK_NGX_FeatureCommonInfo *info, const wchar_t *tag)
{
	std::lock_guard lock(g_shared_mutex);
	for (SharedNgx12 &s : g_shared) {
		if (s.device == device) {
			++s.count;
			wchar_t buf[96]{};
			_snwprintf_s(buf, _TRUNCATE, L"%sNGX D3D12 Init shared (%d sessions on this device)", tag, s.count);
			diag_info("dlss", buf);
			return NVSDK_NGX_Result_Success;
		}
	}
	note_own_ngx_start();
	NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(ngx_dlss::kAppId, data_dir, device, info, NVSDK_NGX_Version_API);
	diag_info("dlss", std::wstring(tag) + ngx_format_result(L"Init(AppId)", r));
	if (NVSDK_NGX_FAILED(r)) {
		r = NVSDK_NGX_D3D12_Init_with_ProjectID(
			ngx_dlss::kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, ngx_dlss::kEngineVersion,
			data_dir, device, info, NVSDK_NGX_Version_API);
		diag_info("dlss", std::wstring(tag) + ngx_format_result(L"Init_with_ProjectID", r));
	}
	if (!NVSDK_NGX_FAILED(r))
		g_shared.push_back(SharedNgx12{ device, 1 });
	return r;
}

void ngx12_release(ID3D12Device *device, const wchar_t *tag)
{
	std::lock_guard lock(g_shared_mutex);
	for (size_t i = 0; i < g_shared.size(); ++i) {
		if (g_shared[i].device != device)
			continue;
		if (--g_shared[i].count > 0) {
			wchar_t buf[96]{};
			_snwprintf_s(buf, _TRUNCATE, L"%sNGX D3D12 instance kept (%d sessions remain)", tag, g_shared[i].count);
			diag_info("dlss", buf);
			return;
		}
		g_shared.erase(g_shared.begin() + static_cast<ptrdiff_t>(i));
		break;
	}
	NVSDK_NGX_D3D12_Shutdown1(device);
}

}

void NgxRuntimeD3D12::set_command_queue(ID3D12CommandQueue *q)
{
	queue = q;
}

void NgxRuntimeD3D12::wait_gpu()
{
	gpu_fence.signal_and_wait(device, queue);
}

bool NgxRuntimeD3D12::init_device(ID3D12Device *dev, uint32_t w, uint32_t h)
{
	shutdown_device();
	device = dev;
	width = w; height = h;
	crashed = false;

	if (!ensure_dll_present())
		return false;
	if (device == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = L"null D3D12 device";
		return false;
	}

	prepare_data_dir();
	diag_info("dlss", L"=== D3D12 init_device begin ===");
	{
		wchar_t buf[256]{};
		_snwprintf_s(buf, _TRUNCATE, L"addon_dir=%s dll_dir=%s data_dir=%s size=%ux%u",
			addon_dir.c_str(), dll_dir.c_str(), data_dir.c_str(), w, h);
		diag_info("dlss", buf);
	}
	{

		std::wstring found;
		for (const wchar_t *m : { L"sl.interposer.dll", L"sl.dlss.dll", L"sl.dlss_g.dll",
				L"nvngx_dlss.dll", L"nvngx_dlssg.dll", L"_nvngx.dll" }) {
			if (GetModuleHandleW(m) != nullptr) {
				if (!found.empty()) found += L", ";
				found += m;
			}
		}
		game_ngx_modules = found;
		diag_info("dlss", found.empty()
			? std::wstring(L"pre-init NGX modules in process: none")
			: (L"pre-init NGX modules in process: " + found));
	}

	const std::wstring search_dir = !dll_dir.empty() ? dll_dir : addon_dir;
	const wchar_t *path_list[] = { search_dir.c_str() };
	NVSDK_NGX_FeatureCommonInfo feature_info{};
	feature_info.PathListInfo.Path = path_list;
	feature_info.PathListInfo.Length = 1;
	feature_info.LoggingInfo.LoggingCallback = ngx_log_callback;
	feature_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
	feature_info.LoggingInfo.DisableOtherLoggingSinks = false;

	const NVSDK_NGX_Result init_res = ngx12_acquire(device, data_dir.c_str(), &feature_info, log_tag);
	if (NVSDK_NGX_FAILED(init_res)) {
		status = UpscalerStatus::InitFailed;
		last_error = ngx_format_result(L"NGX D3D12 Init failed", init_res);
		const std::wstring tail = diag_last_line();
		if (!tail.empty()) last_error += L" | " + tail;
		ngx_initialized = false;
		return false;
	}
	ngx_initialized = true;

	NVSDK_NGX_Parameter *p = nullptr;
	const NVSDK_NGX_Result cap_res = NVSDK_NGX_D3D12_GetCapabilityParameters(&p);
	diag_info("dlss", ngx_format_result(L"D3D12 GetCapabilityParameters", cap_res));
	if (NVSDK_NGX_FAILED(cap_res) || p == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = ngx_format_result(L"D3D12 GetCapabilityParameters", cap_res);
		wait_gpu();
		ngx12_release(device, log_tag);
		ngx_initialized = false;
		return false;
	}
	params = p;

	int available = 0;
	p->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
	diag_info("dlss", std::wstring(L"D3D12 SuperSampling.Available=") + (available ? L"1" : L"0"));
	if (!available) {
		status = UpscalerStatus::InitFailed;
		last_error = L"DLSS SuperSampling not available (driver/GPU/DLL)";
		NVSDK_NGX_D3D12_DestroyParameters(p);
		params = nullptr;
		wait_gpu();
		ngx12_release(device, log_tag);
		ngx_initialized = false;
		return false;
	}

	status = UpscalerStatus::Idle;
	last_error.clear();
	diag_info("dlss", L"=== D3D12 init_device ok ===");
	return true;
}

void NgxRuntimeD3D12::shutdown_device()
{
	diag_info("dlss", L"D3D12 shutdown_device");
	destroy_feature();
	blit_.release();
	if (params != nullptr) {
		NVSDK_NGX_D3D12_DestroyParameters(reinterpret_cast<NVSDK_NGX_Parameter *>(params));
		params = nullptr;
	}
	if (ngx_initialized && device != nullptr) {
		wait_gpu();
		ngx12_release(device, log_tag);
		ngx_initialized = false;
	}
	init_list.release();
	gpu_fence.release();
	queue = nullptr;
	device = nullptr;
	reset_after_shutdown();
}

void NgxRuntimeD3D12::destroy_feature()
{

	if (feature_handle != nullptr || output_tex != nullptr || color_copy != nullptr ||
		depth_scratch != nullptr || color_full != nullptr) {
		diag_trace("dlss", L"D3D12 destroy_feature (wait_gpu)");
		wait_gpu();
	}
	if (feature_handle != nullptr && ngx_initialized) {
		NVSDK_NGX_D3D12_ReleaseFeature(reinterpret_cast<NVSDK_NGX_Handle *>(feature_handle));
		feature_handle = nullptr;
	}
	release_scratch();
}

void NgxRuntimeD3D12::release_scratch()
{
	for (ID3D12Resource **r : { &output_tex, &color_copy, &depth_scratch, &bias_scratch,
			&color_full }) {
		if (*r) { (*r)->Release(); *r = nullptr; }
	}
	created_backbuffer_format = DXGI_FORMAT_UNKNOWN;
}

namespace {

NVSDK_NGX_Result create_dlss_guarded(ID3D12GraphicsCommandList *cmd,
	NVSDK_NGX_Handle **handle, NVSDK_NGX_Parameter *params,
	NVSDK_NGX_DLSS_Create_Params *create, unsigned long *seh_code)
{
	__try {
		return NGX_D3D12_CREATE_DLSS_EXT(cmd, 1, 1, handle, params, create);
	} __except ((*seh_code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return NVSDK_NGX_Result_Fail;
	}
}

NVSDK_NGX_Result evaluate_dlss_guarded(ID3D12GraphicsCommandList *cmd,
	NVSDK_NGX_Handle *handle, NVSDK_NGX_Parameter *params,
	NVSDK_NGX_D3D12_DLSS_Eval_Params *eval, unsigned long *seh_code)
{
	__try {
		return NGX_D3D12_EVALUATE_DLSS_EXT(cmd, handle, params, eval);
	} __except ((*seh_code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return NVSDK_NGX_Result_Fail;
	}
}

}

bool NgxRuntimeD3D12::ensure_feature(ID3D12GraphicsCommandList *cmd, uint32_t w, uint32_t h, DXGI_FORMAT color_format)
{
	if (!ngx_initialized || params == nullptr || cmd == nullptr || device == nullptr)
		return false;

	const DlssFeatureKey key = DlssFeatureKey::from(cfg);
	if (feature_handle != nullptr && width == w && height == h && output_tex != nullptr &&
		scratch_format == color_format && created == key &&
		created_backbuffer_format == backbuffer_format)
		return true;

	diag_info("dlss", L"D3D12 ensure_feature: recreate");
	destroy_feature();
	width = w; height = h;
	scratch_format = color_format;
	created = key;

	auto *p = reinterpret_cast<NVSDK_NGX_Parameter *>(params);
	NVSDK_NGX_PerfQuality_Value perf = NVSDK_NGX_PerfQuality_Value_DLAA;
	if (!ngx_dlss::resolve_render_size(*this, w, h, &perf))
		return false;

	NVSDK_NGX_Parameter *create_params = nullptr;
	if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&create_params)) || create_params == nullptr)
		create_params = p;

	ngx_dlss::set_preset_hints(create_params, cfg);

	NVSDK_NGX_DLSS_Create_Params create;
	ngx_dlss::fill_create_params(*this, w, h, perf, &create);

	{
		wchar_t buf[192]{};
		_snwprintf_s(buf, _TRUNCATE, L"D3D12 ensure_feature: CreateFeature %ux%u -> %ux%u fmt=%u preset=%u",
			render_width, render_height, w, h, static_cast<unsigned>(color_format), cfg.render_preset);
		diag_info("dlss", buf);
	}

	ID3D12GraphicsCommandList *create_cmd = queue != nullptr ? init_list.begin(device) : nullptr;
	const bool own_list = create_cmd != nullptr;
	if (!own_list)
		create_cmd = cmd;

	NVSDK_NGX_Handle *handle = nullptr;
	unsigned long seh_code = 0;
	const NVSDK_NGX_Result cr = create_dlss_guarded(create_cmd, &handle, create_params, &create, &seh_code);
	if (seh_code != 0) {
		crashed = true;
		status = UpscalerStatus::InitFailed;
		{
			wchar_t buf[128]{};
			_snwprintf_s(buf, _TRUNCATE, L"D3D12 CreateFeature CRASHED (exception 0x%08lX) - DLAA disabled", seh_code);
			last_error = buf;
			diag_info("dlss", buf);
		}
		if (game_ngx_modules.find(L"sl.interposer.dll") != std::wstring::npos ||
			game_ngx_modules.find(L"_nvngx.dll") != std::wstring::npos) {
			last_error += L" | Game has its own DLSS/Streamline integration; a second "
				L"NGX instance cannot coexist. Use the game's native DLAA instead.";
		}
		if (own_list)
			init_list.end(device, queue, gpu_fence, false);
		if (create_params != p && create_params != nullptr)
			NVSDK_NGX_D3D12_DestroyParameters(create_params);
		return false;
	}
	diag_info("dlss", ngx_format_result(L"D3D12 CreateFeature", cr));
	if (own_list)
		init_list.end(device, queue, gpu_fence, !NVSDK_NGX_FAILED(cr) && handle != nullptr);
	if (create_params != p && create_params != nullptr)
		NVSDK_NGX_D3D12_DestroyParameters(create_params);
	if (NVSDK_NGX_FAILED(cr) || handle == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = ngx_format_result(L"D3D12 CreateFeature failed", cr);
		return false;
	}
	feature_handle = handle;

	diag_info("dlss", L"D3D12 ensure_feature: create_tex");

	if (!create_tex12(device, width, height, color_format, true, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &output_tex) ||
		!create_tex12(device, render_width, render_height, color_format, true, kNgxSrvState, &color_copy)) {
		destroy_feature();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create D3D12 scratch textures";
		return false;
	}

	if (!create_tex12(device, render_width, render_height, DXGI_FORMAT_R32_FLOAT, false, kNgxSrvState, &depth_scratch)) {
		destroy_feature();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create D3D12 depth scratch";
		return false;
	}
	if (!create_tex12(device, render_width, render_height, DXGI_FORMAT_R8_UNORM, false, kNgxSrvState, &bias_scratch)) {
		destroy_feature();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create D3D12 bias mask scratch";
		return false;
	}

	if (!create_tex12(device, width, height, backbuffer_format, false, D3D12_RESOURCE_STATE_COPY_DEST, &color_full)) {
		destroy_feature();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create D3D12 color staging";
		return false;
	}
	created_backbuffer_format = backbuffer_format;

	status = UpscalerStatus::Ready;
	last_error.clear();
	diag_info("dlss", L"D3D12 ensure_feature ok");
	return true;
}

bool NgxRuntimeD3D12::run(ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *backbuffer, D3D12_RESOURCE_STATES backbuffer_state,
	ID3D12Resource *motion_vectors,
	ID3D12Resource *depth, ID3D12Resource *bias_mask)
{
	if (crashed)
		return false;
	if (!ngx_initialized || params == nullptr || cmd == nullptr || device == nullptr)
		return false;
	if (backbuffer == nullptr || motion_vectors == nullptr)
		return false;
	diag_trace("dlss", L"D3D12 run: begin");
	engine_journal_note(L"DLSS run");
	const D3D12_RESOURCE_DESC bb_desc = backbuffer->GetDesc();
	backbuffer_format = bb_desc.Format;
	const DXGI_FORMAT fmt = upscaler_scratch_format(bb_desc.Format);
	const uint32_t w = static_cast<uint32_t>(bb_desc.Width);
	const uint32_t h = bb_desc.Height;
	{
		wchar_t buf[128]{};
		_snwprintf_s(buf, _TRUNCATE, L"D3D12 run: bb %ux%u fmt=%u scratch=%u", w, h,
			static_cast<unsigned>(bb_desc.Format), static_cast<unsigned>(fmt));
		diag_trace("dlss", buf);
	}

	if (!blit_.ensure(device)) {
		diag_error("dlss", L"D3D12 run: the blit pipeline would not build");
		return false;
	}
	if (!ensure_feature(cmd, w, h, fmt)) {
		diag_error("dlss", L"D3D12 run: the DLSS feature would not build");
		return false;
	}

	barrier12(cmd, backbuffer, backbuffer_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmd->CopyResource(color_full, backbuffer);

	barrier12(cmd, color_full, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	barrier12(cmd, color_copy, kNgxSrvState, D3D12_RESOURCE_STATE_RENDER_TARGET);

	jitter_x = cfg.jitter_x;
	jitter_y = cfg.jitter_y;

	const float shift_x = cfg.jitter_in_frame ? 0.0f : jitter_x;
	const float shift_y = cfg.jitter_in_frame ? 0.0f : jitter_y;
	const float jitter_u = render_width > 0 ? -shift_x / static_cast<float>(render_width) : 0.0f;
	const float jitter_v = render_height > 0 ? -shift_y / static_cast<float>(render_height) : 0.0f;
	bool ok = blit_.draw_fullscreen(device, cmd, color_full,
		color_copy, scratch_format, render_width, render_height, 0.0f, 0u,
		jitter_u, jitter_v);

	barrier12(cmd, color_copy, D3D12_RESOURCE_STATE_RENDER_TARGET, kNgxSrvState);
	barrier12(cmd, color_full, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
	if (!ok) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = L"failed to blit color into UAV scratch";
		diag_error("dlss", last_error);
	}

	if (ok)
		ok = evaluate_inputs(cmd, color_copy, depth, motion_vectors, bias_mask, jitter_u, jitter_v);

	barrier12(cmd, backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	if (ok) {
		diag_trace("dlss", L"D3D12 run: composite");
		ok = blit_.draw_fullscreen(device, cmd, output_tex,
			backbuffer, backbuffer_format, width, height, cfg.sharpness);
	}
	if (backbuffer_state != D3D12_RESOURCE_STATE_RENDER_TARGET)
		barrier12(cmd, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, backbuffer_state);

	if (ok) {
		status = UpscalerStatus::Ready;
		last_error.clear();
		diag_trace("dlss", L"D3D12 run: ok");
	}
	return ok;
}

bool NgxRuntimeD3D12::evaluate_inputs(ID3D12GraphicsCommandList *cmd, ID3D12Resource *in_color,
	ID3D12Resource *depth, ID3D12Resource *motion_vectors, ID3D12Resource *bias_mask,
	float jitter_u, float jitter_v)
{

	ID3D12Resource *depth_input = depth;
	if (depth_scratch != nullptr && depth != nullptr) {
		barrier12(cmd, depth_scratch, kNgxSrvState, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (blit_.draw_fullscreen(device, cmd, depth,
				depth_scratch, DXGI_FORMAT_R32_FLOAT, render_width, render_height,
				0.0f, 0u, jitter_u, jitter_v))
			depth_input = depth_scratch;
		else
			depth_input = nullptr;
		barrier12(cmd, depth_scratch, D3D12_RESOURCE_STATE_RENDER_TARGET, kNgxSrvState);
	} else if (depth_scratch != nullptr) {
		const float far_plane = cfg.depth_inverted ? 0.0f : 1.0f;
		const float flat[4] = { far_plane, far_plane, far_plane, far_plane };
		barrier12(cmd, depth_scratch, kNgxSrvState, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (blit_.clear(device, cmd, depth_scratch, DXGI_FORMAT_R32_FLOAT, flat))
			depth_input = depth_scratch;
		barrier12(cmd, depth_scratch, D3D12_RESOURCE_STATE_RENDER_TARGET, kNgxSrvState);
	}

	ID3D12Resource *bias_input = bias_mask;
	if (bias_input != nullptr && bias_scratch != nullptr) {
		barrier12(cmd, bias_scratch, kNgxSrvState, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (blit_.draw_fullscreen(device, cmd, bias_input,
				bias_scratch, DXGI_FORMAT_R8_UNORM, render_width, render_height,
				0.0f, 0u, jitter_u, jitter_v))
			bias_input = bias_scratch;
		else
			bias_input = nullptr;
		barrier12(cmd, bias_scratch, D3D12_RESOURCE_STATE_RENDER_TARGET, kNgxSrvState);
	}

	diag_trace("dlss", std::wstring(log_tag) + L"run: EvaluateFeature");
	barrier12(cmd, output_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	NVSDK_NGX_Parameter *eval_params = nullptr;
	if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&eval_params)) || eval_params == nullptr)
		eval_params = reinterpret_cast<NVSDK_NGX_Parameter *>(params);

	NVSDK_NGX_D3D12_DLSS_Eval_Params eval{};
	ngx_dlss::fill_dlss_eval_scalars(*this, eval);
	eval.Feature.pInColor = in_color;
	eval.Feature.pInOutput = output_tex;
	eval.pInDepth = depth_input;
	eval.pInMotionVectors = motion_vectors;
	eval.pInBiasCurrentColorMask = bias_input;

	unsigned long seh_code = 0;
	const NVSDK_NGX_Result er = evaluate_dlss_guarded(cmd,
		reinterpret_cast<NVSDK_NGX_Handle *>(feature_handle), eval_params, &eval, &seh_code);
	++eval_count;
	if (eval_params != params && eval_params != nullptr)
		NVSDK_NGX_D3D12_DestroyParameters(eval_params);

	barrier12(cmd, output_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	if (seh_code != 0) {
		crashed = true;
		status = UpscalerStatus::EvaluateFailed;
		wchar_t buf[128]{};
		_snwprintf_s(buf, _TRUNCATE, L"D3D12 EvaluateFeature raised exception 0x%08lX - DLSS disabled for this session", seh_code);
		last_error = buf;
		diag_error("dlss", buf);
		return false;
	}
	if (NVSDK_NGX_FAILED(er)) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = ngx_format_result(L"D3D12 EvaluateFeature failed", er);
		diag_error("dlss", last_error);
		return false;
	}
	return true;
}

}
