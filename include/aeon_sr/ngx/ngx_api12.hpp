#pragma once

#include <Windows.h>
#include <d3d12.h>

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

#include <string>

#include "aeon_sr/ngx/ngx_override.hpp"

struct NVSDK_NGX_D3D12_DLSS_Eval_Params;

namespace aeon_sr {

struct Ngx12Api {
	std::wstring path;
	std::wstring error;

	bool external() const noexcept { return !path.empty(); }
	bool usable() const noexcept { return error.empty(); }

	NVSDK_NGX_Result init(unsigned long long app_id, const wchar_t *data_dir, ID3D12Device *device,
		const NVSDK_NGX_FeatureCommonInfo *info) const;
	NVSDK_NGX_Result init_project(const char *project_id, NVSDK_NGX_EngineType engine,
		const char *engine_version, const wchar_t *data_dir, ID3D12Device *device,
		const NVSDK_NGX_FeatureCommonInfo *info) const;
	NVSDK_NGX_Result shutdown(ID3D12Device *device) const;
	NVSDK_NGX_Result capability_parameters(NVSDK_NGX_Parameter **out) const;
	NVSDK_NGX_Result allocate_parameters(NVSDK_NGX_Parameter **out) const;
	NVSDK_NGX_Result destroy_parameters(NVSDK_NGX_Parameter *p) const;
	NVSDK_NGX_Result release_feature(NVSDK_NGX_Handle *handle) const;
	NVSDK_NGX_Result create_dlss(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle **out,
		NVSDK_NGX_Parameter *params, NVSDK_NGX_DLSS_Create_Params *create) const;
	NVSDK_NGX_Result evaluate_dlss(ID3D12GraphicsCommandList *cmd, NVSDK_NGX_Handle *handle,
		NVSDK_NGX_Parameter *params, NVSDK_NGX_D3D12_DLSS_Eval_Params *eval) const;

	HMODULE module = nullptr;
	void *fn_init_ext = nullptr;
	void *fn_init_project = nullptr;
	void *fn_shutdown = nullptr;
	void *fn_shutdown1 = nullptr;
	void *fn_capability = nullptr;
	void *fn_allocate = nullptr;
	void *fn_destroy = nullptr;
	void *fn_create = nullptr;
	void *fn_evaluate = nullptr;
	void *fn_release = nullptr;
};

Ngx12Api ngx12_load_module(const std::wstring &path);

Ngx12Api ngx12_attach_module(const std::wstring &path);

bool ngx12_has_table(HMODULE module);

const Ngx12Api &ngx12_layer_api(const std::wstring &path);

const Ngx12Api &ngx12_static_api();

const Ngx12Api &ngx12_api(const std::wstring &addon_dir);

std::wstring ngx12_feature_dir(const Ngx12Api &api, const std::wstring &fallback);

}
