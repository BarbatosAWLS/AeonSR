#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"

#include <cstdint>
#include <string>

namespace aeon_sr {

class FramePresenter {
public:
	bool ensure(reshade::api::effect_runtime *runtime, std::wstring *why);
	void release() noexcept;
	void forget() noexcept;

	reshade::api::resource real_copy() const noexcept { return real_; }
	reshade::api::resource generated() const noexcept { return gen_; }
	bool ready() const noexcept { return real_.handle != 0 && gen_.handle != 0; }

	void copy_back_buffer(reshade::api::effect_runtime *runtime);

	bool present_generated(reshade::api::effect_runtime *runtime, std::wstring *why);

	const std::string &route() const noexcept { return route_; }
	uint32_t failures_in_a_row() const noexcept { return failures_; }
	void set_sync_interval(uint32_t s) noexcept { sync_interval_ = s; }

private:
	bool present_vulkan(reshade::api::effect_runtime *runtime, std::wstring *why);
	bool present_native(reshade::api::effect_runtime *runtime, std::wstring *why);
	bool resolve_vulkan(reshade::api::effect_runtime *runtime);
	bool resolve_gl(std::wstring *why);

	reshade::api::device *device_ = nullptr;
	reshade::api::resource real_ = { 0 };
	reshade::api::resource gen_ = { 0 };
	uint32_t w_ = 0, h_ = 0;
	reshade::api::format format_ = reshade::api::format::unknown;
	std::string route_;
	int64_t last_code_ = 0;
	uint32_t sync_interval_ = 0;
	uint32_t failures_ = 0;

	bool d3d9_break_scene_ = false;

	bool gl_resolved_ = false;
	void *gl_swap_ = nullptr;

	struct Vk {
		bool tried = false, ok = false;
		std::wstring why;
		uint64_t device = 0;
		uint64_t fence = 0;
		void *acquire = nullptr, *present = nullptr, *wait = nullptr, *reset = nullptr, *destroy_fence = nullptr;
	} vk_;
};

}
