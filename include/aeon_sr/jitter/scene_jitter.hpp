#pragma once

#include "aeon_sr/jitter/shader_offset.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace aeon_sr {

struct JitterViewport {
	float x = 0.0f, y = 0.0f;
	float width = 0.0f, height = 0.0f;
	float min_depth = 0.0f, max_depth = 1.0f;
};

inline constexpr uint32_t kJitterViewportSlots = 16u;
inline constexpr uint32_t kJitterMovedHistory = 4u;
inline constexpr uint32_t kJitterCountedArms = 4u;
inline constexpr uint32_t kJitterKnownOffsets = 128u;
inline constexpr uint32_t kJitterHeldLists = 16u;
inline constexpr uint32_t kJitterHoldPresents = 600u;
inline constexpr uint32_t kJitterSceneTargets = 16u;
inline constexpr uint32_t kJitterOtherSizes = 8u;
inline constexpr uint32_t kJitterBackBuffers = 8u;
inline constexpr uint32_t kJitterPresentedTargets = 4u;

enum class JitterReach : uint8_t {
	Scene = 0,
	SceneTargets = 1,
};

struct JitterArm {
	uint64_t serial = 0;
	uint32_t index = 0;
	JitterReach reach = JitterReach::Scene;
	float input_x = 0.0f, input_y = 0.0f;
	float x = 0.0f, y = 0.0f;
	float u = 0.0f, v = 0.0f;
	float target_x = 0.0f, target_y = 0.0f;
	uint64_t scene_depth = 0;
	uint32_t scene_w = 0, scene_h = 0;
	uint32_t display_w = 0, display_h = 0;
	bool size_targets = false;
	bool scene_targets = true;
	bool move_window = false;
	bool tested_quads = false;
	bool scene_quads = false;
	bool depth_regrid = false;
	bool layers = false;
	bool depth_proven = false;
	uint64_t back_buffer = 0;
	uint64_t back_buffers[kJitterBackBuffers]{};
	uint32_t back_buffer_count = 0;
};

JitterArm make_jitter_arm(uint32_t index, float jx, float jy, uint32_t dw, uint32_t dh,
	uint32_t rw, uint32_t rh, uint32_t tw, uint32_t th, uint64_t scene_depth, uint64_t back_buffer,
	JitterReach reach = JitterReach::Scene) noexcept;

JitterArm disarmed(const JitterArm &arm) noexcept;
void jitter_arm_add_back_buffer(JitterArm &arm, uint64_t back_buffer) noexcept;
bool jitter_arm_into_back_buffer(const JitterArm &arm, uint64_t target) noexcept;

enum class JitterProofWhy : uint32_t {
	Perspective,
	Flat,
	NotAsked,
	NoTerms,
	Replaced,
	Deferred,
	NotRead,
	Count
};
inline constexpr uint32_t kJitterProofWhyCount = static_cast<uint32_t>(JitterProofWhy::Count);

struct JitterDrawCounts {
	uint32_t plain = 0;
	uint64_t serial[kJitterCountedArms]{};
	uint32_t moved[kJitterCountedArms]{};
	uint32_t overflow = 0;
	uint32_t replayed = 0;
	uint32_t aliased = 0;
	uint32_t plain_aliased = 0;
	uint32_t by_target = 0;
	uint32_t depth_draws = 0;
	uint32_t depth_tested = 0;
	uint32_t outside_draws = 0;
	uint32_t interface_kept = 0;
	uint32_t interface_kept_outside = 0;
	uint32_t proof_why[kJitterProofWhyCount]{};
	uint32_t all_draws = 0;
	uint32_t quads_moved = 0;
	uint32_t quads_kept = 0;
	uint32_t other_w[kJitterOtherSizes]{}, other_h[kJitterOtherSizes]{};
	uint32_t other_draws[kJitterOtherSizes]{};
	uint32_t other_overflow = 0;
	uint32_t other_depth_w[kJitterOtherSizes]{}, other_depth_h[kJitterOtherSizes]{};
	uint32_t other_depth_draws[kJitterOtherSizes]{};
	uint32_t other_depth_overflow = 0;
};

