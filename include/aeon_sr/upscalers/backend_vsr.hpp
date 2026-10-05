#pragma once

#include "aeon_sr/interop/blit_d3d11.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>

#include <cstdint>

namespace reshade::api {
struct device;
}

namespace aeon_sr {

struct VsrProcessorStatus {
	bool capable = false;
	bool enabled = false;
	bool in_use = false;
	uint32_t level = 0;
	bool queried = false;
};

struct VsrD3D11Backend final : UpscalerBackend {
	const char *name() const override;
	reshade::api::device_api api() const override;
	bool runtime_present() override;
	bool run(reshade::api::effect_runtime *runtime,
		const FrameInputs &inputs,
		const UpscalerParams &params) override;
	void on_destroy_swapchain() override;
	void shutdown() override;

	bool vsr_initialized() const noexcept { return processor_ != nullptr; }
	uint32_t out_width() const noexcept { return width_; }
	uint32_t out_height() const noexcept { return height_; }
	uint32_t vsr_render_width() const noexcept { return render_width_; }
	uint32_t vsr_render_height() const noexcept { return render_height_; }
	const VsrProcessorStatus &vsr_status() const noexcept { return vsr_status_; }

private:
	ID3D11Device *device_ = nullptr;
	ID3D11DeviceContext *context_ = nullptr;
	ID3D11VideoDevice *video_device_ = nullptr;
	ID3D11VideoContext *video_context_ = nullptr;
	ID3D11VideoContext1 *video_context1_ = nullptr;
	ID3D11VideoProcessor *processor_ = nullptr;
	ID3D11VideoProcessorEnumerator *enumerator_ = nullptr;

	ID3D11Texture2D *color_full_ = nullptr;
	ID3D11Texture2D *input_tex_ = nullptr;
	ID3D11Texture2D *output_tex_ = nullptr;

	BlitPipelineD3D11 blit_;

	uint32_t width_ = 0, height_ = 0;
	uint32_t render_width_ = 0, render_height_ = 0;
	DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;

	uint32_t created_render_width_ = 0, created_render_height_ = 0;
	DXGI_FORMAT created_format_ = DXGI_FORMAT_UNKNOWN;

	VsrProcessorStatus vsr_status_;
	bool logged_once_ = false;

	bool ensure_device(reshade::api::device *device, ID3D11DeviceContext *ctx);
	bool ensure_processor(uint32_t render_w, uint32_t render_h, uint32_t out_w, uint32_t out_h, DXGI_FORMAT fmt);
	void release_processor();
	void release_scratch();
	bool set_super_resolution(bool enable);
	void query_super_resolution();
};

}
