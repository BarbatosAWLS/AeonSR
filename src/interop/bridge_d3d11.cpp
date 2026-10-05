#include "aeon_sr/interop/interop_bridges.hpp"
#include "aeon_sr/interop/blit_d3d11.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/interop/native_d3d.hpp"

#include <algorithm>

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

DXGI_FORMAT shareable_format_for(DXGI_FORMAT fmt) noexcept
{
	switch (fmt) {
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
	case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
	case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_D16_UNORM:
		return DXGI_FORMAT_R32_FLOAT;
	default:
		return fmt;
	}
}

}

namespace bridge_util {

void release_plane(SharedPlane &plane)
{
	safe_release(plane.engine);
	safe_release(plane.helper);
	plane.game = { 0 };
	plane.width = plane.height = 0;
	plane.format = DXGI_FORMAT_UNKNOWN;
}

bool make_nt_pair(EngineDevice &engine, ID3D11Device *device11,
	uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind_flags, SharedPlane &out)
{
	if (device11 == nullptr || !engine.ready() || w == 0 || h == 0 || fmt == DXGI_FORMAT_UNKNOWN)
		return false;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = fmt;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	ID3D11Texture2D *tex = nullptr;
	if (FAILED(device11->CreateTexture2D(&desc, nullptr, &tex)) || tex == nullptr)
		return false;

	IDXGIResource1 *res = nullptr;
	HANDLE handle = nullptr;
	ID3D12Resource *tex12 = nullptr;
	const bool ok = SUCCEEDED(tex->QueryInterface(IID_PPV_ARGS(&res))) && res != nullptr &&
		SUCCEEDED(res->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle)) &&
		SUCCEEDED(engine.device()->OpenSharedHandle(handle, IID_PPV_ARGS(&tex12)));
	safe_release(res);
	if (handle != nullptr)
		CloseHandle(handle);
	if (!ok || tex12 == nullptr) {
		safe_release(tex12);
		safe_release(tex);
		return false;
	}

	out.helper = tex;
	out.engine = tex12;
	out.width = w;
	out.height = h;
	out.format = fmt;
	return true;
}

bool make_kmt_texture(ID3D11Device *device11, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
	UINT bind_flags, ID3D11Texture2D **out, HANDLE *out_handle)
{
	if (device11 == nullptr || out == nullptr || out_handle == nullptr)
		return false;
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
	desc.BindFlags = bind_flags;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

	if (FAILED(device11->CreateTexture2D(&desc, nullptr, out)) || *out == nullptr)
		return false;

	IDXGIResource *res = nullptr;
	const bool ok = SUCCEEDED((*out)->QueryInterface(IID_PPV_ARGS(&res))) && res != nullptr &&
		SUCCEEDED(res->GetSharedHandle(out_handle));
	safe_release(res);
	if (!ok) {
		safe_release(*out);
		return false;
	}
	return true;
}

bool open_kmt_texture(ID3D11Device *device11, HANDLE handle, ID3D11Texture2D **out)
{
	if (device11 == nullptr || handle == nullptr || out == nullptr)
		return false;
	*out = nullptr;
	return SUCCEEDED(device11->OpenSharedResource(handle, IID_PPV_ARGS(out))) && *out != nullptr;
}

void copy_plane_12(ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst, ID3D12Resource *src)
{
	if (cmd == nullptr || dst == nullptr || src == nullptr)
		return;
	D3D12_RESOURCE_BARRIER b[2]{};
	for (int i = 0; i < 2; ++i) {
		b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	}
	b[0].Transition.pResource = dst;
	b[0].Transition.StateBefore = SharedPlane::kState;
	b[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	b[1].Transition.pResource = src;
	b[1].Transition.StateBefore = SharedPlane::kState;
	b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	cmd->ResourceBarrier(2, b);
	cmd->CopyResource(dst, src);
	for (int i = 0; i < 2; ++i)
		std::swap(b[i].Transition.StateBefore, b[i].Transition.StateAfter);
	cmd->ResourceBarrier(2, b);
}

}

namespace {

class BridgeD3D12 final : public FrameBridge {
public:
	~BridgeD3D12() override { shutdown(); }

	BridgeKind kind() const noexcept override { return BridgeKind::Native12; }
	const char *name() const noexcept override { return "Direct3D 12 (no bridge needed)"; }
	const char *sync_name() const noexcept override { return "none needed"; }

