#include "aeon_sr/upscalers/upscaler_backend.hpp"

namespace aeon_sr {

std::string upscaler_display_name(const char *backend_name, bool is_fsr,
	const std::string &provider_version)
{
	if (!is_fsr)
		return backend_name != nullptr ? std::string(backend_name) : std::string();
	return provider_version.empty() ? std::string("FSR") : "FSR " + provider_version;
}

const char *status_label(UpscalerStatus status) noexcept
{
	switch (status) {
	case UpscalerStatus::Idle: return "idle";
	case UpscalerStatus::MissingRuntime: return "missing runtime DLL";
	case UpscalerStatus::UnsupportedApi: return "unsupported on this API";
	case UpscalerStatus::UnsupportedGpu: return "not supported on this GPU";
	case UpscalerStatus::InitFailed: return "init failed";
	case UpscalerStatus::NeedMotionVectors: return "need motion vectors";
	case UpscalerStatus::NeedDepth: return "need a depth buffer";
	case UpscalerStatus::Ready: return "ready";
	case UpscalerStatus::EvaluateFailed: return "evaluate failed";
	case UpscalerStatus::Crashed: return "runtime crashed (disabled)";
	case UpscalerStatus::Loading: return "loading, the frame goes on without it";
	}
	return "unknown";
}

bool upscaler_waiting(UpscalerStatus status) noexcept
{
	return status == UpscalerStatus::Loading || status == UpscalerStatus::NeedDepth ||
		status == UpscalerStatus::NeedMotionVectors;
}

bool backend_usable(const BackendAvailability &a) noexcept
{
	if (!a.present || a.crashed)
		return false;
	if (a.status == UpscalerStatus::InitFailed || a.status == UpscalerStatus::Crashed ||
		a.status == UpscalerStatus::UnsupportedGpu)
		return false;
	return true;
}

BackendChoice preferred_backend(GpuVendor vendor) noexcept
{
	switch (vendor) {
	case GpuVendor::Amd: return BackendChoice::Fsr;
	case GpuVendor::Intel: return BackendChoice::Xess;
	case GpuVendor::Nvidia:
	case GpuVendor::Unknown: break;
	}
	return BackendChoice::Dlss;
}

BackendChoice backend_choice(unsigned int stored, GpuVendor vendor) noexcept
{
	return stored < kBackendChoiceCount
		? static_cast<BackendChoice>(stored) : preferred_backend(vendor);
}

int pick_backend(BackendChoice choice) noexcept
{
	switch (choice) {
	case BackendChoice::Dlss: return 0;
	case BackendChoice::Fsr: return 1;
	case BackendChoice::Xess: return 2;
	case BackendChoice::Vsr: return 3;
	case BackendChoice::None: break;
	}
	return -1;
}

}
