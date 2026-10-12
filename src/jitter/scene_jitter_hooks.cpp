#include "aeon_sr/jitter/scene_jitter_hooks.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/jitter/constant_reads.hpp"
#include "aeon_sr/jitter/depth_regrid.hpp"
#include "aeon_sr/jitter/gl_jitter.hpp"
#include "aeon_sr/jitter/scene_jitter_shaders.hpp"
#include "aeon_sr/jitter/scene_snapshot.hpp"
#include "aeon_sr/jitter/shader_position.hpp"
#include "aeon_sr/jitter/vulkan_viewport_hook.hpp"

#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

namespace aeon_sr {
namespace {

using namespace reshade::api;

SceneJitter g_scene_jitter;

struct JitterCounted {
	std::mutex lock;
	JitterDrawCounts counts;
	std::atomic<uint32_t> all_draws{ 0 };
	void fold_all_draws() noexcept { counts.all_draws += all_draws.exchange(0, std::memory_order_relaxed); }
};

struct __declspec(uuid("3f0c9a7e-4d2b-4a61-9e55-8b1f6d2c7a40")) JitterContextData : JitterCounted {
	JitterListState state;
	JitterArm arm;
	uint64_t arm_version = ~0ull;
	uint64_t vk_stamp = 0;
	bool vk_hooked = false;
	static constexpr uint32_t kSizeSlots = 8u;
	uint64_t sized[kSizeSlots]{};
	uint32_t size_w[kSizeSlots]{}, size_h[kSizeSlots]{};
	uint32_t size_next = 0;
	uint64_t size_epoch = 0;
	bool bind_seen = false;
	uint64_t asked_epoch = ~0ull;
	uint64_t projection_serial = 0;
	uint32_t projection_tries = 0;
	uint64_t last_pass_target = 0;
	bool closed = false;
	struct RegridSlot {
		shader_stage stage = shader_stage::pixel;
		uint32_t slot = 0;
		ID3D11ShaderResourceView *original = nullptr;
	};
	static constexpr uint32_t kRegridSlots = 8u;
	RegridSlot regrid[kRegridSlots];
	uint32_t regrid_count = 0;
	int immediate = -1;
	bool depth_write = true, dsv_read_only = false;
	uint64_t vertex_pipe = 0, depth_pipe = 0;
	bool vertex_stale = false, depth_stale = false;
	uint64_t dyn_pipe = ~0ull;
	bool dyn_depth = false;
	int8_t dyn_enable = -1, dyn_write = -1, dyn_op = -1, dyn_compares = -1;