	bool init(reshade::api::device *game, EngineDevice &engine) override
	{
		if (game == nullptr || game->get_api() != reshade::api::device_api::d3d12) {
			last_error = L"not a Direct3D 12 device";
			return false;
		}
		if (engine.device() == nullptr || !engine.borrowed()) {
			last_error = L"the engine did not adopt the game's Direct3D 12 device";
			return false;
		}
		engine_ = &engine;
		device_ = game;
		return true;
	}

	void shutdown() override
	{
		engine_ = nullptr;
		device_ = nullptr;
		bridge_util::release_plane(readable_);
	}

	bool ready() const noexcept override { return engine_ != nullptr && device_ != nullptr; }

	BridgeStep begin(reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list, const BridgeInputs &in, BridgeFrame &out) override
	{
		out = BridgeFrame{};
		depth_note.clear();
		if (!ready())
			return BridgeStep::Init;
		if (reshade::api::command_queue *const q = runtime != nullptr ? runtime->get_command_queue() : nullptr)
			engine_->set_borrowed_queue(reinterpret_cast<ID3D12CommandQueue *>(q->get_native()));

		out.cmd = native_d3d12_list(cmd_list);
		if (out.cmd == nullptr)
			return BridgeStep::Begin;

		out.device = engine_->device();
		out.queue = engine_->queue();
		out.native = true;
		out.color = native_res<ID3D12Resource>(in.color);
		out.depth = native_res<ID3D12Resource>(in.depth);
		out.motion_vectors = native_res<ID3D12Resource>(in.motion_vectors);
		out.scene = native_res<ID3D12Resource>(in.scene);
		out.scene_state = D3D12_RESOURCE_STATE_COPY_DEST;
		if (out.color == nullptr)
			return BridgeStep::Import;

		out.color_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		out.motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

		if (out.depth != nullptr)
			out.depth_state = d3d12_states_of(in.depth_state);

		const D3D12_RESOURCE_DESC d = out.color->GetDesc();
		out.width = static_cast<uint32_t>(d.Width);
		out.height = d.Height;
		return BridgeStep::Ok;
	}

	BridgeStep end(reshade::api::effect_runtime *, reshade::api::command_list *,
		reshade::api::resource) override
	{
		return ready() ? BridgeStep::Ok : BridgeStep::Init;
	}

	bool request_readable(SharedPlane &plane, uint32_t w, uint32_t h, DXGI_FORMAT fmt) override
	{
		if (!ready())
			return false;
		if (plane.matches(w, h, fmt))
			return true;
		bridge_util::release_plane(plane);

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
		if (FAILED(engine_->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				SharedPlane::kState, nullptr, IID_PPV_ARGS(&plane.engine)))) {
			last_error = L"the engine device refused a texture";
			return false;
		}
		plane.width = w;
		plane.height = h;
		plane.format = fmt;
		return true;
	}

private:
	EngineDevice *engine_ = nullptr;
	reshade::api::device *device_ = nullptr;
	SharedPlane readable_{};
};

class BridgeD3D11 final : public FrameBridge {
public:
	~BridgeD3D11() override { shutdown(); }

	BridgeKind kind() const noexcept override { return BridgeKind::Shared11; }
	const char *name() const noexcept override { return "Direct3D 11 shared texture"; }
	const char *sync_name() const noexcept override { return "shared fence"; }

