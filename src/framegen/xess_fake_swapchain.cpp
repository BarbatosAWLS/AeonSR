#include "aeon_sr/framegen/xess_fake_swapchain.hpp"
#include "aeon_sr/framegen/vendor_frame_gen_impl.hpp"

#include "aeon_sr/interop/blit_d3d12.hpp"

#include <mutex>
#include <vector>

namespace aeon_sr {

namespace {

template <typename T>
void release(T *&p) noexcept
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

bool is_factory_iid(REFIID riid)
{
	return riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIFactory) ||
		riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory3) ||
		riid == __uuidof(IDXGIFactory4) || riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6) ||
		riid == __uuidof(IDXGIFactory7);
}

bool is_swapchain_iid(REFIID riid)
{
	return riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
		riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
		riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4);
}

class CaptureSwapChain final : public IDXGISwapChain4 {
public:
	CaptureSwapChain(IDXGIFactory7 *real_factory, IDXGIFactory2 *parent, ID3D12CommandQueue *queue, HWND hwnd,
		const DXGI_SWAP_CHAIN_DESC1 &desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs,
		std::shared_ptr<XefgCaptures> captures)
		: real_factory_(real_factory), parent_(parent), queue_(queue), hwnd_(hwnd), desc_(desc),
		  captures_(std::move(captures))
	{
		real_factory_->AddRef();
		queue_->AddRef();
		queue_->GetDevice(IID_PPV_ARGS(&device_));
		if (fs != nullptr)
			fs_ = *fs;
		fs_.Windowed = TRUE;
		latency_ = CreateSemaphoreW(nullptr, 3, 1 << 20, nullptr);
	}

	~CaptureSwapChain()
	{
		lists_.release();
		for (ID3D12Resource *&b : buffers_)
			release(b);
		for (ID3D12CommandQueue *&q : buffer_queues_)
			release(q);
		release(device_);
		release(queue_);
		release(real_factory_);
		if (latency_ != nullptr)
			CloseHandle(latency_);
	}

	bool make_buffers()
	{
		for (ID3D12Resource *&b : buffers_)
			release(b);
		buffers_.assign(desc_.BufferCount, nullptr);
		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC rd{};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rd.Width = desc_.Width;
		rd.Height = desc_.Height;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.Format = desc_.Format;
		rd.SampleDesc.Count = 1;
		rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		if ((desc_.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS) != 0)
			rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		for (ID3D12Resource *&b : buffers_)
			if (device_ == nullptr || FAILED(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
					D3D12_RESOURCE_STATE_PRESENT, nullptr, IID_PPV_ARGS(&b))))
				return false;
		current_ = 0;
		return true;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
	{
		if (out == nullptr)
			return E_POINTER;
		*out = nullptr;
		if (!is_swapchain_iid(riid))
			return E_NOINTERFACE;
		*out = static_cast<IDXGISwapChain4 *>(this);
		AddRef();
		return S_OK;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG r = --refs_;
		if (r == 0)
			delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *size, void *) override
	{
		if (size != nullptr)
			*size = 0;
		return DXGI_ERROR_NOT_FOUND;
	}
	HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override { return parent_->QueryInterface(riid, parent); }

	HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override { return queue_->QueryInterface(riid, device); }

