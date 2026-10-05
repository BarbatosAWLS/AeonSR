#include "aeon_sr/upscalers/fsr_warmup.hpp"

#include <dxgi1_4.h>
#include <excpt.h>

#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>

#include <atomic>
#include <memory>
#include <cstdio>
#include <mutex>
#include <vector>

namespace aeon_sr {
namespace {

constexpr D3D12_RESOURCE_STATES kReadState =
	D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

constexpr DWORD kFenceTimeoutMs = 120000;

constexpr uint64_t kMaxCacheBytes = 1ull << 30;

double ms_between(const LARGE_INTEGER &freq, const LARGE_INTEGER &a, const LARGE_INTEGER &b)
{
	return freq.QuadPart == 0 ? 0.0
		: 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(freq.QuadPart);
}

ID3D12Resource *make_texture(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
	bool uav, D3D12_RESOURCE_STATES state)
{
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = fmt;
	d.SampleDesc.Count = 1;
	d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
	ID3D12Resource *res = nullptr;
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr,
			IID_PPV_ARGS(&res))))
		return nullptr;
	return res;
}

ffxReturnCode_t dispatch_guarded(PfnFfxDispatch fn, ffxContext *ctx,
	const ffxDispatchDescHeader *desc, unsigned long *seh_code)
{
	__try {
		return fn(ctx, desc);
	} __except ((*seh_code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return FFX_API_RETURN_ERROR;
	}
}

template <typename T>
void release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

bool read_file(const std::wstring &path, std::vector<uint8_t> &out)
{
	out.clear();
	const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	LARGE_INTEGER size{};
	bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 &&
		static_cast<uint64_t>(size.QuadPart) <= kMaxCacheBytes;
	if (ok) {
		out.resize(static_cast<size_t>(size.QuadPart));
		DWORD got = 0;
		ok = ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
	}
	CloseHandle(f);
	if (!ok)
		out.clear();
	return ok;
}

bool write_file_atomic(const std::wstring &path, const void *data, size_t bytes)
{
	wchar_t suffix[32]{};
	_snwprintf_s(suffix, _TRUNCATE, L".%lu.tmp", GetCurrentProcessId());
	const std::wstring tmp = path + suffix;
	const HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return false;
	DWORD put = 0;
	const bool wrote = WriteFile(f, data, static_cast<DWORD>(bytes), &put, nullptr) && put == bytes;
	CloseHandle(f);
	if (!wrote || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	return true;
}

constexpr unsigned kNameVariants = 4;

void pipeline_name(const D3D12_COMPUTE_PIPELINE_STATE_DESC &d, unsigned variant, wchar_t (&out)[56])
{
	uint64_t h = 1469598103934665603ull;
	const auto *p = static_cast<const uint8_t *>(d.CS.pShaderBytecode);
	for (size_t i = 0; i < d.CS.BytecodeLength; ++i) {
		h ^= p[i];
		h *= 1099511628211ull;
	}
	_snwprintf_s(out, _TRUNCATE, L"fsr.%016llx.%zx.%x.%x.%u", static_cast<unsigned long long>(h),
		d.CS.BytecodeLength, d.NodeMask, static_cast<unsigned>(d.Flags), variant);
}

using PfnCreateCompute = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *,
	const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **);

constexpr size_t kSlotCreateCompute = 11;

std::mutex g_hook_mutex;
std::atomic<DWORD> g_hook_thread{ 0 };
void **g_slot = nullptr;
std::atomic<PfnCreateCompute> g_real_compute{ nullptr };
FsrPipelineCache *g_cache = nullptr;

D3D12_SHADER_BYTECODE trivial_cs()
{
	static std::once_flag once;
	static std::vector<uint8_t> bytes;
	std::call_once(once, []() {
		const HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (m == nullptr)
			return;
		using PfnCompile = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
			ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
		const auto compile = reinterpret_cast<PfnCompile>(GetProcAddress(m, "D3DCompile"));
		if (compile == nullptr)
			return;
		static const char kSource[] = "[numthreads(1, 1, 1)] void main() {}";
		ID3DBlob *blob = nullptr, *errors = nullptr;
		if (SUCCEEDED(compile(kSource, sizeof(kSource) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0",
				0, 0, &blob, &errors)) && blob != nullptr) {
			const auto *p = static_cast<const uint8_t *>(blob->GetBufferPointer());
			bytes.assign(p, p + blob->GetBufferSize());
		}
		if (blob != nullptr)
			blob->Release();
		if (errors != nullptr)
			errors->Release();
	});
	if (bytes.empty())
		return {};
	return { bytes.data(), bytes.size() };
}

ID3D12PipelineState *standin_for(FsrPipelineCache &c, ID3D12Device *self, PfnCreateCompute real,
	const D3D12_COMPUTE_PIPELINE_STATE_DESC &d)
{
	for (const auto &s : c.standins) {
		if (s.first == d.pRootSignature)
			return s.second;
	}
	const D3D12_SHADER_BYTECODE cs = trivial_cs();
	if (cs.pShaderBytecode == nullptr)
		return nullptr;
	D3D12_COMPUTE_PIPELINE_STATE_DESC sd{};
	sd.pRootSignature = d.pRootSignature;
	sd.CS = cs;
	sd.NodeMask = d.NodeMask;
	ID3D12PipelineState *pso = nullptr;
	if (FAILED(real(self, &sd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&pso))) ||
		pso == nullptr)
		return nullptr;
	d.pRootSignature->AddRef();
	c.standins.emplace_back(d.pRootSignature, pso);
	return pso;
}

