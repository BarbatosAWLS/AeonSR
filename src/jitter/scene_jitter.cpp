#include "aeon_sr/jitter/scene_jitter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace aeon_sr {
namespace {

bool same_viewports(const JitterViewport *a, const JitterViewport *b, uint32_t n) noexcept
{
	for (uint32_t i = 0; i < n; ++i) {
		if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].width != b[i].width ||
			a[i].height != b[i].height || a[i].min_depth != b[i].min_depth ||
			a[i].max_depth != b[i].max_depth)
			return false;
	}
	return true;
}

bool same_rectangles(const JitterViewport *a, const JitterViewport *b, uint32_t n) noexcept
{
	for (uint32_t i = 0; i < n; ++i) {
		if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].width != b[i].width || a[i].height != b[i].height)
			return false;
	}
	return true;
}

bool whole(float f) noexcept
{
	return std::fabs(f - std::nearbyint(f)) < 1e-3f;
}

bool scene_sized(const JitterArm &arm, uint32_t w, uint32_t h) noexcept
{
	return arm.scene_w != 0 && w == arm.scene_w && h == arm.scene_h;
}

bool scene_depth_bound(const JitterListState &s, const JitterArm &arm) noexcept
{
	return s.depth != 0 && (s.depth == arm.scene_depth || scene_sized(arm, s.depth_w, s.depth_h));
}

bool scene_bound(const JitterListState &s, const JitterArm &arm) noexcept
{
	const bool into_back_buffer = jitter_arm_into_back_buffer(arm, s.target);
	const bool scene_depth = scene_depth_bound(s, arm);
	if (arm.reach == JitterReach::SceneTargets) {
		const bool sized = s.target != 0 && scene_sized(arm, s.target_w, s.target_h);
		if (into_back_buffer || (s.target != 0 && s.target_window))
			return (scene_depth && (s.depth_test || !s.scene_tested)) || (arm.move_window && sized);
		return scene_depth || (arm.scene_targets && s.target_scene && sized) || (arm.size_targets && sized);
	}
	return scene_depth && s.depth_test && !into_back_buffer;
}

const JitterMovedSet &moved_back(const JitterListState &s, uint32_t k) noexcept
{
	return s.moved[(s.moved_next + kJitterMovedHistory - 1u - k) % kJitterMovedHistory];
}

const JitterMovedSet *find_moved(const JitterListState &s, uint32_t count, const JitterViewport *viewports) noexcept
{
	for (uint32_t k = 0; k < kJitterMovedHistory; ++k) {
		const JitterMovedSet &e = moved_back(s, k);
		if (e.serial != 0 && e.count == count && same_viewports(e.moved, viewports, count))
			return &e;
	}
	return nullptr;
}

bool recognise_moved(uint32_t count, const JitterViewport *viewports, const JitterOffsets &known,
	JitterViewport *app) noexcept
{
	for (uint32_t k = 0; k < known.count; ++k) {
		const float tx = known.x[k], ty = known.y[k];
		if (tx == 0.0f && ty == 0.0f)
			continue;
		uint32_t i = 0;
		while (i < count && whole(viewports[i].x - tx) && whole(viewports[i].y - ty))
			++i;
		if (i != count)
			continue;
		for (i = 0; i < count; ++i) {
			app[i] = viewports[i];
			app[i].x = std::nearbyint(viewports[i].x - tx);
			app[i].y = std::nearbyint(viewports[i].y - ty);
		}
		return true;
	}
	return false;
}

void remember_moved(JitterListState &s, uint64_t serial, const JitterViewport *moved) noexcept
{
	const uint32_t count = s.app_count;
	const JitterMovedSet &last = moved_back(s, 0u);
	if (last.serial == serial && last.count == count && same_viewports(last.app, s.app, count))
		return;
	JitterMovedSet &e = s.moved[s.moved_next % kJitterMovedHistory];
	++s.moved_next;
	e.serial = serial;
	e.count = count;
	for (uint32_t i = 0; i < count; ++i) {
		e.moved[i] = moved[i];
		e.app[i] = s.app[i];
	}
}

void set_bound(JitterListState &s, uint32_t count, const JitterViewport *viewports) noexcept
{
	s.bound_count = count;
	for (uint32_t i = 0; i < count; ++i)
		s.bound[i] = viewports[i];
}

void sync(JitterListState &s, const JitterArm &arm, bool moved, JitterRebind *out) noexcept
{
	const uint64_t want = (moved && arm.serial != 0 && s.app_count != 0) ? arm.serial : 0u;
	if (want == s.bound_serial)
		return;
	s.bound_serial = want;
	if (s.app_count == 0)
		return;
	out->expect_count = s.bound_count;
	for (uint32_t i = 0; i < s.bound_count; ++i)
		out->expect[i] = s.bound[i];
	out->count = s.app_count;
	for (uint32_t i = 0; i < s.app_count; ++i)
		out->vp[i] = want != 0 ? offset_viewport(s.app[i], arm) : s.app[i];
	set_bound(s, out->count, out->vp);
	if (want != 0)
		remember_moved(s, want, out->vp);
}

bool names_scene(const JitterArm &arm) noexcept
{
	return arm.scene_depth != 0 || (arm.scene_w != 0 && arm.scene_h != 0);
}

constexpr uint64_t kRecognisedSerial = ~0ull;

}