	~JitterContextData();
};

struct __declspec(uuid("9b6a2f41-5c3e-4d7a-8f10-2e4b6c8d1a93")) JitterCommandListData : JitterCounted {
	bool played = false;
	uint64_t first_played = 0;
};

static_assert(sizeof(JitterViewport) == sizeof(viewport), "JitterViewport has to stay reshade::api::viewport");
static_assert(sizeof(JitterViewport) == sizeof(D3D11_VIEWPORT), "JitterViewport has to stay D3D11_VIEWPORT");

enum class Route { None, Viewport, Gl, Shader };

Route route_of(command_list *cmd)
{
	if (cmd == nullptr)
		return Route::None;
	switch (cmd->get_device()->get_api()) {
	case device_api::d3d11:
	case device_api::d3d12:
	case device_api::vulkan:
		return Route::Viewport;
	case device_api::opengl:
		return !gl_jitter_own_call() && gl_jitter_usable() ? Route::Gl : Route::None;
	case device_api::d3d9:
	case device_api::d3d10:
		return Route::Shader;
	default:
		return Route::None;
	}
}

bool followed(command_list *cmd)
{
	return route_of(cmd) != Route::None;
}

bool is_d3d11(command_list *cmd)
{
	return cmd != nullptr && cmd->get_device()->get_api() == device_api::d3d11;
}

bool is_vulkan(command_list *cmd)
{
	return cmd != nullptr && cmd->get_device()->get_api() == device_api::vulkan;
}

void *native_of(command_list *cmd)
{
	return reinterpret_cast<void *>(static_cast<uintptr_t>(cmd->get_native()));
}

bool is_command_list(command_list *cmd)
{
	if (!is_d3d11(cmd))
		return false;
	auto *const native = reinterpret_cast<IUnknown *>(static_cast<uintptr_t>(cmd->get_native()));
	ID3D11CommandList *list = nullptr;
	if (native == nullptr || FAILED(native->QueryInterface(IID_PPV_ARGS(&list))))
		return false;
	list->Release();
	return true;
}

void depth_defaults(command_list *cmd, JitterListState &s);

JitterContextData &context_of(command_list *cmd)
{
	JitterContextData *data = cmd->get_private_data<JitterContextData>();
	if (data == nullptr) {
		data = cmd->create_private_data<JitterContextData>();
		depth_defaults(cmd, data->state);
	}
	return *data;
}

const JitterArm &arm_of(command_list *cmd, JitterContextData &d)
{
	const uint64_t version = g_scene_jitter.version();
	if (d.arm_version != version) {
		d.arm = g_scene_jitter.current_for(jitter_list_key(cmd));
		d.arm_version = version;
	}
	return d.arm;
}

uint64_t resource_of(command_list *cmd, resource_view view)
{
	if (view.handle == 0)
		return 0;
	return cmd->get_device()->get_resource_from_view(view).handle;
}

std::mutex g_regrid_lock;
DepthRegridD3D11 &regrid_copy()
{
	static DepthRegridD3D11 *const copy = new DepthRegridD3D11;
	return *copy;
}
std::atomic<uint64_t> g_depth_writes{ 0 };
std::atomic<float> g_depth_dx{ 0.0f }, g_depth_dy{ 0.0f };
std::atomic<bool> g_depth_deferred{ false };
std::atomic<uint32_t> g_regrid_live{ 0 };
uint64_t g_regrid_at = ~0ull;
float g_regrid_dx = 0.0f, g_regrid_dy = 0.0f;
ID3D11ShaderResourceView *g_regrid_source = nullptr;

void regrid_note(const wchar_t *text)
{
	diag_state("depth_regrid", DiagLevel::Info, "jitter", std::wstring(L"depth regrid: ") + text);
}

void bind_srv(ID3D11DeviceContext *ctx, shader_stage stage, uint32_t slot, ID3D11ShaderResourceView *view)
{
	switch (stage) {
	case shader_stage::vertex: ctx->VSSetShaderResources(slot, 1, &view); break;
	case shader_stage::hull: ctx->HSSetShaderResources(slot, 1, &view); break;
	case shader_stage::domain: ctx->DSSetShaderResources(slot, 1, &view); break;
	case shader_stage::geometry: ctx->GSSetShaderResources(slot, 1, &view); break;
	case shader_stage::pixel: ctx->PSSetShaderResources(slot, 1, &view); break;
	case shader_stage::compute: ctx->CSSetShaderResources(slot, 1, &view); break;
	default: break;
	}
}

bool single_stage(shader_stage s)
{
	return s == shader_stage::vertex || s == shader_stage::hull || s == shader_stage::domain ||
		s == shader_stage::geometry || s == shader_stage::pixel || s == shader_stage::compute;
}

bool immediate_context(command_list *cmd, JitterContextData &d)
{
	if (d.immediate < 0) {
		auto *const ctx = static_cast<ID3D11DeviceContext *>(native_of(cmd));
		d.immediate = ctx != nullptr && ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? 1 : 0;
	}
	return d.immediate == 1;
}

void regrid_drop(command_list *cmd, JitterContextData &d, bool rebind)
{
	auto *const ctx = static_cast<ID3D11DeviceContext *>(native_of(cmd));
	for (uint32_t i = 0; i < d.regrid_count; ++i) {
		JitterContextData::RegridSlot &e = d.regrid[i];
		if (rebind && ctx != nullptr)
			bind_srv(ctx, e.stage, e.slot, e.original);
		if (e.original != nullptr)
			e.original->Release();
		e = JitterContextData::RegridSlot{};
	}
	g_regrid_live.fetch_sub(d.regrid_count, std::memory_order_relaxed);
	d.regrid_count = 0;
}

void regrid_forget(JitterContextData &d, shader_stage stage, uint32_t first, uint32_t count)
{
	for (uint32_t i = 0; i < d.regrid_count;) {
		JitterContextData::RegridSlot &e = d.regrid[i];
		if (e.stage == stage && e.slot >= first && e.slot < first + count) {
			if (e.original != nullptr)
				e.original->Release();
			e = d.regrid[--d.regrid_count];
			d.regrid[d.regrid_count] = JitterContextData::RegridSlot{};
			g_regrid_live.fetch_sub(1, std::memory_order_relaxed);
		} else {
			++i;
		}
	}
}

bool scene_depth_readable(ID3D11DeviceContext *ctx, uint64_t scene_depth)
{
	ID3D11DepthStencilView *dsv = nullptr;
	ctx->OMGetRenderTargets(0, nullptr, &dsv);
	if (dsv == nullptr)
		return true;
	ID3D11Resource *res = nullptr;
	dsv->GetResource(&res);
	D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
	dsv->GetDesc(&dd);
	dsv->Release();
	const bool same = reinterpret_cast<uintptr_t>(res) == static_cast<uintptr_t>(scene_depth);
	if (res != nullptr)
		res->Release();
	return !same || (dd.Flags & D3D11_DSV_READ_ONLY_DEPTH) != 0;
}

JitterContextData::~JitterContextData()
{
	for (uint32_t i = 0; i < regrid_count; ++i)
		if (regrid[i].original != nullptr)
			regrid[i].original->Release();
	g_regrid_live.fetch_sub(regrid_count, std::memory_order_relaxed);
}

void note_depth_written(command_list *cmd, uint64_t resource)
{
	if (resource == 0 || !is_d3d11(cmd))
		return;
	JitterContextData *const d = cmd->get_private_data<JitterContextData>();
	if (d == nullptr || resource != arm_of(cmd, *d).scene_depth)
		return;
	if (immediate_context(cmd, *d))
		g_depth_writes.fetch_add(1, std::memory_order_relaxed);
	else if (arm_of(cmd, *d).depth_regrid)
		g_depth_deferred.store(true, std::memory_order_relaxed);
}

void regrid_bound(command_list *cmd, shader_stage stage, uint32_t first, uint32_t count, const resource_view *views)
{
	JitterContextData *const dp = cmd->get_private_data<JitterContextData>();
	if (dp == nullptr)
		return;
	JitterContextData &d = *dp;
	if (d.regrid_count != 0)
		regrid_forget(d, stage, first, count);
	if (views == nullptr || !single_stage(stage))
		return;
	const JitterArm &arm = arm_of(cmd, d);
	if (!arm.depth_regrid || arm.serial == 0 || arm.scene_depth == 0)
		return;
	const float dx = g_depth_dx.load(std::memory_order_relaxed), dy = g_depth_dy.load(std::memory_order_relaxed);
	if (dx == 0.0f && dy == 0.0f)
		return;
	for (uint32_t i = 0; i < count; ++i) {
		if (views[i].handle == 0)
			continue;
		auto *const view = reinterpret_cast<ID3D11ShaderResourceView *>(static_cast<uintptr_t>(views[i].handle));
		D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
		view->GetDesc(&vd);
		if (vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || !depth_regrid_reads_depth(vd.Format) ||
			resource_of(cmd, views[i]) != arm.scene_depth)
			continue;
		auto *const ctx = static_cast<ID3D11DeviceContext *>(native_of(cmd));
		if (ctx == nullptr)
			return;
		if (!immediate_context(cmd, d)) {
			regrid_note(L"the game reads its depth on a deferred context, which is left as drawn");
			return;
		}
		if (g_depth_deferred.load(std::memory_order_relaxed)) {
			regrid_note(L"the game writes its depth on a deferred context; left as drawn");
			return;
		}
		if (!scene_depth_readable(ctx, arm.scene_depth)) {
			regrid_note(L"the game reads its depth with it still bound for writing; left as drawn");
			continue;
		}
		const resource_desc rd = cmd->get_device()->get_resource_desc(resource{ arm.scene_depth });
		if (rd.texture.width != arm.scene_w || rd.texture.height != arm.scene_h)
			continue;
		ID3D11ShaderResourceView *copy = nullptr;
		{
			const std::lock_guard<std::mutex> lock(g_regrid_lock);
			DepthRegridD3D11 &rg = regrid_copy();
			const uint64_t written = g_depth_writes.load(std::memory_order_relaxed);
			if (written != g_regrid_at || dx != g_regrid_dx || dy != g_regrid_dy || view != g_regrid_source ||
				rg.view() == nullptr) {
				copy = rg.regrid(ctx, view, dx, dy);
				g_regrid_at = copy != nullptr ? written : ~0ull;
				g_regrid_dx = dx;
				g_regrid_dy = dy;
				g_regrid_source = view;
				if (copy != nullptr) {
					for (uint32_t k = 0; k < d.regrid_count; ++k)
						bind_srv(ctx, d.regrid[k].stage, d.regrid[k].slot, copy);
				}
			} else {
				copy = rg.view();
			}
		}
		if (copy == nullptr) {
			regrid_drop(cmd, d, true);
			regrid_note(L"the game reads its depth through a view this does not take; left as drawn");
			return;
		}
		if (d.regrid_count == JitterContextData::kRegridSlots)
			return;
		const uint32_t slot = first + i;
		bind_srv(ctx, stage, slot, copy);
		JitterContextData::RegridSlot &e = d.regrid[d.regrid_count++];
		e.stage = stage;
		e.slot = slot;
		e.original = view;
		e.original->AddRef();
		g_regrid_live.fetch_add(1, std::memory_order_relaxed);
		regrid_note(L"the game reads its depth in its shaders; handed back on the grid while the scene is moved");
	}
}

struct D3D11Depth {
	bool tests = true;
	bool writes = true;
	bool enabled = true;
	bool compares = true;
};

D3D11Depth d3d11_depth(uint64_t state)
{
	D3D11Depth out;
	if (state == 0)
		return out;
	D3D11_DEPTH_STENCIL_DESC desc{};
	reinterpret_cast<ID3D11DepthStencilState *>(static_cast<uintptr_t>(state))->GetDesc(&desc);
	out.enabled = desc.DepthEnable != FALSE;
	out.tests = out.enabled &&
		!(desc.DepthFunc == D3D11_COMPARISON_ALWAYS && desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO);
	out.writes = out.enabled && desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
	out.compares = desc.DepthFunc != D3D11_COMPARISON_ALWAYS && desc.DepthFunc != D3D11_COMPARISON_NEVER;
	return out;
}

bool func_compares(compare_op op)
{
	return op != compare_op::always && op != compare_op::never;
}

void depth_defaults(command_list *cmd, JitterListState &s)
{
	switch (cmd->get_device()->get_api()) {
	case device_api::d3d10:
	case device_api::d3d11:
		jitter_on_depth_pipeline(s, true, 1, 1);
		break;
	case device_api::opengl:
		jitter_on_depth_pipeline(s, true, -1, -1);
		jitter_on_depth_dynamic(s, 0, 1);
		break;
	case device_api::d3d9:
		jitter_on_depth_pipeline(s, true, -1, -1);
		jitter_on_depth_dynamic(s, 1, 1);
		break;
	default:
		jitter_on_depth_pipeline(s, false, -1, -1);
		break;
	}
}

struct PipelineFacts {
	bool depth = false;
	int8_t enable = -1, compares = -1;
	int8_t write = -1, op = -1;
	JitterVertexClass vertex = JitterVertexClass::Unknown;
	bool wterms_known = false;
	DxbcWTerms wterms{};
};
std::shared_mutex g_facts_lock;
std::unordered_map<uint64_t, PipelineFacts> g_facts;
std::atomic<bool> g_any_facts{ false };

void note_pipeline_facts(uint64_t pipe, const PipelineFacts &f)
{
	if (pipe == 0)
		return;
	const bool any = f.depth || f.vertex != JitterVertexClass::Unknown || f.wterms_known;
	if (!any && !g_any_facts.load(std::memory_order_relaxed))
		return;
	const std::unique_lock<std::shared_mutex> lock(g_facts_lock);
	if (any) {
		g_facts[pipe] = f;
		g_any_facts.store(true, std::memory_order_relaxed);
	} else {
		g_facts.erase(pipe);
	}
}

PipelineFacts pipeline_facts(uint64_t pipe)
{
	if (pipe == 0 || !g_any_facts.load(std::memory_order_relaxed))
		return PipelineFacts{};
	const std::shared_lock<std::shared_mutex> lock(g_facts_lock);
	const auto it = g_facts.find(pipe);
	return it != g_facts.end() ? it->second : PipelineFacts{};
}

void depth_reset(command_list *cmd, JitterContextData &d)
{
	depth_defaults(cmd, d.state);
	d.vertex_stale = false;
	d.depth_stale = false;
}

void quad_facts(command_list *cmd, JitterContextData &d, bool vertex = true)
{
	d.state.perspective = false;
	d.state.projection = -1;
	if (!vertex) {
	} else if (d.vertex_pipe == 0 && cmd->get_device()->get_api() == device_api::opengl) {
		d.state.vertex_class = gl_fixed_function_vertex_class(&d.state.perspective);
		d.vertex_stale = false;
	} else if (d.vertex_stale) {
		d.state.vertex_class = pipeline_facts(d.vertex_pipe).vertex;
		d.vertex_stale = false;
	}
	if (d.depth_stale) {
		if (d.depth_pipe == 0 && cmd->get_device()->get_api() == device_api::d3d10) {
			jitter_on_depth_pipeline(d.state, true, 1, 1);
		} else {
			const PipelineFacts f = pipeline_facts(d.depth_pipe);
			jitter_on_depth_pipeline(d.state, f.depth, f.enable, f.compares);
		}
		d.depth_stale = false;
	}
}

int d3d11_projection(command_list *cmd, JitterContextData &d)
{
	const PipelineFacts f = pipeline_facts(d.vertex_pipe);
	if (!f.wterms_known || d3d11_vertex_position_replaced(cmd))
		return -1;
	struct Reader {
		command_list *cmd;
		uint64_t shader;
		uint64_t now;
	} reader{ cmd, d.vertex_pipe, g_scene_jitter.frame() };
	if (immediate_context(cmd, d))
		constant_reads_poll(cmd, reader.now);
	return dxbc_w_terms_perspective(
		f.wterms,
		[](void *user, uint32_t slot, uint32_t reg, float out[4]) {
			const Reader &r = *static_cast<const Reader *>(user);
			return d3d11_vertex_constant(r.cmd, r.shader, slot, reg, r.now, out);
		},
		&reader);
}

constexpr uint32_t kProjectionTries = 16u;
bool try_scene_projection(JitterContextData &d, uint64_t serial)
{
	if (serial == 0 || g_scene_jitter.scene_perspective(serial))
		return false;
	if (d.projection_serial != serial) {
		d.projection_serial = serial;
		d.projection_tries = 0;
	}
	return d.projection_tries++ < kProjectionTries;
}

std::shared_mutex g_untested_lock;
std::unordered_set<uint64_t> g_untested;
std::atomic<bool> g_any_untested{ false };

void note_untested_pipeline(uint64_t pipe, bool untested)
{
	if (pipe == 0)
		return;
	if (!untested && !g_any_untested.load(std::memory_order_relaxed))
		return;
	const std::unique_lock<std::shared_mutex> lock(g_untested_lock);
	if (untested) {
		g_untested.insert(pipe);
		g_any_untested.store(true, std::memory_order_relaxed);
	} else {
		g_untested.erase(pipe);
	}
}

bool untested_pipeline(uint64_t pipe)
{
	if (pipe == 0 || !g_any_untested.load(std::memory_order_relaxed))
		return false;
	const std::shared_lock<std::shared_mutex> lock(g_untested_lock);
	return g_untested.count(pipe) != 0;
}

void size_of(command_list *cmd, JitterContextData &d, uint64_t res, uint32_t *w, uint32_t *h)
{
	*w = 0;
	*h = 0;
	if (res == 0)
		return;
	const uint64_t epoch = g_scene_jitter.size_epoch();
	if (d.size_epoch != epoch) {
		for (uint64_t &r : d.sized)
			r = 0;
		d.size_epoch = epoch;
	}
	for (uint32_t i = 0; i < JitterContextData::kSizeSlots; ++i) {
		if (d.sized[i] == res) {
			*w = d.size_w[i];
			*h = d.size_h[i];
			return;
		}
	}
	const resource_desc desc = cmd->get_device()->get_resource_desc(resource{ res });
	if (desc.type == resource_type::texture_2d || desc.type == resource_type::surface) {
		*w = desc.texture.width;
		*h = desc.texture.height;
	}
	const uint32_t slot = d.size_next++ % JitterContextData::kSizeSlots;
	d.sized[slot] = res;
	d.size_w[slot] = *w;
	d.size_h[slot] = *h;
}

uint32_t bound_viewports(command_list *cmd, JitterViewport *out)
{
	auto *const ctx = reinterpret_cast<ID3D11DeviceContext *>(static_cast<uintptr_t>(cmd->get_native()));
	UINT n = 0;
	ctx->RSGetViewports(&n, nullptr);
	if (n > kJitterViewportSlots)
		n = kJitterViewportSlots;
	if (n != 0)
		ctx->RSGetViewports(&n, reinterpret_cast<D3D11_VIEWPORT *>(out));
	return n;
}

void apply(command_list *cmd, JitterContextData &d, bool held_before, JitterRebind &r)
{
	const Route route = route_of(cmd);
	if (r.count != 0 && route == Route::Gl && !gl_jitter_bound_as(r.expect_count, r.expect)) {
		JitterViewport now[kJitterViewportSlots];
		const uint32_t n = gl_jitter_read(r.expect_count, now);
		bool rounded = n == r.expect_count;
		for (uint32_t i = 0; rounded && i < n; ++i) {
			rounded = std::fabs(now[i].x - std::nearbyint(now[i].x)) < 1e-3f &&
				std::fabs(now[i].y - std::nearbyint(now[i].y)) < 1e-3f &&
				std::fabs(now[i].x - r.expect[i].x) <= 0.5f + 1e-3f &&
				std::fabs(now[i].y - r.expect[i].y) <= 0.5f + 1e-3f &&
				std::fabs(now[i].width - r.expect[i].width) < 1e-3f &&
				std::fabs(now[i].height - r.expect[i].height) < 1e-3f;
		}
		if (!rounded) {
			g_scene_jitter.hold(jitter_list_key(cmd));
			JitterOffsets known;
			g_scene_jitter.known_offsets(&known);
			r = JitterRebind{};
			jitter_on_viewports(d.state, arm_of(cmd, d), n, now, &known, &r);
		}
	}
	if (r.count != 0 && is_d3d11(cmd)) {
		JitterViewport now[kJitterViewportSlots];
		const uint32_t n = bound_viewports(cmd, now);
		if (!jitter_rebind_expected(r, n, now)) {
			g_scene_jitter.hold(jitter_list_key(cmd));
			JitterOffsets known;
			g_scene_jitter.known_offsets(&known);
			r = JitterRebind{};
			jitter_on_viewports(d.state, arm_of(cmd, d), n, now, &known, &r);
		}
	}
	if (r.count != 0) {
		if (route == Route::Gl) {
			gl_jitter_bind(r.count, r.vp);
		} else if (route == Route::Viewport) {
			const bool hooked = d.vk_hooked && is_vulkan(cmd) &&
				vulkan_viewport_hook().set(native_of(cmd), 0u, r.count, r.vp);
			if (!hooked)
				cmd->bind_viewports(0, r.count, reinterpret_cast<const viewport *>(r.vp));
		}
	}
	g_scene_jitter.note_bound(held_before, d.state.bound_serial != 0);
}

void bind_target_and_depth(command_list *cmd, JitterContextData &d, uint64_t target, uint64_t depth)
{
	uint32_t tw = 0, th = 0, dw = 0, dh = 0;
	bool target_scene = false, window = false;
	const JitterArm &arm = arm_of(cmd, d);
	if (g_scene_jitter.engaged()) {
		size_of(cmd, d, target, &tw, &th);
		size_of(cmd, d, depth, &dw, &dh);
		if (depth != 0 && jitter_frame_like(dw, dh, arm.display_w, arm.display_h))
			g_scene_jitter.note_frame_depth();
		window = g_scene_jitter.presented_target(target);
		const bool scene_depth = depth != 0 && arm.scene_w != 0 &&
			(arm.scene_depth != 0 ? depth == arm.scene_depth : dw == arm.scene_w && dh == arm.scene_h);
		if (scene_depth && target != 0 && tw == arm.scene_w && th == arm.scene_h && !window &&
			!jitter_arm_into_back_buffer(arm, target))
			g_scene_jitter.note_scene_target(target, tw, th);
		target_scene = target != 0 && !window && g_scene_jitter.scene_target(target, tw, th);
	}
	jitter_on_target(d.state, target, tw, th, target_scene, window);
	const bool held = d.state.bound_serial != 0;
	JitterRebind r;
	jitter_on_depth(d.state, arm, depth, &r, dw, dh);
	apply(cmd, d, held, r);
}

void take_hooked_viewports(command_list *cmd, JitterContextData &d)
{
	if (!is_vulkan(cmd) || !vulkan_viewport_hook().installed())
		return;
	void *const native = native_of(cmd);
	const uint64_t stamp = vulkan_viewport_hook().stamp_of(native);
	if (stamp == 0u || stamp == d.vk_stamp)
		return;
	d.vk_stamp = stamp;
	d.vk_hooked = true;
	JitterViewport seen[kJitterViewportSlots];
	const uint32_t n = vulkan_viewport_hook().viewports_of(native, seen, kJitterViewportSlots);
	if (n == 0u)
		return;
	const bool held = d.state.bound_serial != 0;
	JitterRebind r;
	JitterOffsets known;
	g_scene_jitter.known_offsets(&known);
	jitter_on_viewports(d.state, arm_of(cmd, d), n, seen, &known, &r);
	apply(cmd, d, held, r);
}

void take_hooked_depth(command_list *cmd, JitterContextData &d)
{
	if (!is_vulkan(cmd) || d.depth_pipe == 0 || !vulkan_viewport_hook().depth_hooked())
		return;
	if (d.dyn_pipe != d.depth_pipe) {
		const PipelineFacts f = pipeline_facts(d.depth_pipe);
		d.dyn_pipe = d.depth_pipe;
		d.dyn_depth = f.depth;
		d.dyn_enable = f.enable;
		d.dyn_write = f.write;
		d.dyn_op = f.op;
		d.dyn_compares = f.compares;
	}
	if (!d.dyn_depth || (d.dyn_enable >= 0 && d.dyn_op >= 0 && d.dyn_write >= 0))
		return;
	int en = -1, wr = -1, op = -1;
	if (!vulkan_viewport_hook().depth_of(native_of(cmd), &en, &wr, &op))
		return;
	const int enable = d.dyn_enable >= 0 ? d.dyn_enable : en;
	const int write = d.dyn_write >= 0 ? d.dyn_write : wr;
	const int func = d.dyn_op >= 0 ? d.dyn_op : op;
	jitter_on_depth_dynamic(d.state, d.dyn_enable < 0 ? en : -1,
		d.dyn_compares < 0 && op >= 0 ? (func_compares(static_cast<compare_op>(op)) ? 1 : 0) : -1);
	if (enable == 0)
		jitter_on_depth_test(d.state, false);
	else if (enable == 1)
		jitter_on_depth_test(d.state, !(func == static_cast<int>(compare_op::always) && write == 0));
}

void count_other(JitterContextData &d, const JitterArm &arm, bool fullscreen)
{
	if (!fullscreen && arm.scene_depth != 0 && d.state.depth != 0 && d.state.depth != arm.scene_depth) {
		const std::lock_guard<std::mutex> lock(d.lock);
		count_other_depth_draw(d.counts, d.state.depth_w, d.state.depth_h);
	}
	if (fullscreen || !jitter_scene_by_size(arm))
		return;
	uint32_t w = 0, h = 0;
	jitter_draw_size(d.state, &w, &h);
	if (!jitter_frame_like(w, h, arm.display_w, arm.display_h))
		return;
	const std::lock_guard<std::mutex> lock(d.lock);
	count_other_draw(d.counts, w, h);
}

void shader_draw(command_list *cmd, bool fullscreen)
{
	JitterContextData &d = context_of(cmd);
	if (!d.bind_seen && g_scene_jitter.engaged() && d.asked_epoch != g_scene_jitter.size_epoch()) {
		d.asked_epoch = g_scene_jitter.size_epoch();
		uint64_t target = 0, depth = 0;
		if (jitter_d3d9_bound(cmd, &target, &depth))
			bind_target_and_depth(cmd, d, resource_of(cmd, resource_view{ target }), resource_of(cmd, resource_view{ depth }));
	}
	bool scene = false;
	bool over_scene = false;
	bool window_quad = false;
	if (g_scene_jitter.engaged())
		d.all_draws.fetch_add(1u, std::memory_order_relaxed);
	if (g_scene_jitter.engaged() && (g_scene_jitter.scene_known() || d.state.bound_serial != 0)) {
		const bool held = d.state.bound_serial != 0;
		JitterRebind r;
		d.state.scene_tested = g_scene_jitter.scene_tested(arm_of(cmd, d).serial);
		const bool into_window = jitter_draw_into_window(d.state, arm_of(cmd, d));
		const bool depth_bound = fullscreen && jitter_scene_depth_bound(d.state, arm_of(cmd, d));
		over_scene = depth_bound && !into_window;
		bool quad_scene = false;
		if (depth_bound && (into_window || arm_of(cmd, d).scene_quads)) {
			quad_facts(cmd, d);
			bool enabled = false, compares = false;
			if (jitter_d3d9_depth_state(cmd, &enabled, &compares))
				jitter_on_depth_dynamic(d.state, enabled ? 1 : 0, compares ? 1 : 0);
			if (over_scene) {
				d.state.vertex_class = jitter_shader_vertex_class(cmd);
				if (d.state.vertex_class == JitterVertexClass::Transformed) {
					d.state.scene_perspective = g_scene_jitter.scene_perspective(arm_of(cmd, d).serial);
					if (d.state.scene_perspective)
						d.state.projection = static_cast<int8_t>(jitter_shader_projection(cmd));
				}
				quad_scene = jitter_quad_is_scene(d.state, arm_of(cmd, d));
			} else {
				bool known = false;
				window_quad = jitter_depth_compares(d.state, &known) && known;
				d.state.perspective = window_quad && arm_of(cmd, d).scene_quads && jitter_shader_perspective(cmd);
				quad_scene = jitter_window_quad_is_scene(d.state, arm_of(cmd, d));
			}
		}
		scene = jitter_on_draw(d.state, arm_of(cmd, d), fullscreen && !quad_scene, &r);
		apply(cmd, d, held, r);
		if (!scene)
			count_other(d, arm_of(cmd, d), fullscreen);
	}
	const JitterArm &arm = arm_of(cmd, d);
	const bool want = d.state.bound_serial != 0 && d.state.app_count != 0;
	const JitterViewport &vp = d.state.app[0];
	const JitterShaderDraw carried = jitter_shader_draw(cmd, want, arm.target_x, arm.target_y, vp.width, vp.height,
		arm.scene_depth == 0, d.state.target == 0 || jitter_draw_into_window(d.state, arm));
	if (scene && carried != JitterShaderDraw::NotADraw && carried != JitterShaderDraw::Interface) {
		const bool by_target = d.state.target_scene && d.state.depth == 0;
		const bool by_depth = jitter_scene_depth_bound(d.state, arm);
		if (by_depth && d.state.depth_test)
			g_scene_jitter.note_scene_tested(arm.serial);
		if (!fullscreen && by_depth && d.state.depth_test && try_scene_projection(d, arm.serial) &&
			jitter_shader_projection(cmd) == 1)
			g_scene_jitter.note_scene_perspective(arm.serial);
		const std::lock_guard<std::mutex> lock(d.lock);
		count_scene_draw(d.counts, carried == JitterShaderDraw::Offset ? d.state.bound_serial : 0u,
			d.state.depth_aliased);
		if (by_target)
			++d.counts.by_target;
		if (by_depth)
			++d.counts.depth_draws;
		if (!jitter_draw_into_window(d.state, arm))
			++d.counts.outside_draws;
		if ((over_scene || window_quad) && carried == JitterShaderDraw::Offset)
			++d.counts.quads_moved;
	} else if (scene && carried == JitterShaderDraw::Interface) {
		const std::lock_guard<std::mutex> lock(d.lock);
		++d.counts.interface_kept;
		if (!jitter_draw_into_window(d.state, arm))
			++d.counts.interface_kept_outside;
	} else if ((over_scene || window_quad) && !scene) {
		const std::lock_guard<std::mutex> lock(d.lock);
		++d.counts.quads_kept;
	}
}

void note_window_draw(command_list *cmd, JitterContextData &d, bool scene, bool fullscreen)
{
	const JitterArm &arm = arm_of(cmd, d);
	if (d.state.target == 0 || arm.serial == 0)
		return;
	const bool window = jitter_draw_into_window(d.state, arm);
	scene_snapshot().count(window, scene, fullscreen, d.state.depth_test && jitter_scene_depth_bound(d.state, arm),
		d.state.target_w, d.state.target_h);
	if (!window)
		return;
	const resource target{ d.state.target };
	if (scene) {
		scene_snapshot().spoil(arm.serial, target, fullscreen);
		scene_snapshot().note_scene(arm.serial, target);
	} else if (!fullscreen || scene_snapshot().scene_drawn_into(arm.serial, target)) {
		scene_snapshot().before_interface(cmd, target, arm.serial);
	}
}

void any_draw(command_list *cmd, bool fullscreen)
{
	if (g_regrid_live.load(std::memory_order_relaxed) != 0) {
		if (JitterContextData *const rd = cmd->get_private_data<JitterContextData>(); rd != nullptr && rd->regrid_count != 0) {
			const JitterArm &a = arm_of(cmd, *rd);
			if (a.serial == 0 || !a.depth_regrid)
				regrid_drop(cmd, *rd, true);
		}
	}
	const Route route = route_of(cmd);
	if (route == Route::Shader) {
		shader_draw(cmd, fullscreen);
		return;
	}
	if (route == Route::Gl)
		gl_jitter_observe(g_scene_jitter.frame());
	if (!g_scene_jitter.engaged() || route == Route::None)
		return;
	JitterContextData &d = context_of(cmd);
	d.all_draws.fetch_add(1u, std::memory_order_relaxed);
	take_hooked_viewports(cmd, d);
	take_hooked_depth(cmd, d);
	if (!g_scene_jitter.scene_known() && d.state.bound_serial == 0)
		return;
	const bool held = d.state.bound_serial != 0;
	JitterRebind r;
	d.state.scene_tested = g_scene_jitter.scene_tested(arm_of(cmd, d).serial);
	const bool quad = fullscreen;
	const bool into_window = d.state.target != 0 && arm_of(cmd, d).serial != 0 &&
		jitter_draw_into_window(d.state, arm_of(cmd, d));
	const uint32_t index =
		into_window ? scene_snapshot().window_draw_index(arm_of(cmd, d).serial, resource{ d.state.target }) : 0u;
	if (fullscreen && into_window && arm_of(cmd, d).tested_quads && d.state.depth_test &&
		jitter_scene_depth_bound(d.state, arm_of(cmd, d)) && index < scene_snapshot().last_mesh_before() &&
		scene_snapshot().scene_drawn_into(arm_of(cmd, d).serial, resource{ d.state.target }))
		fullscreen = false;
	const bool over_scene = quad && !jitter_draw_into_window(d.state, arm_of(cmd, d)) &&
		jitter_scene_depth_bound(d.state, arm_of(cmd, d));
	if (fullscreen && over_scene && arm_of(cmd, d).scene_quads) {
		quad_facts(cmd, d);
		if (is_d3d11(cmd) && d.state.vertex_class == JitterVertexClass::Transformed) {
			d.state.scene_perspective = g_scene_jitter.scene_perspective(arm_of(cmd, d).serial);
			if (d.state.scene_perspective)
				d.state.projection = static_cast<int8_t>(d3d11_projection(cmd, d));
		}
		if (jitter_quad_is_scene(d.state, arm_of(cmd, d)))
			fullscreen = false;
	}
	bool window_quad = false;
	if (quad && jitter_draw_into_window(d.state, arm_of(cmd, d)) && jitter_scene_depth_bound(d.state, arm_of(cmd, d))) {
		quad_facts(cmd, d, false);
		bool known = false;
		window_quad = jitter_depth_compares(d.state, &known) && known;
		if (window_quad && fullscreen && arm_of(cmd, d).scene_quads && is_d3d11(cmd)) {
			d.state.perspective = d3d11_projection(cmd, d) == 1;
			if (jitter_window_quad_is_scene(d.state, arm_of(cmd, d)))
				fullscreen = false;
		}
	}
	const bool scene = jitter_on_draw(d.state, arm_of(cmd, d), fullscreen, &r);
	if (scene && into_window && !quad)
		scene_snapshot().note_scene_mesh(index);
	apply(cmd, d, held, r);
	if (arm_of(cmd, d).depth_regrid && d.state.depth != 0 && d.state.depth == arm_of(cmd, d).scene_depth &&
		d.depth_write && !d.dsv_read_only && is_d3d11(cmd)) {
		if (immediate_context(cmd, d)) {
			g_depth_writes.fetch_add(1, std::memory_order_relaxed);
			if (scene && d.state.bound_serial != 0) {
				g_depth_dx.store(arm_of(cmd, d).target_x, std::memory_order_relaxed);
				g_depth_dy.store(arm_of(cmd, d).target_y, std::memory_order_relaxed);
			}
		} else {
			g_depth_deferred.store(true, std::memory_order_relaxed);
		}
	}
	note_window_draw(cmd, d, scene, fullscreen);
	if (scene) {
		const bool by_target = d.state.target_scene && d.state.depth == 0;
		const bool by_depth = jitter_scene_depth_bound(d.state, arm_of(cmd, d));
		if (by_depth && d.state.depth_test)
			g_scene_jitter.note_scene_tested(arm_of(cmd, d).serial);
		if (!quad && by_depth && d.state.depth_test && is_d3d11(cmd) &&
			try_scene_projection(d, arm_of(cmd, d).serial) && d3d11_projection(cmd, d) == 1)
			g_scene_jitter.note_scene_perspective(arm_of(cmd, d).serial);
		const std::lock_guard<std::mutex> lock(d.lock);
		count_scene_draw(d.counts, d.state.bound_serial, d.state.depth_aliased);
		if (by_target)
			++d.counts.by_target;
		if (by_depth)
			++d.counts.depth_draws;
		if (!jitter_draw_into_window(d.state, arm_of(cmd, d)))
			++d.counts.outside_draws;
		if ((over_scene || window_quad) && d.state.bound_serial != 0)
			++d.counts.quads_moved;
	} else {
		count_other(d, arm_of(cmd, d), fullscreen);
		if (over_scene || window_quad) {
			const std::lock_guard<std::mutex> lock(d.lock);
			++d.counts.quads_kept;
		}
	}
}

void hand_on_counts(command_list *cmd)
{
	if (cmd == nullptr)
		return;
	JitterContextData *const data = cmd->get_private_data<JitterContextData>();
	if (data == nullptr)
		return;
	JitterDrawCounts counts;
	{
		const std::lock_guard<std::mutex> lock(data->lock);
		data->fold_all_draws();
		if (!any_draw_counts(data->counts))
			return;
		counts = data->counts;
		data->counts = JitterDrawCounts{};
	}
	g_scene_jitter.hand_on(counts);
}

command_list *immediate_of(effect_runtime *runtime)
{
	if (runtime == nullptr)
		return nullptr;
	command_queue *const queue = runtime->get_command_queue();
	return queue != nullptr ? queue->get_immediate_command_list() : nullptr;
}

}

namespace jitter_events {

using namespace reshade::api;

void init(command_list *cmd)
{
	if (cmd == nullptr)
		return;
	if (!followed(cmd))
		return;
	g_scene_jitter.release(jitter_list_key(cmd));
	if (is_command_list(cmd)) {
		if (cmd->get_private_data<JitterCommandListData>() == nullptr)
			cmd->create_private_data<JitterCommandListData>();
	} else if (cmd->get_private_data<JitterContextData>() == nullptr) {
		depth_defaults(cmd, cmd->create_private_data<JitterContextData>()->state);
	}
}

void bind_viewports(command_list *cmd, uint32_t first, uint32_t count, const viewport *viewports)
{
	if (cmd == nullptr)
		return;
	if (first != 0 || !followed(cmd))
		return;
	if (count != 0)
		g_scene_jitter.note_viewports(reinterpret_cast<uintptr_t>(cmd->get_device()));
	JitterContextData &d = context_of(cmd);
	const auto *const incoming = reinterpret_cast<const JitterViewport *>(viewports);
	const bool held = d.state.bound_serial != 0;
	JitterRebind r;
	if (jitter_viewports_fractional(count, incoming)) {
		JitterOffsets known;
		g_scene_jitter.known_offsets(&known);
		jitter_on_viewports(d.state, arm_of(cmd, d), count, incoming, &known, &r);
	} else {
		jitter_on_viewports(d.state, arm_of(cmd, d), count, incoming, nullptr, &r);
	}
	apply(cmd, d, held, r);
}

void push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t,
	const descriptor_table_update &update)
{
	if (cmd == nullptr)
		return;
	if ((update.type != descriptor_type::shader_resource_view &&
			update.type != descriptor_type::texture_shader_resource_view) || !is_d3d11(cmd))
		return;
	regrid_bound(cmd, stages, update.binding, update.count, static_cast<const resource_view *>(update.descriptors));
}

bool clear_depth_stencil(command_list *cmd, resource_view dsv, const float *depth, const uint8_t *, uint32_t,
	const rect *)
{
	if (cmd == nullptr)
		return false;
	if (depth != nullptr && dsv.handle != 0 && is_d3d11(cmd))
		note_depth_written(cmd, resource_of(cmd, dsv));
	if (depth != nullptr && dsv.handle != 0 && g_scene_jitter.engaged()) {
		JitterContextData &d = context_of(cmd);
		const JitterArm &a = arm_of(cmd, d);
		const uint64_t cleared = resource_of(cmd, dsv);
		if (a.serial != 0 && cleared != 0 && (cleared == a.scene_depth || cleared == d.state.depth) &&
				g_scene_jitter.scene_tested(a.serial))
			d.state.depth_cleared_serial = a.serial;
	}
	return false;
}

bool copy_resource(command_list *cmd, resource, resource dest)
{
	if (cmd == nullptr)
		return false;
	note_depth_written(cmd, dest.handle);
	return false;
}

bool copy_texture_region(command_list *cmd, resource, uint32_t, const subresource_box *, resource dest, uint32_t,
	const subresource_box *, filter_mode)
{
	if (cmd == nullptr)
		return false;
	note_depth_written(cmd, dest.handle);
	return false;
}

void destroy_device(device *dev)
{
	if (dev != nullptr && (dev->get_api() == device_api::d3d10 || dev->get_api() == device_api::d3d11))
		constant_reads_forget(dev->get_native());
	if (dev == nullptr || dev->get_api() != device_api::d3d11)
		return;
	const std::lock_guard<std::mutex> lock(g_regrid_lock);
	if (regrid_copy().device() == reinterpret_cast<ID3D11Device *>(static_cast<uintptr_t>(dev->get_native()))) {
		regrid_copy().release();
		g_regrid_at = ~0ull;
		g_regrid_source = nullptr;
	}
}

void bind_render_targets(command_list *cmd, uint32_t count, const resource_view *rtvs, resource_view dsv)
{
	if (cmd == nullptr)
		return;
	if (!followed(cmd))
		return;
	JitterContextData &d = context_of(cmd);
	d.bind_seen = true;
	d.dsv_read_only = false;
	if (dsv.handle != 0 && is_d3d11(cmd) && arm_of(cmd, d).depth_regrid &&
		resource_of(cmd, dsv) == arm_of(cmd, d).scene_depth) {
		D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
		reinterpret_cast<ID3D11DepthStencilView *>(static_cast<uintptr_t>(dsv.handle))->GetDesc(&dd);
		d.dsv_read_only = (dd.Flags & D3D11_DSV_READ_ONLY_DEPTH) != 0;
		if (d.regrid_count != 0 && !d.dsv_read_only)
			regrid_drop(cmd, d, true);
	}
	bind_target_and_depth(cmd, d, resource_of(cmd, count != 0 && rtvs != nullptr ? rtvs[0] : resource_view{ 0 }),
		resource_of(cmd, dsv));
}

bool begin_render_pass(command_list *cmd, uint32_t count, const render_pass_render_target_desc *rts,
	const render_pass_depth_stencil_desc *ds, render_pass_flags)
{
	if (cmd == nullptr)
		return false;
	if (!followed(cmd) || vulkan_restarting_pass())
		return false;
	JitterContextData &d = context_of(cmd);
	const uint64_t target = resource_of(cmd, count != 0 && rts != nullptr ? rts[0].view : resource_view{ 0 });
	const uint64_t depth = resource_of(cmd, ds != nullptr ? ds->view : resource_view{ 0 });
	g_scene_jitter.note_render_pass(target, depth != 0);
	d.bind_seen = true;
	if (target != 0 && g_scene_jitter.engaged()) {
		if (g_scene_jitter.swapchain_image(target)) {
			if (depth == 0) {
				if (d.last_pass_target != 0)
					g_scene_jitter.note_presented(d.last_pass_target);
				else
					g_scene_jitter.note_presentation();
				if (is_vulkan(cmd))
					scene_snapshot().end_frame_in_stream(resource{ target });
			}
		} else {
			d.last_pass_target = target;
			g_scene_jitter.note_pass_target(target);
		}
	}
	bind_target_and_depth(cmd, d, target, depth);
	return false;
}

bool end_render_pass(command_list *cmd)
{
	if (cmd == nullptr)
		return false;
	if (!followed(cmd) || vulkan_restarting_pass())
		return false;
	if (is_vulkan(cmd))
		vulkan_rendering_ended(native_of(cmd));
	bind_target_and_depth(cmd, context_of(cmd), 0, 0);
	return false;
}

void bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline state)
{
	if (cmd == nullptr)
		return;
	const bool cleared = stages == pipeline_stage::all && state.handle == 0;
	const bool depth_stage = (static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::depth_stencil)) != 0;
	const bool vertex_stage = (static_cast<uint32_t>(stages) & static_cast<uint32_t>(pipeline_stage::vertex_shader)) != 0;
	if (!cleared && !depth_stage && !vertex_stage)
		return;
	if (!followed(cmd))
		return;
	JitterContextData &d = context_of(cmd);
	if (cleared) {
		g_scene_jitter.note_bound(d.state.bound_serial != 0, false);
		jitter_on_cleared(d.state);
		depth_reset(cmd, d);
		if (d.regrid_count != 0)
			regrid_drop(cmd, d, false);
		d.depth_write = true;
		return;
	}
	if (vertex_stage) {
		d.vertex_pipe = state.handle;
		d.vertex_stale = true;
	}
	if (!depth_stage)
		return;
	if (is_d3d11(cmd)) {
		const D3D11Depth depth = d3d11_depth(state.handle);
		jitter_on_depth_test(d.state, depth.tests);
		d.depth_write = depth.writes;
		jitter_on_depth_pipeline(d.state, true, depth.enabled ? 1 : 0, depth.compares ? 1 : 0);
		d.depth_stale = false;
		return;
	}
	jitter_on_depth_test(d.state, !untested_pipeline(state.handle));
	d.depth_pipe = state.handle;
	d.depth_stale = true;
	d.dyn_pipe = ~0ull;
}

