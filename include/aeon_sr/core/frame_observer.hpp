#pragma once

#include <d3d12.h>

#include <cstdint>

namespace aeon_sr {

struct FrameObservation {
	uint32_t frames = 0;
	double pixels = 0.0, hud = 0.0, held = 0.0;
	double out_change_interface = 0.0, out_change_rest = 0.0;
	double drawn_change_interface = 0.0, drawn_change_rest = 0.0;
	double out_flicker_interface = 0.0, out_flicker_rest = 0.0;
	double motion_rest = 0.0, motion_moving = 0.0, motion_pixels = 0.0;
};

class FrameObserverD3D12 {
public:
	bool begin(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
		D3D12_RESOURCE_STATES state);
	void end(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
		D3D12_RESOURCE_STATES state, ID3D12Resource *mask, D3D12_RESOURCE_STATES mask_state,
		ID3D12Resource *motion, D3D12_RESOURCE_STATES motion_state);
	FrameObservation take() noexcept;
	void release() noexcept;

	static constexpr uint32_t kCounters = 32;

private:
	static constexpr uint32_t kReadback = 4;
	static constexpr uint32_t kSlots = 16;
	static constexpr uint32_t kSlotDescriptors = 7;

	bool ensure(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT format);
	void collect() noexcept;

	ID3D12RootSignature *root_ = nullptr;
	ID3D12PipelineState *observe_ = nullptr, *clear_ = nullptr;
	ID3D12DescriptorHeap *heap_ = nullptr;
	uint32_t stride_ = 0, slot_ = 0;
	ID3D12Resource *drawn_[2]{}, *out_[2]{};
	ID3D12Resource *sums_ = nullptr, *readback_ = nullptr;
	uint32_t *mapped_ = nullptr;
	uint32_t age_[kReadback]{};
	bool written_[kReadback]{};
	uint32_t ring_ = 0;
	uint32_t cur_ = 0;
	uint32_t w_ = 0, h_ = 0;
	DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
	bool have_prev_ = false, begun_ = false, failed_ = false;
	FrameObservation acc_;
};

}
