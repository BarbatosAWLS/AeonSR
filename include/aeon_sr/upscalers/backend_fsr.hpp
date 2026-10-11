#pragma once

#include "aeon_sr/interop/blit_d3d11.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/upscalers/ffx_dx11_loader.hpp"
#include "aeon_sr/upscalers/ffx_loader.hpp"
#include "aeon_sr/interop/gpu12.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace aeon_sr {

struct FsrPipelineCache;
struct FsrWarmupDesc;

struct Fsr31D3D11Backend final : UpscalerBackend {
	std::wstring addon_dir;
	std::wstring dll_dir;
	std::wstring loaded_dll_name;

	const char *name() const override;
	reshade::api::device_api api() const override;
	bool runtime_present() override;
	bool run(reshade::api::effect_runtime *runtime,
		const FrameInputs &inputs,
		const UpscalerParams &params) override;
	void on_destroy_swapchain() override;
	void shutdown() override;

	bool fsr_initialized() const noexcept { return initialized_; }
	uint32_t out_width() const noexcept { return width_; }
	uint32_t out_height() const noexcept { return height_; }
	uint32_t fsr_render_width() const noexcept { return render_width_; }
	uint32_t fsr_render_height() const noexcept { return render_height_; }

private:
	FfxDx11Api api_{};
	FfxFsr3UpscalerContext context_{};
	FfxInterface backend_{};
	bool dll_present_ = false;
	bool initialized_ = false;

	ID3D11Device *device_ = nullptr;

	uint32_t width_ = 0;
	uint32_t height_ = 0;
	uint32_t render_width_ = 0;
	uint32_t render_height_ = 0;
	uint32_t quality_mode_ = 0;
	float render_scale_ = 0.0f;
	float sharpness_ = 0.0f;
	float frame_time_ms_ = 0.0f;

	DXGI_FORMAT scratch_format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT backbuffer_format_ = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT created_backbuffer_format_ = DXGI_FORMAT_UNKNOWN;
	uint32_t created_quality_mode_ = 0xFFFFFFFFu;
	float created_render_scale_ = -1.0f;

	ID3D11Texture2D *output_tex_ = nullptr;
	ID3D11Texture2D *color_copy_ = nullptr;
	ID3D11Texture2D *depth_scratch_ = nullptr;
	ID3D11Texture2D *color_full_ = nullptr;

	BlitPipelineD3D11 blit_;

	bool ensure_dll_present();
	bool init_context(ID3D11Device *device, uint32_t w, uint32_t h);
	void destroy_context();
	void release_scratch();
	bool ensure_scratch(uint32_t w, uint32_t h, DXGI_FORMAT fmt);
	bool query_render_size(uint32_t display_w, uint32_t display_h, uint32_t quality,
		uint32_t &out_w, uint32_t &out_h);
	FfxResource make_resource(ID3D11Resource *res, FfxResourceStates state) const;
};

struct Fsr4D3D12Backend final : UpscalerBackend {

	std::wstring provider_version;

	struct FfxProvider {
		unsigned long long id = 0;
		std::string name;
	};
	std::vector<FfxProvider> providers;

	std::string want_provider;
	std::string created_provider;

	std::wstring addon_dir;
	std::wstring dll_dir;

	~Fsr4D3D12Backend() override
	{
		if (init_thread_.joinable()) {
			if (cancel_event_ != nullptr)
				SetEvent(cancel_event_);
			init_thread_.join();
		}
		if (cancel_event_ != nullptr)
			CloseHandle(cancel_event_);
	}
	const char *name() const override;
	reshade::api::device_api api() const override;
	bool runtime_present() override;
	bool run(reshade::api::effect_runtime *runtime,
		const FrameInputs &inputs,
		const UpscalerParams &params) override;
	void on_destroy_swapchain() override;
	void shutdown() override;

	void set_color_space(uint32_t color_space);

	bool run_native(ID3D12Device *device, ID3D12CommandQueue *queue,
		ID3D12GraphicsCommandList *cmd12,
		ID3D12Resource *backbuffer, ID3D12Resource *motion_vectors,
		ID3D12Resource *depth,
		D3D12_RESOURCE_STATES backbuffer_state,
		const UpscalerParams &params);

