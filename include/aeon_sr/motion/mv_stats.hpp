#pragma once

#include <dxgiformat.h>

#include <cstdint>

namespace aeon_sr {

inline constexpr uint64_t kMvProbeInterval = 600;

inline constexpr uint64_t kMvProbeReadDelay = 8;

inline constexpr float kMvProbeZeroPx = 0.01f;

struct MvProbeResult {
	bool valid = false;
	bool unsupported = false;
	float nonzero_pct = 0.0f;
	float mean_px = 0.0f;
	float max_px = 0.0f;
	uint32_t samples = 0;
	uint64_t frame = 0;
};

bool mv_probe_format_supported(DXGI_FORMAT fmt) noexcept;

bool mv_stats_from_image(const void *data, uint32_t row_pitch, uint32_t w, uint32_t h,
	DXGI_FORMAT fmt, float screen_w, float screen_h, MvProbeResult &out);

}
