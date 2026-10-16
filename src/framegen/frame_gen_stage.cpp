#include "aeon_sr/framegen/frame_gen_stage.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/dxgi_format_util.hpp"
#include "aeon_sr/framegen/overlay_presenter.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"

#include <algorithm>

namespace aeon_sr {

namespace {

DXGI_FORMAT vendor_format_for(ID3D12Device *device, DXGI_FORMAT frame) noexcept
{
	const DXGI_FORMAT f = vendor_frame_format(frame);
	D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
	fs.Format = f;
	if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) &&
		(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0)
		return f;
	return f == DXGI_FORMAT_R16G16B16A16_FLOAT ? f : DXGI_FORMAT_R8G8B8A8_UNORM;
}

std::wstring widen_ascii(const char *s)
{
	std::wstring w;
	for (; s != nullptr && *s != '\0'; ++s)
		w += static_cast<wchar_t>(static_cast<unsigned char>(*s));
	return w;
}

}

DXGI_FORMAT FrameGenStage::vendor_output_format(ID3D12Device *device, DXGI_FORMAT frame) noexcept
{
	return vendor_format_for(device, frame);
}

const char *frame_gen_result_label(FrameGenResult r) noexcept
{
	switch (r) {
	case FrameGenResult::Generated: return "generated";
	case FrameGenResult::Warming: return "warming up";
	case FrameGenResult::Building: return "building its kernels";
	default: return "failed";
	}
}

FrameGenStage::~FrameGenStage()
{
	release();
}

void FrameGenStage::release() noexcept
{
	if (flow_builder_.joinable())
		flow_builder_.join();
	flow_state_.store(kFlowIdle, std::memory_order_release);
	flow_device_ = nullptr;
	flow_error_.clear();
	retired_.clear();
	vendor_.reset();
	vendor_unavailable_ = false;
	vendor_reset_ = true;
	release_vendor_textures(false);
	for (RetiredTexture &t : retired_textures_)
		if (t.res != nullptr)
			t.res->Release();
	retired_textures_.clear();
	active_ = FrameGenerator::AeonSR;
	fallback_note_.clear();
	interp_.release();
	flow_.release();
	blit_.release();
	if (history_ != nullptr) {
		history_->Release();
		history_ = nullptr;
	}
	w_ = h_ = 0;
	format_ = DXGI_FORMAT_UNKNOWN;
	have_history_ = false;
}

void FrameGenStage::release_vendors() noexcept
{
	retired_.clear();
	vendor_.reset();
	vendor_unavailable_ = false;
	vendor_reset_ = true;
	active_ = FrameGenerator::AeonSR;
}

void FrameGenStage::reset() noexcept
{
	have_history_ = false;
	vendor_reset_ = true;
	if (vendor_ != nullptr)
		vendor_->reset();
	if (flow_state_.load(std::memory_order_acquire) == kFlowReady)
		flow_.reset_history();
}

bool FrameGenStage::wait_for_kernels()
{
	if (flow_builder_.joinable())
		flow_builder_.join();
	return interp_.wait_for_kernels() && flow_state_.load(std::memory_order_acquire) == kFlowReady &&
		flow_.wait_for_camera_model();
}

bool FrameGenStage::ensure_history(ID3D12Device *device, const D3D12_RESOURCE_DESC &d, std::wstring *note)
{
	const uint32_t w = static_cast<uint32_t>(d.Width), h = d.Height;
	if (history_ != nullptr && w == w_ && h == h_ && format_family(d.Format) == format_family(format_))
		return true;
	if (history_ != nullptr) {
		history_->Release();
		history_ = nullptr;
	}
	have_history_ = false;
	if (!create_tex12(device, w, h, format_family(d.Format), false, D3D12_RESOURCE_STATE_COPY_DEST, &history_)) {
		if (note != nullptr)
			*note = L"frame generation: the copy of the frame before could not be made";
		return false;
	}
	w_ = w;
	h_ = h;
	format_ = d.Format;
	return true;
}