	HRESULT STDMETHODCALLTYPE Present(UINT, UINT flags) override { return present(flags); }
	HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer, REFIID riid, void **surface) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (surface == nullptr)
			return E_POINTER;
		if (buffer >= buffers_.size() || buffers_[buffer] == nullptr)
			return DXGI_ERROR_INVALID_CALL;
		return buffers_[buffer]->QueryInterface(riid, surface);
	}
	HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput *) override
	{
		return fullscreen ? DXGI_ERROR_NOT_CURRENTLY_AVAILABLE : S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *fullscreen, IDXGIOutput **target) override
	{
		if (fullscreen != nullptr)
			*fullscreen = FALSE;
		if (target != nullptr)
			*target = nullptr;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *d) override
	{
		if (d == nullptr)
			return E_POINTER;
		*d = {};
		d->BufferDesc.Width = desc_.Width;
		d->BufferDesc.Height = desc_.Height;
		d->BufferDesc.Format = desc_.Format;
		d->SampleDesc = desc_.SampleDesc;
		d->BufferUsage = desc_.BufferUsage;
		d->BufferCount = desc_.BufferCount;
		d->OutputWindow = hwnd_;
		d->Windowed = TRUE;
		d->SwapEffect = desc_.SwapEffect;
		d->Flags = desc_.Flags;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) override
	{
		return ResizeBuffers1(count, w, h, fmt, flags, nullptr, nullptr);
	}
	HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **output) override
	{
		if (output == nullptr)
			return E_POINTER;
		*output = nullptr;
		const HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
		IDXGIAdapter1 *adapter = nullptr;
		for (UINT a = 0; real_factory_->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
			IDXGIOutput *o = nullptr;
			for (UINT i = 0; adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i) {
				DXGI_OUTPUT_DESC od{};
				if (SUCCEEDED(o->GetDesc(&od)) && od.Monitor == mon) {
					*output = o;
					adapter->Release();
					return S_OK;
				}
				o->Release();
			}
			adapter->Release();
		}
		return DXGI_ERROR_NOT_FOUND;
	}
	HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) override
	{
		if (stats == nullptr)
			return E_POINTER;
		std::lock_guard<std::mutex> lock(mutex_);
		if (presents_ == 0)
			return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
		*stats = {};
		stats->PresentCount = static_cast<UINT>(presents_);
		stats->PresentRefreshCount = static_cast<UINT>(presents_);
		stats->SyncRefreshCount = static_cast<UINT>(presents_);
		stats->SyncQPCTime = last_qpc_;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *count) override
	{
		if (count == nullptr)
			return E_POINTER;
		std::lock_guard<std::mutex> lock(mutex_);
		*count = static_cast<UINT>(presents_);
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *d) override
	{
		if (d == nullptr)
			return E_POINTER;
		*d = desc_;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *d) override
	{
		if (d == nullptr)
			return E_POINTER;
		*d = fs_;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetHwnd(HWND *hwnd) override
	{
		if (hwnd == nullptr)
			return E_POINTER;
		*hwnd = hwnd_;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID, void **unk) override
	{
		if (unk != nullptr)
			*unk = nullptr;
		return DXGI_ERROR_INVALID_CALL;
	}
	HRESULT STDMETHODCALLTYPE Present1(UINT, UINT flags, const DXGI_PRESENT_PARAMETERS *) override
	{
		return present(flags);
	}
	BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }
	HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **output) override
	{
		if (output != nullptr)
			*output = nullptr;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *color) override
	{
		if (color != nullptr)
			*color = {};
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *rotation) override
	{
		if (rotation != nullptr)
			*rotation = DXGI_MODE_ROTATION_IDENTITY;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE SetSourceSize(UINT, UINT) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *w, UINT *h) override
	{
		if (w != nullptr)
			*w = desc_.Width;
		if (h != nullptr)
			*h = desc_.Height;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override
	{
		max_latency_ = latency;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *latency) override
	{
		if (latency != nullptr)
			*latency = max_latency_;
		return S_OK;
	}
	HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override
	{
		HANDLE dup = nullptr;
		DuplicateHandle(GetCurrentProcess(), latency_, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
		return dup;
	}
	HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *m) override
	{
		if (m != nullptr) {
			*m = {};
			m->_11 = m->_22 = 1.0f;
		}
		return S_OK;
	}

	UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return current_;
	}
	HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE, UINT *support) override
	{
		if (support != nullptr)
			*support = DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
		const UINT *, IUnknown *const *queues) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (count != 0)
			desc_.BufferCount = count;
		if (w != 0)
			desc_.Width = w;
		if (h != 0)
			desc_.Height = h;
		if (fmt != DXGI_FORMAT_UNKNOWN)
			desc_.Format = fmt;
		desc_.Flags = flags;
		for (ID3D12CommandQueue *&q : buffer_queues_)
			release(q);
		buffer_queues_.clear();
		if (queues != nullptr)
			for (UINT i = 0; i < desc_.BufferCount; ++i) {
				ID3D12CommandQueue *q = nullptr;
				if (queues[i] != nullptr)
					queues[i]->QueryInterface(IID_PPV_ARGS(&q));
				buffer_queues_.push_back(q);
			}
		return make_buffers() ? S_OK : E_OUTOFMEMORY;
	}

	HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE, UINT, void *) override { return S_OK; }