void bind_pipeline_states(command_list *cmd, uint32_t count, const dynamic_state *states, const uint32_t *values)
{
	if (cmd == nullptr)
		return;
	if (states == nullptr || values == nullptr || !followed(cmd))
		return;
	for (uint32_t i = 0; i < count; ++i) {
		if (states[i] == dynamic_state::depth_enable) {
			jitter_on_depth_test(context_of(cmd).state, values[i] != 0);
			jitter_on_depth_dynamic(context_of(cmd).state, values[i] != 0 ? 1 : 0, -1);
		} else if (states[i] == dynamic_state::depth_func) {
			jitter_on_depth_dynamic(context_of(cmd).state, -1, func_compares(static_cast<compare_op>(values[i])) ? 1 : 0);
		}
	}
}

uint64_t vertex_code_hash(const shader_desc &vs) noexcept
{
	uint64_t h = 1469598103934665603ull;
	const auto mix = [&h](const uint8_t *p, size_t n) {
		for (size_t i = 0; i < n; ++i) {
			h ^= p[i];
			h *= 1099511628211ull;
		}
	};
	mix(static_cast<const uint8_t *>(vs.code), vs.code_size);
	if (vs.entry_point != nullptr)
		mix(reinterpret_cast<const uint8_t *>(vs.entry_point), std::strlen(vs.entry_point));
	const uint64_t size = vs.code_size;
	mix(reinterpret_cast<const uint8_t *>(&size), sizeof(size));
	return h;
}