FrameGenResult FrameGenStage::record(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
	D3D12_RESOURCE_STATES color_state, ID3D12Resource *dst, D3D12_RESOURCE_STATES dst_state,
	const FrameGenOptions &o, std::wstring *note)
{
	if (device == nullptr || cmd == nullptr || color == nullptr || dst == nullptr) {
		if (note != nullptr)
			*note = L"frame generation: no frame on the engine device";
		return FrameGenResult::Failed;
	}
	if (o.feed != nullptr)
		return record_feed(device, cmd, color, color_state, o, note);
	const D3D12_RESOURCE_DESC cd = color->GetDesc();
	const uint32_t w = static_cast<uint32_t>(cd.Width), h = cd.Height;
	if (w != w_ || h != h_ || format_family(cd.Format) != format_family(format_) || device != flow_device_ ||
		o.color_space != last_color_) {
		reset();
		if (vendor_unavailable_ && vendor_ == nullptr && o.generator == unavailable_kind_) {
			vendor_unavailable_ = false;
			unavailable_kind_ = FrameGenerator::AeonSR;
			fallback_note_.clear();
		}
		last_color_ = o.color_space;
	}

	age_retired();
	const bool use_vendor = select_generator(o);

	bool building = false;
	std::wstring why;
	const bool interp_ok = !use_vendor && interp_.ensure(device, w, h, cd.Format, &building, &why);
	if (!use_vendor && !interp_ok && !building) {
		if (note != nullptr)
			*note = why;
		return FrameGenResult::Failed;
	}
	if (flow_device_ != device) {
		if (flow_builder_.joinable())
			flow_builder_.join();
		if (flow_device_ != nullptr) {
			flow_.release();
			blit_.release();
			if (history_ != nullptr) {
				history_->Release();
				history_ = nullptr;
			}
			have_history_ = false;
		}
		flow_device_ = device;
		flow_state_.store(kFlowIdle, std::memory_order_release);
	}
	const int fs = flow_state_.load(std::memory_order_acquire);
	if (fs == kFlowIdle) {
		if (flow_builder_.joinable())
			flow_builder_.join();
		flow_state_.store(kFlowBuilding, std::memory_order_release);
		device->AddRef();
		flow_builder_ = std::thread([this, device, w, h] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			std::wstring err;
			const bool ok = flow_.ensure(device, w, h, &err);
			(void)blit_.ensure(device);
			flow_error_ = err;
			flow_state_.store(ok ? kFlowReady : kFlowFailed, std::memory_order_release);
			device->Release();
		});
		have_history_ = false;
		return FrameGenResult::Building;
	}
	if (fs == kFlowBuilding) {
		have_history_ = false;
		return FrameGenResult::Building;
	}
	if (flow_builder_.joinable())
		flow_builder_.join();
	if (fs == kFlowFailed || !flow_.ensure(device, w, h, &why)) {
		if (note != nullptr)
			*note = L"frame generation: the estimator " + (fs == kFlowFailed ? flow_error_ : why);
		return FrameGenResult::Failed;
	}
	if (!ensure_history(device, cd, note))
		return FrameGenResult::Failed;

	if (!flow_.record(cmd, color, color_state, nullptr, D3D12_RESOURCE_STATE_COMMON, flow_.usable(o.quality), true,
			&why, o.camera_model)) {
		if (note != nullptr)
			*note = L"frame generation: the estimator " + why;
		reset();
		return FrameGenResult::Failed;
	}

	const auto stamp = [&](uint32_t k) {
		if (stamps_ != nullptr)
			cmd->EndQuery(stamps_, D3D12_QUERY_TYPE_TIMESTAMP, stamp_first_ + k);
	};
	stamp(0);
	FrameGenResult result = FrameGenResult::Warming;
	if (building)
		result = FrameGenResult::Building;
	ID3D12Resource *out = nullptr;
	D3D12_RESOURCE_STATES out_state = D3D12_RESOURCE_STATE_COMMON;
	const D3D12_RESOURCE_DESC dd = dst->GetDesc();
	bool out_copies = false;
	if (use_vendor) {
		if (!ensure_vendor_textures(device, cmd, w, h, vendor_format_for(device, cd.Format), note)) {
			result = FrameGenResult::Failed;
		} else {
			ID3D12Resource *const one[1] = { vendor_out_ };
			if (record_vendor(device, cmd, color, color_state, one, 1, o, &result, note) == VendorTurn::ListLost) {
				reset();
				return FrameGenResult::Failed;
			}
		}
		if (result == FrameGenResult::Generated) {
			out = vendor_out_;
			out_state = VendorFrameGenerator::kOutputState;
			out_copies = format_family(vendor_format_) == format_family(dd.Format);
		}
	} else if (interp_ok && have_history_ && flow_.has_history()) {
		FrameInterpolatorD3D12::Inputs in;
		in.previous = history_;
		in.previous_state = D3D12_RESOURCE_STATE_COPY_DEST;
		in.current = color;
		in.current_state = color_state;
		in.motion = flow_.motion();
		in.motion_state = OpticalFlowD3D12::kPublishedState;
		in.global_flow = flow_.global_flow();
		in.global_state = OpticalFlowD3D12::kPublishedState;
		in.t = o.t;
		if (interp_.record(cmd, in, &why)) {
			result = FrameGenResult::Generated;
			out = interp_.output();
			out_state = FrameInterpolatorD3D12::kOutputState;
			out_copies = interp_.copies_into(dd.Format);
		} else {
			if (note != nullptr)
				*note = why;
			result = FrameGenResult::Failed;
		}
	}

	stamp(1);
	barrier12(cmd, color, color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmd->CopyResource(history_, color);
	barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, color_state);
	have_history_ = true;

	if (result == FrameGenResult::Generated && out != nullptr) {
		if (out_copies) {
			barrier12(cmd, out, out_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
			barrier12(cmd, dst, dst_state, D3D12_RESOURCE_STATE_COPY_DEST);
			cmd->CopyResource(dst, out);
			barrier12(cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST, dst_state);
			barrier12(cmd, out, D3D12_RESOURCE_STATE_COPY_SOURCE, out_state);
		} else {
			barrier12(cmd, out, out_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			barrier12(cmd, dst, dst_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
			const bool drawn = blit_.draw_fullscreen(device, cmd, out, dst, interpolator_read_format(dd.Format),
				static_cast<uint32_t>(dd.Width), dd.Height);
			barrier12(cmd, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, dst_state);
			barrier12(cmd, out, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, out_state);
			if (!drawn) {
				if (note != nullptr)
					*note = L"frame generation: the generated frame could not be drawn into this frame's format";
				result = FrameGenResult::Failed;
			}
		}
	}
	stamp(2);
	return result;
}

FrameGenResult FrameGenStage::record_feed(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
	D3D12_RESOURCE_STATES color_state, const FrameGenOptions &o, std::wstring *note)
{
	const OverlayFeed &feed = *o.feed;
	const D3D12_RESOURCE_DESC cd = color->GetDesc();
	const uint32_t w = static_cast<uint32_t>(cd.Width), h = cd.Height;
	const D3D12_RESOURCE_DESC fd = feed.color->GetDesc();
	if (fd.Width != cd.Width || fd.Height != cd.Height || format_family(fd.Format) != format_family(cd.Format)) {
		if (note != nullptr)
			*note = L"frame generation: the frame does not fit the ring of its own window";
		return FrameGenResult::Failed;
	}
	if (w != w_ || h != h_ || format_family(cd.Format) != format_family(format_) || device != flow_device_) {
		reset();
		w_ = w;
		h_ = h;
		format_ = cd.Format;
	}
	if (flow_device_ != device) {
		if (flow_builder_.joinable())
			flow_builder_.join();
		if (flow_device_ != nullptr)
			flow_.release();
		flow_device_ = device;
		flow_state_.store(kFlowIdle, std::memory_order_release);
	}
	const int fs = flow_state_.load(std::memory_order_acquire);
	if (fs == kFlowIdle) {
		if (flow_builder_.joinable())
			flow_builder_.join();
		flow_state_.store(kFlowBuilding, std::memory_order_release);
		device->AddRef();
		flow_builder_ = std::thread([this, device, w, h] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			std::wstring err;
			const bool ok = flow_.ensure(device, w, h, &err);
			flow_error_ = err;
			flow_state_.store(ok ? kFlowReady : kFlowFailed, std::memory_order_release);
			device->Release();
		});
		return FrameGenResult::Building;
	}
	if (fs == kFlowBuilding)
		return FrameGenResult::Building;
	if (flow_builder_.joinable())
		flow_builder_.join();
	std::wstring why;
	if (fs == kFlowFailed || !flow_.ensure(device, w, h, &why)) {
		if (note != nullptr)
			*note = L"frame generation: the estimator " + (fs == kFlowFailed ? flow_error_ : why);
		return FrameGenResult::Failed;
	}
	if (!flow_.record(cmd, color, color_state, nullptr, D3D12_RESOURCE_STATE_COMMON, flow_.usable(o.quality), true,
			&why, o.camera_model)) {
		if (note != nullptr)
			*note = L"frame generation: the estimator " + why;
		reset();
		return FrameGenResult::Failed;
	}
	const auto copy = [&](ID3D12Resource *dst, ID3D12Resource *src, D3D12_RESOURCE_STATES src_state) {
		barrier12(cmd, src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier12(cmd, dst, OverlayFeed::kRestState, D3D12_RESOURCE_STATE_COPY_DEST);
		cmd->CopyResource(dst, src);
		barrier12(cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST, OverlayFeed::kRestState);
		barrier12(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);
	};
	copy(feed.color, color, color_state);
	copy(feed.motion, flow_.motion(), OpticalFlowD3D12::kPublishedState);
	copy(feed.global, flow_.global_flow(), OpticalFlowD3D12::kPublishedState);

	last_made_ = 0;
	age_retired();
	const bool use_vendor = select_generator(o);
	const uint32_t want = std::min(o.made, feed.made_count);
	const DXGI_FORMAT vf = use_vendor && want > 0 ? vendor_format_for(device, cd.Format) : DXGI_FORMAT_UNKNOWN;
	if (use_vendor && want > 0 && flow_.has_history() &&
		(vf != feed.made_format || !ensure_vendor_textures(device, cmd, w, h, vf, nullptr, false))) {
		vendor_reset_ = true;
		if (++vendor_failures_ >= kVendorFailures) {
			vendor_failures_ = 0;
			unavailable_kind_ = o.generator;
			vendor_unavailable_ = true;
			fallback_note_ = widen_ascii(frame_generator_label(o.generator)) +
				L" frame generation: its textures could not be made (out of video memory?); AeonSR's generator is used";
			diag_logf(DiagLevel::Warn, "framegen", L"%ls", fallback_note_.c_str());
			retire_vendor();
			active_ = FrameGenerator::AeonSR;
		}
	} else if (use_vendor && want > 0 && flow_.has_history()) {
		const uint32_t count = std::min(want, vendor_->max_generated());
		for (uint32_t i = 0; i < count; ++i)
			barrier12(cmd, feed.made[i], OverlayFeed::kRestState, VendorFrameGenerator::kOutputState);
		FrameGenResult vr = FrameGenResult::Warming;
		const VendorTurn turn = record_vendor(device, cmd, color, color_state, feed.made, count, o, &vr, note);
		if (turn == VendorTurn::ListLost) {
			reset();
			return FrameGenResult::Failed;
		}
		for (uint32_t i = 0; i < count; ++i)
			barrier12(cmd, feed.made[i], VendorFrameGenerator::kOutputState, OverlayFeed::kRestState);
		if (vr == FrameGenResult::Generated)
			last_made_ = count;
	} else if (use_vendor) {
		vendor_reset_ = true;
	}
	return flow_.has_history() ? FrameGenResult::Generated : FrameGenResult::Warming;
}

bool FrameGenStage::select_generator(const FrameGenOptions &o)
{
	const bool want_vendor = o.generator != FrameGenerator::AeonSR;
	if (want_vendor && (vendor_ == nullptr || vendor_->kind() != o.generator) &&
		!(vendor_unavailable_ && unavailable_kind_ == o.generator)) {
		retire_vendor();
		vendor_unavailable_ = false;
		fallback_note_.clear();
		vendor_ = make_vendor_frame_generator(o.generator);
		vendor_reset_ = true;
		if (vendor_ == nullptr) {
			vendor_unavailable_ = true;
			unavailable_kind_ = o.generator;
			fallback_note_ = widen_ascii(frame_generator_label(o.generator)) +
				L" frame generation runs in 64-bit games only; AeonSR's generator is used";
		}
	} else if (!want_vendor) {
		retire_vendor();
		vendor_unavailable_ = false;
		fallback_note_.clear();
	}
	if (want_vendor && !vendor_unavailable_ && o.generator == FrameGenerator::Xess && (o.queue == nullptr || !o.flush)) {
		retire_vendor();
		vendor_unavailable_ = true;
		unavailable_kind_ = o.generator;
		fallback_note_ = L"Intel XeSS frame generation cannot run on this route; AeonSR's generator is used";
	}
	const bool use_vendor = want_vendor && vendor_ != nullptr && !vendor_unavailable_;
	active_ = use_vendor ? o.generator : FrameGenerator::AeonSR;
	return use_vendor;
}

void FrameGenStage::retire_vendor() noexcept
{
	if (vendor_ == nullptr)
		return;
	retired_.push_back(Retired{ std::move(vendor_), 0 });
	vendor_reset_ = true;
}

void FrameGenStage::age_retired() noexcept
{
	for (Retired &r : retired_)
		++r.frames;
	retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
		[](const Retired &r) { return r.frames > 4 && !r.gen->busy(); }), retired_.end());
	for (RetiredTexture &t : retired_textures_)
		if (++t.frames > 4 && t.res != nullptr) {
			t.res->Release();
			t.res = nullptr;
		}
	retired_textures_.erase(std::remove_if(retired_textures_.begin(), retired_textures_.end(),
		[](const RetiredTexture &t) { return t.res == nullptr; }), retired_textures_.end());
}

