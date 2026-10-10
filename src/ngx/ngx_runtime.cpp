#include "aeon_sr/ngx/ngx_runtime.hpp"

#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/ngx/ngx_session_ngx.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <dxgi.h>

#ifndef CUDA_VERSION
typedef unsigned long long CUtexObject;
#endif

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace aeon_sr {
namespace {

bool create_tex2d(ID3D11Device *device, uint32_t w, uint32_t h, DXGI_FORMAT format, bool need_uav, ID3D11Texture2D **out)
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

	const HRESULT hr = device->CreateTexture2D(&desc, nullptr, out);
	if (FAILED(hr)) {
		wchar_t buf[128]{};
		_snwprintf_s(buf, _TRUNCATE, L"CreateTexture2D failed hr=0x%08X fmt=%u uav=%d",
			static_cast<unsigned>(hr), static_cast<unsigned>(format), need_uav ? 1 : 0);
		diag_info("dlss", buf);
		return false;
	}
	return true;
}

}

bool NgxRuntime::init_device(ID3D11Device *dev, uint32_t w, uint32_t h)
{
	shutdown_device();

	device = dev;
	width = w;
	height = h;

	if (!ensure_dll_present())
		return false;

	if (device == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = L"null D3D11 device";
		return false;
	}

	std::wstring device_desc;
	{
		IDXGIDevice *dxgi_device = nullptr;
		if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_device))) && dxgi_device) {
			IDXGIAdapter *adapter = nullptr;
			if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter) {
				DXGI_ADAPTER_DESC desc{};
				if (SUCCEEDED(adapter->GetDesc(&desc))) {
					wchar_t buf[192]{};
					_snwprintf_s(buf, _TRUNCATE, L"adapter=%s vendor=0x%04X", desc.Description, desc.VendorId);
					device_desc = buf;
				}
				adapter->Release();
			}
			dxgi_device->Release();
		}
		const D3D_FEATURE_LEVEL fl = device->GetFeatureLevel();
		{
			wchar_t buf[48]{};
			_snwprintf_s(buf, _TRUNCATE, L" fl=0x%X flags=0x%X",
				static_cast<unsigned>(fl), device->GetCreationFlags());
			device_desc += buf;
		}
		ID3D11On12Device *on12 = nullptr;
		if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11On12Device), reinterpret_cast<void **>(&on12))) && on12) {
			device_desc += L" (D3D11-on-12)";
			on12->Release();
		}
		if (device_desc.empty())
			device_desc = L"adapter=unknown";
		diag_info("dlss", device_desc);
	}

	prepare_data_dir();
	diag_logf(DiagLevel::Info, "dlss", L"D3D11 init: addon_dir=%s dll_dir=%s data_dir=%s",
		addon_dir.c_str(), dll_dir.c_str(), data_dir.c_str());

	const std::wstring search_dir = !dll_dir.empty() ? dll_dir : addon_dir;
	const wchar_t *path_list[] = { search_dir.c_str() };

	NVSDK_NGX_FeatureCommonInfo feature_info{};
	feature_info.PathListInfo.Path = path_list;
	feature_info.PathListInfo.Length = 1;
	feature_info.LoggingInfo.LoggingCallback = ngx_log_callback;
	feature_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
	feature_info.LoggingInfo.DisableOtherLoggingSinks = false;

	NVSDK_NGX_Result init_res = NVSDK_NGX_Result_Fail;

	note_own_ngx_start();
	init_res = NVSDK_NGX_D3D11_Init(ngx_dlss::kAppId, data_dir.c_str(), device, &feature_info, NVSDK_NGX_Version_API);
	diag_info("dlss", ngx_format_result(L"Init(AppId)", init_res));

	if (NVSDK_NGX_FAILED(init_res)) {
		init_res = NVSDK_NGX_D3D11_Init_with_ProjectID(
			ngx_dlss::kProjectId,
			NVSDK_NGX_ENGINE_TYPE_CUSTOM,
			ngx_dlss::kEngineVersion,
			data_dir.c_str(),
			device,
			&feature_info,
			NVSDK_NGX_Version_API);
		diag_info("dlss", ngx_format_result(L"Init_with_ProjectID", init_res));
	}

	if (NVSDK_NGX_FAILED(init_res)) {
		status = UpscalerStatus::InitFailed;
		last_error = ngx_format_result(L"NGX Init failed", init_res) + L" | " + device_desc;
		{
			const std::wstring tail = diag_last_line();
			if (!tail.empty())
				last_error += L" | " + tail;
		}
		if (init_res == NVSDK_NGX_Result_FAIL_FeatureNotSupported) {
			last_error += L" | driver NGXCheckIfSupportedOnHW refused this process/device "
				L"(common inside Lossless Scaling; use the game process instead)";
		}
		ngx_initialized = false;
		return false;
	}

	ngx_initialized = true;

	NVSDK_NGX_Parameter *p = nullptr;
	const NVSDK_NGX_Result cap_res = NVSDK_NGX_D3D11_GetCapabilityParameters(&p);
	diag_info("dlss", ngx_format_result(L"GetCapabilityParameters", cap_res));
	if (NVSDK_NGX_FAILED(cap_res) || p == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = ngx_format_result(L"GetCapabilityParameters", cap_res);
		NVSDK_NGX_D3D11_Shutdown1(device);
		ngx_initialized = false;
		return false;
	}
	params = p;

	int available = 0;
	p->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
	diag_info("dlss", std::wstring(L"SuperSampling.Available=") + (available ? L"1" : L"0"));
	if (!available) {
		status = UpscalerStatus::InitFailed;
		last_error = L"DLSS SuperSampling not available (driver/GPU/DLL)";
		{
			const std::wstring tail = diag_last_line();
			if (!tail.empty())
				last_error += L" | " + tail;
		}
		NVSDK_NGX_D3D11_DestroyParameters(p);
		params = nullptr;
		NVSDK_NGX_D3D11_Shutdown1(device);
		ngx_initialized = false;
		return false;
	}

	status = UpscalerStatus::Idle;
	last_error.clear();
	return true;
}