std::mutex g_vertex_classes_lock;
std::unordered_map<uint64_t, JitterVertexClass> g_vertex_classes;
constexpr size_t kVertexClassesKept = 8192u;

JitterVertexClass vertex_class_of(device_api api, const shader_desc &vs, bool layout)
{
	if (vs.code == nullptr || vs.code_size == 0)
		return JitterVertexClass::Unknown;
	if (api == device_api::d3d11)
		return layout ? JitterVertexClass::Unknown : classify_dxbc_vertex_shader(vs.code, vs.code_size);
	if (api != device_api::d3d12 && api != device_api::vulkan && api != device_api::opengl)
		return JitterVertexClass::Unknown;
	const uint64_t key = vertex_code_hash(vs) ^ static_cast<uint64_t>(api);
	{
		const std::lock_guard<std::mutex> lock(g_vertex_classes_lock);
		const auto it = g_vertex_classes.find(key);
		if (it != g_vertex_classes.end())
			return it->second;
	}
	JitterVertexClass c = JitterVertexClass::Unknown;
	if (api == device_api::d3d12)
		c = classify_dxbc_vertex_shader(vs.code, vs.code_size);
	else if (api == device_api::vulkan)
		c = classify_spirv_vertex_shader(vs.code, vs.code_size, vs.entry_point);
	else
		c = classify_glsl_vertex_shader(static_cast<const char *>(vs.code), vs.code_size);
	const std::lock_guard<std::mutex> lock(g_vertex_classes_lock);
	if (g_vertex_classes.size() >= kVertexClassesKept)
		g_vertex_classes.clear();
	g_vertex_classes[key] = c;
	return c;
}