private:
	ID3D12CommandQueue *queue_for(UINT index) const
	{
		if (index < buffer_queues_.size() && buffer_queues_[index] != nullptr)
			return buffer_queues_[index];
		return queue_;
	}

	HRESULT present(UINT flags)
	{
		if ((flags & DXGI_PRESENT_TEST) != 0)
			return S_OK;
		std::lock_guard<std::mutex> lock(mutex_);
		XefgCaptures &c = *captures_;
		const UINT index = current_;
		ID3D12CommandQueue *const q = queue_for(index);
		const uint64_t n = presents_ + 1;
		ID3D12Resource *const slot = c.slot[n % XefgCaptures::kRing];
		ID3D12GraphicsCommandList *cmd = nullptr;
		if (index < buffers_.size() && buffers_[index] != nullptr && slot != nullptr && lists_.ensure(device_, q))
			cmd = lists_.begin();
		if (cmd == nullptr) {
			c.broken.store(true);
			SetEvent(c.event);
			return DXGI_ERROR_DEVICE_REMOVED;
		}
		if (n > XefgCaptures::kRing)
			q->Wait(c.consumed, n - XefgCaptures::kRing);
		barrier12(cmd, buffers_[index], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier12(cmd, slot, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		cmd->CopyResource(slot, buffers_[index]);
		barrier12(cmd, slot, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		barrier12(cmd, buffers_[index], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
		if (lists_.submit() == 0 || FAILED(q->Signal(c.captured, n))) {
			c.broken.store(true);
			SetEvent(c.event);
			return DXGI_ERROR_DEVICE_REMOVED;
		}
		presents_ = n;
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		last_qpc_ = now;
		current_ = desc_.BufferCount != 0 ? (current_ + 1) % desc_.BufferCount : 0;
		c.presents.store(n, std::memory_order_release);
		SetEvent(c.event);
		ReleaseSemaphore(latency_, 1, nullptr);
		return S_OK;
	}

	std::atomic<ULONG> refs_{ 1 };
	IDXGIFactory7 *real_factory_ = nullptr;
	IDXGIFactory2 *parent_ = nullptr;
	ID3D12CommandQueue *queue_ = nullptr;
	ID3D12Device *device_ = nullptr;
	HWND hwnd_ = nullptr;
	DXGI_SWAP_CHAIN_DESC1 desc_{};
	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_{};
	std::shared_ptr<XefgCaptures> captures_;
	std::mutex mutex_;
	std::vector<ID3D12Resource *> buffers_;
	std::vector<ID3D12CommandQueue *> buffer_queues_;
	SideQueueLists lists_;
	UINT current_ = 0;
	uint64_t presents_ = 0;
	LARGE_INTEGER last_qpc_{};
	UINT max_latency_ = 3;
	HANDLE latency_ = nullptr;
};

class CaptureFactory final : public IDXGIFactory7 {
public:
	CaptureFactory(IDXGIFactory7 *real, std::shared_ptr<XefgCaptures> captures)
		: real_(real), captures_(std::move(captures))
	{
		real_->AddRef();
	}
	~CaptureFactory() { release(real_); }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
	{
		if (out == nullptr)
			return E_POINTER;
		*out = nullptr;
		if (!is_factory_iid(riid))
			return E_NOINTERFACE;
		*out = static_cast<IDXGIFactory7 *>(this);
		AddRef();
		return S_OK;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG r = --refs_;
		if (r == 0)
			delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void *data) override
	{
		return real_->SetPrivateData(name, size, data);
	}
	HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown *unknown) override
	{
		return real_->SetPrivateDataInterface(name, unknown);
	}
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT *size, void *data) override
	{
		return real_->GetPrivateData(name, size, data);
	}
	HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override { return real_->GetParent(riid, parent); }

	HRESULT STDMETHODCALLTYPE EnumAdapters(UINT i, IDXGIAdapter **adapter) override
	{
		return real_->EnumAdapters(i, adapter);
	}
	HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND, UINT) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *hwnd) override
	{
		if (hwnd != nullptr)
			*hwnd = nullptr;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc,
		IDXGISwapChain **swapchain) override
	{
		if (desc == nullptr || swapchain == nullptr)
			return E_INVALIDARG;
		DXGI_SWAP_CHAIN_DESC1 d1{};
		d1.Width = desc->BufferDesc.Width;
		d1.Height = desc->BufferDesc.Height;
		d1.Format = desc->BufferDesc.Format;
		d1.SampleDesc = desc->SampleDesc;
		d1.BufferUsage = desc->BufferUsage;
		d1.BufferCount = desc->BufferCount;
		d1.SwapEffect = desc->SwapEffect;
		d1.Flags = desc->Flags;
		IDXGISwapChain1 *sc = nullptr;
		const HRESULT hr = CreateSwapChainForHwnd(device, desc->OutputWindow, &d1, nullptr, nullptr, &sc);
		*swapchain = sc;
		return hr;
	}
	HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE module, IDXGIAdapter **adapter) override
	{
		return real_->CreateSoftwareAdapter(module, adapter);
	}

	HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT i, IDXGIAdapter1 **adapter) override
	{
		return real_->EnumAdapters1(i, adapter);
	}
	BOOL STDMETHODCALLTYPE IsCurrent() override { return real_->IsCurrent(); }

	BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return FALSE; }
	HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown *device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
		const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *, IDXGISwapChain1 **swapchain) override
	{
		if (swapchain == nullptr || desc == nullptr || device == nullptr)
			return E_INVALIDARG;
		*swapchain = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue))) || queue == nullptr)
			return DXGI_ERROR_INVALID_CALL;
		auto *sc = new CaptureSwapChain(real_, this, queue, hwnd, *desc, fs, captures_);
		queue->Release();
		if (!sc->make_buffers()) {
			sc->Release();
			return E_OUTOFMEMORY;
		}
		*swapchain = sc;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown *, IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *,
		IDXGIOutput *, IDXGISwapChain1 **swapchain) override
	{
		if (swapchain != nullptr)
			*swapchain = nullptr;
		return DXGI_ERROR_UNSUPPORTED;
	}
	HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE resource, LUID *luid) override
	{
		return real_->GetSharedResourceAdapterLuid(resource, luid);
	}
	HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND, UINT, DWORD *cookie) override
	{
		if (cookie != nullptr)
			*cookie = 0;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE, DWORD *cookie) override
	{
		if (cookie != nullptr)
			*cookie = 0;
		return S_OK;
	}
	void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD) override {}
	HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND, UINT, DWORD *cookie) override
	{
		if (cookie != nullptr)
			*cookie = 0;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE, DWORD *cookie) override
	{
		if (cookie != nullptr)
			*cookie = 0;
		return S_OK;
	}
	void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD) override {}
	HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown *, const DXGI_SWAP_CHAIN_DESC1 *, IDXGIOutput *,
		IDXGISwapChain1 **swapchain) override
	{
		if (swapchain != nullptr)
			*swapchain = nullptr;
		return DXGI_ERROR_UNSUPPORTED;
	}

	UINT STDMETHODCALLTYPE GetCreationFlags() override { return real_->GetCreationFlags(); }

	HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID riid, void **adapter) override
	{
		return real_->EnumAdapterByLuid(luid, riid, adapter);
	}
	HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void **adapter) override
	{
		return real_->EnumWarpAdapter(riid, adapter);
	}

	HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE feature, void *data, UINT size) override
	{
		return real_->CheckFeatureSupport(feature, data, size);
	}

	HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT i, DXGI_GPU_PREFERENCE pref, REFIID riid,
		void **adapter) override
	{
		return real_->EnumAdapterByGpuPreference(i, pref, riid, adapter);
	}

	HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE ev, DWORD *cookie) override
	{
		return real_->RegisterAdaptersChangedEvent(ev, cookie);
	}
	HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD cookie) override
	{
		return real_->UnregisterAdaptersChangedEvent(cookie);
	}

private:
	std::atomic<ULONG> refs_{ 1 };
	IDXGIFactory7 *real_ = nullptr;
	std::shared_ptr<XefgCaptures> captures_;
};

}

XefgCaptures::~XefgCaptures()
{
	for (ID3D12Resource *&s : slot)
		release(s);
	release(captured);
	release(consumed);
	if (event != nullptr)
		CloseHandle(event);
}

bool XefgCaptures::create(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT format)
{
	for (ID3D12Resource *&s : slot)
		if (!create_tex12(device, w, h, format, false, D3D12_RESOURCE_STATE_COMMON, &s))
			return false;
	event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	return event != nullptr && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&captured))) &&
		SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumed)));
}

IDXGIFactory2 *make_xefg_capture_factory(IDXGIFactory7 *real, std::shared_ptr<XefgCaptures> captures)
{
	return new CaptureFactory(real, std::move(captures));
}

}
