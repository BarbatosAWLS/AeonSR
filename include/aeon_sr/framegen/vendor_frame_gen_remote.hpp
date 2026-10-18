#pragma once

#include "aeon_sr/framegen/fg_host_protocol.hpp"
#include "aeon_sr/framegen/vendor_frame_gen.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aeon_sr {

std::wstring vendor_frame_gen_host_path();

VendorStep vendor_step_from_host(uint32_t step) noexcept;

class RemoteVendorFrameGenerator final : public VendorFrameGenerator {
public:
	RemoteVendorFrameGenerator(FrameGenerator kind, std::wstring host_exe);
	~RemoteVendorFrameGenerator() override;
	RemoteVendorFrameGenerator(const RemoteVendorFrameGenerator &) = delete;
	RemoteVendorFrameGenerator &operator=(const RemoteVendorFrameGenerator &) = delete;

	FrameGenerator kind() const noexcept override { return kind_; }
	VendorStep record(const VendorFrame &f, std::wstring *note) override;
	void reset() noexcept override { pending_reset_ = true; }
	std::wstring version() const override;
	uint32_t max_generated() const noexcept override { return max_generated_.load(); }
	bool busy() const noexcept override { return starting_.load() || host_busy_.load(); }

	uint32_t host_pid() const noexcept { return host_pid_.load(); }

private:
	struct Shared {
		ID3D12Device *device = nullptr;
		uint32_t width = 0, height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		ID3D12Resource *color = nullptr, *motion = nullptr, *depth = nullptr;
		ID3D12Resource *outputs[fg_host::kMaxOutputs] = {};
		ID3D12Fence *to_host = nullptr, *to_game = nullptr;
		void release() noexcept;
	};

	bool make_shared(const VendorFrame &f, std::wstring *why);
	void start_host();
	bool call(uint32_t timeout_ms);
	void end_host(bool kill) noexcept;
	void settle_owed() noexcept;
	static void CALLBACK on_host_exit(void *self, BOOLEAN timed_out);
	void watch_host() noexcept;
	void unwatch_host() noexcept;
	void retire_shared() noexcept;
	void age_retired() noexcept;

	const FrameGenerator kind_;
	const std::wstring exe_;
	Shared shared_;
	struct Retired {
		Shared s;
		uint32_t frames = 0;
	};
	std::vector<Retired> retired_;

	HANDLE mapping_ = nullptr;
	fg_host::Block *block_ = nullptr;
	HANDLE request_ = nullptr, response_ = nullptr, process_ = nullptr;
	std::thread starter_;
	std::atomic<bool> starting_{ false };
	std::atomic<bool> alive_{ false };
	std::atomic<uint32_t> host_pid_{ 0 };
	std::atomic<uint32_t> max_generated_{ 1 };
	std::atomic<bool> host_busy_{ false };
	HANDLE watch_ = nullptr;
	std::atomic<uint64_t> owed_{ 0 };
	std::wstring start_error_;
	mutable std::mutex version_lock_;
	std::wstring version_;

	uint64_t seq_ = 0;
	uint64_t to_host_value_ = 0;
	uint64_t to_game_value_ = 0;
	bool answered_once_ = false;
	bool pending_reset_ = true;
	uint32_t failures_ = 0;
	uint32_t answered_run_ = 0;
	std::wstring failed_;
	ULONGLONG retry_after_ = 0;
};

}
