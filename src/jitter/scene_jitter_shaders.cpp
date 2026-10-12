#include "aeon_sr/jitter/scene_jitter_shaders.hpp"

#include "aeon_sr/jitter/constant_reads.hpp"
#include "aeon_sr/jitter/scene_jitter_hooks.hpp"
#include "aeon_sr/jitter/shader_offset.hpp"

#include <d3d10.h>
#include <d3d9.h>

#include <atomic>
#include <cmath>
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
std::unordered_map<uint64_t, D3D9WTerms> g_wterms;
std::unordered_map<uint64_t, DxbcWTerms> g_dxbc_wterms;
constexpr uint32_t kSlotMask = 0xFFFFu;
constexpr uint32_t kClassShift = 28u;

uint32_t slot_of(uint32_t entry) noexcept
{
	return entry & kSlotMask;
}

JitterVertexClass class_of(uint32_t entry) noexcept
{
	return static_cast<JitterVertexClass>((entry >> kClassShift) & 3u);
}

uint32_t class_bits(JitterVertexClass c) noexcept
{
	return static_cast<uint32_t>(c) << kClassShift;
}

std::atomic<uint32_t> g_d3d9_regs[kD3D9OffsetConstants / 32u]{};

constexpr uint32_t kIndexedEntry = 0x80000000u;
static_assert((kIndexedEntry & (3u << kClassShift)) == 0u && (kSlotMask & (3u << kClassShift)) == 0u,
	"the class bits stand apart from the slot and the indexed bit");

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
thread_local JitterVertexClass t_class = JitterVertexClass::Unknown;
thread_local D3D9WTerms t_wterms{};
thread_local bool t_wterms_known = false;
thread_local DxbcWTerms t_dxbc_wterms{};
thread_local bool t_dxbc_wterms_known = false;

JitterVertexClass classify(device_api api, const shader_desc &desc)
{
	if (desc.code == nullptr || desc.code_size == 0)
		return JitterVertexClass::Unknown;
	if (api == device_api::d3d9)
		return classify_d3d9_vertex_shader(static_cast<const uint32_t *>(desc.code), desc.code_size / 4u);
	return classify_dxbc_vertex_shader(desc.code, desc.code_size);
}

bool read_wterms(device_api api, const shader_desc &desc, D3D9WTerms *out)
{
	if (api != device_api::d3d9 || desc.code == nullptr || desc.code_size == 0)
		return false;
	return d3d9_position_w_terms(static_cast<const uint32_t *>(desc.code), desc.code_size / 4u, out);
}

bool read_dxbc_wterms(device_api api, const shader_desc &desc, DxbcWTerms *out)
{
	if (api != device_api::d3d10 || desc.code == nullptr || desc.code_size == 0)
		return false;
	return dxbc_position_w_terms(desc.code, desc.code_size, out);
}

void ff_chain(const float *world, const float *view, const float *projection, float wvp[16]) noexcept
{
	const auto multiply = [](const float *a, const float *b, float *out) {
		for (uint32_t r = 0; r < 4u; ++r) {
			for (uint32_t c = 0; c < 4u; ++c) {
				float sum = 0.0f;
				for (uint32_t k = 0; k < 4u; ++k)
					sum += a[r * 4u + k] * b[k * 4u + c];
				out[r * 4u + c] = sum;
			}
		}
	};
	float wv[16];
	multiply(world, view, wv);
	multiply(wv, projection, wvp);
}

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

enum class DrawCarries { Grid, Offset, Nothing, Interface };

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
	uint64_t wterms_of = ~0ull;
	uint64_t wterms_generation = 0;
	D3D9WTerms wterms{};
	bool wterms_known = false;
	uint64_t dxbc_wterms_of = ~0ull;
	uint64_t dxbc_wterms_generation = 0;
	DxbcWTerms dxbc_wterms{};
	bool dxbc_wterms_known = false;

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

uint64_t bound_vertex_shader(IDirect3DDevice9 *dev, ShaderJitterState &s)
{
	if (s.pure)
		return s.vs;
	IDirect3DVertexShader9 *bound = nullptr;
	if (FAILED(dev->GetVertexShader(&bound))) {
		s.pure = true;
		return s.vs;
	}
	const uint64_t vs = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(bound));
	if (bound != nullptr)
		bound->Release();
	return vs;
}

