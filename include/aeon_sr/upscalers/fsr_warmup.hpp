#pragma once

#include "aeon_sr/upscalers/ffx_loader.hpp"

#include <d3d12.h>

#include <ffx_upscale.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aeon_sr {

constexpr uint32_t kFsrCreateFlags =
	FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;

constexpr uint32_t kFsrColorSpaceSdr = 0;
constexpr uint32_t fsr_color_flags(uint32_t color_space) noexcept
{
	return color_space == kFsrColorSpaceSdr ? static_cast<uint32_t>(FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB) : 0u;
}

struct FsrWarmupDesc {
	uint32_t render_width = 0;
	uint32_t render_height = 0;
	uint32_t upscale_width = 0;
	uint32_t upscale_height = 0;
	DXGI_FORMAT color_format = DXGI_FORMAT_R8G8B8A8_UNORM;
	bool with_depth = true;
	uint32_t dispatch_flags = 0;
};

struct FsrWarmupTiming {
	double first_record_ms = 0.0;
	double second_record_ms = 0.0;
	double gpu_ms = 0.0;
};

struct FsrPipelineCacheStats {
	uint32_t requested = 0;
	uint32_t loaded = 0;
	uint32_t compiled = 0;
	uint32_t stored = 0;
	uint32_t missed = 0;
	bool hooked = false;
	bool file_read = false;
	bool file_stale = false;
	bool saved = false;
	uint64_t file_bytes = 0;
	double open_ms = 0.0;
	double save_ms = 0.0;
};

struct FsrPipelineCache {
	std::wstring path;
	ID3D12PipelineLibrary *lib = nullptr;
	std::vector<uint8_t> blob;
	FsrPipelineCacheStats st;
	bool load_only = false;
	std::vector<std::pair<ID3D12RootSignature *, ID3D12PipelineState *>> standins;
	size_t opened_size = 0;

	FsrPipelineCache() = default;
	FsrPipelineCache(const FsrPipelineCache &) = delete;
	FsrPipelineCache &operator=(const FsrPipelineCache &) = delete;
	~FsrPipelineCache();

	void open(ID3D12Device *device, const std::wstring &file);
	void save();
	void release_standins();
};

bool fsr_warmup_dispatch(const FfxApi &api, ffxContext *context, ID3D12Device *device,
	const FsrWarmupDesc &desc, FsrPipelineCache *cache, FsrWarmupTiming *timing,
	unsigned long *seh_code, bool *context_dirty, std::wstring &error);

std::wstring pipeline_cache_path(const std::wstring &dir, ID3D12Device *device, const wchar_t *prefix);
inline std::wstring fsr_pipeline_cache_path(const std::wstring &dir, ID3D12Device *device)
{
	return pipeline_cache_path(dir, device, L"fsr");
}

}