void count_moved_draws(JitterDrawCounts &c, uint64_t serial, uint32_t n = 1u) noexcept;
void count_scene_draw(JitterDrawCounts &c, uint64_t bound_serial, bool aliased = false, uint32_t n = 1u) noexcept;
void count_other_draw(JitterDrawCounts &c, uint32_t w, uint32_t h, uint32_t n = 1u) noexcept;
void count_other_depth_draw(JitterDrawCounts &c, uint32_t w, uint32_t h, uint32_t n = 1u) noexcept;
void add_draw_counts(JitterDrawCounts &into, const JitterDrawCounts &from) noexcept;
JitterDrawCounts replayed_draw_counts(const JitterDrawCounts &c) noexcept;
bool any_draw_counts(const JitterDrawCounts &c) noexcept;
bool any_moved_draws(const JitterDrawCounts &c) noexcept;

struct JitterOffsets {
	float x[kJitterKnownOffsets];
	float y[kJitterKnownOffsets];
	uint32_t count = 0;
};

struct JitterMovedSet {
	uint64_t serial = 0;
	uint32_t count = 0;
	JitterViewport moved[kJitterViewportSlots]{};
	JitterViewport app[kJitterViewportSlots]{};
};

struct JitterListState {
	JitterViewport app[kJitterViewportSlots]{};
	uint32_t app_count = 0;
	uint64_t bound_serial = 0;
	JitterViewport bound[kJitterViewportSlots]{};
	uint32_t bound_count = 0;
	uint64_t depth = 0;
	uint32_t depth_w = 0, depth_h = 0;
	bool depth_aliased = false;
	uint64_t target = 0;
	uint32_t target_w = 0, target_h = 0;
	bool target_scene = false;
	bool target_window = false;
	bool depth_test = true;
	bool depth_pipe_known = false;
	int8_t depth_pipe_enable = -1, depth_pipe_func = -1;
	int8_t depth_dyn_enable = -1, depth_dyn_func = -1;
	JitterVertexClass vertex_class = JitterVertexClass::Unknown;
	bool perspective = false;
	int8_t projection = -1;
	bool scene_perspective = false;
	uint64_t depth_cleared_serial = 0;
	bool scene_tested = false;
	bool sky_first = false;
	bool layer = false;
	JitterMovedSet moved[kJitterMovedHistory]{};
	uint32_t moved_next = 0;
};

struct JitterRebind {
	uint32_t count = 0;
	JitterViewport vp[kJitterViewportSlots]{};
	uint32_t expect_count = 0;
	JitterViewport expect[kJitterViewportSlots]{};
};

JitterViewport offset_viewport(const JitterViewport &vp, const JitterArm &arm) noexcept;

bool jitter_viewports_fractional(uint32_t count, const JitterViewport *viewports) noexcept;

void jitter_on_viewports(JitterListState &s, const JitterArm &arm, uint32_t count,
	const JitterViewport *viewports, const JitterOffsets *known, JitterRebind *out) noexcept;
void jitter_on_depth(JitterListState &s, const JitterArm &arm, uint64_t depth, JitterRebind *out,
	uint32_t w = 0u, uint32_t h = 0u) noexcept;
void jitter_on_target(JitterListState &s, uint64_t target, uint32_t w = 0u, uint32_t h = 0u,
	bool scene_target = false, bool window = false) noexcept;
void jitter_on_depth_test(JitterListState &s, bool enabled) noexcept;
bool jitter_on_draw(JitterListState &s, const JitterArm &arm, bool fullscreen, JitterRebind *out) noexcept;
bool jitter_scene_by_size(const JitterArm &arm) noexcept;
bool jitter_scene_depth_bound(const JitterListState &s, const JitterArm &arm) noexcept;
void scene_rules(unsigned int rule, bool move_all, bool *size_targets, bool *move_window) noexcept;
void jitter_draw_size(const JitterListState &s, uint32_t *w, uint32_t *h) noexcept;
void jitter_on_cleared(JitterListState &s) noexcept;
void jitter_on_reset(JitterListState &s) noexcept;
bool jitter_rebind_expected(const JitterRebind &r, uint32_t count, const JitterViewport *bound) noexcept;

bool jitter_is_fullscreen_draw(uint32_t vertices, uint32_t instances) noexcept;

