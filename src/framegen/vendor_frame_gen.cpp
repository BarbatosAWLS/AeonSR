#include "aeon_sr/framegen/vendor_frame_gen.hpp"
#include "aeon_sr/framegen/vendor_frame_gen_impl.hpp"

#include "aeon_sr/core/dxgi_format_util.hpp"
#include "aeon_sr/core/runtime_search.hpp"
#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"

namespace aeon_sr {

DXGI_FORMAT vendor_frame_format(DXGI_FORMAT f) noexcept
{
	switch (format_family(f)) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return DXGI_FORMAT_R8G8B8A8_UNORM;
	}
}

std::unique_ptr<VendorFrameGenerator> make_vendor_frame_generator(FrameGenerator g)
{
#if defined(AEONSR_NO_VENDOR)
	(void)g;
	return nullptr;
#else
	switch (g) {
	case FrameGenerator::Fsr: return make_fsr_frame_generator();
	case FrameGenerator::Dlss: return make_dlss_frame_generator();
	case FrameGenerator::Xess: return make_xess_frame_generator();
	default: return nullptr;
	}
#endif
}

std::wstring vendor_module_directory()
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&vendor_module_directory), &self);
	return module_directory(self);
}

std::wstring find_vendor_runtime(const wchar_t *file_name, const wchar_t *preferred_subdir)
{
	for (const std::wstring &p : runtime_candidates(
			preferred_search_dirs(vendor_module_directory(), exe_directory_w(), preferred_subdir), file_name))
		if (file_exists_w(p))
			return p;
	return {};
}

bool wait_fence(ID3D12Fence *fence, uint64_t value, HANDLE event, DWORD ms)
{
	if (fence == nullptr)
		return false;
	if (fence->GetCompletedValue() >= value)
		return true;
	if (event == nullptr || FAILED(fence->SetEventOnCompletion(value, event)))
		return false;
	return WaitForSingleObject(event, ms) == WAIT_OBJECT_0 || fence->GetCompletedValue() >= value;
}

bool SideQueueLists::ensure(ID3D12Device *device, ID3D12CommandQueue *queue)
{
	if (device_ == device && queue_ == queue && list_ != nullptr)
		return true;
	release();
	if (device == nullptr || queue == nullptr)
		return false;
	for (uint32_t i = 0; i < kSlots; ++i)
		if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_[i])))) {
			release();
			return false;
		}
	if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0], nullptr, IID_PPV_ARGS(&list_))) ||
		FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
		release();
		return false;
	}
	list_->Close();
	event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	device_ = device;
	queue_ = queue;
	queue_->AddRef();
	return event_ != nullptr;
}

ID3D12GraphicsCommandList *SideQueueLists::begin()
{
	if (list_ == nullptr || open_)
		return nullptr;
	slot_ = (slot_ + 1) % kSlots;
	if (!wait_fence(fence_, done_at_[slot_], event_, 5000))
		return nullptr;
	if (FAILED(alloc_[slot_]->Reset()) || FAILED(list_->Reset(alloc_[slot_], nullptr)))
		return nullptr;
	open_ = true;
	return list_;
}

uint64_t SideQueueLists::submit()
{
	if (!open_)
		return 0;
	open_ = false;
	if (FAILED(list_->Close()))
		return 0;
	ID3D12CommandList *lists[] = { list_ };
	queue_->ExecuteCommandLists(1, lists);
	if (FAILED(queue_->Signal(fence_, ++value_)))
		return 0;
	done_at_[slot_] = value_;
	return value_;
}

void SideQueueLists::release() noexcept
{
	const bool done = fence_ == nullptr || value_ == 0 || wait_fence(fence_, value_, event_, 5000);
	for (ID3D12CommandAllocator *&a : alloc_) {
		if (a != nullptr && done)
			a->Release();
		a = nullptr;
	}
	for (uint64_t &d : done_at_)
		d = 0;
	if (list_ != nullptr)
		list_->Release();
	if (fence_ != nullptr)
		fence_->Release();
	if (queue_ != nullptr)
		queue_->Release();
	if (event_ != nullptr)
		CloseHandle(event_);
	list_ = nullptr;
	fence_ = nullptr;
	queue_ = nullptr;
	event_ = nullptr;
	device_ = nullptr;
	value_ = 0;
	slot_ = 0;
	open_ = false;
}

}
