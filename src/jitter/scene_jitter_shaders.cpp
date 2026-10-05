#include "aeon_sr/jitter/scene_jitter_shaders.hpp"

#include "aeon_sr/jitter/shader_offset.hpp"

#include <d3d10.h>
#include <d3d9.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace aeon_sr {
namespace {

using namespace reshade::api;

std::atomic<bool> g_wanted{ true };
std::atomic<uint32_t> g_rewritten{ 0 };
std::atomic<uint32_t> g_kept{ 0 };
std::atomic<uint32_t> g_unwanted{ 0 };
std::atomic<uint32_t> g_indexed{ 0 };
std::atomic<uint32_t> g_indexed_held{ 0 };
std::atomic<uint32_t> g_pretransformed{ 0 };
std::atomic<bool> g_any{ false };

std::shared_mutex g_registry_lock;
std::unordered_map<uint64_t, uint32_t> g_registry;
std::atomic<uint64_t> g_generation{ 1 };

std::atomic<uint32_t> g_d3d9_regs[kD3D9OffsetConstants / 32u]{};

constexpr uint32_t kIndexedEntry = 0x80000000u;

std::atomic<uint32_t> g_game_regs[kD3D9OffsetConstants / 32u]{};

bool game_wrote(uint32_t r) noexcept
{
	return r < kD3D9OffsetConstants && (g_game_regs[r / 32u].load() & (1u << (r % 32u))) != 0;
}
void mark_game_reg(uint32_t r) noexcept
{
	if (r < kD3D9OffsetConstants)
		g_game_regs[r / 32u].fetch_or(1u << (r % 32u));
}

uint32_t count_trailing_zeros(uint32_t v) noexcept
{
	uint32_t n = 0;
	while ((v & 1u) == 0u && n < 32u) {
		v >>= 1;
		++n;
	}
	return n;
}

bool is_d3d9_reg(uint32_t r) noexcept
{
	return r < kD3D9OffsetConstants && (g_d3d9_regs[r / 32u].load() & (1u << (r % 32u))) != 0;
}

thread_local std::vector<uint8_t> t_code;
thread_local const void *t_code_ptr = nullptr;
thread_local uint32_t t_index = 0;
thread_local bool t_indexed = false;

uint32_t registered(uint64_t handle)
{
	if (handle == 0)
		return 0;
	std::shared_lock<std::shared_mutex> lock(g_registry_lock);
	const auto it = g_registry.find(handle);
	return it != g_registry.end() ? it->second : 0u;
}

struct KnownReg {
	bool ours = false;
	float game[4]{};
	bool game_set = false;
};

enum class DrawCarries { Grid, Offset, Nothing };

struct __declspec(uuid("6d0e2b1c-8a47-4f35-9c21-5b7e3a90d4f8")) ShaderJitterState {
	uint64_t vs = 0;
	uint64_t looked_up = ~0ull;
	uint64_t looked_up_generation = 0;
	uint32_t entry = 0;
	bool gs = false;

	bool pure = false;
	bool stream_out = false;
	KnownReg known[kD3D9OffsetConstants];
	uint32_t ours_mask[kD3D9OffsetConstants / 32u]{};
	D3DMATRIX ff_game{};
	D3DMATRIX ff_ours{};
	bool ff_ours_set = false;
	uint64_t ff_frame = ~0ull;
	uint64_t decl = 0;
	bool decl_pretransformed = false;

	ID3D10Buffer *cb_zero = nullptr;
	ID3D10Buffer *cb_moved = nullptr;
	bool buffers_failed = false;
	float moved_xy[2]{};
	bool moved_set = false;
	uint32_t displaced = 0;
	ID3D10Buffer *game_cb[kD3D10OffsetSlots]{};
	uint8_t ours_at[kD3D10OffsetSlots]{};