void FrameGenStage::release_vendor_textures(bool deferred) noexcept
{
	for (ID3D12Resource **r : { &vendor_in_, &vendor_out_, &vendor_depth_ }) {
		if (*r != nullptr) {
			if (deferred)
				retired_textures_.push_back(RetiredTexture{ *r, 0 });
			else
				(*r)->Release();
		}
		*r = nullptr;
	}
	if (vendor_rtv_ != nullptr)
		vendor_rtv_->Release();
	vendor_rtv_ = nullptr;
	vendor_tex_device_ = nullptr;
	vendor_w_ = vendor_h_ = 0;
	vendor_format_ = DXGI_FORMAT_UNKNOWN;
	vendor_depth_cleared_ = false;
}

bool FrameGenStage::ensure_vendor_textures(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, uint32_t w,
	uint32_t h, DXGI_FORMAT format, std::wstring *note, bool need_out)
{
	if (vendor_tex_device_ != device || vendor_w_ != w || vendor_h_ != h || vendor_format_ != format ||
		(need_out && vendor_out_ == nullptr)) {
		release_vendor_textures(true);
		const bool ok = create_tex12(device, w, h, format, false, VendorFrameGenerator::kInputState, &vendor_in_) &&
			(!need_out || create_tex12(device, w, h, format, true, VendorFrameGenerator::kOutputState, &vendor_out_)) &&
			create_tex12(device, w, h, DXGI_FORMAT_R32_FLOAT, false, D3D12_RESOURCE_STATE_RENDER_TARGET, &vendor_depth_);
		D3D12_DESCRIPTOR_HEAP_DESC hd{};
		hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		hd.NumDescriptors = 1;
		if (!ok || FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&vendor_rtv_)))) {
			release_vendor_textures(false);
			if (note != nullptr)
				*note = L"frame generation: the vendor generator's textures could not be made";
			return false;
		}
		vendor_tex_device_ = device;
		vendor_w_ = w;
		vendor_h_ = h;
		vendor_format_ = format;
		vendor_reset_ = true;
	}
	if (!vendor_depth_cleared_) {
		const D3D12_CPU_DESCRIPTOR_HANDLE rtv = vendor_rtv_->GetCPUDescriptorHandleForHeapStart();
		device->CreateRenderTargetView(vendor_depth_, nullptr, rtv);
		const float far_plane[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		cmd->ClearRenderTargetView(rtv, far_plane, 0, nullptr);
		barrier12(cmd, vendor_depth_, D3D12_RESOURCE_STATE_RENDER_TARGET, VendorFrameGenerator::kInputState);
		vendor_depth_cleared_ = true;
	}
	return true;
}

