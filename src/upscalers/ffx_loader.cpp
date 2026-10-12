#include "aeon_sr/upscalers/ffx_loader.hpp"
#include "aeon_sr/upscalers/runtime_load_check.hpp"

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

bool ffx_load(FfxApi &out, const std::wstring &dll_path, std::wstring &error)
{
	ffx_unload(out);
	error.clear();

	if (dll_path.empty()) {
		error = L"FFX DLL path is empty";
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
	std::wstring got;
	if (!loaded_module_is(module, dll_path, &got)) {
		error = runtime_load_redirect_error(dll_path, got);
		return false;
	}

	FfxApi api{};
	api.module = module;

	if (!resolve_export(module, "ffxCreateContext", api.CreateContext, error) ||
		!resolve_export(module, "ffxDestroyContext", api.DestroyContext, error) ||
		!resolve_export(module, "ffxConfigure", api.Configure, error) ||
		!resolve_export(module, "ffxQuery", api.Query, error) ||
		!resolve_export(module, "ffxDispatch", api.Dispatch, error)) {
		FreeLibrary(module);
		return false;
	}
	for (const char *name : { "ffxCreateContext", "ffxDispatch" }) {
		if (export_patched(module, name)) {
			error = runtime_hooked_error(dll_path, name);
			FreeLibrary(module);
			return false;
		}
	}

	out = api;
	return true;
}

void ffx_unload(FfxApi &api)
{
	if (api.module != nullptr) {
		FreeLibrary(api.module);
		api.module = nullptr;
	}
	api.CreateContext = nullptr;
	api.DestroyContext = nullptr;
	api.Configure = nullptr;
	api.Query = nullptr;
	api.Dispatch = nullptr;
}

}
