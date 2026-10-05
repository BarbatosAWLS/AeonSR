#include "aeon_sr/motion/mv_stats.hpp"

#include "aeon_sr/core/half_float.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aeon_sr {
namespace {

constexpr uint32_t kGridMax = 64;

struct FormatInfo {
	uint32_t stride = 0;
	bool half = false;
};

FormatInfo format_info(DXGI_FORMAT fmt) noexcept
{
	switch (fmt) {
	case DXGI_FORMAT_R16G16_FLOAT: return { 4, true };
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return { 8, true };
	case DXGI_FORMAT_R32G32_FLOAT: return { 8, false };
	case DXGI_FORMAT_R32G32B32A32_FLOAT: return { 16, false };
	default: return {};
	}
}

}

bool mv_probe_format_supported(DXGI_FORMAT fmt) noexcept
{
	return format_info(fmt).stride != 0;
}

bool mv_stats_from_image(const void *data, uint32_t row_pitch, uint32_t w, uint32_t h,
	DXGI_FORMAT fmt, float screen_w, float screen_h, MvProbeResult &out)
{
	const FormatInfo info = format_info(fmt);
	if (info.stride == 0) {
		out.unsupported = true;
		out.valid = false;
		return false;
	}
	if (data == nullptr || w == 0 || h == 0 || row_pitch < w * info.stride)
		return false;

	const uint32_t step_x = std::max(1u, w / kGridMax);
	const uint32_t step_y = std::max(1u, h / kGridMax);
	const auto *const bytes = static_cast<const uint8_t *>(data);

	double sum = 0.0;
	float max_px = 0.0f;
	uint32_t samples = 0, nonzero = 0;

	for (uint32_t y = 0; y < h; y += step_y) {
		const uint8_t *const row = bytes + static_cast<size_t>(y) * row_pitch;
		for (uint32_t x = 0; x < w; x += step_x) {
			const uint8_t *const px = row + static_cast<size_t>(x) * info.stride;
			float u, v;
			if (info.half) {
				uint16_t raw[2];
				memcpy(raw, px, sizeof(raw));
				u = half_to_float(raw[0]);
				v = half_to_float(raw[1]);
			} else {
				float raw[2];
				memcpy(raw, px, sizeof(raw));
				u = raw[0];
				v = raw[1];
			}

			if (!std::isfinite(u) || !std::isfinite(v)) {
				++samples;
				continue;
			}
			const float dx = u * screen_w;
			const float dy = v * screen_h;
			const float mag = std::sqrt(dx * dx + dy * dy);
			sum += mag;
			max_px = std::max(max_px, mag);
			if (mag > kMvProbeZeroPx)
				++nonzero;
			++samples;
		}
	}

	if (samples == 0)
		return false;

	out.valid = true;
	out.unsupported = false;
	out.samples = samples;
	out.nonzero_pct = 100.0f * static_cast<float>(nonzero) / static_cast<float>(samples);
	out.mean_px = static_cast<float>(sum / samples);
	out.max_px = max_px;
	return true;
}

}
