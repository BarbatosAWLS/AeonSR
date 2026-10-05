#include "aeon_sr/upscalers/backend_dlss.hpp"
#include "aeon_sr/core/frame_inputs.hpp"

namespace aeon_sr {
namespace {

void sync_status(UpscalerBackend &backend, const NgxSession &rt)
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

}

const char *DlssD3D12Backend::name() const { return "DLSS"; }
reshade::api::device_api DlssD3D12Backend::api() const { return reshade::api::device_api::d3d12; }

bool DlssD3D12Backend::runtime_present()
{
	if (dlss_loading())
		return true;
	return rt != nullptr && rt->ensure_dll_present();
}

void DlssD3D12Backend::on_destroy_swapchain()
{
	if (rt != nullptr && !dlss_loading())
		rt->destroy_feature();
}

void DlssD3D12Backend::join_init()
{
	if (init_thread_.joinable())
		init_thread_.join();
	init_state_.store(kIdle, std::memory_order_release);
}

void DlssD3D12Backend::shutdown()
{
	join_init();
	if (rt != nullptr)
		rt->shutdown_device();
}

bool DlssD3D12Backend::run(
	reshade::api::effect_runtime *runtime,
	const FrameInputs &inputs,
	const UpscalerParams &params)
{
	if (rt == nullptr || !inputs.engine.ready())
		return false;
	if (runtime == nullptr && (inputs.width == 0 || inputs.height == 0))
		return false;

	const int state = init_state_.load(std::memory_order_acquire);
	if (state == kRunning) {
		status = UpscalerStatus::Loading;
		return false;
	}
	if (state == kFinished) {
		join_init();
		if (!init_ok_) {
			sync_status(*this, *rt);
			return false;
		}
	}

	if (!rt->ensure_dll_present()) {
		sync_status(*this, *rt);
		return false;
	}

	rt->cfg = params;

	if (inputs.engine.queue != nullptr)
		rt->set_command_queue(inputs.engine.queue);

	if (!rt->ngx_initialized) {
		uint32_t w = inputs.width, h = inputs.height;
		if ((w == 0 || h == 0) && runtime != nullptr)
			runtime->get_screenshot_width_and_height(&w, &h);
		if (inputs.engine.device == nullptr) {
			sync_status(*this, *rt);
			return false;
		}
		ID3D12Device *const device = inputs.engine.device;
		NgxRuntimeD3D12 *const session = rt;
		status = UpscalerStatus::Loading;
		init_ok_ = false;
		init_state_.store(kRunning, std::memory_order_release);
		init_thread_ = std::thread([this, session, device, w, h] {
			init_ok_ = session->init_device(device, w, h);
			init_state_.store(kFinished, std::memory_order_release);
		});
		return false;
	}

	rt->run(inputs.engine.cmd,
		inputs.engine.color,
		inputs.engine.color_state,
		inputs.engine.motion,
		inputs.engine.depth,
		native_handle<ID3D12Resource>(params.bias_mask));

	sync_status(*this, *rt);
	return !rt->crashed;
}

}
