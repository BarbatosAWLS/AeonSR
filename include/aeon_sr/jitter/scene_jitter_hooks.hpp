#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"
#include "aeon_sr/jitter/scene_jitter.hpp"

namespace aeon_sr {

SceneJitter &scene_jitter() noexcept;

void register_scene_jitter_hooks() noexcept;

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
void close(reshade::api::command_list *cmd);
void execute(reshade::api::command_queue *queue, reshade::api::command_list *cmd);
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

}

}