DrawCarries d3d9_draw(command_list *cmd, ShaderJitterState &s, bool moved, ClipOffset o, bool scene_by_size,
	bool into_window)
{
	auto *const dev = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(cmd->get_native()));
	const uint64_t vs = bound_vertex_shader(dev, s);
	lookup(s, vs);
	const bool process = s.stream_out;
	const bool overlay = moved && !process && into_window && tests_no_depth(dev, scene_by_size);
	if (process || overlay)
		moved = false;
	if (vs == 0) {
		give_back_regs(dev, s, ~0u);
		const bool carried = fixed_function_draw(dev, s, moved, o);
		return process ? DrawCarries::Nothing : overlay ? DrawCarries::Interface
			: carried ? DrawCarries::Offset : DrawCarries::Grid;
	}
	if (s.ff_ours_set) {
		dev->SetTransform(D3DTS_PROJECTION, &s.ff_game);
		s.ff_ours_set = false;
	}
	if (slot_of(s.entry) == 0) {
		give_back_regs(dev, s, ~0u);
		return process ? DrawCarries::Nothing : overlay ? DrawCarries::Interface : DrawCarries::Grid;
	}
	const uint32_t reg = slot_of(s.entry) - 1u;
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
		return process ? DrawCarries::Nothing : overlay ? DrawCarries::Interface : DrawCarries::Grid;
	}
	give_back_regs(dev, s, reg);
	const float v[4] = { moved ? o.x : 0.0f, moved ? o.y : 0.0f, 0.0f, 0.0f };
	set_offset_reg(dev, s, reg, v);
	return process ? DrawCarries::Nothing : overlay ? DrawCarries::Interface : moved ? DrawCarries::Offset
		: DrawCarries::Grid;
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
	if (slot_of(s.entry) == 0) {
		give_back_slots(dev, s, ~0u);
		return DrawCarries::Grid;
	}
	const uint32_t slot = slot_of(s.entry) - 1u;
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
	bool scene_by_size, bool into_window) noexcept
{
	if (!moved && !g_any.load())
		return JitterShaderDraw::Grid;
	ShaderJitterState &s = state_of(cmd);
	const ClipOffset o = jitter_clip_offset(tx, ty, vw, vh);
	DrawCarries c = DrawCarries::Grid;
	switch (cmd->get_device()->get_api()) {
	case device_api::d3d9:
		c = d3d9_draw(cmd, s, moved, o, scene_by_size, into_window);
		break;
	case device_api::d3d10:
		c = d3d10_draw(cmd, s, moved, o);
		break;
	default:
		break;
	}
	return c == DrawCarries::Offset ? JitterShaderDraw::Offset
		: c == DrawCarries::Nothing ? JitterShaderDraw::NotADraw
		: c == DrawCarries::Interface ? JitterShaderDraw::Interface : JitterShaderDraw::Grid;
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

JitterVertexClass jitter_shader_vertex_class(command_list *cmd) noexcept
{
	if (cmd == nullptr)
		return JitterVertexClass::Unknown;
	const device_api api = cmd->get_device()->get_api();
	if (!jitter_in_shaders(api))
		return JitterVertexClass::Unknown;
	ShaderJitterState &s = state_of(cmd);
	if (api == device_api::d3d10) {
		lookup(s, s.vs);
		return class_of(s.entry);
	}
	auto *const dev = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(cmd->get_native()));
	const uint64_t vs = bound_vertex_shader(dev, s);
	if (vs != 0) {
		lookup(s, vs);
		return class_of(s.entry);
	}
	if (s.pure)
		return JitterVertexClass::Unknown;
	if (pretransformed(dev, s))
		return JitterVertexClass::Screen;
	D3DMATRIX world{}, view{}, projection{};
	if (FAILED(dev->GetTransform(D3DTS_WORLD, &world)) || FAILED(dev->GetTransform(D3DTS_VIEW, &view)) ||
		FAILED(dev->GetTransform(D3DTS_PROJECTION, &projection)))
		return JitterVertexClass::Unknown;
	if (s.ff_ours_set && std::memcmp(&projection, &s.ff_ours, sizeof(projection)) == 0)
		projection = s.ff_game;
	return jitter_fixed_function_class(&world.m[0][0], &view.m[0][0], &projection.m[0][0]);
}

JitterVertexClass jitter_fixed_function_class(const float world[16], const float view[16],
	const float projection[16]) noexcept
{
	if (projection[3] == 0.0f && projection[7] == 0.0f && projection[11] == 0.0f && projection[15] == 1.0f)
		return JitterVertexClass::Screen;
	float wvp[16];
	ff_chain(world, view, projection, wvp);
	bool identity = true;
	for (uint32_t i = 0; i < 16u && identity; ++i) {
		const float want = (i % 5u) == 0u ? 1.0f : 0.0f;
		identity = std::fabs(wvp[i] - want) <= 1e-5f;
	}
	return identity ? JitterVertexClass::Screen : JitterVertexClass::Transformed;
}

