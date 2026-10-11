#include "aeon_sr/upscalers/backend_fsr.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/core/runtime_search.hpp"
#include "aeon_sr/core/settings.hpp"

#include <excpt.h>

#include <vector>

namespace aeon_sr {
namespace {

bool create_tex2d(ID3D11Device *device, uint32_t w, uint32_t h, DXGI_FORMAT format, bool need_uav,
	ID3D11Texture2D **out)
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

	return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, out));
}

const wchar_t *ffx_error_name(FfxErrorCode code)
{
	switch (code) {
	case FFX_OK: return L"OK";
	case FFX_ERROR_INVALID_POINTER: return L"INVALID_POINTER";
	case FFX_ERROR_INCOMPLETE_INTERFACE: return L"INCOMPLETE_INTERFACE";
	case FFX_ERROR_BACKEND_API_ERROR: return L"BACKEND_API_ERROR";
	case FFX_ERROR_OUT_OF_RANGE: return L"OUT_OF_RANGE";
	default: return L"Unknown";
	}
}

std::wstring ffx_format_result(const wchar_t *what, FfxErrorCode code)
{
	wchar_t buf[256]{};
	_snwprintf_s(buf, _TRUNCATE, L"%s: %s (0x%08X)", what, ffx_error_name(code), static_cast<unsigned>(code));
	return buf;
}

uint32_t ffx_quality_for(uint32_t upscale_mode)
{
	switch (static_cast<UpscaleMode>(upscale_mode)) {
	case UpscaleMode::UltraQuality:
	case UpscaleMode::Quality:
		return FFX_FSR3UPSCALER_QUALITY_MODE_QUALITY;
	case UpscaleMode::Balanced:
		return FFX_FSR3UPSCALER_QUALITY_MODE_BALANCED;
	case UpscaleMode::Performance:
		return FFX_FSR3UPSCALER_QUALITY_MODE_PERFORMANCE;
	case UpscaleMode::UltraPerformance:
		return FFX_FSR3UPSCALER_QUALITY_MODE_ULTRA_PERFORMANCE;
	case UpscaleMode::Dlaa:
	default:
		return FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA;
	}
}

bool needs_depth_scratch(uint32_t quality_mode)
{
	return ffx_quality_for(quality_mode) != FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA;
}

