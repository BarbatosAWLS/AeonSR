#include "aeon_sr/interop/interop_bridges.hpp"
#include "aeon_sr/interop/native_d3d.hpp"

#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstring>
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

constexpr D3DFORMAT kFormatIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

constexpr DWORD kLastEnumFormat = 0xFFu;

const char kQuadHlsl[] = R"(
sampler2D srcSamp : register(s0);

struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(VSIn i)
{
	VSOut o;
	o.pos = float4(i.pos, 1.0);
	o.uv = i.uv;
	return o;
}

float4 PSCopy(float2 uv : TEXCOORD0) : COLOR
{
	return tex2D(srcSamp, uv);
}

float4 PSDepth(float2 uv : TEXCOORD0) : COLOR
{
	return tex2D(srcSamp, uv).r;
}
)";

struct QuadVertex {
	float x, y, z;
	float u, v;
};

D3DFORMAT d3d9_format_of(DXGI_FORMAT fmt) noexcept
{
	switch (fmt) {
	case DXGI_FORMAT_B8G8R8A8_UNORM: return D3DFMT_A8R8G8B8;
	case DXGI_FORMAT_B8G8R8X8_UNORM: return D3DFMT_X8R8G8B8;
	case DXGI_FORMAT_R10G10B10A2_UNORM: return D3DFMT_A2B10G10R10;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return D3DFMT_A16B16G16R16F;
	case DXGI_FORMAT_R32_FLOAT: return D3DFMT_R32F;
	case DXGI_FORMAT_R16_FLOAT: return D3DFMT_R16F;
	case DXGI_FORMAT_R16G16_FLOAT: return D3DFMT_G16R16F;
	case DXGI_FORMAT_R32G32_FLOAT: return D3DFMT_G32R32F;
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return D3DFMT_A32B32G32R32F;
	default: return D3DFMT_UNKNOWN;
	}
}

IDirect3DSurface9 *surface_of(reshade::api::resource r)
{
	auto *const res = native_res<IDirect3DResource9>(r);
	if (res == nullptr)
		return nullptr;
	switch (res->GetType()) {
	case D3DRTYPE_SURFACE: {
		auto *const surface = static_cast<IDirect3DSurface9 *>(res);
		surface->AddRef();
		return surface;
	}
	case D3DRTYPE_TEXTURE: {
		IDirect3DSurface9 *surface = nullptr;
		if (FAILED(static_cast<IDirect3DTexture9 *>(res)->GetSurfaceLevel(0, &surface)))
			return nullptr;
		return surface;
	}
	default:
		return nullptr;
	}
}

IDirect3DTexture9 *texture_of(reshade::api::resource r)
{
	auto *const res = native_res<IDirect3DResource9>(r);
	if (res == nullptr)
		return nullptr;
	if (res->GetType() == D3DRTYPE_TEXTURE) {
		auto *const tex = static_cast<IDirect3DTexture9 *>(res);
		tex->AddRef();
		return tex;
	}
	if (res->GetType() == D3DRTYPE_SURFACE) {
		IDirect3DTexture9 *tex = nullptr;
		static_cast<IDirect3DSurface9 *>(res)->GetContainer(IID_PPV_ARGS(&tex));
		return tex;
	}
	return nullptr;
}

class BridgeD3D9 final : public FrameBridge {
public:
	~BridgeD3D9() override { shutdown(); }

	BridgeKind kind() const noexcept override { return BridgeKind::Legacy9; }

	const char *name() const noexcept override
	{
		return used_hop_
			? "Direct3D 9Ex shared texture through a Direct3D 11 hop"
			: "Direct3D 9Ex shared texture";
	}

	const char *sync_name() const noexcept override
	{
		return used_hop_
			? "CPU wait (Direct3D 9 event query), then a shared fence on the Direct3D 11 hop"
			: "CPU wait (Direct3D 9 has no fence another device can see)";
	}

