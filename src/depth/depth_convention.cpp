#include "aeon_sr/depth/depth_convention.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace aeon_sr {

namespace {

bool lookup(const DepthDefinitions &defs, const std::string &effect, const char *name, std::string &value)
{
	value.clear();
	bool found = !effect.empty() && defs.own && defs.own(effect, name, value);
	if (!found) {
		value.clear();
		found = defs.shared && defs.shared(name, value);
	}
	if (found && value.empty())
		value = "1";
	return found;
}

bool has_own(const DepthDefinitions &defs, const std::string &effect)
{
	if (!defs.own)
		return false;
	std::string value;
	for (const char *name : kDepthDefinitionNames) {
		value.clear();
		if (defs.own(effect, name, value))
			return true;
	}
	return false;
}

}

float depth_x_offset(const DepthConvention &how, uint32_t buffer_width) noexcept
{
	if (how.x_pixel_offset == 0.0f || buffer_width == 0)
		return how.x_offset;
	return 2.0f * how.x_pixel_offset / static_cast<float>(buffer_width);
}

float depth_y_offset(const DepthConvention &how, uint32_t buffer_height) noexcept
{
	if (how.y_pixel_offset == 0.0f || buffer_height == 0)
		return how.y_offset;
	return 2.0f * how.y_pixel_offset / static_cast<float>(buffer_height);
}

DepthConvention depth_convention_for(const DepthDefinitions &defs, const std::string &effect)
{
	std::string value;
	const auto flag = [&](const char *name, bool fallback) {
		if (!lookup(defs, effect, name, value))
			return fallback;
		return std::strtol(value.c_str(), nullptr, 10) != 0;
	};
	const auto number = [&](const char *name, float fallback) {
		if (!lookup(defs, effect, name, value))
			return fallback;
		const float v = std::strtof(value.c_str(), nullptr);
		return std::isfinite(v) ? v : fallback;
	};

	DepthConvention how;
	how.upside_down = flag("RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN", false);
	how.mirrored = flag("RESHADE_DEPTH_INPUT_IS_MIRRORED", false);
	how.reversed = flag("RESHADE_DEPTH_INPUT_IS_REVERSED", true);
	how.logarithmic = flag("RESHADE_DEPTH_INPUT_IS_LOGARITHMIC", false);
	how.multiplier = number("RESHADE_DEPTH_MULTIPLIER", 1.0f);
	how.far_plane = number("RESHADE_DEPTH_LINEARIZATION_FAR_PLANE", 1000.0f);
	how.x_scale = number("RESHADE_DEPTH_INPUT_X_SCALE", 1.0f);
	how.y_scale = number("RESHADE_DEPTH_INPUT_Y_SCALE", 1.0f);
	how.x_offset = number("RESHADE_DEPTH_INPUT_X_OFFSET", 0.0f);
	how.y_offset = number("RESHADE_DEPTH_INPUT_Y_OFFSET", 0.0f);
	how.x_pixel_offset = number("RESHADE_DEPTH_INPUT_X_PIXEL_OFFSET", 0.0f);
	how.y_pixel_offset = number("RESHADE_DEPTH_INPUT_Y_PIXEL_OFFSET", 0.0f);
	return how;
}

ResolvedDepthConvention resolve_depth_convention(const DepthDefinitions &defs,
	const std::vector<DepthEffect> &effects)
{
	ResolvedDepthConvention out;
	out.how = depth_convention_for(defs, std::string());
	for (const DepthEffect &e : effects) {
		if (!e.active || e.name.empty() ||
			std::find(out.overridden.begin(), out.overridden.end(), e.name) != out.overridden.end())
			continue;
		if (has_own(defs, e.name) && depth_convention_for(defs, e.name) != out.how)
			out.overridden.push_back(e.name);
	}
	return out;
}

std::wstring describe_depth_convention(const ResolvedDepthConvention &resolved)
{
	const DepthConvention &h = resolved.how;
	wchar_t numbers[256];
	std::swprintf(numbers, sizeof(numbers) / sizeof(numbers[0]),
		L"multiplier %g, far plane %g, scale %g x %g, offset %g, %g, pixel offset %g, %g",
		static_cast<double>(h.multiplier), static_cast<double>(h.far_plane),
		static_cast<double>(h.x_scale), static_cast<double>(h.y_scale),
		static_cast<double>(h.x_offset), static_cast<double>(h.y_offset),
		static_cast<double>(h.x_pixel_offset), static_cast<double>(h.y_pixel_offset));

	std::wstring text = L"depth definitions: ";
	text += h.upside_down ? L"upside down" : L"upright";
	text += h.mirrored ? L", mirrored" : L"";
	text += h.reversed ? L", reversed" : L", not reversed";
	text += h.logarithmic ? L", logarithmic, " : L", ";
	text += numbers;
	if (!resolved.overridden.empty()) {
		text += L". These effects carry their own and see a different depth:";
		for (const std::string &name : resolved.overridden)
			text += L" " + std::wstring(name.begin(), name.end());
	}
	return text;
}

}