void NgxRuntime::destroy_feature()
{
	if (feature_handle != nullptr && ngx_initialized) {
		NVSDK_NGX_D3D11_ReleaseFeature(reinterpret_cast<NVSDK_NGX_Handle *>(feature_handle));
		feature_handle = nullptr;
	}
	release_scratch();
}

void NgxRuntime::release_scratch()
{
	if (output_tex) {
		output_tex->Release();
		output_tex = nullptr;
	}
	if (color_copy) {
		color_copy->Release();
		color_copy = nullptr;
	}
	if (depth_scratch) {
		depth_scratch->Release();
		depth_scratch = nullptr;
	}
	if (bias_scratch) {
		bias_scratch->Release();
		bias_scratch = nullptr;
	}
	if (color_full) {
		color_full->Release();
		color_full = nullptr;
	}
}

bool NgxRuntime::blit_texture(ID3D11DeviceContext *ctx, ID3D11Resource *src, ID3D11Resource *dst, float sharpness,
	float jitter_u, float jitter_v)
{
	return blit_.ensure(device) && blit_.blit(ctx, src, dst, sharpness, jitter_u, jitter_v);
}

void NgxRuntime::shutdown_device()
{
	destroy_feature();
	blit_.release();

	if (params != nullptr) {
		NVSDK_NGX_D3D11_DestroyParameters(reinterpret_cast<NVSDK_NGX_Parameter *>(params));
		params = nullptr;
	}

	if (ngx_initialized && device != nullptr) {
		NVSDK_NGX_D3D11_Shutdown1(device);
		ngx_initialized = false;
	}

	device = nullptr;
	reset_after_shutdown();
}

bool NgxRuntime::ensure_feature(ID3D11DeviceContext *ctx, uint32_t w, uint32_t h, DXGI_FORMAT color_format)
{
	if (!ngx_initialized || params == nullptr || ctx == nullptr || device == nullptr)
		return false;

	const DlssFeatureKey key = DlssFeatureKey::from(cfg);
	if (feature_handle != nullptr && width == w && height == h && output_tex != nullptr &&
		scratch_format == color_format && created == key)
		return true;

	destroy_feature();
	width = w;
	height = h;
	scratch_format = color_format;
	created = key;

	auto *p = reinterpret_cast<NVSDK_NGX_Parameter *>(params);
	NVSDK_NGX_PerfQuality_Value perf = NVSDK_NGX_PerfQuality_Value_DLAA;
	if (!ngx_dlss::resolve_render_size(*this, w, h, &perf))
		return false;

	NVSDK_NGX_Parameter *create_params = nullptr;
	if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&create_params)) || create_params == nullptr)
		create_params = p;

	ngx_dlss::set_preset_hints(create_params, cfg);

	NVSDK_NGX_DLSS_Create_Params create;
	ngx_dlss::fill_create_params(*this, w, h, perf, &create);

	NVSDK_NGX_Handle *handle = nullptr;
	const NVSDK_NGX_Result cr = NGX_D3D11_CREATE_DLSS_EXT(ctx, &handle, create_params, &create);
	diag_info("dlss", ngx_format_result(L"CreateFeature", cr));
	{
		wchar_t buf[128]{};
		_snwprintf_s(buf, _TRUNCATE, L"mode=%u preset=%u autoExp=%d render=%ux%u target=%ux%u",
			cfg.quality_mode, cfg.render_preset, 1,
			render_width, render_height, w, h);
		diag_info("dlss", buf);
	}

	if (create_params != p && create_params != nullptr)
		NVSDK_NGX_D3D11_DestroyParameters(create_params);

	if (NVSDK_NGX_FAILED(cr) || handle == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = ngx_format_result(L"CreateFeature failed", cr);
		return false;
	}
	feature_handle = handle;

	if (!create_tex2d(device, width, height, color_format, true, &output_tex) ||
		!create_tex2d(device, render_width, render_height, color_format, true, &color_copy)) {
		destroy_feature();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create scratch textures (UAV)";
		return false;
	}

	if (!create_tex2d(device, render_width, render_height, DXGI_FORMAT_R32_FLOAT, false, &depth_scratch)) {
		destroy_feature();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create depth scratch texture";
		return false;
	}
	if (!create_tex2d(device, render_width, render_height, DXGI_FORMAT_R8_UNORM, false, &bias_scratch)) {
		destroy_feature();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create bias mask scratch texture";
		return false;
	}

	if (!create_tex2d(device, width, height, backbuffer_format, false, &color_full)) {
		destroy_feature();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create full-res color staging texture";
		return false;
	}

	status = UpscalerStatus::Ready;
	last_error.clear();
	return true;
}

