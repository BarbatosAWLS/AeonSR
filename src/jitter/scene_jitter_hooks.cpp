#include "aeon_sr/jitter/scene_jitter_hooks.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/jitter/constant_reads.hpp"
#include "aeon_sr/jitter/depth_regrid.hpp"
#include "aeon_sr/jitter/gl_jitter.hpp"
#include "aeon_sr/jitter/scene_jitter_shaders.hpp"
#include "aeon_sr/jitter/scene_snapshot.hpp"
#include "aeon_sr/jitter/shader_position.hpp"
#include "aeon_sr/jitter/vulkan_constants.hpp"
#include "aeon_sr/jitter/vulkan_viewport_hook.hpp"

#include <d3d10.h>
#include <d3d11.h>
#include <d3d12.h>

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
	JitterProofWhy projection_why = JitterProofWhy::NotRead;
	uint64_t last_pass_target = 0;
	struct CompSeen {
		uint64_t target = 0;
		bool drawn = false, first_pass = false, tested = false;
	};
	static constexpr uint32_t kCompSeen = 8u;
	CompSeen comp_seen[kCompSeen]{};
	uint32_t comp_seen_next = 0;
	uint64_t comp_frame = 0;
	uint64_t comp_target = 0, comp_target_frame = 0;
	uint32_t comp_w = 0, comp_h = 0;
	bool window_drawn = false;
	uint64_t window_pending = 0;
	uint32_t pending_w = 0, pending_h = 0;
	bool bound_window_image = false;
	uint64_t scene_tested_here = 0;
	uint64_t depth_cleared_boundary = 0;
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
	uint64_t layer_told_target = 0, layer_told_frame = 0, tested_frame = 0;
	bool layer_told_written = false;
	LayerNotes layer_notes;
	uint64_t layer_asked_target = 0, layer_asked_frame = 0;
	bool layer_answer = false;
	int ordered = -1;
	uint64_t dyn_pipe = ~0ull;
	bool dyn_depth = false;
	int8_t dyn_enable = -1, dyn_write = -1, dyn_op = -1, dyn_compares = -1;
	D3D12RootArgs root;
	int8_t d3d12_direct = -1;
	struct D3D12Pending {
		D3D12ConstantChecks::Key key;
		bool table = false;
		uint64_t where = 0, at = 0, offset = 0;
		bool drawn = false;
		float at_draw[4]{};
	};
	static constexpr size_t kPendingKept = 256u;
	std::vector<D3D12Pending> pending;
	int8_t bundle = -1;
	uint32_t bundle_draws = 0, bundle_tested = 0, bundle_quads = 0;
	bool played = false;
	uint64_t first_played = 0;
	JitterDrawCounts recorded;

	~JitterContextData();
};

