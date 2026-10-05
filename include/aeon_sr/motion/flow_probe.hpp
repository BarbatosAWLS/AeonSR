#pragma once

#include <d3d11.h>

#include <cstdint>

namespace aeon_sr {

struct FlowProbe11 {
	ID3D11Device *device = nullptr;
	ID3D11Texture2D *staging[2] = {};
	int idx = 0;
	bool have_prev = false;

	bool ensure(ID3D11Device *dev);
	void release();

	bool sample(ID3D11DeviceContext *ctx, ID3D11Resource *flow,
		float screen_w, float screen_h, float &out_motion_px);
};

}
