#pragma once

#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <dxgiformat.h>

#include <cstdint>
#include <string>

namespace aeon_sr {

struct DlssFeatureKey {
	uint32_t render_preset = 0xFFFFFFFFu;
	uint32_t quality_mode = 0xFFFFFFFFu;
	float render_scale = -1.0f;
	uint32_t render_width = 0, render_height = 0;
	bool depth_inverted = false;

	bool depth_reshape = false;

	static DlssFeatureKey from(const UpscalerParams &p) noexcept
	{
		DlssFeatureKey k;
		k.render_preset = p.render_preset;
		k.quality_mode = p.quality_mode;
		k.render_scale = p.render_scale;
		k.render_width = p.render_width;
		k.render_height = p.render_height;
		k.depth_inverted = p.depth_inverted;

		return k;
	}
	bool operator==(const DlssFeatureKey &) const noexcept = default;
};

struct NgxSession {
	explicit NgxSession(const wchar_t *tag) noexcept : log_tag(tag) {}

	const wchar_t *log_tag;

	bool ngx_initialized = false;
	bool dll_present = false;

	bool crashed = false;
	UpscalerStatus status = UpscalerStatus::Idle;
	std::wstring last_error;

	uint32_t width = 0, height = 0;
	uint32_t render_width = 0, render_height = 0;

	UpscalerParams cfg;
	DlssFeatureKey created;

	float jitter_x = 0.0f, jitter_y = 0.0f;

	uint64_t eval_count = 0;

	void *feature_handle = nullptr;
	void *params = nullptr;

	DXGI_FORMAT scratch_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT backbuffer_format = DXGI_FORMAT_UNKNOWN;

	std::wstring addon_dir, dll_dir, data_dir;

	bool ensure_dll_present();

	std::wstring prepare_data_dir();

	void reset_after_shutdown() noexcept;
};

}
