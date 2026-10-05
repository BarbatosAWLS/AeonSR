#include "aeon_sr/core/frame_inputs.hpp"

#include <cstring>

namespace aeon_sr {

const char *motion_provider_label(MotionProvider p) noexcept
{
	switch (p) {
	case MotionProvider::Internal: return "built-in optical flow";
	default: return "none";
	}
}

const char *depth_provider_label(DepthProvider p) noexcept
{
	switch (p) {
	case DepthProvider::Generic: return "game depth, raw";
	case DepthProvider::Normalized: return "game depth, normalised";
	default: return "none";
	}
}

namespace {

template <typename Chain>
reshade::api::resource current_of(Chain *chain)
{
	if (chain == nullptr)
		return {};
	const uint32_t index = chain->get_current_back_buffer_index();
	if (index >= chain->get_back_buffer_count())
		return {};
	return chain->get_back_buffer(index);
}

}

reshade::api::resource current_back_buffer_of(reshade::api::swapchain *swapchain)
{
	return current_of(swapchain);
}

reshade::api::resource current_back_buffer_of(reshade::api::effect_runtime *runtime)
{
	return current_of(runtime);
}

bool swapchain_surface_of(reshade::api::swapchain *swapchain, reshade::api::resource_desc *out)
{
	if (swapchain == nullptr || out == nullptr)
		return false;
	reshade::api::device *const device = swapchain->get_device();
	if (device == nullptr || swapchain->get_back_buffer_count() == 0)
		return false;
	const reshade::api::resource first = swapchain->get_back_buffer(0);
	if (first.handle == 0)
		return false;
	*out = device->get_resource_desc(first);
	return out->texture.width != 0 && out->texture.height != 0;
}

bool resolve_frame_inputs(
	reshade::api::effect_runtime *runtime,
	reshade::api::resource_view finished_rtv,
	FrameInputs &out)
{
	out = FrameInputs{};
	if (runtime == nullptr)
		return false;

	reshade::api::device *const device = runtime->get_device();
	if (device == nullptr)
		return false;
	const reshade::api::device_api api = device->get_api();

	uint32_t w = 0, h = 0;
	runtime->get_screenshot_width_and_height(&w, &h);
	out.width = w;
	out.height = h;

	reshade::api::resource backbuffer = current_back_buffer_of(runtime);
	if (finished_rtv.handle != 0) {
		const reshade::api::resource from_rtv = device->get_resource_from_view(finished_rtv);
		if (from_rtv.handle != 0)
			backbuffer = from_rtv;
	}
	if (backbuffer.handle == 0)
		return false;

	out.color = backbuffer;

	{
		if (const auto *depth_data = runtime->get_private_data<generic_depth_data>()) {
			if (depth_data->selected_depth_stencil.handle == 0 &&
				depth_data->selected_shader_resource.handle != 0) {
				out.depth_view_withheld = true;
				out.depth_note = L"ReShade dropped its depth buffer selection without publishing a new "
					L"one, so the view it still publishes belongs to a buffer it no longer uses. That "
					L"view is not read and this frame has no depth; depth returns when ReShade selects "
					L"a buffer again.";
			} else {
				reshade::api::resource depth_res = { 0 };
				if (depth_data->selected_shader_resource.handle != 0)
					depth_res = device->get_resource_from_view(depth_data->selected_shader_resource);
				else if (api != reshade::api::device_api::d3d12)
					depth_res = depth_data->selected_depth_stencil;

				if (depth_res.handle != 0) {
					out.depth = depth_res;
					out.have_depth = true;
					out.depth_provider = DepthProvider::Generic;
					out.depth_view = depth_data->selected_shader_resource;
					out.depth_is_backup = depth_data->selected_shader_resource.handle != 0 &&
						depth_data->using_backup_texture;
				}
			}
		}
	}

	const auto info_of = [device](reshade::api::resource r) {
		TextureInfo info;
		if (r.handle != 0) {
			const reshade::api::resource_desc d = device->get_resource_desc(r);
			info.width = d.texture.width;
			info.height = d.texture.height;
			info.format = static_cast<uint32_t>(d.texture.format);
		}
		return info;
	};
	out.color_info = info_of(out.color);
	if (out.have_motion_vectors)
		out.motion_info = info_of(out.motion_vectors);
	if (out.have_depth)
		out.depth_info = info_of(out.depth);

	return out.color.handle != 0;
}

bool request_swapchain_usage(reshade::api::device_api api, reshade::api::swapchain_desc &desc) noexcept
{
	if (api != reshade::api::device_api::vulkan)
		return false;
	if (desc.back_buffer.texture.format == reshade::api::format::unknown)
		return false;
	constexpr reshade::api::resource_usage need =
		reshade::api::resource_usage::copy_source | reshade::api::resource_usage::copy_dest;
	if ((desc.back_buffer.usage & need) == need)
		return false;
	desc.back_buffer.usage |= need;
	return true;
}

BridgeInputs bridge_inputs_of(const FrameInputs &inputs, bool after_effects) noexcept
{
	BridgeInputs bin;
	bin.color = inputs.color;
	if (inputs.have_depth) {
		bin.depth = inputs.depth;
		bin.depth_view = inputs.depth_view;
		bin.depth_state = published_depth_state(after_effects, inputs.depth_is_backup);
	}
	return bin;
}

EffectsFrame effects_frame_of(reshade::addon_event event, bool upscale_effects) noexcept
{
	EffectsFrame frame;
	if (event == reshade::addon_event::reshade_begin_effects)
		frame.after_effects = false;
	else if (event == reshade::addon_event::reshade_finish_effects)
		frame.after_effects = true;
	else
		return frame;
	frame.runs = upscale_effects == frame.after_effects;
	return frame;
}

void take_bridge_depth_note(FrameInputs &inputs, const std::wstring &bridge_note)
{
	if (!bridge_note.empty())
		inputs.depth_note = bridge_note;
}

std::wstring dlss_depth_message(const FrameInputs &inputs)
{
	if (!inputs.have_depth && !inputs.depth_view_withheld)
		return L"DLSS is waiting for the game's depth buffer. Menus and loading screens have none, "
			L"and it starts on its own once the game draws a scene. If it never does, pick FSR or "
			L"XeSS, which do not need depth.";
	std::wstring text = L"DLSS needs the game's depth buffer and did not get it this frame";
	text += inputs.depth_note.empty() ? std::wstring(L".") : L": " + inputs.depth_note;
	text += L" FSR and XeSS do not need depth.";
	return text;
}

reshade::api::resource_usage published_depth_state(bool after_effects, bool is_backup) noexcept
{
	if (!after_effects)
		return reshade::api::resource_usage::shader_resource;
	return is_backup
		? reshade::api::resource_usage::copy_dest
		: reshade::api::resource_usage::depth_stencil | reshade::api::resource_usage::shader_resource;
}

}
