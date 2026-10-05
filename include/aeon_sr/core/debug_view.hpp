#pragma once

#include "aeon_sr/depth/depth_normalize.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/core/pass_context.hpp"
#include "aeon_sr/core/settings.hpp"

#include <d3d11.h>

#include <cstdint>

namespace aeon_sr {

struct DebugView {
	int last = 0;
	int logged = 0;
	const char *note = "";

	void apply(const PassContext &ctx, const Settings &s, const FrameInputs &in,
		const DepthConvention &how);
	void release() noexcept;

private:
	bool ensure_copy(ID3D11Device *dev, ID3D11Resource *src, ID3D11Texture2D **out,
		uint32_t *w, uint32_t *h, uint32_t *fmt) noexcept;

	ID3D11Device *device_ = nullptr;
	ID3D11Texture2D *field_copy_ = nullptr;
	ID3D11Texture2D *frame_copy_ = nullptr;
	uint32_t field_w_ = 0, field_h_ = 0, field_fmt_ = 0;
	uint32_t frame_w_ = 0, frame_h_ = 0, frame_fmt_ = 0;
};

}