struct __declspec(uuid("9b6a2f41-5c3e-4d7a-8f10-2e4b6c8d1a93")) JitterCommandListData : JitterCounted {
	bool played = false;
	uint64_t first_played = 0;
	LayerNotes layer_notes;
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

bool is_d3d12(command_list *cmd)
{
	return cmd != nullptr && cmd->get_device()->get_api() == device_api::d3d12;
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

bool d3d12_bundle(command_list *cmd)
{
	if (cmd == nullptr || cmd->get_device()->get_api() != device_api::d3d12)
		return false;
	auto *const native = reinterpret_cast<IUnknown *>(static_cast<uintptr_t>(cmd->get_native()));
	ID3D12CommandList *list = nullptr;
	if (native == nullptr || FAILED(native->QueryInterface(IID_PPV_ARGS(&list))))
		return false;
	const bool bundle = list->GetType() == D3D12_COMMAND_LIST_TYPE_BUNDLE;
	list->Release();
	return bundle;
}

std::atomic<bool> g_any_bundle{ false };

void depth_defaults(command_list *cmd, JitterListState &s);

JitterContextData &context_of(command_list *cmd)
{
	JitterContextData *data = cmd->get_private_data<JitterContextData>();
	if (data == nullptr) {
		data = cmd->create_private_data<JitterContextData>();
		depth_defaults(cmd, data->state);
		data->bundle = d3d12_bundle(cmd) ? 1 : 0;
		if (data->bundle == 1)
			g_any_bundle.store(true, std::memory_order_relaxed);
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
	uint64_t layout = 0;
	bool position_replaced = false;
	bool spirv_known = false;
	SpirvWTerms spirv{};
	uint64_t vertex_hash = 0;
};
std::shared_mutex g_facts_lock;
std::unordered_map<uint64_t, PipelineFacts> g_facts;
std::atomic<bool> g_any_facts{ false };

void note_pipeline_facts(uint64_t pipe, const PipelineFacts &f)
{
	if (pipe == 0)
		return;
	const bool any = f.depth || f.vertex != JitterVertexClass::Unknown || f.wterms_known || f.spirv_known;
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
	d.projection_why = !f.wterms_known ? JitterProofWhy::NoTerms : JitterProofWhy::Replaced;
	if (!f.wterms_known || d3d11_vertex_position_replaced(cmd))
		return -1;
	d.projection_why = immediate_context(cmd, d) ? JitterProofWhy::NotRead : JitterProofWhy::Deferred;
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

std::atomic<uint64_t> g_root_epoch{ 1 };
std::atomic<bool> g_root_missed{ false };

uint64_t root_epoch()
{
	if (g_root_missed.load(std::memory_order_relaxed) && g_root_missed.exchange(false))
		g_root_epoch.fetch_add(1);
	return g_root_epoch.load(std::memory_order_relaxed);
}

D3D12RootArgs *root_args_of(command_list *cmd)
{
	if (!g_scene_jitter.engaged()) {
		if (!g_root_missed.load(std::memory_order_relaxed))
			g_root_missed.store(true, std::memory_order_relaxed);
		return nullptr;
	}
	const uint64_t epoch = root_epoch();
	D3D12RootArgs &args = context_of(cmd).root;
	if (args.epoch != epoch) {
		args.clear();
		args.epoch = epoch;
	}
	return &args;
}

bool d3d12_direct(command_list *cmd, JitterContextData &d)
{
	if (d.d3d12_direct < 0) {
		auto *const unknown = static_cast<IUnknown *>(native_of(cmd));
		ID3D12GraphicsCommandList *list = nullptr;
		bool bundle = false;
		if (unknown != nullptr && SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&list))) && list != nullptr) {
			bundle = list->GetType() == D3D12_COMMAND_LIST_TYPE_BUNDLE;
			list->Release();
		}
		d.d3d12_direct = bundle ? 0 : 1;
	}
	return d.d3d12_direct == 1;
}

bool d3d12_read_table(uint64_t heap, uint32_t index, uint64_t offset, float out[4])
{
	uint64_t va = 0, bytes = 0;
	if (!d3d12_cbv_heaps().find(heap, index, &va, &bytes) || offset > bytes || bytes - offset < 16u)
		return false;
	return d3d12_buffers().read_va(va + offset, out);
}

int d3d12_projection(command_list *cmd, JitterContextData &d)
{
	const PipelineFacts f = pipeline_facts(d.vertex_pipe);
	d.projection_why = !f.wterms_known ? JitterProofWhy::NoTerms
		: f.position_replaced ? JitterProofWhy::Replaced : JitterProofWhy::NotRead;
	if (!f.wterms_known || f.position_replaced || f.layout == 0 || d.root.epoch != root_epoch())
		return -1;
	struct Reader {
		command_list *cmd;
		JitterContextData *d;
		uint64_t layout, pipeline, now;
		bool direct;
	} reader{ cmd, &d, f.layout, d.vertex_pipe, g_scene_jitter.frame(), d3d12_direct(cmd, d) };
	return dxbc_w_terms_perspective_spaced(
		f.wterms,
		[](void *user, uint32_t space, uint32_t slot, uint32_t reg, float out[4]) {
			const Reader &r = *static_cast<const Reader *>(user);
			const D3D12Place p = d3d12_root_place(r.d->root, r.layout, space, slot, reg);
			if (p.kind == D3D12Place::Kind::Value) {
				std::memcpy(out, p.value, 16u);
				return true;
			}
			if (p.kind == D3D12Place::Kind::None)
				return false;
			JitterContextData::D3D12Pending at;
			at.key = D3D12ConstantChecks::Key{ r.pipeline, space, slot, reg };
			if (p.kind == D3D12Place::Kind::View) {
				at.where = p.buffer;
				at.at = p.offset;
				at.drawn = d3d12_buffers().read(p.buffer, p.offset, at.at_draw);
			} else {
				descriptor_heap heap{ 0 };
				uint32_t index = 0;
				r.cmd->get_device()->get_descriptor_heap_offset(descriptor_table{ p.table }, p.index, 0u, &heap, &index);
				if (heap.handle == 0)
					return false;
				at.table = true;
				at.where = heap.handle;
				at.at = index;
				at.offset = p.offset;
				at.drawn = d3d12_read_table(heap.handle, index, p.offset, at.at_draw);
			}
			if (r.direct) {
				const std::lock_guard<std::mutex> lock(r.d->lock);
				bool known = false;
				for (const JitterContextData::D3D12Pending &q : r.d->pending)
					known = known || (q.key == at.key && q.table == at.table && q.where == at.where && q.at == at.at &&
						q.offset == at.offset);
				if (!known && r.d->pending.size() < JitterContextData::kPendingKept)
					r.d->pending.push_back(at);
			}
			D3D12ConstantChecks &checks = d3d12_constant_checks();
			if (at.drawn && checks.trusted(at.key)) {
				std::memcpy(out, at.at_draw, 16u);
				return true;
			}
			return checks.agreed(at.key, r.now, out);
		},
		&reader);
}

void d3d12_check_submitted(command_list *cmd)
{
	JitterContextData *const d = cmd->get_private_data<JitterContextData>();
	if (d == nullptr)
		return;
	const uint64_t now = g_scene_jitter.frame();
	D3D12ConstantChecks &checks = d3d12_constant_checks();
	const std::lock_guard<std::mutex> lock(d->lock);
	for (const JitterContextData::D3D12Pending &p : d->pending) {
		float v[4] = {};
		const bool read = p.table ? d3d12_read_table(p.where, static_cast<uint32_t>(p.at), p.offset, v)
								  : d3d12_buffers().read(p.where, p.at, v);
		checks.checked(p.key, p.drawn ? p.at_draw : nullptr, read ? v : nullptr, now);
	}
	d->pending.clear();
}

int vulkan_projection(command_list *cmd, JitterContextData &d)
{
	const PipelineFacts f = pipeline_facts(d.vertex_pipe);
	d.projection_why = !f.spirv_known ? JitterProofWhy::NoTerms
		: f.position_replaced ? JitterProofWhy::Replaced : JitterProofWhy::NotRead;
	if (!f.spirv_known || f.position_replaced)
		return -1;
	return vulkan_constants().perspective(native_of(cmd), d.vertex_pipe, f.vertex_hash, f.spirv, g_scene_jitter.frame());
}

bool projection_read(command_list *cmd)
{
	return is_d3d11(cmd) || is_d3d12(cmd) || is_vulkan(cmd);
}

int projection_of(command_list *cmd, JitterContextData &d)
{
	if (is_d3d11(cmd))
		return d3d11_projection(cmd, d);
	if (is_d3d12(cmd))
		return d3d12_projection(cmd, d);
	if (is_vulkan(cmd))
		return vulkan_projection(cmd, d);
	return -1;
}

void note_depth_cleared(JitterContextData &d, uint64_t serial)
{
	d.state.depth_cleared_serial = serial;
	d.depth_cleared_boundary = g_scene_jitter.boundaries();
}

void forget_clear_of_frame_before(JitterContextData &d)
{
	if (d.state.depth_cleared_serial != 0 && d.depth_cleared_boundary != g_scene_jitter.boundaries())
		d.state.depth_cleared_serial = 0;
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

void bind_composite(command_list *cmd, JitterContextData &d, uint64_t target, uint32_t w, uint32_t h);

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
		if (target != 0)
			bind_composite(cmd, d, target, tw, th);
		const bool scene_depth = depth != 0 && arm.scene_w != 0 &&
			(arm.scene_depth != 0 ? depth == arm.scene_depth : dw == arm.scene_w && dh == arm.scene_h);
		if (scene_depth && target != 0 && tw == arm.scene_w && th == arm.scene_h && !window &&
			!jitter_arm_into_back_buffer(arm, target) && cmd->get_device()->get_api() != device_api::d3d9)
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

void count_other(JitterContextData &d, const JitterArm &arm, bool fullscreen, uint32_t n = 1u)
{
	if (!fullscreen && arm.scene_depth != 0 && d.state.depth != 0 && d.state.depth != arm.scene_depth) {
		const std::lock_guard<std::mutex> lock(d.lock);
		count_other_depth_draw(d.counts, d.state.depth_w, d.state.depth_h, n);
	}
	if (fullscreen || !jitter_scene_by_size(arm))
		return;
	uint32_t w = 0, h = 0;
	jitter_draw_size(d.state, &w, &h);
	if (!jitter_frame_like(w, h, arm.display_w, arm.display_h))
		return;
	const std::lock_guard<std::mutex> lock(d.lock);
	count_other_draw(d.counts, w, h, n);
}

void note_window_draw(command_list *cmd, JitterContextData &d, bool scene, bool fullscreen);

bool layer_ordered(command_list *cmd, JitterContextData &d)
{
	if (d.ordered < 0) {
		switch (cmd->get_device()->get_api()) {
		case device_api::d3d11:
			d.ordered = immediate_context(cmd, d) ? 1 : 0;
			break;
		case device_api::d3d9:
		case device_api::d3d10:
		case device_api::opengl:
			d.ordered = 1;
			break;
		default:
			d.ordered = 0;
			break;
		}
	}
	return d.ordered == 1;
}

void layer_window(command_list *cmd, JitterContextData &d)
{
	if (layer_ordered(cmd, d))
		scene_snapshot().note_window_written();
	else
		d.layer_notes.window();
}

bool layer_tested_due(command_list *cmd, JitterContextData &d)
{
	return layer_ordered(cmd, d) ? d.tested_frame != scene_snapshot().layer_frame() : !d.layer_notes.segment_tested;
}

__declspec(noinline) void layer_after_draw(command_list *cmd, JitterContextData &d, bool scene, bool into_window)
{
	const JitterArm &arm = d.arm;
	if (arm.serial == 0)
		return;
	if (scene && d.state.depth_test && jitter_scene_depth_bound(d.state, arm) && layer_tested_due(cmd, d)) {
		if (layer_ordered(cmd, d)) {
			d.tested_frame = scene_snapshot().layer_frame();
			scene_snapshot().note_scene_tested();
		} else {
			d.layer_notes.tested();
		}
	}
	if (into_window && d.state.target != 0)
		layer_window(cmd, d);
}

bool layer_candidate(const JitterContextData &d, bool quad, bool into_window)
{
	return d.arm.layers && !quad && !into_window && !d.state.target_scene && d.state.target != 0 &&
		d.state.target_w == d.arm.scene_w && d.state.target_h == d.arm.scene_h;
}

bool layer_after_due(command_list *cmd, JitterContextData &d, bool scene, bool into_window)
{
	return d.arm.layers && (into_window || (scene && d.state.depth_test && layer_tested_due(cmd, d)));
}

__declspec(noinline) void note_layer(command_list *cmd, JitterContextData &d, bool quad, bool into_window)
{
	const JitterArm &arm = d.arm;
	if (quad || into_window || d.state.target == 0 || d.state.target_scene || arm.serial == 0 || arm.scene_w == 0 ||
		d.state.target_w != arm.scene_w || d.state.target_h != arm.scene_h)
		return;
	const bool no_depth = d.state.depth == 0;
	SceneSnapshot &snap = scene_snapshot();
	if (!layer_ordered(cmd, d)) {
		d.layer_notes.drawn(d.state.target);
		if (!no_depth)
			return;
		const uint64_t frame = snap.layer_frame();
		if (d.layer_asked_target != d.state.target || d.layer_asked_frame != frame) {
			d.layer_asked_target = d.state.target;
			d.layer_asked_frame = frame;
			d.layer_answer = snap.layer_played(d.state.target);
		}
		d.state.layer = d.layer_answer;
		return;
	}
	const bool written = snap.window_written();
	const uint64_t frame = snap.layer_frame();
	if (d.layer_told_target != d.state.target || d.layer_told_frame != frame || d.layer_told_written != written) {
		d.layer_told_target = d.state.target;
		d.layer_told_frame = frame;
		d.layer_told_written = written;
		if (written)
			snap.note_after_window(d.state.target);
		else
			snap.note_before_window(d.state.target);
	}
	d.state.layer = written && no_depth && snap.layer_target(d.state.target);
}

void note_copy_into_window(command_list *cmd, uint64_t dest)
{
	if (dest == 0 || !g_scene_jitter.engaged())
		return;
	JitterContextData *const d = cmd->get_private_data<JitterContextData>();
	if (d == nullptr)
		return;
	const JitterArm &arm = arm_of(cmd, *d);
	if (arm.serial == 0 || !arm.layers)
		return;
	if (g_scene_jitter.presented_target(dest) || jitter_arm_into_back_buffer(arm, dest))
		layer_window(cmd, *d);
}

void note_scene_target_drawn(command_list *cmd, JitterContextData &d)
{
	const JitterArm &arm = d.arm;
	const JitterListState &s = d.state;
	if (s.target_scene || s.target == 0 || s.depth == 0 || arm.scene_w == 0 || s.target_window ||
		s.target_w != arm.scene_w || s.target_h != arm.scene_h || cmd->get_device()->get_api() != device_api::d3d9)
		return;
	if (arm.scene_depth != 0 ? s.depth != arm.scene_depth : s.depth_w != arm.scene_w || s.depth_h != arm.scene_h)
		return;
	if (jitter_arm_into_back_buffer(arm, s.target))
		return;
	g_scene_jitter.note_scene_target(s.target, s.target_w, s.target_h);
	d.state.target_scene = true;
}

bool window_image(const JitterArm &arm, uint64_t target)
{
	return target != 0 && (g_scene_jitter.swapchain_image(target) || jitter_arm_into_back_buffer(arm, target) ||
		(g_scene_jitter.presented_target(target) && !g_scene_jitter.composite_window(target)));
}

void comp_tick(JitterContextData &d)
{
	const uint64_t frame = scene_snapshot().layer_frame();
	if (d.comp_frame == frame)
		return;
	if (d.window_pending != 0) {
		g_scene_jitter.note_presentation_from(d.window_pending, true);
		g_scene_jitter.note_composite_size(d.window_pending, d.pending_w, d.pending_h);
	}
	d.window_pending = 0;
	d.window_drawn = false;
	for (JitterContextData::CompSeen &e : d.comp_seen)
		e = JitterContextData::CompSeen{};
	d.comp_frame = frame;
}

JitterContextData::CompSeen *comp_find(JitterContextData &d, uint64_t target)
{
	for (JitterContextData::CompSeen &e : d.comp_seen)
		if (e.target == target)
			return &e;
	return nullptr;
}

JitterContextData::CompSeen &comp_entry(JitterContextData &d, uint64_t target)
{
	if (JitterContextData::CompSeen *const e = comp_find(d, target))
		return *e;
	JitterContextData::CompSeen &e = d.comp_seen[d.comp_seen_next++ % JitterContextData::kCompSeen];
	e = JitterContextData::CompSeen{};
	e.target = target;
	return e;
}

bool samples_first(command_list *cmd, uint64_t target)
{
	const device_api api = cmd->get_device()->get_api();
	if (api != device_api::d3d11 && api != device_api::d3d10)
		return true;
	IUnknown *res = nullptr;
	if (api == device_api::d3d11) {
		auto *const ctx = static_cast<ID3D11DeviceContext *>(native_of(cmd));
		ID3D11ShaderResourceView *srv = nullptr;
		if (ctx != nullptr)
			ctx->PSGetShaderResources(0, 1, &srv);
		if (srv != nullptr) {
			ID3D11Resource *r = nullptr;
			srv->GetResource(&r);
			res = r;
			srv->Release();
		}
	} else {
		auto *const dev = static_cast<ID3D10Device *>(native_of(cmd));
		ID3D10ShaderResourceView *srv = nullptr;
		if (dev != nullptr)
			dev->PSGetShaderResources(0, 1, &srv);
		if (srv != nullptr) {
			ID3D10Resource *r = nullptr;
			srv->GetResource(&r);
			res = r;
			srv->Release();
		}
	}
	const bool same = res != nullptr && static_cast<uint64_t>(reinterpret_cast<uintptr_t>(res)) == target;
	if (res != nullptr)
		res->Release();
	return same;
}

bool composite_candidate(command_list *cmd, JitterContextData &d, uint64_t from, uint64_t window)
{
	if (from == 0 || from == window || g_scene_jitter.presented_target(from))
		return false;
	const JitterContextData::CompSeen *const e = comp_find(d, from);
	if (e == nullptr || !e->drawn || !e->first_pass || e->tested)
		return false;
	device *const dev = cmd->get_device();
	if (dev->get_api() == device_api::opengl)
		return false;
	const resource_desc a = dev->get_resource_desc(resource{ from });
	const resource_desc b = dev->get_resource_desc(resource{ window });
	const auto image = [](const resource_desc &r) {
		return r.type == resource_type::texture_2d || r.type == resource_type::surface;
	};
	const bool same_size = a.texture.width == b.texture.width && a.texture.height == b.texture.height;
	return image(a) && image(b) && a.texture.width != 0u && a.texture.height != 0u && a.texture.samples <= 1u &&
		b.texture.samples <= 1u &&
		(!same_size || format_to_typeless(a.texture.format) == format_to_typeless(b.texture.format));
}

bool scaled_composite(const JitterListState &s, const JitterArm &arm)
{
	return arm.display_w != 0u && s.target_w != 0u && (s.target_w != arm.display_w || s.target_h != arm.display_h) &&
		g_scene_jitter.composite_window(s.target);
}

void bind_composite(command_list *cmd, JitterContextData &d, uint64_t target, uint32_t w, uint32_t h)
{
	comp_tick(d);
	d.bound_window_image = window_image(arm_of(cmd, d), target);
	if (d.bound_window_image)
		return;
	d.comp_target = target;
	d.comp_target_frame = d.comp_frame;
	d.comp_w = w;
	d.comp_h = h;
}

void note_window_composite(command_list *cmd, JitterContextData &d, bool fullscreen)
{
	const uint64_t t = d.state.target;
	if (t == 0 || !g_scene_jitter.engaged())
		return;
	comp_tick(d);
	const JitterArm &arm = arm_of(cmd, d);
	if (d.bound_window_image) {
		const bool first = !d.window_drawn;
		d.window_drawn = true;
		const uint64_t from = d.comp_target_frame == d.comp_frame ? d.comp_target : 0u;
		d.window_pending = first && fullscreen && composite_candidate(cmd, d, from, t) && samples_first(cmd, from)
			? from : 0u;
		d.pending_w = d.comp_w;
		d.pending_h = d.comp_h;
		return;
	}
	const bool tested = d.state.depth_test && jitter_scene_depth_bound(d.state, arm);
	JitterContextData::CompSeen &e = comp_entry(d, t);
	if (!e.drawn) {
		e.drawn = true;
		e.first_pass = fullscreen;
	}
	if (tested) {
		e.tested = true;
		if (g_scene_jitter.composite_window(t))
			g_scene_jitter.forget_composite(t);
	}
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
	bool quad_scene = false;
	if (g_scene_jitter.engaged())
		d.all_draws.fetch_add(1u, std::memory_order_relaxed);
	if (g_scene_jitter.engaged() && (g_scene_jitter.scene_known() || d.state.bound_serial != 0)) {
		const bool held = d.state.bound_serial != 0;
		JitterRebind r;
		d.state.scene_tested = g_scene_jitter.scene_tested(arm_of(cmd, d).serial);
		d.state.sky_first = g_scene_jitter.sky_first(arm_of(cmd, d));
		const bool into_window = jitter_draw_into_window(d.state, arm_of(cmd, d));
		const bool depth_bound = fullscreen && jitter_scene_depth_bound(d.state, arm_of(cmd, d));
		over_scene = depth_bound && !into_window;
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
		note_scene_target_drawn(cmd, d);
		d.state.layer = false;
		if (layer_candidate(d, fullscreen, into_window))
			note_layer(cmd, d, fullscreen, into_window);
		scene = jitter_on_draw(d.state, arm_of(cmd, d), fullscreen && !quad_scene, &r);
		if (d.state.target != 0 && d.state.target == d.comp_target && jitter_scene_depth_bound(d.state, arm_of(cmd, d))) {
			quad_facts(cmd, d, false);
			bool enabled = false, compares = false;
			if (jitter_d3d9_depth_state(cmd, &enabled, &compares))
				jitter_on_depth_dynamic(d.state, enabled ? 1 : 0, compares ? 1 : 0);
		}
		note_window_composite(cmd, d, fullscreen);
		if (d.state.layer && !scene)
			scene_snapshot().count_layer();
		if (layer_after_due(cmd, d, scene, into_window))
			layer_after_draw(cmd, d, scene, into_window);
		apply(cmd, d, held, r);
		if (!scene)
			count_other(d, arm_of(cmd, d), fullscreen);
	}
	const JitterArm &arm = arm_of(cmd, d);
	const bool want = d.state.bound_serial != 0 && d.state.app_count != 0;
	const JitterViewport &vp = d.state.app[0];
	const JitterShaderDraw carried = jitter_shader_draw(cmd, want, arm.target_x, arm.target_y, vp.width, vp.height,
		arm.scene_depth == 0, d.state.target == 0 || jitter_draw_into_window(d.state, arm));
	if (carried != JitterShaderDraw::NotADraw)
		note_window_draw(cmd, d, scene && carried != JitterShaderDraw::Interface, fullscreen && !quad_scene);
	if (scene && carried != JitterShaderDraw::NotADraw && carried != JitterShaderDraw::Interface) {
		const bool by_target = d.state.target_scene && d.state.depth == 0;
		const bool by_depth = jitter_scene_depth_bound(d.state, arm);
		if (by_depth && d.state.depth_test) {
			g_scene_jitter.note_scene_tested(arm.serial, jitter_draw_into_window(d.state, arm));
			d.scene_tested_here = arm.serial;
			if (!fullscreen)
				d.state.depth_cleared_serial = 0;
		}
		int proof = -1;
		if (!fullscreen && by_depth && d.state.depth_test && !g_scene_jitter.scene_perspective(arm.serial)) {
			if (!try_scene_projection(d, arm.serial)) {
				proof = static_cast<int>(JitterProofWhy::NotAsked);
			} else {
				const int p = jitter_shader_projection(cmd);
				if (p == 1)
					g_scene_jitter.note_scene_perspective(arm.serial);
				proof = static_cast<int>(p == 1 ? JitterProofWhy::Perspective
					: p == 0 ? JitterProofWhy::Flat : JitterProofWhy::NotRead);
			}
		}
		const std::lock_guard<std::mutex> lock(d.lock);
		if (proof >= 0)
			++d.counts.proof_why[proof];
		count_scene_draw(d.counts, carried == JitterShaderDraw::Offset ? d.state.bound_serial : 0u,
			d.state.depth_aliased);
		if (by_target)
			++d.counts.by_target;
		if (by_depth)
			++d.counts.depth_draws;
		if (by_depth && d.state.depth_test)
			++d.counts.depth_tested;
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
	if (!window || scaled_composite(d.state, arm))
		return;
	const resource target{ d.state.target };
	if (scene) {
		scene_snapshot().spoil(arm.serial, target, fullscreen);
		scene_snapshot().note_scene(arm.serial, target);
	} else if (fullscreen && !scene_snapshot().scene_drawn_into(arm.serial, target) &&
		g_scene_jitter.composite_window(target.handle)) {
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
	if (route == Route::Viewport && g_any_bundle.load(std::memory_order_relaxed) &&
		cmd->get_device()->get_api() == device_api::d3d12) {
		JitterContextData *const b = cmd->get_private_data<JitterContextData>();
		if (b != nullptr && b->bundle == 1) {
			const std::lock_guard<std::mutex> lock(b->lock);
			if (fullscreen) {
				++b->bundle_quads;
			} else {
				++b->bundle_draws;
				if (b->state.depth_test)
					++b->bundle_tested;
			}
			return;
		}
	}
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
	d.state.sky_first = into_window && g_scene_jitter.sky_first(arm_of(cmd, d));
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
		if (projection_read(cmd) && d.state.vertex_class == JitterVertexClass::Transformed) {
			d.state.scene_perspective = g_scene_jitter.scene_perspective(arm_of(cmd, d).serial);
			if (d.state.scene_perspective)
				d.state.projection = static_cast<int8_t>(projection_of(cmd, d));
		}
		if (jitter_quad_is_scene(d.state, arm_of(cmd, d)))
			fullscreen = false;
	}
	bool window_quad = false;
	if (quad && jitter_draw_into_window(d.state, arm_of(cmd, d)) && jitter_scene_depth_bound(d.state, arm_of(cmd, d))) {
		quad_facts(cmd, d, false);
		bool known = false;
		window_quad = jitter_depth_compares(d.state, &known) && known;
		if (window_quad && fullscreen && arm_of(cmd, d).scene_quads && projection_read(cmd)) {
			d.state.perspective = projection_of(cmd, d) == 1;
			forget_clear_of_frame_before(d);
			if (jitter_window_quad_is_scene(d.state, arm_of(cmd, d)))
				fullscreen = false;
		}
	}
	d.state.layer = false;
	if (layer_candidate(d, quad, into_window))
		note_layer(cmd, d, quad, into_window);
	const bool scene = jitter_on_draw(d.state, arm_of(cmd, d), fullscreen, &r);
	note_window_composite(cmd, d, quad);
	if (scene && into_window && !quad)
		scene_snapshot().note_scene_mesh(index);
	if (d.state.layer && !scene)
		scene_snapshot().count_layer();
	if (layer_after_due(cmd, d, scene, into_window))
		layer_after_draw(cmd, d, scene, into_window);
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
		if (by_depth && d.state.depth_test) {
			g_scene_jitter.note_scene_tested(arm_of(cmd, d).serial, jitter_draw_into_window(d.state, arm_of(cmd, d)));
			d.scene_tested_here = arm_of(cmd, d).serial;
			if (!quad)
				d.state.depth_cleared_serial = 0;
		}
		int proof = -1;
		if (!quad && by_depth && d.state.depth_test && projection_read(cmd) &&
			!g_scene_jitter.scene_perspective(arm_of(cmd, d).serial)) {
			if (!try_scene_projection(d, arm_of(cmd, d).serial)) {
				proof = static_cast<int>(JitterProofWhy::NotAsked);
			} else {
				const int p = projection_of(cmd, d);
				if (p == 1)
					g_scene_jitter.note_scene_perspective(arm_of(cmd, d).serial);
				proof = static_cast<int>(p == 1 ? JitterProofWhy::Perspective
					: p == 0 ? JitterProofWhy::Flat : d.projection_why);
			}
		}
		const std::lock_guard<std::mutex> lock(d.lock);
		if (proof >= 0)
			++d.counts.proof_why[proof];
		count_scene_draw(d.counts, d.state.bound_serial, d.state.depth_aliased);
		if (by_target)
			++d.counts.by_target;
		if (by_depth)
			++d.counts.depth_draws;
		if (by_depth && d.state.depth_test)
			++d.counts.depth_tested;
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

void play_bundle(command_list *cmd, JitterContextData &b)
{
	uint32_t draws = 0, tested = 0, quads = 0;
	{
		const std::lock_guard<std::mutex> lock(b.lock);
		draws = b.bundle_draws;
		tested = b.bundle_tested;
		quads = b.bundle_quads;
	}
	if ((draws == 0 && quads == 0) || !g_scene_jitter.engaged() || route_of(cmd) != Route::Viewport)
		return;
	JitterContextData &d = context_of(cmd);
	d.all_draws.fetch_add(draws + quads, std::memory_order_relaxed);
	comp_tick(d);
	if (window_image(arm_of(cmd, d), d.state.target)) {
		d.window_drawn = true;
		d.window_pending = 0;
	}
	if (!g_scene_jitter.scene_known() && d.state.bound_serial == 0)
		return;
	const JitterArm &arm = arm_of(cmd, d);
	const bool held = d.state.bound_serial != 0;
	JitterRebind r;
	d.state.scene_tested = g_scene_jitter.scene_tested(arm.serial);
	const bool list_test = d.state.depth_test;
	if (draws != 0)
		d.state.depth_test = tested != 0;
	const bool scene = jitter_on_draw(d.state, arm, draws == 0, &r);
	d.state.depth_test = list_test;
	apply(cmd, d, held, r);
	if (!scene) {
		count_other(d, arm, draws == 0, draws);
		return;
	}
	const bool by_target = d.state.target_scene && d.state.depth == 0;
	const bool by_depth = jitter_scene_depth_bound(d.state, arm);
	if (by_depth && tested != 0)
		g_scene_jitter.note_scene_tested(arm.serial);
	const std::lock_guard<std::mutex> lock(d.lock);
	count_scene_draw(d.counts, d.state.bound_serial, d.state.depth_aliased, draws);
	if (by_target)
		d.counts.by_target += draws;
	if (by_depth)
		d.counts.depth_draws += draws;
	if (!jitter_draw_into_window(d.state, arm))
		d.counts.outside_draws += draws;
}

JitterDrawCounts take_played(JitterContextData &s)
{
	const std::lock_guard<std::mutex> lock(s.lock);
	s.fold_all_draws();
	const uint64_t frame = g_scene_jitter.frame();
	if (!s.played) {
		s.played = true;
		s.first_played = frame;
		s.recorded = s.counts;
		s.counts = JitterDrawCounts{};
		return s.recorded;
	}
	return s.first_played != frame ? replayed_draw_counts(s.recorded) : s.recorded;
}

void play_secondary(command_list *cmd, JitterContextData &s)
{
	const JitterDrawCounts counts = take_played(s);
	if (!any_draw_counts(counts))
		return;
	JitterContextData &into = context_of(cmd);
	const std::lock_guard<std::mutex> lock(into.lock);
	add_draw_counts(into.counts, counts);
}

bool plays_recorded(command_list *cmd)
{
	const device_api api = cmd->get_device()->get_api();
	return api == device_api::d3d12 || api == device_api::vulkan;
}

void play_list(command_list *cmd)
{
	JitterContextData *const data = cmd->get_private_data<JitterContextData>();
	if (data == nullptr)
		return;
	const JitterDrawCounts counts = take_played(*data);
	if (any_draw_counts(counts))
		g_scene_jitter.hand_on(counts);
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
		context_of(cmd);
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

void push_constants(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t param, uint32_t first,
	uint32_t count, const void *values)
{
	if (cmd == nullptr)
		return;
	if (values == nullptr || !is_d3d12(cmd) ||
		(static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::vertex)) == 0u)
		return;
	if (D3D12RootArgs *const args = root_args_of(cmd))
		args->constants(layout.handle, param, first, count, static_cast<const uint32_t *>(values));
}

void push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t param,
	const descriptor_table_update &update)
{
	if (cmd == nullptr)
		return;
	if (update.type == descriptor_type::constant_buffer && is_d3d12(cmd)) {
		if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::vertex)) == 0u ||
			update.count != 1u || update.descriptors == nullptr)
			return;
		D3D12RootArgs *const args = root_args_of(cmd);
		if (args == nullptr)
			return;
		const auto &range = *static_cast<const buffer_range *>(update.descriptors);
		args->view(layout.handle, param + update.binding, range.buffer.handle, range.offset);
		return;
	}
	if (is_vulkan(cmd) && update.descriptors != nullptr &&
		(update.type == descriptor_type::constant_buffer || update.type == descriptor_type::shader_storage_buffer)) {
		if (VulkanCommandState *const s = vulkan_constants().state(native_of(cmd))) {
			const auto *const ranges = static_cast<const buffer_range *>(update.descriptors);
			for (uint32_t k = 0; k < update.count; ++k) {
				if (update.array_offset + k != 0u)
					continue;
				VulkanBufferRange r;
				r.buffer = ranges[k].buffer.handle;
				r.offset = ranges[k].offset;
				r.range = ranges[k].size;
				s->push_descriptor(param, update.binding, r);
			}
		}
		return;
	}
	if ((update.type != descriptor_type::shader_resource_view &&
			update.type != descriptor_type::texture_shader_resource_view) || !is_d3d11(cmd))
		return;
	regrid_bound(cmd, stages, update.binding, update.count, static_cast<const resource_view *>(update.descriptors));
}

