#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"

#include <cstdint>
#include <functional>

namespace aeon_sr {

struct GameScaleDraw {
	uint32_t elements = 0;
	uint32_t instances = 0;
	uint32_t target_w = 0, target_h = 0;
	float vp_x = 0.0f, vp_y = 0.0f, vp_w = 0.0f, vp_h = 0.0f;
	uint32_t source_w = 0, source_h = 0;
	bool source_is_target = false;
	uint32_t source_vp_w = 0, source_vp_h = 0;
	uint32_t corner_depth_draws = 0, most_depth_draws = 0;
};

enum class GameScaleCheck : uint8_t {
	Match,
	NotStretch,
	OtherCorner,
};

GameScaleCheck game_scale_draw_check(const GameScaleDraw &d, uint32_t *render_w, uint32_t *render_h) noexcept;

bool game_scale_draw_matches(const GameScaleDraw &d, uint32_t *render_w, uint32_t *render_h) noexcept;

GameScaleCheck game_scale_full_check(const GameScaleDraw &d) noexcept;

struct GameScaleStretch {
	reshade::api::command_list *cmd = nullptr;
	reshade::api::resource source = { 0 };
	reshade::api::resource target = { 0 };
	reshade::api::resource depth = { 0 };
	reshade::api::resource before = { 0 };
	uint32_t render_w = 0, render_h = 0;
	uint32_t width = 0, height = 0;
};

using GameScaleHandler = std::function<bool(const GameScaleStretch &)>;

void register_game_render_scale(GameScaleHandler handler);
void unregister_game_render_scale();
void set_game_render_scale_enabled(bool on) noexcept;
void set_game_render_scale_full(bool on) noexcept;

struct GameScaleState {
	uint64_t found_frame = 0;
	uint32_t render_w = 0, render_h = 0, width = 0, height = 0;
	uint64_t presents = 0;
	uint64_t replaced = 0;
	double scene_ms = 0.0, frame_ms = 0.0;
	uint64_t missed_corner = 0, missed_none = 0;
	uint32_t miss_w = 0, miss_h = 0, miss_draws = 0, miss_most = 0, miss_most_w = 0, miss_most_h = 0;
};
GameScaleState game_render_scale_state() noexcept;

enum class GameScaleVerdict : uint8_t {
	Off,
	WrongApi,
	Looking,
	Working,
	Idle,
	OtherCorner,
	NotFound,
};

struct GameScaleWatch {
	uint64_t presents = 0;
	uint64_t missed_corner = 0;
};

inline constexpr uint64_t kGameScaleRecentPresents = 90u;
inline constexpr uint64_t kGameScaleVerdictPresents = 1800u;

GameScaleVerdict game_scale_verdict(bool on, bool api_ok, const GameScaleState &g, const GameScaleWatch &since) noexcept;

}
