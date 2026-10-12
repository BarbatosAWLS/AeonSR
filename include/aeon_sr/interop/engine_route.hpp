#pragma once

namespace aeon_sr {

struct EngineRoute {
	bool dlss_in_host = false;
	bool all_in_host = false;
};

inline EngineRoute engine_route(bool bits32, bool native_dlss, bool ngx_layer, bool dlss_selected,
	bool engine_upscaler_on, bool neural_on) noexcept
{
	EngineRoute r;
	if (bits32) {
		r.dlss_in_host = r.all_in_host = true;
		return r;
	}
	r.all_in_host = ngx_layer && (neural_on || engine_upscaler_on);
	r.dlss_in_host = r.all_in_host || ((native_dlss || ngx_layer) && dlss_selected);
	return r;
}

inline EngineRoute with_ngx_override(EngineRoute r, bool ngx_override) noexcept
{
	if (ngx_override)
		r.dlss_in_host = r.all_in_host;
	return r;
}

}