void bind_descriptor_tables(command_list *cmd, shader_stage stages, pipeline_layout layout, uint32_t first,
	uint32_t count, const descriptor_table *tables, uint32_t, const uint32_t *)
{
	if (cmd == nullptr)
		return;
	if (!is_d3d12(cmd) || (static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::vertex)) == 0u)
		return;
	D3D12RootArgs *const args = root_args_of(cmd);
	if (args == nullptr)
		return;
	if (count == 0 || tables == nullptr)
		args->use(layout.handle);
	else
		args->tables(layout.handle, first, count, reinterpret_cast<const uint64_t *>(tables));
}

bool update_descriptor_tables(device *dev, uint32_t count, const descriptor_table_update *updates)
{
	if (dev == nullptr || updates == nullptr || !d3d12_vertex_tables_seen() || dev->get_api() != device_api::d3d12)
		return false;
	D3D12CbvHeaps &cbvs = d3d12_cbv_heaps();
	for (uint32_t i = 0; i < count; ++i) {
		const descriptor_table_update &u = updates[i];
		const bool view = u.type == descriptor_type::constant_buffer;
		if (u.table.handle == 0 || u.count == 0 || u.array_offset != 0 || (!view && !cbvs.any()))
			continue;
		descriptor_heap heap{ 0 };
		uint32_t index = 0;
		dev->get_descriptor_heap_offset(u.table, u.binding, 0u, &heap, &index);
		if (heap.handle == 0)
			continue;
		if (!view || u.descriptors == nullptr) {
			cbvs.cleared(heap.handle, index, u.count);
			continue;
		}
		const auto *const ranges = static_cast<const buffer_range *>(u.descriptors);
		for (uint32_t k = 0; k < u.count; ++k) {
			auto *const buffer = reinterpret_cast<ID3D12Resource *>(static_cast<uintptr_t>(ranges[k].buffer.handle));
			const uint64_t va = buffer != nullptr ? buffer->GetGPUVirtualAddress() + ranges[k].offset : 0u;
			cbvs.written(heap.handle, index + k, va, ranges[k].size);
		}
	}
	return false;
}

