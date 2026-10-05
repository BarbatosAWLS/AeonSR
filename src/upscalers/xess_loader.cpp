#include "aeon_sr/upscalers/xess_loader.hpp"

namespace aeon_sr {

bool xess_load(XessApi &out, const std::wstring &dll_path, std::wstring &error)
{
	xess_unload(out);
	out.module = LoadLibraryW(dll_path.c_str());
	if (out.module == nullptr) {
		error = L"LoadLibrary failed for libxess.dll";
		return false;
	}

	auto resolve = [&](const char *name) -> FARPROC {
		return GetProcAddress(out.module, name);
	};

	out.CreateContext = reinterpret_cast<XessApi::PFN_xessD3D12CreateContext>(resolve("xessD3D12CreateContext"));
	out.BuildPipelines = reinterpret_cast<XessApi::PFN_xessD3D12BuildPipelines>(resolve("xessD3D12BuildPipelines"));
	out.Init = reinterpret_cast<XessApi::PFN_xessD3D12Init>(resolve("xessD3D12Init"));
	out.Execute = reinterpret_cast<XessApi::PFN_xessD3D12Execute>(resolve("xessD3D12Execute"));
	out.DestroyContext = reinterpret_cast<XessApi::PFN_xessDestroyContext>(resolve("xessDestroyContext"));
	out.GetInputResolution = reinterpret_cast<XessApi::PFN_xessGetInputResolution>(resolve("xessGetInputResolution"));
	out.SetVelocityScale = reinterpret_cast<XessApi::PFN_xessSetVelocityScale>(resolve("xessSetVelocityScale"));

	if (!out.CreateContext || !out.Init || !out.Execute || !out.DestroyContext || !out.GetInputResolution) {
		error = L"libxess.dll missing required exports";
		xess_unload(out);
		return false;
	}
	error.clear();
	return true;
}

void xess_unload(XessApi &api)
{
	if (api.module != nullptr) {
		FreeLibrary(api.module);
		api.module = nullptr;
	}
	api = XessApi{};
}

}
