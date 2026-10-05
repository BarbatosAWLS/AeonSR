#pragma once

#include "aeon_sr/upscalers/upscaler_capture_format.hpp"

#include <d3d12.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aeon_sr {

constexpr uint32_t kUpscalerCaptureFrames = 32;
constexpr uint32_t kUpscalerCaptureKey = 0x78u;

class UpscalerCaptureD3D12 {
public:
	static constexpr uint64_t kMaxPendingBytes = 2ull << 30;

	~UpscalerCaptureD3D12();

	void request(const std::wstring &root, uint32_t frames = kUpscalerCaptureFrames);
	bool recording() const noexcept { return left_ > 0; }

	void begin_frame(ID3D12Device *device, ID3D12CommandQueue *queue);

	void record_drawn(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *color,
		D3D12_RESOURCE_STATES state);

	void record(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
		ID3D12Resource *color, D3D12_RESOURCE_STATES color_state,
		ID3D12Resource *mvec, D3D12_RESOURCE_STATES mvec_state,
		ID3D12Resource *depth, D3D12_RESOURCE_STATES depth_state,
		CapturedFrameInfo info);

	void finish(ID3D12Device *device, ID3D12CommandQueue *queue);
	size_t gpu_objects() const;

	std::wstring status() const;

	static constexpr uint64_t kScopeMaxBytes = 4ull << 30;
	void request_scope(const std::wstring &dir, uint32_t frames, uint64_t max_bytes = kScopeMaxBytes);
	bool scope_recording() const noexcept { return scope_left_ > 0; }
	uint32_t scope_next() const noexcept { return scope_next_; }
	void scope_begin(ID3D12Device *device, ID3D12CommandQueue *queue);
	void scope_copy(ScopePlane plane, ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *res,
		D3D12_RESOURCE_STATES state);
	void scope_end(ScopeTraceRow row, const std::string &header);
	std::wstring scope_status() const;

private:
	struct Plane {
		ID3D12Resource *readback = nullptr;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		UINT rows = 0;
		UINT64 row_bytes = 0;
		UINT64 total = 0;
	};
	struct ScopeSlot {
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		UINT rows = 0;
		UINT64 row_bytes = 0;
	};
	struct Frame {
		CapturedFrameInfo info;
		Plane planes[4];
		std::wstring dir;
		UINT64 fence_value = 0;
		bool armed = false;
		bool scope = false;
		uint32_t n = 0;
		ID3D12Resource *scope_buffer = nullptr;
		uint32_t scope_planes = 0;
		ScopeSlot slots[kScopeStagePlanes];
	};

	bool copy_plane(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, ID3D12Resource *res,
		D3D12_RESOURCE_STATES state, const char *file, Plane *plane, CapturedPlane *out);
	void collect(ID3D12Device *device, ID3D12CommandQueue *queue);
	bool scope_size(ID3D12Device *device, const D3D12_RESOURCE_DESC &color);
	void scope_close_all();
	bool scope_finalize_due() const;
	void scope_finalize();
	void stop(const std::wstring &why);
	void writer_main();
	static void release(Frame &f) noexcept;
	void set_status(const std::wstring &s);

	std::wstring root_;
	uint32_t wanted_ = 0;
	uint32_t left_ = 0;
	uint32_t next_index_ = 0;
	uint64_t sequence_ = 0;
	uint64_t last_recorded_ = 0;
	bool recorded_this_frame_ = false;
	Plane drawn_{};
	CapturedPlane drawn_info_{};

	ID3D12Fence *fence_ = nullptr;
	UINT64 fence_value_ = 0;
	std::vector<Frame> in_flight_;

	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<Frame> to_write_;
	uint64_t pending_bytes_ = 0;
	uint32_t written_ = 0;
	bool writer_busy_ = false;
	bool quit_ = false;
	std::wstring status_;
	std::wstring write_error_;
	std::thread writer_;

	std::wstring scope_dir_;
	uint32_t scope_left_ = 0;
	uint32_t scope_next_ = 0;
	uint64_t scope_max_bytes_ = kScopeMaxBytes;
	bool scope_sized_ = false;
	D3D12_RESOURCE_DESC scope_color_{};
	ScopeSlot scope_slots_[kScopeStagePlanes];
	uint64_t scope_frame_bytes_ = 0;
	std::vector<ID3D12Resource *> scope_pool_;
	Frame scope_cur_;
	uint32_t scope_skipped_ = 0;
	std::vector<ScopeTraceRow> scope_rows_;
	std::string scope_header_;
	CapturedPlane scope_fmt_[kScopeStagePlanes];
	std::vector<uint32_t> scope_members_, scope_held_;
	uint32_t scope_outstanding_ = 0;
	bool scope_closed_ = false;
	bool scope_done_ = true;
	uint32_t scope_frames_written_ = 0;
	std::wstring scope_note_;
	std::wstring scope_error_;
};

}
