#include "aeon_sr/upscalers/backend_xess.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/upscalers/fsr_warmup.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/core/runtime_search.hpp"
#include "aeon_sr/core/settings.hpp"

#include <d3d12.h>

#include <cstring>

namespace aeon_sr {

static constexpr uint32_t kXessInitFlags =
	XESS_INIT_FLAG_HIGH_RES_MV | XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE | XESS_INIT_FLAG_LDR_INPUT_COLOR;

xess_quality_settings_t XessD3D12Backend::quality_for(uint32_t mode) const noexcept
{
	switch (static_cast<UpscaleMode>(mode)) {
	case UpscaleMode::UltraQuality: return XESS_QUALITY_SETTING_ULTRA_QUALITY;
	case UpscaleMode::Quality: return XESS_QUALITY_SETTING_QUALITY;
	case UpscaleMode::Balanced: return XESS_QUALITY_SETTING_BALANCED;
	case UpscaleMode::Performance: return XESS_QUALITY_SETTING_PERFORMANCE;
	case UpscaleMode::UltraPerformance: return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
	case UpscaleMode::Dlaa:
	default: return XESS_QUALITY_SETTING_AA;
	}
}

const char *XessD3D12Backend::name() const { return "XeSS"; }

reshade::api::device_api XessD3D12Backend::api() const
{
	return reshade::api::device_api::d3d12;
}

bool XessD3D12Backend::ensure_dll_present()
{
	if (api_.module != nullptr)
		return true;
	for (const auto &p : runtime_candidates(runtime_search_dirs(addon_dir, exe_directory_w()), L"libxess.dll")) {
		if (file_exists_w(p)) {
			dll_path_ = p;
			return true;
		}
	}
	status = UpscalerStatus::MissingRuntime;
	last_error = L"Place libxess.dll next to AeonSR.addon64 (run scripts/bootstrap-sr-runtimes.ps1)";
	return false;
}

bool XessD3D12Backend::runtime_present()
{
	if (xess_loading())
		return true;
	return ensure_dll_present();
}

void XessD3D12Backend::wait_gpu()
{
	if (queue_ == nullptr || device_ == nullptr)
		return;
	if (context_ == nullptr && output_tex_ == nullptr && color_copy_ == nullptr && color_full_ == nullptr)
		return;
	if (!gpu_fence_.signal_and_wait(device_, queue_, 5000))
		diag_warn("upscaler", L"XeSS: the GPU did not finish its last frame before XeSS was rebuilt");
}

void XessD3D12Backend::release_resources()
{
	auto rel = [](ID3D12Resource *&r) { if (r) { r->Release(); r = nullptr; } };
	rel(output_tex_);
	rel(color_copy_);
	rel(color_full_);
	created_backbuffer_format_ = DXGI_FORMAT_UNKNOWN;
	created_quality_mode_ = 0xFFFFFFFFu;
	created_render_scale_ = -1.0f;
}

void XessD3D12Backend::destroy_context()
{
	release_resources();
	if (context_ != nullptr && api_.DestroyContext != nullptr)
		api_.DestroyContext(context_);
	context_ = nullptr;
	device_ = nullptr;
	prepared_ = false;
	delete pso_cache_;
	pso_cache_ = nullptr;
	blit_.release();
	gpu_fence_.release();
	queue_ = nullptr;
}

void XessD3D12Backend::join_init()
{
	if (init_thread_.joinable())
		init_thread_.join();
	if (init_state_.load(std::memory_order_acquire) != kIdle) {
		status = worker_status_;
		last_error = worker_error_;
		render_width_ = worker_render_w_;
		render_height_ = worker_render_h_;
		init_state_.store(kIdle, std::memory_order_release);
	}
}

void XessD3D12Backend::on_destroy_swapchain()
{
	wait_gpu();
	release_resources();
}

void XessD3D12Backend::shutdown()
{
	join_init();
	destroy_context();
	xess_unload(api_);
}

bool XessD3D12Backend::build(ID3D12Device *device, uint32_t w, uint32_t h, uint32_t quality, float scale)
{
	LARGE_INTEGER freq{}, t0{}, t1{};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);

