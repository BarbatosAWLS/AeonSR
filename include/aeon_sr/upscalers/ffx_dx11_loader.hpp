#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>

namespace aeon_sr {

inline constexpr const wchar_t *kFsrDx11DllCandidates[] = {
	L"ffx_fsr3_dx11.dll",
	L"ffx_sdk_dx11.dll",
};

struct FfxDx11Api {
	HMODULE module = nullptr;
	void *scratch_buffer = nullptr;
	size_t scratch_size = 0;

	using PfnGetScratchMemorySizeDX11 = size_t (*)(size_t maxContexts);
	using PfnGetDeviceDX11 = FfxDevice (*)(ID3D11Device *device);
	using PfnGetInterfaceDX11 = FfxErrorCode (*)(FfxInterface *backendInterface, FfxDevice device,
		void *scratchBuffer, size_t scratchBufferSize, uint32_t maxContexts);
	using PfnGetCommandListDX11 = FfxCommandList (*)(ID3D11DeviceContext *deviceContext);
	using PfnGetResourceDX11 = FfxResource (*)(const ID3D11Resource *dx11Resource,
		FfxResourceDescription ffxResDescription, wchar_t const *ffxResName, FfxResourceStates state);
	using PfnGetResourceDescriptionDX11 = FfxResourceDescription (*)(ID3D11Resource *pResource);
	using PfnUpscalerContextCreate = FfxErrorCode (*)(FfxFsr3UpscalerContext *pContext,
		const FfxFsr3UpscalerContextDescription *pContextDescription);
	using PfnUpscalerContextDestroy = FfxErrorCode (*)(FfxFsr3UpscalerContext *pContext);
	using PfnUpscalerContextDispatch = FfxErrorCode (*)(FfxFsr3UpscalerContext *pContext,
		const FfxFsr3UpscalerDispatchDescription *pDispatchDescription);
	using PfnGetRenderResolutionFromQualityMode = FfxErrorCode (*)(FfxFsr3UpscalerQualityMode qualityMode,
		uint32_t displayWidth, uint32_t displayHeight, uint32_t *pRenderWidth, uint32_t *pRenderHeight);

	PfnGetScratchMemorySizeDX11 GetScratchMemorySizeDX11 = nullptr;
	PfnGetDeviceDX11 GetDeviceDX11 = nullptr;
	PfnGetInterfaceDX11 GetInterfaceDX11 = nullptr;
	PfnGetCommandListDX11 GetCommandListDX11 = nullptr;
	PfnGetResourceDX11 GetResourceDX11 = nullptr;
	PfnGetResourceDescriptionDX11 GetResourceDescriptionDX11 = nullptr;
	PfnUpscalerContextCreate UpscalerContextCreate = nullptr;
	PfnUpscalerContextDestroy UpscalerContextDestroy = nullptr;
	PfnUpscalerContextDispatch UpscalerContextDispatch = nullptr;
	PfnGetRenderResolutionFromQualityMode GetRenderResolutionFromQualityMode = nullptr;
};

bool ffx_dx11_load(FfxDx11Api &out, const std::wstring &dll_path, std::wstring &error);
void ffx_dx11_unload(FfxDx11Api &api);

}
