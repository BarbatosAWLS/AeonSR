#pragma once

#include <d3d12.h>

#include <nvsdk_ngx_defs.h>

namespace aeon_sr {

NVSDK_NGX_Result ngx12_acquire(ID3D12Device *device, const wchar_t *data_dir,
	const NVSDK_NGX_FeatureCommonInfo *info, const wchar_t *tag);
void ngx12_release(ID3D12Device *device, const wchar_t *tag);

}