	if (api_.module == nullptr) {
		std::wstring err;
		if (!xess_load(api_, dll_path_, err)) {
			worker_status_ = UpscalerStatus::InitFailed;
			worker_error_ = err;
			return false;
		}
	}

	bool built_pipelines = false;
	if (context_ == nullptr) {
		const xess_result_t cr = api_.CreateContext(device, &context_);
		if (cr != XESS_RESULT_SUCCESS || context_ == nullptr) {
			wchar_t buf[128]{};
			_snwprintf_s(buf, _TRUNCATE, L"xessD3D12CreateContext failed (%d)", static_cast<int>(cr));
			worker_status_ = UpscalerStatus::InitFailed;
			worker_error_ = buf;
			context_ = nullptr;
			return false;
		}
		pso_cache_ = new FsrPipelineCache();
		const std::wstring dir = !cache_dir_override.empty() ? cache_dir_override : local_appdata_dir_w();
		if (!dir.empty()) {
			CreateDirectoryW(dir.c_str(), nullptr);
			WIN32_FILE_ATTRIBUTE_DATA fa{};
			unsigned long long dll_bytes = 0;
			if (!dll_path_.empty() && GetFileAttributesExW(dll_path_.c_str(), GetFileExInfoStandard, &fa))
				dll_bytes = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
			wchar_t prefix[40]{};
			_snwprintf_s(prefix, _TRUNCATE, L"xess_%llx", dll_bytes);
			pso_cache_->open(device, pipeline_cache_path(dir, device, prefix));
		}
		if (api_.BuildPipelines) {
			const xess_result_t br = api_.BuildPipelines(context_, pso_cache_->lib, true, kXessInitFlags);
			if (br != XESS_RESULT_SUCCESS) {
				wchar_t buf[128]{};
				_snwprintf_s(buf, _TRUNCATE, L"xessD3D12BuildPipelines failed (%d)", static_cast<int>(br));
				worker_status_ = UpscalerStatus::InitFailed;
				worker_error_ = buf;
				return false;
			}
			pso_cache_->save();
			built_pipelines = true;
		}
	}

	xess_2d_t out_res{ w, h };
	xess_2d_t in_res{ w, h };
	xess_quality_settings_t q = quality_for(quality);
	if (scale > 0.0f) {
		uint32_t rw = 0, rh = 0;
		scaled_render_size(w, h, scale, &rw, &rh);
		in_res = { rw, rh };
		q = (rw == w && rh == h) ? XESS_QUALITY_SETTING_AA : XESS_QUALITY_SETTING_ULTRA_QUALITY;
	} else if (api_.GetInputResolution(context_, &out_res, q, &in_res) != XESS_RESULT_SUCCESS) {
		in_res = out_res;
	}

	xess_d3d12_init_params_t init{};
	init.outputResolution = out_res;
	init.qualitySetting = q;
	init.initFlags = kXessInitFlags;
	init.creationNodeMask = 1;
	init.visibleNodeMask = 1;
	const xess_result_t ir = api_.Init(context_, &init);
	if (ir != XESS_RESULT_SUCCESS) {
		wchar_t buf[128]{};
		_snwprintf_s(buf, _TRUNCATE, L"xessD3D12Init failed (%d)", static_cast<int>(ir));
		worker_status_ = UpscalerStatus::InitFailed;
		worker_error_ = buf;
		return false;
	}
	worker_render_w_ = in_res.x;
	worker_render_h_ = in_res.y;
	prepared_ = true;
	prepared_w_ = w;
	prepared_h_ = h;
	prepared_quality_ = quality;
	prepared_scale_ = scale;
	worker_status_ = UpscalerStatus::Idle;
	worker_error_.clear();

