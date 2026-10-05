#pragma once

#include "aeon_sr/interop/interop.hpp"

namespace aeon_sr {

FrameBridge *make_bridge_d3d12();
FrameBridge *make_bridge_d3d11();
FrameBridge *make_bridge_d3d10();
FrameBridge *make_bridge_d3d9();
FrameBridge *make_bridge_opengl();
FrameBridge *make_bridge_vulkan();
FrameBridge *make_bridge_staging();

DXGI_FORMAT vulkan_plane_format(reshade::api::format fmt) noexcept;

void vulkan_depth_pass_source_for_tests(const char *fx) noexcept;
uint32_t vulkan_depth_pass_builds_for_tests() noexcept;
using VulkanProcResolver = void *(*)(void *device, const char *name);
void vulkan_proc_resolver_for_tests(VulkanProcResolver resolver) noexcept;

namespace bridge_util {

bool make_nt_pair(EngineDevice &engine, ID3D11Device *device11,
	uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind_flags, SharedPlane &out);

void release_plane(SharedPlane &plane);

bool make_kmt_texture(ID3D11Device *device11, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
	UINT bind_flags, ID3D11Texture2D **out, HANDLE *out_handle);

bool open_kmt_texture(ID3D11Device *device11, HANDLE handle, ID3D11Texture2D **out);

void copy_plane_12(ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst, ID3D12Resource *src);

}

}
