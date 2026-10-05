#include "aeon_sr/core/debug_view.hpp"

#include "aeon_sr/interop/native_d3d.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"

namespace aeon_sr {

namespace {

constexpr float kArrowCellPx = 32.0f;

}

void DebugView::release() noexcept
{
	if (field_copy_) { field_copy_->Release(); field_copy_ = nullptr; }
	if (frame_copy_) { frame_copy_->Release(); frame_copy_ = nullptr; }
	field_w_ = field_h_ = field_fmt_ = 0;
	frame_w_ = frame_h_ = frame_fmt_ = 0;
	device_ = nullptr;
}

bool DebugView::ensure_copy(ID3D11Device *dev, ID3D11Resource *src, ID3D11Texture2D **out,
	uint32_t *w, uint32_t *h, uint32_t *fmt) noexcept
{
	ID3D11Texture2D *tex = nullptr;
	if (src == nullptr ||
		FAILED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) ||
		tex == nullptr)
		return false;
	D3D11_TEXTURE2D_DESC d{};
	tex->GetDesc(&d);
	tex->Release();
	if (d.Width == 0 || d.Height == 0)
		return false;
	if (*out != nullptr && *w == d.Width && *h == d.Height &&
		*fmt == static_cast<uint32_t>(d.Format))
		return true;
	if (*out != nullptr) { (*out)->Release(); *out = nullptr; }

	D3D11_TEXTURE2D_DESC c{};
	c.Width = d.Width;
	c.Height = d.Height;
	c.MipLevels = 1;
	c.ArraySize = 1;
	c.Format = d.Format;
	c.SampleDesc.Count = 1;
	c.Usage = D3D11_USAGE_DEFAULT;
	c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(dev->CreateTexture2D(&c, nullptr, out)))
		return false;
	*w = d.Width;
	*h = d.Height;
	*fmt = static_cast<uint32_t>(d.Format);
	return true;
}

void DebugView::apply(const PassContext &ctx, const Settings &s, const FrameInputs &in,
	const DepthConvention &how)
{
	if (s.debug_view == 0 || s.debug_view > kDebugViewCount - 1u ||
		ctx.runtime == nullptr || ctx.pipelines == nullptr) {
		last = 0;
		logged = 0;
		note = "";
		return;
	}
	reshade::api::device *const device = ctx.runtime->get_device();
	if (device == nullptr)
		return;

	const reshade::api::resource field =
		s.debug_view == 3 ? in.motion_confidence
		: s.debug_view == 4 ? in.depth
		: in.motion_vectors;
	if (field.handle == 0 || in.color.handle == 0) {
		last = -6;
		note = s.debug_view == 3 ? "no confidence buffer yet"
			: s.debug_view == 4 ? "no depth buffer"
			: "no motion vectors yet - the estimator needs two frames";
		if (last != logged) {
			logged = last;
			diag_info("debug-view", L"nothing to draw yet");
		}
		return;
	}

	if (ID3D11Device *const dev11 = native_d3d11_device(device)) {
		ID3D11DeviceContext *const ctx11 = native_d3d11_context(ctx.runtime);
		auto *const src = native_res<ID3D11Resource>(field);
		auto *const dst = native_res<ID3D11Resource>(in.color);
		if (ctx11 == nullptr || src == nullptr || dst == nullptr) {
			last = -5;
			note = "no D3D11 context to draw with";
		} else {
			if (device_ != dev11)
				release();
			device_ = dev11;
			if (!ensure_copy(dev11, src, &field_copy_, &field_w_, &field_h_, &field_fmt_) ||
				!ensure_copy(dev11, dst, &frame_copy_, &frame_w_, &frame_h_, &frame_fmt_)) {
				last = -9;
				note = "could not allocate the debug copies";
			} else {
				ctx11->CopyResource(field_copy_, src);
				ctx11->CopyResource(frame_copy_, dst);
				last = ctx.pipelines->d3d11.debug_view_draw(ctx11, field_copy_, frame_copy_, dst,
					s.debug_view, kArrowCellPx, how.far_plane, how.reversed);
				note = last > 0 ? "" : "the draw was refused";
			}
		}
	} else {
		ID3D12Device *const dev12 = native_d3d12_device(device);
		ID3D12GraphicsCommandList *const cmd12 = native_d3d12_list(ctx.cmd_list);
		auto *const src = native_res<ID3D12Resource>(field);
		auto *const dst = native_res<ID3D12Resource>(in.color);
		if (dev12 != nullptr && cmd12 != nullptr && src != nullptr && dst != nullptr) {
			const reshade::api::resource_desc dd = device->get_resource_desc(in.color);
			const uint32_t mode = s.debug_view == 4 ? 2u : 1u;
			last = ctx.pipelines->d3d12.draw_fullscreen(dev12, cmd12, src,
				dst, static_cast<DXGI_FORMAT>(dd.texture.format), dd.texture.width, dd.texture.height,
				0.0f, mode) ? 1 : -7;
			note = s.debug_view == 2 ? "arrows need D3D11; showing the raw field" : "";
		} else {
			last = -5;
			note = "no D3D12 command list to draw with";
		}
	}

	if (last < 0 && last != logged) {
		logged = last;
		wchar_t buf[96]{};
		_snwprintf_s(buf, _TRUNCATE, L"debug view: failed at stage %d", last);
		diag_warn("debug-view", buf);
	} else if (last > 0) {
		logged = 0;
	}
}

}