bool NgxRuntime::run(ID3D11DeviceContext *ctx, ID3D11Resource *color, ID3D11Resource *motion_vectors,
	ID3D11Resource *depth, ID3D11Resource *bias_mask)
{
	if (!ngx_initialized || params == nullptr || ctx == nullptr || device == nullptr)
		return false;
	if (color == nullptr || motion_vectors == nullptr)
		return false;

	D3D11_TEXTURE2D_DESC color_desc{};
	{
		ID3D11Texture2D *tex = nullptr;
		if (FAILED(color->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) || tex == nullptr)
			return false;
		tex->GetDesc(&color_desc);
		tex->Release();
	}

	const DXGI_FORMAT fmt = resolve_scratch_format(color_desc.Format);
	backbuffer_format = color_desc.Format;
	if (!ensure_feature(ctx, color_desc.Width, color_desc.Height, fmt))
		return false;

	if (output_tex == nullptr || color_copy == nullptr)
		return false;

	jitter_x = cfg.jitter_x;
	jitter_y = cfg.jitter_y;
	const float jitter_u = render_width > 0 ? -jitter_x / static_cast<float>(render_width) : 0.0f;
	const float jitter_v = render_height > 0 ? -jitter_y / static_cast<float>(render_height) : 0.0f;

	const bool resampled = jitter_u != 0.0f || jitter_v != 0.0f ||
		render_width != width || render_height != height ||
		scratch_format != backbuffer_format;
	ID3D11Resource *color_src = color;
	if (resampled && color_full != nullptr) {
		ctx->CopyResource(color_full, color);
		color_src = color_full;
	}
	if (!blit_texture(ctx, color_src, color_copy, 0.0f, jitter_u, jitter_v)) {
		wchar_t buf[224]{};
		_snwprintf_s(buf, _TRUNCATE, L"colour into scratch refused: stage %d, "
			L"%ux%u fmt=%u -> %ux%u fmt=%u, jitter %.5f/%.5f, staged=%d",
			blit_.last_stage,
			width, height, static_cast<unsigned>(backbuffer_format),
			render_width, render_height, static_cast<unsigned>(scratch_format),
			jitter_u, jitter_v, color_src == color ? 0 : 1);
		if (jitter_note != buf) {
			jitter_note = buf;
			diag_warn("dlss", jitter_note);
		}
		if ((jitter_u != 0.0f || jitter_v != 0.0f) &&
			blit_texture(ctx, color_src, color_copy, 0.0f, 0.0f, 0.0f)) {
			jitter_x = 0.0f;
			jitter_y = 0.0f;
			jitter_failed = true;
		} else {
			status = UpscalerStatus::EvaluateFailed;
			last_error = buf;
			return false;
		}
	} else {
		jitter_failed = false;
	}

	ID3D11Resource *depth_input = depth;
	if (depth_scratch != nullptr && depth != nullptr) {
		if (blit_texture(ctx, depth, depth_scratch, 0.0f, jitter_u, jitter_v))
			depth_input = depth_scratch;
		else
			depth_input = nullptr;
	}

	ID3D11Resource *bias_input = bias_mask;
	if (bias_input != nullptr && bias_scratch != nullptr) {
		if (blit_texture(ctx, bias_input, bias_scratch, 0.0f, jitter_u, jitter_v))
			bias_input = bias_scratch;
		else
			bias_input = nullptr;
	}

	NVSDK_NGX_Parameter *eval_params = nullptr;
	if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_AllocateParameters(&eval_params)) || eval_params == nullptr)
		eval_params = reinterpret_cast<NVSDK_NGX_Parameter *>(params);

	auto *handle = reinterpret_cast<NVSDK_NGX_Handle *>(feature_handle);

	NVSDK_NGX_D3D11_DLSS_Eval_Params eval{};
	ngx_dlss::fill_dlss_eval_scalars(*this, eval);
	eval.Feature.pInColor = color_copy;
	eval.Feature.pInOutput = output_tex;
	eval.pInDepth = depth_input;
	eval.pInMotionVectors = motion_vectors;
	eval.pInBiasCurrentColorMask = bias_input;

	const NVSDK_NGX_Result er = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, handle, eval_params, &eval);
	++eval_count;

	if (eval_params != params && eval_params != nullptr)
		NVSDK_NGX_D3D11_DestroyParameters(eval_params);

	if (NVSDK_NGX_FAILED(er)) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = ngx_format_result(L"EvaluateFeature failed", er);
		diag_error("dlss", last_error);
		return false;
	}

	status = UpscalerStatus::Ready;
	last_error.clear();

	blit_texture(ctx, output_tex, color, cfg.sharpness);
	return true;
}

}
