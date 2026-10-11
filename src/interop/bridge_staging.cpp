#include "aeon_sr/depth/d3d9_depth.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/interop/interop_bridges.hpp"

#include <cstring>
#include <iterator>

namespace aeon_sr {
namespace {

namespace rapi = reshade::api;

template <typename T>
void safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

constexpr uint64_t kWaitNanoseconds = 2000ull * 1000ull * 1000ull;

DXGI_FORMAT engine_format_of(DXGI_FORMAT fmt) noexcept
{
	switch (fmt) {
	case DXGI_FORMAT_B8G8R8X8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	default: return fmt;
	}
}

constexpr uint32_t kProbeSize = 64;
constexpr rapi::format kProbeFormat = rapi::format::b8g8r8a8_unorm;

struct UpRecipe {
	rapi::resource_type type;
	rapi::memory_heap heap;
	rapi::resource_flags flags;
};

constexpr UpRecipe kUpRecipes[] = {
	{ rapi::resource_type::texture_2d, rapi::memory_heap::scratch, rapi::resource_flags::none },
	{ rapi::resource_type::texture_2d, rapi::memory_heap::upload, rapi::resource_flags::none },
	{ rapi::resource_type::surface, rapi::memory_heap::default_, rapi::resource_flags::dynamic },
};

struct DownRecipe {
	rapi::resource_type type;
};

constexpr DownRecipe kDownRecipes[] = {
	{ rapi::resource_type::texture_2d },
	{ rapi::resource_type::surface },
};

constexpr rapi::resource_usage kUpStates[] = {
	rapi::resource_usage::copy_source,
	rapi::resource_usage::cpu_access,
};

uint32_t probe_pixel(uint32_t x, uint32_t y, uint32_t salt) noexcept
{
	return 0xFF000000u | ((x * 4u) << 16) | ((y * 4u) << 8) | ((x ^ y ^ salt) & 0xFFu);
}

struct StagePlane {
	rapi::resource down = { 0 };
	rapi::resource up = { 0 };
	rapi::format fmt = rapi::format::unknown;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t row_bytes = 0;
	uint32_t rows = 0;

	SharedPlane plane{};
	ID3D12Resource *push = nullptr;
	ID3D12Resource *pull = nullptr;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot{};
	uint64_t total = 0;
};

class BridgeStaging final : public FrameBridge {
public:
	BridgeKind kind() const noexcept override { return BridgeKind::Staging; }
	const char *name() const noexcept override { return "system memory (slow)"; }
	const char *sync_name() const noexcept override { return "CPU wait on both devices"; }

	~BridgeStaging() override { shutdown(); }

	bool init(reshade::api::device *game, EngineDevice &engine) override
	{
		if (game == nullptr) {
			last_error = L"no device";
			return false;
		}
		if (!engine.ready()) {
			last_error = engine.last_error.empty()
				? std::wstring(L"the engine device is not ready") : engine.last_error;
			return false;
		}
		if (engine.borrowed()) {
			last_error = L"the engine is running on the game's own Direct3D 12 device, which "
				L"has no command list to spare for a system memory copy. The Direct3D 12 "
				L"bridge is the one that belongs on this game.";
			return false;
		}
		if (engine.device() == nullptr) {
			last_error = L"the engine has no Direct3D 12 device";
			return false;
		}

		device_ = game;
		engine_ = &engine;

		if (!device_->create_fence(0, rapi::fence_flags::none, &fence_) || fence_ == 0) {
			last_error = L"this driver would not create a fence, and without one the add-on "
				L"cannot tell when the game has finished drawing the frame it is about to "
				L"read. A graphics driver update is the only thing that fixes this.";
			shutdown();
			return false;
		}

		if (!engine.adapter_matched())
			diag_warn("interop", L"the add-on could not create its Direct3D 12 device on the "
				L"same graphics card the game is using, so every frame travels through system "
				L"memory. Forcing the game onto the other card in Windows graphics settings "
				L"would make it much faster.");
		return true;
	}

