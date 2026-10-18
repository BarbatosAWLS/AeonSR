#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"
#include "aeon_sr/ngx/ngx_runtime.hpp"
#include "aeon_sr/ngx/ngx_runtime_d3d12.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"
#include "aeon_sr/core/runtime_search.hpp"

#include <atomic>
#include <thread>

namespace aeon_sr {

inline void sync_status(UpscalerBackend &backend, const NgxSession &rt)
{
	backend.status = backend.crashed ? UpscalerStatus::Crashed : rt.status;
	backend.last_error = rt.last_error;
	backend.crashed = rt.crashed || backend.crashed;
}

template <class T>
T *native_handle(uint64_t handle) noexcept
{
	return reinterpret_cast<T *>(static_cast<uintptr_t>(handle));
}

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
	RecentLook looked_;
	std::wstring found_layer_;
	bool dll_found(bool may_keep);
};

}