void init_pipeline(device *dev, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline pipe)
{
	if (dev == nullptr || pipe.handle == 0 || subobjects == nullptr)
		return;
	const device_api api = dev->get_api();
	if (api == device_api::d3d9)
		return;
	const depth_stencil_desc *ds = nullptr;
	const shader_desc *vs = nullptr;
	const pipeline *libraries = nullptr;
	uint32_t library_count = 0;
	bool dynamic_enable = false, dynamic_func = false, dynamic_write = false, layout = false;
	for (uint32_t i = 0; i < count; ++i) {
		layout = layout || subobjects[i].type == pipeline_subobject_type::input_layout;
		if (subobjects[i].data == nullptr)
			continue;
		if (subobjects[i].type == pipeline_subobject_type::depth_stencil_state) {
			ds = static_cast<const depth_stencil_desc *>(subobjects[i].data);
		} else if (subobjects[i].type == pipeline_subobject_type::vertex_shader && subobjects[i].count != 0) {
			vs = static_cast<const shader_desc *>(subobjects[i].data);
		} else if (subobjects[i].type == pipeline_subobject_type::libraries) {
			libraries = static_cast<const pipeline *>(subobjects[i].data);
			library_count = subobjects[i].count;
		} else if (subobjects[i].type == pipeline_subobject_type::dynamic_pipeline_states) {
			const auto *const states = static_cast<const dynamic_state *>(subobjects[i].data);
			for (uint32_t k = 0; k < subobjects[i].count; ++k) {
				dynamic_enable = dynamic_enable || states[k] == dynamic_state::depth_enable;
				dynamic_func = dynamic_func || states[k] == dynamic_state::depth_func;
				dynamic_write = dynamic_write || states[k] == dynamic_state::depth_write_mask;
			}
		}
	}
	bool untested = api != device_api::d3d11 && !dynamic_enable && !dynamic_func && ds != nullptr &&
		!(ds->depth_enable && !(ds->depth_func == compare_op::always && !ds->depth_write_mask));
	PipelineFacts facts;
	if (ds != nullptr && api != device_api::d3d11) {
		facts.depth = true;
		facts.enable = static_cast<int8_t>(dynamic_enable ? -1 : ds->depth_enable ? 1 : 0);
		facts.compares = static_cast<int8_t>(dynamic_func ? -1 : func_compares(ds->depth_func) ? 1 : 0);
		facts.write = static_cast<int8_t>(dynamic_write ? -1 : ds->depth_write_mask ? 1 : 0);
		facts.op = static_cast<int8_t>(dynamic_func ? -1 : static_cast<int>(ds->depth_func));
	}
	if (vs != nullptr)
		facts.vertex = vertex_class_of(api, *vs, layout);
	if (vs != nullptr && api == device_api::d3d11 && !layout && vs->code != nullptr)
		facts.wterms_known = dxbc_position_w_terms(vs->code, vs->code_size, &facts.wterms);
	for (uint32_t k = 0; libraries != nullptr && k < library_count; ++k) {
		const PipelineFacts lib = pipeline_facts(libraries[k].handle);
		if (!facts.depth && lib.depth) {
			facts.depth = true;
			facts.enable = lib.enable;
			facts.compares = lib.compares;
			facts.write = lib.write;
			facts.op = lib.op;
		}
		if (facts.vertex == JitterVertexClass::Unknown)
			facts.vertex = lib.vertex;
		untested = untested || untested_pipeline(libraries[k].handle);
	}
	if (untested)
		note_untested_pipeline(pipe.handle, true);
	note_pipeline_facts(pipe.handle, facts);
}

