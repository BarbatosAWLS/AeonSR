#pragma once

#include "aeon_sr/depth/depth_convention.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"

#include <d3d12.h>

#include <cstdint>

namespace aeon_sr {

struct DebugView {
	int last = 0;
	int logged = 0;
	const char *note = "";

	void apply(unsigned int mode, const FrameInputs &in, const DepthConvention &how);
	void release() noexcept;

private:
	void fail(int stage, const char *why);
	bool ensure_frame_copy(ID3D12Device *device, const D3D12_RESOURCE_DESC &color) noexcept;

	BlitPipelineD3D12 blit_;
	ID3D12Device *device_ = nullptr;
	ID3D12Resource *frame_copy_ = nullptr;
	D3D12_RESOURCE_STATES frame_copy_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
};

}