bool copy_descriptor_tables(device *dev, uint32_t count, const descriptor_table_copy *copies)
{
	D3D12CbvHeaps &cbvs = d3d12_cbv_heaps();
	if (dev == nullptr || copies == nullptr || !cbvs.any() || dev->get_api() != device_api::d3d12)
		return false;
	for (uint32_t i = 0; i < count; ++i) {
		const descriptor_table_copy &c = copies[i];
		if (c.source_table.handle == 0 || c.dest_table.handle == 0 || c.count == 0 || c.source_array_offset != 0 ||
			c.dest_array_offset != 0)
			continue;
		descriptor_heap src{ 0 }, dst{ 0 };
		uint32_t si = 0, di = 0;
		dev->get_descriptor_heap_offset(c.source_table, c.source_binding, 0u, &src, &si);
		dev->get_descriptor_heap_offset(c.dest_table, c.dest_binding, 0u, &dst, &di);
		if (dst.handle != 0)
			cbvs.copied(dst.handle, di, src.handle, si, c.count);
	}
	return false;
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
				g_scene_jitter.scene_tested_in_frame(a.serial, d.scene_tested_here == a.serial))
			note_depth_cleared(d, a.serial);
	}
	return false;
}

void note_window_copy(command_list *cmd, uint64_t source, uint64_t dest, bool whole_region)
{
	if (dest == 0 || !g_scene_jitter.engaged())
		return;
	JitterContextData *const d = cmd->get_private_data<JitterContextData>();
	if (d == nullptr)
		return;
	comp_tick(*d);
	device *const dev = cmd->get_device();
	const resource_desc sd = dev->get_resource_desc(resource{ source });
	const resource_desc dd = dev->get_resource_desc(resource{ dest });
	const auto image = [](const resource_desc &r) {
		return r.type == resource_type::texture_2d || r.type == resource_type::surface;
	};
	const bool whole = whole_region && source != 0 && image(sd) && image(dd) &&
		sd.texture.width == dd.texture.width && sd.texture.height == dd.texture.height &&
		format_to_typeless(sd.texture.format) == format_to_typeless(dd.texture.format);
	const JitterArm &arm = arm_of(cmd, *d);
	if (window_image(arm, dest)) {
		const bool first = !d->window_drawn;
		d->window_drawn = true;
		d->window_pending = whole && first && composite_candidate(cmd, *d, source, dest) ? source : 0u;
		d->pending_w = sd.texture.width;
		d->pending_h = sd.texture.height;
		return;
	}
	if (whole) {
		JitterContextData::CompSeen &e = comp_entry(*d, dest);
		if (!e.drawn) {
			e.drawn = true;
			e.first_pass = true;
		}
	}
}

