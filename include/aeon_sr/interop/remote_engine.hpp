#pragma once

#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/core/gpu_vendor.hpp"
#include "aeon_sr/motion/guide_builder.hpp"
#include "aeon_sr/interop/interop.hpp"
#include "aeon_sr/ngx/neural_params.hpp"
#include "aeon_sr/interop/remote_protocol.hpp"
#include "aeon_sr/upscalers/upscaler_backend.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace aeon_sr {

class RemoteEngine {
public:
	struct Extras {
		std::string fsr_provider;
		uint32_t neural_restart_epoch = 0;
		uint32_t accum_epoch = 0, accum_target = 0;
		uint32_t capture_epoch = 0, capture_frames = 0;
		std::wstring capture_dir;
	};
	struct Info {
		bool valid = false;
		uint32_t neural_flags = 0;
		bool accum_running = false, accum_holding = false;
		uint32_t accum_done = 0, accum_target = 0;
		float neural_area_share = 0.0f;
		uint32_t neural_driver_major = 0, neural_driver_minor = 0;
		uint32_t neural_required_major = 0, neural_required_minor = 0;
		uint32_t neural_min_architecture = 0, neural_card_architecture = 0;
		uint32_t neural_runtime_minimum = 0, neural_reported_architecture = 0;
		uint64_t neural_eval_count = 0;
		uint32_t vsr_status = 0;
		std::vector<std::string> fsr_providers;
		std::wstring fsr_provider_version;
		std::string neural_runtime_version, neural_runtime_sha256, neural_runtime_kernels, neural_runtime_cards;
		std::wstring neural_runtime_file, neural_capture_status;
	};
	struct Request {
		const FrameInputs *inputs = nullptr;
		BackendChoice backend = BackendChoice::None;
		UpscalerParams upscaler;
		bool run_neural = false;
		bool neural_want_depth = false;
		NeuralRenderParams neural;
		ID3D12Resource *guide = nullptr;
		D3D12_RESOURCE_STATES guide_state = GuideBuilder::kMaskState;
		float depth_jitter_u = 0.0f, depth_jitter_v = 0.0f;
		bool upscaled_here = false;
		reshade::api::command_queue *game_queue = nullptr;
		Extras extras;
	};

	struct Reply {
		remote::RemoteStep step = remote::RemoteStep::NoHost;
		BackendChoice backend_ran = BackendChoice::None;
		UpscalerStatus upscaler_status = UpscalerStatus::Idle;
		int neural_ran = 0;
		uint32_t out_width = 0, out_height = 0;
		uint32_t in_width = 0, in_height = 0;
		uint32_t neural_model_width = 0, neural_model_height = 0;
		uint32_t neural_eval_rows = 0, neural_passes_built = 0;
		float neural_gpu_ms = 0.0f;
		std::wstring message;
		bool list_recording = true;
		float wait_ms = std::numeric_limits<float>::quiet_NaN();
		bool host_busy = false;
		Info info;
	};

	bool start(EngineDevice &engine, HMODULE module, reshade::api::device_api game_api);
	void stop();
	bool ready() const noexcept;

	bool run(EngineDevice &engine, const Request &req, Reply &reply);

	struct Row {
		UpscalerStatus status = UpscalerStatus::Idle;
		std::wstring message;
	};
	static Row upscaler_row(const Reply &reply, const FrameInputs &inputs);

	static bool reports_neural(const Reply &reply) noexcept;

	std::wstring last_error;

	uint32_t host_pid() const noexcept { return host_pid_; }
	std::wstring describe() const;

	bool backend_present(BackendChoice choice) const noexcept;
	bool neural_present() const noexcept { return neural_present_ != 0; }
	uint32_t host_gpu_vendor() const noexcept { return host_gpu_vendor_; }

	~RemoteEngine();

	bool host_gpu_priority_high = false;

private:
	struct SharedTexture {
		ID3D12Resource *res = nullptr;
		HANDLE handle = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint64_t generation = 0;
		wchar_t name[remote::kNameChars] = {};
	};
	static constexpr uint32_t kPlanes = static_cast<uint32_t>(remote::PlaneRole::Count);

	bool ensure_plane(EngineDevice &engine, uint32_t role, ID3D12Resource *src,
		remote::PlaneDesc &desc);
	bool ensure_texture(EngineDevice &engine, SharedTexture &t, const wchar_t *tag, bool written_there,
		ID3D12Resource *src, remote::PlaneDesc &desc);
	bool exchange(EngineDevice &engine, const Request &req,
		const remote::PlaneDesc *descs, Reply &reply);
	void release_plane(uint32_t role);
	void release_texture(SharedTexture &t);
	bool host_still_owns_planes() const noexcept;

	HANDLE map_ = nullptr;
	remote::Block *block_ = nullptr;
	HANDLE request_ = nullptr;
	HANDLE response_ = nullptr;
	HANDLE host_process_ = nullptr;

	EngineDevice *engine_dev_ = nullptr;

	ID3D12Fence *fence_ = nullptr;
	HANDLE fence_handle_ = nullptr;
	uint64_t fence_value_ = 0;
	uint64_t frame_index_ = 0;

	SharedTexture planes_[kPlanes];
	SharedTexture output_;

	remote::SessionNames names_{};
	std::wstring host_path_;
	uint32_t game_pid_ = 0;
	uint32_t host_pid_ = 0;
	uint32_t backends_present_ = 0;
	uint32_t neural_present_ = 0;
	uint32_t host_gpu_vendor_ = static_cast<uint32_t>(GpuVendor::Unknown);
	bool handshaked_ = false;
	bool first_frame_ = true;

	uint64_t last_wait_value_ = 0;
	uint64_t outstanding_signal_ = 0;
	bool frame_outstanding_ = false;

	struct Stats {
		uint32_t frames = 0, answered = 0, timeouts = 0, busy = 0;
		double wait_ms = 0.0, wait_max_ms = 0.0;
		ULONGLONG since = 0;
	} stats_;
	void note_stats() noexcept;
};

}
