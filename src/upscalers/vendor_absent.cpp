#include "aeon_sr/upscalers/backend_dlss.hpp"
#include "aeon_sr/upscalers/backend_fsr.hpp"
#include "aeon_sr/upscalers/backend_vsr.hpp"
#include "aeon_sr/upscalers/backend_xess.hpp"
#include "aeon_sr/ngx/ngx_dlssnr.hpp"
#include "aeon_sr/ngx/ngx_runtime.hpp"
#include "aeon_sr/ngx/ngx_runtime_d3d12.hpp"

#if defined(AEONSR_NO_VENDOR)

namespace aeon_sr {
namespace {

const wchar_t *const kWhy =
	L"this game is a 32-bit program, so the upscalers run in AeonSRHost.exe beside "
	L"the add-on. If nothing is upscaling, that helper did not start; AeonSR.log "
	L"says why.";

bool refuse(UpscalerBackend &b)
{
	b.status = UpscalerStatus::MissingRuntime;
	b.last_error = kWhy;
	return false;
}

}

const char *DlssD3D11Backend::name() const { return "DLSS"; }
reshade::api::device_api DlssD3D11Backend::api() const { return reshade::api::device_api::d3d11; }
bool DlssD3D11Backend::runtime_present() { return false; }
void DlssD3D11Backend::on_destroy_swapchain() {}
void DlssD3D11Backend::shutdown() {}
bool DlssD3D11Backend::run(reshade::api::effect_runtime *, const FrameInputs &, const UpscalerParams &)
{
	return refuse(*this);
}

const char *DlssD3D12Backend::name() const { return "DLSS"; }
reshade::api::device_api DlssD3D12Backend::api() const { return reshade::api::device_api::d3d12; }
bool DlssD3D12Backend::runtime_present() { return false; }
void DlssD3D12Backend::on_destroy_swapchain() {}
void DlssD3D12Backend::shutdown() {}
bool DlssD3D12Backend::run(reshade::api::effect_runtime *, const FrameInputs &, const UpscalerParams &)
{
	return refuse(*this);
}

const char *Fsr31D3D11Backend::name() const { return "FSR 3.1"; }
reshade::api::device_api Fsr31D3D11Backend::api() const { return reshade::api::device_api::d3d11; }
bool Fsr31D3D11Backend::runtime_present() { return false; }
void Fsr31D3D11Backend::on_destroy_swapchain() {}
void Fsr31D3D11Backend::shutdown() {}
bool Fsr31D3D11Backend::run(reshade::api::effect_runtime *, const FrameInputs &, const UpscalerParams &)
{
	return refuse(*this);
}

const char *Fsr4D3D12Backend::name() const { return "FSR"; }
reshade::api::device_api Fsr4D3D12Backend::api() const { return reshade::api::device_api::d3d12; }
bool Fsr4D3D12Backend::runtime_present() { return false; }
void Fsr4D3D12Backend::on_destroy_swapchain() {}
void Fsr4D3D12Backend::shutdown() {}
bool Fsr4D3D12Backend::run(reshade::api::effect_runtime *, const FrameInputs &, const UpscalerParams &)
{
	return refuse(*this);
}

const char *XessD3D12Backend::name() const { return "XeSS"; }
reshade::api::device_api XessD3D12Backend::api() const { return reshade::api::device_api::d3d12; }
bool XessD3D12Backend::runtime_present() { return false; }
void XessD3D12Backend::on_destroy_swapchain() {}
void XessD3D12Backend::shutdown() {}
bool XessD3D12Backend::run(reshade::api::effect_runtime *, const FrameInputs &, const UpscalerParams &)
{
	return refuse(*this);
}

const char *VsrD3D11Backend::name() const { return "RTX VSR"; }
reshade::api::device_api VsrD3D11Backend::api() const { return reshade::api::device_api::d3d11; }
bool VsrD3D11Backend::runtime_present() { return false; }
void VsrD3D11Backend::on_destroy_swapchain() {}
void VsrD3D11Backend::shutdown() {}
bool VsrD3D11Backend::run(reshade::api::effect_runtime *, const FrameInputs &, const UpscalerParams &)
{
	status = UpscalerStatus::UnsupportedApi;
	last_error = L"RTX VSR is not offered in a 32-bit game: it has never been run in one, and an "
		L"upscaler that quietly does nothing is worse than one that says so.";
	return false;
}

void NgxRuntime::shutdown_device() {}
void NgxRuntimeD3D12::shutdown_device() {}

bool NeuralRenderCommon::ensure_dll_present()
{
	set_status(UpscalerStatus::MissingRuntime, kWhy);
	return false;
}

bool NeuralRenderCommon::probe_dll_present()
{
	set_status(UpscalerStatus::MissingRuntime, kWhy);
	return false;
}

bool NeuralRenderD3D12::init_device(ID3D12Device *, uint32_t, uint32_t)
{
	set_status(UpscalerStatus::MissingRuntime, kWhy);
	return false;
}
void NeuralRenderD3D12::shutdown_device() {}
void NeuralRenderD3D12::set_command_queue(ID3D12CommandQueue *) {}
void NeuralRenderD3D12::destroy_feature() {}
void NeuralRenderD3D12::request_capture(std::wstring, uint32_t) {}
bool NeuralRenderD3D12::run(ID3D12GraphicsCommandList *, ID3D12Resource *,
	D3D12_RESOURCE_STATES, ID3D12Resource *, ID3D12Resource *, const NeuralRenderParams &,
	float, float)
{
	return false;
}

}

#endif
