#pragma once

#include "aeon_sr/core/gpu_vendor.hpp"
#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/upscalers/upscaler_status.hpp"

#include <cstdint>
#include <string>

namespace reshade::api {
struct effect_runtime;
struct command_list;
enum class device_api;
}

namespace aeon_sr {

enum class BackendChoice : unsigned int {
	Dlss = 0,
	Fsr = 1,
	Xess = 2,

	None = 3,

	Vsr = 4,
};
inline constexpr unsigned int kBackendChoiceCount = 5u;

const char *status_label(UpscalerStatus status) noexcept;
bool upscaler_waiting(UpscalerStatus status) noexcept;

std::string upscaler_display_name(const char *backend_name, bool is_fsr,
	const std::string &provider_version);

struct BackendAvailability {
	bool present = false;
	bool crashed = false;
	UpscalerStatus status = UpscalerStatus::Idle;
};

bool backend_usable(const BackendAvailability &a) noexcept;

inline UpscalerStatus status_after_unready_init(UpscalerStatus current) noexcept
{
	if (current == UpscalerStatus::Loading || current == UpscalerStatus::MissingRuntime)
		return current;
	return UpscalerStatus::InitFailed;
}

BackendChoice preferred_backend(GpuVendor vendor) noexcept;

BackendChoice backend_choice(unsigned int stored, GpuVendor vendor) noexcept;

int pick_backend(BackendChoice choice) noexcept;

struct UpscalerParams {
	uint32_t quality_mode = 0;
	uint32_t render_preset = 11;
	float sharpness = 0.25f;
	float frame_time_ms = 0.0f;
	bool reset = false;

	float render_scale = 0.0f;

	uint32_t render_width = 0, render_height = 0;

	bool depth_inverted = false;

	float jitter_x = 0.0f, jitter_y = 0.0f;
	bool jitter_in_frame = false;

	uint32_t vsr_input_format = 0;
	uint32_t color_space = 0;

	uint64_t bias_mask = 0;
};

struct FrameInputs;

struct UpscalerBackend {
	virtual ~UpscalerBackend() = default;
	virtual const char *name() const = 0;
	virtual reshade::api::device_api api() const = 0;
	virtual bool runtime_present() = 0;
	virtual bool run(reshade::api::effect_runtime *runtime,
		const FrameInputs &inputs,
		const UpscalerParams &params) = 0;
	virtual void on_destroy_swapchain() = 0;
	virtual void shutdown() = 0;

	UpscalerStatus status = UpscalerStatus::Idle;
	std::wstring last_error;
	bool crashed = false;
};

}
