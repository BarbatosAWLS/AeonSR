#include "aeon_sr/jitter/frame_shift.hpp"

#include "aeon_sr/core/dxgi_format_util.hpp"

#include <cmath>

namespace aeon_sr {

namespace {

uint32_t uncovered(float shift) noexcept
{
	return shift >= 0.5f ? static_cast<uint32_t>(std::floor(shift - 0.5f)) + 1u : 0u;
}

uint32_t trailing(float shift, ViewportClip clip) noexcept
{
	const uint32_t centres = uncovered(-shift);
	if (clip != ViewportClip::WholePixels || shift >= 0.0f)
		return centres;
	const uint32_t whole = static_cast<uint32_t>(std::ceil(-shift));
	return whole > centres ? whole : centres;
}

uint32_t in_frame(uint32_t lost, uint32_t scene, uint32_t frame) noexcept
{
	if (lost == 0 || scene == frame)
		return lost;
	const double k = static_cast<double>(frame) / static_cast<double>(scene);
	return static_cast<uint32_t>(std::ceil(k * (static_cast<double>(lost) + 0.5) - 0.5 - 1e-9));
}

}

FrameValidRect frame_valid_rect(float shift_x, float shift_y, uint32_t w, uint32_t h, ViewportClip clip,
	uint32_t scene_w, uint32_t scene_h) noexcept
{
	FrameValidRect r;
	if (w == 0 || h == 0)
		return r;
	if (scene_w == 0 || scene_h == 0) {
		scene_w = w;
		scene_h = h;
	}
	const float sx = shift_x * static_cast<float>(scene_w) / static_cast<float>(w);
	const float sy = shift_y * static_cast<float>(scene_h) / static_cast<float>(h);
	r.left = in_frame(uncovered(sx), scene_w, w);
	r.right = in_frame(trailing(sx, clip), scene_w, w);
	r.top = in_frame(uncovered(sy), scene_h, h);
	r.bottom = in_frame(trailing(sy, clip), scene_h, h);
	if (r.left + r.right >= w || r.top + r.bottom >= h) {
		r.left = r.right = r.top = r.bottom = 0;
		return r;
	}
	r.any = r.left + r.right + r.top + r.bottom != 0;
	const float fw = static_cast<float>(w), fh = static_cast<float>(h);
	r.uv[0] = (static_cast<float>(r.left) + 0.5f) / fw;
	r.uv[1] = (static_cast<float>(r.top) + 0.5f) / fh;
	r.uv[2] = (fw - static_cast<float>(r.right) - 0.5f) / fw;
	r.uv[3] = (fh - static_cast<float>(r.bottom) - 0.5f) / fh;
	return r;
}

bool FrameShiftD3D12::apply(ID3D12Device *device, ID3D12CommandQueue *queue, Gpu12Fence &fence,
	ID3D12GraphicsCommandList *cmd, BlitPipelineD3D12 &blit, ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
	float u, float v, const float *valid_uv, bool catmull_rom)
{
	if (device == nullptr || cmd == nullptr || color == nullptr)
		return false;
	const D3D12_RESOURCE_DESC cd = color->GetDesc();
	if ((cd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 || cd.SampleDesc.Count != 1)
		return false;
	const uint32_t w = static_cast<uint32_t>(cd.Width), h = cd.Height;
	if (!ensure_scratch(device, queue, fence, cd) || !blit.ensure(device))
		return false;

	barrier12(cmd, color, color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier12(cmd, scratch_, kState, D3D12_RESOURCE_STATE_COPY_DEST);
	cmd->CopyResource(scratch_, color);
	barrier12(cmd, scratch_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	const bool ok = blit.draw_fullscreen(device, cmd, scratch_, color, view_format_for(cd.Format), w, h, 0.0f, 0u, u, v,
		valid_uv, catmull_rom);
	if (!ok) {
		barrier12(cmd, color, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
		barrier12(cmd, scratch_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
		cmd->CopyResource(color, scratch_);
		barrier12(cmd, scratch_, D3D12_RESOURCE_STATE_COPY_SOURCE, kState);
		barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_DEST, color_state);
		return false;
	}
	barrier12(cmd, color, D3D12_RESOURCE_STATE_RENDER_TARGET, color_state);
	barrier12(cmd, scratch_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kState);
	return true;
}

bool FrameShiftD3D12::ensure_scratch(ID3D12Device *device, ID3D12CommandQueue *queue, Gpu12Fence &fence,
	const D3D12_RESOURCE_DESC &cd)
{
	const uint32_t w = static_cast<uint32_t>(cd.Width), h = cd.Height;
	const bool same = format_ == cd.Format && w_ == w && h_ == h;
	if (scratch_ != nullptr && !same) {
		fence.signal_and_wait(device, queue);
		scratch_->Release();
		scratch_ = nullptr;
	}
	if (scratch_ == nullptr) {
		if (same && failed_)
			return false;
		format_ = cd.Format;
		w_ = w;
		h_ = h;
		failed_ = !create_tex12(device, w, h, cd.Format, false, kState, &scratch_);
		if (failed_)
			return false;
	}
	return true;
}

bool FrameShiftD3D12::fill_edges(ID3D12Device *device, ID3D12CommandQueue *queue, Gpu12Fence &fence,
	ID3D12GraphicsCommandList *cmd, BlitPipelineD3D12 &blit, ID3D12Resource *color,
	D3D12_RESOURCE_STATES color_state, const FrameValidRect &band)
{
	if (device == nullptr || cmd == nullptr || color == nullptr)
		return false;
	if (!band.any)
		return true;
	const D3D12_RESOURCE_DESC cd = color->GetDesc();
	if ((cd.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 || cd.SampleDesc.Count != 1)
		return false;
	const LONG w = static_cast<LONG>(cd.Width), h = static_cast<LONG>(cd.Height);
	if (static_cast<LONG>(band.left + band.right) >= w || static_cast<LONG>(band.top + band.bottom) >= h)
		return false;
	if (!ensure_scratch(device, queue, fence, cd) || !blit.ensure(device))
		return false;
	const LONG x0 = static_cast<LONG>(band.left), x1 = w - static_cast<LONG>(band.right);
	const LONG y0 = static_cast<LONG>(band.top), y1 = h - static_cast<LONG>(band.bottom);
	struct Side {
		bool on;
		D3D12_BOX read;
		D3D12_RECT draw;
	};
	const auto cx = [w](LONG v) { return static_cast<UINT>(v < 0 ? 0 : (v > w ? w : v)); };
	const auto cy = [h](LONG v) { return static_cast<UINT>(v < 0 ? 0 : (v > h ? h : v)); };
	const Side sides[4] = {
		{ band.left != 0, { cx(x0 - 1), 0, 0, cx(x0 + 2), static_cast<UINT>(h), 1 }, { 0, 0, x0, h } },
		{ band.right != 0, { cx(x1 - 2), 0, 0, cx(x1 + 1), static_cast<UINT>(h), 1 }, { x1, 0, w, h } },
		{ band.top != 0, { 0, cy(y0 - 1), 0, static_cast<UINT>(w), cy(y0 + 2), 1 }, { 0, 0, w, y0 } },
		{ band.bottom != 0, { 0, cy(y1 - 2), 0, static_cast<UINT>(w), cy(y1 + 1), 1 }, { 0, y1, w, h } },
	};

	barrier12(cmd, color, color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	barrier12(cmd, scratch_, kState, D3D12_RESOURCE_STATE_COPY_DEST);
	D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
	dst.pResource = scratch_;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.pResource = color;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	for (const Side &s : sides)
		if (s.on)
			cmd->CopyTextureRegion(&dst, s.read.left, s.read.top, 0, &src, &s.read);
	barrier12(cmd, scratch_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	bool ok = true;
	for (const Side &s : sides)
		if (s.on && ok)
			ok = blit.draw_fullscreen(device, cmd, scratch_, color, view_format_for(cd.Format), static_cast<uint32_t>(w),
				static_cast<uint32_t>(h), 0.0f, 0u, 0.0f, 0.0f, band.uv, false, &s.draw);
	barrier12(cmd, color, D3D12_RESOURCE_STATE_RENDER_TARGET, color_state);
	barrier12(cmd, scratch_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kState);
	return ok;
}

void FrameShiftD3D12::release() noexcept
{
	if (scratch_ != nullptr) {
		scratch_->Release();
		scratch_ = nullptr;
	}
	format_ = DXGI_FORMAT_UNKNOWN;
	w_ = h_ = 0;
	failed_ = false;
}

}
