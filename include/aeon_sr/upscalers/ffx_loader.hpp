#pragma once

#include <Windows.h>

#include <string>

#include <ffx_api.h>

namespace aeon_sr {

struct FfxApi {
	HMODULE module = nullptr;
	PfnFfxCreateContext CreateContext = nullptr;
	PfnFfxDestroyContext DestroyContext = nullptr;
	PfnFfxConfigure Configure = nullptr;
	PfnFfxQuery Query = nullptr;
	PfnFfxDispatch Dispatch = nullptr;
};

bool ffx_load(FfxApi &out, const std::wstring &dll_path, std::wstring &error);
void ffx_unload(FfxApi &api);

}