bool jitter_fixed_function_perspective(const float world[16], const float view[16],
	const float projection[16]) noexcept
{
	float wvp[16];
	ff_chain(world, view, projection, wvp);
	return std::fabs(wvp[3]) > 1e-6f || std::fabs(wvp[7]) > 1e-6f || std::fabs(wvp[11]) > 1e-6f;
}

namespace {

int d3d10_projection(command_list *cmd, ShaderJitterState &s)
{
	const uint64_t generation = g_generation.load();
	if (s.vs != s.dxbc_wterms_of || generation != s.dxbc_wterms_generation) {
		s.dxbc_wterms_of = s.vs;
		s.dxbc_wterms_generation = generation;
		std::shared_lock<std::shared_mutex> lock(g_registry_lock);
		const auto it = g_dxbc_wterms.find(s.vs);
		s.dxbc_wterms_known = it != g_dxbc_wterms.end();
		s.dxbc_wterms = s.dxbc_wterms_known ? it->second : DxbcWTerms{};
	}
	if (!s.dxbc_wterms_known || s.gs)
		return -1;
	struct Reader {
		command_list *cmd;
		ID3D10Device *dev;
		ShaderJitterState *s;
		uint64_t now;
	} reader{ cmd, reinterpret_cast<ID3D10Device *>(static_cast<uintptr_t>(cmd->get_native())), &s,
		scene_jitter().frame() };
	constant_reads_poll(cmd, reader.now);
	return dxbc_w_terms_perspective(
		s.dxbc_wterms,
		[](void *user, uint32_t slot, uint32_t reg, float out[4]) {
			const Reader &r = *static_cast<const Reader *>(user);
			if (slot >= kD3D10OffsetSlots)
				return false;
			ID3D10Buffer *b = nullptr;
			r.dev->VSGetConstantBuffers(slot, 1, &b);
			uint64_t handle = reinterpret_cast<uintptr_t>(b);
			if (b != nullptr && (b == r.s->cb_zero || b == r.s->cb_moved))
				handle = reinterpret_cast<uintptr_t>(r.s->game_cb[slot]);
			const bool read = handle != 0 && d3d10_vertex_constant(r.cmd, r.s->vs, slot, reg, r.now, handle, out);
			if (b != nullptr)
				b->Release();
			return read;
		},
		&reader);
}

}

bool jitter_shader_perspective(command_list *cmd) noexcept
{
	return jitter_shader_projection(cmd) == 1;
}

int jitter_shader_projection(command_list *cmd) noexcept
{
	if (cmd == nullptr)
		return -1;
	const device_api api = cmd->get_device()->get_api();
	if (api == device_api::d3d10)
		return d3d10_projection(cmd, state_of(cmd));
	if (api != device_api::d3d9)
		return -1;
	ShaderJitterState &s = state_of(cmd);
	auto *const dev = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(cmd->get_native()));
	const uint64_t vs = bound_vertex_shader(dev, s);
	if (s.pure)
		return -1;
	if (vs == 0) {
		if (pretransformed(dev, s))
			return 0;
		D3DMATRIX world{}, view{}, projection{};
		if (FAILED(dev->GetTransform(D3DTS_WORLD, &world)) || FAILED(dev->GetTransform(D3DTS_VIEW, &view)) ||
			FAILED(dev->GetTransform(D3DTS_PROJECTION, &projection)))
			return -1;
		if (s.ff_ours_set && std::memcmp(&projection, &s.ff_ours, sizeof(projection)) == 0)
			projection = s.ff_game;
		return jitter_fixed_function_perspective(&world.m[0][0], &view.m[0][0], &projection.m[0][0]) ? 1 : 0;
	}
	const uint64_t generation = g_generation.load();
	if (vs != s.wterms_of || generation != s.wterms_generation) {
		s.wterms_of = vs;
		s.wterms_generation = generation;
		std::shared_lock<std::shared_mutex> lock(g_registry_lock);
		const auto it = g_wterms.find(vs);
		s.wterms_known = it != g_wterms.end();
		s.wterms = s.wterms_known ? it->second : D3D9WTerms{};
	}
	if (!s.wterms_known)
		return -1;
	float file[kD3D9OffsetConstants * 4u] = {};
	for (uint32_t i = 0; i < s.wterms.count; ++i) {
		const uint32_t reg = s.wterms.reg[i];
		if (reg >= kD3D9OffsetConstants)
			return -1;
		float *const at = file + reg * 4u;
		const KnownReg &k = s.known[reg];
		if (k.ours) {
			if (k.game_set)
				std::memcpy(at, k.game, sizeof(k.game));
		} else if (FAILED(dev->GetVertexShaderConstantF(reg, at, 1))) {
			return -1;
		}
	}
	return d3d9_w_terms_perspective(s.wterms, file, kD3D9OffsetConstants) ? 1 : 0;
}

