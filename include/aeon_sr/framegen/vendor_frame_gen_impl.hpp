#pragma once

#include "aeon_sr/framegen/vendor_frame_gen.hpp"

#include <d3d12.h>

#include <memory>
#include <string>
#include <vector>

namespace aeon_sr {

std::unique_ptr<VendorFrameGenerator> make_fsr_frame_generator();
std::unique_ptr<VendorFrameGenerator> make_dlss_frame_generator();
std::unique_ptr<VendorFrameGenerator> make_xess_frame_generator();

std::wstring vendor_module_directory();
std::wstring find_vendor_runtime(const wchar_t *file_name, const wchar_t *preferred_subdir = nullptr);

class SideQueueLists {
public:
	~SideQueueLists() { release(); }
	bool ensure(ID3D12Device *device, ID3D12CommandQueue *queue);
	ID3D12GraphicsCommandList *begin();
	uint64_t submit();
	ID3D12Fence *fence() const noexcept { return fence_; }
	ID3D12CommandQueue *queue() const noexcept { return queue_; }
	void release() noexcept;

private:
	static constexpr uint32_t kSlots = 4;
	ID3D12Device *device_ = nullptr;
	ID3D12CommandQueue *queue_ = nullptr;
	ID3D12CommandAllocator *alloc_[kSlots] = {};
	uint64_t done_at_[kSlots] = {};
	ID3D12GraphicsCommandList *list_ = nullptr;
	ID3D12Fence *fence_ = nullptr;
	HANDLE event_ = nullptr;
	uint64_t value_ = 0;
	uint32_t slot_ = 0;
	bool open_ = false;
};

bool wait_fence(ID3D12Fence *fence, uint64_t value, HANDLE event, DWORD ms);

inline uint32_t xefg_interpolation_limit(bool intel, bool can_query, uint32_t max_supported, bool can_set,
	uint32_t ring) noexcept
{
	if (!intel || !can_query || !can_set || max_supported <= 1 || ring < 3)
		return 1;
	uint32_t n = max_supported;
	if (n > ring - 1)
		n = ring - 1;
	if (n > kVendorMaxGenerated)
		n = kVendorMaxGenerated;
	return n;
}

bool dlssg_refusal_is_final(bool capability_refused, uint32_t ngx_result, uint32_t seh_code) noexcept;

void xefg_use_for_tests(const std::wstring &xefg_dll, const std::wstring &xell_dll, int intel);

struct XefgPresentLedger {
	static constexpr uint32_t kPatience = 3;
	uint64_t expected = 0;
	uint32_t short_frames = 0;
	uint64_t last_arrived = 0;

	void settle(uint64_t arrived) noexcept
	{
		if (arrived >= expected) {
			short_frames = 0;
			last_arrived = arrived;
			return;
		}
		if (arrived != last_arrived) {
			short_frames = 0;
			last_arrived = arrived;
			return;
		}
		if (++short_frames >= kPatience) {
			expected = arrived;
			short_frames = 0;
		}
	}
	uint64_t add(uint32_t presented) noexcept
	{
		const uint64_t first = expected + 1;
		expected += presented;
		return first;
	}
};

}