void note_copied_into(command_list *cmd, uint64_t dest)
{
	if (is_vulkan(cmd) && followed(cmd))
		g_scene_jitter.note_chain_written(dest);
}

bool copy_resource(command_list *cmd, resource source, resource dest)
{
	if (cmd == nullptr)
		return false;
	note_depth_written(cmd, dest.handle);
	note_copied_into(cmd, dest.handle);
	note_copy_into_window(cmd, dest.handle);
	note_window_copy(cmd, source.handle, dest.handle, true);
	return false;
}

bool copy_texture_region(command_list *cmd, resource source, uint32_t source_subresource,
	const subresource_box *source_box, resource dest, uint32_t dest_subresource, const subresource_box *dest_box,
	filter_mode)
{
	if (cmd == nullptr)
		return false;
	note_depth_written(cmd, dest.handle);
	note_copied_into(cmd, dest.handle);
	note_copy_into_window(cmd, dest.handle);
	note_window_copy(cmd, source.handle, dest.handle,
		source_box == nullptr && dest_box == nullptr && source_subresource == 0 && dest_subresource == 0);
	return false;
}

bool resolve_texture_region(command_list *cmd, resource, uint32_t, const subresource_box *, resource dest, uint32_t,
	uint32_t, uint32_t, uint32_t, format)
{
	if (cmd == nullptr)
		return false;
	note_copied_into(cmd, dest.handle);
	note_copy_into_window(cmd, dest.handle);
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
	if (ds != nullptr && ds->depth_load_op == render_pass_load_op::clear && depth != 0 && g_scene_jitter.engaged()) {
		const JitterArm &a = arm_of(cmd, d);
		if (a.serial != 0 && (depth == a.scene_depth || depth == d.state.depth) &&
			g_scene_jitter.scene_tested_in_frame(a.serial, d.scene_tested_here == a.serial))
			note_depth_cleared(d, a.serial);
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
	JitterContextData &d = context_of(cmd);
	if (is_vulkan(cmd)) {
		vulkan_rendering_ended(native_of(cmd));
		g_scene_jitter.note_chain_written(d.state.target);
	}
	bind_target_and_depth(cmd, d, 0, 0);
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
		d.root.clear();
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

struct WTermsKept {
	bool known = false;
	DxbcWTerms terms{};
};
std::mutex g_wterms_lock;
std::unordered_map<uint64_t, WTermsKept> g_wterms;

WTermsKept d3d12_w_terms_of(const shader_desc &vs)
{
	const uint64_t key = vertex_code_hash(vs);
	{
		const std::lock_guard<std::mutex> lock(g_wterms_lock);
		const auto it = g_wterms.find(key);
		if (it != g_wterms.end())
			return it->second;
	}
	WTermsKept w;
	w.known = dxbc_position_w_terms(vs.code, vs.code_size, &w.terms) ||
		dxil_position_w_terms(vs.code, vs.code_size, &w.terms);
	const std::lock_guard<std::mutex> lock(g_wterms_lock);
	if (g_wterms.size() >= kVertexClassesKept)
		g_wterms.clear();
	g_wterms[key] = w;
	return w;
}

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
	if (api == device_api::d3d12) {
		c = classify_dxbc_vertex_shader(vs.code, vs.code_size);
		if (c == JitterVertexClass::Unknown)
			c = classify_dxil_vertex_shader(vs.code, vs.code_size);
	} else if (api == device_api::vulkan)
		c = classify_spirv_vertex_shader(vs.code, vs.code_size, vs.entry_point);
	else
		c = classify_glsl_vertex_shader(static_cast<const char *>(vs.code), vs.code_size);
	const std::lock_guard<std::mutex> lock(g_vertex_classes_lock);
	if (g_vertex_classes.size() >= kVertexClassesKept)
		g_vertex_classes.clear();
	g_vertex_classes[key] = c;
	return c;
}

struct SpirvTermsRead {
	bool known = false;
	SpirvWTerms terms{};
};
std::mutex g_spirv_terms_lock;
std::unordered_map<uint64_t, SpirvTermsRead> g_spirv_terms;

bool spirv_terms_of(const shader_desc &vs, uint64_t hash, SpirvWTerms *out)
{
	{
		const std::lock_guard<std::mutex> lock(g_spirv_terms_lock);
		const auto it = g_spirv_terms.find(hash);
		if (it != g_spirv_terms.end()) {
			*out = it->second.terms;
			return it->second.known;
		}
	}
	SpirvTermsRead r;
	r.known = spirv_position_w_terms(vs.code, vs.code_size, vs.entry_point, &r.terms);
	const std::lock_guard<std::mutex> lock(g_spirv_terms_lock);
	if (g_spirv_terms.size() >= kVertexClassesKept)
		g_spirv_terms.clear();
	g_spirv_terms[hash] = r;
	*out = r.terms;
	return r.known;
}

void init_pipeline(device *dev, pipeline_layout layout_handle, uint32_t count, const pipeline_subobject *subobjects,
	pipeline pipe)
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
	bool dynamic_enable = false, dynamic_func = false, dynamic_write = false, layout = false, replaced = false;
	for (uint32_t i = 0; i < count; ++i) {
		layout = layout || subobjects[i].type == pipeline_subobject_type::input_layout;
		if (subobjects[i].data == nullptr)
			continue;
		if ((subobjects[i].type == pipeline_subobject_type::geometry_shader ||
				subobjects[i].type == pipeline_subobject_type::hull_shader ||
				subobjects[i].type == pipeline_subobject_type::domain_shader) &&
			subobjects[i].count != 0 && static_cast<const shader_desc *>(subobjects[i].data)->code_size != 0)
			replaced = true;
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
	if (vs != nullptr && api == device_api::d3d12 && vs->code != nullptr && vs->code_size != 0) {
		const WTermsKept w = d3d12_w_terms_of(*vs);
		facts.wterms_known = w.known;
		facts.wterms = w.terms;
		facts.layout = layout_handle.handle;
		facts.position_replaced = replaced;
		if (w.known)
			d3d12_constant_checks().forget_pipeline(pipe.handle);
	}
	if (vs != nullptr && api == device_api::vulkan && vs->code != nullptr && vs->code_size != 0) {
		facts.vertex_hash = vertex_code_hash(*vs);
		facts.spirv_known = spirv_terms_of(*vs, facts.vertex_hash, &facts.spirv);
		facts.position_replaced = replaced;
	}
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
		if (!facts.spirv_known && lib.spirv_known) {
			facts.spirv_known = true;
			facts.spirv = lib.spirv;
			facts.vertex_hash = lib.vertex_hash;
		}
		facts.position_replaced = facts.position_replaced || lib.position_replaced;
		untested = untested || untested_pipeline(libraries[k].handle);
	}
	if (untested)
		note_untested_pipeline(pipe.handle, true);
	note_pipeline_facts(pipe.handle, facts);
}

