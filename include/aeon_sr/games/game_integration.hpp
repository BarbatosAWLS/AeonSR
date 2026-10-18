#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace aeon_sr::games {

struct CompatEntry {
	const char *id;
	const char *title;
	const wchar_t *exe;
	const char *engine;
	const char *api;
	const char *scale_method;
	const char *upscale_point;
	bool hud_native;
	const char *motion;
	const char *scale_range;
	const char *status;
	const char *tested_build;
};

const CompatEntry *compatibility_list(size_t *count);

class GameIntegration {
public:
	virtual ~GameIntegration() = default;
	virtual const CompatEntry &entry() const = 0;

	virtual bool attach(std::wstring *why) = 0;
	virtual void detach() = 0;
	virtual bool attached() const = 0;
	virtual std::wstring note() const = 0;

	virtual void set_render_scale(float scale) noexcept = 0;
	virtual float applied_render_scale() const noexcept = 0;
	virtual bool replaces_stretch() const noexcept = 0;
	virtual bool routes_full_size() const noexcept { return false; }
	virtual bool reads_before_game_aa() const noexcept { return false; }
};

GameIntegration *integration_for_process();
const CompatEntry *find_entry(const wchar_t *exe_name);

float render_scale_for_mode(unsigned int upscale_mode) noexcept;

}