JitterArm make_jitter_arm(uint32_t index, float jx, float jy, uint32_t dw, uint32_t dh,
	uint32_t rw, uint32_t rh, uint32_t tw, uint32_t th, uint64_t scene_depth, uint64_t back_buffer,
	JitterReach reach) noexcept
{
	JitterArm a;
	a.index = index;
	a.reach = reach;
	a.scene_depth = scene_depth;
	a.scene_w = tw;
	a.scene_h = th;
	a.display_w = dw;
	a.display_h = dh;
	a.back_buffer = back_buffer;
	if (dw == 0 || dh == 0 || rw == 0 || rh == 0 || tw == 0 || th == 0)
		return a;
	a.input_x = jx;
	a.input_y = jy;
	a.x = -jx * static_cast<float>(dw) / static_cast<float>(rw);
	a.y = -jy * static_cast<float>(dh) / static_cast<float>(rh);
	a.u = -jx / static_cast<float>(rw);
	a.v = -jy / static_cast<float>(rh);
	a.target_x = a.x * static_cast<float>(tw) / static_cast<float>(dw);
	a.target_y = a.y * static_cast<float>(th) / static_cast<float>(dh);
	return a;
}

JitterArm disarmed(const JitterArm &arm) noexcept
{
	JitterArm kept;
	kept.reach = arm.reach;
	kept.scene_depth = arm.scene_depth;
	kept.scene_w = arm.scene_w;
	kept.scene_h = arm.scene_h;
	kept.display_w = arm.display_w;
	kept.display_h = arm.display_h;
	kept.size_targets = arm.size_targets;
	kept.scene_targets = arm.scene_targets;
	kept.move_window = arm.move_window;
	kept.tested_quads = arm.tested_quads;
	kept.back_buffer = arm.back_buffer;
	kept.back_buffer_count = arm.back_buffer_count;
	for (uint32_t i = 0; i < arm.back_buffer_count; ++i)
		kept.back_buffers[i] = arm.back_buffers[i];
	return kept;
}

void jitter_arm_add_back_buffer(JitterArm &arm, uint64_t back_buffer) noexcept
{
	if (back_buffer == 0 || back_buffer == arm.back_buffer)
		return;
	for (uint32_t i = 0; i < arm.back_buffer_count; ++i) {
		if (arm.back_buffers[i] == back_buffer)
			return;
	}
	if (arm.back_buffer_count < kJitterBackBuffers)
		arm.back_buffers[arm.back_buffer_count++] = back_buffer;
}

bool jitter_arm_into_back_buffer(const JitterArm &arm, uint64_t target) noexcept
{
	if (target == 0)
		return false;
	if (target == arm.back_buffer)
		return true;
	for (uint32_t i = 0; i < arm.back_buffer_count; ++i) {
		if (arm.back_buffers[i] == target)
			return true;
	}
	return false;
}

void count_moved_draws(JitterDrawCounts &c, uint64_t serial, uint32_t n) noexcept
{
	for (uint32_t i = 0; i < kJitterCountedArms; ++i) {
		if (c.serial[i] == serial) {
			c.moved[i] += n;
			return;
		}
	}
	for (uint32_t i = 0; i < kJitterCountedArms; ++i) {
		if (c.serial[i] == 0) {
			c.serial[i] = serial;
			c.moved[i] = n;
			return;
		}
	}
	c.overflow += n;
}

void count_scene_draw(JitterDrawCounts &c, uint64_t bound_serial, bool aliased) noexcept
{
	if (bound_serial == 0) {
		++c.plain;
		if (aliased)
			++c.plain_aliased;
	} else
		count_moved_draws(c, bound_serial);
	if (aliased)
		++c.aliased;
}

void count_other_draw(JitterDrawCounts &c, uint32_t w, uint32_t h, uint32_t n) noexcept
{
	if (w == 0 || h == 0 || n == 0)
		return;
	for (uint32_t i = 0; i < kJitterOtherSizes; ++i) {
		if (c.other_draws[i] != 0 && c.other_w[i] == w && c.other_h[i] == h) {
			c.other_draws[i] += n;
			return;
		}
	}
	for (uint32_t i = 0; i < kJitterOtherSizes; ++i) {
		if (c.other_draws[i] == 0) {
			c.other_w[i] = w;
			c.other_h[i] = h;
			c.other_draws[i] = n;
			return;
		}
	}
	c.other_overflow += n;
}

void add_draw_counts(JitterDrawCounts &into, const JitterDrawCounts &from) noexcept
{
	into.plain += from.plain;
	into.overflow += from.overflow;
	into.replayed += from.replayed;
	into.aliased += from.aliased;
	into.plain_aliased += from.plain_aliased;
	into.by_target += from.by_target;
	into.depth_draws += from.depth_draws;
	into.outside_draws += from.outside_draws;
	for (uint32_t i = 0; i < kJitterCountedArms; ++i) {
		if (from.serial[i] != 0 && from.moved[i] != 0)
			count_moved_draws(into, from.serial[i], from.moved[i]);
	}
	for (uint32_t i = 0; i < kJitterOtherSizes; ++i)
		count_other_draw(into, from.other_w[i], from.other_h[i], from.other_draws[i]);
	into.other_overflow += from.other_overflow;
}

JitterDrawCounts replayed_draw_counts(const JitterDrawCounts &c) noexcept
{
	JitterDrawCounts r;
	r.plain = c.plain;
	r.aliased = c.aliased;
	r.plain_aliased = c.plain_aliased;
	r.by_target = c.by_target;
	r.depth_draws = c.depth_draws;
	r.outside_draws = c.outside_draws;
	r.replayed = c.replayed + c.overflow;
	for (uint32_t i = 0; i < kJitterCountedArms; ++i)
		r.replayed += c.moved[i];
	for (uint32_t i = 0; i < kJitterOtherSizes; ++i) {
		r.other_w[i] = c.other_w[i];
		r.other_h[i] = c.other_h[i];
		r.other_draws[i] = c.other_draws[i];
	}
	r.other_overflow = c.other_overflow;
	return r;
}

