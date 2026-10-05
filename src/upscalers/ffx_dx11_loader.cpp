#include "aeon_sr/upscalers/ffx_dx11_loader.hpp"

#include <cstring>

namespace aeon_sr {
namespace {

template <typename Fn>
bool resolve_export(HMODULE module, const char *name, Fn *&out, std::wstring &error)
{
	out = reinterpret_cast<Fn *>(GetProcAddress(module, name));
	if (out == nullptr) {
		error = L"GetProcAddress failed for ";
		error += std::wstring(name, name + strlen(name));
		return false;
	}
	return true;
}

}

bool ffx_dx11_load(FfxDx11Api &out, const std::wstring &dll_path, std::wstring &error)
{
	ffx_dx11_unload(out);
	error.clear();

	if (dll_path.empty()) {
		error = L"FFX DX11 DLL path is empty";
		return false;
	}

	HMODULE module = LoadLibraryW(dll_path.c_str());
	if (module == nullptr) {
		const DWORD err = GetLastError();
		wchar_t buf[64]{};
		_snwprintf_s(buf, _TRUNCATE, L"LoadLibraryW failed (0x%08X)", err);
		error = buf;
		return false;
	}

	FfxDx11Api api{};
	api.module = module;

	if (!resolve_export(module, "ffxGetScratchMemorySizeDX11", api.GetScratchMemorySizeDX11, error) ||
		!resolve_export(module, "ffxGetDeviceDX11_Fsr31", api.GetDeviceDX11, error) ||
		!resolve_export(module, "ffxGetInterfaceDX11", api.GetInterfaceDX11, error) ||
		!resolve_export(module, "ffxGetCommandListDX11", api.GetCommandListDX11, error) ||
		!resolve_export(module, "ffxGetResourceDX11_Fsr31", api.GetResourceDX11, error) ||
		!resolve_export(module, "GetFfxResourceDescriptionDX11", api.GetResourceDescriptionDX11, error) ||
		!resolve_export(module, "ffxFsr3UpscalerContextCreate", api.UpscalerContextCreate, error) ||
		!resolve_export(module, "ffxFsr3UpscalerContextDestroy", api.UpscalerContextDestroy, error) ||
		!resolve_export(module, "ffxFsr3UpscalerContextDispatch", api.UpscalerContextDispatch, error) ||
		!resolve_export(module, "ffxFsr3UpscalerGetRenderResolutionFromQualityMode",
			api.GetRenderResolutionFromQualityMode, error)) {
		error += L" — DLL must export classic FSR3 upscaler symbols (build wrapper from "
			L"optiscaler/FidelityFX-SDK-DX11; FFX API 2.x DLLs are incompatible)";
		FreeLibrary(module);
		return false;
	}

	out = api;
	return true;
}

void ffx_dx11_unload(FfxDx11Api &api)
{
	if (api.scratch_buffer != nullptr) {
		free(api.scratch_buffer);
		api.scratch_buffer = nullptr;
		api.scratch_size = 0;
	}
	if (api.module != nullptr) {
		FreeLibrary(api.module);
		api.module = nullptr;
	}
	api.GetScratchMemorySizeDX11 = nullptr;
	api.GetDeviceDX11 = nullptr;
	api.GetInterfaceDX11 = nullptr;
	api.GetCommandListDX11 = nullptr;
	api.GetResourceDX11 = nullptr;
	api.GetResourceDescriptionDX11 = nullptr;
	api.UpscalerContextCreate = nullptr;
	api.UpscalerContextDestroy = nullptr;
	api.UpscalerContextDispatch = nullptr;
	api.GetRenderResolutionFromQualityMode = nullptr;
}

}
