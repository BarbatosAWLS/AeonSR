#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/interop/interop_bridges.hpp"
#include "aeon_sr/interop/native_d3d.hpp"

#include <d3d10_1.h>
#include <d3dcompiler.h>

#include <cstring>
#include <functional>
#include <vector>

namespace aeon_sr {
namespace {

template <typename T>
void safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

constexpr uint64_t kKeyIdle = 0;
constexpr uint64_t kKeyHelperPull = 1;
constexpr uint64_t kKeyHelperPush = 2;
constexpr uint64_t kKeyGameTake = 3;
constexpr uint64_t kKeyCount = 4;

constexpr DWORD kAcquireTimeoutMs = 1000;

constexpr uint64_t kQueryTimeoutMs = 2000;

struct Handoff {
	IDXGIKeyedMutex *mutex = nullptr;
	uint64_t take = kKeyIdle;
	uint64_t give = kKeyIdle;
};

bool take_all(const std::vector<Handoff> &steps)
{
	for (size_t i = 0; i < steps.size(); ++i) {
		if (steps[i].mutex != nullptr && steps[i].mutex->AcquireSync(steps[i].take, kAcquireTimeoutMs) == S_OK)
			continue;
		for (size_t j = 0; j < i; ++j)
			steps[j].mutex->ReleaseSync(steps[j].take);
		return false;
	}
	return true;
}

void give_all(const std::vector<Handoff> &steps)
{
	for (const Handoff &s : steps)
		if (s.mutex != nullptr)
			s.mutex->ReleaseSync(s.give);
}

void copy_top_level(reshade::api::command_list *cmd_list, reshade::api::resource src,
	reshade::api::resource dst)
{
	cmd_list->copy_texture_region(src, 0, nullptr, dst, 0, nullptr,
		reshade::api::filter_mode::min_mag_mip_point);
}

DXGI_FORMAT shareable_format(DXGI_FORMAT fmt) noexcept
{
	switch (fmt) {
	case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
	case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
	default: return fmt;
	}
}

DXGI_FORMAT depth_view_format(DXGI_FORMAT fmt) noexcept
{
	switch (fmt) {
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
		return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
		return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT:
		return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_D16_UNORM:
		return DXGI_FORMAT_R16_UNORM;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

const char kDepthHlsl[] = R"(
Texture2D<float> depth : register(t0);
float4 VSMain(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float PSMain(float4 pos : SV_Position) : SV_Target
{
	return depth.Load(int3(pos.xy, 0));
}
)";

struct Plane10 {
	SharedPlane nt{};
	ID3D11Texture2D *keyed = nullptr;
	IDXGIKeyedMutex *mutex_helper = nullptr;
	IDXGIKeyedMutex *mutex_game = nullptr;
	reshade::api::resource game = { 0 };
	ID3D10RenderTargetView *game_rtv = nullptr;

	uint32_t width = 0;
	uint32_t height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

	bool matches(uint32_t w, uint32_t h, DXGI_FORMAT fmt) const noexcept
	{
		return nt.engine != nullptr && width == w && height == h && format == fmt;
	}
};

struct Readable {
	const SharedPlane *owner = nullptr;
	Plane10 pair{};
	reshade::api::resource visible = { 0 };
};

class BridgeD3D10 final : public FrameBridge {
public:
	~BridgeD3D10() override { shutdown(); }

	BridgeKind kind() const noexcept override { return BridgeKind::Legacy10; }
	const char *name() const noexcept override { return "Direct3D 10 shared texture (two hops)"; }

	const char *sync_name() const noexcept override
	{
		if (mode_ == Mode::Keyless)
			return fence12_ != nullptr ? "CPU waits on both devices, then a shared fence"
				: "CPU waits on both devices";
		return fence12_ != nullptr
			? "keyed mutex, then a shared fence"
			: "keyed mutex, then a CPU wait";
	}

	bool init(reshade::api::device *game, EngineDevice &engine) override
	{
		if (game == nullptr || game->get_api() != reshade::api::device_api::d3d10) {
			last_error = L"not a Direct3D 10 device";
			return false;
		}
		if (game->get_native() == 0) {
			last_error = L"the Direct3D 10 device could not be read back from ReShade";
			return false;
		}
		if (!engine.ready()) {
			last_error = engine.last_error.empty()
				? std::wstring(L"the engine device is not ready") : engine.last_error;
			return false;
		}
		if (!engine.adapter_matched()) {
			last_error = L"the add-on's Direct3D 12 device came up on a different graphics "
				L"card from the game's, and a shared texture cannot cross two cards. Set "
				L"this game to the other card in the graphics driver's per-application "
				L"settings and restart it.";
			return false;
		}

		device_ = game;
		engine_ = &engine;

		ID3D11Device *const helper = engine.device11();
		if (helper == nullptr) {
			last_error = engine.last_error.empty()
				? std::wstring(L"the helper Direct3D 11 device could not be created, and "
					L"Direct3D 10 has no way of reaching Direct3D 12 without it")
				: engine.last_error;
			shutdown();
			return false;
		}
		ID3D11DeviceContext *const helper_context = engine.context11();
		if (helper_context == nullptr) {
			last_error = L"the helper Direct3D 11 device came up without a context";
			shutdown();
			return false;
		}
		device11_ = helper;
		device11_->AddRef();
		context11_ = helper_context;
		context11_->AddRef();

		ID3D11Device5 *const device11_5 = engine.device11_5();
		if (device11_5 != nullptr &&
			SUCCEEDED(context11_->QueryInterface(IID_PPV_ARGS(&context11_4_))) &&
			context11_4_ != nullptr &&
			SUCCEEDED(device11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11_)))) {
			HANDLE handle = nullptr;
			if (FAILED(fence11_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle)) ||
				FAILED(engine.device()->OpenSharedHandle(handle, IID_PPV_ARGS(&fence12_))))
				safe_release(fence12_);
			if (handle != nullptr)
				CloseHandle(handle);
		}

		if (fence12_ == nullptr) {
			safe_release(fence11_);
			safe_release(context11_4_);
			D3D11_QUERY_DESC qdesc{};
			qdesc.Query = D3D11_QUERY_EVENT;
			if (FAILED(device11_->CreateQuery(&qdesc, &query11_)) || query11_ == nullptr) {
				last_error = L"this driver offered the add-on neither a shared fence nor an "
					L"event query on Direct3D 11, so the game's card and the add-on's "
					L"cannot be kept in step safely";
				shutdown();
				return false;
			}
		}

		value_ = 0;
		frame_open_ = false;
		return true;
	}