	void on_swapchain_reset() override
	{
		if (engine_ == nullptr)
			return;
		if (engine_list_ != nullptr) {
			diag_warn("interop", L"the game reset its device in the middle of a frame; the frame's "
				L"copies were kept, and the reset may be refused");
			return;
		}
		engine_->wait_cpu(engine_->last_submitted());
		wait_game();
		const bool held = color_.down != 0 || color_.up != 0 || depth_.down != 0 || motion_.down != 0;
		depth9_.release();
		release_stage(color_);
		release_stage(depth_);
		release_stage(motion_);
		if (device_ != nullptr && fence_ != 0)
			device_->destroy_fence(fence_);
		fence_ = { 0 };
		fence_value_ = 0;
		if (held)
			diag_info("interop", L"the game's device is being reset: the frame copies on its side "
				L"were released, and are made again on the next frame");
	}

	void shutdown() override
	{
		depth9_.release();
		if (engine_ != nullptr) {
			if (engine_list_ != nullptr)
				engine_->abandon_list();
			engine_->wait_cpu(engine_->last_submitted());
		}
		wait_game();

		release_stage(color_);
		release_stage(depth_);
		release_stage(motion_);

		if (device_ != nullptr && fence_ != 0)
			device_->destroy_fence(fence_);
		fence_ = { 0 };
		fence_value_ = 0;
		queue_ = nullptr;
		engine_list_ = nullptr;
		device_ = nullptr;
		engine_ = nullptr;
		probed_ = false;
		broken_ = false;
		up_recipe_ = -1;
		down_recipe_ = -1;
	}

	bool ready() const noexcept override
	{
		return device_ != nullptr && engine_ != nullptr && !broken_;
	}

	BridgeStep begin(reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list, const BridgeInputs &in, BridgeFrame &out) override
	{
		(void)cmd_list;

		out = BridgeFrame{};
		engine_list_ = nullptr;
		if (!ready())
			return BridgeStep::Init;

		queue_ = runtime != nullptr ? runtime->get_command_queue() : nullptr;
		rapi::command_list *const list = queue_ != nullptr ? queue_->get_immediate_command_list() : nullptr;
		if (list == nullptr) {
			last_error = L"ReShade has no command list for this frame";
			return BridgeStep::Import;
		}
		if (in.color == 0) {
			last_error = L"the game gave the add-on no colour texture this frame";
			return BridgeStep::Import;
		}

		if (fence_ == 0 && (!device_->create_fence(0, rapi::fence_flags::none, &fence_) || fence_ == 0)) {
			last_error = L"the game's device would not make a fence again after it was reset";
			return BridgeStep::Import;
		}

		if (!probed_ && !probe_routes(list, device_->get_resource_desc(in.color))) {
			broken_ = true;
			return BridgeStep::Import;
		}

		if (!ensure_stage(color_, in.color, true))
			return BridgeStep::Import;
		const rapi::resource depth = depth_to_read(in.depth);
		const bool want_depth = depth != 0 && ensure_stage(depth_, depth, false);
		note_optional(in.depth != 0 && !want_depth, L"depth");
		const bool want_motion = in.motion_vectors != 0 && ensure_stage(motion_, in.motion_vectors, false);
		note_optional(in.motion_vectors != 0 && !want_motion, L"motion vectors");

		download(list, in.color, color_, rapi::resource_usage::render_target);
		if (want_depth)
			download(list, depth, depth_, rapi::resource_usage::shader_resource_non_pixel);
		if (want_motion)
			download(list, in.motion_vectors, motion_, rapi::resource_usage::shader_resource_non_pixel);

		if (!sync_game()) {
			last_error = L"the game's graphics driver stopped responding while the add-on was "
				L"waiting for the frame to arrive in system memory";
			return BridgeStep::Import;
		}

		if (!engine_->wait_cpu(engine_->last_submitted())) {
			last_error = engine_->last_error;
			return BridgeStep::Begin;
		}
		out.cmd = engine_->begin_list();
		engine_list_ = out.cmd;
		if (out.cmd == nullptr) {
			last_error = engine_->last_error;
			return BridgeStep::Begin;
		}

		if (!push_plane(out.cmd, color_) ||
			(want_depth && !push_plane(out.cmd, depth_)) ||
			(want_motion && !push_plane(out.cmd, motion_))) {
			engine_->abandon_list();
			engine_list_ = nullptr;
			out.cmd = nullptr;
			return BridgeStep::Import;
		}

		out.color = color_.plane.engine;
		out.depth = want_depth ? depth_.plane.engine : nullptr;
		out.motion_vectors = want_motion ? motion_.plane.engine : nullptr;
		out.width = color_.width;
		out.height = color_.height;
		return BridgeStep::Ok;
	}

