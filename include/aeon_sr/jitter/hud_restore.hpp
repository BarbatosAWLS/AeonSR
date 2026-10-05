#pragma once

#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>

namespace aeon_sr {

inline constexpr uint32_t kHudRestoreNeed = 24u;
inline constexpr uint32_t kHudRestoreCap = 40u;
inline constexpr uint32_t kHudRestoreCost = 16u;
inline constexpr float kHudRestoreMinStep = 0.125f;
inline constexpr float kHudRestoreFlat = 3.0f / 255.0f;

struct HudRestoreFrame {
	bool drawn = false;
	float x = 0.0f, y = 0.0f;
	ID3D12Resource *scene = nullptr;
	D3D12_RESOURCE_STATES scene_state = D3D12_RESOURCE_STATE_COMMON;
};

struct HudRestoreTuning {
	uint32_t need = kHudRestoreNeed;
	uint32_t cost = kHudRestoreCost;
	uint32_t cap = kHudRestoreCap;
	float flat = kHudRestoreFlat;
	bool register_ring = false;
};

class HudRestoreD3D12 {
public:
	bool detect(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
		D3D12_RESOURCE_STATES color_state, const HudRestoreFrame &frame);
	bool register_input(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
		D3D12_RESOURCE_STATES color_state);
	bool clear_motion(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *motion,
		D3D12_RESOURCE_STATES state);
	bool composite(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
		D3D12_RESOURCE_STATES state);

	void reset() noexcept;
	void release() noexcept;

	HudRestoreTuning tuning;

	ID3D12Resource *mask() const noexcept { return has_mask_ ? core_ : nullptr; }
	static constexpr D3D12_RESOURCE_STATES kMaskState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

private:
	static constexpr D3D12_RESOURCE_STATES kSrv = kMaskState;
	static constexpr uint32_t kRingSlots = 256u;
	static constexpr uint32_t kSrvCount = 8u;
	static constexpr uint32_t kUavCount = 5u;
	static constexpr uint32_t kSlotDescriptors = kSrvCount + kUavCount;
	static constexpr uint32_t kRtvSlots = 64u;
	static constexpr uint32_t kPsoSlots = 12u;
	static constexpr uint32_t kRetireFrames = 8u;
	static constexpr uint32_t kTargets = 10u;

	enum Kind : uint32_t { Composite = 0, Register = 1, Clear = 2, KindCount = 3 };
	ID3D12PipelineState *trans_pso_ = nullptr;

	struct Retired {
		ID3D12Resource *res = nullptr;
		uint32_t frames = 0;
	};

	struct Pso {
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint32_t kind = 0;
		ID3D12PipelineState *pso = nullptr;
	};

	struct Slot {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
		D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu{}, uav_gpu{};
	};

	bool ensure_pipeline(ID3D12Device *device);
	bool ensure_targets(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT format);
	ID3D12PipelineState *graphics_pso(ID3D12Device *device, Kind kind, DXGI_FORMAT rtv);
	bool draw(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, Kind kind, ID3D12Resource *dst,
		D3D12_RESOURCE_STATES dst_state);
	bool matches(ID3D12Resource *res) const noexcept;
	void retire(ID3D12Resource *&res) noexcept;
	void age_retired() noexcept;
	Slot next_slot() noexcept;
	void write_srv(ID3D12Device *device, const Slot &slot, uint32_t i, ID3D12Resource *res) const;
	void write_uav(ID3D12Device *device, const Slot &slot, uint32_t i, ID3D12Resource *res) const;
	void constants(float *c) const noexcept;

	ID3D12RootSignature *root_ = nullptr;
	ID3D12PipelineState *evidence_ = nullptr;
	ID3D12PipelineState *mask_ = nullptr;
	ID3D12PipelineState *clear_ = nullptr;
	void *vs_ = nullptr;
	void *ps_[KindCount]{};
	Pso psos_[kPsoSlots]{};
	ID3D12DescriptorHeap *srv_heap_ = nullptr;
	ID3D12DescriptorHeap *rtv_heap_ = nullptr;
	uint32_t srv_stride_ = 0, rtv_stride_ = 0;
	uint32_t ring_ = 0, rtv_next_ = 0;
	bool pipeline_failed_ = false;

	ID3D12Resource *frames_[2]{};
	uint32_t cur_ = 0;
	ID3D12Resource *stable_ = nullptr;
	ID3D12Resource *state_[2]{};
	ID3D12Resource *core_ = nullptr;
	ID3D12Resource *scenes_[2]{};
	ID3D12Resource *trans_[2]{};
	uint32_t trans_cur_ = 0;
	bool prev_split_ = false;
	bool trans_history_ = false;
	DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
	uint32_t w_ = 0, h_ = 0;
	bool targets_failed_ = false;
	Retired retired_[20]{};

	bool have_prev_ = false;
	bool prev_drawn_ = false;
	float prev_x_ = 0.0f, prev_y_ = 0.0f;
	bool gain_ = false;
	bool has_mask_ = false;
	HudRestoreFrame frame_;
};

inline constexpr float kHudMaskHeld = 128.0f / 255.0f;

}
