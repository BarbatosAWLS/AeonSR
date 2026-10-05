#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"
#include "aeon_sr/interop/interop.hpp"

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace aeon_sr {

struct __declspec(uuid("7c6363c7-f94e-437a-9160-141782c44a98")) generic_depth_data
{
	reshade::api::resource selected_depth_stencil = { 0 };
	reshade::api::resource override_depth_stencil = { 0 };
	reshade::api::resource_view selected_shader_resource = { 0 };
	bool using_backup_texture = false;
};

enum class MotionProvider : uint8_t { None = 0, Internal };

enum class DepthProvider : uint8_t { None = 0, Generic, Normalized };

const char *motion_provider_label(MotionProvider p) noexcept;
const char *depth_provider_label(DepthProvider p) noexcept;

struct TextureInfo {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t format = 0;
};

struct EngineFrame {
	ID3D12Device *device = nullptr;
	ID3D12CommandQueue *queue = nullptr;
	ID3D12GraphicsCommandList *cmd = nullptr;

	ID3D12Resource *color = nullptr;
	ID3D12Resource *depth = nullptr;
	ID3D12Resource *motion = nullptr;
	ID3D12Resource *confidence = nullptr;
	ID3D12Resource *global_flow = nullptr;
	ID3D12Resource *scene = nullptr;
	D3D12_RESOURCE_STATES scene_state = D3D12_RESOURCE_STATE_COMMON;

	D3D12_RESOURCE_STATES color_state = D3D12_RESOURCE_STATE_COMMON;
	D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_COMMON;
	D3D12_RESOURCE_STATES motion_state = D3D12_RESOURCE_STATE_COMMON;

	bool native = false;

	bool ready() const noexcept { return cmd != nullptr && color != nullptr; }
};

struct FrameInputs {
	reshade::api::resource color = { 0 };
	reshade::api::resource motion_vectors = { 0 };
	reshade::api::resource depth = { 0 };
	reshade::api::resource_view depth_view = { 0 };
	bool depth_is_backup = false;
	bool depth_view_withheld = false;
	reshade::api::resource global_flow = { 0 };

	reshade::api::resource motion_confidence = { 0 };

	bool have_motion_vectors = false;
	bool have_depth = false;
	bool have_global_flow = false;
	bool have_motion_confidence = false;
	MotionProvider motion_provider = MotionProvider::None;
	DepthProvider depth_provider = DepthProvider::None;
	uint32_t width = 0;
	uint32_t height = 0;
	TextureInfo color_info;
	TextureInfo motion_info;
	TextureInfo depth_info;
	std::wstring depth_note;

	EngineFrame engine;

	bool depth_sense_known() const noexcept { return depth_provider == DepthProvider::Normalized; }
};

bool resolve_frame_inputs(
	reshade::api::effect_runtime *runtime,
	reshade::api::resource_view finished_rtv,
	FrameInputs &out);

reshade::api::resource current_back_buffer_of(reshade::api::swapchain *swapchain);
reshade::api::resource current_back_buffer_of(reshade::api::effect_runtime *runtime);

bool swapchain_surface_of(reshade::api::swapchain *swapchain, reshade::api::resource_desc *out);

reshade::api::resource_usage published_depth_state(bool after_effects, bool is_backup) noexcept;

struct EffectsFrame {
	bool runs = false;
	bool after_effects = false;
};
EffectsFrame effects_frame_of(reshade::addon_event event, bool upscale_effects) noexcept;

bool request_swapchain_usage(reshade::api::device_api api, reshade::api::swapchain_desc &desc) noexcept;

BridgeInputs bridge_inputs_of(const FrameInputs &inputs, bool after_effects) noexcept;

void take_bridge_depth_note(FrameInputs &inputs, const std::wstring &bridge_note);

std::wstring dlss_depth_message(const FrameInputs &inputs);

}