	bool init(reshade::api::device *game, EngineDevice &engine) override
	{
		shutdown();
		last_error.clear();

		if (game == nullptr || game->get_api() != reshade::api::device_api::d3d9) {
			last_error = L"not a Direct3D 9 device";
			return false;
		}
		auto *const device = reinterpret_cast<IDirect3DDevice9 *>(game->get_native());
		if (device == nullptr) {
			last_error = L"the Direct3D 9 device could not be read back from ReShade";
			return false;
		}
		if (!engine.ready()) {
			last_error = engine.last_error.empty()
				? std::wstring(L"the engine device is not ready") : engine.last_error;
			return false;
		}
		if (!engine.adapter_matched()) {
			last_error = L"the add-on's Direct3D 12 device came up on a different graphics "
				L"card from the game's, and a shared texture cannot cross two cards. Point "
				L"the game and the add-on at the same GPU in Windows Graphics settings and "
				L"start the game again.";
			return false;
		}

		if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device9ex_))) || device9ex_ == nullptr) {
			safe_release(device9ex_);
			last_error = L"this game's Direct3D 9 device is a plain one (Direct3D 9Ex refused it), and "
				L"only Direct3D 9Ex can hand a texture to another device. The frame will travel "
				L"through system memory instead, which is slow: around 13 ms a frame at 1280x720 "
				L"and several times that at 2560x1440.";
			return false;
		}
		device9_ = device;
		device9_->AddRef();
		engine_ = &engine;

		D3DCAPS9 caps{};
		if (FAILED(device9_->GetDeviceCaps(&caps))) {
			last_error = L"the Direct3D 9 device would not report its capabilities";
			shutdown();
			return false;
		}
		shader_model3_ = caps.PixelShaderVersion >= D3DPS_VERSION(3, 0) &&
			caps.VertexShaderVersion >= D3DVS_VERSION(3, 0);
		num_rts_ = caps.NumSimultaneousRTs;
		if (num_rts_ == 0)
			num_rts_ = 1;
		if (num_rts_ > kMaxRenderTargets)
			num_rts_ = kMaxRenderTargets;

		if (FAILED(device9_->CreateQuery(D3DQUERYTYPE_EVENT, nullptr))) {
			last_error = L"this Direct3D 9 driver has no event query, which is the only way "
				L"to know when the game has finished drawing. Without it the upscaler would "
				L"read half-written frames, so the add-on will not bridge this device.";
			shutdown();
			return false;
		}

		D3DDEVICE_CREATION_PARAMETERS cp{};
		if (SUCCEEDED(device9_->GetCreationParameters(&cp)))
			mixed_vp_ = (cp.BehaviorFlags & D3DCREATE_MIXED_VERTEXPROCESSING) != 0;

		bool shareable = false;
		direct_ok_ = probe_direct(shareable);
		if (!shareable) {
			last_error = L"this driver would not create a shareable texture on the game's "
				L"Direct3D 9Ex device, so nothing can reach the upscaler without going "
				L"through system memory. A graphics driver update is the thing to try.";
			shutdown();
			return false;
		}
		if (!direct_ok_ && !ensure_hop()) {
			shutdown();
			return false;
		}
		used_hop_ = !direct_ok_;
		return true;
	}

	void shutdown() override
	{
		if (engine_ != nullptr)
			engine_->wait_cpu(engine_->last_submitted());
		idle_11();
		idle_9();

		for (Readable &r : readables_)
			safe_release(r.tex9);
		readables_.clear();

		release_plane(color_);
		release_plane(depth_);
		release_plane(motion_);

		safe_release(decl_);
		safe_release(ps_depth_);
		safe_release(ps_copy_);
		safe_release(vs_);

		safe_release(query11_);
		safe_release(fence12_);
		safe_release(fence11_);
		safe_release(context11_);
		safe_release(helper11_);

		safe_release(query9_);
		safe_release(device9ex_);
		safe_release(device9_);

		engine_ = nullptr;
		value_ = 0;
		num_rts_ = 1;
		mixed_vp_ = false;
		shader_model3_ = false;
		direct_ok_ = false;
		used_hop_ = false;
	}

	bool ready() const noexcept override
	{
		return device9_ != nullptr && device9ex_ != nullptr && engine_ != nullptr;
	}

	BridgeStep begin(reshade::api::effect_runtime *, reshade::api::command_list *,
		const BridgeInputs &in, BridgeFrame &out) override
	{
		out = BridgeFrame{};
		if (!ready())
			return BridgeStep::Init;
		last_error.clear();

		IDirect3DSurface9 *game_color = surface_of(in.color);
		D3DSURFACE_DESC cd{};
		if (game_color == nullptr || FAILED(game_color->GetDesc(&cd))) {
			safe_release(game_color);
			last_error = L"the game's colour buffer is not a Direct3D 9 surface this bridge "
				L"knows how to read";
			return BridgeStep::Import;
		}

		const D3DFORMAT color_fmt = cd.Format == D3DFMT_X8R8G8B8 ? D3DFMT_A8R8G8B8 : cd.Format;
		const bool have_color = ensure_plane(color_, cd.Width, cd.Height, color_fmt, true,
			D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE) &&
			carry_in(game_color, in.color, color_);
		safe_release(game_color);
		if (!have_color)
			return BridgeStep::Import;

		ID3D12Resource *const depth12 = in.depth.handle != 0 ? carry_depth(in.depth) : nullptr;
		ID3D12Resource *const motion12 = in.motion_vectors.handle != 0
			? carry_motion(in.motion_vectors) : nullptr;

		if (!idle_9()) {
			last_error = L"the Direct3D 9 device never reported the copy into the shared "
				L"textures as finished. The game may have lost the graphics device.";
			return BridgeStep::Import;
		}

		if (!cross_hop_in(depth12 != nullptr && depth12 == depth_.shared.engine,
				motion12 != nullptr && motion12 == motion_.shared.engine))
			return BridgeStep::Import;

		out.cmd = engine_->begin_list();
		if (out.cmd == nullptr) {
			last_error = engine_->last_error;
			return BridgeStep::Begin;
		}
		out.color = color_.shared.engine;
		out.depth = depth12;
		out.motion_vectors = motion12;
		out.width = color_.width;
		out.height = color_.height;
		return BridgeStep::Ok;
	}

	BridgeStep end(reshade::api::effect_runtime *, reshade::api::command_list *,
		reshade::api::resource game_color) override
	{
		if (!ready())
			return BridgeStep::Init;
		if (!engine_->submit_list()) {
			last_error = engine_->last_error;
			return BridgeStep::Finish;
		}

		if (color_.hop != nullptr) {
			if (FAILED(engine_->queue()->Signal(fence12_, ++value_))) {
				last_error = L"the engine queue refused a fence signal";
				return BridgeStep::Finish;
			}
			ID3D11DeviceContext4 *ctx4 = nullptr;
			if (FAILED(context11_->QueryInterface(IID_PPV_ARGS(&ctx4))) || ctx4 == nullptr) {
				safe_release(ctx4);
				last_error = L"the helper Direct3D 11 context cannot wait on a fence";
				return BridgeStep::Export;
			}
			const HRESULT hr = ctx4->Wait(fence11_, value_);
			ctx4->Release();
			if (FAILED(hr)) {
				last_error = L"the helper Direct3D 11 device refused to wait for the engine";
				return BridgeStep::Export;
			}
			context11_->CopyResource(color_.hop, color_.shared.helper);
			if (!idle_11()) {
				last_error = L"the helper Direct3D 11 device never finished the copy back "
					L"towards the game";
				return BridgeStep::Export;
			}
		} else if (!engine_->wait_cpu(engine_->last_submitted())) {
			last_error = engine_->last_error;
			return BridgeStep::Export;
		}

		IDirect3DSurface9 *dst = surface_of(game_color);
		D3DSURFACE_DESC dd{};
		if (dst == nullptr || FAILED(dst->GetDesc(&dd))) {
			safe_release(dst);
			last_error = L"the finished frame has nowhere to go: the game's colour buffer is "
				L"not a Direct3D 9 surface this bridge knows how to write";
			return BridgeStep::Export;
		}

		bool ok = false;
		if (dd.MultiSampleType == D3DMULTISAMPLE_NONE) {
			const D3DTEXTUREFILTERTYPE filter =
				(dd.Width == color_.width && dd.Height == color_.height)
					? D3DTEXF_NONE : D3DTEXF_LINEAR;
			ok = SUCCEEDED(device9_->StretchRect(color_.surf9, nullptr, dst, nullptr, filter));
		}
		if (!ok)
			ok = draw_quad(color_.tex9, dst, dd.Width, dd.Height, false);
		safe_release(dst);

		if (!ok) {
			if (last_error.empty())
				last_error = L"the upscaled frame could not be written back into the game's "
					L"colour buffer";
			return BridgeStep::Export;
		}
		return BridgeStep::Ok;
	}

	bool request_readable(SharedPlane &plane, uint32_t w, uint32_t h, DXGI_FORMAT fmt) override
	{
		if (!ready() || w == 0 || h == 0 || fmt == DXGI_FORMAT_UNKNOWN)
			return false;
		if (plane.matches(w, h, fmt))
			return true;
		engine_->wait_cpu(engine_->last_submitted());
		drop_readable(&plane);
		bridge_util::release_plane(plane);

		if (direct_ok_ && make_readable_shared(plane, w, h, fmt))
			return true;

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
			last_error = L"the engine device refused a texture for the estimator's output";
			return false;
		}
		plane.width = w;
		plane.height = h;
		plane.format = fmt;
		return true;
	}

