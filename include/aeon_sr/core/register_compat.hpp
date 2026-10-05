#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"

#include <cstdint>

namespace aeon_sr {

#ifndef AEONSR_RESHADE_API_VERSION
#define AEONSR_RESHADE_API_VERSION 18
#endif

inline bool register_addon_with_version(HMODULE addon_module, HMODULE reshade_module, std::uint32_t api_version) noexcept
{
	addon_module = reshade::internal::get_current_module_handle(addon_module);
	reshade_module = reshade::internal::get_reshade_module_handle(reshade_module);

	if (reshade_module == nullptr)
		return false;

	const auto reg = reinterpret_cast<bool (*)(void *, std::uint32_t)>(
		GetProcAddress(reshade_module, "ReShadeRegisterAddon"));
	if (reg == nullptr || !reg(addon_module, api_version))
		return false;

#if defined(IMGUI_VERSION_NUM)
	const auto imgui_func = reinterpret_cast<const imgui_function_table *(*)(std::uint32_t)>(
		GetProcAddress(reshade_module, "ReShadeGetImGuiFunctionTable"));
	if (imgui_func == nullptr || !(imgui_function_table_instance() = imgui_func(IMGUI_VERSION_NUM))) {
		reshade::unregister_addon(addon_module, reshade_module);
		return false;
	}
#endif

	return true;
}

inline bool register_addon_compat(HMODULE addon_module, HMODULE reshade_module) noexcept
{
	constexpr std::uint32_t kCandidates[] = {
		static_cast<std::uint32_t>(AEONSR_RESHADE_API_VERSION),
		20, 19, 18, 17, 16
	};

	for (std::uint32_t v : kCandidates) {
		if (register_addon_with_version(addon_module, reshade_module, v))
			return true;
	}
	return false;
}

}
