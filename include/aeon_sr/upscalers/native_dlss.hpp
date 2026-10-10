#pragma once

#include <string>
#include <vector>

namespace aeon_sr {

struct NativeDlss {
	bool queried = false;
	bool streamline = false;
	bool dlss = false;
	bool frame_generation = false;
	bool ray_reconstruction = false;
	bool ngx_core = false;
	bool native = false;
	std::wstring dlss_path;
	std::wstring source;
	std::wstring modules;
};

enum class DlssModuleKind {
	None,
	Streamline,
	NgxCore,
	Dlss,
	FrameGeneration,
	RayReconstruction,
};

DlssModuleKind dlss_module_kind(const std::wstring &path);
bool dlss_module_from_driver_store(const std::wstring &path);

bool same_module_file(const std::wstring &a, const std::wstring &b);

NativeDlss classify_native_dlss(const std::vector<std::wstring> &module_paths, const std::wstring &own_runtime,
	bool own_ngx_started, const std::vector<std::wstring> &before_own = {});

NativeDlss scan_native_dlss(const std::wstring &own_runtime);

struct NgxLayer {
	std::wstring path;
	std::wstring name;
	bool optiscaler = false;
};

bool ngx_layer_candidate(const std::wstring &path, bool exports_ngx);

std::vector<NgxLayer> scan_ngx_layers(void *self);

std::wstring ngx_layer_label(const NgxLayer &layer);

void snapshot_native_dlss();
const NativeDlss &native_dlss_at_load() noexcept;

}