HRESULT STDMETHODCALLTYPE cached_compute(ID3D12Device *self,
	const D3D12_COMPUTE_PIPELINE_STATE_DESC *d, REFIID riid, void **out)
{
	const PfnCreateCompute real = g_real_compute.load(std::memory_order_acquire);
	if (g_hook_thread.load(std::memory_order_acquire) != GetCurrentThreadId() || g_cache == nullptr)
		return real(self, d, riid, out);

	FsrPipelineCache &c = *g_cache;
	++c.st.requested;
	if (c.lib == nullptr || d == nullptr || out == nullptr || d->CS.pShaderBytecode == nullptr ||
		d->CS.BytecodeLength == 0 || d->CachedPSO.pCachedBlob != nullptr) {
		++c.st.compiled;
		return real(self, d, riid, out);
	}

	wchar_t name[56]{};
	for (unsigned v = 0; v < kNameVariants; ++v) {
		pipeline_name(*d, v, name);
		if (SUCCEEDED(c.lib->LoadComputePipeline(name, d, riid, out))) {
			++c.st.loaded;
			return S_OK;
		}
	}

	if (c.load_only) {
		++c.st.missed;
		ID3D12PipelineState *const standin =
			d->pRootSignature != nullptr ? standin_for(c, self, real, *d) : nullptr;
		if (standin != nullptr)
			return standin->QueryInterface(riid, out);
		*out = nullptr;
		return E_FAIL;
	}

	const HRESULT hr = real(self, d, riid, out);
	if (FAILED(hr) || *out == nullptr)
		return hr;
	++c.st.compiled;
	ID3D12PipelineState *pso = nullptr;
	if (SUCCEEDED(static_cast<IUnknown *>(*out)->QueryInterface(IID_PPV_ARGS(&pso)))) {
		for (unsigned v = 0; v < kNameVariants; ++v) {
			pipeline_name(*d, v, name);
			if (SUCCEEDED(c.lib->StorePipeline(name, pso))) {
				++c.st.stored;
				break;
			}
		}
		pso->Release();
	}
	return hr;
}

bool write_slot(void **slot, void *value)
{
	DWORD old = 0;
	if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old))
		return false;
	InterlockedExchangePointer(slot, value);
	VirtualProtect(slot, sizeof(void *), old, &old);
	return true;
}

class CacheHook {
public:
	CacheHook(ID3D12Device *device, FsrPipelineCache &cache)
		: lock_(g_hook_mutex), slot_(&(*reinterpret_cast<void ***>(device))[kSlotCreateCompute])
	{
		if (g_slot == nullptr) {
			g_slot = slot_;
			g_real_compute.store(reinterpret_cast<PfnCreateCompute>(*slot_), std::memory_order_release);
		}
		if (slot_ != g_slot || *slot_ != reinterpret_cast<void *>(g_real_compute.load()))
			return;
		g_cache = &cache;
		g_hook_thread.store(GetCurrentThreadId(), std::memory_order_release);
		active_ = write_slot(slot_, reinterpret_cast<void *>(&cached_compute));
		cache.st.hooked = active_;
		if (!active_) {
			g_hook_thread.store(0, std::memory_order_release);
			g_cache = nullptr;
		}
	}
	~CacheHook()
	{
		if (!active_)
			return;
		g_hook_thread.store(0, std::memory_order_release);
		g_cache = nullptr;
		if (*slot_ == reinterpret_cast<void *>(&cached_compute)) {
			write_slot(slot_, reinterpret_cast<void *>(g_real_compute.load()));
		} else {
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				reinterpret_cast<LPCWSTR>(&cached_compute), &self);
		}
	}
	CacheHook(const CacheHook &) = delete;
	CacheHook &operator=(const CacheHook &) = delete;
	bool active() const noexcept { return active_; }

private:
	std::lock_guard<std::mutex> lock_;
	void **slot_;
	bool active_ = false;
};

