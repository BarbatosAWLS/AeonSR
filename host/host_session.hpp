#pragma once

#include "aeon_sr/upscalers/backend_dlss.hpp"
#include "aeon_sr/upscalers/backend_fsr.hpp"
#include "aeon_sr/upscalers/backend_xess.hpp"
#include "aeon_sr/interop/interop.hpp"
#include "aeon_sr/ngx/ngx_dlssnr.hpp"
#include "aeon_sr/ngx/ngx_runtime_d3d12.hpp"
#include "aeon_sr/interop/remote_protocol.hpp"

#include <d3d12.h>

#include <cstdint>
#include <string>

namespace aeon_sr::host {

void copy_text(wchar_t *dst, uint32_t dst_chars, const std::wstring &text) noexcept;

struct HostCapabilities {
	uint32_t backends_present = 0;
	uint32_t neural_present = 0;
	uint32_t gpu_vendor = 0;
};

class HostSession {
public:
	HostSession() = default;
	HostSession(const HostSession &) = delete;
	HostSession &operator=(const HostSession &) = delete;

	bool start(uint32_t game_pid, remote::Block *block, const std::wstring &host_dir);
	void shutdown();
	~HostSession() { shutdown(); }

	HostCapabilities capabilities();

	void serve_frame();

	bool shutdown_requested() const noexcept;

	const std::wstring &last_error() const noexcept { return last_error_; }

private:
	struct Plane {
		ID3D12Resource *res = nullptr;
		uint64_t generation = 0;
		wchar_t name[remote::kNameChars] = {};
	};

	void run_frame();
	void finish_frame(uint64_t frame_index);

	bool ensure_fence();
	bool open_planes();
	bool run_upscaler(const FrameInputs &inputs);
	void fail_neural(const std::wstring &text);
	void run_neural(const FrameInputs &inputs, ID3D12Resource *colour,
		ID3D12Resource *motion, ID3D12Resource *depth);

	ID3D12Resource *open_shared_texture(const wchar_t *name, HRESULT *out_hr) const;
	void release_plane(Plane &plane) noexcept;
	void release_planes() noexcept;

	void fail(remote::RemoteStep step, const char *key, std::wstring text,
		const wchar_t *detail = nullptr);

	remote::Block *block_ = nullptr;
	uint32_t game_pid_ = 0;

	EngineDevice engine_;
	NgxRuntimeD3D12 ngx12_;
	DlssD3D12Backend dlss_;
	Fsr4D3D12Backend fsr_;
	XessD3D12Backend xess_;
	NeuralRenderD3D12 neural_;
	bool neural_init_failed_ = false;

	Plane planes_[static_cast<uint32_t>(remote::PlaneRole::Count)];

	ID3D12Fence *fence_ = nullptr;
	wchar_t fence_name_[remote::kNameChars] = {};

	remote::RemoteStep step_ = remote::RemoteStep::Ok;
	std::wstring message_;
	uint32_t backend_ran_ = 0;
	uint32_t upscaler_status_ = 0;
	uint32_t neural_ran_ = 0;
	uint32_t out_width_ = 0;
	uint32_t out_height_ = 0;
	uint32_t in_width_ = 0;
	uint32_t in_height_ = 0;
	uint64_t fence_signal_value_ = 0;

	std::wstring last_error_;
};

}
