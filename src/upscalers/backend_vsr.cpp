#include "aeon_sr/upscalers/backend_vsr.hpp"

#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/interop/native_d3d.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"

#include <cstdio>
#include <cstring>

namespace aeon_sr {
namespace {

constexpr GUID kNvidiaPpeInterfaceGuid = {
	0xD43CE1B3, 0x1F4B, 0x48AC, { 0xBA, 0xEE, 0xC3, 0xC2, 0x53, 0x75, 0xE6, 0xF7 }
};
constexpr UINT kStreamExtensionVersionV1 = 0x1;
constexpr UINT kStreamExtensionMethodSuperResolution = 0x2;

struct NvidiaVsrSet {
	UINT version;
	UINT method;
	UINT enable;
};

struct NvidiaVsrGetData {
	UINT gpu_is_vsr_capable : 1;
	UINT other_fields_valid : 1;
	UINT enabled : 1;
	UINT in_use_for_this_vp : 1;
	UINT level : 3;
	UINT reserved : 25;
};

DXGI_FORMAT tex_format(ID3D11Resource *res)
{
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
	if (res == nullptr)
		return fmt;
	ID3D11Texture2D *t = nullptr;
	if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t))) && t) {
		D3D11_TEXTURE2D_DESC d{};
		t->GetDesc(&d);
		fmt = d.Format;
		t->Release();
	}
	return fmt;
}

bool make_tex(ID3D11Device *dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, ID3D11Texture2D **out)
{
	if (dev == nullptr || out == nullptr || w == 0 || h == 0 || fmt == DXGI_FORMAT_UNKNOWN)
		return false;
	D3D11_TEXTURE2D_DESC d{};
	d.Width = w;
	d.Height = h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = bind;
	return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, out));
}

template <class T>
void release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

DXGI_FORMAT vp_format_for(DXGI_FORMAT backbuffer)
{
	switch (backbuffer) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

}

const char *VsrD3D11Backend::name() const { return "RTX VSR"; }
reshade::api::device_api VsrD3D11Backend::api() const { return reshade::api::device_api::d3d11; }

bool VsrD3D11Backend::runtime_present() { return true; }

void VsrD3D11Backend::on_destroy_swapchain()
{
	release_processor();
	release_scratch();
}

void VsrD3D11Backend::release_scratch()
{
	release(color_full_);
	release(input_tex_);
	release(output_tex_);
}

void VsrD3D11Backend::release_processor()
{
	release(processor_);
	release(enumerator_);
	created_render_width_ = created_render_height_ = 0;
	created_format_ = DXGI_FORMAT_UNKNOWN;
}

void VsrD3D11Backend::shutdown()
{
	release_processor();
	release_scratch();
	blit_.release();
	release(video_context1_);
	release(video_context_);
	release(video_device_);

	device_ = nullptr;
	context_ = nullptr;
	if (status != UpscalerStatus::MissingRuntime)
		status = UpscalerStatus::Idle;
}

bool VsrD3D11Backend::ensure_device(reshade::api::device *device, ID3D11DeviceContext *ctx)
{
	ID3D11Device *const dev = native_d3d11_device(device);
	if (dev == nullptr || ctx == nullptr) {
		status = UpscalerStatus::UnsupportedApi;
		last_error = L"RTX VSR runs through the D3D11 video processor and is unavailable on this "
			L"API. Use DLSS on a D3D12 game.";
		return false;
	}
	if (device_ == dev && video_device_ != nullptr && video_context_ != nullptr) {
		context_ = ctx;
		return true;
	}

	shutdown();
	device_ = dev;
	context_ = ctx;

	if (FAILED(dev->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void **>(&video_device_))) ||
		video_device_ == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = L"ID3D11VideoDevice unavailable (no video processor on this device)";
		return false;
	}
	if (FAILED(ctx->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void **>(&video_context_))) ||
		video_context_ == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = L"ID3D11VideoContext unavailable";
		release(video_device_);
		return false;
	}

	if (FAILED(ctx->QueryInterface(__uuidof(ID3D11VideoContext1), reinterpret_cast<void **>(&video_context1_))))
		video_context1_ = nullptr;
	return true;
}