	~ShaderJitterState()
	{
		for (uint32_t s = 0; s < kD3D10OffsetSlots; ++s) {
			if (game_cb[s] != nullptr)
				game_cb[s]->Release();
		}
		if (cb_zero != nullptr)
			cb_zero->Release();
		if (cb_moved != nullptr)
			cb_moved->Release();
	}
};

ShaderJitterState &state_of(command_list *cmd)
{
	ShaderJitterState *s = cmd->get_private_data<ShaderJitterState>();
	if (s == nullptr)
		s = cmd->create_private_data<ShaderJitterState>();
	return *s;
}

bool lookup(ShaderJitterState &s, uint64_t vs)
{
	const uint64_t generation = g_generation.load();
	if (vs == s.looked_up && generation == s.looked_up_generation)
		return false;
	s.looked_up = vs;
	s.looked_up_generation = generation;
	s.entry = registered(vs);
	return true;
}

void give_back_regs(IDirect3DDevice9 *dev, ShaderJitterState &s, uint32_t keep)
{
	for (uint32_t w = 0; w < kD3D9OffsetConstants / 32u; ++w) {
		uint32_t bits = s.ours_mask[w];
		while (bits != 0) {
			const uint32_t b = count_trailing_zeros(bits);
			bits &= bits - 1u;
			const uint32_t reg = w * 32u + b;
			if (reg == keep)
				continue;
			KnownReg &k = s.known[reg];
			static const float kZero[4] = {};
			dev->SetVertexShaderConstantF(reg, k.game_set ? k.game : kZero, 1);
			k.ours = false;
			s.ours_mask[w] &= ~(1u << b);
		}
	}
}

void set_offset_reg(IDirect3DDevice9 *dev, ShaderJitterState &s, uint32_t reg, const float v[4])
{
	KnownReg &k = s.known[reg];
	if (!k.ours) {
		float now[4];
		if (!s.pure && SUCCEEDED(dev->GetVertexShaderConstantF(reg, now, 1))) {
			std::memcpy(k.game, now, sizeof(now));
			k.game_set = true;
		}
		k.ours = true;
		s.ours_mask[reg / 32u] |= 1u << (reg % 32u);
	}
	dev->SetVertexShaderConstantF(reg, v, 1);
}

bool pretransformed(IDirect3DDevice9 *dev, ShaderJitterState &s)
{
	DWORD fvf = 0;
	if (SUCCEEDED(dev->GetFVF(&fvf)) && fvf != 0)
		return (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
	IDirect3DVertexDeclaration9 *decl = nullptr;
	if (FAILED(dev->GetVertexDeclaration(&decl)) || decl == nullptr)
		return false;
	const uint64_t handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(decl));
	if (handle != s.decl) {
		s.decl = handle;
		s.decl_pretransformed = false;
		D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
		UINT n = MAXD3DDECLLENGTH + 1;
		if (SUCCEEDED(decl->GetDeclaration(elements, &n))) {
			for (UINT i = 0; i < n && elements[i].Stream != 0xFF; ++i) {
				if (elements[i].Usage == D3DDECLUSAGE_POSITIONT)
					s.decl_pretransformed = true;
			}
		}
	}
	decl->Release();
	return s.decl_pretransformed;
}

bool fixed_function_draw(IDirect3DDevice9 *dev, ShaderJitterState &s, bool moved, ClipOffset o)
{
	if (!moved && !s.ff_ours_set)
		return false;
	D3DMATRIX now{};
	if (pretransformed(dev, s) || FAILED(dev->GetTransform(D3DTS_PROJECTION, &now))) {
		if (moved)
			g_pretransformed.fetch_add(1);
		if (s.ff_ours_set) {
			dev->SetTransform(D3DTS_PROJECTION, &s.ff_game);
			s.ff_ours_set = false;
		}
		return false;
	}
	if (s.ff_ours_set && std::memcmp(&now, &s.ff_ours, sizeof(now)) == 0)
		now = s.ff_game;
	else
		s.ff_ours_set = false;
	s.ff_game = now;
	if (!moved) {
		if (s.ff_ours_set) {
			dev->SetTransform(D3DTS_PROJECTION, &s.ff_game);
			s.ff_ours_set = false;
		}
		return false;
	}
	D3DMATRIX want{};
	offset_projection(&now.m[0][0], o, &want.m[0][0]);
	if (!s.ff_ours_set || std::memcmp(&want, &s.ff_ours, sizeof(want)) != 0) {
		dev->SetTransform(D3DTS_PROJECTION, &want);
		s.ff_ours = want;
		s.ff_ours_set = true;
	}
	g_any.store(true);
	return true;
}

bool tests_no_depth(IDirect3DDevice9 *dev, bool scene_by_size)
{
	DWORD z = D3DZB_TRUE;
	if (FAILED(dev->GetRenderState(D3DRS_ZENABLE, &z)))
		return false;
	bool none = z == D3DZB_FALSE;
	if (!none) {
		DWORD func = D3DCMP_LESSEQUAL, write = TRUE;
		none = SUCCEEDED(dev->GetRenderState(D3DRS_ZFUNC, &func)) &&
			SUCCEEDED(dev->GetRenderState(D3DRS_ZWRITEENABLE, &write)) && func == D3DCMP_ALWAYS && write == FALSE;
	}
	if (!none || !scene_by_size)
		return none;
	IDirect3DSurface9 *depth = nullptr;
	if (FAILED(dev->GetDepthStencilSurface(&depth)) || depth == nullptr)
		return false;
	depth->Release();
	return true;
}

DrawCarries d3d9_draw(command_list *cmd, ShaderJitterState &s, bool moved, ClipOffset o, bool scene_by_size)
{
	auto *const dev = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(cmd->get_native()));
	uint64_t vs = s.vs;
	if (!s.pure) {
		IDirect3DVertexShader9 *bound = nullptr;
		if (FAILED(dev->GetVertexShader(&bound))) {
			s.pure = true;
		} else {
			vs = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(bound));
			if (bound != nullptr)
				bound->Release();
		}
	}
	lookup(s, vs);
	const bool process = s.stream_out;
	const bool overlay = moved && !process && tests_no_depth(dev, scene_by_size);
	if (process || overlay)
		moved = false;
	if (vs == 0) {
		give_back_regs(dev, s, ~0u);
		const bool carried = fixed_function_draw(dev, s, moved, o);
		return process || overlay ? DrawCarries::Nothing : carried ? DrawCarries::Offset : DrawCarries::Grid;
	}
	if (s.ff_ours_set) {
		dev->SetTransform(D3DTS_PROJECTION, &s.ff_game);
		s.ff_ours_set = false;
	}
	if (s.entry == 0) {
		give_back_regs(dev, s, ~0u);
		return process || overlay ? DrawCarries::Nothing : DrawCarries::Grid;
	}
	const uint32_t reg = (s.entry & ~kIndexedEntry) - 1u;
	if ((s.entry & kIndexedEntry) != 0u && !s.known[reg].ours) {
		float now[4];
		if (!s.pure && SUCCEEDED(dev->GetVertexShaderConstantF(reg, now, 1)) &&
			(now[0] != 0.0f || now[1] != 0.0f || now[2] != 0.0f || now[3] != 0.0f))
			mark_game_reg(reg);
	}
	if ((s.entry & kIndexedEntry) != 0u && game_wrote(reg)) {
		give_back_regs(dev, s, ~0u);
		if (moved && !process)
			g_indexed_held.fetch_add(1);
		return process || overlay ? DrawCarries::Nothing : DrawCarries::Grid;
	}
	give_back_regs(dev, s, reg);
	const float v[4] = { moved ? o.x : 0.0f, moved ? o.y : 0.0f, 0.0f, 0.0f };
	set_offset_reg(dev, s, reg, v);
	return process || overlay ? DrawCarries::Nothing : moved ? DrawCarries::Offset : DrawCarries::Grid;
}

