#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace aeon_sr {

struct DepthConvention {
	bool upside_down = false;
	bool mirrored = false;
	bool reversed = true;
	bool logarithmic = false;
	float multiplier = 1.0f;
	float far_plane = 1000.0f;
	float x_scale = 1.0f;
	float y_scale = 1.0f;
	float x_offset = 0.0f;
	float y_offset = 0.0f;
	float x_pixel_offset = 0.0f;
	float y_pixel_offset = 0.0f;

	bool operator==(const DepthConvention &o) const noexcept
	{
		return upside_down == o.upside_down && mirrored == o.mirrored &&
			reversed == o.reversed && logarithmic == o.logarithmic &&
			multiplier == o.multiplier && far_plane == o.far_plane &&
			x_scale == o.x_scale && y_scale == o.y_scale &&
			x_offset == o.x_offset && y_offset == o.y_offset &&
			x_pixel_offset == o.x_pixel_offset && y_pixel_offset == o.y_pixel_offset;
	}
	bool operator!=(const DepthConvention &o) const noexcept { return !(*this == o); }
};

float depth_x_offset(const DepthConvention &how, uint32_t buffer_width) noexcept;
float depth_y_offset(const DepthConvention &how, uint32_t buffer_height) noexcept;

inline constexpr const char *kDepthDefinitionNames[] = {
	"RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN",
	"RESHADE_DEPTH_INPUT_IS_MIRRORED",
	"RESHADE_DEPTH_INPUT_IS_REVERSED",
	"RESHADE_DEPTH_INPUT_IS_LOGARITHMIC",
	"RESHADE_DEPTH_MULTIPLIER",
	"RESHADE_DEPTH_LINEARIZATION_FAR_PLANE",
	"RESHADE_DEPTH_INPUT_X_SCALE",
	"RESHADE_DEPTH_INPUT_Y_SCALE",
	"RESHADE_DEPTH_INPUT_X_OFFSET",
	"RESHADE_DEPTH_INPUT_Y_OFFSET",
	"RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET",
	"RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET",
};

struct DepthDefinitions {
	std::function<bool(const std::string &effect, const char *name, std::string &value)> own;
	std::function<bool(const char *name, std::string &value)> shared;
};

struct DepthEffect {
	std::string name;
	bool active = false;
};

struct ResolvedDepthConvention {
	DepthConvention how;
	std::vector<std::string> overridden;
};

DepthConvention depth_convention_for(const DepthDefinitions &defs, const std::string &effect);

ResolvedDepthConvention resolve_depth_convention(const DepthDefinitions &defs,
	const std::vector<DepthEffect> &effects);

std::wstring describe_depth_convention(const ResolvedDepthConvention &resolved);

}
