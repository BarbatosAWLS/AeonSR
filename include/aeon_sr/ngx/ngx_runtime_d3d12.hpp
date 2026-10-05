#pragma once

#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/interop/gpu12.hpp"
#include "aeon_sr/ngx/ngx_session.hpp"

#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>
#include <string>

namespace aeon_sr {

struct NgxRuntimeD3D12 : NgxSession {

	explicit NgxRuntimeD3D12(const wchar_t *tag = L"D3D12 ") noexcept : NgxSession(tag) {}

	ID3D12Device *device = nullptr;

	static constexpr D3D12_RESOURCE_STATES kNgxSrvState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	ID3D12Resource *output_tex = nullptr;
	ID3D12Resource *color_copy = nullptr;
	ID3D12Resource *depth_scratch = nullptr;

	ID3D12Resource *bias_scratch = nullptr;
	ID3D12Resource *color_full = nullptr;

	DXGI_FORMAT created_backbuffer_format = DXGI_FORMAT_UNKNOWN;

	ID3D12CommandQueue *queue = nullptr;
	Gpu12Fence gpu_fence;

	Gpu12InitList init_list;

	std::wstring game_ngx_modules;

	BlitPipelineD3D12 blit_;

	bool init_device(ID3D12Device *dev, uint32_t w, uint32_t h);
	void shutdown_device();
	void set_command_queue(ID3D12CommandQueue *q);
	void wait_gpu();

	bool run(ID3D12GraphicsCommandList *cmd,
		ID3D12Resource *backbuffer,
		D3D12_RESOURCE_STATES backbuffer_state,
		ID3D12Resource *motion_vectors,
		ID3D12Resource *depth,
		ID3D12Resource *bias_mask);

	bool ensure_feature(ID3D12GraphicsCommandList *cmd, uint32_t w, uint32_t h, DXGI_FORMAT color_format);
	void destroy_feature();
	void release_scratch();

private:

	bool evaluate_inputs(ID3D12GraphicsCommandList *cmd, ID3D12Resource *in_color,
		ID3D12Resource *depth, ID3D12Resource *motion_vectors, ID3D12Resource *bias_mask,
		float jitter_u, float jitter_v);
};

}