	QueryPerformanceCounter(&t1);
	const FsrPipelineCacheStats cs = pso_cache_ != nullptr ? pso_cache_->st : FsrPipelineCacheStats{};
	diag_logf(DiagLevel::Info, "upscaler",
		L"XeSS came up in %.0f ms on a thread of its own, frames kept going (%ux%u -> %ux%u%s)",
		freq.QuadPart == 0 ? 0.0 : 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
			static_cast<double>(freq.QuadPart),
		in_res.x, in_res.y, w, h,
		!built_pipelines ? L""
			: cs.file_read ? (cs.saved ? L", pipelines from the shared cache and added to it"
				: L", pipelines from the shared cache")
			: cs.saved ? L", pipelines built and saved to the shared cache" : L"");
	return true;
}

bool XessD3D12Backend::ensure_resources(uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	if (device_ == nullptr || context_ == nullptr)
		return false;

	const DXGI_FORMAT scratch = resolve_scratch_format(fmt);
	const bool same = (width_ == w && height_ == h && created_backbuffer_format_ == fmt &&
		created_quality_mode_ == quality_mode_ && created_render_scale_ == render_scale_ &&
		output_tex_ != nullptr && color_full_ != nullptr && color_copy_ != nullptr);
	if (same)
		return true;

	wait_gpu();
	release_resources();
	width_ = w;
	height_ = h;
	backbuffer_format_ = fmt;
	scratch_format_ = scratch;

	if (!create_tex12(device_, w, h, scratch, true, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &output_tex_) ||
		!create_tex12(device_, render_width_, render_height_, scratch, true, kXessSrvState, &color_copy_) ||
		!create_tex12(device_, w, h, fmt, false, D3D12_RESOURCE_STATE_COPY_DEST, &color_full_)) {
		status = UpscalerStatus::InitFailed;
		last_error = L"XeSS scratch allocation failed";
		release_resources();
		return false;
	}

	created_backbuffer_format_ = fmt;
	created_quality_mode_ = quality_mode_;
	created_render_scale_ = render_scale_;
	if (!blit_.ensure(device_)) {
		status = UpscalerStatus::InitFailed;
		last_error = L"XeSS blit pipeline failed";
		return false;
	}
	return true;
}

bool XessD3D12Backend::run(
	reshade::api::effect_runtime *runtime,
	const FrameInputs &inputs,
	const UpscalerParams &params)
{
	(void)runtime;
	if (crashed || !inputs.engine.ready() || inputs.engine.device == nullptr)
		return false;

	queue_ = inputs.engine.queue;
	return run_native(inputs.engine.device, inputs.engine.cmd,
		inputs.engine.color, inputs.engine.motion,
		inputs.engine.color_state, params);
}