	void shutdown() override
	{
		if (engine_ != nullptr)
			engine_->wait_cpu(engine_->last_submitted());

		for (Readable &r : readables_)
			release_readable(r);
		readables_.clear();
		release_plane(colour_);
		release_plane(depth_);
		release_plane(motion_);

		safe_release(fence12_);
		safe_release(fence11_);
		safe_release(context11_4_);
		safe_release(query11_);
		safe_release(sync11_);
		safe_release(query10_);
		safe_release(depth_vs_);
		safe_release(depth_ps_);
		safe_release(depth_rs_);
		safe_release(context11_);
		safe_release(device11_);
		mode_ = Mode::Unknown;

		device_ = nullptr;
		engine_ = nullptr;
		value_ = 0;
		frame_open_ = false;
	}

	bool ready() const noexcept override
	{
		return device_ != nullptr && engine_ != nullptr && device11_ != nullptr &&
			context11_ != nullptr && (fence12_ != nullptr || query11_ != nullptr);
	}

	BridgeStep begin(reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list, const BridgeInputs &in, BridgeFrame &out) override
	{
		out = BridgeFrame{};
		if (!ready())
			return BridgeStep::Init;

		reshade::api::command_queue *const queue = runtime != nullptr ? runtime->get_command_queue() : nullptr;
		if (cmd_list == nullptr || queue == nullptr) {
			last_error = L"ReShade offered no Direct3D 10 command list this frame";
			return BridgeStep::Import;
		}
		if (frame_open_)
			reset_rings();
		frame_open_ = false;

		if (in.color == 0) {
			last_error = L"the game's colour texture was not available this frame";
			return BridgeStep::Import;
		}
		if (!ensure_plane(colour_, in.color, L"colour",
				D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
			return BridgeStep::Import;

		const bool want_depth = in.depth != 0 && ensure_depth_plane(in.depth);
		if (in.depth != 0 && !want_depth && !last_error.empty()) {
			diag_state("interop-depth10", DiagLevel::Info, "interop", L"depth is not being carried: " + last_error);
			last_error.clear();
		}
		const bool want_motion = in.motion_vectors != 0 &&
			ensure_plane(motion_, in.motion_vectors, L"motion vector", D3D11_BIND_SHADER_RESOURCE);

		if (mode_ == Mode::Keyless) {
			copy_top_level(cmd_list, in.color, colour_.game);
			const bool depth_drawn = want_depth && draw_depth(in.depth);
			if (want_motion)
				copy_top_level(cmd_list, in.motion_vectors, motion_.game);
			queue->flush_immediate_command_list();
			if (!wait_game()) {
				last_error = L"the game's Direct3D 10 device did not finish copying the frame in time, "
					L"which usually means the graphics driver has stopped responding";
				return BridgeStep::Import;
			}
			context11_->CopyResource(colour_.nt.helper, colour_.keyed);
			if (depth_drawn)
				context11_->CopyResource(depth_.nt.helper, depth_.keyed);
			if (want_motion)
				context11_->CopyResource(motion_.nt.helper, motion_.keyed);
			context11_->Flush();
			return open_engine_frame(depth_drawn, want_motion, out);
		}

		steps_.clear();
		steps_.push_back({ colour_.mutex_game, kKeyIdle, kKeyHelperPull });
		if (want_depth)
			steps_.push_back({ depth_.mutex_game, kKeyIdle, kKeyHelperPull });
		if (want_motion)
			steps_.push_back({ motion_.mutex_game, kKeyIdle, kKeyHelperPull });
		if (!take_all(steps_)) {
			last_error = L"the game's Direct3D 10 device could not take the shared textures "
				L"in time, which usually means the graphics driver has stopped responding";
			return BridgeStep::Import;
		}
		copy_top_level(cmd_list, in.color, colour_.game);
		if (want_depth && !draw_depth(in.depth))
			last_error.clear();
		if (want_motion)
			copy_top_level(cmd_list, in.motion_vectors, motion_.game);
		queue->flush_immediate_command_list();
		give_all(steps_);

		steps_.clear();
		steps_.push_back({ colour_.mutex_helper, kKeyHelperPull, kKeyHelperPush });
		if (want_depth)
			steps_.push_back({ depth_.mutex_helper, kKeyHelperPull, kKeyIdle });
		if (want_motion)
			steps_.push_back({ motion_.mutex_helper, kKeyHelperPull, kKeyIdle });
		if (!take_all(steps_)) {
			last_error = L"the helper Direct3D 11 device could not take the shared textures in time";
			reset_rings();
			return BridgeStep::Import;
		}
		context11_->CopyResource(colour_.nt.helper, colour_.keyed);
		if (want_depth)
			context11_->CopyResource(depth_.nt.helper, depth_.keyed);
		if (want_motion)
			context11_->CopyResource(motion_.nt.helper, motion_.keyed);
		context11_->Flush();
		give_all(steps_);

		return open_engine_frame(want_depth, want_motion, out);
	}

	BridgeStep open_engine_frame(bool want_depth, bool want_motion, BridgeFrame &out)
	{
		if (!order_engine_behind_helper()) {
			reset_rings();
			return BridgeStep::Import;
		}

		out.cmd = engine_->begin_list();
		if (out.cmd == nullptr) {
			last_error = engine_->last_error;
			reset_rings();
			return BridgeStep::Begin;
		}

		out.color = colour_.nt.engine;
		out.depth = want_depth ? depth_.nt.engine : nullptr;
		out.motion_vectors = want_motion ? motion_.nt.engine : nullptr;
		out.width = colour_.width;
		out.height = colour_.height;
		frame_open_ = true;
		return BridgeStep::Ok;
	}

	BridgeStep end(reshade::api::effect_runtime *runtime, reshade::api::command_list *cmd_list,
		reshade::api::resource game_color) override
	{
		if (!ready())
			return BridgeStep::Init;
		const bool was_open = frame_open_;
		frame_open_ = false;
		if (!was_open)
			return BridgeStep::Init;

		if (!engine_->submit_list()) {
			last_error = engine_->last_error;
			reset_rings();
			return BridgeStep::Finish;
		}

		reshade::api::command_queue *const queue = runtime != nullptr ? runtime->get_command_queue() : nullptr;
		if (cmd_list == nullptr || queue == nullptr || game_color == 0) {
			last_error = L"ReShade offered no Direct3D 10 command list to put the frame back with";
			reset_rings();
			return BridgeStep::Export;
		}
		if (!order_helper_behind_engine()) {
			reset_rings();
			return BridgeStep::Finish;
		}

		if (mode_ == Mode::Keyless) {
			context11_->CopyResource(colour_.keyed, colour_.nt.helper);
			for (const Readable &r : readables_)
				context11_->CopyResource(r.pair.keyed, r.pair.nt.helper);
			if (!wait_helper()) {
				last_error = L"the helper Direct3D 11 device did not finish the frame in time";
				return BridgeStep::Export;
			}
			copy_top_level(cmd_list, colour_.game, game_color);
			for (const Readable &r : readables_)
				copy_top_level(cmd_list, r.pair.game, r.visible);
			queue->flush_immediate_command_list();
			return BridgeStep::Ok;
		}

		steps_.clear();
		steps_.push_back({ colour_.mutex_helper, kKeyHelperPush, kKeyGameTake });
		for (const Readable &r : readables_)
			steps_.push_back({ r.pair.mutex_helper, kKeyIdle, kKeyGameTake });
		if (!take_all(steps_)) {
			last_error = L"the helper Direct3D 11 device could not take the shared texture back";
			reset_rings();
			return BridgeStep::Export;
		}
		context11_->CopyResource(colour_.keyed, colour_.nt.helper);
		for (const Readable &r : readables_)
			context11_->CopyResource(r.pair.keyed, r.pair.nt.helper);
		context11_->Flush();
		give_all(steps_);

		steps_.clear();
		steps_.push_back({ colour_.mutex_game, kKeyGameTake, kKeyIdle });
		for (const Readable &r : readables_)
			steps_.push_back({ r.pair.mutex_game, kKeyGameTake, kKeyIdle });
		if (!take_all(steps_)) {
			last_error = L"the game's Direct3D 10 device could not take the finished frame back";
			reset_rings();
			return BridgeStep::Export;
		}
		copy_top_level(cmd_list, colour_.game, game_color);
		for (const Readable &r : readables_)
			copy_top_level(cmd_list, r.pair.game, r.visible);
		queue->flush_immediate_command_list();
		give_all(steps_);
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

		Readable *record = nullptr;
		for (Readable &r : readables_) {
			if (r.owner == &plane) {
				record = &r;
				break;
			}
		}
		if (record == nullptr) {
			readables_.push_back(Readable{});
			record = &readables_.back();
			record->owner = &plane;
		}
		release_readable(*record);

		if (!create_plane(record->pair, w, h, fmt, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) {
			drop_readable(record);
			last_error = L"this driver refused a shared texture for one of the add-on's own planes";
			return false;
		}
		const reshade::api::resource_desc desc(w, h, 1, 1, reshade_format_of(fmt), 1,
			reshade::api::memory_heap::default_,
			reshade::api::resource_usage::copy_dest | reshade::api::resource_usage::shader_resource);
		if (!device_->create_resource(desc, nullptr, reshade::api::resource_usage::copy_dest,
				&record->visible)) {
			drop_readable(record);
			last_error = L"the game's Direct3D 10 device refused a texture for one of the "
				L"add-on's own planes";
			return false;
		}

		plane.engine = record->pair.nt.engine;
		plane.engine->AddRef();
		plane.helper = record->pair.nt.helper;
		plane.helper->AddRef();
		plane.game = record->visible;
		plane.width = w;
		plane.height = h;
		plane.format = fmt;
		return true;
	}

private:
	bool order_engine_behind_helper()
	{
		if (fence12_ != nullptr) {
			if (FAILED(context11_4_->Signal(fence11_, ++value_))) {
				last_error = L"the helper Direct3D 11 device refused a fence signal";
				return false;
			}
			if (FAILED(engine_->queue()->Wait(fence12_, value_))) {
				last_error = L"the engine queue refused to wait on the shared fence";
				return false;
			}
			context11_->Flush();
			return true;
		}
		context11_->End(query11_);
		context11_->Flush();
		if (!wait_query()) {
			last_error = L"the helper Direct3D 11 device stopped responding while the add-on "
				L"waited for the game's frame";
			return false;
		}
		return true;
	}

	bool order_helper_behind_engine()
	{
		if (fence12_ != nullptr) {
			if (FAILED(engine_->queue()->Signal(fence12_, ++value_))) {
				last_error = L"the engine queue refused a fence signal";
				return false;
			}
			if (FAILED(context11_4_->Wait(fence11_, value_))) {
				last_error = L"the helper Direct3D 11 device refused to wait on the shared fence";
				return false;
			}
			return true;
		}
		if (!engine_->wait_cpu(engine_->last_submitted())) {
			last_error = engine_->last_error;
			return false;
		}
		return true;
	}

	bool wait_query()
	{
		const uint64_t deadline = GetTickCount64() + kQueryTimeoutMs;
		for (unsigned poll = 0;; ++poll) {
			BOOL done = FALSE;
			const HRESULT hr = context11_->GetData(query11_, &done, sizeof(done), 0);
			if (hr == S_OK)
				return true;
			if (FAILED(hr) || GetTickCount64() > deadline)
				return false;
			if (poll < 256)
				Sleep(0);
			else
				bridge_util::short_wait();
		}
	}

	bool ensure_plane(Plane10 &plane, reshade::api::resource src, const wchar_t *what, UINT bind)
	{
		const reshade::api::resource_desc desc = device_->get_resource_desc(src);
		if (desc.texture.samples > 1) {
			last_error = std::wstring(L"the game's ") + what +
				L" texture is multisampled, which cannot be shared between two devices";
			return false;
		}
		const DXGI_FORMAT fmt = shareable_format(dxgi_format_of(desc.texture.format));
		if (fmt == DXGI_FORMAT_UNKNOWN || desc.texture.width == 0 || desc.texture.height == 0) {
			last_error = std::wstring(L"the game's ") + what +
				L" texture is in a format the add-on cannot carry between two devices";
			return false;
		}
		if (plane.matches(desc.texture.width, desc.texture.height, fmt))
			return true;

		engine_->wait_cpu(engine_->last_submitted());
		release_plane(plane);
		if (!create_plane(plane, desc.texture.width, desc.texture.height, fmt, bind)) {
			last_error = std::wstring(L"this driver refused a shared texture for the game's ") +
				what + L" buffer (" + plane_step_ + L")";
			return false;
		}
		return true;
	}

	bool ensure_depth_plane(reshade::api::resource src)
	{
		const reshade::api::resource_desc desc = device_->get_resource_desc(src);
		if (depth_view_format(dxgi_format_of(desc.texture.format)) == DXGI_FORMAT_UNKNOWN)
			return ensure_plane(depth_, src, L"depth", D3D11_BIND_SHADER_RESOURCE);
		if (desc.texture.samples > 1) {
			last_error = L"the game's depth texture is multisampled, which cannot be shared between two devices";
			return false;
		}
		if (depth_.matches(desc.texture.width, desc.texture.height, DXGI_FORMAT_R32_FLOAT) &&
			depth_.game_rtv != nullptr)
			return true;
		engine_->wait_cpu(engine_->last_submitted());
		release_plane(depth_);
		if (!create_plane(depth_, desc.texture.width, desc.texture.height, DXGI_FORMAT_R32_FLOAT,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET)) {
			last_error = std::wstring(L"this driver refused a shared texture for the game's depth buffer (") +
				plane_step_ + L")";
			return false;
		}
		ID3D10Device *const dev = game_device();
		auto *const tex = native_res<ID3D10Resource>(depth_.game);
		if (dev == nullptr || tex == nullptr || FAILED(dev->CreateRenderTargetView(tex, nullptr, &depth_.game_rtv))) {
			release_plane(depth_);
			last_error = L"the game's device would not draw into the shared depth texture";
			return false;
		}
		return true;
	}

	ID3D10Device *game_device() const noexcept
	{
		return reinterpret_cast<ID3D10Device *>(static_cast<uintptr_t>(device_->get_native()));
	}

	bool ensure_depth_pipeline(ID3D10Device *dev)
	{
		if (depth_vs_ != nullptr && depth_ps_ != nullptr && depth_rs_ != nullptr)
			return true;
		ID3DBlob *vs = nullptr, *ps = nullptr;
		D3DCompile(kDepthHlsl, std::strlen(kDepthHlsl), "aeon_bridge10", nullptr, nullptr, "VSMain", "vs_4_0", 0, 0,
			&vs, nullptr);
		D3DCompile(kDepthHlsl, std::strlen(kDepthHlsl), "aeon_bridge10", nullptr, nullptr, "PSMain", "ps_4_0", 0, 0,
			&ps, nullptr);
		D3D10_RASTERIZER_DESC rd{};
		rd.FillMode = D3D10_FILL_SOLID;
		rd.CullMode = D3D10_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		const bool ok = vs != nullptr && ps != nullptr &&
			SUCCEEDED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), &depth_vs_)) &&
			SUCCEEDED(dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), &depth_ps_)) &&
			SUCCEEDED(dev->CreateRasterizerState(&rd, &depth_rs_));
		safe_release(vs);
		safe_release(ps);
		if (!ok) {
			safe_release(depth_vs_);
			safe_release(depth_ps_);
			safe_release(depth_rs_);
		}
		return ok;
	}

	bool draw_depth(reshade::api::resource src)
	{
		if (depth_.game_rtv == nullptr) {
			copy_top_level_native(src, depth_.game);
			return true;
		}
		ID3D10Device *const dev = game_device();
		auto *const tex = native_res<ID3D10Resource>(src);
		if (dev == nullptr || tex == nullptr || !ensure_depth_pipeline(dev)) {
			last_error = L"the pass that converts the game's depth buffer would not build on this driver";
			return false;
		}
		const reshade::api::resource_desc desc = device_->get_resource_desc(src);
		D3D10_SHADER_RESOURCE_VIEW_DESC sd{};
		sd.Format = depth_view_format(dxgi_format_of(desc.texture.format));
		sd.ViewDimension = D3D10_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		ID3D10ShaderResourceView *srv = nullptr;
		if (FAILED(dev->CreateShaderResourceView(tex, &sd, &srv)) || srv == nullptr) {
			last_error = L"the game's depth buffer cannot be read by a shader";
			return false;
		}

		ID3D10VertexShader *old_vs = nullptr;
		ID3D10PixelShader *old_ps = nullptr;
		ID3D10GeometryShader *old_gs = nullptr;
		ID3D10InputLayout *old_layout = nullptr;
		D3D10_PRIMITIVE_TOPOLOGY old_topology = D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D10RasterizerState *old_rs = nullptr;
		ID3D10BlendState *old_blend = nullptr;
		float old_factor[4]{};
		UINT old_mask = 0;
		ID3D10DepthStencilState *old_ds = nullptr;
		UINT old_ref = 0;
		ID3D10RenderTargetView *old_rtv[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D10DepthStencilView *old_dsv = nullptr;
		ID3D10ShaderResourceView *old_srv = nullptr;
		UINT old_vp_count = D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D10_VIEWPORT old_vp[D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		dev->VSGetShader(&old_vs);
		dev->PSGetShader(&old_ps);
		dev->GSGetShader(&old_gs);
		dev->IAGetInputLayout(&old_layout);
		dev->IAGetPrimitiveTopology(&old_topology);
		dev->RSGetState(&old_rs);
		dev->OMGetBlendState(&old_blend, old_factor, &old_mask);
		dev->OMGetDepthStencilState(&old_ds, &old_ref);
		dev->OMGetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtv, &old_dsv);
		dev->PSGetShaderResources(0, 1, &old_srv);
		dev->RSGetViewports(&old_vp_count, old_vp);

		dev->OMSetRenderTargets(1, &depth_.game_rtv, nullptr);
		dev->OMSetBlendState(nullptr, old_factor, 0xFFFFFFFFu);
		dev->OMSetDepthStencilState(nullptr, 0);
		D3D10_VIEWPORT vp{ 0, 0, depth_.width, depth_.height, 0.0f, 1.0f };
		dev->RSSetViewports(1, &vp);
		dev->RSSetState(depth_rs_);
		dev->IASetInputLayout(nullptr);
		dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		dev->VSSetShader(depth_vs_);
		dev->GSSetShader(nullptr);
		dev->PSSetShader(depth_ps_);
		dev->PSSetShaderResources(0, 1, &srv);
		dev->Draw(3, 0);

		ID3D10ShaderResourceView *const none = nullptr;
		dev->PSSetShaderResources(0, 1, old_srv != nullptr ? &old_srv : &none);
		dev->PSSetShader(old_ps);
		dev->GSSetShader(old_gs);
		dev->VSSetShader(old_vs);
		dev->IASetPrimitiveTopology(old_topology);
		dev->IASetInputLayout(old_layout);
		dev->RSSetState(old_rs);
		dev->RSSetViewports(old_vp_count, old_vp);
		dev->OMSetDepthStencilState(old_ds, old_ref);
		dev->OMSetBlendState(old_blend, old_factor, old_mask);
		dev->OMSetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, old_rtv, old_dsv);
		safe_release(old_srv);
		safe_release(old_ps);
		safe_release(old_gs);
		safe_release(old_vs);
		safe_release(old_layout);
		safe_release(old_rs);
		safe_release(old_ds);
		safe_release(old_blend);
		for (ID3D10RenderTargetView *&v : old_rtv)
			safe_release(v);
		safe_release(old_dsv);
		safe_release(srv);
		return true;
	}

	void copy_top_level_native(reshade::api::resource src, reshade::api::resource dst)
	{
		ID3D10Device *const dev = game_device();
		auto *const s = native_res<ID3D10Resource>(src);
		auto *const d = native_res<ID3D10Resource>(dst);
		if (dev != nullptr && s != nullptr && d != nullptr)
			dev->CopySubresourceRegion(d, 0, 0, 0, 0, s, 0, nullptr);
	}

	bool create_plane(Plane10 &plane, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind)
	{
		if (mode_ != Mode::Keyless && create_keyed_plane(plane, w, h, fmt, bind)) {
			mode_ = Mode::Keyed;
			return true;
		}
		if (mode_ == Mode::Keyed)
			return false;
		const wchar_t *const keyed_step = plane_step_;
		if (!create_keyless_plane(plane, w, h, fmt, bind))
			return false;
		if (mode_ != Mode::Keyless)
			diag_info("interop", std::wstring(L"Direct3D 10: this driver refused a keyed-mutex texture (") +
				keyed_step + L"), so the game's device and the helper are kept in step by waiting on the "
				L"CPU after each copy");
		mode_ = Mode::Keyless;
		return true;
	}

	bool create_keyless_plane(Plane10 &plane, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind)
	{
		plane_step_ = L"the texture shared with the engine";
		if (!bridge_util::make_nt_pair(*engine_, device11_, w, h, fmt, bind, plane.nt))
			return false;
		HANDLE legacy = nullptr;
		plane_step_ = L"the plain shared texture on the helper device";
		if (!bridge_util::make_kmt_texture(device11_, w, h, fmt, bind, &plane.keyed, &legacy) ||
			legacy == nullptr) {
			release_plane(plane);
			return false;
		}
		plane_step_ = L"opening the plain shared texture on the game's device";
		if (!open_on_game(plane, w, h, fmt, legacy)) {
			release_plane(plane);
			return false;
		}
		plane.width = w;
		plane.height = h;
		plane.format = fmt;
		return true;
	}

	bool open_on_game(Plane10 &plane, uint32_t w, uint32_t h, DXGI_FORMAT fmt, HANDLE legacy)
	{
		const reshade::api::resource_desc desc(w, h, 1, 1, reshade_format_of(fmt), 1,
			reshade::api::memory_heap::default_,
			reshade::api::resource_usage::copy_dest | reshade::api::resource_usage::copy_source,
			reshade::api::resource_flags::shared);
		void *handle = legacy;
		return device_->create_resource(desc, nullptr, reshade::api::resource_usage::copy_dest,
			&plane.game, &handle) && plane.game != 0;
	}

	bool poll_until(const std::function<HRESULT()> &get)
	{
		const uint64_t deadline = GetTickCount64() + kQueryTimeoutMs;
		for (unsigned poll = 0;; ++poll) {
			const HRESULT hr = get();
			if (hr == S_OK)
				return true;
			if (FAILED(hr) || GetTickCount64() > deadline)
				return false;
			if (poll < 256)
				Sleep(0);
			else
				bridge_util::short_wait();
		}
	}

	bool wait_game()
	{
		auto *const dev = reinterpret_cast<ID3D10Device *>(static_cast<uintptr_t>(device_->get_native()));
		if (query10_ == nullptr) {
			D3D10_QUERY_DESC qd{};
			qd.Query = D3D10_QUERY_EVENT;
			if (dev == nullptr || FAILED(dev->CreateQuery(&qd, &query10_)) || query10_ == nullptr)
				return false;
		}
		query10_->End();
		dev->Flush();
		return poll_until([this] { return query10_->GetData(nullptr, 0, 0); });
	}

	bool wait_helper()
	{
		if (sync11_ == nullptr) {
			D3D11_QUERY_DESC qd{};
			qd.Query = D3D11_QUERY_EVENT;
			if (FAILED(device11_->CreateQuery(&qd, &sync11_)) || sync11_ == nullptr)
				return false;
		}
		context11_->End(sync11_);
		context11_->Flush();
		return poll_until([this] {
			BOOL done = FALSE;
			return context11_->GetData(sync11_, &done, sizeof(done), 0);
		});
	}

	bool create_keyed_plane(Plane10 &plane, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind)
	{
		plane_step_ = L"the texture shared with the engine";
		if (!bridge_util::make_nt_pair(*engine_, device11_, w, h, fmt, bind, plane.nt))
			return false;

		HANDLE legacy = nullptr;
		plane_step_ = L"the keyed-mutex texture on the helper device";
		if (!make_keyed_texture(w, h, fmt, bind, &plane.keyed, &legacy) ||
			FAILED(plane.keyed->QueryInterface(IID_PPV_ARGS(&plane.mutex_helper))) ||
			plane.mutex_helper == nullptr) {
			release_plane(plane);
			return false;
		}
		plane_step_ = L"opening that texture on the game's device";

		const reshade::api::resource_desc desc(w, h, 1, 1, reshade_format_of(fmt), 1,
			reshade::api::memory_heap::default_,
			reshade::api::resource_usage::copy_dest | reshade::api::resource_usage::copy_source,
			reshade::api::resource_flags::shared);
		void *handle = legacy;
		if (!device_->create_resource(desc, nullptr, reshade::api::resource_usage::copy_dest,
				&plane.game, &handle) || plane.game == 0) {
			release_plane(plane);
			return false;
		}
		plane_step_ = L"the keyed mutex of the texture on the game's device";
		if (FAILED(native_res<IUnknown>(plane.game)->QueryInterface(IID_PPV_ARGS(&plane.mutex_game))) ||
			plane.mutex_game == nullptr) {
			release_plane(plane);
			return false;
		}

		plane.width = w;
		plane.height = h;
		plane.format = fmt;
		return true;
	}

	bool make_keyed_texture(uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind,
		ID3D11Texture2D **out, HANDLE *out_handle)
	{
		*out = nullptr;
		*out_handle = nullptr;

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = w;
		desc.Height = h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = fmt;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = bind;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
		if (FAILED(device11_->CreateTexture2D(&desc, nullptr, out)) || *out == nullptr)
			return false;

		IDXGIResource *res = nullptr;
		const bool ok = SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(&res))) && res != nullptr &&
			SUCCEEDED(res->GetSharedHandle(out_handle)) && *out_handle != nullptr;
		safe_release(res);
		if (!ok)
			safe_release(*out);
		return ok;
	}

	void release_plane(Plane10 &plane)
	{
		safe_release(plane.game_rtv);
		if (device_ != nullptr && plane.game != 0)
			device_->destroy_resource(plane.game);
		plane.game = { 0 };
		safe_release(plane.mutex_game);
		safe_release(plane.mutex_helper);
		safe_release(plane.keyed);
		bridge_util::release_plane(plane.nt);
		plane.width = plane.height = 0;
		plane.format = DXGI_FORMAT_UNKNOWN;
	}

	void release_readable(Readable &record)
	{
		release_plane(record.pair);
		if (device_ != nullptr && record.visible != 0)
			device_->destroy_resource(record.visible);
		record.visible = { 0 };
	}

	void drop_readable(Readable *record)
	{
		release_readable(*record);
		readables_.erase(readables_.begin() + (record - readables_.data()));
	}

	static void reset_mutex(IDXGIKeyedMutex *mutex)
	{
		if (mutex == nullptr)
			return;
		for (uint64_t key = 0; key < kKeyCount; ++key) {
			if (mutex->AcquireSync(key, 0) == S_OK) {
				mutex->ReleaseSync(kKeyIdle);
				return;
			}
		}
	}

	void reset_rings()
	{
		reset_mutex(colour_.mutex_helper);
		reset_mutex(depth_.mutex_helper);
		reset_mutex(motion_.mutex_helper);
		for (Readable &r : readables_)
			reset_mutex(r.pair.mutex_helper);
	}

	reshade::api::device *device_ = nullptr;
	EngineDevice *engine_ = nullptr;

	ID3D11Device *device11_ = nullptr;
	const wchar_t *plane_step_ = L"";
	ID3D11DeviceContext *context11_ = nullptr;
	ID3D11DeviceContext4 *context11_4_ = nullptr;
	ID3D11Fence *fence11_ = nullptr;
	ID3D12Fence *fence12_ = nullptr;
	ID3D11Query *query11_ = nullptr;
	ID3D11Query *sync11_ = nullptr;
	ID3D10Query *query10_ = nullptr;
	ID3D10VertexShader *depth_vs_ = nullptr;
	ID3D10PixelShader *depth_ps_ = nullptr;
	ID3D10RasterizerState *depth_rs_ = nullptr;
	enum class Mode { Unknown, Keyed, Keyless };
	Mode mode_ = Mode::Unknown;
	uint64_t value_ = 0;
	bool frame_open_ = false;

	Plane10 colour_{};
	Plane10 depth_{};
	Plane10 motion_{};
	std::vector<Readable> readables_;
	std::vector<Handoff> steps_;
};

}

FrameBridge *make_bridge_d3d10() { return new BridgeD3D10(); }

}