	BridgeStep end(reshade::api::effect_runtime *runtime, reshade::api::command_list *,
		reshade::api::resource game_color) override
	{
		if (!ready())
			return BridgeStep::Init;
		if (color_.pull == nullptr || color_.up == 0) {
			last_error = L"the colour plane was never carried in, so there is nothing to put back";
			return BridgeStep::Export;
		}

		ID3D12GraphicsCommandList *const cmd = engine_list_;
		if (cmd == nullptr) {
			last_error = L"the engine command list was never opened for this frame";
			return BridgeStep::Finish;
		}
		engine_list_ = nullptr;
		barrier_engine(cmd, color_.plane.engine, SharedPlane::kState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		copy_engine(cmd, color_, false);
		barrier_engine(cmd, color_.plane.engine, D3D12_RESOURCE_STATE_COPY_SOURCE, SharedPlane::kState);

		if (!engine_->submit_list()) {
			last_error = engine_->last_error;
			return BridgeStep::Finish;
		}
		if (!engine_->wait_cpu(engine_->last_submitted())) {
			last_error = engine_->last_error;
			return BridgeStep::Finish;
		}

		queue_ = runtime != nullptr ? runtime->get_command_queue() : queue_;
		rapi::command_list *const list = queue_ != nullptr ? queue_->get_immediate_command_list() : nullptr;
		if (list == nullptr || game_color == 0) {
			last_error = L"ReShade has no command list to put the finished frame back with";
			return BridgeStep::Export;
		}

		const rapi::resource_desc dst = device_->get_resource_desc(game_color);
		if (dst.texture.width != color_.width || dst.texture.height != color_.height ||
			dst.texture.format != color_.fmt) {
			last_error = L"the game changed the size or format of its colour texture in the "
				L"middle of a frame, so the upscaled result could not be put back";
			return BridgeStep::Export;
		}

		if (!pull_plane(color_))
			return BridgeStep::Export;

		const rapi::resource_usage from = rapi::resource_usage::render_target;
		const rapi::resource_usage to = rapi::resource_usage::copy_dest;
		list->barrier(1, &game_color, &from, &to);
		list->copy_texture_region(color_.up, 0, nullptr, game_color, 0, nullptr);
		list->barrier(1, &game_color, &to, &from);

		signal_game();
		return BridgeStep::Ok;
	}

	bool request_readable(SharedPlane &plane, uint32_t w, uint32_t h, DXGI_FORMAT fmt) override
	{
		if (!ready() || w == 0 || h == 0 || fmt == DXGI_FORMAT_UNKNOWN)
			return false;
		if (plane.matches(w, h, fmt))
			return true;
		engine_->wait_cpu(engine_->last_submitted());
		bridge_util::release_plane(plane);

		if (!make_engine_texture(w, h, fmt, false, &plane.engine)) {
			last_error = L"the engine device refused a texture";
			return false;
		}
		plane.width = w;
		plane.height = h;
		plane.format = fmt;
		return true;
	}

private:

	bool signal_game()
	{
		if (queue_ == nullptr || fence_ == 0)
			return false;
		queue_->flush_immediate_command_list();
		const uint64_t next = fence_value_ + 1;
		if (!queue_->signal(fence_, next))
			return false;
		fence_value_ = next;
		queue_->flush_immediate_command_list();
		return true;
	}

	bool wait_game()
	{
		if (device_ == nullptr || fence_ == 0 || fence_value_ == 0)
			return true;
		return device_->wait(fence_, fence_value_, kWaitNanoseconds);
	}

	bool sync_game() { return signal_game() && wait_game(); }

	bool make_engine_texture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool renderable,
		ID3D12Resource **out) const
	{
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = fmt;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = renderable ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
		return SUCCEEDED(engine_->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
			&desc, SharedPlane::kState, nullptr, IID_PPV_ARGS(out)));
	}

