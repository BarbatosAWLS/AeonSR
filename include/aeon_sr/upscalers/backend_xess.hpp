#pragma once

#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/interop/gpu12.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"
#include "aeon_sr/upscalers/xess_loader.hpp"

#include <d3d11.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace aeon_sr {

struct FsrPipelineCache;

struct XessD3D12Backend : UpscalerBackend {
	std::wstring addon_dir;
	std::wstring cache_dir_override;

	~XessD3D12Backend() override
	{
		if (init_thread_.joinable())
			init_thread_.join();
	}

	const char *name() const override;
	reshade::api::device_api api() const override;
	bool runtime_present() override;
	bool run(reshade::api::effect_runtime *runtime,
		const FrameInputs &inputs,
		const UpscalerParams &params) override;
	void on_destroy_swapchain() override;
	void shutdown() override;

	bool run_native(ID3D12Device *device, ID3D12GraphicsCommandList *cmd12,
		ID3D12Resource *backbuffer, ID3D12Resource *motion_vectors,
		D3D12_RESOURCE_STATES backbuffer_state,
		const UpscalerParams &params);

	void install_api_for_test(const XessApi &api) noexcept { api_ = api; }

	bool xess_loading() const noexcept { return init_state_.load(std::memory_order_acquire) == kRunning; }
	bool xess_initialized() const noexcept { return !xess_loading() && context_ != nullptr; }
	uint32_t out_width() const noexcept { return width_; }
	uint32_t out_height() const noexcept { return height_; }
	uint32_t xess_render_width() const noexcept { return render_width_; }
	uint32_t xess_render_height() const noexcept { return render_height_; }

private:
	XessApi api_{};
	xess_context_handle_t context_ = nullptr;
	ID3D12Device *device_ = nullptr;
	ID3D12CommandQueue *queue_ = nullptr;
	Gpu12Fence gpu_fence_;
	BlitPipelineD3D12 blit_;
	std::wstring dll_path_;

	static constexpr D3D12_RESOURCE_STATES kXessSrvState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	ID3D12Resource *output_tex_ = nullptr;
	ID3D12Resource *color_copy_ = nullptr;
	ID3D12Resource *color_full_ = nullptr;

	uint32_t width_ = 0, height_ = 0;
	uint32_t render_width_ = 0, render_height_ = 0;
	uint32_t quality_mode_ = 0;
	float render_scale_ = 0.0f;
	DXGI_FORMAT scratch_format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT backbuffer_format_ = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT created_backbuffer_format_ = DXGI_FORMAT_UNKNOWN;
	uint32_t created_quality_mode_ = 0xFFFFFFFFu;
	float created_render_scale_ = -1.0f;

	bool prepared_ = false;
	uint32_t prepared_w_ = 0, prepared_h_ = 0;
	uint32_t prepared_quality_ = 0xFFFFFFFFu;
	float prepared_scale_ = -1.0f;

	static constexpr int kIdle = 0, kRunning = 1, kFinished = 2;
	std::thread init_thread_;
	std::atomic<int> init_state_{ kIdle };
	bool init_ok_ = false;
	UpscalerStatus worker_status_ = UpscalerStatus::Idle;
	std::wstring worker_error_;
	uint32_t worker_render_w_ = 0, worker_render_h_ = 0;
	FsrPipelineCache *pso_cache_ = nullptr;

	bool ensure_dll_present();
	bool build(ID3D12Device *device, uint32_t w, uint32_t h, uint32_t quality, float scale);
	void join_init();
	bool ensure_resources(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
	void destroy_context();
	void release_resources();
	void wait_gpu();
	xess_quality_settings_t quality_for(uint32_t mode) const noexcept;
};

}