bool jitter_d3d9_depth_state(command_list *cmd, bool *enabled, bool *compares) noexcept
{
	*enabled = false;
	*compares = false;
	if (cmd == nullptr || cmd->get_device()->get_api() != device_api::d3d9)
		return false;
	auto *const dev = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(cmd->get_native()));
	DWORD z = D3DZB_FALSE, func = D3DCMP_LESSEQUAL;
	if (FAILED(dev->GetRenderState(D3DRS_ZENABLE, &z)) || FAILED(dev->GetRenderState(D3DRS_ZFUNC, &func)))
		return false;
	*enabled = z != D3DZB_FALSE;
	*compares = func != D3DCMP_ALWAYS && func != D3DCMP_NEVER;
	return true;
}

namespace jitter_shader_events {

bool create_pipeline(device *device, pipeline_layout, uint32_t count, const pipeline_subobject *subobjects)
{
	t_code_ptr = nullptr;
	t_class = JitterVertexClass::Unknown;
	t_wterms_known = false;
	if (!jitter_in_shaders(device->get_api()))
		return false;
	for (uint32_t i = 0; i < count; ++i) {
		if (subobjects[i].type != pipeline_subobject_type::vertex_shader || subobjects[i].data == nullptr)
			continue;
		if (!g_wanted.load()) {
			g_unwanted.fetch_add(1);
			return false;
		}
		const shader_desc &game = *static_cast<const shader_desc *>(subobjects[i].data);
		const JitterVertexClass c = classify(device->get_api(), game);
		D3D9WTerms w{};
		const bool w_known = read_wterms(device->get_api(), game, &w);
		DxbcWTerms dw{};
		const bool dw_known = read_dxbc_wterms(device->get_api(), game, &dw);
		if (!rewrite(device, *static_cast<shader_desc *>(subobjects[i].data))) {
			g_kept.fetch_add(1);
			return false;
		}
		t_class = c;
		t_wterms = w;
		t_wterms_known = w_known;
		t_dxbc_wterms = dw;
		t_dxbc_wterms_known = dw_known;
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
		bool layout = false;
		for (uint32_t k = 0; k < count; ++k)
			layout = layout || subobjects[k].type == pipeline_subobject_type::input_layout;
		const JitterVertexClass c = ours ? t_class : layout ? JitterVertexClass::Unknown : classify(device->get_api(), *desc);
		D3D9WTerms w = t_wterms;
		const bool w_known = ours ? t_wterms_known : !layout && read_wterms(device->get_api(), *desc, &w);
		DxbcWTerms dw = t_dxbc_wterms;
		const bool dw_known = ours ? t_dxbc_wterms_known : !layout && read_dxbc_wterms(device->get_api(), *desc, &dw);
		{
			std::unique_lock<std::shared_mutex> lock(g_registry_lock);
			if (ours)
				g_registry[pipeline.handle] = (t_index + 1u) | (t_indexed ? kIndexedEntry : 0u) | class_bits(c);
			else if (c != JitterVertexClass::Unknown)
				g_registry[pipeline.handle] = class_bits(c);
			else
				g_registry.erase(pipeline.handle);
			if (w_known)
				g_wterms[pipeline.handle] = w;
			else
				g_wterms.erase(pipeline.handle);
			if (dw_known)
				g_dxbc_wterms[pipeline.handle] = dw;
			else
				g_dxbc_wterms.erase(pipeline.handle);
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
	t_class = JitterVertexClass::Unknown;
	t_wterms_known = false;
	t_dxbc_wterms_known = false;
}

void destroy_pipeline(device *device, pipeline pipeline)
{
	if (!jitter_in_shaders(device->get_api()))
		return;
	std::unique_lock<std::shared_mutex> lock(g_registry_lock);
	const size_t gone =
		g_registry.erase(pipeline.handle) + g_wterms.erase(pipeline.handle) + g_dxbc_wterms.erase(pipeline.handle);
	if (gone != 0)
		g_generation.fetch_add(1);
	lock.unlock();
	constant_reads().forget_shader(pipeline.handle);
}

void bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pipeline)
{
	if (cmd == nullptr)
		return;
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
	if (cmd == nullptr)
		return;
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
	if (cmd == nullptr)
		return;
	if (first != 0 || count == 0 || buffers == nullptr || cmd->get_device()->get_api() != device_api::d3d9)
		return;
	state_of(cmd).stream_out = buffers[0].handle != 0;
}

void push_descriptors(command_list *cmd, shader_stage stages, pipeline_layout, uint32_t,
	const descriptor_table_update &update)
{
	if (cmd == nullptr)
		return;
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
	if (cmd == nullptr)
		return;
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
