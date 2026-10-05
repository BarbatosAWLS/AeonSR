#pragma once

#include "aeon_sr/interop/blit_d3d11.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/core/imgui_reshade.hpp"

namespace aeon_sr {

struct AddonPipelines {
	BlitPipelineD3D11 d3d11;
	BlitPipelineD3D12 d3d12;

	void release()
	{
		d3d11.release();
		d3d12.release();
	}
};

struct PassContext {
	reshade::api::effect_runtime *runtime = nullptr;
	reshade::api::command_list *cmd_list = nullptr;
	AddonPipelines *pipelines = nullptr;
};

}