void give_back_slots(ID3D10Device *dev, ShaderJitterState &s, uint32_t keep)
{
	for (uint32_t slot = 0; s.displaced != 0 && slot < kD3D10OffsetSlots; ++slot) {
		const uint32_t bit = 1u << slot;
		if ((s.displaced & bit) == 0 || slot == keep)
			continue;
		dev->VSSetConstantBuffers(slot, 1, &s.game_cb[slot]);
		if (s.game_cb[slot] != nullptr) {
			s.game_cb[slot]->Release();
			s.game_cb[slot] = nullptr;
		}
		s.displaced &= ~bit;
		s.ours_at[slot] = 0;
	}
}

void forget_slot(ShaderJitterState &s, uint32_t slot)
{
	if (slot >= kD3D10OffsetSlots)
		return;
	if (s.game_cb[slot] != nullptr) {
		s.game_cb[slot]->Release();
		s.game_cb[slot] = nullptr;
	}
	s.displaced &= ~(1u << slot);
	s.ours_at[slot] = 0;
}

bool make_buffers(ID3D10Device *dev, ShaderJitterState &s)
{
	if (s.cb_zero != nullptr && s.cb_moved != nullptr)
		return true;
	if (s.buffers_failed)
		return false;
	D3D10_BUFFER_DESC desc{};
	desc.ByteWidth = 16;
	desc.Usage = D3D10_USAGE_DEFAULT;
	desc.BindFlags = D3D10_BIND_CONSTANT_BUFFER;
	const float zero[4]{};
	D3D10_SUBRESOURCE_DATA init{ zero, 16, 16 };
	if (FAILED(dev->CreateBuffer(&desc, &init, &s.cb_zero)) || FAILED(dev->CreateBuffer(&desc, &init, &s.cb_moved))) {
		s.buffers_failed = true;
		return false;
	}
	s.moved_set = false;
	return true;
}

