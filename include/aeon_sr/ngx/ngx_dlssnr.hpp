#pragma once

#include "aeon_sr/interop/blit_d3d12.hpp"
#include "aeon_sr/depth/depth_on_grid.hpp"
#include "aeon_sr/ngx/neural_hardware.hpp"
#include "aeon_sr/interop/gpu12.hpp"
#include "aeon_sr/ngx/neural_params.hpp"
#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/upscalers/upscaler_status.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace aeon_sr {

namespace dlssnr_param {

inline constexpr const char *kWidth = "DLSSNR.Width";
inline constexpr const char *kHeight = "DLSSNR.Height";
inline constexpr const char *kScalingRatio = "DLSSNR.ScalingRatio";
inline constexpr const char *kEnabled = "DLSSNR.Enabled";
inline constexpr const char *kReset = "DLSSNR.Reset";
inline constexpr const char *kRenderPreset = "DLSSNR.Hint.Render.Preset";

inline constexpr const char *kColor = "DLSSNR.Color";
inline constexpr const char *kOutput = "DLSSNR.Output";
inline constexpr const char *kDepth = "DLSSNR.Depth";
inline constexpr const char *kMVec = "DLSSNR.MVec";

inline constexpr const char *kMVecScaleX = "DLSSNR.MVecScaleX";
inline constexpr const char *kMVecScaleY = "DLSSNR.MVecScaleY";
inline constexpr const char *kDepthInverted = "DLSSNR.DepthInverted";

inline constexpr const char *kStyle = "DLSSNR.Style";
inline constexpr const char *kIntensity = "DLSSNR.Intensity";
inline constexpr const char *kLocalStructureStrength = "DLSSNR.LocalStructureStrength";
inline constexpr const char *kLocalToneStrength = "DLSSNR.LocalToneStrength";
inline constexpr const char *kSkinStructureStrength = "DLSSNR.SkinStructureStrength";
inline constexpr const char *kUICorrection = "DLSSNR.UICorrection";
inline constexpr const char *kUseAutoMask = "DLSSNR.UseAutoMask";

inline constexpr const char *kColorSubrectBaseX = "DLSSNR.ColorSubrectBaseX";
inline constexpr const char *kColorSubrectBaseY = "DLSSNR.ColorSubrectBaseY";
inline constexpr const char *kColorSubrectWidth = "DLSSNR.ColorSubrectWidth";
inline constexpr const char *kColorSubrectHeight = "DLSSNR.ColorSubrectHeight";
inline constexpr const char *kOutputSubrectBaseX = "DLSSNR.OutputSubrectBaseX";
inline constexpr const char *kOutputSubrectBaseY = "DLSSNR.OutputSubrectBaseY";
inline constexpr const char *kOutputSubrectWidth = "DLSSNR.OutputSubrectWidth";
inline constexpr const char *kOutputSubrectHeight = "DLSSNR.OutputSubrectHeight";
inline constexpr const char *kDepthSubrectBaseX = "DLSSNR.DepthSubrectBaseX";
inline constexpr const char *kDepthSubrectBaseY = "DLSSNR.DepthSubrectBaseY";
inline constexpr const char *kDepthSubrectWidth = "DLSSNR.DepthSubrectWidth";
inline constexpr const char *kDepthSubrectHeight = "DLSSNR.DepthSubrectHeight";
inline constexpr const char *kMVecSubrectBaseX = "DLSSNR.MVecSubrectBaseX";
inline constexpr const char *kMVecSubrectBaseY = "DLSSNR.MVecSubrectBaseY";
inline constexpr const char *kMVecSubrectWidth = "DLSSNR.MVecSubrectWidth";
inline constexpr const char *kMVecSubrectHeight = "DLSSNR.MVecSubrectHeight";
}

inline constexpr int kNeuralFeatureId = 1;

inline constexpr unsigned int kNeuralRenderPresetShipping = 1u;

struct NgxShim {
	void *nr_runtime = nullptr;
	void *trampoline = nullptr;
	void *fn_init = nullptr, *fn_create = nullptr, *fn_evaluate = nullptr;
	void *fn_release = nullptr, *fn_shutdown = nullptr;
	void *fn_requirements = nullptr;
	void *fn_gpu_architecture = nullptr;
	void *via_init = nullptr, *via_create = nullptr, *via_evaluate = nullptr;
	void *via_release = nullptr, *via_shutdown = nullptr;
	void *via_requirements = nullptr;

	bool ready() const noexcept { return nr_runtime != nullptr && trampoline != nullptr; }

	uint32_t minimum_architecture() const noexcept;

