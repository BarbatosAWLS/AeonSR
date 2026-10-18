#pragma once

#include <d3d12.h>

#include <string>
#include <vector>

#include <nvsdk_ngx_defs.h>

#include "aeon_sr/ngx/ngx_api12.hpp"

namespace aeon_sr {

NVSDK_NGX_Result ngx12_acquire(const Ngx12Api &api, ID3D12Device *device, const wchar_t *data_dir,
	const NVSDK_NGX_FeatureCommonInfo *info, const wchar_t *tag);
void ngx12_release(const Ngx12Api &api, ID3D12Device *device, const wchar_t *tag);
std::vector<std::wstring> ngx12_instance_paths(const Ngx12Api &api, ID3D12Device *device);

}
