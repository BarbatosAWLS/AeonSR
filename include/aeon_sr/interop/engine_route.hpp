#pragma once

namespace aeon_sr {

struct EngineRoute {
	bool dlss_in_host = false;
	bool all_in_host = false;
	bool neural_in_host = false;
	bool dlss_to_layer = false;
};

inline EngineRoute engine_route(bool bits32, bool native_dlss, bool ngx_layer, bool dlss_selected,
	bool engine_upscaler_on, bool neural_on) noexcept
{
	EngineRoute r;
	if (bits32) {
		r.dlss_in_host = r.all_in_host = r.neural_in_host = true;
		return r;
	}
	r.all_in_host = ngx_layer && (neural_on || engine_upscaler_on);
	r.dlss_in_host = r.all_in_host || ((native_dlss || ngx_layer) && dlss_selected);
	r.neural_in_host = r.all_in_host;
	return r;
}

inline EngineRoute with_layer_dlss(EngineRoute r, bool layer_dlss, bool native_dlss, bool dlss_selected,
	bool neural_on) noexcept
{
	if (!layer_dlss || native_dlss || !dlss_selected)
		return r;
	r.dlss_to_layer = true;
	r.dlss_in_host = false;
	r.all_in_host = false;
	r.neural_in_host = neural_on;
	return r;
}

inline EngineRoute with_ngx_override(EngineRoute r, bool ngx_override) noexcept
{
	if (ngx_override)
		r.dlss_in_host = r.all_in_host;
	return r;
}

inline bool neural_alone_after_local(const EngineRoute &r, bool upscaled_on_engine_planes) noexcept
{
	return upscaled_on_engine_planes && r.neural_in_host && (r.all_in_host || r.dlss_to_layer);
}

}