private:
	static constexpr DWORD kMaxRenderTargets = 8;
	static constexpr ULONGLONG kWaitMs = 1000;

	struct Plane9 {
		IDirect3DTexture9 *tex9 = nullptr;
		IDirect3DSurface9 *surf9 = nullptr;
		HANDLE kmt = nullptr;
		ID3D11Texture2D *hop = nullptr;
		SharedPlane shared{};
		D3DFORMAT want9 = D3DFMT_UNKNOWN;
		D3DFORMAT fmt9 = D3DFMT_UNKNOWN;
		uint32_t width = 0;
		uint32_t height = 0;

		bool matches(uint32_t w, uint32_t h, D3DFORMAT want) const noexcept
		{
			return shared.engine != nullptr && width == w && height == h && want9 == want;
		}
	};

	struct Readable {
		SharedPlane *owner = nullptr;
		IDirect3DTexture9 *tex9 = nullptr;
	};

	struct SavedState {
		IDirect3DStateBlock9 *block = nullptr;
		IDirect3DSurface9 *rts[kMaxRenderTargets]{};
		IDirect3DSurface9 *depth_stencil = nullptr;
		D3DVIEWPORT9 viewport{};
		BOOL software_vp = FALSE;
	};

	void release_plane(Plane9 &p)
	{
		bridge_util::release_plane(p.shared);
		safe_release(p.hop);
		safe_release(p.surf9);
		safe_release(p.tex9);
		p.kmt = nullptr;
		p.want9 = D3DFMT_UNKNOWN;
		p.fmt9 = D3DFMT_UNKNOWN;
		p.width = 0;
		p.height = 0;
	}

	bool probe_direct(bool &shareable)
	{
		shareable = false;
		IDirect3DTexture9 *tex = nullptr;
		HANDLE kmt = nullptr;
		if (FAILED(device9_->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
				D3DPOOL_DEFAULT, &tex, &kmt)) || tex == nullptr || kmt == nullptr) {
			safe_release(tex);
			return false;
		}
		shareable = true;

		ID3D12Resource *opened = nullptr;
		const bool ok = SUCCEEDED(engine_->device()->OpenSharedHandle(kmt, IID_PPV_ARGS(&opened))) &&
			opened != nullptr;
		safe_release(opened);
		safe_release(tex);
		return ok;
	}

	bool ensure_hop()
	{
		if (helper11_ != nullptr && context11_ != nullptr && fence11_ != nullptr && fence12_ != nullptr)
			return true;

		ID3D11Device *const device = engine_->device11();
		ID3D11DeviceContext *const context = engine_->context11();
		ID3D11Device5 *const device5 = engine_->device11_5();
		if (device == nullptr || context == nullptr) {
			last_error = L"Direct3D 12 would not open the game's Direct3D 9 texture directly "
				L"and the helper Direct3D 11 device that would carry it instead could not be "
				L"created, so there is no way across.";
			return false;
		}
		if (device5 == nullptr) {
			last_error = L"the frame has to cross through a helper Direct3D 11 device, and "
				L"this driver is older than Windows 10 1709, where the fence that orders the "
				L"two devices was added.";
			return false;
		}

		HANDLE fence_handle = nullptr;
		const bool ok = SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11_))) &&
			SUCCEEDED(fence11_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle)) &&
			SUCCEEDED(engine_->device()->OpenSharedHandle(fence_handle, IID_PPV_ARGS(&fence12_)));
		if (fence_handle != nullptr)
			CloseHandle(fence_handle);
		if (!ok) {
			safe_release(fence12_);
			safe_release(fence11_);
			last_error = L"this driver refused a shared fence between Direct3D 11 and "
				L"Direct3D 12, which is what orders the two halves of the crossing.";
			return false;
		}

		helper11_ = device;
		helper11_->AddRef();
		context11_ = context;
		context11_->AddRef();
		return true;
	}

	bool ensure_plane(Plane9 &p, uint32_t w, uint32_t h, D3DFORMAT want, bool allow_bgra,
		UINT bind_flags)
	{
		if (p.matches(w, h, want))
			return true;
		if (w == 0 || h == 0 || want == D3DFMT_UNKNOWN)
			return false;

		engine_->wait_cpu(engine_->last_submitted());
		idle_11();
		idle_9();
		release_plane(p);

		const std::wstring note = last_error;
		bool made = make_plane(p, w, h, want, bind_flags);
		if (!made && allow_bgra && want != D3DFMT_A8R8G8B8) {
			release_plane(p);
			made = make_plane(p, w, h, D3DFMT_A8R8G8B8, bind_flags);
		}
		if (!made) {
			release_plane(p);
			if (last_error == note)
				last_error = L"this driver would not hand the game's frame to the upscaler as "
					L"a shared texture. A graphics driver update is the thing to try.";
			return false;
		}

		last_error = note;
		p.want9 = want;
		return true;
	}

	bool make_plane(Plane9 &p, uint32_t w, uint32_t h, D3DFORMAT fmt, UINT bind_flags)
	{
		if (FAILED(device9_->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT,
				&p.tex9, &p.kmt)) || p.tex9 == nullptr || p.kmt == nullptr)
			return false;
		if (FAILED(p.tex9->GetSurfaceLevel(0, &p.surf9)) || p.surf9 == nullptr)
			return false;

		if (direct_ok_ &&
			SUCCEEDED(engine_->device()->OpenSharedHandle(p.kmt, IID_PPV_ARGS(&p.shared.engine))) &&
			p.shared.engine != nullptr) {
			p.shared.format = p.shared.engine->GetDesc().Format;
			p.shared.width = w;
			p.shared.height = h;
		} else {
			safe_release(p.shared.engine);
			if (!ensure_hop() || !bridge_util::open_kmt_texture(helper11_, p.kmt, &p.hop))
				return false;
			D3D11_TEXTURE2D_DESC hd{};
			p.hop->GetDesc(&hd);
			if (!bridge_util::make_nt_pair(*engine_, helper11_, w, h, hd.Format, bind_flags,
					p.shared))
				return false;
			used_hop_ = true;
		}

		p.shared.game = { reinterpret_cast<uintptr_t>(static_cast<IDirect3DResource9 *>(p.tex9)) };
		p.fmt9 = fmt;
		p.width = w;
		p.height = h;
		return true;
	}

	bool carry_in(IDirect3DSurface9 *src, reshade::api::resource src_res, Plane9 &p)
	{
		if (SUCCEEDED(device9_->StretchRect(src, nullptr, p.surf9, nullptr, D3DTEXF_NONE)))
			return true;

		IDirect3DTexture9 *const tex = texture_of(src_res);
		if (tex == nullptr) {
			last_error = L"Direct3D 9 would not copy the game's frame into the shared texture, "
				L"and the frame is not in a texture the bridge could draw from instead. A back "
				L"buffer in a format the driver will not convert is the usual cause.";
			return false;
		}
		const bool ok = draw_quad(tex, p.surf9, p.width, p.height, false);
		tex->Release();
		return ok;
	}

	ID3D12Resource *carry_depth(reshade::api::resource depth)
	{
		IDirect3DTexture9 *tex = texture_of(depth);
		D3DSURFACE_DESC dd{};
		if (tex == nullptr || FAILED(tex->GetLevelDesc(0, &dd))) {
			safe_release(tex);
			last_error = L"the game's depth buffer is not a texture this bridge can sample, "
				L"so the upscaler runs without depth and edges around moving objects will be "
				L"softer than they should be.";
			return nullptr;
		}
		if (dd.Format != kFormatIntz && ((dd.Usage & D3DUSAGE_DEPTHSTENCIL) != 0 ||
				static_cast<DWORD>(dd.Format) > kLastEnumFormat)) {
			tex->Release();
			last_error = L"this game's depth buffer is in an encoding this bridge does not "
				L"decode - DF24, DF16 and RAWZ are the usual ones. The upscaler runs without "
				L"depth, so edges around moving objects will be softer than they should be. "
				L"Only INTZ depth is carried on Direct3D 9 today.";
			return nullptr;
		}

		if (!ensure_plane(depth_, dd.Width, dd.Height, D3DFMT_R32F, false,
				D3D11_BIND_SHADER_RESOURCE)) {
			tex->Release();
			return nullptr;
		}

		bool ok = false;
		if (dd.Format == D3DFMT_R32F && (dd.Usage & D3DUSAGE_RENDERTARGET) != 0) {
			IDirect3DSurface9 *src = nullptr;
			if (SUCCEEDED(tex->GetSurfaceLevel(0, &src)) && src != nullptr)
				ok = SUCCEEDED(device9_->StretchRect(src, nullptr, depth_.surf9, nullptr,
					D3DTEXF_NONE));
			safe_release(src);
		}
		if (!ok)
			ok = draw_quad(tex, depth_.surf9, depth_.width, depth_.height, true);
		tex->Release();

		if (!ok) {
			if (last_error.empty())
				last_error = L"the game's depth buffer could not be converted into a format "
					L"the upscaler can read, so it runs without depth";
			return nullptr;
		}
		return depth_.shared.engine;
	}

	ID3D12Resource *carry_motion(reshade::api::resource motion)
	{
		if (SharedPlane *const written = readable_for(motion); written != nullptr)
			return written->engine;

		IDirect3DSurface9 *src = surface_of(motion);
		D3DSURFACE_DESC md{};
		if (src == nullptr || FAILED(src->GetDesc(&md))) {
			safe_release(src);
			return nullptr;
		}
		const bool ok = ensure_plane(motion_, md.Width, md.Height, md.Format, false,
			D3D11_BIND_SHADER_RESOURCE) && carry_in(src, motion, motion_);
		src->Release();
		return ok ? motion_.shared.engine : nullptr;
	}

	bool cross_hop_in(bool have_depth, bool have_motion)
	{
		Plane9 *const planes[3] = { &color_, have_depth ? &depth_ : nullptr,
			have_motion ? &motion_ : nullptr };
		bool any = false;
		for (Plane9 *const p : planes) {
			if (p == nullptr || p->hop == nullptr)
				continue;
			context11_->CopyResource(p->shared.helper, p->hop);
			any = true;
		}
		if (!any)
			return true;

		ID3D11DeviceContext4 *ctx4 = nullptr;
		if (FAILED(context11_->QueryInterface(IID_PPV_ARGS(&ctx4))) || ctx4 == nullptr) {
			safe_release(ctx4);
			last_error = L"the helper Direct3D 11 context cannot signal a fence";
			return false;
		}
		const HRESULT hr = ctx4->Signal(fence11_, ++value_);
		ctx4->Release();
		if (FAILED(hr)) {
			last_error = L"the helper Direct3D 11 fence signal failed";
			return false;
		}
		if (FAILED(engine_->queue()->Wait(fence12_, value_))) {
			last_error = L"the engine queue refused to wait on the shared fence";
			return false;
		}
		return true;
	}

	bool ensure_quad()
	{
		if (vs_ != nullptr && ps_copy_ != nullptr && ps_depth_ != nullptr && decl_ != nullptr)
			return true;
		if (!shader_model3_) {
			last_error = L"this graphics card does not run Direct3D 9 shader model 3, which "
				L"is what the bridge needs to convert the game's depth buffer. The upscaler "
				L"runs without depth.";
			return false;
		}

		ID3DBlob *vs_blob = nullptr;
		ID3DBlob *copy_blob = nullptr;
		ID3DBlob *depth_blob = nullptr;
		bool ok = compile_quad("VSMain", "vs_3_0", &vs_blob) &&
			compile_quad("PSCopy", "ps_3_0", &copy_blob) &&
			compile_quad("PSDepth", "ps_3_0", &depth_blob);
		if (ok)
			ok = SUCCEEDED(device9_->CreateVertexShader(
					static_cast<const DWORD *>(vs_blob->GetBufferPointer()), &vs_)) &&
				SUCCEEDED(device9_->CreatePixelShader(
					static_cast<const DWORD *>(copy_blob->GetBufferPointer()), &ps_copy_)) &&
				SUCCEEDED(device9_->CreatePixelShader(
					static_cast<const DWORD *>(depth_blob->GetBufferPointer()), &ps_depth_));
		safe_release(depth_blob);
		safe_release(copy_blob);
		safe_release(vs_blob);

		if (ok) {
			const D3DVERTEXELEMENT9 elements[] = {
				{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
				{ 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
				D3DDECL_END()
			};
			ok = SUCCEEDED(device9_->CreateVertexDeclaration(elements, &decl_));
		}
		if (!ok) {
			safe_release(decl_);
			safe_release(ps_depth_);
			safe_release(ps_copy_);
			safe_release(vs_);
			last_error = L"the small shader the bridge uses to convert the game's depth "
				L"buffer would not build on this driver. The upscaler runs without depth.";
			return false;
		}
		return true;
	}

	bool compile_quad(const char *entry, const char *target, ID3DBlob **out)
	{
		ID3DBlob *errors = nullptr;
		const HRESULT hr = D3DCompile(kQuadHlsl, std::strlen(kQuadHlsl), "aeon_bridge9",
			nullptr, nullptr, entry, target, 0, 0, out, &errors);
		safe_release(errors);
		return SUCCEEDED(hr) && *out != nullptr;
	}

	bool capture_state(SavedState &s)
	{
		if (FAILED(device9_->CreateStateBlock(D3DSBT_ALL, &s.block)) || s.block == nullptr) {
			safe_release(s.block);
			last_error = L"the Direct3D 9 device would not save its render state, so the "
				L"bridge will not draw on it";
			return false;
		}
		s.block->Capture();
		device9_->GetViewport(&s.viewport);
		for (DWORD i = 0; i < num_rts_; ++i)
			device9_->GetRenderTarget(i, &s.rts[i]);
		device9_->GetDepthStencilSurface(&s.depth_stencil);
		if (mixed_vp_) {
			s.software_vp = device9_->GetSoftwareVertexProcessing();
			device9_->SetSoftwareVertexProcessing(FALSE);
		}
		return true;
	}

	void restore_state(SavedState &s)
	{
		if (s.block != nullptr) {
			s.block->Apply();
			s.block->Release();
			s.block = nullptr;
		}
		if (mixed_vp_)
			device9_->SetSoftwareVertexProcessing(s.software_vp);
		for (DWORD i = 0; i < num_rts_; ++i) {
			if (i != 0 || s.rts[0] != nullptr)
				device9_->SetRenderTarget(i, s.rts[i]);
			safe_release(s.rts[i]);
		}
		device9_->SetDepthStencilSurface(s.depth_stencil);
		safe_release(s.depth_stencil);
		device9_->SetViewport(&s.viewport);
	}

	bool draw_quad(IDirect3DBaseTexture9 *src, IDirect3DSurface9 *dst, uint32_t w, uint32_t h,
		bool depth)
	{
		if (src == nullptr || dst == nullptr || w == 0 || h == 0)
			return false;
		if (!ensure_quad())
			return false;

		SavedState saved{};
		if (!capture_state(saved))
			return false;

		const bool opened_scene = SUCCEEDED(device9_->BeginScene());

		bool ok = SUCCEEDED(device9_->SetRenderTarget(0, dst));
		for (DWORD i = 1; i < num_rts_; ++i)
			device9_->SetRenderTarget(i, nullptr);
		ok = ok && SUCCEEDED(device9_->SetDepthStencilSurface(nullptr));

		D3DVIEWPORT9 vp{};
		vp.Width = w;
		vp.Height = h;
		vp.MaxZ = 1.0f;
		ok = ok && SUCCEEDED(device9_->SetViewport(&vp));

		device9_->SetRenderState(D3DRS_ZENABLE, FALSE);
		device9_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		device9_->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		device9_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		device9_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		device9_->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		device9_->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
		device9_->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		device9_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		device9_->SetRenderState(D3DRS_FOGENABLE, FALSE);
		device9_->SetRenderState(D3DRS_CLIPPING, FALSE);
		device9_->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
		device9_->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
		device9_->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED |
			D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
		device9_->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

		device9_->SetTexture(0, src);
		device9_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		device9_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		device9_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		device9_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		device9_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		device9_->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
		device9_->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

		ok = ok && SUCCEEDED(device9_->SetVertexDeclaration(decl_)) &&
			SUCCEEDED(device9_->SetVertexShader(vs_)) &&
			SUCCEEDED(device9_->SetPixelShader(depth ? ps_depth_ : ps_copy_));

		const float ox = -1.0f / static_cast<float>(w);
		const float oy = 1.0f / static_cast<float>(h);
		const QuadVertex vertices[4] = {
			{ -1.0f + ox,  1.0f + oy, 0.0f, 0.0f, 0.0f },
			{  1.0f + ox,  1.0f + oy, 0.0f, 1.0f, 0.0f },
			{ -1.0f + ox, -1.0f + oy, 0.0f, 0.0f, 1.0f },
			{  1.0f + ox, -1.0f + oy, 0.0f, 1.0f, 1.0f },
		};
		ok = ok && SUCCEEDED(device9_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices,
			static_cast<UINT>(sizeof(QuadVertex))));

		if (opened_scene)
			device9_->EndScene();
		restore_state(saved);
		if (!ok && last_error.empty())
			last_error = L"the Direct3D 9 device turned down the copy the bridge draws";
		return ok;
	}

	bool idle_9()
	{
		if (device9_ == nullptr)
			return false;
		if (query9_ == nullptr && FAILED(device9_->CreateQuery(D3DQUERYTYPE_EVENT, &query9_)))
			query9_ = nullptr;
		if (query9_ == nullptr)
			return false;

		query9_->Issue(D3DISSUE_END);
		const ULONGLONG deadline = GetTickCount64() + kWaitMs;
		HRESULT hr = S_FALSE;
		while ((hr = query9_->GetData(nullptr, 0, D3DGETDATA_FLUSH)) == S_FALSE) {
			if (GetTickCount64() >= deadline)
				return false;
			Sleep(0);
		}
		return hr == S_OK;
	}

	bool idle_11()
	{
		if (helper11_ == nullptr || context11_ == nullptr)
			return true;
		if (query11_ == nullptr) {
			D3D11_QUERY_DESC qd{};
			qd.Query = D3D11_QUERY_EVENT;
			if (FAILED(helper11_->CreateQuery(&qd, &query11_)))
				query11_ = nullptr;
		}
		if (query11_ == nullptr)
			return false;

		context11_->End(query11_);
		context11_->Flush();
		BOOL done = FALSE;
		const ULONGLONG deadline = GetTickCount64() + kWaitMs;
		while (context11_->GetData(query11_, &done, sizeof(done), 0) != S_OK) {
			if (GetTickCount64() >= deadline)
				return false;
			Sleep(0);
		}
		return done != FALSE;
	}

	bool make_readable_shared(SharedPlane &plane, uint32_t w, uint32_t h, DXGI_FORMAT fmt)
	{
		const D3DFORMAT fmt9 = d3d9_format_of(fmt);
		if (fmt9 == D3DFMT_UNKNOWN)
			return false;

		IDirect3DTexture9 *tex = nullptr;
		HANDLE kmt = nullptr;
		if (FAILED(device9_->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt9, D3DPOOL_DEFAULT,
				&tex, &kmt)) || tex == nullptr || kmt == nullptr) {
			safe_release(tex);
			return false;
		}

		ID3D12Resource *opened = nullptr;
		if (FAILED(engine_->device()->OpenSharedHandle(kmt, IID_PPV_ARGS(&opened))) ||
			opened == nullptr || opened->GetDesc().Format != fmt) {
			safe_release(opened);
			tex->Release();
			return false;
		}

		plane.engine = opened;
		plane.width = w;
		plane.height = h;
		plane.format = fmt;
		plane.game = { reinterpret_cast<uintptr_t>(static_cast<IDirect3DResource9 *>(tex)) };
		readables_.push_back(Readable{ &plane, tex });
		return true;
	}

	SharedPlane *readable_for(reshade::api::resource r) const
	{
		for (const Readable &entry : readables_)
			if (entry.owner != nullptr && entry.owner->game.handle == r.handle &&
				entry.owner->engine != nullptr)
				return entry.owner;
		return nullptr;
	}

	void drop_readable(SharedPlane *owner)
	{
		for (auto it = readables_.begin(); it != readables_.end(); ++it) {
			if (it->owner != owner)
				continue;
			safe_release(it->tex9);
			readables_.erase(it);
			return;
		}
	}

	IDirect3DDevice9 *device9_ = nullptr;
	IDirect3DDevice9Ex *device9ex_ = nullptr;
	IDirect3DQuery9 *query9_ = nullptr;
	EngineDevice *engine_ = nullptr;

	ID3D11Device *helper11_ = nullptr;
	ID3D11DeviceContext *context11_ = nullptr;
	ID3D11Query *query11_ = nullptr;
	ID3D11Fence *fence11_ = nullptr;
	ID3D12Fence *fence12_ = nullptr;
	uint64_t value_ = 0;

	IDirect3DVertexShader9 *vs_ = nullptr;
	IDirect3DPixelShader9 *ps_copy_ = nullptr;
	IDirect3DPixelShader9 *ps_depth_ = nullptr;
	IDirect3DVertexDeclaration9 *decl_ = nullptr;

	Plane9 color_{};
	Plane9 depth_{};
	Plane9 motion_{};
	std::vector<Readable> readables_;

	DWORD num_rts_ = 1;
	bool mixed_vp_ = false;
	bool shader_model3_ = false;
	bool direct_ok_ = false;
	bool used_hop_ = false;
};

}

FrameBridge *make_bridge_d3d9() { return new BridgeD3D9(); }

}