void destroy_pipeline(device *, pipeline pipe)
{
	note_untested_pipeline(pipe.handle, false);
	note_pipeline_facts(pipe.handle, PipelineFacts{});
	constant_reads().forget_shader(pipe.handle);
}

bool draw(command_list *cmd, uint32_t vertices, uint32_t instances, uint32_t, uint32_t)
{
	if (cmd == nullptr)
		return false;
	any_draw(cmd, jitter_is_fullscreen_draw(vertices, instances));
	return false;
}

bool draw_indexed(command_list *cmd, uint32_t indices, uint32_t instances, uint32_t, int32_t, uint32_t)
{
	if (cmd == nullptr)
		return false;
	any_draw(cmd, jitter_is_fullscreen_draw(indices, instances));
	return false;
}

bool draw_indirect(command_list *cmd, indirect_command type, resource, uint64_t, uint32_t, uint32_t)
{
	if (cmd == nullptr)
		return false;
	if (type != indirect_command::dispatch)
		any_draw(cmd, false);
	return false;
}

void close(command_list *cmd)
{
	if (cmd == nullptr)
		return;
	if (JitterContextData *const data = cmd->get_private_data<JitterContextData>()) {
		const std::lock_guard<std::mutex> lock(data->lock);
		data->closed = true;
	}
}

