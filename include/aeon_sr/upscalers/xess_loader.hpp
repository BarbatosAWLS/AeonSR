#pragma once

#include <Windows.h>

#include <string>

#include <xess/xess.h>
#include <xess/xess_d3d12.h>

namespace aeon_sr {

struct XessApi {
	HMODULE module = nullptr;

	using PFN_xessD3D12CreateContext = xess_result_t (*)(ID3D12Device *, xess_context_handle_t *);
	using PFN_xessD3D12BuildPipelines = xess_result_t (*)(xess_context_handle_t, ID3D12PipelineLibrary *, bool, uint32_t);
	using PFN_xessD3D12Init = xess_result_t (*)(xess_context_handle_t, const xess_d3d12_init_params_t *);
	using PFN_xessD3D12Execute = xess_result_t (*)(xess_context_handle_t, ID3D12GraphicsCommandList *, const xess_d3d12_execute_params_t *);
	using PFN_xessDestroyContext = xess_result_t (*)(xess_context_handle_t);
	using PFN_xessGetInputResolution = xess_result_t (*)(xess_context_handle_t, const xess_2d_t *, xess_quality_settings_t, xess_2d_t *);
	using PFN_xessSetVelocityScale = xess_result_t (*)(xess_context_handle_t, float, float);

	PFN_xessD3D12CreateContext CreateContext = nullptr;
	PFN_xessD3D12BuildPipelines BuildPipelines = nullptr;
	PFN_xessD3D12Init Init = nullptr;
	PFN_xessD3D12Execute Execute = nullptr;
	PFN_xessDestroyContext DestroyContext = nullptr;
	PFN_xessGetInputResolution GetInputResolution = nullptr;
	PFN_xessSetVelocityScale SetVelocityScale = nullptr;
};

bool xess_load(XessApi &out, const std::wstring &dll_path, std::wstring &error);
void xess_unload(XessApi &api);

}
