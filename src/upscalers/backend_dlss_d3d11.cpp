#include "aeon_sr/upscalers/backend_dlss.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/interop/native_d3d.hpp"

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

const char *DlssD3D11Backend::name() const { return "DLSS"; }
reshade::api::device_api DlssD3D11Backend::api() const { return reshade::api::device_api::d3d11; }
bool DlssD3D11Backend::runtime_present() { return rt != nullptr && rt->ensure_dll_present(); }
void DlssD3D11Backend::on_destroy_swapchain() { if (rt != nullptr) rt->destroy_feature(); }
void DlssD3D11Backend::shutdown() { if (rt != nullptr) rt->shutdown_device(); }

bool DlssD3D11Backend::run(
	reshade::api::effect_runtime *runtime,
	const FrameInputs &inputs,
	const UpscalerParams &params)
{
	if (rt == nullptr || runtime == nullptr)
		return false;

	if (!rt->ensure_dll_present()) {
		sync_status(*this, *rt);
		return false;
	}

	rt->cfg = params;

	reshade::api::device *const device = runtime->get_device();
	if (device == nullptr)
		return false;

	if (!rt->ngx_initialized) {
		ID3D11Device *const d3d = native_d3d11_device(device);
		uint32_t w = 0, h = 0;
		runtime->get_screenshot_width_and_height(&w, &h);
		if (d3d == nullptr || !rt->init_device(d3d, w, h)) {
			sync_status(*this, *rt);
			return false;
		}
	}

	ID3D11DeviceContext *const ctx = native_d3d11_context(runtime);
	if (ctx == nullptr)
		return false;

	const bool ok = rt->run(ctx,
		native_res<ID3D11Resource>(inputs.color),
		native_res<ID3D11Resource>(inputs.motion_vectors),
		native_res<ID3D11Resource>(inputs.depth),
		native_handle<ID3D11Resource>(params.bias_mask));

	sync_status(*this, *rt);
	return ok;
}

}
