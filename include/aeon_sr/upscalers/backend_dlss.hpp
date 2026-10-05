#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"
#include "aeon_sr/ngx/ngx_runtime.hpp"
#include "aeon_sr/ngx/ngx_runtime_d3d12.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <atomic>
#include <thread>

namespace aeon_sr {

struct DlssD3D11Backend final : UpscalerBackend {
	NgxRuntime *rt = nullptr;

	const char *name() const override;
	reshade::api::device_api api() const override;
	bool runtime_present() override;
	bool run(reshade::api::effect_runtime *runtime,
		const FrameInputs &inputs,
		const UpscalerParams &params) override;
	void on_destroy_swapchain() override;
	void shutdown() override;
};

struct DlssD3D12Backend final : UpscalerBackend {
	NgxRuntimeD3D12 *rt = nullptr;

	~DlssD3D12Backend() override
	{
		if (init_thread_.joinable())
			init_thread_.join();
	}

	bool dlss_loading() const noexcept { return init_state_.load(std::memory_order_acquire) == kRunning; }
	bool dlss_starting() const noexcept { return init_state_.load(std::memory_order_acquire) != kIdle; }

	const char *name() const override;
	reshade::api::device_api api() const override;
	bool runtime_present() override;
	bool run(reshade::api::effect_runtime *runtime,
		const FrameInputs &inputs,
		const UpscalerParams &params) override;
	void on_destroy_swapchain() override;
	void shutdown() override;

private:
	static constexpr int kIdle = 0, kRunning = 1, kFinished = 2;
	std::thread init_thread_;
	std::atomic<int> init_state_{ kIdle };
	bool init_ok_ = false;
	void join_init();
};

}
