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
#include <string>

namespace aeon_sr {

class RemoteEngine {
public:
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
		reshade::api::command_queue *game_queue = nullptr;
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

	const std::wstring &host_path() const noexcept { return host_path_; }
	uint32_t host_pid() const noexcept { return host_pid_; }
	std::wstring describe() const;

	bool backend_present(BackendChoice choice) const noexcept;
	bool neural_present() const noexcept { return neural_present_ != 0; }
	uint32_t host_gpu_vendor() const noexcept { return host_gpu_vendor_; }

	~RemoteEngine();

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
	bool exchange(EngineDevice &engine, const Request &req,
		const remote::PlaneDesc *descs, Reply &reply);
	void release_plane(uint32_t role);
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
};

}