	bool init_context(ID3D12Device *device, uint32_t w, uint32_t h,
		DXGI_FORMAT color_format = DXGI_FORMAT_R8G8B8A8_UNORM, bool with_depth = true);

	void install_api_for_test(const FfxApi &api) noexcept { api_ = api; }
	std::wstring cache_dir_override;

	bool fsr_initialized() const noexcept { return initialized_; }
	bool fsr_loading() const noexcept { return init_state_.load(std::memory_order_acquire) == kRunning; }
	bool fsr_device_busy() const noexcept { return device_busy_.load(std::memory_order_acquire); }
	uint32_t out_width() const noexcept { return width_; }
	uint32_t out_height() const noexcept { return height_; }
	uint32_t fsr_render_width() const noexcept { return render_width_; }
	uint32_t fsr_render_height() const noexcept { return render_height_; }

private:
	FfxApi api_{};
	ffxContext context_ = nullptr;
	bool dll_present_ = false;
	bool initialized_ = false;

	static constexpr int kIdle = 0, kRunning = 1, kFinished = 2;
	std::thread init_thread_;
	std::atomic<int> init_state_{ kIdle };
	std::atomic<bool> device_busy_{ false };
	bool init_ok_ = false;
	UpscalerStatus worker_status_ = UpscalerStatus::Idle;
	std::wstring worker_error_;
	bool build_context(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT color_format,
		bool with_depth);
	bool reset_pending_ = false;
	FsrPipelineCache *pso_cache_ = nullptr;

	static constexpr int kPrebuildNotRun = -1;
	static constexpr int kPrebuildNoCache = -2;
	static constexpr int kPrebuildMissing = -3;
	static constexpr int kPrebuildNoStart = -4;
	static constexpr int kPrebuildStopped = -5;
	static constexpr int kPrebuildGivenUp = -6;
	static constexpr unsigned long kPrebuildTimeoutMs = 10ul * 60ul * 1000ul;
	HANDLE cancel_event_ = nullptr;
	bool prebuild_given_up_ = false;
	uint32_t build_quality_ = 0;
	float build_scale_ = 0.0f;
	std::string build_provider_;
	uint32_t build_color_flags_ = 0;
	std::wstring build_dll_path_;
	std::wstring build_addon_dir_;
	int run_prebuild(ID3D12Device *device, const FsrWarmupDesc &desc, const std::wstring &cache_file,
		double *ms);
	static const wchar_t *prebuild_failure(int code);
	void join_init();

	ID3D12Device *device_ = nullptr;
	ID3D12CommandQueue *queue_ = nullptr;

	uint32_t width_ = 0;
	uint32_t height_ = 0;
	uint32_t render_width_ = 0;
	uint32_t render_height_ = 0;
	uint32_t quality_mode_ = 0;
	float render_scale_ = 0.0f;
	float sharpness_ = 0.0f;
	float frame_time_ms_ = 0.0f;

	DXGI_FORMAT scratch_format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT backbuffer_format_ = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT created_backbuffer_format_ = DXGI_FORMAT_UNKNOWN;
	uint32_t created_quality_mode_ = 0xFFFFFFFFu;
	float created_render_scale_ = -1.0f;
	uint32_t color_flags_ = 0;
	uint32_t created_color_flags_ = 0xFFFFFFFFu;

	static constexpr D3D12_RESOURCE_STATES kFfxSrvState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	ID3D12Resource *output_tex_ = nullptr;
	ID3D12Resource *color_copy_ = nullptr;
	ID3D12Resource *depth_scratch_ = nullptr;
	ID3D12Resource *color_full_ = nullptr;

	Gpu12Fence gpu_fence_;

	BlitPipelineD3D12 blit_;

	bool ensure_dll_present();

	void enumerate_providers(ID3D12Device *device);
	void destroy_context();
	void release_scratch();
	void wait_gpu();
	bool ensure_scratch(ID3D12GraphicsCommandList *cmd, uint32_t w, uint32_t h, DXGI_FORMAT fmt);
	bool query_render_size(uint32_t display_w, uint32_t display_h, uint32_t quality,
		uint32_t &out_w, uint32_t &out_h);
};

}