void jitter_on_depth_pipeline(JitterListState &s, bool known, int enable, int compares) noexcept;
void jitter_on_depth_dynamic(JitterListState &s, int enable, int compares) noexcept;
bool jitter_depth_compares(const JitterListState &s, bool *known) noexcept;

bool jitter_quad_is_scene(const JitterListState &s, const JitterArm &arm) noexcept;

bool jitter_window_quad_is_scene(const JitterListState &s, const JitterArm &arm) noexcept;

struct JitterFrame {
	bool valid = false;
	bool ambiguous = false;
	uint32_t index = 0;
	float x = 0.0f, y = 0.0f;
	float u = 0.0f, v = 0.0f;
	float input_x = 0.0f, input_y = 0.0f;
	uint32_t moved = 0, plain = 0;
	uint32_t replayed = 0;
	uint32_t elsewhere = 0;
	uint32_t aliased = 0;
	uint32_t plain_aliased = 0;
	uint32_t by_target = 0;
	uint32_t depth_draws = 0;
	uint32_t depth_tested = 0;
	uint32_t outside_draws = 0;
	uint32_t interface_kept = 0, interface_kept_outside = 0;
	uint32_t proof_why[kJitterProofWhyCount]{};
	uint32_t all_draws = 0;
	uint32_t quads_moved = 0, quads_kept = 0;
	uint32_t other_w = 0, other_h = 0, other_draws = 0;
	uint32_t other_depth_w = 0, other_depth_h = 0, other_depth_draws = 0;
	bool empty = false;

	uint64_t serial = 0;
	uint64_t serials[kJitterCountedArms]{};
	uint32_t serial_moved[kJitterCountedArms]{};
	uint32_t unknown = 0;
	uint32_t overflow = 0;
	uint64_t armed_serial = 0;
	bool armed_pending = false;
};

bool jitter_frame_like(uint32_t w, uint32_t h, uint32_t dw, uint32_t dh) noexcept;
bool jitter_move_all(bool was_on, const JitterFrame &frame, bool depth_proven = false) noexcept;
bool jitter_interface_alone(const JitterFrame &frame, bool depth_proven) noexcept;
struct JitterDepthProof {
	uint32_t proven_w = 0, proven_h = 0;
	bool armed_proven = false;
	bool arm(bool by_depth, uint32_t w, uint32_t h, const JitterFrame &drawn) noexcept;
	void reset() noexcept { *this = JitterDepthProof{}; }
};
struct JitterSceneSize {
	uint32_t learned_w = 0, learned_h = 0, learned_dw = 0, learned_dh = 0;
	uint32_t depth_w = 0, depth_h = 0, depth_dw = 0, depth_dh = 0;
	uint32_t comp_w = 0, comp_h = 0, comp_dw = 0, comp_dh = 0;
	uint32_t w = 0, h = 0;
	bool by_depth = false;
	bool by_composite = false;
	void find(uint64_t scene_depth, uint32_t sel_w, uint32_t sel_h, uint32_t dw, uint32_t dh,
		const JitterFrame &drawn, uint32_t cw = 0, uint32_t ch = 0) noexcept;
	void reset() noexcept { *this = JitterSceneSize{}; }
};
bool jitter_draw_into_window(const JitterListState &s, const JitterArm &arm) noexcept;
inline constexpr uint32_t kJitterLearnDraws = 16u;
bool jitter_learn_scene_size(const JitterFrame &frame, uint32_t display_w, uint32_t display_h,
	uint32_t *w, uint32_t *h) noexcept;

struct JitterPromotions {
	uint32_t at_boundary = 0;
	uint32_t between_frames = 0;
	uint32_t forced = 0;
};

class SceneJitter {
public:
	void arm(JitterArm arm) noexcept;
	void disarm() noexcept;
	void stop() noexcept;
	void stop_now() noexcept;
	JitterArm current() const noexcept;
	uint64_t last_serial() const noexcept;