bool any_draw_counts(const JitterDrawCounts &c) noexcept
{
	if (c.plain != 0 || c.other_overflow != 0 || any_moved_draws(c))
		return true;
	for (uint32_t i = 0; i < kJitterOtherSizes; ++i) {
		if (c.other_draws[i] != 0)
			return true;
	}
	return false;
}

bool any_moved_draws(const JitterDrawCounts &c) noexcept
{
	if (c.overflow != 0 || c.replayed != 0)
		return true;
	for (uint32_t i = 0; i < kJitterCountedArms; ++i) {
		if (c.moved[i] != 0)
			return true;
	}
	return false;
}

JitterViewport offset_viewport(const JitterViewport &vp, const JitterArm &arm) noexcept
{
	JitterViewport out = vp;
	out.x += arm.target_x;
	out.y += arm.target_y;
	return out;
}

bool jitter_viewports_fractional(uint32_t count, const JitterViewport *viewports) noexcept
{
	if (viewports == nullptr)
		return false;
	for (uint32_t i = 0; i < count && i < kJitterViewportSlots; ++i) {
		if (!whole(viewports[i].x) || !whole(viewports[i].y))
			return true;
	}
	return false;
}

void jitter_on_viewports(JitterListState &s, const JitterArm &arm, uint32_t count,
	const JitterViewport *viewports, const JitterOffsets *known, JitterRebind *out) noexcept
{
	if (count == 0 || count > kJitterViewportSlots || viewports == nullptr) {
		s.app_count = 0;
		s.bound_serial = 0;
		s.bound_count = 0;
		return;
	}
	if (const JitterMovedSet *const back = find_moved(s, count, viewports)) {
		for (uint32_t i = 0; i < count; ++i)
			s.app[i] = back->app[i];
		s.bound_serial = back->serial;
	} else if (known != nullptr && recognise_moved(count, viewports, *known, s.app)) {
		s.bound_serial = kRecognisedSerial;
	} else {
		for (uint32_t i = 0; i < count; ++i)
			s.app[i] = viewports[i];
		s.bound_serial = 0;
	}
	s.app_count = count;
	set_bound(s, count, viewports);
	sync(s, arm, scene_bound(s, arm), out);
}

void jitter_on_depth(JitterListState &s, const JitterArm &arm, uint64_t depth, JitterRebind *out,
	uint32_t w, uint32_t h) noexcept
{
	s.depth = depth;
	s.depth_w = depth != 0 ? w : 0u;
	s.depth_h = depth != 0 ? h : 0u;
	sync(s, arm, scene_bound(s, arm), out);
}

void jitter_on_target(JitterListState &s, uint64_t target, uint32_t w, uint32_t h, bool scene_target,
	bool window) noexcept
{
	s.target = target;
	s.target_w = target != 0 ? w : 0u;
	s.target_h = target != 0 ? h : 0u;
	s.target_scene = target != 0 && scene_target;
	s.target_window = target != 0 && window;
}

void jitter_on_depth_test(JitterListState &s, bool enabled) noexcept
{
	s.depth_test = enabled;
}

bool jitter_move_all(bool was_on, const JitterFrame &frame) noexcept
{
	return frame.empty || (was_on && frame.depth_draws == 0 && frame.outside_draws == 0);
}

bool jitter_draw_into_window(const JitterListState &s, const JitterArm &arm) noexcept
{
	return jitter_arm_into_back_buffer(arm, s.target) || (s.target != 0 && s.target_window);
}

void scene_rules(unsigned int rule, bool move_all, bool *size_targets, bool *move_window) noexcept
{
	*size_targets = rule != 2u || move_all;
	*move_window = rule == 1u || move_all;
}

bool jitter_scene_depth_bound(const JitterListState &s, const JitterArm &arm) noexcept
{
	return scene_depth_bound(s, arm);
}

bool jitter_scene_by_size(const JitterArm &arm) noexcept
{
	return arm.scene_depth == 0 && arm.scene_w != 0 && arm.scene_h != 0;
}

void jitter_draw_size(const JitterListState &s, uint32_t *w, uint32_t *h) noexcept
{
	const bool target = s.target != 0 && s.target_w != 0;
	*w = target ? s.target_w : s.depth != 0 ? s.depth_w : 0u;
	*h = target ? s.target_h : s.depth != 0 ? s.depth_h : 0u;
}

bool jitter_on_draw(JitterListState &s, const JitterArm &arm, bool fullscreen, JitterRebind *out) noexcept
{
	const bool scene = scene_bound(s, arm) && !fullscreen;
	s.depth_aliased = scene && arm.scene_depth != 0 && s.depth != 0 && s.depth != arm.scene_depth &&
		scene_sized(arm, s.depth_w, s.depth_h);
	sync(s, arm, scene, out);
	return scene;
}

void jitter_on_cleared(JitterListState &s) noexcept
{
	s.app_count = 0;
	s.bound_serial = 0;
	s.bound_count = 0;
	s.depth = 0;
	s.depth_w = 0;
	s.depth_h = 0;
	s.depth_aliased = false;
	s.target = 0;
	s.target_w = 0;
	s.target_h = 0;
	s.target_scene = false;
	s.target_window = false;
	s.depth_test = true;
}

void jitter_on_reset(JitterListState &s) noexcept
{
	s = JitterListState{};
}

bool jitter_rebind_expected(const JitterRebind &r, uint32_t count, const JitterViewport *bound) noexcept
{
	return r.expect_count == count && same_rectangles(r.expect, bound, count);
}

bool jitter_is_fullscreen_draw(uint32_t vertices, uint32_t instances) noexcept
{
	return vertices != 0u && vertices <= 6u && instances <= 1u;
}