DrawCarries d3d10_draw(command_list *cmd, ShaderJitterState &s, bool moved, ClipOffset o)
{
	auto *const dev = reinterpret_cast<ID3D10Device *>(static_cast<uintptr_t>(cmd->get_native()));
	lookup(s, s.vs);
	if (s.entry == 0) {
		give_back_slots(dev, s, ~0u);
		return DrawCarries::Grid;
	}
	const uint32_t slot = s.entry - 1u;
	give_back_slots(dev, s, slot);
	const bool buffers = make_buffers(dev, s);
	moved = moved && !s.gs && buffers;
	if (moved && (!s.moved_set || s.moved_xy[0] != o.x || s.moved_xy[1] != o.y)) {
		const float v[4] = { o.x, o.y, 0.0f, 0.0f };
		dev->UpdateSubresource(s.cb_moved, 0, nullptr, v, 16, 16);
		s.moved_xy[0] = o.x;
		s.moved_xy[1] = o.y;
		s.moved_set = true;
	}
	const uint32_t bit = 1u << slot;
	if ((s.displaced & bit) == 0) {
		ID3D10Buffer *game = nullptr;
		dev->VSGetConstantBuffers(slot, 1, &game);
		if (game != nullptr && (game == s.cb_zero || game == s.cb_moved)) {
			game->Release();
			game = nullptr;
		}
		s.game_cb[slot] = game;
		s.displaced |= bit;
		s.ours_at[slot] = 0;
	}
	const uint8_t tag = !buffers ? 3u : moved ? 2u : 1u;
	if (s.ours_at[slot] != tag) {
		ID3D10Buffer *const buf = !buffers ? nullptr : moved ? s.cb_moved : s.cb_zero;
		dev->VSSetConstantBuffers(slot, 1, &buf);
		s.ours_at[slot] = tag;
	}
	return moved ? DrawCarries::Offset : DrawCarries::Grid;
}

bool rewrite(device *device, shader_desc &desc)
{
	if (desc.code == nullptr || desc.code_size == 0)
		return false;
	const device_api api = device->get_api();
	uint32_t index = 0;
	if (api == device_api::d3d9) {
		auto *const dev = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(device->get_native()));
		D3DCAPS9 caps{};
		uint32_t limit = kD3D9OffsetConstants;
		if (SUCCEEDED(dev->GetDeviceCaps(&caps)) && caps.MaxVertexShaderConst != 0 &&
			caps.MaxVertexShaderConst < limit)
			limit = caps.MaxVertexShaderConst;
		std::vector<uint32_t> out;
		bool indexed = false;
		if (!offset_d3d9_vertex_shader(static_cast<const uint32_t *>(desc.code), desc.code_size / 4u, limit, out,
				&index, &indexed))
			return false;
		IDirect3DVertexShader9 *trial = nullptr;
		if (FAILED(dev->CreateVertexShader(reinterpret_cast<const DWORD *>(out.data()), &trial)) || trial == nullptr)
			return false;
		trial->Release();
		t_code.resize(out.size() * 4u);
		std::memcpy(t_code.data(), out.data(), t_code.size());
		g_d3d9_regs[index / 32u].fetch_or(1u << (index % 32u));
		t_indexed = indexed;
	} else {
		t_indexed = false;
		auto *const dev = reinterpret_cast<ID3D10Device *>(static_cast<uintptr_t>(device->get_native()));
		std::vector<uint8_t> out;
		if (!offset_dxbc_vertex_shader(desc.code, desc.code_size, out, &index))
			return false;
		ID3D10VertexShader *trial = nullptr;
		if (FAILED(dev->CreateVertexShader(out.data(), out.size(), &trial)) || trial == nullptr)
			return false;
		trial->Release();
		t_code = std::move(out);
	}
	desc.code = t_code.data();
	desc.code_size = t_code.size();
	t_code_ptr = t_code.data();
	t_index = index;
	return true;
}

}

