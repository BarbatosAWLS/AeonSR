#pragma once
#include <d3d12.h>
#include <dxgi.h>
#include <cstdint>

namespace aeon_sr {

inline constexpr uint32_t kD3D12RingSlots = 64;
inline constexpr uint32_t kSrvPerSlot = 4;

void barrier12(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

void engine_journal_note(const wchar_t *note) noexcept;
void engine_journal_dump() noexcept;
bool create_tex12(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
	bool need_uav, D3D12_RESOURCE_STATES initial, ID3D12Resource **out);

struct NeuralDrawConstants {
	float detail = 1.0f;
	float colour = 1.0f;
	float paper_white = 1.0f;
	float guard = 2.0f;
	float model_w = 0.0f, model_h = 0.0f;
	float frame_w = 0.0f, frame_h = 0.0f;
	float hdr = 0.0f;
	float debug = 0.0f;

	float jitter_u = 0.0f, jitter_v = 0.0f;

	float proxy_is_frame = 0.0f;

	float carry = 0.0f;
	float alpha = 1.0f;
	float sigma_r = 0.10f;
	float scale_comp = 1.0f;
	float mv_u = 1.0f, mv_v = 1.0f;
	float history = 0.0f;
	float smooth_w = 0.2f;
	float carry_reject = 0.5f;
	float warp_x = 0.0f;
	float warp_y = 0.0f;
	float guide_motion = 0.0f;
	float input_detail = 0.0f;
	float pad2 = 0.0f;
	float pad3 = 0.0f;
};

inline constexpr uint32_t kNeuralConstantCount = 28;
inline constexpr uint32_t kBlitConstantCount = 16;

enum class NeuralPass { Proxy, Guide, Delta, Rebuild, Smooth, Composite };

struct BlitPipelineD3D12 {
	ID3D12RootSignature *root_sig = nullptr;
	struct PsoPair {
		DXGI_FORMAT rtv = DXGI_FORMAT_UNKNOWN;
		ID3D12PipelineState *blit = nullptr;
		ID3D12PipelineState *debug = nullptr;

		ID3D12PipelineState *proxy = nullptr;
		ID3D12PipelineState *guide = nullptr;
		ID3D12PipelineState *delta = nullptr;
		ID3D12PipelineState *rebuild = nullptr;
		ID3D12PipelineState *smooth = nullptr;
		ID3D12PipelineState *neural = nullptr;
		ID3D12PipelineState *debug_view = nullptr;
	};
	PsoPair pso_cache[8]{};
	void *vs_blob = nullptr, *ps_blit_blob = nullptr, *ps_debug_blob = nullptr;
	void *ps_proxy_blob = nullptr, *ps_guide_blob = nullptr, *ps_delta_blob = nullptr;
	void *ps_rebuild_blob = nullptr, *ps_smooth_blob = nullptr, *ps_neural_blob = nullptr;
	void *ps_debug_view_blob = nullptr;
	ID3D12DescriptorHeap *srv_heap = nullptr;
	ID3D12DescriptorHeap *rtv_heap = nullptr;
	uint32_t ring_index = 0;
	uint32_t srv_stride = 0, rtv_stride = 0;
	bool blit_ready = false;

	bool ensure(ID3D12Device *device);
	void release();
	bool draw_fullscreen(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
		ID3D12Resource *effect,
		ID3D12Resource *dst, DXGI_FORMAT dst_format, uint32_t dst_w, uint32_t dst_h,
		float sharpness = 0.0f,
		uint32_t debug_mode = 0, float jitter_u = 0.0f, float jitter_v = 0.0f,
		const float *valid_uv = nullptr, bool catmull_rom = false);

	bool draw_debug_view(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *field,
		ID3D12Resource *frame, ID3D12Resource *dst, DXGI_FORMAT dst_format, uint32_t dst_w, uint32_t dst_h,
		uint32_t mode, float cell_px, float depth_far, bool depth_reversed);

	bool clear(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst, DXGI_FORMAT dst_format,
		const float rgba[4]);

	bool draw_neural(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, NeuralPass pass,
		ID3D12Resource *src, ID3D12Resource *orig, ID3D12Resource *proxy, ID3D12Resource *aux,
		ID3D12Resource *dst, DXGI_FORMAT dst_format, uint32_t dst_w, uint32_t dst_h,
		const NeuralDrawConstants &c);

private:

	PsoPair *prepare_draw(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
		ID3D12Resource *t0, ID3D12Resource *t1, ID3D12Resource *t2, ID3D12Resource *t3,
		ID3D12Resource *dst, DXGI_FORMAT dst_format, uint32_t dst_w, uint32_t dst_h);
};

}
