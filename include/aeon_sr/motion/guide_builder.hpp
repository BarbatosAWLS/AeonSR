#pragma once

#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/core/pass_context.hpp"
#include "aeon_sr/core/settings.hpp"

#include <d3d11.h>
#include <d3d12.h>

#include <cstdint>

namespace aeon_sr {

struct GuideBuilder {

	static constexpr D3D12_RESOURCE_STATES kMaskState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	reshade::api::resource bias_mask(const PassContext &ctx, const Settings &s, const FrameInputs &in);

	void release();

private:
	enum class Kind { BiasFromConfidence };

	struct Mask {
		ID3D12Resource *t12 = nullptr;
		uint32_t w = 0, h = 0;
		void release();
	};

	reshade::api::resource draw(const PassContext &ctx, Mask &mask, const FrameInputs &in,
		Kind kind, float strength, bool invert);

	Mask bias_;
};

}
