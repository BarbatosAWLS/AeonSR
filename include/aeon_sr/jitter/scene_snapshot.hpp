#pragma once

#include <reshade.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace aeon_sr {

struct SceneSnapshotTaken {
	reshade::api::resource resource = { 0 };
	reshade::api::resource_usage state = reshade::api::resource_usage::copy_dest;
};

class SceneSnapshot {
public:
	static constexpr uint32_t kRing = 4u;
	static constexpr uint32_t kQueued = kRing - 1u;
	static constexpr uint32_t kCopiesPerFrame = 12u;

	void before_interface(reshade::api::command_list *cmd, reshade::api::resource window, uint64_t serial);
	void spoil(uint64_t serial, reshade::api::resource target, bool fullscreen);
	void note_scene(uint64_t serial, reshade::api::resource target);
	bool scene_drawn_into(uint64_t serial, reshade::api::resource target);
	uint32_t window_draw_index(uint64_t serial, reshade::api::resource window);
	void note_scene_mesh(uint32_t index);
	uint32_t last_mesh_before() const noexcept { return mesh_prev_.load(std::memory_order_relaxed); }
	void end_frame_in_stream(reshade::api::resource presented_to);
	void end_frame_at_present();
	bool take(uint64_t serial, SceneSnapshotTaken *out);
	void prepare(reshade::api::device *device);
	void release(reshade::api::device *device);

	struct Census {
		uint32_t window_scene = 0, window_quads = 0, window_other = 0;
		uint32_t elsewhere = 0, elsewhere_quads = 0;
		uint32_t elsewhere_w = 0, elsewhere_h = 0;
		char order[121] = {};
		uint32_t order_len = 0;
	};
	void count(bool window, bool scene, bool quad, bool depth_tested, uint32_t w, uint32_t h);
	Census take_census();

	uint64_t copied() const noexcept { return copied_; }
	const char *note() const noexcept { return note_; }
	struct Tally {
		uint32_t in_stream = 0, at_present = 0, taken = 0, none = 0, empty = 0, copies = 0;
	};
	Tally take_tally();

private:
	static constexpr uint32_t kNone = ~0u;
	struct Slot {
		reshade::api::resource res = { 0 };
		reshade::api::resource_desc desc;
		uint64_t window = 0;
		uint64_t serial = 0;
		bool valid = false;
		bool spoiled = false;
		uint32_t copies = 0;
	};
	static bool fits(const Slot &slot, const reshade::api::resource_desc &like) noexcept;
	bool copy(reshade::api::command_list *cmd, reshade::api::resource window, Slot &slot, bool resolve);
	bool queued(uint32_t slot) const noexcept;
	void end_frame_locked();

	std::mutex lock_;
	Slot ring_[kRing];
	uint32_t cur_ = kNone;
	uint32_t done_[kQueued]{};
	uint32_t done_count_ = 0;
	bool ended_in_stream_ = false;
	reshade::api::device *device_ = nullptr;
	std::atomic<uint32_t> window_scene_{ 0 }, window_quads_{ 0 }, window_other_{ 0 };
	std::atomic<uint32_t> elsewhere_{ 0 }, elsewhere_quads_{ 0 };
	std::atomic<uint32_t> elsewhere_w_{ 0 }, elsewhere_h_{ 0 }, order_len_{ 0 };
	char order_[121] = {};
	std::atomic<uint64_t> frame_{ 1 };
	std::atomic<uint64_t> scene_frame_{ 0 }, scene_image_{ 0 };
	std::atomic<uint64_t> good_frame_{ 0 }, copied_frame_{ 0 }, window_image_{ 0 };
	std::atomic<uint32_t> index_{ 0 }, mesh_cur_{ 0 }, mesh_prev_{ 0 };
	reshade::api::resource_desc wanted_;
	bool want_ = false;
	uint64_t copied_ = 0;
	Tally tally_;
	const char *note_ = "";
};

SceneSnapshot &scene_snapshot();

struct SplitGate {
	static constexpr uint32_t kMisses = 8u;
	static constexpr uint32_t kClean = 60u;
	uint64_t missed = 0;
	uint32_t hold = 0;
	bool step(bool have) noexcept
	{
		missed = (missed << 1) | (have ? 0u : 1u);
		uint32_t n = 0;
		for (uint64_t m = missed; m != 0; m &= m - 1u)
			++n;
		if (n >= kMisses)
			hold = kClean;
		else if (have && hold > 0u)
			--hold;
		return have && hold == 0u;
	}
};

bool vulkan_rendering_hook_install(void *device, void *get_device_proc) noexcept;
bool vulkan_rendering_hook_installed() noexcept;
bool vulkan_copy_in_pass(void *cmd, uint64_t window, uint64_t dst, uint32_t w, uint32_t h, bool resolve,
	const char **why) noexcept;
void vulkan_rendering_ended(void *cmd) noexcept;
bool vulkan_restarting_pass() noexcept;

}