bool XessD3D12Backend::run_native(
	ID3D12Device *d3d12, ID3D12GraphicsCommandList *cmd12,
	ID3D12Resource *backbuffer, ID3D12Resource *motion_vectors,
	D3D12_RESOURCE_STATES backbuffer_state,
	const UpscalerParams &params)
{
	if (d3d12 == nullptr || cmd12 == nullptr)
		return false;
	if (crashed)
		return false;

	const int state = init_state_.load(std::memory_order_acquire);
	if (state == kRunning) {
		status = UpscalerStatus::Loading;
		return false;
	}
	if (state == kFinished) {
		join_init();
		if (!init_ok_)
			return false;
	}

	quality_mode_ = params.quality_mode;
	render_scale_ = params.render_scale;

	if (!ensure_dll_present())
		return false;
	if (backbuffer == nullptr || motion_vectors == nullptr)
		return false;
	const D3D12_RESOURCE_DESC bb_desc = backbuffer->GetDesc();
	const uint32_t w = static_cast<uint32_t>(bb_desc.Width);
	const uint32_t h = bb_desc.Height;

	const bool ready = context_ != nullptr && device_ == d3d12 && prepared_ && prepared_w_ == w &&
		prepared_h_ == h && prepared_quality_ == quality_mode_ && prepared_scale_ == render_scale_;
	if (!ready) {
		if (device_ != d3d12) {
			destroy_context();
		} else {
			wait_gpu();
		}
		release_resources();
		prepared_ = false;
		device_ = d3d12;
		status = UpscalerStatus::Loading;
		worker_status_ = UpscalerStatus::Loading;
		worker_error_.clear();
		init_ok_ = false;
		init_state_.store(kRunning, std::memory_order_release);
		const uint32_t quality = quality_mode_;
		const float scale = render_scale_;
		init_thread_ = std::thread([this, d3d12, w, h, quality, scale] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			init_ok_ = build(d3d12, w, h, quality, scale);
			init_state_.store(kFinished, std::memory_order_release);
		});
		return false;
	}

	if (!ensure_resources(w, h, bb_desc.Format))
		return false;

	barrier12(cmd12, backbuffer, backbuffer_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmd12->CopyResource(color_full_, backbuffer);

	barrier12(cmd12, color_full_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	const float shift_x = params.jitter_in_frame ? 0.0f : params.jitter_x;
	const float shift_y = params.jitter_in_frame ? 0.0f : params.jitter_y;
	const float jitter_u = render_width_ > 0 ? -shift_x / static_cast<float>(render_width_) : 0.0f;
	const float jitter_v = render_height_ > 0 ? -shift_y / static_cast<float>(render_height_) : 0.0f;

	barrier12(cmd12, color_copy_, kXessSrvState, D3D12_RESOURCE_STATE_RENDER_TARGET);
	bool ok = blit_.draw_fullscreen(d3d12, cmd12, color_full_,
		color_copy_, scratch_format_, render_width_, render_height_,
		0.0f, 0u, jitter_u, jitter_v);
	barrier12(cmd12, color_copy_, D3D12_RESOURCE_STATE_RENDER_TARGET, kXessSrvState);
	barrier12(cmd12, color_full_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
	if (!ok) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = L"XeSS failed to blit color into render scratch";
	}

	if (ok && api_.SetVelocityScale)
		api_.SetVelocityScale(context_, static_cast<float>(width_), static_cast<float>(height_));

	if (ok) {
		barrier12(cmd12, output_tex_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		xess_d3d12_execute_params_t exec{};
		exec.pColorTexture = color_copy_;
		exec.pVelocityTexture = motion_vectors;
		exec.pOutputTexture = output_tex_;
		exec.jitterOffsetX = params.jitter_x;
		exec.jitterOffsetY = params.jitter_y;
		exec.exposureScale = 1.0f;
		exec.resetHistory = params.reset ? 1u : 0u;
		exec.inputWidth = render_width_;
		exec.inputHeight = render_height_;

		const xess_result_t er = api_.Execute(context_, cmd12, &exec);
		barrier12(cmd12, output_tex_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		if (er != XESS_RESULT_SUCCESS) {
			status = UpscalerStatus::EvaluateFailed;
			wchar_t buf[160]{};
			_snwprintf_s(buf, _TRUNCATE, L"xessD3D12Execute failed (%d) in=%ux%u out=%ux%u q=%u",
				static_cast<int>(er), render_width_, render_height_, width_, height_, quality_mode_);
			last_error = buf;
			ok = false;
		}
	}

	barrier12(cmd12, backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

	if (ok)
		ok = blit_.draw_fullscreen(d3d12, cmd12, output_tex_,
			backbuffer, backbuffer_format_, width_, height_, params.sharpness);

	if (backbuffer_state != D3D12_RESOURCE_STATE_RENDER_TARGET)
		barrier12(cmd12, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, backbuffer_state);

	if (ok) {
		status = UpscalerStatus::Ready;
		last_error.clear();
	}
	return ok && !crashed;
}

}