void SceneJitter::arm(JitterArm arm) noexcept
{
	std::unique_lock<std::shared_mutex> lock(lock_);
	arm.serial = ++next_serial_;
	history_[history_next_ % kHistory] = arm;
	++history_next_;
	bool seen = false;
	for (uint32_t k = 0; k < known_.count && !seen; ++k)
		seen = known_.x[k] == arm.target_x && known_.y[k] == arm.target_y;
	if (!seen) {
		const uint32_t slot = known_.count < kJitterKnownOffsets
			? known_.count++ : known_next_++ % kJitterKnownOffsets;
		known_.x[slot] = arm.target_x;
		known_.y[slot] = arm.target_y;
	}
	frame_.fetch_add(1);
	if (lags_.load()) {
		pending_ = arm;
		pending_set_ = true;
		return;
	}
	pending_ = JitterArm{};
	pending_set_ = false;
	arm_ = arm;
	scene_known_.store(names_scene(arm));
	version_.fetch_add(1);
}

void SceneJitter::disarm() noexcept
{
	if (lags_.load())
		return;
	std::unique_lock<std::shared_mutex> lock(lock_);
	arm_ = disarmed(arm_);
	version_.fetch_add(1);
}

void SceneJitter::stop() noexcept
{
	std::unique_lock<std::shared_mutex> lock(lock_);
	frame_.fetch_add(1);
	pending_ = JitterArm{};
	if (lags_.load()) {
		pending_set_ = true;
		return;
	}
	pending_set_ = false;
	arm_ = JitterArm{};
	scene_known_.store(false);
	version_.fetch_add(1);
}

void SceneJitter::stop_now() noexcept
{
	std::unique_lock<std::shared_mutex> lock(lock_);
	arm_ = JitterArm{};
	pending_ = JitterArm{};
	pending_set_ = false;
	in_frame_.store(false);
	scene_known_.store(false);
	frame_.fetch_add(1);
	version_.fetch_add(1);
}

void SceneJitter::set_present_lags(bool lags) noexcept
{
	if (lags_.exchange(lags) == lags || lags)
		return;
	promote_now();
}

namespace {

uint64_t packed_size(uint32_t w, uint32_t h) noexcept
{
	return (static_cast<uint64_t>(w) << 32) | h;
}

}

void SceneJitter::note_presented(uint64_t target) noexcept
{
	if (target == 0 || presented_target(target))
		return;
	presented_[presented_next_.fetch_add(1, std::memory_order_relaxed) % kJitterPresentedTargets].store(target,
		std::memory_order_release);
	for (uint32_t i = 0; i < kJitterSceneTargets; ++i) {
		uint64_t t = target;
		scene_targets_[i].compare_exchange_strong(t, 0, std::memory_order_acq_rel);
	}
}

void SceneJitter::note_presentation() noexcept
{
	const uint64_t from = last_pass_target_.load(std::memory_order_relaxed);
	if (from == 0 || presented_target(from))
		return;
	uint32_t seen = 0;
	{
		const std::lock_guard<std::mutex> lock(presentation_lock_);
		for (uint32_t i = 0; i < kPresentedHistory; ++i)
			seen += presentations_[i] == from ? 1u : 0u;
		presentations_[presentation_next_++ % kPresentedHistory] = from;
	}
	if (seen != 0)
		note_presented(from);
}

bool SceneJitter::presented_target(uint64_t target) const noexcept
{
	if (target == 0)
		return false;
	for (uint32_t i = 0; i < kJitterPresentedTargets; ++i) {
		if (presented_[i].load(std::memory_order_acquire) == target)
			return true;
	}
	return false;
}

void SceneJitter::forget_target(uint64_t resource) noexcept
{
	if (resource == 0)
		return;
	for (uint32_t i = 0; i < kJitterSceneTargets; ++i) {
		uint64_t t = resource;
		scene_targets_[i].compare_exchange_strong(t, 0, std::memory_order_acq_rel);
	}
	for (uint32_t i = 0; i < kJitterPresentedTargets; ++i) {
		uint64_t t = resource;
		presented_[i].compare_exchange_strong(t, 0, std::memory_order_acq_rel);
	}
}

void SceneJitter::note_scene_target(uint64_t target, uint32_t w, uint32_t h) noexcept
{
	if (target == 0 || w == 0 || h == 0 || presented_target(target) || scene_target(target, w, h))
		return;
	const uint32_t slot = scene_target_next_.fetch_add(1, std::memory_order_relaxed) % kJitterSceneTargets;
	scene_targets_[slot].store(0, std::memory_order_release);
	scene_target_sizes_[slot].store(packed_size(w, h), std::memory_order_release);
	scene_targets_[slot].store(target, std::memory_order_release);
}

bool SceneJitter::scene_target(uint64_t target, uint32_t w, uint32_t h) const noexcept
{
	if (target == 0 || presented_target(target))
		return false;
	const uint64_t size = packed_size(w, h);
	for (uint32_t i = 0; i < kJitterSceneTargets; ++i) {
		if (scene_targets_[i].load(std::memory_order_acquire) == target &&
			scene_target_sizes_[i].load(std::memory_order_acquire) == size)
			return true;
	}
	return false;
}

void SceneJitter::forget_scene_targets() noexcept
{
	for (uint32_t i = 0; i < kJitterSceneTargets; ++i)
		scene_targets_[i].store(0, std::memory_order_release);
	for (uint32_t i = 0; i < kJitterPresentedTargets; ++i)
		presented_[i].store(0, std::memory_order_release);
	last_pass_target_.store(0, std::memory_order_relaxed);
	const std::lock_guard<std::mutex> lock(presentation_lock_);
	for (uint64_t &p : presentations_)
		p = 0;
}