bool jitter_in_shaders(device_api api) noexcept
{
	return api == device_api::d3d9 || api == device_api::d3d10;
}

void set_jitter_shaders_wanted(bool wanted) noexcept
{
	g_wanted.store(wanted);
}

JitterShaderCounts jitter_shader_counts() noexcept
{
	JitterShaderCounts c;
	c.rewritten = g_rewritten.load();
	c.kept = g_kept.load();
	c.unwanted = g_unwanted.load();
	c.indexed = g_indexed.load();
	c.indexed_held = g_indexed_held.load();
	c.pretransformed = g_pretransformed.load();
	return c;
}

JitterShaderDraw jitter_shader_draw(command_list *cmd, bool moved, float tx, float ty, float vw, float vh,
	bool scene_by_size) noexcept
{
	if (!moved && !g_any.load())
		return JitterShaderDraw::Grid;
	ShaderJitterState &s = state_of(cmd);
	const ClipOffset o = jitter_clip_offset(tx, ty, vw, vh);
	DrawCarries c = DrawCarries::Grid;
	switch (cmd->get_device()->get_api()) {
	case device_api::d3d9:
		c = d3d9_draw(cmd, s, moved, o, scene_by_size);
		break;
	case device_api::d3d10:
		c = d3d10_draw(cmd, s, moved, o);
		break;
	default:
		break;
	}
	return c == DrawCarries::Offset ? JitterShaderDraw::Offset
		: c == DrawCarries::Nothing ? JitterShaderDraw::NotADraw : JitterShaderDraw::Grid;
}

bool jitter_d3d9_bound(command_list *cmd, uint64_t *target, uint64_t *depth) noexcept
{
	*target = 0;
	*depth = 0;
	if (cmd == nullptr || cmd->get_device()->get_api() != device_api::d3d9)
		return false;
	auto *const dev = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(cmd->get_native()));
	IDirect3DSurface9 *rt = nullptr;
	if (FAILED(dev->GetRenderTarget(0, &rt)) || rt == nullptr)
		return false;
	*target = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(rt));
	rt->Release();
	IDirect3DSurface9 *ds = nullptr;
	if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds != nullptr) {
		*depth = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ds));
		ds->Release();
	}
	return true;
}

namespace jitter_shader_events {

bool create_pipeline(device *device, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects)
{
	t_code_ptr = nullptr;
	if (!jitter_in_shaders(device->get_api()))
		return false;
	for (uint32_t i = 0; i < count; ++i) {
		if (subobjects[i].type != pipeline_subobject_type::vertex_shader || subobjects[i].data == nullptr)
			continue;
		if (!g_wanted.load()) {
			g_unwanted.fetch_add(1);
			return false;
		}
		if (!rewrite(device, *static_cast<shader_desc *>(subobjects[i].data))) {
			g_kept.fetch_add(1);
			return false;
		}
		return true;
	}
	return false;
}

void init_pipeline(device *device, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects, pipeline pipeline)
{
	if (!jitter_in_shaders(device->get_api()))
		return;
	for (uint32_t i = 0; i < count; ++i) {
		if (subobjects[i].type != pipeline_subobject_type::vertex_shader || subobjects[i].data == nullptr)
			continue;
		const auto *const desc = static_cast<const shader_desc *>(subobjects[i].data);
		const bool ours = t_code_ptr != nullptr && desc->code == t_code_ptr;
		{
			std::unique_lock<std::shared_mutex> lock(g_registry_lock);
			if (ours)
				g_registry[pipeline.handle] = (t_index + 1u) | (t_indexed ? kIndexedEntry : 0u);
			else
				g_registry.erase(pipeline.handle);
			g_generation.fetch_add(1);
		}
		if (ours) {
			g_rewritten.fetch_add(1);
			if (t_indexed)
				g_indexed.fetch_add(1);
			g_any.store(true);
		}
		break;
	}
	t_code_ptr = nullptr;
}

void destroy_pipeline(device *device, pipeline pipeline)
{
	if (!jitter_in_shaders(device->get_api()))
		return;
	std::unique_lock<std::shared_mutex> lock(g_registry_lock);
	if (g_registry.erase(pipeline.handle) != 0)
		g_generation.fetch_add(1);
}

void bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pipeline)
{
	const auto bits = static_cast<uint32_t>(stages);
	const bool cleared = stages == pipeline_stage::all && pipeline.handle == 0;
	if (!cleared && (bits & (static_cast<uint32_t>(pipeline_stage::vertex_shader) |
		static_cast<uint32_t>(pipeline_stage::geometry_shader))) == 0)
		return;
	if (!jitter_in_shaders(cmd->get_device()->get_api()))
		return;
	ShaderJitterState &s = state_of(cmd);
	if (cleared) {
		s.vs = 0;
		s.gs = false;
		for (uint32_t slot = 0; slot < kD3D10OffsetSlots; ++slot)
			forget_slot(s, slot);
		return;
	}
	if ((bits & static_cast<uint32_t>(pipeline_stage::vertex_shader)) != 0)
		s.vs = pipeline.handle;
	if ((bits & static_cast<uint32_t>(pipeline_stage::geometry_shader)) != 0)
		s.gs = pipeline.handle != 0;
}

