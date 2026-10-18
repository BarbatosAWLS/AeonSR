#include "aeon_sr/core/game_render_scale.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/frame_inputs.hpp"

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aeon_sr {

using namespace reshade::api;

GameScaleCheck game_scale_draw_check(const GameScaleDraw &d, uint32_t *render_w, uint32_t *render_h) noexcept
{
	if (d.elements == 0 || d.elements > 6 || d.instances > 1)
		return GameScaleCheck::NotStretch;
	if (d.target_w == 0 || d.target_h == 0 || d.source_is_target)
		return GameScaleCheck::NotStretch;
	if (d.vp_x != 0.0f || d.vp_y != 0.0f || d.vp_w != static_cast<float>(d.target_w) ||
		d.vp_h != static_cast<float>(d.target_h))
		return GameScaleCheck::NotStretch;
	if (d.source_w != d.target_w || d.source_h != d.target_h)
		return GameScaleCheck::NotStretch;
	const uint32_t rw = d.source_vp_w, rh = d.source_vp_h;
	if (rw == 0 || rh == 0 || rw >= d.target_w || rh >= d.target_h)
		return GameScaleCheck::NotStretch;
	if (rw * 4u < d.target_w || rh * 4u < d.target_h)
		return GameScaleCheck::NotStretch;
	const double sx = static_cast<double>(rw) / d.target_w, sy = static_cast<double>(rh) / d.target_h;
	const double tol = 1.5 / static_cast<double>(rw < rh ? rw : rh);
	if (std::fabs(sx - sy) > tol)
		return GameScaleCheck::NotStretch;
	if (d.corner_depth_draws == 0 || d.corner_depth_draws * 4u < d.most_depth_draws)
		return GameScaleCheck::OtherCorner;
	*render_w = rw;
	*render_h = rh;
	return GameScaleCheck::Match;
}

bool game_scale_draw_matches(const GameScaleDraw &d, uint32_t *render_w, uint32_t *render_h) noexcept
{
	return game_scale_draw_check(d, render_w, render_h) == GameScaleCheck::Match;
}

GameScaleCheck game_scale_full_check(const GameScaleDraw &d) noexcept
{
	if (d.elements == 0 || d.elements > 6 || d.instances > 1)
		return GameScaleCheck::NotStretch;
	if (d.target_w == 0 || d.target_h == 0 || d.source_is_target)
		return GameScaleCheck::NotStretch;
	if (d.vp_x != 0.0f || d.vp_y != 0.0f || d.vp_w != static_cast<float>(d.target_w) ||
		d.vp_h != static_cast<float>(d.target_h))
		return GameScaleCheck::NotStretch;
	if (d.source_w != d.target_w || d.source_h != d.target_h || d.source_vp_w != d.target_w ||
		d.source_vp_h != d.target_h)
		return GameScaleCheck::NotStretch;
	if (d.corner_depth_draws == 0 || d.corner_depth_draws * 4u < d.most_depth_draws)
		return GameScaleCheck::OtherCorner;
	return GameScaleCheck::Match;
}

namespace {

struct Written {
	uint64_t frame = 0;
	uint32_t w = 0, h = 0;
};

struct Timing {
	static constexpr int kSlots = 4;
	struct Slot {
		ID3D11Query *disjoint = nullptr, *start = nullptr, *stretch = nullptr, *end = nullptr;
		bool pending = false, has_stretch = false;
	};
	ID3D11DeviceContext *ctx = nullptr;
	Slot slots[kSlots];
	int cur = 0;
	bool begun = false;
	uint64_t last_end = 0, last_freq = 0;
	double scene_sum = 0.0, frame_sum = 0.0;
	uint32_t frames = 0, framed = 0, stretches = 0;
	uint64_t report_at = 0;

	bool ensure(ID3D11DeviceContext *c)
	{
		if (ctx == c)
			return true;
		release();
		ID3D11Device *dev = nullptr;
		c->GetDevice(&dev);
		if (dev == nullptr)
			return false;
		D3D11_QUERY_DESC dj{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, ts{ D3D11_QUERY_TIMESTAMP, 0 };
		bool ok = true;
		for (Slot &s : slots)
			ok = ok && SUCCEEDED(dev->CreateQuery(&dj, &s.disjoint)) && SUCCEEDED(dev->CreateQuery(&ts, &s.start)) &&
				SUCCEEDED(dev->CreateQuery(&ts, &s.stretch)) && SUCCEEDED(dev->CreateQuery(&ts, &s.end));
		dev->Release();
		if (!ok) {
			release();
			return false;
		}
		ctx = c;
		return true;
	}
	void release()
	{
		for (Slot &s : slots) {
			ID3D11Query **const all[] = { &s.disjoint, &s.start, &s.stretch, &s.end };
			for (ID3D11Query **q : all)
				if (*q != nullptr) {
					(*q)->Release();
					*q = nullptr;
				}
			s.pending = false;
		}
		ctx = nullptr;
		begun = false;
	}
	void first_draw(ID3D11DeviceContext *c)
	{
		if (begun || !ensure(c))
			return;
		Slot &s = slots[cur];
		if (s.pending)
			return;
		ctx->Begin(s.disjoint);
		ctx->End(s.start);
		s.has_stretch = false;
		begun = true;
	}
	void at_stretch()
	{
		if (!begun)
			return;
		ctx->End(slots[cur].stretch);
		slots[cur].has_stretch = true;
	}
	void at_present(GameScaleState &out)
	{
		if (begun) {
			Slot &s = slots[cur];
			ctx->End(s.end);
			ctx->End(s.disjoint);
			s.pending = true;
			begun = false;
			cur = (cur + 1) % kSlots;
		}
		if (ctx == nullptr)
			return;
		Slot &old = slots[cur];
		if (!old.pending)
			return;
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
		if (ctx->GetData(old.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			return;
		UINT64 t0 = 0, t1 = 0, t2 = 0;
		const bool got = ctx->GetData(old.start, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			ctx->GetData(old.end, &t2, sizeof(t2), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
			(!old.has_stretch || ctx->GetData(old.stretch, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK);
		old.pending = false;
		if (!got || dj.Disjoint || dj.Frequency == 0)
			return;
		const double to_ms = 1000.0 / static_cast<double>(dj.Frequency);
		scene_sum += static_cast<double>((old.has_stretch ? t1 : t2) - t0) * to_ms;
		++frames;
		stretches += old.has_stretch ? 1u : 0u;
		if (last_end != 0 && last_freq == dj.Frequency && t2 > last_end) {
			frame_sum += static_cast<double>(t2 - last_end) * to_ms;
			++framed;
		}
		last_end = t2;
		last_freq = dj.Frequency;
		const uint64_t now = GetTickCount64();
		if (report_at == 0)
			report_at = now;
		if (now >= report_at + 10000u && frames != 0) {
			out.scene_ms = scene_sum / frames;
			out.frame_ms = framed != 0 ? frame_sum / framed : 0.0;
			diag_logf(DiagLevel::Info, "scale", L"render scale: the game's own GPU time, last 10 s, %u frames (%u through "
				L"its stretch): %.2f ms a frame drawing its scene and what follows up to the stretch (to the present on a "
				L"frame with none); %.2f ms from present to present on the GPU",
				frames, stretches, out.scene_ms, out.frame_ms);
			scene_sum = frame_sum = 0.0;
			frames = framed = stretches = 0;
			report_at = now;
		}
	}
};

struct State {
	std::mutex lock;
	GameScaleHandler handler;
	device *game = nullptr;
	uint64_t frame = 1;

	resource rt0{};
	uint32_t rt0_w = 0, rt0_h = 0;
	resource dsv{};
	viewport vp{};
	resource ps_t0{};

	std::unordered_map<uint64_t, Written> written;
	struct From {
		uint64_t texture = 0;
		uint64_t at = 0;
	};
	std::unordered_map<uint64_t, From> written_from;
	uint64_t draw_index = 0;
	std::unordered_map<uint64_t, uint64_t> last_draw;
	std::map<std::pair<uint32_t, uint32_t>, uint32_t> depth_draws;
	std::map<std::pair<uint32_t, uint32_t>, std::unordered_map<uint64_t, uint32_t>> depth_by_vp;

	std::vector<uint64_t> windows;
	uint64_t window_source = 0, composite = 0;
	bool composite_drawn = false;

	bool found = false;
	bool other_corner = false;
	uint32_t near_w = 0, near_h = 0, near_draws = 0, near_most = 0, near_most_w = 0, near_most_h = 0;
	uint64_t missed_at = 0, missed_corner_said = 0, missed_none_said = 0, presents_said = 0;

	GameScaleState out;
	Timing timing;
};

State &st()
{
	static State s;
	return s;
}

std::atomic<bool> g_enabled{ false };
std::atomic<bool> g_full{ false };
bool g_registered = false;

bool immediate_d3d11(command_list *cmd)
{
	device *const dev = cmd->get_device();
	const device_api api = dev->get_api();
	if (api == device_api::d3d10)
		return true;
	if (api != device_api::d3d11)
		return false;
	auto *const ctx = reinterpret_cast<ID3D11DeviceContext *>(static_cast<uintptr_t>(cmd->get_native()));
	return ctx != nullptr && ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

void size_of(device *dev, resource r, uint32_t *w, uint32_t *h)
{
	*w = *h = 0;
	if (r.handle == 0)
		return;
	const resource_desc d = dev->get_resource_desc(r);
	if (d.type != resource_type::texture_2d)
		return;
	*w = d.texture.width;
	*h = d.texture.height;
}

void on_bind_render_targets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
	if (!g_enabled.load(std::memory_order_relaxed) || !immediate_d3d11(cmd))
		return;
	State &s = st();
	device *const dev = cmd->get_device();
	const resource rt = count > 0 && rtvs[0].handle != 0 ? dev->get_resource_from_view(rtvs[0]) : resource{};
	const resource ds = dsv.handle != 0 ? dev->get_resource_from_view(dsv) : resource{};
	uint32_t w = 0, h = 0;
	size_of(dev, rt, &w, &h);
	std::lock_guard<std::mutex> g(s.lock);
	if (dev != s.game)
		return;
	s.rt0 = rt;
	s.rt0_w = w;
	s.rt0_h = h;
	s.dsv = ds;
}

void on_bind_viewports(command_list *cmd, uint32_t first, uint32_t count, const viewport *vps)
{
	if (!g_enabled.load(std::memory_order_relaxed) || first != 0 || count == 0 || !immediate_d3d11(cmd))
		return;
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (cmd->get_device() == s.game)
		s.vp = vps[0];
}

void on_push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t,
	const descriptor_table_update &u)
{
	if (!g_enabled.load(std::memory_order_relaxed) || (stages & shader_stage::pixel) == static_cast<shader_stage>(0) ||
		u.type != descriptor_type::shader_resource_view || u.binding != 0 || u.count == 0 || !immediate_d3d11(cmd))
		return;
	State &s = st();
	device *const dev = cmd->get_device();
	const resource_view v = static_cast<const resource_view *>(u.descriptors)[0];
	const resource r = v.handle != 0 ? dev->get_resource_from_view(v) : resource{};
	std::lock_guard<std::mutex> g(s.lock);
	if (dev == s.game)
		s.ps_t0 = r;
}

bool on_draw_any(command_list *cmd, uint32_t elements, uint32_t instances)
{
	if (!g_enabled.load(std::memory_order_relaxed))
		return false;
	if (!immediate_d3d11(cmd))
		return false;
	State &s = st();
	device *const dev = cmd->get_device();
	GameScaleStretch stretch;
	GameScaleHandler handler;
	{
		std::lock_guard<std::mutex> g(s.lock);
		if (dev != s.game || s.rt0.handle == 0)
			return false;
		const bool immediate = true;
		if (immediate && dev->get_api() == device_api::d3d11)
			s.timing.first_draw(reinterpret_cast<ID3D11DeviceContext *>(static_cast<uintptr_t>(cmd->get_native())));
		const uint32_t vw = static_cast<uint32_t>(s.vp.width + 0.5f), vh = static_cast<uint32_t>(s.vp.height + 0.5f);
		GameScaleDraw d;
		d.elements = elements;
		d.instances = instances;
		d.target_w = s.rt0_w;
		d.target_h = s.rt0_h;
		d.vp_x = s.vp.x;
		d.vp_y = s.vp.y;
		d.vp_w = s.vp.width;
		d.vp_h = s.vp.height;
		d.source_is_target = s.ps_t0.handle == s.rt0.handle;
		if (s.ps_t0.handle != 0) {
			size_of(dev, s.ps_t0, &d.source_w, &d.source_h);
			const auto it = s.written.find(s.ps_t0.handle);
			if (it != s.written.end() && it->second.frame == s.frame) {
				d.source_vp_w = it->second.w;
				d.source_vp_h = it->second.h;
			}
		}
		uint32_t most_w = 0, most_h = 0;
		for (const auto &e : s.depth_draws) {
			if (e.second > d.most_depth_draws) {
				d.most_depth_draws = e.second;
				most_w = e.first.first;
				most_h = e.first.second;
			}
		}
		const auto corner = s.depth_draws.find(std::make_pair(d.source_vp_w, d.source_vp_h));
		d.corner_depth_draws = corner != s.depth_draws.end() ? corner->second : 0u;
		uint32_t rw = 0, rh = 0;
		const GameScaleCheck check = game_scale_draw_check(d, &rw, &rh);
		bool match = check == GameScaleCheck::Match && immediate;
		if (!match && g_full.load(std::memory_order_relaxed) && s.composite != 0 &&
			s.rt0.handle == s.composite && !s.composite_drawn &&
			game_scale_full_check(d) == GameScaleCheck::Match) {
			match = true;
			rw = d.target_w;
			rh = d.target_h;
		}
		if (s.rt0.handle == s.composite)
			s.composite_drawn = true;
		if (elements <= 6 && s.ps_t0.handle != 0 && s.vp.width * 2.0f >= static_cast<float>(s.rt0_w) &&
			s.vp.height * 2.0f >= static_cast<float>(s.rt0_h) &&
			std::find(s.windows.begin(), s.windows.end(), s.rt0.handle) != s.windows.end())
			s.window_source = s.ps_t0.handle;
		if (check == GameScaleCheck::OtherCorner) {
			s.other_corner = true;
			s.near_w = d.source_vp_w;
			s.near_h = d.source_vp_h;
			s.near_draws = d.corner_depth_draws;
			s.near_most = d.most_depth_draws;
			s.near_most_w = most_w;
			s.near_most_h = most_h;
		}
		s.found = s.found || match;
		s.written[s.rt0.handle] = Written{ s.frame, vw, vh };
		const uint64_t at = ++s.draw_index;
		s.last_draw[s.rt0.handle] = at;
		if (elements <= 6 && instances <= 1 && s.ps_t0.handle != 0 && s.ps_t0.handle != s.rt0.handle &&
			s.vp.x == 0.0f && s.vp.y == 0.0f)
			s.written_from[s.rt0.handle] = State::From{ s.ps_t0.handle, at };
		else
			s.written_from.erase(s.rt0.handle);
		if (s.dsv.handle != 0 && elements > 6) {
			const auto key = std::make_pair(vw, vh);
			++s.depth_draws[key];
			++s.depth_by_vp[key][s.dsv.handle];
		}
		if (!match || !s.handler)
			return false;
		resource depth{};
		uint32_t most = 0;
		const auto byvp = s.depth_by_vp.find(std::make_pair(rw, rh));
		if (byvp != s.depth_by_vp.end()) {
			for (const auto &e : byvp->second) {
				if (e.second > most) {
					most = e.second;
					depth = resource{ e.first };
				}
			}
		}
		stretch.cmd = cmd;
		stretch.source = s.ps_t0;
		const auto from = s.written_from.find(s.ps_t0.handle);
		const auto drawn = from != s.written_from.end() ? s.last_draw.find(from->second.texture) : s.last_draw.end();
		if (from != s.written_from.end() && s.written.count(from->second.texture) != 0 &&
			drawn != s.last_draw.end() && drawn->second < from->second.at) {
			const resource_desc a = dev->get_resource_desc(s.ps_t0);
			const resource_desc b = dev->get_resource_desc(resource{ from->second.texture });
			if (b.type == resource_type::texture_2d && a.texture.width == b.texture.width &&
				a.texture.height == b.texture.height && a.texture.format == b.texture.format &&
				b.texture.samples == 1 && b.texture.levels == 1 && b.texture.depth_or_layers == 1)
				stretch.before = resource{ from->second.texture };
		}
		stretch.target = s.rt0;
		stretch.depth = depth;
		stretch.render_w = rw;
		stretch.render_h = rh;
		stretch.width = s.rt0_w;
		stretch.height = s.rt0_h;
		s.out.found_frame = s.frame;
		s.out.render_w = rw;
		s.out.render_h = rh;
		s.out.width = s.rt0_w;
		s.out.height = s.rt0_h;
		s.timing.at_stretch();
		handler = s.handler;
	}
	if (!handler(stretch))
		return false;
	std::lock_guard<std::mutex> g(s.lock);
	++s.out.replaced;
	return true;
}

bool on_draw(command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t, uint32_t)
{
	return on_draw_any(cmd, vertices, instances);
}

bool on_draw_indexed(command_list *cmd, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t)
{
	return on_draw_any(cmd, indices, instances);
}

void on_present(command_queue *, swapchain *chain, const rect *, const rect *, uint32_t, const rect *)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (chain != nullptr) {
		s.game = chain->get_device();
		const resource bb = current_back_buffer_of(chain);
		if (bb.handle != 0 && std::find(s.windows.begin(), s.windows.end(), bb.handle) == s.windows.end()) {
			if (s.windows.size() >= 8)
				s.windows.erase(s.windows.begin());
			s.windows.push_back(bb.handle);
		}
	}
	if (s.window_source != 0)
		s.composite = s.window_source;
	s.window_source = 0;
	s.composite_drawn = false;
	if (g_enabled.load(std::memory_order_relaxed)) {
		s.timing.at_present(s.out);
		if (!s.found) {
			if (s.other_corner) {
				++s.out.missed_corner;
				s.out.miss_w = s.near_w;
				s.out.miss_h = s.near_h;
				s.out.miss_draws = s.near_draws;
				s.out.miss_most = s.near_most;
				s.out.miss_most_w = s.near_most_w;
				s.out.miss_most_h = s.near_most_h;
			} else {
				++s.out.missed_none;
			}
		}
		const uint64_t now = GetTickCount64();
		if (s.missed_at == 0)
			s.missed_at = now;
		if (now >= s.missed_at + 10000u) {
			const uint64_t corner = s.out.missed_corner - s.missed_corner_said, none = s.out.missed_none - s.missed_none_said;
			if (corner != 0)
				diag_logf(DiagLevel::Info, "scale", L"render scale: last 10 s %llu presents with a stretch of a corner the "
					L"scene was not drawn at (the last %ux%u, %u draws with a depth buffer there, %u at %ux%u), %llu with no "
					L"stretch", corner, s.out.miss_w, s.out.miss_h, s.out.miss_draws, s.out.miss_most, s.out.miss_most_w,
					s.out.miss_most_h, none);
			else if (none != 0 && none < s.out.presents - s.presents_said)
				diag_logf(DiagLevel::Info, "scale", L"render scale: last 10 s %llu presents with no stretch", none);
			s.missed_corner_said = s.out.missed_corner;
			s.missed_none_said = s.out.missed_none;
			s.presents_said = s.out.presents;
			s.missed_at = now;
		}
	}
	s.found = s.other_corner = false;
	++s.frame;
	++s.out.presents;
	s.written.clear();
	s.written_from.clear();
	s.last_draw.clear();
	s.draw_index = 0;
	s.depth_draws.clear();
	s.depth_by_vp.clear();
}

void on_init_swapchain(swapchain *chain, bool)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	for (uint32_t i = 0; i < chain->get_back_buffer_count(); ++i) {
		const uint64_t h = chain->get_back_buffer(i).handle;
		if (h != 0 && std::find(s.windows.begin(), s.windows.end(), h) == s.windows.end())
			s.windows.push_back(h);
	}
}

void on_destroy_swapchain(swapchain *chain, bool)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	for (uint32_t i = 0; i < chain->get_back_buffer_count(); ++i) {
		const uint64_t h = chain->get_back_buffer(i).handle;
		s.windows.erase(std::remove(s.windows.begin(), s.windows.end(), h), s.windows.end());
	}
	s.composite = s.window_source = 0;
}

void on_destroy_device(device *dev)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	if (dev == s.game) {
		s.timing.release();
		s.game = nullptr;
	}
}

void on_destroy_resource(device *, resource r)
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	s.written.erase(r.handle);
	s.written_from.erase(r.handle);
	s.last_draw.erase(r.handle);
	s.windows.erase(std::remove(s.windows.begin(), s.windows.end(), r.handle), s.windows.end());
	if (s.composite == r.handle)
		s.composite = 0;
	if (s.window_source == r.handle)
		s.window_source = 0;
	if (s.rt0.handle == r.handle)
		s.rt0 = resource{};
	if (s.ps_t0.handle == r.handle)
		s.ps_t0 = resource{};
	if (s.dsv.handle == r.handle)
		s.dsv = resource{};
}

}

void register_game_render_scale(GameScaleHandler handler)
{
	{
		State &s = st();
		std::lock_guard<std::mutex> g(s.lock);
		s.handler = std::move(handler);
	}
	if (g_registered)
		return;
	g_registered = true;
	reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
	reshade::register_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
	reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
	reshade::register_event<reshade::addon_event::draw>(on_draw);
	reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
	reshade::register_event<reshade::addon_event::present>(on_present);
	reshade::register_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
	reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
	reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
	reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
}

void unregister_game_render_scale()
{
	if (!g_registered)
		return;
	g_registered = false;
	g_enabled.store(false);
	reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
	reshade::unregister_event<reshade::addon_event::bind_viewports>(on_bind_viewports);
	reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
	reshade::unregister_event<reshade::addon_event::draw>(on_draw);
	reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
	reshade::unregister_event<reshade::addon_event::present>(on_present);
	reshade::unregister_event<reshade::addon_event::destroy_resource>(on_destroy_resource);
	reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
	reshade::unregister_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
	reshade::unregister_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	s.handler = nullptr;
	s.timing.release();
}

void set_game_render_scale_enabled(bool on) noexcept
{
	g_enabled.store(on, std::memory_order_relaxed);
}

void set_game_render_scale_full(bool on) noexcept
{
	g_full.store(on, std::memory_order_relaxed);
}

GameScaleState game_render_scale_state() noexcept
{
	State &s = st();
	std::lock_guard<std::mutex> g(s.lock);
	return s.out;
}

GameScaleVerdict game_scale_verdict(bool on, bool api_ok, const GameScaleState &g, const GameScaleWatch &since) noexcept
{
	if (!api_ok)
		return GameScaleVerdict::WrongApi;
	if (!on)
		return GameScaleVerdict::Off;
	const bool found = g.replaced != 0 && g.found_frame >= since.presents;
	if (found)
		return g.found_frame + kGameScaleRecentPresents >= g.presents ? GameScaleVerdict::Working : GameScaleVerdict::Idle;
	if (g.presents < since.presents + kGameScaleVerdictPresents)
		return GameScaleVerdict::Looking;
	return g.missed_corner > since.missed_corner ? GameScaleVerdict::OtherCorner : GameScaleVerdict::NotFound;
}

}