	void set_present_lags(bool lags) noexcept;
	bool present_lags() const noexcept { return lags_.load(); }
	void watch_swapchain(const uint64_t *images, uint32_t count) noexcept;
	void forget_sizes() noexcept { size_epoch_.fetch_add(1, std::memory_order_acq_rel); }
	uint64_t size_epoch() const noexcept { return size_epoch_.load(std::memory_order_acquire); }
	void note_scene_target(uint64_t target, uint32_t w, uint32_t h) noexcept;
	bool scene_target(uint64_t target, uint32_t w, uint32_t h) const noexcept;
	void note_presented(uint64_t target) noexcept;
	bool presented_target(uint64_t target) const noexcept;
	static constexpr uint32_t kPresentedHistory = 8u;
	void note_pass_target(uint64_t target) noexcept { last_pass_target_.store(target, std::memory_order_relaxed); }
	void note_presentation() noexcept;
	void note_presentation_from(uint64_t from, bool composite = false) noexcept;
	void forget_composite(uint64_t target) noexcept;
	bool composite_window(uint64_t target) const noexcept;
	void note_composite_size(uint64_t from, uint32_t w, uint32_t h) noexcept;
	uint64_t composite_size() const noexcept { return composite_size_.load(std::memory_order_acquire); }
	void forget_target(uint64_t resource) noexcept;
	void note_scene_tested(uint64_t serial, bool window = false) noexcept
	{
		if (serial == 0)
			return;
		if (scene_tested_.load(std::memory_order_relaxed) != serial)
			scene_tested_.store(serial, std::memory_order_relaxed);
		const uint64_t frame = boundaries_.load(std::memory_order_relaxed);
		if (scene_tested_boundary_.load(std::memory_order_relaxed) != frame)
			scene_tested_boundary_.store(frame, std::memory_order_relaxed);
		if (window && window_tested_.load(std::memory_order_relaxed) != serial)
			window_tested_.store(serial, std::memory_order_relaxed);
	}
	bool scene_tested(uint64_t serial) const noexcept
	{
		return serial != 0 && scene_tested_.load(std::memory_order_relaxed) == serial;
	}
	bool scene_tested_in_frame(uint64_t serial, bool on_list) const noexcept;
	bool frames_delimited() const noexcept { return !promoted_forced_.load(std::memory_order_relaxed); }
	static constexpr uint64_t kSkyArms = 2u;
	bool window_tested_lately(uint64_t serial) const noexcept
	{
		const uint64_t w = window_tested_.load(std::memory_order_relaxed);
		return serial != 0 && w != 0 && w <= serial && serial - w <= kSkyArms;
	}
	bool sky_first(const JitterArm &arm) const noexcept
	{
		return !arm.depth_proven || window_tested_lately(arm.serial);
	}
	bool perspective_seen() const noexcept { return scene_perspective_.load(std::memory_order_relaxed) != 0; }
	void forget_scene_seen() noexcept
	{
		scene_tested_.store(0, std::memory_order_relaxed);
		scene_perspective_.store(0, std::memory_order_relaxed);
	}
	void note_scene_perspective(uint64_t serial) noexcept
	{
		if (serial != 0 && scene_perspective_.load(std::memory_order_relaxed) != serial)
			scene_perspective_.store(serial, std::memory_order_relaxed);
	}
	bool scene_perspective(uint64_t serial) const noexcept
	{
		return serial != 0 && scene_perspective_.load(std::memory_order_relaxed) == serial;
	}
	void forget_scene_targets() noexcept;
	bool swapchain_image(uint64_t resource) const noexcept;
	bool swapchain_watched() const noexcept;
	void back_buffers_into(JitterArm &arm) const noexcept;
	void note_render_pass(uint64_t target, bool depth) noexcept;
	void note_chain_written(uint64_t target) noexcept;
	bool in_frame() const noexcept { return in_frame_.load(); }
	void promote_now(bool forced = false) noexcept;
	JitterPromotions take_promotions() noexcept;
	bool pending() const noexcept;
	uint64_t boundaries() const noexcept { return boundaries_.load(); }
	JitterArm current_for(uintptr_t list) const noexcept;
	uint64_t version() const noexcept { return version_.load(); }
	bool scene_known() const noexcept { return scene_known_.load(); }
	bool engaged() const noexcept;
	uint64_t frame() const noexcept { return frame_.load(); }
	void known_offsets(JitterOffsets *out) const noexcept;
	void note_bound(bool before, bool after) noexcept;
	void watch_device(uintptr_t device) noexcept;
	void note_viewports(uintptr_t device) noexcept;
	bool saw_viewports() const noexcept { return saw_viewports_.load(); }
	void hold(uintptr_t list) noexcept;
	void release(uintptr_t list) noexcept;
	bool held(uintptr_t list) const noexcept;
	uint32_t live_holds() const noexcept;
	void hand_on(const JitterDrawCounts &counts) noexcept;
	void discard_counts() noexcept;
	JitterFrame take_frame() noexcept;

private:
	static constexpr uint32_t kHistory = 8u;

