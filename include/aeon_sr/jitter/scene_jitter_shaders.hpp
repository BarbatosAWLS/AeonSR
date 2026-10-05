#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"

#include <cstdint>

namespace aeon_sr {

bool jitter_in_shaders(reshade::api::device_api api) noexcept;

void set_jitter_shaders_wanted(bool wanted) noexcept;

struct JitterShaderCounts {
	uint32_t rewritten = 0;
	uint32_t kept = 0;
	uint32_t unwanted = 0;
	uint32_t indexed = 0;
	uint32_t indexed_held = 0;
	uint32_t pretransformed = 0;
};
JitterShaderCounts jitter_shader_counts() noexcept;

enum class JitterShaderDraw {
	Grid,
	Offset,
	NotADraw,
};

JitterShaderDraw jitter_shader_draw(reshade::api::command_list *cmd, bool moved, float tx, float ty, float vw,
	float vh, bool scene_by_size = false) noexcept;

bool jitter_d3d9_bound(reshade::api::command_list *cmd, uint64_t *target, uint64_t *depth) noexcept;

void register_jitter_shader_hooks() noexcept;

namespace jitter_shader_events {

bool create_pipeline(reshade::api::device *device, reshade::api::pipeline_layout layout, uint32_t count,
	const reshade::api::pipeline_subobject *subobjects);
void init_pipeline(reshade::api::device *device, reshade::api::pipeline_layout layout, uint32_t count,
	const reshade::api::pipeline_subobject *subobjects, reshade::api::pipeline pipeline);
void destroy_pipeline(reshade::api::device *device, reshade::api::pipeline pipeline);
void bind_pipeline(reshade::api::command_list *cmd, reshade::api::pipeline_stage stages,
	reshade::api::pipeline pipeline);
void push_constants(reshade::api::command_list *cmd, reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout, uint32_t param, uint32_t first, uint32_t count, const void *values);
void push_descriptors(reshade::api::command_list *cmd, reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout, uint32_t param, const reshade::api::descriptor_table_update &update);
void bind_stream_output(reshade::api::command_list *cmd, uint32_t first, uint32_t count,
	const reshade::api::resource *buffers, const uint64_t *offsets, const uint64_t *max_sizes,
	const reshade::api::resource *counter_buffers, const uint64_t *counter_offsets);
void destroy_command_list(reshade::api::command_list *cmd);
void forget_game_registers() noexcept;

}

}
