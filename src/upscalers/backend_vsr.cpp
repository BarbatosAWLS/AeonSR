#include "aeon_sr/upscalers/backend_vsr.hpp"
#include "aeon_sr/core/com_release.hpp"

#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/interop/interop.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"

#include <d3d11_4.h>
#include <dxgi1_2.h>

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

bool make_pair(EngineDevice &engine, ID3D11Device *device11, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind,
	SharedPlane &out)
{
	if (device11 == nullptr || w == 0 || h == 0)
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
	d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	ID3D11Texture2D *tex = nullptr;
	if (FAILED(device11->CreateTexture2D(&d, nullptr, &tex)) || tex == nullptr)
		return false;
	IDXGIResource1 *res = nullptr;
	HANDLE handle = nullptr;
	ID3D12Resource *tex12 = nullptr;
	const bool ok = SUCCEEDED(tex->QueryInterface(IID_PPV_ARGS(&res))) && res != nullptr &&
		SUCCEEDED(res->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle)) &&
		SUCCEEDED(engine.device()->OpenSharedHandle(handle, IID_PPV_ARGS(&tex12)));
	safe_release(res);
	if (handle != nullptr)
		CloseHandle(handle);
	if (!ok || tex12 == nullptr) {
		safe_release(tex12);
		safe_release(tex);
		return false;
	}
	out.helper = tex;
	out.engine = tex12;
	out.width = w;
	out.height = h;
	out.format = fmt;
	return true;
}

void drop_pair(SharedPlane &plane)
{
	safe_release(plane.engine);
	safe_release(plane.helper);
	plane.game = { 0 };
	plane.width = plane.height = 0;
	plane.format = DXGI_FORMAT_UNKNOWN;
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
		safe_release(enumerator_);
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

const char *VsrD3D11Backend::name() const { return "RTX VSR"; }
reshade::api::device_api VsrD3D11Backend::api() const { return reshade::api::device_api::d3d12; }

bool VsrD3D11Backend::runtime_present() { return true; }

void VsrD3D11Backend::on_destroy_swapchain()
{
	release_processor();
	release_scratch();
}

void VsrD3D11Backend::release_scratch()
{
	safe_release(color_full_);
	safe_release(input_tex_);
	safe_release(output_tex_);
	if (engine != nullptr && (in_.engine != nullptr || out_.engine != nullptr || flush_trigger_ != nullptr))
		engine->drain();
	drop_pair(in_);
	drop_pair(out_);
	safe_release(flush_trigger_);
}

void VsrD3D11Backend::release_processor()
{
	safe_release(processor_);
	safe_release(enumerator_);
	created_render_width_ = created_render_height_ = 0;
	created_format_ = DXGI_FORMAT_UNKNOWN;
}

void VsrD3D11Backend::shutdown()
{
	release_processor();
	release_scratch();
	blit_.release();
	safe_release(video_context1_);
	safe_release(video_context_);
	safe_release(video_device_);
	safe_release(context4_);
	safe_release(fence11_);
	safe_release(fence12_);
	value_ = 0;

	device_ = nullptr;
	context_ = nullptr;
	if (status != UpscalerStatus::MissingRuntime)
		status = UpscalerStatus::Idle;
}

bool VsrD3D11Backend::ensure_device()
{
	ID3D11Device *const dev = engine != nullptr ? engine->device11() : nullptr;
	ID3D11DeviceContext *const ctx = engine != nullptr ? engine->context11() : nullptr;
	ID3D11Device5 *const dev5 = engine != nullptr ? engine->device11_5() : nullptr;
	if (dev == nullptr || ctx == nullptr || dev5 == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = L"RTX VSR needs a Direct3D 11.4 device beside the engine (Windows 10 1709 or newer)";
		return false;
	}
	if (device_ == dev && video_device_ != nullptr && video_context_ != nullptr && fence12_ != nullptr)
		return true;

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
		video_context_ == nullptr || FAILED(ctx->QueryInterface(IID_PPV_ARGS(&context4_))) || context4_ == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = L"ID3D11VideoContext unavailable";
		safe_release(video_context_);
		safe_release(video_device_);
		return false;
	}
	if (FAILED(ctx->QueryInterface(__uuidof(ID3D11VideoContext1), reinterpret_cast<void **>(&video_context1_))))
		video_context1_ = nullptr;

	HANDLE h = nullptr;
	const bool ok = SUCCEEDED(dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11_))) &&
		SUCCEEDED(fence11_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)) &&
		SUCCEEDED(engine->device()->OpenSharedHandle(h, IID_PPV_ARGS(&fence12_)));
	if (h != nullptr)
		CloseHandle(h);
	if (!ok) {
		status = UpscalerStatus::InitFailed;
		last_error = L"this driver refused a shared fence between Direct3D 11 and Direct3D 12";
		safe_release(fence11_);
		safe_release(fence12_);
		return false;
	}
	value_ = 0;
	return true;
}

