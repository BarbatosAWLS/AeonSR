#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace aeon_sr {

struct XefgCaptures {
	static constexpr uint32_t kRing = 4;
	ID3D12Resource *slot[kRing] = {};
	ID3D12Fence *captured = nullptr;
	ID3D12Fence *consumed = nullptr;
	std::atomic<uint64_t> presents{ 0 };
	HANDLE event = nullptr;
	std::atomic<bool> broken{ false };

	~XefgCaptures();
	bool create(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT format);
};

IDXGIFactory2 *make_xefg_capture_factory(IDXGIFactory7 *real, std::shared_ptr<XefgCaptures> captures);

}