	bool init(reshade::api::device *game, EngineDevice &engine) override
	{
		if (game == nullptr || game->get_api() != reshade::api::device_api::d3d11) {
			last_error = L"not a Direct3D 11 device";
			return false;
		}
		device11_ = native_d3d11_device(game);
		if (device11_ == nullptr) {
			last_error = L"the Direct3D 11 device could not be read back from ReShade";
			return false;
		}
		if (!engine.ready()) {
			last_error = engine.last_error.empty()
				? std::wstring(L"the engine device is not ready") : engine.last_error;
			return false;
		}
		device11_->AddRef();
		engine_ = &engine;

		if (FAILED(device11_->QueryInterface(IID_PPV_ARGS(&device11_5_))) || device11_5_ == nullptr) {
			last_error = L"this Direct3D 11 driver is older than Windows 10 1709, which is "
				L"where the shared fence the bridge synchronises with was added";
			shutdown();
			return false;
		}

		HANDLE fence_handle = nullptr;
		const bool ok = SUCCEEDED(device11_5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11_))) &&
			SUCCEEDED(fence11_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle)) &&
			SUCCEEDED(engine.device()->OpenSharedHandle(fence_handle, IID_PPV_ARGS(&fence12_)));
		if (fence_handle != nullptr)
			CloseHandle(fence_handle);
		if (!ok) {
			last_error = L"this driver refused a shared fence between Direct3D 11 and Direct3D 12";
			shutdown();
			return false;
		}
		if (!engine.adapter_matched()) {
			diag_warn("interop", L"the engine device is not on the game's adapter, so a "
				L"shared texture may not open. This usually means the game is running on a "
				L"different GPU from the one the add-on found.");
		}
		value_ = 0;
		return true;
	}

	void shutdown() override
	{
		if (engine_ != nullptr)
			engine_->wait_cpu(engine_->last_submitted());
		bridge_util::release_plane(color_);
		bridge_util::release_plane(depth_);
		bridge_util::release_plane(motion_);
		bridge_util::release_plane(scene_);
		safe_release(fence12_);
		safe_release(fence11_);
		safe_release(device11_5_);
		safe_release(device11_);
		engine_ = nullptr;
		value_ = 0;
	}

	bool ready() const noexcept override
	{
		return device11_ != nullptr && engine_ != nullptr && fence11_ != nullptr &&
			fence12_ != nullptr && engine_->queue() != nullptr;
	}

	BridgeStep begin(reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list, const BridgeInputs &in, BridgeFrame &out) override
	{
		(void)cmd_list;
		out = BridgeFrame{};
		if (!ready())
			return BridgeStep::Init;

		ID3D11DeviceContext *const ctx = native_d3d11_context(runtime);
		if (ctx == nullptr) {
			last_error = L"no Direct3D 11 context this frame";
			return BridgeStep::Import;
		}

		auto *const game_color = native_res<ID3D11Resource>(in.color);
		if (game_color == nullptr)
			return BridgeStep::Import;
		if (!ensure_like(color_, game_color, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
			return BridgeStep::Import;

		auto *const game_depth = native_res<ID3D11Resource>(in.depth);
		auto *const game_motion = native_res<ID3D11Resource>(in.motion_vectors);
		bool want_depth = game_depth != nullptr &&
			ensure_like(depth_, game_depth, D3D11_BIND_SHADER_RESOURCE);
		bool want_motion = game_motion != nullptr &&
			ensure_like(motion_, game_motion, D3D11_BIND_SHADER_RESOURCE);
		auto *const game_scene = native_res<ID3D11Resource>(in.scene);
		const bool want_scene = game_scene != nullptr &&
			ensure_like(scene_, game_scene, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);

		ctx->CopyResource(color_.helper, game_color);
		if (want_scene)
			ctx->CopyResource(scene_.helper, game_scene);
		if (want_depth && !carry_in(ctx, depth_, game_depth))
			want_depth = false;
		if (want_motion && !carry_in(ctx, motion_, game_motion))
			want_motion = false;

		ID3D11DeviceContext4 *ctx4 = nullptr;
		if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4))) || ctx4 == nullptr) {
			last_error = L"this Direct3D 11 context cannot signal a fence (Windows 10 1709+ needed)";
			return BridgeStep::Import;
		}
		const HRESULT hr = ctx4->Signal(fence11_, ++value_);
		ctx4->Release();
		if (FAILED(hr)) {
			last_error = L"the Direct3D 11 fence signal failed";
			return BridgeStep::Import;
		}
		ID3D12CommandQueue *const engine_queue = engine_->queue();
		if (engine_queue == nullptr) {
			last_error = L"the engine device has no command queue this frame";
			return BridgeStep::Import;
		}
		if (FAILED(engine_queue->Wait(fence12_, value_))) {
			last_error = L"the engine queue refused to wait on the shared fence";
			return BridgeStep::Import;
		}

		out.cmd = engine_->begin_list();
		if (out.cmd == nullptr) {
			last_error = engine_->last_error;
			return BridgeStep::Begin;
		}

		out.device = engine_->device();
		out.queue = engine_->queue();
		out.color = color_.engine;
		out.depth = want_depth ? depth_.engine : nullptr;
		out.motion_vectors = want_motion ? motion_.engine : nullptr;
		out.scene = want_scene ? scene_.engine : nullptr;
		out.width = color_.width;
		out.height = color_.height;
		return BridgeStep::Ok;
	}

	BridgeStep end(reshade::api::effect_runtime *runtime, reshade::api::command_list *,
		reshade::api::resource game_color) override
	{
		if (!ready())
			return BridgeStep::Init;
		if (!engine_->submit_list()) {
			last_error = engine_->last_error;
			return BridgeStep::Finish;
		}
		ID3D12CommandQueue *const engine_queue = engine_->queue();
		if (engine_queue == nullptr || FAILED(engine_queue->Signal(fence12_, ++value_))) {
			last_error = L"the engine queue refused a fence signal";
			return BridgeStep::Finish;
		}

		ID3D11DeviceContext *const ctx = native_d3d11_context(runtime);
		auto *const dst = native_res<ID3D11Resource>(game_color);
		if (ctx == nullptr || dst == nullptr || color_.helper == nullptr)
			return BridgeStep::Export;

		ID3D11DeviceContext4 *ctx4 = nullptr;
		if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4))) || ctx4 == nullptr)
			return BridgeStep::Export;
		const HRESULT hr = ctx4->Wait(fence11_, value_);
		ctx4->Release();
		if (FAILED(hr)) {
			last_error = L"the Direct3D 11 fence wait failed";
			return BridgeStep::Export;
		}
		ctx->CopyResource(dst, color_.helper);
		ctx->Flush();
		return BridgeStep::Ok;
	}

	bool request_readable(SharedPlane &plane, uint32_t w, uint32_t h, DXGI_FORMAT fmt) override
	{
		if (!ready())
			return false;
		if (plane.matches(w, h, fmt))
			return true;
		engine_->wait_cpu(engine_->last_submitted());
		bridge_util::release_plane(plane);
		if (!bridge_util::make_nt_pair(*engine_, device11_, w, h, fmt,
				D3D11_BIND_SHADER_RESOURCE, plane)) {
			last_error = L"a shared texture could not be created";
			return false;
		}
		plane.game = { reinterpret_cast<uintptr_t>(plane.helper) };
		return true;
	}