FfxErrorCode create_context_guarded(FfxDx11Api &api, FfxFsr3UpscalerContext *ctx,
	const FfxFsr3UpscalerContextDescription *desc, unsigned long *seh_code)
{
	__try {
		return api.UpscalerContextCreate(ctx, desc);
	} __except ((*seh_code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return FFX_ERROR_BACKEND_API_ERROR;
	}
}

FfxErrorCode dispatch_guarded(FfxDx11Api &api, FfxFsr3UpscalerContext *ctx,
	const FfxFsr3UpscalerDispatchDescription *desc, unsigned long *seh_code)
{
	__try {
		return api.UpscalerContextDispatch(ctx, desc);
	} __except ((*seh_code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return FFX_ERROR_BACKEND_API_ERROR;
	}
}

std::wstring fsr_dx11_dll_hint()
{
	std::wstring hint =
		L"This game is D3D11. Official AMD FSR (amd_fidelityfx_*_dx12.dll) is DX12-only "
		L"and is already bootstrapped for D3D12 titles. For D3D11 you need a user-built "
		L"MIT wrapper next to AeonSR.addon64 (AMD does not ship one). Candidates: ";
	for (size_t i = 0; i < std::size(kFsrDx11DllCandidates); ++i) {
		if (i > 0)
			hint += L", ";
		hint += kFsrDx11DllCandidates[i];
	}
	hint += L" — build from optiscaler/FidelityFX-SDK-DX11 (static libs only). "
		L"On NVIDIA D3D11, prefer DLSS.";
	return hint;
}

}

const char *Fsr31D3D11Backend::name() const { return "FSR 3.1"; }

reshade::api::device_api Fsr31D3D11Backend::api() const
{
	return reshade::api::device_api::d3d11;
}

bool Fsr31D3D11Backend::ensure_dll_present()
{
	dll_present_ = false;
	dll_dir.clear();
	loaded_dll_name.clear();

	const std::vector<std::wstring> search = runtime_search_dirs(addon_dir, exe_directory_w());

	for (const auto &dir : search) {
		for (const wchar_t *name : kFsrDx11DllCandidates) {
			const std::wstring path = join_path(dir, name);
			if (file_exists_w(path)) {
				dll_present_ = true;
				dll_dir = dir;
				loaded_dll_name = name;
				return true;
			}
		}
	}

	status = UpscalerStatus::MissingRuntime;
	last_error = fsr_dx11_dll_hint();
	return false;
}

bool Fsr31D3D11Backend::runtime_present()
{
	return ensure_dll_present();
}

FfxResource Fsr31D3D11Backend::make_resource(ID3D11Resource *res, FfxResourceStates state) const
{
	if (res == nullptr || api_.GetResourceDescriptionDX11 == nullptr || api_.GetResourceDX11 == nullptr)
		return {};

	ID3D11Resource *mut = res;
	const FfxResourceDescription desc = api_.GetResourceDescriptionDX11(mut);
	return api_.GetResourceDX11(res, desc, nullptr, state);
}

void Fsr31D3D11Backend::release_scratch()
{
	for (ID3D11Texture2D **t : { &output_tex_, &color_copy_, &depth_scratch_, &color_full_ }) {
		if (*t) {
			(*t)->Release();
			*t = nullptr;
		}
	}
	created_backbuffer_format_ = DXGI_FORMAT_UNKNOWN;
}

void Fsr31D3D11Backend::destroy_context()
{
	if (initialized_ && api_.UpscalerContextDestroy != nullptr) {
		unsigned long seh_code = 0;
		__try {
			api_.UpscalerContextDestroy(&context_);
		} __except (seh_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
			crashed = true;
		}
	}
	memset(&context_, 0, sizeof(context_));
	if (api_.scratch_buffer != nullptr) {
		free(api_.scratch_buffer);
		api_.scratch_buffer = nullptr;
		api_.scratch_size = 0;
	}
	memset(&backend_, 0, sizeof(backend_));
	release_scratch();
	initialized_ = false;
	created_quality_mode_ = 0xFFFFFFFFu;
	created_render_scale_ = -1.0f;
}

void Fsr31D3D11Backend::on_destroy_swapchain()
{
	destroy_context();
}

void Fsr31D3D11Backend::shutdown()
{
	destroy_context();
	blit_.release();
	ffx_dx11_unload(api_);
	device_ = nullptr;
	width_ = height_ = render_width_ = render_height_ = 0;
	if (status != UpscalerStatus::MissingRuntime)
		status = UpscalerStatus::Idle;
}

bool Fsr31D3D11Backend::query_render_size(uint32_t display_w, uint32_t display_h, uint32_t quality,
	uint32_t &out_w, uint32_t &out_h)
{
	if (quality == FFX_FSR3UPSCALER_QUALITY_MODE_NATIVEAA) {
		out_w = display_w;
		out_h = display_h;
		return true;
	}
	if (api_.GetRenderResolutionFromQualityMode == nullptr)
		return false;

	uint32_t rw = 0, rh = 0;
	const FfxErrorCode qr = api_.GetRenderResolutionFromQualityMode(
		static_cast<FfxFsr3UpscalerQualityMode>(quality), display_w, display_h, &rw, &rh);
	if (qr != FFX_OK || rw == 0 || rh == 0)
		return false;
	out_w = rw;
	out_h = rh;
	return true;
}

bool Fsr31D3D11Backend::init_context(ID3D11Device *device, uint32_t w, uint32_t h)
{
	if (!ensure_dll_present())
		return false;
	if (device == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = L"null D3D11 device";
		return false;
	}

	if (initialized_ && device_ == device && width_ == w && height_ == h &&
		created_quality_mode_ == quality_mode_ &&
		created_render_scale_ == render_scale_)
		return true;

	destroy_context();
	device_ = device;
	width_ = w;
	height_ = h;

	if (api_.module == nullptr) {
		const std::wstring path = join_path(!dll_dir.empty() ? dll_dir : addon_dir, loaded_dll_name.c_str());
		if (!ffx_dx11_load(api_, path, last_error)) {
			status = UpscalerStatus::InitFailed;
			last_error += L" | DLL must export classic FSR3 upscaler symbols (not FFX API 2.x) | ";
			last_error += fsr_dx11_dll_hint();
			return false;
		}
	}

	if (render_scale_ > 0.0f) {
		scaled_render_size(w, h, render_scale_, &render_width_, &render_height_);
	} else {
		const uint32_t ffx_quality = ffx_quality_for(quality_mode_);
		if (!query_render_size(w, h, ffx_quality, render_width_, render_height_)) {
			status = UpscalerStatus::InitFailed;
			last_error = L"FFX GetRenderResolutionFromQualityMode failed";
			return false;
		}
	}

	if (api_.scratch_buffer == nullptr) {
		const size_t scratch_size = api_.GetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
		void *scratch = calloc(scratch_size, 1);
		if (scratch == nullptr) {
			status = UpscalerStatus::InitFailed;
			last_error = L"failed to allocate FFX DX11 scratch buffer";
			return false;
		}
		api_.scratch_buffer = scratch;
		api_.scratch_size = scratch_size;
	}

	const FfxDevice ffx_device = api_.GetDeviceDX11(device_);
	const FfxErrorCode iface_res = api_.GetInterfaceDX11(&backend_, ffx_device, api_.scratch_buffer,
		api_.scratch_size, FFX_FSR3UPSCALER_CONTEXT_COUNT);
	if (iface_res != FFX_OK) {
		status = UpscalerStatus::InitFailed;
		last_error = ffx_format_result(L"ffxGetInterfaceDX11 failed", iface_res);
		return false;
	}

	FfxFsr3UpscalerContextDescription desc{};
	desc.flags = FFX_FSR3UPSCALER_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
		FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
	desc.maxRenderSize.width = render_width_;
	desc.maxRenderSize.height = render_height_;
	desc.maxUpscaleSize.width = w;
	desc.maxUpscaleSize.height = h;
	desc.fpMessage = nullptr;
	desc.backendInterface = backend_;

	unsigned long seh_code = 0;
	const FfxErrorCode cr = create_context_guarded(api_, &context_, &desc, &seh_code);
	if (seh_code != 0) {
		crashed = true;
		status = UpscalerStatus::Crashed;
		wchar_t buf[128]{};
		_snwprintf_s(buf, _TRUNCATE, L"FFX CreateContext CRASHED (exception 0x%08lX) - FSR disabled", seh_code);
		last_error = buf;
		return false;
	}
	if (cr != FFX_OK) {
		status = UpscalerStatus::InitFailed;
		last_error = ffx_format_result(L"ffxFsr3UpscalerContextCreate failed", cr);
		return false;
	}

	initialized_ = true;
	created_quality_mode_ = quality_mode_;
	created_render_scale_ = render_scale_;
	status = UpscalerStatus::Ready;
	last_error.clear();
	return true;
}

bool Fsr31D3D11Backend::ensure_scratch(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	if (!initialized_ || device_ == nullptr)
		return false;

	uint32_t rw = 0, rh = 0;
	if (render_scale_ > 0.0f)
		scaled_render_size(w, h, render_scale_, &rw, &rh);
	else if (!query_render_size(w, h, ffx_quality_for(quality_mode_), rw, rh))
		return false;

	if (output_tex_ != nullptr && width_ == w && height_ == h && render_width_ == rw &&
		render_height_ == rh && scratch_format_ == fmt &&
		created_backbuffer_format_ == backbuffer_format_ && created_quality_mode_ == quality_mode_ &&
		created_render_scale_ == render_scale_)
		return true;

	release_scratch();
	width_ = w;
	height_ = h;
	render_width_ = rw;
	render_height_ = rh;
	scratch_format_ = fmt;

	if (!create_tex2d(device_, width_, height_, fmt, true, &output_tex_) ||
		!create_tex2d(device_, render_width_, render_height_, fmt, true, &color_copy_)) {
		release_scratch();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create FSR D3D11 scratch textures";
		return false;
	}
	const bool want_depth = needs_depth_scratch(quality_mode_) ||
		(render_width_ != width_ || render_height_ != height_);
	if (want_depth &&
		!create_tex2d(device_, render_width_, render_height_, DXGI_FORMAT_R32_FLOAT, false, &depth_scratch_)) {
		release_scratch();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create FSR D3D11 depth scratch";
		return false;
	}
	if (!create_tex2d(device_, width_, height_, backbuffer_format_, false, &color_full_)) {
		release_scratch();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create FSR D3D11 color staging";
		return false;
	}
	created_backbuffer_format_ = backbuffer_format_;
	created_quality_mode_ = quality_mode_;
	created_render_scale_ = render_scale_;
	return true;
}

bool Fsr31D3D11Backend::run(
	reshade::api::effect_runtime *runtime,
	const FrameInputs &inputs,
	const UpscalerParams &params)
{
	if (crashed)
		return false;
	if (runtime == nullptr)
		return false;

	quality_mode_ = params.quality_mode;
	render_scale_ = params.render_scale;
	sharpness_ = params.sharpness;
	frame_time_ms_ = params.frame_time_ms;

	if (!ensure_dll_present())
		return false;

	reshade::api::device *const rdevice = runtime->get_device();
	if (rdevice == nullptr || rdevice->get_api() != reshade::api::device_api::d3d11)
		return false;

	auto *const d3d = reinterpret_cast<ID3D11Device *>(rdevice->get_native());
	reshade::api::command_queue *const queue = runtime->get_command_queue();
	if (queue == nullptr)
		return false;
	auto *const ctx = reinterpret_cast<ID3D11DeviceContext *>(queue->get_native());
	if (ctx == nullptr || d3d == nullptr)
		return false;

	auto *const backbuffer = reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(inputs.color.handle));
	auto *const motion_vectors = reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(inputs.motion_vectors.handle));
	auto *const depth = reinterpret_cast<ID3D11Resource *>(static_cast<uintptr_t>(inputs.depth.handle));

	if (backbuffer == nullptr || motion_vectors == nullptr) {
		status = UpscalerStatus::NeedMotionVectors;
		return false;
	}

	ID3D11Texture2D *bb_tex = nullptr;
	if (FAILED(backbuffer->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&bb_tex))) ||
		bb_tex == nullptr)
		return false;
	D3D11_TEXTURE2D_DESC bb_desc{};
	bb_tex->GetDesc(&bb_desc);
	bb_tex->Release();

	backbuffer_format_ = bb_desc.Format;
	const DXGI_FORMAT fmt = resolve_scratch_format(bb_desc.Format);
	const uint32_t w = bb_desc.Width;
	const uint32_t h = bb_desc.Height;

	if (!init_context(d3d, w, h)) {
		if (!crashed)
			status = UpscalerStatus::InitFailed;
		return false;
	}
	if (!blit_.ensure(d3d)) {
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create D3D11 blit pipeline";
		return false;
	}
	if (!ensure_scratch(w, h, fmt)) {
		status = UpscalerStatus::InitFailed;
		return false;
	}

	bool ok = true;

	const float shift_x = params.jitter_in_frame ? 0.0f : params.jitter_x;
	const float shift_y = params.jitter_in_frame ? 0.0f : params.jitter_y;
	const float jitter_u = render_width_ > 0 ? -shift_x / static_cast<float>(render_width_) : 0.0f;
	const float jitter_v = render_height_ > 0 ? -shift_y / static_cast<float>(render_height_) : 0.0f;

	ID3D11Resource *color_src = backbuffer;
	const bool resampled = jitter_u != 0.0f || jitter_v != 0.0f ||
		render_width_ != width_ || render_height_ != height_ ||
		scratch_format_ != backbuffer_format_;
	if (resampled && color_full_ != nullptr) {
		ctx->CopyResource(color_full_, backbuffer);
		color_src = color_full_;
	}
	if (!blit_.blit(ctx, color_src, color_copy_, 0.0f, jitter_u, jitter_v)) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = L"failed to blit color into FSR scratch";
		ok = false;
	}

	ID3D11Resource *depth_input = depth;
	if (ok && depth_scratch_ != nullptr && depth != nullptr) {
		if (blit_.blit(ctx, depth, depth_scratch_))
			depth_input = depth_scratch_;
		else
			depth_input = nullptr;
	}

	if (ok) {
		FfxFsr3UpscalerDispatchDescription dispatch{};
		dispatch.commandList = api_.GetCommandListDX11(ctx);
		dispatch.color = make_resource(color_copy_, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatch.depth = depth_input != nullptr
			? make_resource(depth_input, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ)
			: FfxResource{};
		dispatch.motionVectors = make_resource(motion_vectors, FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatch.exposure = FfxResource{};
		dispatch.reactive = FfxResource{};
		dispatch.transparencyAndComposition = FfxResource{};
		dispatch.output = make_resource(output_tex_, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
		dispatch.jitterOffset.x = params.jitter_x;
		dispatch.jitterOffset.y = params.jitter_y;
		dispatch.motionVectorScale.x = static_cast<float>(width_);
		dispatch.motionVectorScale.y = static_cast<float>(height_);
		dispatch.renderSize.width = render_width_;
		dispatch.renderSize.height = render_height_;
		dispatch.upscaleSize.width = width_;
		dispatch.upscaleSize.height = height_;
		dispatch.enableSharpening = false;
		dispatch.sharpness = 0.0f;
		dispatch.frameTimeDelta = frame_time_ms_;
		dispatch.preExposure = 1.0f;
		dispatch.reset = params.reset;
		dispatch.cameraNear = 0.0f;
		dispatch.cameraFar = 0.0f;
		dispatch.cameraFovAngleVertical = 0.0f;
		dispatch.viewSpaceToMetersFactor = 1.0f;
		dispatch.flags = 0;

		unsigned long seh_code = 0;
		const FfxErrorCode dr = dispatch_guarded(api_, &context_, &dispatch, &seh_code);
		if (seh_code != 0) {
			crashed = true;
			status = UpscalerStatus::Crashed;
			wchar_t buf[128]{};
			_snwprintf_s(buf, _TRUNCATE, L"FFX Dispatch CRASHED (exception 0x%08lX) - FSR disabled", seh_code);
			last_error = buf;
			ok = false;
		} else if (dr != FFX_OK) {
			status = UpscalerStatus::EvaluateFailed;
			last_error = ffx_format_result(L"ffxFsr3UpscalerContextDispatch failed", dr);
			ok = false;
		}
	}

	if (ok)
		ok = blit_.blit(ctx, output_tex_, backbuffer, params.sharpness);

	if (ok) {
		status = UpscalerStatus::Ready;
		last_error.clear();
	}
	return ok && !crashed;
}

}
