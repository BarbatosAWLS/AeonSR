#pragma once

#include "aeon_sr/framegen/frame_interpolator.hpp"
#include "aeon_sr/framegen/vendor_frame_gen.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/motion/optical_flow.hpp"

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace aeon_sr {

struct Settings;

uint64_t frame_gen_settings_key(const Settings &s) noexcept;

enum class FrameGenResult : uint8_t {
	Generated = 0,
	Warming,
	Building,
	Failed,
};

struct FrameGenOptions {
	OpticalFlowD3D12::Quality quality = OpticalFlowD3D12::Quality::Balanced;
	bool camera_model = false;
	float t = 0.5f;

	FrameGenerator generator = FrameGenerator::AeonSR;
	double frame_ms = 16.7;
	ID3D12CommandQueue *queue = nullptr;
	std::function<bool()> flush;
	HWND window = nullptr;
	VendorColor color_space = VendorColor::Sdr;
	const struct OverlayFeed *feed = nullptr;
	uint32_t made = 0;
};

class FrameGenStage {
public:
	~FrameGenStage();

	FrameGenResult record(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
		D3D12_RESOURCE_STATES color_state, ID3D12Resource *dst, D3D12_RESOURCE_STATES dst_state,
		const FrameGenOptions &o, std::wstring *note);

	void reset() noexcept;
	void release() noexcept;
	void release_vendors() noexcept;
	bool has_vendors() const noexcept { return vendor_ != nullptr || !retired_.empty(); }

	bool wait_for_kernels();

	void set_timestamps(ID3D12QueryHeap *heap, uint32_t first) noexcept
	{
		stamps_ = heap;
		stamp_first_ = first;
	}

	FrameGenerator active_generator() const noexcept { return active_; }
	const std::wstring &fallback_note() const noexcept { return fallback_note_; }
	std::wstring vendor_version() const { return vendor_ != nullptr ? vendor_->version() : std::wstring(); }
	bool vendor_usable(FrameGenerator g) const noexcept { return !(vendor_unavailable_ && unavailable_kind_ == g); }
	void forget_refusal() noexcept;
	void settings_changed(uint64_t key) noexcept;
	uint32_t vendor_max_generated() const noexcept { return vendor_ != nullptr ? vendor_->max_generated() : 1u; }
	uint32_t last_made() const noexcept { return last_made_; }
	static DXGI_FORMAT vendor_output_format(ID3D12Device *device, DXGI_FORMAT frame) noexcept;

	OpticalFlowD3D12 &flow() noexcept { return flow_; }
	FrameInterpolatorD3D12 &interpolator() noexcept { return interp_; }
	ID3D12Resource *history() const noexcept { return history_; }
	bool has_history() const noexcept { return have_history_; }

private:
	FrameGenResult record_feed(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
		D3D12_RESOURCE_STATES color_state, const FrameGenOptions &o, std::wstring *note);
	bool ensure_history(ID3D12Device *device, const D3D12_RESOURCE_DESC &d, std::wstring *note);
	enum class VendorTurn : uint8_t { Done, ListLost };
	VendorTurn record_vendor(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
		D3D12_RESOURCE_STATES color_state, ID3D12Resource *const *outputs, uint32_t count,
		const FrameGenOptions &o, FrameGenResult *result, std::wstring *note);
	bool select_generator(const FrameGenOptions &o);
	bool ensure_vendor_textures(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, uint32_t w, uint32_t h,
		DXGI_FORMAT format, std::wstring *note, bool need_out = true);
	void release_vendor_textures(bool deferred) noexcept;
	void retire_vendor() noexcept;
	void age_retired() noexcept;

	enum : int { kFlowIdle = 0, kFlowBuilding, kFlowReady, kFlowFailed };
	std::atomic<int> flow_state_{ kFlowIdle };
	std::thread flow_builder_;
	std::wstring flow_error_;
	ID3D12Device *flow_device_ = nullptr;

	OpticalFlowD3D12 flow_;
	FrameInterpolatorD3D12 interp_;
	BlitPipelineD3D12 blit_;
	ID3D12Resource *history_ = nullptr;
	uint32_t w_ = 0, h_ = 0;
	DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
	bool have_history_ = false;
	ID3D12QueryHeap *stamps_ = nullptr;
	uint32_t stamp_first_ = 0;

	FrameGenerator active_ = FrameGenerator::AeonSR;
	std::wstring fallback_note_;
	std::unique_ptr<VendorFrameGenerator> vendor_;
	bool vendor_unavailable_ = false;
	FrameGenerator unavailable_kind_ = FrameGenerator::AeonSR;
	uint64_t settings_key_ = 0;
	struct Retired {
		std::unique_ptr<VendorFrameGenerator> gen;
		uint32_t frames = 0;
	};
	std::vector<Retired> retired_;
	struct RetiredTexture {
		ID3D12Resource *res = nullptr;
		uint32_t frames = 0;
	};
	std::vector<RetiredTexture> retired_textures_;
	bool vendor_reset_ = true;
	ID3D12Resource *vendor_in_ = nullptr;
	ID3D12Resource *vendor_out_ = nullptr;
	ID3D12Resource *vendor_depth_ = nullptr;
	ID3D12DescriptorHeap *vendor_rtv_ = nullptr;
	ID3D12Device *vendor_tex_device_ = nullptr;
	uint32_t vendor_w_ = 0, vendor_h_ = 0;
	DXGI_FORMAT vendor_format_ = DXGI_FORMAT_UNKNOWN;
	bool vendor_depth_cleared_ = false;
	uint32_t vendor_failures_ = 0;
	VendorColor last_color_ = VendorColor::Sdr;
	uint32_t last_made_ = 0;
	static constexpr uint32_t kVendorFailures = 30;
};

}