bool VsrD3D11Backend::ensure_processor(uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h,
	DXGI_FORMAT fmt)
{
	if (processor_ != nullptr && enumerator_ != nullptr && width_ == out_w && height_ == out_h &&
		created_render_width_ == render_w && created_render_height_ == render_h && created_format_ == fmt &&
		color_full_ != nullptr && input_tex_ != nullptr && output_tex_ != nullptr)
		return true;

	release_processor();
	release_scratch();

	D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
	desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
	desc.InputWidth = render_w;
	desc.InputHeight = render_h;
	desc.OutputWidth = out_w;
	desc.OutputHeight = out_h;
	desc.InputFrameRate.Numerator = 60;
	desc.InputFrameRate.Denominator = 1;
	desc.OutputFrameRate.Numerator = 60;
	desc.OutputFrameRate.Denominator = 1;
	desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

	if (FAILED(video_device_->CreateVideoProcessorEnumerator(&desc, &enumerator_)) || enumerator_ == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = L"CreateVideoProcessorEnumerator failed";
		return false;
	}
	if (FAILED(video_device_->CreateVideoProcessor(enumerator_, 0, &processor_)) || processor_ == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = L"CreateVideoProcessor failed";
		release(enumerator_);
		return false;
	}

	video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_, 0, FALSE);
	video_context_->VideoProcessorSetStreamFrameFormat(processor_, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
	if (video_context1_ != nullptr) {
		video_context1_->VideoProcessorSetStreamColorSpace1(processor_, 0,
			DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
		video_context1_->VideoProcessorSetOutputColorSpace1(processor_,
			DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
	} else {
		D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};
		cs.Usage = 0;
		cs.RGB_Range = 0;
		cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
		video_context_->VideoProcessorSetStreamColorSpace(processor_, 0, &cs);
		video_context_->VideoProcessorSetOutputColorSpace(processor_, &cs);
	}

	const bool ok =
		make_tex(device_, out_w, out_h, fmt, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &color_full_) &&
		make_tex(device_, render_w, render_h, fmt, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &input_tex_) &&
		make_tex(device_, out_w, out_h, fmt, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &output_tex_);
	if (!ok) {
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create VSR staging/input/output textures";
		release_processor();
		release_scratch();
		return false;
	}

	width_ = out_w;
	height_ = out_h;
	render_width_ = render_w;
	render_height_ = render_h;
	format_ = fmt;
	created_render_width_ = render_w;
	created_render_height_ = render_h;
	created_format_ = fmt;
	return true;
}

bool VsrD3D11Backend::set_super_resolution(bool enable)
{
	NvidiaVsrSet ext{ kStreamExtensionVersionV1, kStreamExtensionMethodSuperResolution, enable ? 1u : 0u };
	const HRESULT hr = video_context_->VideoProcessorSetStreamExtension(
		processor_, 0, &kNvidiaPpeInterfaceGuid, sizeof(ext), &ext);
	return SUCCEEDED(hr);
}

void VsrD3D11Backend::query_super_resolution()
{
	NvidiaVsrGetData data{};
	const HRESULT hr = video_context_->VideoProcessorGetStreamExtension(
		processor_, 0, &kNvidiaPpeInterfaceGuid, sizeof(data), &data);
	vsr_status_ = VsrProcessorStatus{};
	if (FAILED(hr))
		return;
	vsr_status_.queried = true;
	vsr_status_.capable = data.gpu_is_vsr_capable != 0;
	if (data.other_fields_valid) {
		vsr_status_.enabled = data.enabled != 0;
		vsr_status_.in_use = data.in_use_for_this_vp != 0;
		vsr_status_.level = data.level;
	}
}

bool VsrD3D11Backend::run(
	reshade::api::effect_runtime *runtime,
	const FrameInputs &inputs,
	const UpscalerParams &params)
{
	if (runtime == nullptr)
		return false;
	reshade::api::device *const device = runtime->get_device();
	if (device == nullptr)
		return false;
	ID3D11DeviceContext *const ctx = native_d3d11_context(runtime);
	if (!ensure_device(device, ctx))
		return false;

	auto *const backbuffer = native_res<ID3D11Resource>(inputs.color);
	if (backbuffer == nullptr) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = L"no colour resource";
		return false;
	}

	if (params.color_space != 0) {
		status = UpscalerStatus::UnsupportedApi;
		last_error = L"RTX VSR is an SDR feature; this frame is HDR (scRGB/PQ). "
			L"Use DLSS, or switch the game to SDR for VSR.";
		return false;
	}

	const DXGI_FORMAT bb_fmt = tex_format(backbuffer);
	const DXGI_FORMAT fmt = vp_format_for(bb_fmt);
	if (fmt == DXGI_FORMAT_UNKNOWN) {
		status = UpscalerStatus::UnsupportedApi;
		last_error = L"RTX VSR needs an 8-bit RGBA/BGRA frame; this back buffer format is not one.";
		return false;
	}

	uint32_t out_w = 0, out_h = 0;
	{
		D3D11_TEXTURE2D_DESC d{};
		ID3D11Texture2D *t = nullptr;
		if (SUCCEEDED(backbuffer->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&t))) && t) {
			t->GetDesc(&d);
			t->Release();
		}
		out_w = d.Width;
		out_h = d.Height;
	}
	if (out_w == 0 || out_h == 0)
		return false;

	float scale = params.render_scale > 0.0f ? params.render_scale : 1.0f;
	if (scale > 1.0f)
		scale = 1.0f;
	uint32_t render_w = out_w, render_h = out_h;
	scaled_render_size(out_w, out_h, scale, &render_w, &render_h);

	if (!ensure_processor(render_w, render_h, out_w, out_h, fmt))
		return false;

	ctx->CopyResource(color_full_, backbuffer);
	if (!blit_.ensure(device_) || !blit_.blit(ctx, color_full_, input_tex_)) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = L"failed to downscale the frame into the VSR input";
		return false;
	}

	set_super_resolution(true);

	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
	ivd.FourCC = 0;
	ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	ivd.Texture2D.MipSlice = 0;
	ivd.Texture2D.ArraySlice = 0;
	ID3D11VideoProcessorInputView *iview = nullptr;
	if (FAILED(video_device_->CreateVideoProcessorInputView(input_tex_, enumerator_, &ivd, &iview)) ||
		iview == nullptr) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = L"CreateVideoProcessorInputView failed";
		return false;
	}

	D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
	ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
	ovd.Texture2D.MipSlice = 0;
	ID3D11VideoProcessorOutputView *oview = nullptr;
	if (FAILED(video_device_->CreateVideoProcessorOutputView(output_tex_, enumerator_, &ovd, &oview)) ||
		oview == nullptr) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = L"CreateVideoProcessorOutputView failed";
		release(iview);
		return false;
	}

	const RECT src_rect{ 0, 0, static_cast<LONG>(render_w), static_cast<LONG>(render_h) };
	const RECT dst_rect{ 0, 0, static_cast<LONG>(out_w), static_cast<LONG>(out_h) };
	video_context_->VideoProcessorSetStreamSourceRect(processor_, 0, TRUE, &src_rect);
	video_context_->VideoProcessorSetStreamDestRect(processor_, 0, TRUE, &dst_rect);
	video_context_->VideoProcessorSetOutputTargetRect(processor_, TRUE, &dst_rect);

	D3D11_VIDEO_PROCESSOR_STREAM stream{};
	stream.Enable = TRUE;
	stream.OutputIndex = 0;
	stream.InputFrameOrField = 0;
	stream.PastFrames = 0;
	stream.FutureFrames = 0;
	stream.pInputSurface = iview;

	const HRESULT blt = video_context_->VideoProcessorBlt(processor_, oview, 0, 1, &stream);
	release(oview);
	release(iview);

	if (FAILED(blt)) {

		set_super_resolution(false);
		status = UpscalerStatus::EvaluateFailed;
		last_error = L"VideoProcessorBlt failed (driver rejected RTX VSR?)";
		return false;
	}

	query_super_resolution();

	if (!logged_once_) {
		logged_once_ = true;
		wchar_t buf[192]{};
		_snwprintf_s(buf, _TRUNCATE, L"[VSR] Blt %ux%u -> %ux%u  capable=%d enabled=%d in_use=%d level=%u",
			render_w, render_h, out_w, out_h, vsr_status_.capable ? 1 : 0,
			vsr_status_.enabled ? 1 : 0, vsr_status_.in_use ? 1 : 0, vsr_status_.level);
		diag_info("vsr", buf);
	}

	ctx->CopyResource(backbuffer, output_tex_);

	status = UpscalerStatus::Ready;
	if (!vsr_status_.capable) {

		last_error = L"the driver reports this GPU cannot run RTX VSR; the video processor "
			L"scaled the frame without it.";
	} else if (vsr_status_.queried && !vsr_status_.enabled) {
		last_error = L"RTX video enhancement is off in the NVIDIA Control Panel; the frame was "
			L"scaled without the VSR network. Turn it on there.";
	} else {
		last_error.clear();
	}
	return true;
}

}