	bool held_locked(uintptr_t list) const noexcept;
	void promote_locked() noexcept;

	mutable std::shared_mutex lock_;
	JitterArm arm_{};
	JitterArm pending_{};
	bool pending_set_ = false;
	std::atomic<bool> lags_{ false };
	std::atomic<bool> in_frame_{ false };
	std::atomic<uint64_t> boundaries_{ 0 };
	std::atomic<uint64_t> frame_image_{ 0 };
	JitterPromotions promotions_{};
	uint64_t swapchain_[kJitterBackBuffers]{};
	uint32_t swapchain_count_ = 0;
	std::atomic<uint64_t> version_{ 0 };
	std::atomic<uint64_t> size_epoch_{ 0 };
	std::atomic<uint64_t> scene_targets_[kJitterSceneTargets]{};
	std::atomic<uint64_t> scene_target_sizes_[kJitterSceneTargets]{};
	std::atomic<uint32_t> scene_target_next_{ 0 };
	std::atomic<uint64_t> presented_[kJitterPresentedTargets]{};
	std::atomic<uint64_t> composite_[kJitterPresentedTargets]{};
	std::atomic<uint32_t> composite_next_{ 0 };
	std::atomic<uint64_t> composite_size_{ 0 };
	uint64_t composite_seen_[kPresentedHistory]{};
	uint32_t composite_seen_next_ = 0;
	std::atomic<uint64_t> last_pass_target_{ 0 };
	std::mutex presentation_lock_;
	uint64_t presentations_[kPresentedHistory]{};
	uint32_t presentation_next_ = 0;
	std::atomic<uint32_t> presented_next_{ 0 };
	std::atomic<uint64_t> scene_tested_{ 0 };
	std::atomic<uint64_t> scene_tested_boundary_{ 0 };
	std::atomic<bool> promoted_forced_{ false };
	std::atomic<uint64_t> window_tested_{ 0 };
	std::atomic<uint64_t> scene_perspective_{ 0 };
	std::atomic<uint64_t> frame_{ 0 };
	std::atomic<bool> scene_known_{ false };
	std::atomic<int> lists_bound_{ 0 };
	std::atomic<uintptr_t> watched_{ 0 };
	std::atomic<bool> saw_viewports_{ false };
	uint64_t next_serial_ = 0;
	JitterArm history_[kHistory]{};
	uint32_t history_next_ = 0;
	JitterOffsets known_{};
	uint32_t known_next_ = 0;
	uintptr_t held_[kJitterHeldLists]{};
	uint64_t held_at_[kJitterHeldLists]{};
	uint32_t held_next_ = 0;
	std::atomic<uint32_t> held_slots_{ 0 };

	std::mutex counts_mutex_;
	JitterDrawCounts counts_{};
};

class JitterGate {
public:
	static constexpr uint32_t kRun = 4u;
	static constexpr uint32_t kFirstWait = 300u;
	static constexpr uint32_t kLongestWait = 4800u;
	static constexpr uint32_t kElsewhereShare = 10u;
	static constexpr uint32_t kSteady = 600u;

	static bool misplaced(const JitterFrame &frame) noexcept;
	void note(const JitterFrame &frame) noexcept;
	void note_failed() noexcept;
	bool open() const noexcept { return closed_for_ == 0u; }

private:
	void close() noexcept;

	uint32_t run_ = 0;
	uint32_t clean_ = 0;
	uint32_t closed_for_ = 0;
	uint32_t next_wait_ = kFirstWait;
};