void destroy_pipeline(device *dev, pipeline pipe)
{
	note_untested_pipeline(pipe.handle, false);
	note_pipeline_facts(pipe.handle, PipelineFacts{});
	constant_reads().forget_shader(pipe.handle);
	if (dev == nullptr || dev->get_api() == device_api::d3d12)
		d3d12_constant_checks().forget_pipeline(pipe.handle);
	if (dev != nullptr && dev->get_api() == device_api::vulkan)
		vulkan_constants().pipeline_destroyed(pipe.handle);
}

void init_pipeline_layout(device *dev, uint32_t count, const pipeline_layout_param *params, pipeline_layout layout)
{
	if (dev == nullptr || dev->get_api() != device_api::d3d12)
		return;
	d3d12_note_layout(layout.handle, count, params);
}

void destroy_pipeline_layout(device *dev, pipeline_layout layout)
{
	if (dev == nullptr || dev->get_api() != device_api::d3d12)
		return;
	d3d12_forget_layout(layout.handle);
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
	if (type == indirect_command::unknown && is_d3d12(cmd)) {
		if (JitterContextData *const d = cmd->get_private_data<JitterContextData>())
			d->root.clear();
	}
	if (type != indirect_command::dispatch)
		any_draw(cmd, false);
	return false;
}