void SceneJitter::watch_swapchain(const uint64_t *images, uint32_t count) noexcept
{
	forget_sizes();
	forget_scene_targets();
	std::unique_lock<std::shared_mutex> lock(lock_);
	swapchain_count_ = 0;
	for (uint32_t i = 0; images != nullptr && i < count && swapchain_count_ < kJitterBackBuffers; ++i) {
		if (images[i] != 0)
			swapchain_[swapchain_count_++] = images[i];
	}
}

bool SceneJitter::swapchain_image(uint64_t resource) const noexcept
{
	if (resource == 0)
		return false;
	std::shared_lock<std::shared_mutex> lock(lock_);
	for (uint32_t i = 0; i < swapchain_count_; ++i) {
		if (swapchain_[i] == resource)
			return true;
	}
	return false;
}

void SceneJitter::back_buffers_into(JitterArm &arm) const noexcept
{
	std::shared_lock<std::shared_mutex> lock(lock_);
	for (uint32_t i = 0; i < swapchain_count_; ++i)
		jitter_arm_add_back_buffer(arm, swapchain_[i]);
}

void SceneJitter::note_render_pass(uint64_t target, bool depth) noexcept
{
	if (!lags_.load())
		return;
	if (!depth) {
		if (swapchain_image(target))
			in_frame_.store(false);
		return;
	}
	if (in_frame_.exchange(true))
		return;
	boundaries_.fetch_add(1);
	std::unique_lock<std::shared_mutex> lock(lock_);
	if (pending_set_)
		++promotions_.at_boundary;
	promote_locked();
}

void SceneJitter::promote_now(bool forced) noexcept
{
	std::unique_lock<std::shared_mutex> lock(lock_);
	if (pending_set_)
		++(forced ? promotions_.forced : promotions_.between_frames);
	promote_locked();
}

JitterPromotions SceneJitter::take_promotions() noexcept
{
	std::unique_lock<std::shared_mutex> lock(lock_);
	const JitterPromotions p = promotions_;
	promotions_ = JitterPromotions{};
	return p;
}

void SceneJitter::promote_locked() noexcept
{
	if (!pending_set_)
		return;
	arm_ = pending_;
	pending_ = JitterArm{};
	pending_set_ = false;
	scene_known_.store(names_scene(arm_));
	version_.fetch_add(1);
}

bool SceneJitter::pending() const noexcept
{
	std::shared_lock<std::shared_mutex> lock(lock_);
	return pending_set_;
}

JitterArm SceneJitter::current() const noexcept
{
	std::shared_lock<std::shared_mutex> lock(lock_);
	return arm_;
}

uint64_t SceneJitter::last_serial() const noexcept
{
	std::shared_lock<std::shared_mutex> lock(lock_);
	return next_serial_;
}

bool SceneJitter::held_locked(uintptr_t list) const noexcept
{
	const uint64_t now = frame_.load();
	for (uint32_t i = 0; i < kJitterHeldLists; ++i) {
		if (held_[i] == list)
			return now - held_at_[i] < kJitterHoldPresents;
	}
	return false;
}

JitterArm SceneJitter::current_for(uintptr_t list) const noexcept
{
	std::shared_lock<std::shared_mutex> lock(lock_);
	if (list != 0 && held_slots_.load() != 0 && held_locked(list))
		return disarmed(arm_);
	return arm_;
}

bool SceneJitter::engaged() const noexcept
{
	return scene_known_.load() || lists_bound_.load() > 0;
}

void SceneJitter::watch_device(uintptr_t device) noexcept
{
	if (watched_.exchange(device) != device)
		saw_viewports_.store(false);
}

void SceneJitter::note_viewports(uintptr_t device) noexcept
{
	if (device != 0 && device == watched_.load())
		saw_viewports_.store(true);
}

void SceneJitter::known_offsets(JitterOffsets *out) const noexcept
{
	std::shared_lock<std::shared_mutex> lock(lock_);
	out->count = known_.count;
	for (uint32_t k = 0; k < known_.count; ++k) {
		out->x[k] = known_.x[k];
		out->y[k] = known_.y[k];
	}
}

void SceneJitter::note_bound(bool before, bool after) noexcept
{
	if (before != after)
		lists_bound_.fetch_add(after ? 1 : -1);
}

void SceneJitter::hold(uintptr_t list) noexcept
{
	if (list == 0)
		return;
	std::unique_lock<std::shared_mutex> lock(lock_);
	const uint64_t now = frame_.load();
	uint32_t slot = kJitterHeldLists;
	for (uint32_t i = 0; i < kJitterHeldLists && slot == kJitterHeldLists; ++i) {
		if (held_[i] == list)
			slot = i;
	}
	if (slot == kJitterHeldLists) {
		for (uint32_t i = 0; i < kJitterHeldLists && slot == kJitterHeldLists; ++i) {
			if (held_[i] == 0 || now - held_at_[i] >= kJitterHoldPresents)
				slot = i;
		}
		if (slot == kJitterHeldLists)
			slot = held_next_++ % kJitterHeldLists;
		if (held_[slot] == 0)
			held_slots_.fetch_add(1);
		held_[slot] = list;
	}
	held_at_[slot] = now;
	version_.fetch_add(1);
}

void SceneJitter::release(uintptr_t list) noexcept
{
	if (list == 0 || held_slots_.load() == 0)
		return;
	std::unique_lock<std::shared_mutex> lock(lock_);
	for (uint32_t i = 0; i < kJitterHeldLists; ++i) {
		if (held_[i] == list) {
			held_[i] = 0;
			held_at_[i] = 0;
			held_slots_.fetch_sub(1);
			version_.fetch_add(1);
			return;
		}
	}
}