FrameGenStage::VendorTurn FrameGenStage::record_vendor(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *color, D3D12_RESOURCE_STATES color_state, ID3D12Resource *const *outputs, uint32_t count,
	const FrameGenOptions &o, FrameGenResult *result, std::wstring *note)
{
	const D3D12_RESOURCE_DESC cd = color->GetDesc();
	const uint32_t w = static_cast<uint32_t>(cd.Width), h = cd.Height;
	const DXGI_FORMAT vf = vendor_format_;

	const D3D12_RESOURCE_STATES in_state = VendorFrameGenerator::kInputState;
	if (format_family(cd.Format) == format_family(vf)) {
		barrier12(cmd, color, color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier12(cmd, vendor_in_, in_state, D3D12_RESOURCE_STATE_COPY_DEST);
		cmd->CopyResource(vendor_in_, color);
		barrier12(cmd, vendor_in_, D3D12_RESOURCE_STATE_COPY_DEST, in_state);
		barrier12(cmd, color, D3D12_RESOURCE_STATE_COPY_SOURCE, color_state);
	} else {
		barrier12(cmd, color, color_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		barrier12(cmd, vendor_in_, in_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		const bool drawn = blit_.draw_fullscreen(device, cmd, color, vendor_in_, vf, w, h);
		barrier12(cmd, vendor_in_, D3D12_RESOURCE_STATE_RENDER_TARGET, in_state);
		barrier12(cmd, color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, color_state);
		if (!drawn) {
			if (note != nullptr)
				*note = L"frame generation: the frame could not be drawn into the vendor generator's format";
			*result = FrameGenResult::Failed;
			return VendorTurn::Done;
		}
	}

	VendorFrame f;
	f.device = device;
	f.queue = o.queue;
	f.cmd = cmd;
	f.flush = o.flush;
	f.color = vendor_in_;
	f.motion = flow_.motion();
	f.motion_state = OpticalFlowD3D12::kPublishedState;
	f.depth = vendor_depth_;
	f.output = outputs[0];
	f.generated = std::clamp(count, 1u, kVendorMaxGenerated);
	for (uint32_t i = 1; i < f.generated; ++i)
		f.more_outputs[i - 1] = outputs[i];
	f.width = w;
	f.height = h;
	f.format = vf;
	f.color_space = o.color_space == VendorColor::Scrgb && vf != DXGI_FORMAT_R16G16B16A16_FLOAT ? VendorColor::Sdr : o.color_space;
	f.reset = vendor_reset_ || !flow_.has_history();
	f.frame_ms = o.frame_ms;
	f.window = o.window;
	std::wstring why;
	const VendorStep step = vendor_->record(f, &why);
	vendor_reset_ = false;
	if (step != VendorStep::Failed && step != VendorStep::ListLost)
		vendor_failures_ = 0;
	switch (step) {
	case VendorStep::Generated:
		*result = FrameGenResult::Generated;
		break;
	case VendorStep::Warming:
		*result = FrameGenResult::Warming;
		break;
	case VendorStep::Building:
		*result = FrameGenResult::Building;
		vendor_reset_ = true;
		break;
	case VendorStep::Unavailable:
		unavailable_kind_ = o.generator;
		vendor_unavailable_ = true;
		fallback_note_ = why + L"; AeonSR's generator is used";
		diag_logf(DiagLevel::Warn, "framegen", L"%ls", fallback_note_.c_str());
		retire_vendor();
		*result = FrameGenResult::Warming;
		break;
	default:
		vendor_reset_ = true;
		*result = FrameGenResult::Failed;
		if (++vendor_failures_ >= kVendorFailures) {
			vendor_failures_ = 0;
			unavailable_kind_ = o.generator;
			vendor_unavailable_ = true;
			fallback_note_ = why + L" (every frame)" + L"; AeonSR's generator is used";
			diag_logf(DiagLevel::Warn, "framegen", L"%ls", fallback_note_.c_str());
			retire_vendor();
			*result = FrameGenResult::Warming;
		} else if (note != nullptr) {
			*note = why;
		}
		if (step == VendorStep::ListLost) {
			if (note != nullptr)
				*note = why;
			return VendorTurn::ListLost;
		}
		break;
	}
	return VendorTurn::Done;
}

}
