#pragma once

#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/ngx/ngx_session.hpp"

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <cstdint>

namespace aeon_sr::ngx_dlss {

inline constexpr unsigned long long kAppId = 231313132ull;
inline constexpr const char *kProjectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
inline constexpr const char *kEngineVersion = "1.0.0";

NVSDK_NGX_PerfQuality_Value perf_quality_for(uint32_t quality_mode) noexcept;

NVSDK_NGX_PerfQuality_Value perf_hint_for_ratio(uint32_t render_w, uint32_t render_h,
	uint32_t display_w, uint32_t display_h) noexcept;

bool resolve_render_size(NgxSession &s, uint32_t w, uint32_t h, NVSDK_NGX_PerfQuality_Value *out_perf);

void set_preset_hints(NVSDK_NGX_Parameter *create_params, const UpscalerParams &cfg);

void fill_create_params(const NgxSession &s, uint32_t w, uint32_t h,
	NVSDK_NGX_PerfQuality_Value perf, NVSDK_NGX_DLSS_Create_Params *out) noexcept;

template <class Eval>
inline void fill_dlss_eval_scalars(const NgxSession &s, Eval &e) noexcept
{

	e.Feature.InSharpness = 0.0f;
	e.InJitterOffsetX = s.jitter_x;
	e.InJitterOffsetY = s.jitter_y;
	e.InReset = s.cfg.reset ? 1 : 0;
	e.InMVScaleX = static_cast<float>(s.width);
	e.InMVScaleY = static_cast<float>(s.height);
	e.InPreExposure = 1.0f;
	e.InExposureScale = 1.0f;
	e.InFrameTimeDeltaInMsec = s.cfg.frame_time_ms;

	e.InRenderSubrectDimensions.Width = s.render_width;
	e.InRenderSubrectDimensions.Height = s.render_height;
}

}