bool SceneJitter::held(uintptr_t list) const noexcept
{
	if (list == 0 || held_slots_.load() == 0)
		return false;
	std::shared_lock<std::shared_mutex> lock(lock_);
	return held_locked(list);
}

uint32_t SceneJitter::live_holds() const noexcept
{
	if (held_slots_.load() == 0)
		return 0;
	std::shared_lock<std::shared_mutex> lock(lock_);
	const uint64_t now = frame_.load();
	uint32_t n = 0;
	for (uint32_t i = 0; i < kJitterHeldLists; ++i) {
		if (held_[i] != 0 && now - held_at_[i] < kJitterHoldPresents)
			++n;
	}
	return n;
}

void SceneJitter::hand_on(const JitterDrawCounts &counts) noexcept
{
	std::lock_guard<std::mutex> lock(counts_mutex_);
	add_draw_counts(counts_, counts);
}

void SceneJitter::discard_counts() noexcept
{
	std::lock_guard<std::mutex> lock(counts_mutex_);
	counts_ = JitterDrawCounts{};
}

JitterFrame SceneJitter::take_frame() noexcept
{
	JitterDrawCounts c;
	{
		std::lock_guard<std::mutex> lock(counts_mutex_);
		c = counts_;
		counts_ = JitterDrawCounts{};
	}
	JitterFrame f;
	f.plain = c.plain;
	f.replayed = c.replayed;
	f.aliased = c.aliased;
	f.plain_aliased = c.plain_aliased;
	f.overflow = c.overflow;
	f.unknown = c.overflow;
	for (uint32_t i = 0; i < kJitterCountedArms; ++i) {
		f.serials[i] = c.serial[i];
		f.serial_moved[i] = c.moved[i];
	}
	{
		std::shared_lock<std::shared_mutex> lock(lock_);
		f.armed_serial = arm_.serial;
		f.armed_pending = pending_set_;
	}

	f.by_target = c.by_target;
	f.depth_draws = c.depth_draws;
	f.outside_draws = c.outside_draws;

	for (uint32_t i = 0; i < kJitterOtherSizes; ++i) {
		if (c.other_draws[i] > f.other_draws) {
			f.other_w = c.other_w[i];
			f.other_h = c.other_h[i];
			f.other_draws = c.other_draws[i];
		}
	}
	f.moved = c.overflow;
	for (uint32_t i = 0; i < kJitterCountedArms; ++i)
		f.moved += c.moved[i];
	if (f.moved == 0) {
		f.elsewhere = c.replayed;
		return f;
	}

	struct Group {
		float x = 0.0f, y = 0.0f;
		uint32_t draws = 0;
		const JitterArm *arm = nullptr;
	};
	Group groups[kJitterCountedArms + 1u];
	uint32_t group_count = 1u;
	groups[0].draws = c.plain;
	uint32_t unknown = c.overflow;

	std::shared_lock<std::shared_mutex> lock(lock_);
	for (uint32_t i = 0; i < kJitterCountedArms; ++i) {
		if (c.serial[i] == 0 || c.moved[i] == 0)
			continue;
		const JitterArm *a = nullptr;
		for (const JitterArm &h : history_) {
			if (h.serial == c.serial[i]) {
				a = &h;
				break;
			}
		}
		if (a == nullptr) {
			unknown += c.moved[i];
			f.unknown += c.moved[i];
			continue;
		}
		uint32_t g = 0;
		while (g < group_count && (groups[g].x != a->target_x || groups[g].y != a->target_y))
			++g;
		if (g == group_count) {
			groups[g].x = a->target_x;
			groups[g].y = a->target_y;
			++group_count;
		}
		groups[g].draws += c.moved[i];
		if (groups[g].arm == nullptr)
			groups[g].arm = a;
	}

	uint32_t best = 0;
	for (uint32_t g = 1; g < group_count; ++g) {
		if (groups[g].draws > groups[best].draws || (best == 0 && groups[g].draws == groups[0].draws))
			best = g;
	}
	const uint32_t total = f.moved + f.plain;
	if (groups[best].arm != nullptr && groups[best].draws > unknown) {
		const JitterArm &a = *groups[best].arm;
		f.valid = true;
		f.serial = a.serial;
		f.index = a.index;
		f.x = a.x;
		f.y = a.y;
		f.u = a.u;
		f.v = a.v;
		f.input_x = a.input_x;
		f.input_y = a.input_y;
		f.elsewhere = total - groups[best].draws + c.replayed;
		return f;
	}
	f.ambiguous = true;
	f.elsewhere = f.moved + c.replayed;
	return f;
}

bool jitter_frame_like(uint32_t w, uint32_t h, uint32_t dw, uint32_t dh) noexcept
{
	if (w == 0 || h == 0 || dw == 0 || dh == 0)
		return false;
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float display = static_cast<float>(dw) / static_cast<float>(dh);
	return std::fabs(aspect - display) <= 0.1f && 4ull * w >= dw && 4ull * h >= dh &&
		w <= 2ull * dw && h <= 2ull * dh;
}

bool jitter_learn_scene_size(const JitterFrame &frame, uint32_t display_w, uint32_t display_h,
	uint32_t *w, uint32_t *h) noexcept
{
	const uint64_t reached = static_cast<uint64_t>(frame.moved) + frame.plain;
	if (frame.other_draws < kJitterLearnDraws || frame.other_draws <= reached ||
		!jitter_frame_like(frame.other_w, frame.other_h, display_w, display_h))
		return false;
	*w = frame.other_w;
	*h = frame.other_h;
	return true;
}