bool dispatch_mesh(command_list *cmd, uint32_t, uint32_t, uint32_t)
{
	if (cmd == nullptr)
		return false;
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
	if (is_d3d12(cmd))
		d3d12_check_submitted(cmd);
	if (is_vulkan(cmd))
		vulkan_constants().submitted(native_of(cmd), g_scene_jitter.frame());
	if (plays_recorded(cmd))
		play_list(cmd);
	else
		hand_on_counts(cmd);
	if (JitterContextData *const d = cmd->get_private_data<JitterContextData>())
		if (d->layer_notes.count != 0)
			scene_snapshot().play(d->layer_notes);
}

void present(command_queue *, swapchain *chain, const rect *, const rect *, uint32_t, const rect *)
{
	const uint64_t image = chain != nullptr ? current_back_buffer_of(chain).handle : 0u;
	if (image != 0 && g_scene_jitter.swapchain_watched() && !g_scene_jitter.swapchain_image(image))
		return;
	scene_snapshot().note_present();
}

void execute_secondary(command_list *cmd, command_list *secondary)
{
	if (cmd == nullptr || secondary == nullptr)
		return;
	if (is_d3d12(cmd))
		context_of(cmd).root.clear();
	if (is_vulkan(cmd))
		vulkan_constants().executed_secondary(native_of(cmd), native_of(secondary));
	if (!is_d3d11(cmd)) {
		JitterContextData *const s = secondary->get_private_data<JitterContextData>();
		if (s == nullptr || !followed(cmd))
			return;
		{
			const std::lock_guard<std::mutex> lock(s->lock);
			if (s->layer_notes.count != 0)
				context_of(cmd).layer_notes.append(s->layer_notes);
		}
		if (s->bundle == 1)
			play_bundle(cmd, *s);
		else if (is_vulkan(cmd))
			play_secondary(cmd, *s);
		return;
	}
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
		list->layer_notes = context->layer_notes;
		return;
	}
	JitterCommandListData *const list = secondary->get_private_data<JitterCommandListData>();
	if (list == nullptr)
		return;
	JitterDrawCounts counts;
	bool replay = false;
	LayerNotes notes;
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
		notes = list->layer_notes;
	}
	if (notes.count != 0) {
		JitterContextData &into = context_of(cmd);
		if (layer_ordered(cmd, into))
			scene_snapshot().play(notes);
		else
			into.layer_notes.append(notes);
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
	if (is_vulkan(cmd))
		vulkan_constants().reset(native_of(cmd));
	JitterContextData *const data = cmd->get_private_data<JitterContextData>();
	if (data == nullptr)
		return;
	data->vk_stamp = 0;
	data->last_pass_target = 0;
	if (data->window_pending != 0) {
		g_scene_jitter.note_presentation_from(data->window_pending, true);
		g_scene_jitter.note_composite_size(data->window_pending, data->pending_w, data->pending_h);
	}
	data->window_pending = 0;
	data->window_drawn = false;
	data->comp_target = data->comp_target_frame = data->comp_frame = 0;
	data->bound_window_image = false;
	data->comp_w = data->comp_h = 0;
	for (JitterContextData::CompSeen &e : data->comp_seen)
		e = JitterContextData::CompSeen{};
	data->root.clear();
	data->scene_tested_here = 0;
	data->layer_notes.clear();
	data->layer_asked_target = data->layer_asked_frame = 0;
	g_scene_jitter.note_bound(data->state.bound_serial != 0, false);
	jitter_on_reset(data->state);
	depth_reset(cmd, *data);
	const std::lock_guard<std::mutex> lock(data->lock);
	data->counts = JitterDrawCounts{};
	data->all_draws.store(0, std::memory_order_relaxed);
	data->closed = false;
	data->pending.clear();
	data->bundle_draws = data->bundle_tested = data->bundle_quads = 0;
	data->played = false;
	data->first_played = 0;
	data->recorded = JitterDrawCounts{};
}

void init_resource(device *dev, const resource_desc &desc, const subresource_data *, resource_usage, resource res)
{
	if ((desc.usage & (resource_usage::render_target | resource_usage::depth_stencil)) != 0) {
		g_scene_jitter.forget_sizes();
		g_scene_jitter.forget_target(res.handle);
	}
	if (desc.type == resource_type::buffer && desc.heap != memory_heap::default_ && desc.heap != memory_heap::readback &&
		desc.heap != memory_heap::unknown && res.handle != 0 && dev != nullptr && dev->get_api() == device_api::d3d12) {
		uint64_t va = 0, bytes = 0;
		if (const void *const cpu = d3d12_map_cpu_buffer(res.handle, &va, &bytes))
			d3d12_buffers().note(res.handle, va, bytes, cpu);
	}
}

void destroy_resource(device *dev, resource res)
{
	if (res.handle != 0 && dev != nullptr && dev->get_api() == device_api::d3d12)
		d3d12_buffers().forget(res.handle);
}

