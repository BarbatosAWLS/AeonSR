#pragma once

#include "aeon_sr/jitter/scene_jitter.hpp"

#include <cstdint>

namespace aeon_sr {

struct VkViewportFields {
	float x = 0.0f, y = 0.0f;
	float width = 0.0f, height = 0.0f;
	float min_depth = 0.0f, max_depth = 1.0f;
};

JitterViewport jitter_viewport_from_vk(const VkViewportFields &v) noexcept;
VkViewportFields vk_from_jitter_viewport(const JitterViewport &v, bool flipped = true) noexcept;

inline constexpr uint32_t kVulkanViewportSlots = 64u;

class VulkanViewportHook {
public:
	bool install(void *device, void *get_device_proc) noexcept;
	void forget() noexcept;

	bool installed() const noexcept { return installed_; }
	const char *note() const noexcept { return note_; }
	bool saw_viewports() const noexcept { return saw_; }

	uint32_t viewports_of(void *cmd, JitterViewport *out, uint32_t max) const noexcept;
	uint64_t stamp_of(void *cmd) const noexcept;
	bool set(void *cmd, uint32_t first, uint32_t count, const JitterViewport *vp) const noexcept;

	void note_viewports(void *cmd, uint32_t first, uint32_t count, const JitterViewport *vp,
		const bool *flipped = nullptr) noexcept;
	void forget_list(void *cmd) noexcept;

private:
	struct Slot {
		void *cmd = nullptr;
		uint32_t count = 0;
		uint64_t stamp = 0;
		JitterViewport vp[kJitterViewportSlots]{};
		bool flipped[kJitterViewportSlots]{};
	};

	Slot *find(void *cmd) noexcept;
	const Slot *find(void *cmd) const noexcept;

	bool installed_ = false;
	bool saw_ = false;
	const char *note_ = "";
	void *device_ = nullptr;
	void *original_ = nullptr;
	uint64_t stamp_ = 0;
	Slot slots_[kVulkanViewportSlots]{};
};

VulkanViewportHook &vulkan_viewport_hook() noexcept;

}
