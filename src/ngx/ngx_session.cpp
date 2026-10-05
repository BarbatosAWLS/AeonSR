#include "aeon_sr/ngx/ngx_session_ngx.hpp"
#include "aeon_sr/core/runtime_search.hpp"

#include <Windows.h>

#include <cmath>
#include <cstdio>

namespace aeon_sr {

bool NgxSession::ensure_dll_present()
{
	dll_present = false;
	for (const std::wstring &dir : runtime_search_dirs(addon_dir, exe_directory_w())) {
		if (!file_exists_w(ngx_dll_path(dir)))
			continue;
		dll_present = true;
		dll_dir = dir;
		break;
	}
	if (!dll_present) {
		status = UpscalerStatus::MissingRuntime;
		last_error = L"Place nvngx_dlss.dll next to AeonSR.addon64, or in a runtime\\ folder beside it";
	}
	return dll_present;
}

std::wstring NgxSession::prepare_data_dir()
{

	const std::wstring appdata = local_appdata_dir_w();
	data_dir = !appdata.empty() ? appdata : join_path(addon_dir, L"AeonSR_data");
	CreateDirectoryW(data_dir.c_str(), nullptr);
	return data_dir;
}

void NgxSession::reset_after_shutdown() noexcept
{
	width = height = render_width = render_height = 0;
	if (status != UpscalerStatus::MissingRuntime)
		status = UpscalerStatus::Idle;
}

namespace ngx_dlss {
namespace {

bool query_optimal(NVSDK_NGX_Parameter *p, uint32_t w, uint32_t h,
	NVSDK_NGX_PerfQuality_Value perf, unsigned int *out_w, unsigned int *out_h)
{
	unsigned int max_w = 0, max_h = 0, min_w = 0, min_h = 0;
	float sharpness = 0.0f;
	*out_w = 0;
	*out_h = 0;
	const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(
		p, w, h, perf, out_w, out_h, &max_w, &max_h, &min_w, &min_h, &sharpness);
	return !NVSDK_NGX_FAILED(r) && *out_w > 0 && *out_h > 0;
}

bool nearly_native_res(unsigned int rw, unsigned int rh, uint32_t w, uint32_t h) noexcept
{

	return rw * 25 >= w * 23 && rh * 25 >= h * 23;
}

bool resolve_chain(NVSDK_NGX_Parameter *p, uint32_t w, uint32_t h,
	NVSDK_NGX_PerfQuality_Value requested, NVSDK_NGX_PerfQuality_Value *out_perf,
	unsigned int *render_w, unsigned int *render_h)
{
	*out_perf = requested;
	*render_w = w;
	*render_h = h;
	if (requested == NVSDK_NGX_PerfQuality_Value_DLAA)
		return true;

	const NVSDK_NGX_PerfQuality_Value chain[] = {
		requested,
		NVSDK_NGX_PerfQuality_Value_MaxQuality,
		NVSDK_NGX_PerfQuality_Value_Balanced,
		NVSDK_NGX_PerfQuality_Value_MaxPerf,
	};
	for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); ++i) {
		const NVSDK_NGX_PerfQuality_Value c = chain[i];
		bool dup = false;
		for (size_t j = 0; j < i; ++j) {
			if (chain[j] == c) { dup = true; break; }
		}
		if (dup)
			continue;
		unsigned int ow = 0, oh = 0;
		if (!query_optimal(p, w, h, c, &ow, &oh))
			continue;
		if (nearly_native_res(ow, oh, w, h) && c != NVSDK_NGX_PerfQuality_Value_MaxPerf)
			continue;
		*out_perf = c;
		*render_w = ow;
		*render_h = oh;
		return true;
	}
	return false;
}

}

NVSDK_NGX_PerfQuality_Value perf_quality_for(uint32_t quality_mode) noexcept
{
	switch (static_cast<UpscaleMode>(quality_mode)) {
	case UpscaleMode::UltraQuality: return NVSDK_NGX_PerfQuality_Value_UltraQuality;
	case UpscaleMode::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	case UpscaleMode::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
	case UpscaleMode::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
	case UpscaleMode::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
	case UpscaleMode::Dlaa:
	default: return NVSDK_NGX_PerfQuality_Value_DLAA;
	}
}