	bool make_engine_buffer(uint64_t bytes, bool readback, ID3D12Resource **out) const
	{
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = readback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_UPLOAD;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = bytes;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		return SUCCEEDED(engine_->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			readback ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr, IID_PPV_ARGS(out)));
	}

	rapi::resource make_down(int recipe, uint32_t w, uint32_t h, rapi::format fmt) const
	{
		rapi::resource out = { 0 };
		const rapi::resource_desc desc(kDownRecipes[recipe].type, w, h, 1, 1, fmt, 1,
			rapi::memory_heap::readback, rapi::resource_usage::copy_dest);
		if (!device_->create_resource(desc, nullptr, rapi::resource_usage::copy_dest, &out))
			return { 0 };
		return out;
	}

	rapi::resource make_up(int recipe, uint32_t w, uint32_t h, rapi::format fmt) const
	{
		const UpRecipe &r = kUpRecipes[recipe];
		const rapi::resource_desc desc(r.type, w, h, 1, 1, fmt, 1, r.heap,
			rapi::resource_usage::copy_source, r.flags);
		for (const rapi::resource_usage state : kUpStates) {
			rapi::resource out = { 0 };
			if (device_->create_resource(desc, nullptr, state, &out) && out != 0)
				return out;
		}
		return { 0 };
	}

	void release_stage(StagePlane &p)
	{
		if (device_ != nullptr) {
			if (p.down != 0)
				device_->destroy_resource(p.down);
			if (p.up != 0)
				device_->destroy_resource(p.up);
		}
		p.down = { 0 };
		p.up = { 0 };
		safe_release(p.push);
		safe_release(p.pull);
		bridge_util::release_plane(p.plane);
		p.fmt = rapi::format::unknown;
		p.width = p.height = p.row_bytes = p.rows = 0;
		p.foot = D3D12_PLACED_SUBRESOURCE_FOOTPRINT{};
		p.total = 0;
	}

	void note_optional(bool failed, const wchar_t *what)
	{
		if (!failed)
			return;
		if (!last_error.empty()) {
			diag_state("interop-optional", DiagLevel::Info, "interop",
				std::wstring(what) + L" is not being carried: " + last_error);
			last_error.clear();
		}
	}

