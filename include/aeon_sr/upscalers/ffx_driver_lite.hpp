#pragma once

#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/upscalers/ffx_loader.hpp"

#include <Windows.h>

#include <cstdint>
#include <string>

struct ID3D12Device;

namespace aeon_sr {

inline constexpr const wchar_t *kFfxDriverDll = L"amdxcffx64.dll";

enum class FfxDriverModel : uint32_t { Fp8, Int8, Int8Navi2, Int8Lite, Count };

struct FfxDriverLite {
	std::string version_name;
	int model_hooks = 0;
};

bool ffx_load_driver_lite(FfxApi &api, const std::wstring &path, FfxDriverLite *info, std::wstring &error);

std::string fsr_version_of_driver(uint16_t major, uint16_t minor, uint16_t build) noexcept;

std::string ffx_driver_lite_entry(const std::wstring &path);

bool is_ffx_driver_path(const std::wstring &path) noexcept;

bool fsr_lite_device_supported(ID3D12Device *device, std::wstring *why);

FfxDriverModel ffx_driver_active_model() noexcept;
const char *ffx_driver_model_name(FfxDriverModel model) noexcept;

bool ffx_driver_model_allowed(FfxDriverModel model) noexcept;

bool ffx_sdk_module_name(const wchar_t *name) noexcept;

void **msvc_rtti_vtable(HMODULE module, const char *mangled) noexcept;

bool patch_module_import(HMODULE module, const char *dll, const char *function, void *hook,
	void **original) noexcept;

bool fsr_render_size_for_quality(uint32_t display_w, uint32_t display_h, uint32_t quality,
	uint32_t *w, uint32_t *h) noexcept;

}