NVSDK_NGX_PerfQuality_Value perf_hint_for_ratio(uint32_t render_w, uint32_t render_h,
	uint32_t display_w, uint32_t display_h) noexcept
{
	if (render_w >= display_w && render_h >= display_h)
		return NVSDK_NGX_PerfQuality_Value_DLAA;

	if (render_w * 100u >= display_w * 49u && render_h * 100u >= display_h * 49u)
		return NVSDK_NGX_PerfQuality_Value_MaxQuality;
	return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}

bool resolve_render_size(NgxSession &s, uint32_t w, uint32_t h, NVSDK_NGX_PerfQuality_Value *out_perf)
{
	auto *const p = reinterpret_cast<NVSDK_NGX_Parameter *>(s.params);
	NVSDK_NGX_PerfQuality_Value perf = perf_quality_for(s.cfg.quality_mode);

	s.render_width = w;
	s.render_height = h;
	if (s.cfg.render_width > 0 && s.cfg.render_height > 0) {

		s.render_width = s.cfg.render_width < w ? s.cfg.render_width : w;
		s.render_height = s.cfg.render_height < h ? s.cfg.render_height : h;
		perf = perf_hint_for_ratio(s.render_width, s.render_height, w, h);
		wchar_t buf[160]{};
		_snwprintf_s(buf, _TRUNCATE, L"%sexplicit input %ux%u -> %ux%u (perf hint %d)", s.log_tag,
			s.render_width, s.render_height, w, h, static_cast<int>(perf));
		diag_info("dlss", buf);
	} else if (s.cfg.render_scale > 0.0f) {
		scaled_render_size(w, h, s.cfg.render_scale, &s.render_width, &s.render_height);

		perf = perf_hint_for_ratio(s.render_width, s.render_height, w, h);
		wchar_t buf[160]{};
		_snwprintf_s(buf, _TRUNCATE, L"%scustom render_scale=%.3f -> %ux%u", s.log_tag,
			s.cfg.render_scale, s.render_width, s.render_height);
		diag_info("dlss", buf);
	} else if (perf != NVSDK_NGX_PerfQuality_Value_DLAA) {
		diag_info("dlss", std::wstring(s.log_tag) + L"ensure_feature: OptimalSettings");
		unsigned int opt_w = 0, opt_h = 0;
		if (!resolve_chain(p, w, h, perf, &perf, &opt_w, &opt_h)) {
			s.status = UpscalerStatus::InitFailed;
			s.last_error = L"OptimalSettings (no usable DLSS profile)";
			diag_error("dlss", s.last_error);
			return false;
		}
		s.render_width = opt_w;
		s.render_height = opt_h;
		if (perf != perf_quality_for(s.cfg.quality_mode)) {
			wchar_t buf[160]{};
			_snwprintf_s(buf, _TRUNCATE, L"%sOptimalSettings fallback -> perf=%d %ux%u", s.log_tag,
				static_cast<int>(perf), opt_w, opt_h);
			diag_info("dlss", buf);
		}
	}
	*out_perf = perf;
	return true;
}

void set_preset_hints(NVSDK_NGX_Parameter *create_params, const UpscalerParams &cfg)
{

	{
		const unsigned preset = cfg.render_preset;
		create_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
		create_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
		create_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
		create_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset);
		create_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset);
		create_params->Set(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, preset);
	}
}

void fill_create_params(const NgxSession &s, uint32_t w, uint32_t h,
	NVSDK_NGX_PerfQuality_Value perf, NVSDK_NGX_DLSS_Create_Params *out) noexcept
{
	*out = NVSDK_NGX_DLSS_Create_Params{};
	out->Feature.InWidth = s.render_width;
	out->Feature.InHeight = s.render_height;
	out->Feature.InTargetWidth = w;
	out->Feature.InTargetHeight = h;
	out->Feature.InPerfQualityValue = perf;

	out->InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

	if (s.cfg.depth_inverted)
		out->InFeatureCreateFlags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
	out->InEnableOutputSubrects = false;
}

}
}
