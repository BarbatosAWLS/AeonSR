#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"
#include "aeon_sr/jitter/scene_jitter.hpp"

namespace aeon_sr {

SceneJitter &scene_jitter() noexcept;

void register_scene_jitter_hooks() noexcept;

void scene_jitter_device_coming(reshade::api::device_api api) noexcept;

JitterFrame take_scene_jitter_frame(reshade::api::effect_runtime *runtime) noexcept;

void discard_scene_jitter_counts(reshade::api::effect_runtime *runtime) noexcept;

class RuntimeJitterCounts final : public JitterCounts {
public:
	explicit RuntimeJitterCounts(reshade::api::effect_runtime *runtime) noexcept : runtime_(runtime) {}
	JitterFrame take() noexcept override { return take_scene_jitter_frame(runtime_); }
	void discard() noexcept override { discard_scene_jitter_counts(runtime_); }

private:
	reshade::api::effect_runtime *runtime_;
};

inline uintptr_t jitter_list_key(const reshade::api::command_list *cmd) noexcept
{
	return reinterpret_cast<uintptr_t>(cmd);
}

namespace jitter_events {

void init(reshade::api::command_list *cmd);
void bind_viewports(reshade::api::command_list *cmd, uint32_t first, uint32_t count,
	const reshade::api::viewport *viewports);
void bind_render_targets(reshade::api::command_list *cmd, uint32_t count,
	const reshade::api::resource_view *rtvs, reshade::api::resource_view dsv);
bool begin_render_pass(reshade::api::command_list *cmd, uint32_t count,
	const reshade::api::render_pass_render_target_desc *rts,
	const reshade::api::render_pass_depth_stencil_desc *ds,
	reshade::api::render_pass_flags flags);
bool end_render_pass(reshade::api::command_list *cmd);
void bind_pipeline(reshade::api::command_list *cmd, reshade::api::pipeline_stage stages,
	reshade::api::pipeline state);
bool draw(reshade::api::command_list *cmd, uint32_t vertices, uint32_t instances,
	uint32_t first_vertex, uint32_t first_instance);
bool draw_indexed(reshade::api::command_list *cmd, uint32_t indices, uint32_t instances,
	uint32_t first_index, int32_t first_vertex, uint32_t first_instance);
bool draw_indirect(reshade::api::command_list *cmd, reshade::api::indirect_command type,
	reshade::api::resource buffer, uint64_t offset, uint32_t draw_count, uint32_t stride);
bool dispatch_mesh(reshade::api::command_list *cmd, uint32_t group_count_x, uint32_t group_count_y,
	uint32_t group_count_z);
void close(reshade::api::command_list *cmd);
void execute(reshade::api::command_queue *queue, reshade::api::command_list *cmd);
void present(reshade::api::command_queue *queue, reshade::api::swapchain *swapchain, const reshade::api::rect *source,
	const reshade::api::rect *dest, uint32_t dirty_rect_count, const reshade::api::rect *dirty_rects);
void execute_secondary(reshade::api::command_list *cmd, reshade::api::command_list *secondary);
void reset(reshade::api::command_list *cmd);
void destroy(reshade::api::command_list *cmd);
void init_resource(reshade::api::device *dev, const reshade::api::resource_desc &desc,
	const reshade::api::subresource_data *initial_data, reshade::api::resource_usage initial_state,
	reshade::api::resource res);
void bind_pipeline_states(reshade::api::command_list *cmd, uint32_t count, const reshade::api::dynamic_state *states,
	const uint32_t *values);
void init_pipeline(reshade::api::device *dev, reshade::api::pipeline_layout layout, uint32_t count,
	const reshade::api::pipeline_subobject *subobjects, reshade::api::pipeline pipe);
void destroy_pipeline(reshade::api::device *dev, reshade::api::pipeline pipe);
bool clear_depth_stencil(reshade::api::command_list *cmd, reshade::api::resource_view dsv, const float *depth,
	const uint8_t *stencil, uint32_t rect_count, const reshade::api::rect *rects);
void push_constants(reshade::api::command_list *cmd, reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout, uint32_t param, uint32_t first, uint32_t count, const void *values);
void push_descriptors(reshade::api::command_list *cmd, reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout, uint32_t param, const reshade::api::descriptor_table_update &update);
void init_pipeline_layout(reshade::api::device *dev, uint32_t count, const reshade::api::pipeline_layout_param *params,
	reshade::api::pipeline_layout layout);
void destroy_pipeline_layout(reshade::api::device *dev, reshade::api::pipeline_layout layout);
void bind_descriptor_tables(reshade::api::command_list *cmd, reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout, uint32_t first, uint32_t count, const reshade::api::descriptor_table *tables,
	uint32_t dynamic_offset_count, const uint32_t *dynamic_offsets);
bool update_descriptor_tables(reshade::api::device *dev, uint32_t count, const reshade::api::descriptor_table_update *updates);
bool copy_descriptor_tables(reshade::api::device *dev, uint32_t count, const reshade::api::descriptor_table_copy *copies);
void destroy_resource(reshade::api::device *dev, reshade::api::resource res);
bool copy_resource(reshade::api::command_list *cmd, reshade::api::resource source, reshade::api::resource dest);
bool copy_texture_region(reshade::api::command_list *cmd, reshade::api::resource source, uint32_t source_subresource,
	const reshade::api::subresource_box *source_box, reshade::api::resource dest, uint32_t dest_subresource,
	const reshade::api::subresource_box *dest_box, reshade::api::filter_mode filter);

}

}