void execute(command_queue *, command_list *cmd)
{
	if (cmd == nullptr)
		return;
	hand_on_counts(cmd);
}

void execute_secondary(command_list *cmd, command_list *secondary)
{
	if (cmd == nullptr || secondary == nullptr)
		return;
	if (JitterContextData *const context = secondary->get_private_data<JitterContextData>()) {
		JitterDrawCounts counts;
		{
			const std::lock_guard<std::mutex> lock(context->lock);
			if (!context->closed)
				return;
			context->closed = false;
			context->fold_all_draws();
			counts = context->counts;
			context->counts = JitterDrawCounts{};
		}
		JitterCommandListData *list = cmd->get_private_data<JitterCommandListData>();
		if (list == nullptr)
			list = cmd->create_private_data<JitterCommandListData>();
		const std::lock_guard<std::mutex> lock(list->lock);
		add_draw_counts(list->counts, counts);
		return;
	}
	JitterCommandListData *const list = secondary->get_private_data<JitterCommandListData>();
	if (list == nullptr)
		return;
	JitterDrawCounts counts;
	bool replay = false;
	{
		const std::lock_guard<std::mutex> lock(list->lock);
		const uint64_t frame = g_scene_jitter.frame();
		if (!list->played) {
			list->played = true;
			list->first_played = frame;
		} else {
			replay = list->first_played != frame;
		}
		counts = list->counts;
	}
	if (!any_draw_counts(counts))
		return;
	if (replay)
		counts = replayed_draw_counts(counts);
	JitterContextData &into = context_of(cmd);
	const std::lock_guard<std::mutex> lock(into.lock);
	add_draw_counts(into.counts, counts);
}

void reset(command_list *cmd)
{
	if (cmd == nullptr)
		return;
	vulkan_viewport_hook().forget_list(native_of(cmd));
	JitterContextData *const data = cmd->get_private_data<JitterContextData>();
	if (data == nullptr)
		return;
	data->vk_stamp = 0;
	data->last_pass_target = 0;
	g_scene_jitter.note_bound(data->state.bound_serial != 0, false);
	jitter_on_reset(data->state);
	depth_reset(cmd, *data);
	const std::lock_guard<std::mutex> lock(data->lock);
	data->counts = JitterDrawCounts{};
	data->all_draws.store(0, std::memory_order_relaxed);
	data->closed = false;
}

void init_resource(device *, const resource_desc &desc, const subresource_data *, resource_usage, resource res)
{
	if ((desc.usage & (resource_usage::render_target | resource_usage::depth_stencil)) != 0) {
		g_scene_jitter.forget_sizes();
		g_scene_jitter.forget_target(res.handle);
	}
}

void destroy(command_list *cmd)
{
	if (cmd == nullptr)
		return;
	vulkan_viewport_hook().forget_list(native_of(cmd));
	if (JitterContextData *const data = cmd->get_private_data<JitterContextData>()) {
		g_scene_jitter.note_bound(data->state.bound_serial != 0, false);
		data->state.bound_serial = 0;
		g_scene_jitter.release(jitter_list_key(cmd));
		cmd->destroy_private_data<JitterContextData>();
	}
	if (cmd->get_private_data<JitterCommandListData>() != nullptr)
		cmd->destroy_private_data<JitterCommandListData>();
}

}

SceneJitter &scene_jitter() noexcept
{
	return g_scene_jitter;
}

void register_scene_jitter_hooks() noexcept
{
	register_jitter_shader_hooks();
	constant_reads_forget(0);
	reshade::register_event<reshade::addon_event::init_command_list>(jitter_events::init);
	reshade::register_event<reshade::addon_event::bind_viewports>(jitter_events::bind_viewports);
	reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(jitter_events::bind_render_targets);
	reshade::register_event<reshade::addon_event::push_descriptors>(jitter_events::push_descriptors);
	reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(jitter_events::clear_depth_stencil);
	reshade::register_event<reshade::addon_event::copy_resource>(jitter_events::copy_resource);
	reshade::register_event<reshade::addon_event::copy_texture_region>(jitter_events::copy_texture_region);
	reshade::register_event<reshade::addon_event::destroy_device>(jitter_events::destroy_device);
	reshade::register_event<reshade::addon_event::begin_render_pass>(jitter_events::begin_render_pass);
	reshade::register_event<reshade::addon_event::end_render_pass>(jitter_events::end_render_pass);
	reshade::register_event<reshade::addon_event::bind_pipeline>(jitter_events::bind_pipeline);
	reshade::register_event<reshade::addon_event::draw>(jitter_events::draw);
	reshade::register_event<reshade::addon_event::draw_indexed>(jitter_events::draw_indexed);
	reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(jitter_events::draw_indirect);
	reshade::register_event<reshade::addon_event::close_command_list>(jitter_events::close);
	reshade::register_event<reshade::addon_event::execute_command_list>(jitter_events::execute);
	reshade::register_event<reshade::addon_event::execute_secondary_command_list>(jitter_events::execute_secondary);
	reshade::register_event<reshade::addon_event::reset_command_list>(jitter_events::reset);
	reshade::register_event<reshade::addon_event::destroy_command_list>(jitter_events::destroy);
	reshade::register_event<reshade::addon_event::init_resource>(jitter_events::init_resource);
	reshade::register_event<reshade::addon_event::bind_pipeline_states>(jitter_events::bind_pipeline_states);
	reshade::register_event<reshade::addon_event::init_pipeline>(jitter_events::init_pipeline);
	reshade::register_event<reshade::addon_event::destroy_pipeline>(jitter_events::destroy_pipeline);
}

JitterFrame take_scene_jitter_frame(reshade::api::effect_runtime *runtime) noexcept
{
	if (reshade::api::command_list *const immediate = immediate_of(runtime))
		hand_on_counts(immediate);
	return g_scene_jitter.take_frame();
}

void discard_scene_jitter_counts(reshade::api::effect_runtime *runtime) noexcept
{
	if (reshade::api::command_list *const immediate = immediate_of(runtime)) {
		if (JitterContextData *const data = immediate->get_private_data<JitterContextData>()) {
			const std::lock_guard<std::mutex> lock(data->lock);
			data->counts = JitterDrawCounts{};
			data->all_draws.store(0, std::memory_order_relaxed);
		}
	}
	g_scene_jitter.discard_counts();
}

SceneSnapshot &scene_snapshot()
{
	static SceneSnapshot s;
	return s;
}

bool SceneSnapshot::fits(const Slot &slot, const reshade::api::resource_desc &like) noexcept
{
	return slot.res.handle != 0 && slot.desc.texture.width == like.texture.width &&
		slot.desc.texture.height == like.texture.height && slot.desc.texture.format == like.texture.format;
}

bool SceneSnapshot::queued(uint32_t slot) const noexcept
{
	for (uint32_t i = 0; i < done_count_; ++i) {
		if (done_[i] == slot)
			return true;
	}
	return false;
}

void SceneSnapshot::prepare(reshade::api::device *device)
{
	if (device == nullptr)
		return;
	const std::lock_guard<std::mutex> lock(lock_);
	if (!want_ || device != device_)
		return;
	want_ = false;
	const resource_desc d(wanted_.texture.width, wanted_.texture.height, 1, 1, wanted_.texture.format, 1,
		memory_heap::default_, resource_usage::copy_dest | resource_usage::copy_source | resource_usage::resolve_dest);
	done_count_ = 0;
	cur_ = kNone;
	for (Slot &slot : ring_) {
		if (fits(slot, wanted_))
			continue;
		if (slot.res.handle != 0)
			device->destroy_resource(slot.res);
		slot = Slot{};
		if (!device->create_resource(d, nullptr, resource_usage::copy_dest, &slot.res)) {
			slot.res = resource{ 0 };
			note_ = "the copy of the window could not be made";
			return;
		}
		slot.desc = d;
	}
}