bool JitterGate::misplaced(const JitterFrame &frame) noexcept
{
	if (frame.ambiguous)
		return true;
	const uint64_t total = static_cast<uint64_t>(frame.moved) + frame.plain + frame.replayed;
	return frame.elsewhere != 0 && static_cast<uint64_t>(frame.elsewhere) * kElsewhereShare > total;
}

void JitterGate::note(const JitterFrame &frame) noexcept
{
	if (closed_for_ != 0) {
		--closed_for_;
		return;
	}
	const bool off = misplaced(frame);
	if (frame.valid && !off) {
		run_ = 0;
		if (++clean_ >= kSteady)
			next_wait_ = kFirstWait;
		return;
	}
	if (off)
		clean_ = 0;
	if (!off || ++run_ < kRun)
		return;
	close();
}

void JitterGate::note_failed() noexcept
{
	if (closed_for_ == 0)
		close();
}

void JitterGate::close() noexcept
{
	run_ = 0;
	clean_ = 0;
	closed_for_ = next_wait_;
	next_wait_ = (std::min)(next_wait_ * 2u, kLongestWait);
}

void JitterSchedule::earn(uintptr_t swapchain, const JitterArm &arm) noexcept
{
	owner_ = swapchain;
	pending_ = arm;
	pending_set_ = true;
}

JitterSchedule::Present JitterSchedule::present(uintptr_t swapchain, JitterArm *arm) noexcept
{
	if (owner_ == 0 || swapchain != owner_)
		return Present::Ignore;
	if (pending_set_) {
		*arm = pending_;
		pending_set_ = false;
		stops_ = 0;
		return Present::Arm;
	}
	if (++stops_ >= kReleaseAfter) {
		owner_ = 0;
		stops_ = 0;
	}
	return Present::Stop;
}

void JitterSchedule::forget() noexcept
{
	owner_ = 0;
	pending_set_ = false;
	stops_ = 0;
}

void JitterDriver::begin_frame(uintptr_t swapchain, JitterCounts &counts) noexcept
{
	swapchain_ = swapchain;
	upscaled_ = false;
	reached_ = false;
	waiting_ = false;
	drawn_ = JitterFrame{};
	if (last_swapchain_ != 0 && swapchain != last_swapchain_)
		alone_for_ = 0;
	else if (alone_for_ < kAloneAfter)
		++alone_for_;
	last_swapchain_ = swapchain;

	drives_ = schedule_.drives(swapchain);
	if (!drives_)
		return;
	schedule_.begin();
	drawn_ = counts.take();
	drawn_.empty = armed_ && drawn_.moved == 0 && drawn_.replayed == 0;
	jitter_.disarm();
	gate_.note(drawn_);
	frame_open_ = true;
	note_ = !armed_ || drawn_.valid || drawn_.ambiguous ? ""
		: drawn_.plain != 0 ? "the game drew the scene without it"
		: "nothing the game drew could take it";
	armed_ = false;
}

const char *JitterDriver::refusal() const noexcept
{
	if (!drives_)
		return "another window of the game drives it";
	if (alone_for_ < kAloneAfter)
		return "the game is drawing to more than one window";
	if (!upscaled_)
		return "the upscaler did not run this frame";
	if (!reached_)
		return "the upscaled picture did not reach the game";
	if (!gate_.open())
		return "paused, part of the scene sat where the upscaler was not told";
	return nullptr;
}

bool JitterDriver::earn(const JitterArm &arm) noexcept
{
	if (refusal() != nullptr)
		return false;
	schedule_.earn(swapchain_, arm);
	return true;
}

void JitterDriver::present(uintptr_t swapchain, JitterCounts &counts) noexcept
{
	JitterArm arm;
	switch (schedule_.present(swapchain, &arm)) {
	case JitterSchedule::Present::Ignore:
		return;
	case JitterSchedule::Present::Arm:
		counts.discard();
		jitter_.arm(arm);
		armed_serial_ = jitter_.last_serial();
		armed_ = true;
		frame_open_ = false;
		raw_history_ <<= 1;
		break;
	case JitterSchedule::Present::Stop:
		counts.discard();
		jitter_.stop();
		armed_serial_ = 0;
		raw_history_ <<= 1;
		if (frame_open_ && drawn_.moved != 0 && !(upscaled_ && reached_) && !waiting_) {
			raw_history_ |= 1u;
			uint32_t raw = 0;
			for (uint32_t bits = raw_history_ & ((1u << kRawMovedWindow) - 1u); bits != 0; bits >>= 1)
				raw += bits & 1u;
			if (!jitter_.present_lags() || raw >= kRawMovedInWindow)
				gate_.note_failed();
		}
		armed_ = false;
		frame_open_ = false;
		break;
	}
	if (jitter_.present_lags()) {
		const uint64_t seen = jitter_.boundaries();
		if (seen != last_boundaries_ && last_boundaries_ != ~0ull) {
			stale_presents_ = 0;
		} else if (last_boundaries_ != ~0ull) {
			++stale_presents_;
			if (!jitter_.in_frame() || stale_presents_ >= kPromoteStaleAfter) {
				jitter_.promote_now(jitter_.in_frame());
				stale_presents_ = 0;
			}
		}
		last_boundaries_ = seen;
	}
}

void JitterDriver::forget() noexcept
{
	jitter_.stop_now();
	schedule_.forget();
	gate_ = JitterGate{};
	drawn_ = JitterFrame{};
	last_swapchain_ = 0;
	alone_for_ = kAloneAfter;
	drives_ = false;
	armed_ = false;
	frame_open_ = false;
	last_boundaries_ = ~0ull;
	stale_presents_ = 0;
	raw_history_ = 0;
	armed_serial_ = 0;
}

void JitterDriver::depth_uv(float *u, float *v) const noexcept
{
	const bool on = upscaled_ && drawn_.valid;
	*u = on ? drawn_.u : 0.0f;
	*v = on ? drawn_.v : 0.0f;
}