ffxReturnCode_t record_two(PfnFfxDispatch fn, ffxContext *context, ffxDispatchDescUpscale &dispatch,
	LARGE_INTEGER *t, unsigned long *seh_code, bool *recorded)
{
	QueryPerformanceCounter(&t[0]);
	dispatch.reset = true;
	ffxReturnCode_t r = dispatch_guarded(fn, context, &dispatch.header, seh_code);
	QueryPerformanceCounter(&t[1]);
	if (*seh_code == 0 && r == FFX_API_RETURN_OK) {
		*recorded = true;
		dispatch.reset = false;
		r = dispatch_guarded(fn, context, &dispatch.header, seh_code);
	}
	QueryPerformanceCounter(&t[2]);
	return r;
}

}

FsrPipelineCache::~FsrPipelineCache()
{
	release_standins();
	release(lib);
}

void FsrPipelineCache::release_standins()
{
	for (auto &s : standins) {
		s.second->Release();
		s.first->Release();
	}
	standins.clear();
}

void FsrPipelineCache::open(ID3D12Device *device, const std::wstring &file)
{
	release_standins();
	release(lib);
	blob.clear();
	st = {};
	opened_size = 0;
	path = file;
	if (device == nullptr || path.empty())
		return;
	LARGE_INTEGER freq{}, t0{}, t1{};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	ID3D12Device1 *d1 = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d1)))) {
		if (read_file(path, blob)) {
			if (SUCCEEDED(d1->CreatePipelineLibrary(blob.data(), blob.size(), IID_PPV_ARGS(&lib)))) {
				st.file_read = true;
				st.file_bytes = blob.size();
			} else {
				st.file_stale = true;
				blob.clear();
			}
		}
		if (lib == nullptr)
			d1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&lib));
		d1->Release();
		if (lib != nullptr)
			opened_size = lib->GetSerializedSize();
	}
	QueryPerformanceCounter(&t1);
	st.open_ms = ms_between(freq, t0, t1);
}

void FsrPipelineCache::save()
{
	if (lib == nullptr || path.empty())
		return;
	if (st.stored == 0 && lib->GetSerializedSize() <= opened_size)
		return;
	LARGE_INTEGER freq{}, t0{}, t1{};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	const SIZE_T size = lib->GetSerializedSize();
	if (size > 0 && size <= kMaxCacheBytes) {
		std::vector<uint8_t> out(size);
		if (SUCCEEDED(lib->Serialize(out.data(), size))) {
			st.saved = write_file_atomic(path, out.data(), out.size());
			if (st.saved)
				st.file_bytes = size;
		}
	}
	QueryPerformanceCounter(&t1);
	st.save_ms = ms_between(freq, t0, t1);
}

std::wstring pipeline_cache_path(const std::wstring &dir, ID3D12Device *device, const wchar_t *prefix)
{
	if (dir.empty() || device == nullptr)
		return {};
	const LUID luid = device->GetAdapterLuid();
	IDXGIFactory4 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return {};
	IDXGIAdapter1 *adapter = nullptr;
	DXGI_ADAPTER_DESC1 ad{};
	const bool named = SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) &&
		SUCCEEDED(adapter->GetDesc1(&ad));
	if (adapter != nullptr)
		adapter->Release();
	factory->Release();
	if (!named)
		return {};
	wchar_t name[64]{};
	_snwprintf_s(name, _TRUNCATE, L"%s_pipelines_%04x_%04x.bin", prefix, ad.VendorId, ad.DeviceId);
	std::wstring path = dir;
	if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
		path += L'\\';
	return path + name;
}