void push_constants(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t param, uint32_t first,
	uint32_t count, const void *values)
{
	if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::vertex)) == 0 || param != 2u ||
		values == nullptr || cmd->get_device()->get_api() != device_api::d3d9)
		return;
	const uint32_t reg_first = first / 4u;
	const uint32_t reg_end = (first + count) / 4u;
	for (uint32_t r = reg_first; r < reg_end && r < kD3D9OffsetConstants; ++r)
		mark_game_reg(r);
	ShaderJitterState *s = nullptr;
	for (uint32_t r = reg_first; r < reg_end && r < kD3D9OffsetConstants; ++r) {
		if (!is_d3d9_reg(r))
			continue;
		if (s == nullptr)
			s = &state_of(cmd);
		KnownReg &k = s->known[r];
		std::memcpy(k.game, static_cast<const float *>(values) + (r - reg_first) * 4u, sizeof(k.game));
		k.game_set = true;
		k.ours = false;
		s->ours_mask[r / 32u] &= ~(1u << (r % 32u));
	}
}

void bind_stream_output(command_list *cmd, uint32_t first, uint32_t count, const resource *buffers,
	const uint64_t *, const uint64_t *, const resource *, const uint64_t *)
{
	if (first != 0 || count == 0 || buffers == nullptr || cmd->get_device()->get_api() != device_api::d3d9)
		return;
	state_of(cmd).stream_out = buffers[0].handle != 0;
}

void push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t,
	const descriptor_table_update &update)
{
	if ((static_cast<uint32_t>(stages) & static_cast<uint32_t>(shader_stage::vertex)) == 0 ||
		update.type != descriptor_type::constant_buffer || cmd->get_device()->get_api() != device_api::d3d10)
		return;
	ShaderJitterState *const s = cmd->get_private_data<ShaderJitterState>();
	if (s == nullptr || s->displaced == 0)
		return;
	for (uint32_t i = 0; i < update.count; ++i)
		forget_slot(*s, update.binding + i);
}

void forget_game_registers() noexcept
{
	for (auto &w : g_game_regs)
		w.store(0);
}

void destroy_command_list(command_list *cmd)
{
	if (cmd->get_private_data<ShaderJitterState>() != nullptr)
		cmd->destroy_private_data<ShaderJitterState>();
}

}

void register_jitter_shader_hooks() noexcept
{
	reshade::register_event<reshade::addon_event::create_pipeline>(jitter_shader_events::create_pipeline);
	reshade::register_event<reshade::addon_event::init_pipeline>(jitter_shader_events::init_pipeline);
	reshade::register_event<reshade::addon_event::destroy_pipeline>(jitter_shader_events::destroy_pipeline);
	reshade::register_event<reshade::addon_event::bind_pipeline>(jitter_shader_events::bind_pipeline);
	reshade::register_event<reshade::addon_event::push_constants>(jitter_shader_events::push_constants);
	reshade::register_event<reshade::addon_event::push_descriptors>(jitter_shader_events::push_descriptors);
	reshade::register_event<reshade::addon_event::bind_stream_output_buffers>(jitter_shader_events::bind_stream_output);
	reshade::register_event<reshade::addon_event::destroy_command_list>(jitter_shader_events::destroy_command_list);
}

}