	rapi::resource depth_to_read(rapi::resource depth)
	{
		if (depth == 0 || device_->get_api() != rapi::device_api::d3d9)
			return depth;
		auto *const res = reinterpret_cast<IDirect3DResource9 *>(static_cast<uintptr_t>(depth.handle));
		IDirect3DTexture9 *tex = nullptr;
		if (res == nullptr || FAILED(res->QueryInterface(IID_PPV_ARGS(&tex))) || tex == nullptr)
			return depth;
		rapi::resource out = depth;
		if (D3D9DepthToFloat::wants(tex)) {
			auto *const dev = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(device_->get_native()));
			std::wstring why;
			IDirect3DTexture9 *const converted = depth9_.convert(dev, tex, why);
			if (converted != nullptr) {
				out = rapi::resource{ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(converted)) };
			} else {
				last_error = why;
				out = rapi::resource{ 0 };
				note_optional(true, L"depth");
			}
		}
		tex->Release();
		return out;
	}

	bool ensure_stage(StagePlane &p, rapi::resource src, bool two_way)
	{
		if (up_recipe_ < 0 || down_recipe_ < 0)
			return false;
		const rapi::resource_desc desc = device_->get_resource_desc(src);
		const uint32_t w = desc.texture.width;
		const uint32_t h = desc.texture.height;
		if (w == 0 || h == 0) {
			last_error = L"the game handed over a texture with no size";
			return false;
		}
		if (desc.texture.samples != 1) {
			last_error = L"this game renders with multisampling, which cannot be copied "
				L"through system memory. Turning the game's own anti-aliasing off lets the "
				L"add-on run.";
			return false;
		}
		const DXGI_FORMAT dxgi = engine_format_of(dxgi_format_of(desc.texture.format));
		if (dxgi == DXGI_FORMAT_UNKNOWN) {
			last_error = L"one of the game's textures is in a format Direct3D 12 has no name "
				L"for, so the upscaler cannot be shown it";
			return false;
		}
		if (p.plane.matches(w, h, dxgi) && p.fmt == desc.texture.format &&
			p.down != 0 && (!two_way || (p.up != 0 && p.pull != nullptr)))
			return true;

		engine_->wait_cpu(engine_->last_submitted());
		sync_game();
		release_stage(p);

		const uint32_t row = rapi::format_row_pitch(desc.texture.format, w);
		if (row == 0) {
			last_error = L"the add-on does not know how many bytes a row of the game's "
				L"texture format takes, so it cannot copy one";
			return false;
		}
		p.fmt = desc.texture.format;
		p.width = w;
		p.height = h;
		p.row_bytes = row;
		p.rows = rapi::format_slice_pitch(desc.texture.format, row, h) / row;

		if (!make_engine_texture(w, h, dxgi, two_way, &p.plane.engine)) {
			last_error = L"the engine device refused a texture for the frame";
			release_stage(p);
			return false;
		}
		p.plane.width = w;
		p.plane.height = h;
		p.plane.format = dxgi;

		const D3D12_RESOURCE_DESC engine_desc = p.plane.engine->GetDesc();
		UINT num_rows = 0;
		UINT64 row_size = 0;
		engine_->device()->GetCopyableFootprints(&engine_desc, 0, 1, 0, &p.foot, &num_rows,
			&row_size, &p.total);
		if (num_rows != p.rows || row_size != p.row_bytes || p.total == 0) {
			last_error = L"the add-on and Direct3D 12 disagree about the layout of the game's "
				L"texture format, so carrying it would corrupt the picture";
			release_stage(p);
			return false;
		}

		p.down = make_down(down_recipe_, w, h, p.fmt);
		if (p.down == 0) {
			last_error = L"this driver refused a system memory copy of the game's texture";
			release_stage(p);
			return false;
		}
		if (!make_engine_buffer(p.total, false, &p.push)) {
			last_error = L"the engine device refused an upload buffer";
			release_stage(p);
			return false;
		}
		if (two_way) {
			p.up = make_up(up_recipe_, w, h, p.fmt);
			if (p.up == 0 || !make_engine_buffer(p.total, true, &p.pull)) {
				last_error = L"this driver refused the allocation the finished frame is "
					L"handed back through";
				release_stage(p);
				return false;
			}
		}
		return true;
	}

	static void barrier_engine(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res,
		D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		D3D12_RESOURCE_BARRIER b{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = res;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		cmd->ResourceBarrier(1, &b);
	}

	static void copy_engine(ID3D12GraphicsCommandList *cmd, StagePlane &p, bool into_texture)
	{
		D3D12_TEXTURE_COPY_LOCATION tex{};
		tex.pResource = p.plane.engine;
		tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		tex.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION buf{};
		buf.pResource = into_texture ? p.push : p.pull;
		buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		buf.PlacedFootprint = p.foot;
		if (into_texture)
			cmd->CopyTextureRegion(&tex, 0, 0, 0, &buf, nullptr);
		else
			cmd->CopyTextureRegion(&buf, 0, 0, 0, &tex, nullptr);
	}

	void download(rapi::command_list *list, rapi::resource src, const StagePlane &p,
		rapi::resource_usage held) const
	{
		const rapi::resource_usage to = rapi::resource_usage::copy_source;
		list->barrier(1, &src, &held, &to);
		list->copy_texture_region(src, 0, nullptr, p.down, 0, nullptr);
		list->barrier(1, &src, &to, &held);
	}

	bool push_plane(ID3D12GraphicsCommandList *cmd, StagePlane &p)
	{
		rapi::subresource_data src{};
		if (!device_->map_texture_region(p.down, 0, nullptr, rapi::map_access::read_only, &src) ||
			src.data == nullptr) {
			last_error = L"this driver would not let the add-on read the frame back out of "
				L"system memory";
			return false;
		}
		bool ok = src.row_pitch >= p.row_bytes;
		void *dst = nullptr;
		const D3D12_RANGE nothing{ 0, 0 };
		if (ok)
			ok = SUCCEEDED(p.push->Map(0, &nothing, &dst)) && dst != nullptr;
		if (ok) {
			for (uint32_t r = 0; r < p.rows; ++r)
				std::memcpy(static_cast<uint8_t *>(dst) + static_cast<size_t>(r) * p.foot.Footprint.RowPitch,
					static_cast<const uint8_t *>(src.data) + static_cast<size_t>(r) * src.row_pitch,
					p.row_bytes);
			p.push->Unmap(0, nullptr);
		}
		device_->unmap_texture_region(p.down, 0);
		if (!ok) {
			last_error = L"the engine device would not accept the frame out of system memory";
			return false;
		}

		barrier_engine(cmd, p.plane.engine, SharedPlane::kState, D3D12_RESOURCE_STATE_COPY_DEST);
		copy_engine(cmd, p, true);
		barrier_engine(cmd, p.plane.engine, D3D12_RESOURCE_STATE_COPY_DEST, SharedPlane::kState);
		return true;
	}

	bool pull_plane(StagePlane &p)
	{
		void *src = nullptr;
		const D3D12_RANGE all{ 0, static_cast<SIZE_T>(p.total) };
		if (FAILED(p.pull->Map(0, &all, &src)) || src == nullptr) {
			last_error = L"the engine device would not hand the finished frame back";
			return false;
		}
		rapi::subresource_data dst{};
		bool ok = device_->map_texture_region(p.up, 0, nullptr, rapi::map_access::write_only, &dst) &&
			dst.data != nullptr && dst.row_pitch >= p.row_bytes;
		if (ok) {
			for (uint32_t r = 0; r < p.rows; ++r)
				std::memcpy(static_cast<uint8_t *>(dst.data) + static_cast<size_t>(r) * dst.row_pitch,
					static_cast<const uint8_t *>(src) + static_cast<size_t>(r) * p.foot.Footprint.RowPitch,
					p.row_bytes);
		}
		if (dst.data != nullptr)
			device_->unmap_texture_region(p.up, 0);
		const D3D12_RANGE nothing{ 0, 0 };
		p.pull->Unmap(0, &nothing);
		if (!ok) {
			last_error = L"this driver would not let the add-on write the finished frame back "
				L"into a texture it can read";
			return false;
		}
		return true;
	}

	bool probe_routes(rapi::command_list *list, const rapi::resource_desc &like)
	{
		probed_ = true;
		up_recipe_ = -1;
		down_recipe_ = -1;

		rapi::resource target = { 0 };
		const rapi::resource_desc desc(like.type, kProbeSize, kProbeSize, 1, 1, kProbeFormat, 1,
			rapi::memory_heap::default_,
			rapi::resource_usage::render_target | rapi::resource_usage::copy_source |
			rapi::resource_usage::copy_dest);
		if (!device_->create_resource(desc, nullptr, rapi::resource_usage::copy_dest, &target) ||
			target == 0) {
			last_error = L"this driver would not create the small test texture the add-on "
				L"checks a device with before it uses it";
			return false;
		}

		for (int up = 0; up_recipe_ < 0 && up < static_cast<int>(std::size(kUpRecipes)); ++up) {
			const uint32_t salt = static_cast<uint32_t>(up) + 1u;
			const rapi::resource src = make_up(up, kProbeSize, kProbeSize, kProbeFormat);
			if (src == 0)
				continue;
			if (fill_probe(src, salt)) {
				for (int down = 0; down < static_cast<int>(std::size(kDownRecipes)); ++down) {
					const rapi::resource dst = make_down(down, kProbeSize, kProbeSize, kProbeFormat);
					if (dst == 0)
						continue;
					const bool good = run_probe(list, src, target, dst, salt);
					device_->destroy_resource(dst);
					if (good) {
						up_recipe_ = up;
						down_recipe_ = down;
						break;
					}
				}
			}
			device_->destroy_resource(src);
		}

		device_->destroy_resource(target);
		if (up_recipe_ < 0) {
			last_error = L"this graphics driver could not copy a picture out to system memory "
				L"and back again, which is the only way the add-on can reach a game on this "
				L"graphics API. Every way of doing it was tried. A driver update, or running "
				L"the game on a newer graphics API if it offers one, is what would change it.";
			return false;
		}
		return true;
	}

	bool fill_probe(rapi::resource dst, uint32_t salt) const
	{
		rapi::subresource_data data{};
		if (!device_->map_texture_region(dst, 0, nullptr, rapi::map_access::write_only, &data) ||
			data.data == nullptr)
			return false;
		const bool ok = data.row_pitch >= kProbeSize * 4u;
		if (ok) {
			for (uint32_t y = 0; y < kProbeSize; ++y) {
				auto *const row = reinterpret_cast<uint32_t *>(
					static_cast<uint8_t *>(data.data) + static_cast<size_t>(y) * data.row_pitch);
				for (uint32_t x = 0; x < kProbeSize; ++x)
					row[x] = probe_pixel(x, y, salt);
			}
		}
		device_->unmap_texture_region(dst, 0);
		return ok;
	}

	bool run_probe(rapi::command_list *list, rapi::resource src, rapi::resource target,
		rapi::resource dst, uint32_t salt)
	{
		const rapi::resource_usage as_dst = rapi::resource_usage::copy_dest;
		const rapi::resource_usage as_src = rapi::resource_usage::copy_source;
		list->copy_texture_region(src, 0, nullptr, target, 0, nullptr);
		list->barrier(1, &target, &as_dst, &as_src);
		list->copy_texture_region(target, 0, nullptr, dst, 0, nullptr);
		list->barrier(1, &target, &as_src, &as_dst);
		if (!sync_game())
			return false;

		rapi::subresource_data data{};
		if (!device_->map_texture_region(dst, 0, nullptr, rapi::map_access::read_only, &data) ||
			data.data == nullptr)
			return false;
		bool ok = data.row_pitch >= kProbeSize * 4u;
		for (uint32_t y = 0; ok && y < kProbeSize; ++y) {
			const auto *const row = reinterpret_cast<const uint32_t *>(
				static_cast<const uint8_t *>(data.data) + static_cast<size_t>(y) * data.row_pitch);
			for (uint32_t x = 0; ok && x < kProbeSize; ++x)
				ok = row[x] == probe_pixel(x, y, salt);
		}
		device_->unmap_texture_region(dst, 0);
		return ok;
	}

	reshade::api::device *device_ = nullptr;
	EngineDevice *engine_ = nullptr;
	reshade::api::command_queue *queue_ = nullptr;
	reshade::api::fence fence_ = { 0 };
	uint64_t fence_value_ = 0;
	ID3D12GraphicsCommandList *engine_list_ = nullptr;

	bool probed_ = false;
	bool broken_ = false;
	int up_recipe_ = -1;
	int down_recipe_ = -1;

	StagePlane color_{};
	StagePlane depth_{};
	StagePlane motion_{};
	D3D9DepthToFloat depth9_;
};

}

FrameBridge *make_bridge_staging() { return new BridgeStaging(); }

}
