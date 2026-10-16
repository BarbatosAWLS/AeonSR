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

struct LayerNotes {
	static constexpr uint32_t kNotes = 32u;
	enum Kind : uint8_t { kTested, kWindow, kTarget };
	uint64_t target[kNotes]{};
	uint8_t kind[kNotes]{};
	uint32_t count = 0;
	uint32_t segment = 0;
	bool overflow = false;
	bool segment_tested = false;
	uint64_t segment_last = 0;

	void tested() noexcept
	{
		if (segment_tested)
			return;
		segment_tested = true;
		add(kTested, 0);
	}
	void window() noexcept
	{
		if (count != 0 && kind[count - 1u] == kWindow)
			return;
		if (add(kWindow, 0)) {
			segment = count;
			segment_tested = false;
			segment_last = 0;
		}
	}
	void drawn(uint64_t t) noexcept
	{
		if (t == 0 || t == segment_last)
			return;
		for (uint32_t i = segment; i < count; ++i)
			if (kind[i] == kTarget && target[i] == t)
				return;
		segment_last = t;
		add(kTarget, t);
	}
	void append(const LayerNotes &o) noexcept
	{
		for (uint32_t i = 0; i < o.count; ++i) {
			if (o.kind[i] == kTested)
				tested();
			else if (o.kind[i] == kWindow)
				window();
			else
				drawn(o.target[i]);
		}
		overflow = overflow || o.overflow;
	}
	void clear() noexcept
	{
		count = segment = 0;
		overflow = false;
		segment_tested = false;
		segment_last = 0;
	}

private:
	bool add(Kind k, uint64_t t) noexcept
	{
		if (count == kNotes) {
			overflow = true;
			return false;
		}
		kind[count] = static_cast<uint8_t>(k);
		target[count] = t;
		++count;
		return true;
	}
};

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
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
		uint32_t layer = 0;
		char order[121] = {};
		uint32_t order_len = 0;
	};
	void count(bool window, bool scene, bool quad, bool depth_tested, uint32_t w, uint32_t h);
	Census take_census();

	void note_present() noexcept { layer_frame_.fetch_add(1u, std::memory_order_acq_rel); }
	uint64_t layer_frame() const noexcept { return layer_frame_.load(std::memory_order_acquire); }
	void note_scene_tested() noexcept;
	bool scene_tested() const noexcept;
	void note_window_written() noexcept;
	bool window_written() const noexcept;
	void note_before_window(uint64_t target) noexcept;
	void note_after_window(uint64_t target) noexcept;
	static constexpr uint64_t kLayerFrames = 8u;
	bool layer_target(uint64_t target) const noexcept;
	bool layer_played(uint64_t target) const noexcept;
	void play(const LayerNotes &notes) noexcept;
	void count_layer() noexcept { layer_.fetch_add(1u, std::memory_order_relaxed); }

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
	static constexpr uint32_t kMarks = 16u;
	alignas(64) std::atomic<uint64_t> layer_frame_{ 1 };
	std::atomic<uint64_t> tested_frame_{ 0 }, written_frame_{ 0 };
	alignas(64) std::atomic<uint64_t> mark_target_[kMarks]{};
	std::atomic<uint64_t> mark_before_[kMarks]{}, mark_after_[kMarks]{};
	std::atomic<uint32_t> mark_next_{ 0 };
	alignas(64) std::atomic<uint32_t> layer_{ 0 };
	uint32_t mark_slot(uint64_t target, bool make) noexcept;
	uint32_t find_mark(uint64_t target) const noexcept;
	reshade::api::resource_desc wanted_;
	bool want_ = false;
	uint64_t copied_ = 0;
	Tally tally_;
	const char *note_ = "";
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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

struct SplitCover {
	static constexpr uint32_t kWindow = 32u;
	static constexpr uint32_t kOver = 24u;
	static constexpr uint32_t kRefuse = 600u;
	uint32_t seen = 0;
	uint32_t over = 0;
	uint32_t refuse = 0;
	bool started = false;
	void feed(uint32_t shown, uint32_t pixels) noexcept
	{
		if (pixels == 0u)
			return;
		seen = (seen << 1) | 1u;
		over = (over << 1) | (2u * static_cast<uint64_t>(shown) > pixels ? 1u : 0u);
		uint32_t n = 0;
		for (uint32_t m = over; m != 0; m &= m - 1u)
			++n;
		if (refuse == 0u && n >= kOver) {
			refuse = kRefuse;
			started = true;
			seen = over = 0;
		}
	}
	bool present() noexcept
	{
		if (refuse == 0u)
			return true;
		--refuse;
		return false;
	}
};

inline bool split_gate_present(SplitGate &gate, bool drawn, bool took) noexcept
{
	return drawn && gate.step(took);
}

bool vulkan_rendering_hook_install(void *device, void *get_device_proc) noexcept;
bool vulkan_rendering_hook_installed() noexcept;
bool vulkan_copy_in_pass(void *cmd, uint64_t window, uint64_t dst, uint32_t w, uint32_t h, bool resolve,
	const char **why) noexcept;
void vulkan_rendering_ended(void *cmd) noexcept;
bool vulkan_restarting_pass() noexcept;

}