void JitterDriver::drawn_uv(float *u, float *v) const noexcept
{
	*u = drawn_.valid ? drawn_.u : 0.0f;
	*v = drawn_.valid ? drawn_.v : 0.0f;
}

namespace {

bool fault_number(const std::string &v, double *out)
{
	if (v.empty())
		return false;
	char *end = nullptr;
	const double d = std::strtod(v.c_str(), &end);
	if (end == nullptr || *end != '\0' || !std::isfinite(d))
		return false;
	*out = d;
	return true;
}

}

bool parse_jitter_fault(const std::string &text, JitterFault *out)
{
	JitterFault f;
	size_t at = 0;
	const std::string all = text;
	while (at <= all.size()) {
		size_t end = all.find('+', at);
		if (end == std::string::npos)
			end = all.size();
		std::string item = all.substr(at, end - at);
		while (!item.empty() && (item.front() == ' ' || item.front() == '\t'))
			item.erase(item.begin());
		while (!item.empty() && (item.back() == ' ' || item.back() == '\t' || item.back() == '\r'))
			item.pop_back();
		at = end + 1;
		if (item.empty() || item == "none") {
			if (end == all.size())
				break;
			continue;
		}
		const size_t sep = item.find_first_of(":=");
		const std::string name = item.substr(0, sep);
		const std::string value = sep == std::string::npos ? std::string() : item.substr(sep + 1);
		double v = 0.0;
		if (name == "told_lag" && value.empty())
			f.told_lag = true;
		else if (name == "told_drop_at" && fault_number(value, &v) && v >= 0.0 && v == std::floor(v) && v < 2147483647.0)
			f.told_drop_at = static_cast<int32_t>(v);
		else if (name == "draw_drop_every" && fault_number(value, &v) && v >= 1.0 && v == std::floor(v) && v < 4294967295.0)
			f.draw_drop_every = static_cast<uint32_t>(v);
		else if (name == "odd_told_scale" && fault_number(value, &v) && std::fabs(v) <= 8.0)
			f.odd_told_scale = static_cast<float>(v);
		else if (name == "draw_scale" && fault_number(value, &v) && std::fabs(v) <= 8.0)
			f.draw_scale = static_cast<float>(v);
		else if (name == "told_sign_x" && value.empty())
			f.told_sign_x = -1.0f;
		else if (name == "told_sign_x" && fault_number(value, &v) && std::fabs(v) <= 8.0)
			f.told_sign_x = static_cast<float>(v);
		else
			return false;
		if (end == all.size())
			break;
	}
	*out = f;
	return true;
}

std::string jitter_fault_text(const JitterFault &f)
{
	std::string s;
	const auto add = [&s](const char *name, const char *fmt, double v) {
		char b[64]{};
		std::snprintf(b, sizeof(b), fmt, v);
		s += (s.empty() ? "" : "+") + std::string(name) + b;
	};
	if (f.told_lag)
		add("told_lag", "", 0.0);
	if (f.told_drop_at >= 0)
		add("told_drop_at", "=%.0f", f.told_drop_at);
	if (f.draw_drop_every != 0)
		add("draw_drop_every", "=%.0f", f.draw_drop_every);
	if (f.odd_told_scale != 1.0f)
		add("odd_told_scale", "=%g", f.odd_told_scale);
	if (f.draw_scale != 1.0f)
		add("draw_scale", "=%g", f.draw_scale);
	if (f.told_sign_x != 1.0f)
		add("told_sign_x", "=%g", f.told_sign_x);
	return s.empty() ? "none" : s;
}

bool jitter_fault_any(const JitterFault &f) noexcept
{
	return f.told_lag || f.told_drop_at >= 0 || f.draw_drop_every != 0 || f.odd_told_scale != 1.0f ||
		f.draw_scale != 1.0f || f.told_sign_x != 1.0f;
}

bool jitter_fault_hit(uint32_t every, uint32_t frame) noexcept
{
	if (every == 0 || frame < every)
		return false;
	const uint64_t g1 = static_cast<uint64_t>(every) + 3u, g2 = every > 3u ? every - 2u : 1u, g3 = every;
	const uint64_t r = (frame - every) % (g1 + g2 + g3);
	return r == 0 || r == g1 || r == g1 + g2;
}

bool apply_told_fault(const JitterFault &f, uint32_t frame, bool odd, bool have_prev, float prev_x,
	float prev_y, float *told_x, float *told_y) noexcept
{
	bool changed = false;
	if (f.told_lag && have_prev) {
		*told_x = prev_x;
		*told_y = prev_y;
		changed = true;
	}
	if (f.odd_told_scale != 1.0f && odd) {
		*told_x *= f.odd_told_scale;
		*told_y *= f.odd_told_scale;
		changed = true;
	}
	if (f.told_drop_at >= 0 && frame == static_cast<uint32_t>(f.told_drop_at)) {
		*told_x = *told_y = 0.0f;
		changed = true;
	}
	if (f.told_sign_x != 1.0f && *told_x != 0.0f) {
		*told_x *= f.told_sign_x;
		changed = true;
	}
	return changed;
}

bool apply_draw_fault(const JitterFault &f, uint32_t frame, JitterArm *arm) noexcept
{
	if (arm->target_x == 0.0f && arm->target_y == 0.0f)
		return false;
	if (jitter_fault_hit(f.draw_drop_every, frame)) {
		arm->target_x = arm->target_y = 0.0f;
		return true;
	}
	if (f.draw_scale != 1.0f) {
		arm->target_x *= f.draw_scale;
		arm->target_y *= f.draw_scale;
		return true;
	}
	return false;
}

}
