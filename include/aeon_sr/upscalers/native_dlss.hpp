#pragma once

#include <string>

namespace aeon_sr {

struct NativeDlss {
	bool queried = false;
	bool streamline = false;
	bool dlss = false;
	bool frame_generation = false;
	bool ray_reconstruction = false;
	bool ngx_core = false;
	std::wstring dlss_path;
	std::wstring modules;
};

NativeDlss scan_native_dlss();

void snapshot_native_dlss();
const NativeDlss &native_dlss_at_load() noexcept;

}