	bool load(const std::wstring &runtime_path, const std::wstring &trampoline_path, std::wstring *error);
	void unload();

	long init(unsigned long long app_id, const wchar_t *data_path, ID3D12Device *device,
		int sdk_version, unsigned long *seh) const;
	long create(ID3D12GraphicsCommandList *cmd, int feature_id, void *params,
		void **out_handle, unsigned long *seh) const;
	long evaluate(ID3D12GraphicsCommandList *cmd, void *handle, void *params, unsigned long *seh) const;
	long release(void *handle, unsigned long *seh) const;
	long shutdown(ID3D12Device *device, unsigned long *seh) const;
	long requirements(IDXGIAdapter *adapter, const void *discovery, void *out_requirement) const;
};

struct NeuralRenderCommon {
	bool dll_present = false;
	bool shim_present = false;
	bool initialized = false;
	bool crashed = false;
	UpscalerStatus status = UpscalerStatus::Idle;
	std::wstring last_error;
	uint64_t eval_count = 0;

	uint32_t driver_major = 0, driver_minor = 0;
	uint32_t required_driver_major = 0, required_driver_minor = 0;

	bool driver_too_old() const noexcept
	{
		if (driver_major == 0 || required_driver_major == 0)
			return false;
		return driver_major < required_driver_major ||
			(driver_major == required_driver_major && driver_minor < required_driver_minor);
	}

	bool requirement_known = false;
	uint32_t feature_support = 0;
	uint32_t min_architecture = 0;
	bool feature_unsupported = false;

	static constexpr uint32_t kSupportCheckNotPresent = 1u;
	static constexpr uint32_t kSupportDriverUnsupported = 2u;
	static constexpr uint32_t kSupportAdapterUnsupported = 4u;
	static constexpr uint32_t kSupportOsBelowMinimum = 8u;
	static constexpr uint32_t kSupportNotImplemented = 16u;

	bool adapter_unsupported() const noexcept
	{
		return requirement_known && (feature_support & kSupportAdapterUnsupported) != 0u;
	}

	uint32_t card_architecture = 0;
	bool runtime_targets_known = false;
	NeuralRuntimeKernels runtime_kernels;
	NeuralRuntimeIdentity runtime_identity;
	std::string runtime_sha256;

	bool runtime_serves_card() const noexcept
	{
		if (!runtime_targets_known || card_architecture == 0u)
			return true;
		return neural_kernels_serve(runtime_kernels, card_architecture);
	}

	uint32_t runtime_minimum_architecture = 0;
	uint32_t reported_architecture = 0;

	std::wstring addon_dir;
	std::wstring dll_dir;
	std::wstring dll_path;
	std::vector<std::wstring> runtime_candidates;
	std::wstring data_dir;

	mutable std::mutex text_mutex;
	struct Text {
		UpscalerStatus status = UpscalerStatus::Idle;
		std::wstring last_error;
		std::wstring dll_path;
		std::vector<std::wstring> candidates;
		bool targets_known = false;
		NeuralRuntimeKernels kernels;
		NeuralRuntimeIdentity identity;
		std::string sha256;
	};
	Text text() const
	{
		const std::lock_guard<std::mutex> lock(text_mutex);
		Text t;
		t.status = status;
		t.last_error = last_error;
		t.dll_path = dll_path;
		t.candidates = runtime_candidates;
		t.targets_known = runtime_targets_known;
		t.kernels = runtime_kernels;
		t.identity = runtime_identity;
		t.sha256 = runtime_sha256;
		return t;
	}
	void set_status(UpscalerStatus s, std::wstring detail)
	{
		const std::lock_guard<std::mutex> lock(text_mutex);
		status = s;
		last_error = std::move(detail);
	}
	bool serves_card(const Text &t) const noexcept
	{
		if (!t.targets_known || card_architecture == 0u)
			return true;
		return neural_kernels_serve(t.kernels, card_architecture);
	}

	bool ensure_dll_present();
	bool probe_dll_present();
	std::wstring shim_path() const;
	void fail(UpscalerStatus s, std::wstring detail);

private:
	bool refresh_files(bool choose);
};