class JitterSchedule {
public:
	enum class Present { Ignore, Arm, Stop };
	static constexpr uint32_t kReleaseAfter = 120u;

	bool drives(uintptr_t swapchain) const noexcept { return owner_ == 0 || owner_ == swapchain; }
	void begin() noexcept { pending_set_ = false; }
	void earn(uintptr_t swapchain, const JitterArm &arm) noexcept;
	Present present(uintptr_t swapchain, JitterArm *arm) noexcept;
	void forget() noexcept;

private:
	uintptr_t owner_ = 0;
	JitterArm pending_{};
	bool pending_set_ = false;
	uint32_t stops_ = 0;
	uint32_t others_ = 0;
};

class JitterCounts {
public:
	virtual ~JitterCounts() = default;
	virtual JitterFrame take() noexcept = 0;
	virtual void discard() noexcept = 0;
};

class JitterDriver {
public:
	static constexpr uint32_t kAloneAfter = 120u;
	static constexpr uint32_t kRawMovedInWindow = 3u;
	static constexpr uint32_t kRawMovedWindow = 8u;
	static constexpr uint32_t kRawMovedAlone = 2u;
	static constexpr uint32_t kRawMovedAloneWindow = 32u;
	static constexpr uint32_t kPromoteStaleAfter = 4u;

	explicit JitterDriver(SceneJitter &jitter) noexcept : jitter_(jitter) {}

	void begin_frame(uintptr_t swapchain, JitterCounts &counts) noexcept;
	void note_upscaled() noexcept { upscaled_ = true; }
	void note_reached_game() noexcept { reached_ = true; }
	void note_waiting() noexcept { waiting_ = true; }
	void drop_upscaled() noexcept { upscaled_ = false; }
	const char *pause_reason() const noexcept { return pause_reason_; }
	const char *refusal() const noexcept;
	bool earn(const JitterArm &arm) noexcept;
	void present(uintptr_t swapchain, JitterCounts &counts) noexcept;
	void forget() noexcept;

	bool drives() const noexcept { return drives_; }
	uint64_t armed_serial() const noexcept { return armed_serial_; }
	bool upscaled() const noexcept { return upscaled_; }
	const JitterFrame &drawn() const noexcept { return drawn_; }
	bool gate_open() const noexcept { return gate_.open(); }
	const char *note() const noexcept { return note_; }
	void depth_uv(float *u, float *v) const noexcept;
	void drawn_uv(float *u, float *v) const noexcept;

private:
	SceneJitter &jitter_;
	JitterSchedule schedule_;
	JitterGate gate_;
	JitterFrame drawn_{};
	uintptr_t swapchain_ = 0;
	uintptr_t last_swapchain_ = 0;
	uint32_t alone_for_ = kAloneAfter;
	bool drives_ = false;
	bool upscaled_ = false;
	bool reached_ = false;
	bool waiting_ = false;
	bool armed_ = false;
	bool frame_open_ = false;
	uint64_t last_boundaries_ = ~0ull;
	uint32_t stale_presents_ = 0;
	uint32_t raw_history_ = 0;
	const char *note_ = "";
	const char *pause_reason_ = "";
	uint64_t armed_serial_ = 0;
};

struct JitterFault {
	bool told_lag = false;
	int32_t told_drop_at = -1;
	uint32_t draw_drop_every = 0;
	uint32_t upscale_drop_every = 0;
	float odd_told_scale = 1.0f;
	float draw_scale = 1.0f;
	float told_sign_x = 1.0f;
	bool game_jitter_still = false;
	bool game_jitter_forget = false;
	bool game_jitter_assume = false;
	bool withheld_no_restore = false;
	bool withheld_no_fill = false;
};

bool parse_jitter_fault(const std::string &text, JitterFault *out);
std::string jitter_fault_text(const JitterFault &f);
bool jitter_fault_any(const JitterFault &f) noexcept;
bool jitter_fault_hit(uint32_t every, uint32_t frame) noexcept;
bool apply_told_fault(const JitterFault &f, uint32_t frame, bool odd, bool have_prev, float prev_x,
	float prev_y, float *told_x, float *told_y) noexcept;
bool apply_draw_fault(const JitterFault &f, uint32_t frame, JitterArm *arm) noexcept;

}
