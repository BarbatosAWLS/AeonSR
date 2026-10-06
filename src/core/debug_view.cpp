#include "aeon_sr/core/debug_view.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/motion/optical_flow.hpp"

#include <cwchar>

namespace aeon_sr {

namespace {

constexpr float kArrowCellPx = 32.0f;

constexpr D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

D3D12_RESOURCE_STATES readable(D3D12_RESOURCE_STATES s) noexcept
{
	return (s & kRead) != 0 ? s : kRead;
}

}

void DebugView::release() noexcept
{
	if (frame_copy_ != nullptr) {
		frame_copy_->Release();
		frame_copy_ = nullptr;
	}
	frame_copy_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
	blit_.release();
	device_ = nullptr;
}

void DebugView::fail(int stage, const char *why)
{
	last = stage;
	note = why;
	if (last == logged)
		return;
	logged = last;
	wchar_t buf[160]{};
	_snwprintf_s(buf, _TRUNCATE, L"debug view: %hs (stage %d)", why, stage);
	if (stage == -6)
		diag_info("debug-view", buf);
	else
		diag_warn("debug-view", buf);
}

bool DebugView::ensure_frame_copy(ID3D12Device *device, const D3D12_RESOURCE_DESC &color) noexcept
{
	if (frame_copy_ != nullptr) {
		const D3D12_RESOURCE_DESC d = frame_copy_->GetDesc();
		if (d.Width == color.Width && d.Height == color.Height && d.Format == color.Format)
			return true;
		frame_copy_->Release();
		frame_copy_ = nullptr;
	}
	D3D12_RESOURCE_DESC d = color;
	d.MipLevels = 1;
	d.DepthOrArraySize = 1;
	d.SampleDesc = { 1, 0 };
	d.Flags = D3D12_RESOURCE_FLAG_NONE;
	d.Alignment = 0;
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr, IID_PPV_ARGS(&frame_copy_))))
		return false;
	frame_copy_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
	return true;
}

void DebugView::apply(unsigned int mode, const FrameInputs &in, const DepthConvention &how)
{
	if (mode == 0 || mode > kDebugViewCount - 1u) {
		last = 0;
		logged = 0;
		note = "";
		return;
	}
	const EngineFrame &e = in.engine;
	if (!e.ready() || e.device == nullptr) {
		fail(-5, "the frame never reached the add-on's engine");
		return;
	}

	ID3D12Resource *field = nullptr;
	D3D12_RESOURCE_STATES field_state = D3D12_RESOURCE_STATE_COMMON;
	if (mode == 3) {
		field = in.have_motion_confidence ? e.confidence : nullptr;
		field_state = OpticalFlowD3D12::kPublishedState;
	} else if (mode == 4) {
		field = in.have_depth ? e.depth : nullptr;
		field_state = e.depth_state;
	} else {
		field = in.have_motion_vectors ? e.motion : nullptr;
		field_state = e.motion_state;
	}
	if (field == nullptr) {
		fail(-6, mode == 3 ? "no confidence buffer yet"
			: mode == 4 ? "no depth buffer"
			: "no motion vectors yet - the estimator needs two frames");
		return;
	}

	if (device_ != e.device)
		release();
	device_ = e.device;

	const D3D12_RESOURCE_DESC cd = e.color->GetDesc();
	const bool arrows = mode == 2;
	if (arrows && !ensure_frame_copy(e.device, cd)) {
		fail(-9, "could not allocate the frame copy");
		return;
	}

	ID3D12GraphicsCommandList *const cmd = e.cmd;
	if (arrows) {
		barrier12(cmd, e.color, e.color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier12(cmd, frame_copy_, frame_copy_state_, D3D12_RESOURCE_STATE_COPY_DEST);
		cmd->CopyResource(frame_copy_, e.color);
		barrier12(cmd, frame_copy_, D3D12_RESOURCE_STATE_COPY_DEST, kRead);
		frame_copy_state_ = kRead;
		barrier12(cmd, e.color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	} else {
		barrier12(cmd, e.color, e.color_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
	}
	const D3D12_RESOURCE_STATES field_read = readable(field_state);
	barrier12(cmd, field, field_state, field_read);

	const bool reversed = in.depth_sense_known() ? false : how.reversed;
	const bool drawn = blit_.draw_debug_view(e.device, cmd, field, arrows ? frame_copy_ : field, e.color,
		cd.Format, static_cast<uint32_t>(cd.Width), cd.Height, mode, kArrowCellPx, how.far_plane, reversed);

	barrier12(cmd, field, field_read, field_state);
	barrier12(cmd, e.color, D3D12_RESOURCE_STATE_RENDER_TARGET, e.color_state);

	if (!drawn) {
		fail(-7, "the draw was rejected");
		return;
	}
	last = 1;
	logged = 0;
	note = "";
}

}
