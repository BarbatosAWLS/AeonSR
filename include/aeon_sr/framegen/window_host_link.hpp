#pragma once

#include "aeon_sr/framegen/window_host_protocol.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <cstdint>
#include <string>

namespace aeon_sr {

struct WindowHostAnswer {
	HRESULT present_hr = E_FAIL;
	UINT present_count = 0;
	bool stats_valid = false;
	UINT stats_present_count = 0;
	int64_t stats_sync_qpc = 0;
};

class WindowHostLink {
public:
	WindowHostLink() = default;
	WindowHostLink(const WindowHostLink &) = delete;
	WindowHostLink &operator=(const WindowHostLink &) = delete;
	~WindowHostLink() { stop(); }

	bool start(const std::wstring &exe, ID3D12Device *device, ID3D12Resource *const shared[window_host::kShared],
		ID3D12Fence *ready, ID3D12Fence *done, uint32_t w, uint32_t h, DXGI_FORMAT format,
		window_host::ColorSpace color_space, std::wstring *why);

	bool present(uint32_t index, uint64_t ready_value, uint64_t done_value, uint32_t sync_interval,
		WindowHostAnswer *out);
	bool place(bool visible, int x, int y, int cw, int ch);

	HANDLE released() const noexcept { return released_; }
	bool alive() const noexcept { return alive_; }
	uint32_t host_pid() const noexcept { return host_pid_; }
	int gpu_class() const noexcept { return gpu_class_; }
	const std::wstring &error() const noexcept { return error_; }

	void stop(bool kill = false) noexcept;

private:
	bool call(uint32_t timeout_ms);
	void fail(const std::wstring &why) noexcept;

	HANDLE mapping_ = nullptr;
	window_host::Block *block_ = nullptr;
	HANDLE request_ = nullptr, response_ = nullptr, released_ = nullptr;
	HANDLE process_ = nullptr;
	uint32_t host_pid_ = 0;
	int gpu_class_ = -1;
	uint64_t seq_ = 0;
	bool alive_ = false;
	std::wstring error_;
};

}
