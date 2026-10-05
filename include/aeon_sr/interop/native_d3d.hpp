#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"

#include <d3d11.h>
#include <d3d12.h>

#include <cstdint>

namespace aeon_sr {

template <class T>
inline T *native_res(reshade::api::resource r) noexcept
{
	return reinterpret_cast<T *>(static_cast<uintptr_t>(r.handle));
}

inline ID3D11Device *native_d3d11_device(reshade::api::device *device) noexcept
{
	if (device == nullptr || device->get_api() != reshade::api::device_api::d3d11)
		return nullptr;
	return reinterpret_cast<ID3D11Device *>(device->get_native());
}

inline ID3D11DeviceContext *native_d3d11_context(reshade::api::effect_runtime *runtime) noexcept
{
	reshade::api::command_queue *const q = runtime != nullptr ? runtime->get_command_queue() : nullptr;
	return q != nullptr ? reinterpret_cast<ID3D11DeviceContext *>(q->get_native()) : nullptr;
}

inline ID3D12Device *native_d3d12_device(reshade::api::device *device) noexcept
{
	if (device == nullptr || device->get_api() != reshade::api::device_api::d3d12)
		return nullptr;
	return reinterpret_cast<ID3D12Device *>(device->get_native());
}

inline ID3D12CommandQueue *native_d3d12_queue(reshade::api::effect_runtime *runtime) noexcept
{
	reshade::api::command_queue *const q = runtime != nullptr ? runtime->get_command_queue() : nullptr;
	return q != nullptr ? reinterpret_cast<ID3D12CommandQueue *>(q->get_native()) : nullptr;
}

inline ID3D12GraphicsCommandList *native_d3d12_list(reshade::api::command_list *cmd_list) noexcept
{
	return cmd_list != nullptr ? reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native()) : nullptr;
}

}