bool SceneSnapshot::copy(reshade::api::command_list *cmd, reshade::api::resource window, Slot &slot, bool resolve)
{
	const auto take = [&]() {
		if (resolve) {
			cmd->barrier(slot.res, resource_usage::copy_dest, resource_usage::resolve_dest);
			cmd->resolve_texture_region(window, 0, nullptr, slot.res, 0, 0, 0, 0, slot.desc.texture.format);
			cmd->barrier(slot.res, resource_usage::resolve_dest, resource_usage::copy_dest);
		} else {
			cmd->copy_texture_region(window, 0, nullptr, slot.res, 0, nullptr);
		}
	};
	switch (cmd->get_device()->get_api()) {
	case device_api::d3d11:
		take();
		return true;
	case device_api::d3d12:
		cmd->barrier(window, resource_usage::render_target,
			resolve ? resource_usage::resolve_source : resource_usage::copy_source);
		take();
		cmd->barrier(window, resolve ? resource_usage::resolve_source : resource_usage::copy_source,
			resource_usage::render_target);
		return true;
	case device_api::vulkan: {
		const char *why = nullptr;
		if (!vulkan_copy_in_pass(native_of(cmd), window.handle, slot.res.handle, slot.desc.texture.width,
				slot.desc.texture.height, resolve, &why)) {
			note_ = why != nullptr ? why : "the render pass could not be begun again";
			return false;
		}
		return true;
	}
	default:
		note_ = "the window is not copied before the interface on this API yet";
		return false;
	}
}

void SceneSnapshot::before_interface(reshade::api::command_list *cmd, reshade::api::resource window, uint64_t serial)
{
	const uint64_t frame = frame_.load(std::memory_order_acquire);
	if (cmd == nullptr || window.handle == 0 || good_frame_.load(std::memory_order_acquire) == frame)
		return;
	reshade::api::device *const device = cmd->get_device();
	const std::lock_guard<std::mutex> lock(lock_);
	if (device_ != device) {
		for (Slot &s : ring_)
			s = Slot{};
		cur_ = kNone;
		done_count_ = 0;
		device_ = device;
	}
	if (cur_ != kNone) {
		const Slot &s = ring_[cur_];
		if (!s.spoiled || s.window != window.handle || s.copies >= kCopiesPerFrame)
			return;
	} else {
		for (uint32_t k = 0; k < kRing && cur_ == kNone; ++k) {
			if (!queued(k))
				cur_ = k;
		}
		if (cur_ == kNone)
			return;
		ring_[cur_].copies = 0;
	}
	Slot &slot = ring_[cur_];
	slot.serial = serial;
	slot.window = window.handle;
	slot.valid = false;
	slot.spoiled = false;
	const resource_desc like = device->get_resource_desc(window);
	if (like.type != resource_type::texture_2d || like.texture.samples == 0) {
		static char why[160];
		std::snprintf(why, sizeof(why), "the window is not a two-dimensional texture (type %d, %ux%u, %u layers, "
			"%u mips, %u samples, format %d)", static_cast<int>(like.type), like.texture.width, like.texture.height,
			like.texture.depth_or_layers, like.texture.levels, like.texture.samples, static_cast<int>(like.texture.format));
		note_ = why;
		slot.copies = kCopiesPerFrame;
		return;
	}
	if (!fits(slot, like)) {
		wanted_ = like;
		want_ = true;
		note_ = "the copy of the window is made at the next present";
		slot.copies = kCopiesPerFrame;
		return;
	}
	++slot.copies;
	copied_frame_.store(frame, std::memory_order_release);
	if (!copy(cmd, window, slot, like.texture.samples > 1))
		return;
	slot.valid = true;
	++copied_;
	++tally_.copies;
	good_frame_.store(frame, std::memory_order_release);
}

void SceneSnapshot::count(bool window, bool scene, bool quad, bool depth_tested, uint32_t w, uint32_t h)
{
	constexpr auto relaxed = std::memory_order_relaxed;
	if (window) {
		(scene ? window_scene_ : quad ? window_quads_ : window_other_).fetch_add(1u, relaxed);
		const uint32_t at = order_len_.fetch_add(1u, relaxed);
		if (at < sizeof(order_) - 1u)
			order_[at] = scene ? 'S' : quad ? (depth_tested ? 'd' : 'q') : (depth_tested ? 'D' : 'o');
		return;
	}
	if (scene)
		return;
	elsewhere_.fetch_add(1u, relaxed);
	if (quad)
		elsewhere_quads_.fetch_add(1u, relaxed);
	elsewhere_w_.store(w, relaxed);
	elsewhere_h_.store(h, relaxed);
}

void SceneSnapshot::note_scene(uint64_t, reshade::api::resource target)
{
	scene_image_.store(target.handle, std::memory_order_relaxed);
	scene_frame_.store(frame_.load(std::memory_order_acquire), std::memory_order_release);
}

bool SceneSnapshot::scene_drawn_into(uint64_t, reshade::api::resource target)
{
	return scene_frame_.load(std::memory_order_acquire) == frame_.load(std::memory_order_acquire) &&
		scene_image_.load(std::memory_order_relaxed) == target.handle;
}

uint32_t SceneSnapshot::window_draw_index(uint64_t, reshade::api::resource window)
{
	window_image_.store(window.handle, std::memory_order_relaxed);
	return index_.fetch_add(1u, std::memory_order_relaxed);
}

void SceneSnapshot::note_scene_mesh(uint32_t index)
{
	uint32_t was = mesh_cur_.load(std::memory_order_relaxed);
	while (index > was && !mesh_cur_.compare_exchange_weak(was, index, std::memory_order_relaxed)) {
	}
}

void SceneSnapshot::end_frame_locked()
{
	const uint32_t slot = cur_ != kNone && ring_[cur_].valid && !ring_[cur_].spoiled ? cur_ : kNone;
	if (cur_ != kNone && slot == kNone && ring_[cur_].valid)
		note_ = ring_[cur_].copies >= kCopiesPerFrame
			? "the scene and the interface alternate in the window more than the copies a frame allow"
			: "the scene was drawn into the window after the last interface draw";
	if (done_count_ == kQueued) {
		for (uint32_t i = 1; i < kQueued; ++i)
			done_[i - 1] = done_[i];
		--done_count_;
	}
	done_[done_count_++] = slot;
	cur_ = kNone;
	mesh_prev_.store(mesh_cur_.exchange(0u, std::memory_order_relaxed), std::memory_order_relaxed);
	index_.store(0u, std::memory_order_relaxed);
	frame_.fetch_add(1u, std::memory_order_acq_rel);
}

void SceneSnapshot::end_frame_in_stream(reshade::api::resource presented_to)
{
	const uint64_t window = window_image_.load(std::memory_order_relaxed);
	if (presented_to.handle == 0 || window == 0 || window == presented_to.handle)
		return;
	const std::lock_guard<std::mutex> lock(lock_);
	end_frame_locked();
	ended_in_stream_ = true;
	++tally_.in_stream;
}

void SceneSnapshot::end_frame_at_present()
{
	const std::lock_guard<std::mutex> lock(lock_);
	if (!ended_in_stream_) {
		end_frame_locked();
		++tally_.at_present;
	}
	ended_in_stream_ = false;
}

void SceneSnapshot::spoil(uint64_t, reshade::api::resource target, bool)
{
	const uint64_t frame = frame_.load(std::memory_order_acquire);
	if (copied_frame_.load(std::memory_order_acquire) != frame)
		return;
	const std::lock_guard<std::mutex> lock(lock_);
	if (cur_ != kNone && ring_[cur_].window == target.handle && !ring_[cur_].spoiled) {
		ring_[cur_].spoiled = true;
		good_frame_.store(0, std::memory_order_release);
	}
}

bool SceneSnapshot::take(uint64_t, SceneSnapshotTaken *out)
{
	if (out == nullptr)
		return false;
	const std::lock_guard<std::mutex> lock(lock_);
	if (done_count_ == 0) {
		++tally_.empty;
		return false;
	}
	const uint32_t slot = done_[0];
	for (uint32_t i = 1; i < done_count_; ++i)
		done_[i - 1] = done_[i];
	--done_count_;
	if (slot == kNone) {
		++tally_.none;
		return false;
	}
	++tally_.taken;
	out->resource = ring_[slot].res;
	out->state = resource_usage::copy_dest;
	return true;
}

SceneSnapshot::Tally SceneSnapshot::take_tally()
{
	const std::lock_guard<std::mutex> lock(lock_);
	const Tally t = tally_;
	tally_ = Tally{};
	return t;
}

SceneSnapshot::Census SceneSnapshot::take_census()
{
	constexpr auto relaxed = std::memory_order_relaxed;
	Census c;
	c.window_scene = window_scene_.exchange(0u, relaxed);
	c.window_quads = window_quads_.exchange(0u, relaxed);
	c.window_other = window_other_.exchange(0u, relaxed);
	c.elsewhere = elsewhere_.exchange(0u, relaxed);
	c.elsewhere_quads = elsewhere_quads_.exchange(0u, relaxed);
	c.elsewhere_w = elsewhere_w_.load(relaxed);
	c.elsewhere_h = elsewhere_h_.load(relaxed);
	const uint32_t n = order_len_.exchange(0u, relaxed);
	c.order_len = n < sizeof(order_) - 1u ? n : static_cast<uint32_t>(sizeof(order_) - 1u);
	for (uint32_t i = 0; i < c.order_len; ++i)
		c.order[i] = order_[i];
	c.order[c.order_len] = '\0';
	return c;
}

void SceneSnapshot::release(reshade::api::device *device)
{
	const std::lock_guard<std::mutex> lock(lock_);
	if (device == nullptr || device != device_)
		return;
	for (Slot &s : ring_) {
		if (s.res.handle != 0)
			device->destroy_resource(s.res);
		s = Slot{};
	}
	cur_ = kNone;
	done_count_ = 0;
	device_ = nullptr;
}

}