bool fsr_warmup_dispatch(const FfxApi &api, ffxContext *context, ID3D12Device *device,
	const FsrWarmupDesc &desc, FsrPipelineCache *cache, FsrWarmupTiming *timing,
	unsigned long *seh_code, bool *context_dirty, std::wstring &error)
{
	*seh_code = 0;
	if (context_dirty != nullptr)
		*context_dirty = false;
	bool recorded = false;
	bool finished = false;
	if (api.Dispatch == nullptr || context == nullptr || *context == nullptr || device == nullptr ||
		desc.render_width == 0 || desc.render_height == 0 ||
		desc.upscale_width == 0 || desc.upscale_height == 0) {
		error = L"FSR warm-up: nothing to warm";
		return false;
	}

	LARGE_INTEGER freq{};
	QueryPerformanceFrequency(&freq);

	ID3D12CommandQueue *queue = nullptr;
	ID3D12CommandAllocator *alloc = nullptr;
	ID3D12GraphicsCommandList *list = nullptr;
	ID3D12Fence *fence = nullptr;
	HANDLE event = nullptr;
	ID3D12Resource *color = nullptr, *depth = nullptr, *motion = nullptr, *output = nullptr;
	bool ok = false;
	bool gpu_may_hold = false;

	do {
		D3D12_COMMAND_QUEUE_DESC qd{};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) ||
			FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
			FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
				IID_PPV_ARGS(&list))) ||
			FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
			error = L"FSR warm-up: could not create a command queue";
			break;
		}
		event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (event == nullptr) {
			error = L"FSR warm-up: could not create an event";
			break;
		}

		color = make_texture(device, desc.render_width, desc.render_height, desc.color_format, false, kReadState);
		motion = make_texture(device, desc.upscale_width, desc.upscale_height, DXGI_FORMAT_R16G16_FLOAT, false, kReadState);
		output = make_texture(device, desc.upscale_width, desc.upscale_height, desc.color_format, true,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		if (desc.with_depth)
			depth = make_texture(device, desc.render_width, desc.render_height, DXGI_FORMAT_R32_FLOAT, false, kReadState);
		if (color == nullptr || motion == nullptr || output == nullptr || (desc.with_depth && depth == nullptr)) {
			error = L"FSR warm-up: could not create its textures";
			break;
		}

		ffxDispatchDescUpscale dispatch{};
		dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
		dispatch.commandList = list;
		dispatch.color = ffxApiGetResourceDX12(color, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatch.depth = depth != nullptr
			? ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ) : FfxApiResource{};
		dispatch.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatch.output = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
		dispatch.motionVectorScale.x = static_cast<float>(desc.upscale_width);
		dispatch.motionVectorScale.y = static_cast<float>(desc.upscale_height);
		dispatch.renderSize.width = desc.render_width;
		dispatch.renderSize.height = desc.render_height;
		dispatch.upscaleSize.width = desc.upscale_width;
		dispatch.upscaleSize.height = desc.upscale_height;
		dispatch.frameTimeDelta = 16.7f;
		dispatch.preExposure = 1.0f;
		dispatch.viewSpaceToMetersFactor = 1.0f;
		dispatch.flags = desc.dispatch_flags;

		LARGE_INTEGER t[4]{};
		ffxReturnCode_t r = FFX_API_RETURN_ERROR;
		bool unserved = false;
		{
			std::unique_ptr<CacheHook> hook;
			if (cache != nullptr && cache->lib != nullptr)
				hook = std::make_unique<CacheHook>(device, *cache);
			unserved = cache != nullptr && cache->load_only && (hook == nullptr || !hook->active());
			if (!unserved)
				r = record_two(api.Dispatch, context, dispatch, t, seh_code, &recorded);
		}
		if (unserved) {
			error = L"FSR warm-up: the shared pipeline cache could not be used here, so nothing was warmed";
			break;
		}
		if (*seh_code != 0) {
			wchar_t buf[128]{};
			_snwprintf_s(buf, _TRUNCATE, L"FSR warm-up: ffxDispatch CRASHED (exception 0x%08lX)", *seh_code);
			error = buf;
			break;
		}
		if (r != FFX_API_RETURN_OK) {
			wchar_t buf[128]{};
			_snwprintf_s(buf, _TRUNCATE, L"FSR warm-up: ffxDispatch failed (0x%08X)", static_cast<unsigned>(r));
			error = buf;
			break;
		}
		if (FAILED(list->Close())) {
			error = L"FSR warm-up: the command list would not close";
			break;
		}

		ID3D12CommandList *lists[] = { list };
		queue->ExecuteCommandLists(1, lists);
		gpu_may_hold = true;
		if (FAILED(queue->Signal(fence, 1)) || FAILED(fence->SetEventOnCompletion(1, event))) {
			error = L"FSR warm-up: could not signal its fence";
			break;
		}
		if (WaitForSingleObject(event, kFenceTimeoutMs) != WAIT_OBJECT_0) {
			error = L"FSR warm-up: the GPU did not finish in time";
			break;
		}
		gpu_may_hold = false;
		finished = true;
		QueryPerformanceCounter(&t[3]);

		if (timing != nullptr) {
			timing->first_record_ms = ms_between(freq, t[0], t[1]);
			timing->second_record_ms = ms_between(freq, t[1], t[2]);
			timing->gpu_ms = ms_between(freq, t[2], t[3]);
		}
		if (device->GetDeviceRemovedReason() != S_OK) {
			error = L"FSR warm-up: the device was removed";
			break;
		}
		ok = true;
	} while (false);

	if (ok && cache != nullptr)
		cache->save();
	if (context_dirty != nullptr)
		*context_dirty = recorded && !finished && *seh_code == 0;

	if (gpu_may_hold)
		return false;

	release(color);
	release(depth);
	release(motion);
	release(output);
	release(list);
	release(alloc);
	release(fence);
	release(queue);
	if (event != nullptr)
		CloseHandle(event);
	return ok;
}

}