private:
	bool ensure_like(SharedPlane &plane, ID3D11Resource *src, UINT bind_flags)
	{
		ID3D11Texture2D *tex = nullptr;
		if (FAILED(src->QueryInterface(IID_PPV_ARGS(&tex))) || tex == nullptr)
			return false;
		D3D11_TEXTURE2D_DESC d{};
		tex->GetDesc(&d);
		tex->Release();
		if (d.SampleDesc.Count != 1)
			return false;

		const DXGI_FORMAT want = shareable_format_for(d.Format);
		if (want != d.Format)
			bind_flags |= D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

		if (plane.matches(d.Width, d.Height, want))
			return true;

		engine_->wait_cpu(engine_->last_submitted());
		bridge_util::release_plane(plane);
		if (!bridge_util::make_nt_pair(*engine_, device11_, d.Width, d.Height, want,
				bind_flags, plane)) {
			last_error = L"this driver refused a shared texture between Direct3D 11 and "
				L"Direct3D 12";
			return false;
		}
		plane.game = { reinterpret_cast<uintptr_t>(plane.helper) };
		return true;
	}

	bool carry_in(ID3D11DeviceContext *ctx, SharedPlane &plane, ID3D11Resource *src)
	{
		ID3D11Texture2D *tex = nullptr;
		if (FAILED(src->QueryInterface(IID_PPV_ARGS(&tex))) || tex == nullptr)
			return false;
		D3D11_TEXTURE2D_DESC d{};
		tex->GetDesc(&d);
		tex->Release();

		if (d.Format == plane.format) {
			ctx->CopyResource(plane.helper, src);
			return true;
		}
		if (!blit_.ensure(device11_)) {
			last_error = L"this driver would not build the pass that converts the game's "
				L"depth buffer into a format the add-on can share";
			return false;
		}
		if (!blit_.blit(ctx, src, plane.helper)) {
			last_error = L"the game's depth buffer could not be converted for sharing";
			return false;
		}
		return true;
	}

	BlitPipelineD3D11 blit_;

	ID3D11Device *device11_ = nullptr;
	ID3D11Device5 *device11_5_ = nullptr;
	ID3D11Fence *fence11_ = nullptr;
	ID3D12Fence *fence12_ = nullptr;
	EngineDevice *engine_ = nullptr;
	uint64_t value_ = 0;

	SharedPlane color_{};
	SharedPlane depth_{};
	SharedPlane motion_{};
	SharedPlane scene_{};
};

}

FrameBridge *make_bridge_d3d12() { return new BridgeD3D12(); }
FrameBridge *make_bridge_d3d11() { return new BridgeD3D11(); }

}