bool VsrD3D11Backend::run(
	reshade::api::effect_runtime *runtime,
	const FrameInputs &inputs,
	const UpscalerParams &params)
{
	if (engine == nullptr || !inputs.engine.ready() || inputs.engine.color == nullptr)
		return false;
	const bool borrowed = engine->borrowed();
	reshade::api::command_queue *const game_queue = runtime != nullptr ? runtime->get_command_queue() : nullptr;
	reshade::api::command_list *const game_list = borrowed && game_queue != nullptr
		? game_queue->get_immediate_command_list() : nullptr;
	if (borrowed && (game_list == nullptr ||
			reinterpret_cast<ID3D12GraphicsCommandList *>(game_list->get_native()) != inputs.engine.cmd)) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = L"this frame is not on ReShade's own command list, so RTX VSR cannot take it";
		return false;
	}
	if (params.color_space != 0) {
		status = UpscalerStatus::UnsupportedApi;
		last_error = L"RTX VSR is an SDR feature; this frame is HDR (scRGB/PQ). "
			L"Use DLSS, or switch the game to SDR for VSR.";
		return false;
	}
	const D3D12_RESOURCE_DESC cd = inputs.engine.color->GetDesc();
	const DXGI_FORMAT fmt = vp_format_for(cd.Format);
	if (fmt == DXGI_FORMAT_UNKNOWN) {
		status = UpscalerStatus::UnsupportedApi;
		last_error = L"RTX VSR needs an 8-bit RGBA/BGRA frame; this back buffer format is not one.";
		return false;
	}
	const uint32_t out_w = static_cast<uint32_t>(cd.Width), out_h = cd.Height;
	if (out_w == 0 || out_h == 0)
		return false;
	if (!ensure_device())
		return false;

	float scale = params.render_scale > 0.0f ? params.render_scale : 1.0f;
	if (scale > 1.0f)
		scale = 1.0f;
	uint32_t render_w = out_w, render_h = out_h;
	scaled_render_size(out_w, out_h, scale, &render_w, &render_h);

	if (!ensure_processor(render_w, render_h, out_w, out_h, fmt))
		return false;
	if (!in_.matches(out_w, out_h, fmt) || !out_.matches(out_w, out_h, fmt)) {
		drop_pair(in_);
		drop_pair(out_);
		if (!make_pair(*engine, device_, out_w, out_h, fmt,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, in_) ||
			!make_pair(*engine, device_, out_w, out_h, fmt,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, out_)) {
			status = UpscalerStatus::InitFailed;
			last_error = L"the textures RTX VSR shares with the engine could not be made";
			return false;
		}
	}

	ID3D12GraphicsCommandList *const cmd = inputs.engine.cmd;
	const D3D12_RESOURCE_STATES cs = inputs.engine.color_state;
	barrier12(cmd, inputs.engine.color, cs, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier12(cmd, in_.engine, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->CopyResource(in_.engine, inputs.engine.color);
	barrier12(cmd, in_.engine, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
	barrier12(cmd, inputs.engine.color, D3D12_RESOURCE_STATE_COPY_SOURCE, cs);
	if (borrowed) {
		if (flush_trigger_ == nullptr) {
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC d{};
			d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			d.Width = 256;
			d.Height = 1;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.SampleDesc.Count = 1;
			d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			if (FAILED(engine->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&flush_trigger_)))) {
				status = UpscalerStatus::InitFailed;
				last_error = L"RTX VSR could not make the buffer that has ReShade submit its list";
				return false;
			}
		}
		const reshade::api::resource trigger{ reinterpret_cast<uintptr_t>(flush_trigger_) };
		game_list->barrier(trigger, reshade::api::resource_usage::unordered_access,
			reshade::api::resource_usage::unordered_access);
		game_queue->flush_immediate_command_list();
	} else if (!engine->submit_list()) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = L"the engine's list could not be submitted before RTX VSR: " + engine->last_error;
		list_lost = true;
		return false;
	}
	ID3D12CommandQueue *const queue = engine->queue();
	const uint64_t handed = ++value_;
	const bool signalled = SUCCEEDED(queue->Signal(fence12_, handed));
	if (!borrowed)
		engine->use_ring(EngineDevice::Ring::Remote);

	bool ran = false;
	if (signalled && SUCCEEDED(context4_->Wait(fence11_, handed))) {
		context_->CopyResource(color_full_, in_.helper);
		if (blit_.ensure(device_) && blit_.blit(context_, color_full_, input_tex_)) {
			set_super_resolution(true);
			D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
			ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
			D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
			ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
			ID3D11VideoProcessorInputView *iview = nullptr;
			ID3D11VideoProcessorOutputView *oview = nullptr;
			if (SUCCEEDED(video_device_->CreateVideoProcessorInputView(input_tex_, enumerator_, &ivd, &iview)) &&
				iview != nullptr &&
				SUCCEEDED(video_device_->CreateVideoProcessorOutputView(output_tex_, enumerator_, &ovd, &oview)) &&
				oview != nullptr) {
				const RECT src_rect{ 0, 0, static_cast<LONG>(render_w), static_cast<LONG>(render_h) };
				const RECT dst_rect{ 0, 0, static_cast<LONG>(out_w), static_cast<LONG>(out_h) };
				video_context_->VideoProcessorSetStreamSourceRect(processor_, 0, TRUE, &src_rect);
				video_context_->VideoProcessorSetStreamDestRect(processor_, 0, TRUE, &dst_rect);
				video_context_->VideoProcessorSetOutputTargetRect(processor_, TRUE, &dst_rect);
				D3D11_VIDEO_PROCESSOR_STREAM stream{};
				stream.Enable = TRUE;
				stream.pInputSurface = iview;
				const HRESULT blt = video_context_->VideoProcessorBlt(processor_, oview, 0, 1, &stream);
				if (SUCCEEDED(blt)) {
					context_->CopyResource(out_.helper, output_tex_);
					ran = true;
				} else {
					set_super_resolution(false);
					last_error = L"VideoProcessorBlt failed (driver rejected RTX VSR?)";
				}
			} else {
				last_error = L"the video processor's views could not be made";
			}
			safe_release(oview);
			safe_release(iview);
		} else {
			last_error = L"failed to downscale the frame into the VSR input";
		}
	} else {
		last_error = L"the engine and the video processor could not be ordered on the GPU";
	}
	const uint64_t back = ++value_;
	if (FAILED(context4_->Signal(fence11_, back)))
		fence12_->Signal(back);
	context_->Flush();
	queue->Wait(fence12_, back);
	ID3D12GraphicsCommandList *const again = borrowed
		? reinterpret_cast<ID3D12GraphicsCommandList *>(game_list->get_native()) : engine->begin_list();
	if (again != cmd) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = L"the engine's list could not be reopened after RTX VSR: " + engine->last_error;
		list_lost = true;
		return false;
	}
	if (!ran) {
		status = UpscalerStatus::EvaluateFailed;
		return false;
	}
	barrier12(cmd, inputs.engine.color, cs, D3D12_RESOURCE_STATE_COPY_DEST);
	barrier12(cmd, out_.engine, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmd->CopyResource(inputs.engine.color, out_.engine);
	barrier12(cmd, out_.engine, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
	barrier12(cmd, inputs.engine.color, D3D12_RESOURCE_STATE_COPY_DEST, cs);

	query_super_resolution();
	if (!logged_once_) {
		logged_once_ = true;
		wchar_t buf[224]{};
		_snwprintf_s(buf, _TRUNCATE, L"[VSR] Blt %ux%u -> %ux%u on the engine's planes  capable=%d enabled=%d "
			L"in_use=%d level=%u", render_w, render_h, out_w, out_h, vsr_status_.capable ? 1 : 0,
			vsr_status_.enabled ? 1 : 0, vsr_status_.in_use ? 1 : 0, vsr_status_.level);
		diag_info("vsr", buf);
	}

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