struct NeuralCapturePlane {
	const wchar_t *file = nullptr;
	const char *name = nullptr;
	ID3D12Resource *readback = nullptr;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	UINT rows = 0;
	UINT64 row_bytes = 0;
	UINT64 total_bytes = 0;
	uint32_t width = 0, height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct NeuralCapture {
	static constexpr uint32_t kMaxPlanes = 5;
	bool requested = false;
	bool pending = false;
	bool armed = false;

	bool discard = false;
	UINT64 fence_value = 0;
	std::wstring dir;
	NeuralCapturePlane planes[kMaxPlanes];
	uint32_t plane_count = 0;
	std::string meta;
	std::wstring last;

	std::wstring burst_root;
	uint32_t burst_left = 0;
	uint32_t burst_index = 0;
};

inline constexpr uint32_t kNeuralCaptureBurst = 8;

struct NeuralRenderD3D12 : NeuralRenderCommon {
	ID3D12Device *device = nullptr;
	ID3D12CommandQueue *queue = nullptr;
	uint32_t width = 0, height = 0;

	NgxShim shim;
	void *params = nullptr;

	void *feature_handles[kNeuralPassMax] = {};
	uint32_t created_passes = 0;
	uint32_t requested_passes = 0;

	static constexpr D3D12_RESOURCE_STATES kNgxSrvState =
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

	ID3D12Resource *color_copy = nullptr;
	ID3D12Resource *output_tex = nullptr;

	ID3D12Resource *frame_copy = nullptr;

	ID3D12Resource *proxy_copy = nullptr;

	ID3D12Resource *mv_small = nullptr, *depth_small = nullptr;

	ID3D12Resource *delta_tex[2] = { nullptr, nullptr };
	ID3D12Resource *built_tex = nullptr;
	ID3D12Resource *smooth_tex[2] = { nullptr, nullptr };
	uint32_t delta_index = 0;
	bool delta_valid = false;
	uint32_t last_eval_rows = 0;
	float last_warp_x = 0.0f, last_warp_y = 0.0f;
	float input_detail = kNeuralInputDetail;
	float last_area_share = 0.0f;
	uint32_t logged_whole = 0;
	uint64_t cost_logged_at = 0;
	DXGI_FORMAT mv_small_format = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT depth_small_format = DXGI_FORMAT_UNKNOWN;

	DXGI_FORMAT scratch_format = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;

	BlitPipelineD3D12 blit;
	uint32_t created_style = 0xFFFFFFFFu;

	uint32_t model_width = 0, model_height = 0;
	float created_scale = 1.0f;

	Gpu12Fence gpu_fence;
	Gpu12InitList init_list;
	Gpu12Timer timer;

	NeuralCapture capture;

	ScreenshotAccum accum;

	D3D12_RESOURCE_STATES guide_state = kNgxSrvState;

	bool init_device(ID3D12Device *dev, uint32_t w, uint32_t h);
	void shutdown_device();
	void set_command_queue(ID3D12CommandQueue *q);
	void wait_gpu();

	bool ensure_feature(ID3D12GraphicsCommandList *cmd, uint32_t w, uint32_t h,
		DXGI_FORMAT color_format, DXGI_FORMAT src_format, const NeuralRenderParams &p);
	void destroy_feature();
	void release_scratch();

	bool ensure_composite_scratch(uint32_t w, uint32_t h, DXGI_FORMAT src_format, bool need_proxy);
	bool ensure_delta_scratch();

	ID3D12Resource *downscale_guide(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src,
		ID3D12Resource **slot, DXGI_FORMAT *slot_format, const NeuralDrawConstants &c);

	bool evaluate_chain(ID3D12GraphicsCommandList *cmd, uint32_t mw, uint32_t mh,
		ID3D12Resource *motion_vectors, ID3D12Resource *depth,
		const NeuralRenderParams &p);

	void request_capture(std::wstring dir, uint32_t frames = kNeuralCaptureBurst);
	void capture_record(ID3D12GraphicsCommandList *cmd, ID3D12Resource *motion_vectors,
		ID3D12Resource *depth, const NeuralRenderParams &p,
		float mv_scale_x, float mv_scale_y);
	void capture_poll();
	void capture_complete();
	void capture_finish_now();
	void capture_release();

	bool run(ID3D12GraphicsCommandList *cmd,
		ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
		ID3D12Resource *motion_vectors, ID3D12Resource *depth,
		const NeuralRenderParams &p, float depth_jitter_u = 0.0f, float depth_jitter_v = 0.0f);

	DepthOnGridD3D12 depth_grid;

	bool run_fast(ID3D12GraphicsCommandList *cmd,
		ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
		ID3D12Resource *motion_vectors, ID3D12Resource *depth,
		const NeuralRenderParams &p);
	bool run_composite(ID3D12GraphicsCommandList *cmd,
		ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
		const D3D12_RESOURCE_DESC &desc,
		ID3D12Resource *motion_vectors, ID3D12Resource *depth,
		const NeuralRenderParams &p);
};

}
