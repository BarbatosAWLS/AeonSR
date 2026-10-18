#include "aeon_sr/upscalers/backend_dlss.hpp"
#include "aeon_sr/core/frame_inputs.hpp"

namespace aeon_sr {

const char *DlssD3D12Backend::name() const { return "DLSS"; }
reshade::api::device_api DlssD3D12Backend::api() const { return reshade::api::device_api::d3d12; }

bool DlssD3D12Backend::runtime_present()
{
	if (dlss_loading())
		return true;
	return rt != nullptr && dll_found(true);
}

bool DlssD3D12Backend::dll_found(bool may_keep)
{
	const uint64_t now = GetTickCount64();
	if (may_keep && looked_.fresh(now) && looked_.found() && rt->dll_present && found_layer_ == rt->layer_path)
		return true;
	const bool found = rt->ensure_dll_present();
	looked_.note(found, now);
	found_layer_ = rt->layer_path;
	return found;
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

	if (!dll_found(rt->ngx_initialized)) {
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

	const bool wrote = rt->run(inputs.engine.cmd,
		inputs.engine.color,
		inputs.engine.color_state,
		inputs.engine.motion,
		inputs.engine.depth,
		native_handle<ID3D12Resource>(params.bias_mask),
		inputs.engine.output,
		inputs.engine.output_state);

	sync_status(*this, *rt);
	return !rt->crashed && (inputs.engine.output == nullptr || wrote);
}

}
