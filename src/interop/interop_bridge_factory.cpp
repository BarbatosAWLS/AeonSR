#include "aeon_sr/interop/interop.hpp"
#include "aeon_sr/interop/interop_bridges.hpp"
#include "aeon_sr/core/diagnostics.hpp"

namespace aeon_sr {

FrameBridge *make_bridge(reshade::api::device *game, EngineDevice &engine, std::wstring *error,
	bool system_memory_only)
{
	const auto say = [error](const wchar_t *why) {
		if (error != nullptr)
			*error = why;
		return static_cast<FrameBridge *>(nullptr);
	};

	if (game == nullptr)
		return say(L"no device");
	if (!engine.init(game)) {
		if (error != nullptr)
			*error = engine.last_error;
		return nullptr;
	}

	FrameBridge *bridge = nullptr;
	switch (system_memory_only ? static_cast<reshade::api::device_api>(0) : game->get_api()) {
	case reshade::api::device_api::d3d12:
		bridge = make_bridge_d3d12();
		break;
	case reshade::api::device_api::d3d11:
		bridge = make_bridge_d3d11();
		break;
	case reshade::api::device_api::d3d10:
		bridge = make_bridge_d3d10();
		break;
	case reshade::api::device_api::d3d9:
		bridge = make_bridge_d3d9();
		break;
	case reshade::api::device_api::opengl:
		bridge = make_bridge_opengl();
		break;
	case reshade::api::device_api::vulkan:
		bridge = make_bridge_vulkan();
		break;
	}

	if (bridge != nullptr && bridge->init(game, engine))
		return bridge;

	std::wstring first;
	if (bridge != nullptr) {
		first = bridge->last_error;
		retire_bridge(bridge, BridgeRelease::Ordinary);
	}

	bridge = make_bridge_staging();
	if (bridge != nullptr && bridge->init(game, engine)) {
		diag_warn("interop", system_memory_only
			? L"system memory route, as ForceSystemMemory in AeonSR.ini asks"
			: first.empty()
			? L"no shared-texture route on this API; falling back to system memory"
			: L"shared textures unavailable (" + first + L"); falling back to system memory");
		return bridge;
	}
	if (bridge != nullptr) {
		if (error != nullptr)
			*error = first.empty() ? bridge->last_error : first;
		retire_bridge(bridge, BridgeRelease::Ordinary);
	} else if (error != nullptr) {
		*error = first.empty() ? std::wstring(L"no bridge for this graphics API") : first;
	}
	return nullptr;
}

}