void destroy(command_list *cmd)
{
	if (cmd == nullptr)
		return;
	vulkan_viewport_hook().forget_list(native_of(cmd));
	if (is_vulkan(cmd))
		vulkan_constants().destroyed(native_of(cmd));
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

namespace {

std::atomic<bool> g_d3d12_events{ false };
std::atomic<bool> g_vulkan_seen{ false };

}

void scene_jitter_device_coming(reshade::api::device_api api) noexcept
{
	if (api == reshade::api::device_api::vulkan) {
		g_vulkan_seen.store(true);
		return;
	}
	if (api != reshade::api::device_api::d3d12 || g_vulkan_seen.load() || g_d3d12_events.exchange(true))
		return;
	reshade::register_event<reshade::addon_event::update_descriptor_tables>(jitter_events::update_descriptor_tables);
	reshade::register_event<reshade::addon_event::copy_descriptor_tables>(jitter_events::copy_descriptor_tables);
	reshade::register_event<reshade::addon_event::bind_descriptor_tables>(jitter_events::bind_descriptor_tables);
}

void register_scene_jitter_hooks() noexcept
{
	register_jitter_shader_hooks();
	constant_reads_forget(0);
	d3d12_constant_checks().clear();
	d3d12_buffers().clear();
	d3d12_cbv_heaps().clear();
	d3d12_forget_layouts();
	g_d3d12_events.store(false);
	g_vulkan_seen.store(false);
	reshade::register_event<reshade::addon_event::init_command_list>(jitter_events::init);
	reshade::register_event<reshade::addon_event::bind_viewports>(jitter_events::bind_viewports);
	reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(jitter_events::bind_render_targets);
	reshade::register_event<reshade::addon_event::push_descriptors>(jitter_events::push_descriptors);
	reshade::register_event<reshade::addon_event::init_pipeline_layout>(jitter_events::init_pipeline_layout);
	reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(jitter_events::destroy_pipeline_layout);
	reshade::register_event<reshade::addon_event::push_constants>(jitter_events::push_constants);
	reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(jitter_events::clear_depth_stencil);
	reshade::register_event<reshade::addon_event::copy_resource>(jitter_events::copy_resource);
	reshade::register_event<reshade::addon_event::copy_texture_region>(jitter_events::copy_texture_region);
	reshade::register_event<reshade::addon_event::resolve_texture_region>(jitter_events::resolve_texture_region);
	reshade::register_event<reshade::addon_event::destroy_device>(jitter_events::destroy_device);
	reshade::register_event<reshade::addon_event::begin_render_pass>(jitter_events::begin_render_pass);
	reshade::register_event<reshade::addon_event::end_render_pass>(jitter_events::end_render_pass);
	reshade::register_event<reshade::addon_event::bind_pipeline>(jitter_events::bind_pipeline);
	reshade::register_event<reshade::addon_event::draw>(jitter_events::draw);
	reshade::register_event<reshade::addon_event::draw_indexed>(jitter_events::draw_indexed);
	reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(jitter_events::draw_indirect);
	reshade::register_event<reshade::addon_event::dispatch_mesh>(jitter_events::dispatch_mesh);
	reshade::register_event<reshade::addon_event::close_command_list>(jitter_events::close);
	reshade::register_event<reshade::addon_event::execute_command_list>(jitter_events::execute);
	reshade::register_event<reshade::addon_event::execute_secondary_command_list>(jitter_events::execute_secondary);
	reshade::register_event<reshade::addon_event::reset_command_list>(jitter_events::reset);
	reshade::register_event<reshade::addon_event::present>(jitter_events::present);
	reshade::register_event<reshade::addon_event::destroy_command_list>(jitter_events::destroy);
	reshade::register_event<reshade::addon_event::init_resource>(jitter_events::init_resource);
	reshade::register_event<reshade::addon_event::destroy_resource>(jitter_events::destroy_resource);
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
	case device_api::d3d10:
	case device_api::d3d9:
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
	const bool d3d9_surface = like.type == resource_type::surface && device->get_api() == device_api::d3d9;
	if ((like.type != resource_type::texture_2d && !d3d9_surface) || like.texture.samples == 0) {
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

void SceneSnapshot::note_scene_tested() noexcept
{
	const uint64_t frame = layer_frame();
	if (tested_frame_.load(std::memory_order_relaxed) != frame)
		tested_frame_.store(frame, std::memory_order_release);
}

bool SceneSnapshot::scene_tested() const noexcept
{
	return tested_frame_.load(std::memory_order_acquire) == layer_frame();
}

void SceneSnapshot::note_window_written() noexcept
{
	const uint64_t frame = layer_frame();
	if (written_frame_.load(std::memory_order_relaxed) == frame || tested_frame_.load(std::memory_order_acquire) != frame)
		return;
	written_frame_.store(frame, std::memory_order_release);
}

bool SceneSnapshot::window_written() const noexcept
{
	return written_frame_.load(std::memory_order_acquire) == layer_frame();
}

uint32_t SceneSnapshot::find_mark(uint64_t target) const noexcept
{
	for (uint32_t i = 0; i < kMarks; ++i)
		if (mark_target_[i].load(std::memory_order_acquire) == target)
			return i;
	return kMarks;
}

uint32_t SceneSnapshot::mark_slot(uint64_t target, bool make) noexcept
{
	if (target == 0)
		return kMarks;
	const uint32_t at = find_mark(target);
	if (at != kMarks || !make)
		return at;
	const uint32_t slot = mark_next_.fetch_add(1u, std::memory_order_relaxed) % kMarks;
	mark_target_[slot].store(0, std::memory_order_release);
	mark_before_[slot].store(0, std::memory_order_release);
	mark_after_[slot].store(0, std::memory_order_release);
	mark_target_[slot].store(target, std::memory_order_release);
	return slot;
}

void SceneSnapshot::note_before_window(uint64_t target) noexcept
{
	const uint32_t slot = mark_slot(target, true);
	const uint64_t frame = layer_frame();
	if (slot != kMarks && mark_before_[slot].load(std::memory_order_relaxed) != frame)
		mark_before_[slot].store(frame, std::memory_order_release);
}

void SceneSnapshot::note_after_window(uint64_t target) noexcept
{
	const uint32_t slot = mark_slot(target, true);
	const uint64_t frame = layer_frame();
	if (slot != kMarks && mark_after_[slot].load(std::memory_order_relaxed) != frame)
		mark_after_[slot].store(frame, std::memory_order_release);
}

namespace {

bool lately(uint64_t marked, uint64_t frame) noexcept
{
	return marked != 0 && marked <= frame && frame - marked <= SceneSnapshot::kLayerFrames;
}

}

bool SceneSnapshot::layer_target(uint64_t target) const noexcept
{
	if (target == 0 || !window_written())
		return false;
	const uint32_t slot = find_mark(target);
	return slot == kMarks || !lately(mark_before_[slot].load(std::memory_order_acquire), layer_frame());
}

bool SceneSnapshot::layer_played(uint64_t target) const noexcept
{
	if (target == 0)
		return false;
	const uint32_t slot = find_mark(target);
	if (slot == kMarks)
		return false;
	const uint64_t frame = layer_frame();
	return lately(mark_after_[slot].load(std::memory_order_acquire), frame) &&
		!lately(mark_before_[slot].load(std::memory_order_acquire), frame);
}

void SceneSnapshot::play(const LayerNotes &notes) noexcept
{
	for (uint32_t i = 0; i < notes.count; ++i) {
		switch (notes.kind[i]) {
		case LayerNotes::kTested:
			note_scene_tested();
			break;
		case LayerNotes::kWindow:
			note_window_written();
			break;
		default:
			if (window_written())
				note_after_window(notes.target[i]);
			else
				note_before_window(notes.target[i]);
			break;
		}
	}
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
	c.layer = layer_.exchange(0u, relaxed);
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
