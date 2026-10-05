#include "aeon_sr/interop/interop.hpp"
#include "aeon_sr/interop/interop_bridges.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/interop/gpu12.hpp"

#include <d3d9.h>
#include <dxgi1_6.h>

namespace aeon_sr {
namespace {

bool d3d9_adapter_luid(reshade::api::device *game, LUID *out)
{
	auto *const dev = reinterpret_cast<IDirect3DDevice9 *>(static_cast<uintptr_t>(game->get_native()));
	D3DDEVICE_CREATION_PARAMETERS cp{};
	if (dev == nullptr || FAILED(dev->GetCreationParameters(&cp)))
		return false;
	wchar_t path[MAX_PATH + 16]{};
	const UINT n = GetSystemDirectoryW(path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return false;
	wcscat_s(path, L"\\d3d9.dll");
	const HMODULE module = LoadLibraryW(path);
	if (module == nullptr)
		return false;
	typedef HRESULT(WINAPI * PfnCreate9Ex)(UINT, IDirect3D9Ex **);
	const auto create = reinterpret_cast<PfnCreate9Ex>(GetProcAddress(module, "Direct3DCreate9Ex"));
	IDirect3D9Ex *ex = nullptr;
	const bool ok = create != nullptr && SUCCEEDED(create(D3D_SDK_VERSION, &ex)) && ex != nullptr &&
		SUCCEEDED(ex->GetAdapterLUID(cp.AdapterOrdinal, out));
	if (ex != nullptr)
		ex->Release();
	FreeLibrary(module);
	return ok;
}

template <typename T>
void safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

}

void retire_bridge(FrameBridge *&bridge, BridgeRelease how)
{
	if (bridge == nullptr)
		return;
	bridge->device_destroying = how == BridgeRelease::GameDeviceGone;
	bridge->shutdown();
	delete bridge;
	bridge = nullptr;
}

std::optional<BridgeRelease> bridge_release_on_destroy(const reshade::api::device *destroyed,
	const reshade::api::device *bridge_device, bool destroyed_is_engine) noexcept
{
	if (destroyed == nullptr || destroyed_is_engine)
		return std::nullopt;
	if (bridge_device != nullptr && bridge_device != destroyed)
		return std::nullopt;
	return BridgeRelease::GameDeviceGone;
}

bool adopt_on_init(reshade::api::device_api api, const reshade::api::device *bridge_device) noexcept
{
	return api != reshade::api::device_api::vulkan && bridge_device == nullptr;
}

void PendingDevices::add(const reshade::api::device *device)
{
	if (device == nullptr)
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const reshade::api::device *d : devices_)
		if (d == device)
			return;
	devices_.push_back(device);
}

void PendingDevices::remove(const reshade::api::device *device)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (size_t i = 0; i < devices_.size(); ++i) {
		if (devices_[i] == device) {
			devices_.erase(devices_.begin() + static_cast<std::ptrdiff_t>(i));
			return;
		}
	}
}

bool PendingDevices::take(const reshade::api::device *device)
{
	if (device == nullptr)
		return false;
	std::lock_guard<std::mutex> lock(mutex_);
	for (size_t i = 0; i < devices_.size(); ++i) {
		if (devices_[i] == device) {
			devices_.erase(devices_.begin() + static_cast<std::ptrdiff_t>(i));
			return true;
		}
	}
	return false;
}

size_t PendingDevices::size()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return devices_.size();
}

const char *bridge_kind_label(BridgeKind kind) noexcept
{
	switch (kind) {
	case BridgeKind::None: return "none";
	case BridgeKind::Native12: return "native";
	case BridgeKind::Shared11: return "shared texture";
	case BridgeKind::Legacy10: return "two-hop shared texture";
	case BridgeKind::Legacy9: return "two-hop shared texture";
	case BridgeKind::OpenGl: return "OpenGL interop";
	case BridgeKind::Vulkan: return "Vulkan external memory";
	case BridgeKind::Staging: return "system memory";
	}
	return "unknown";
}

const char *bridge_step_label(BridgeStep step) noexcept
{
	switch (step) {
	case BridgeStep::Ok: return "ok";
	case BridgeStep::Init: return "bridge setup";
	case BridgeStep::Import: return "carrying the frame in";
	case BridgeStep::Begin: return "opening the command list";
	case BridgeStep::Core: return "the upscaler";
	case BridgeStep::Finish: return "submitting";
	case BridgeStep::Export: return "carrying the frame back";
	}
	return "unknown";
}

DXGI_FORMAT dxgi_format_of(reshade::api::format fmt) noexcept
{
	const uint32_t v = static_cast<uint32_t>(fmt);
	if (v <= 191u)
		return static_cast<DXGI_FORMAT>(v);
	return DXGI_FORMAT_UNKNOWN;
}

reshade::api::format reshade_format_of(DXGI_FORMAT fmt) noexcept
{
	return static_cast<reshade::api::format>(static_cast<uint32_t>(fmt));
}

D3D12_RESOURCE_STATES d3d12_states_of(reshade::api::resource_usage usage) noexcept
{
	using reshade::api::resource_usage;
	if (usage == resource_usage::general)
		return D3D12_RESOURCE_STATE_COMMON;
	if (usage == resource_usage::present)
		return D3D12_RESOURCE_STATE_PRESENT;
	if (usage == resource_usage::cpu_access)
		return D3D12_RESOURCE_STATE_GENERIC_READ;

	auto states = static_cast<D3D12_RESOURCE_STATES>(static_cast<uint32_t>(usage) & 0xFFFFFFFu);

	if ((usage & resource_usage::depth_stencil) == resource_usage::depth_stencil) {
		if (usage == resource_usage::depth_stencil)
			return D3D12_RESOURCE_STATE_DEPTH_WRITE;
		states ^= D3D12_RESOURCE_STATE_DEPTH_WRITE;
	}
	if ((usage & resource_usage::constant_buffer) == resource_usage::constant_buffer) {
		states |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		states ^= static_cast<D3D12_RESOURCE_STATES>(resource_usage::constant_buffer);
	}
	return states;
}

bool EngineDevice::init_standalone(LUID adapter)
{
	if (ready())
		return true;
	if (initialising_) {
		last_error = L"the engine device was asked for while it was being created";
		return false;
	}
	initialising_ = true;
	struct Done {
		bool &flag;
		~Done() { flag = false; }
	} done{ initialising_ };

	shutdown();
	last_error.clear();
	luid_ = adapter;

	IDXGIFactory4 *factory = nullptr;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr) {
		IDXGIAdapter *found = nullptr;
		if (SUCCEEDED(factory->EnumAdapterByLuid(luid_, IID_PPV_ARGS(&found))) && found != nullptr) {
			if (SUCCEEDED(found->QueryInterface(IID_PPV_ARGS(&adapter_))))
				adapter_matched_ = true;
			safe_release(found);
		}
	}
	safe_release(factory);
	if (!adapter_matched_) {
		last_error = L"the graphics card the game is rendering on could not be found from "
			L"this process. It may have been disabled or switched while the game was running.";
		shutdown();
		return false;
	}
	return finish_device();
}

bool EngineDevice::init(reshade::api::device *game)
{
	if (ready())
		return true;
	if (initialising_) {
		last_error = L"the engine device was asked for while it was being created";
		return false;
	}
	initialising_ = true;
	struct Done {
		bool &flag;
		~Done() { flag = false; }
	} done{ initialising_ };

	shutdown();
	last_error.clear();

	if (game == nullptr) {
		last_error = L"no device";
		return false;
	}

	uint64_t luid_bits = 0;
	if (game->get_property(reshade::api::device_properties::adapter_luid, &luid_bits))
		memcpy(&luid_, &luid_bits, sizeof(luid_));
	else if (game->get_api() == reshade::api::device_api::d3d9)
		d3d9_adapter_luid(game, &luid_);

	IDXGIFactory4 *factory = nullptr;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr) {
		IDXGIAdapter *found = nullptr;
		if ((luid_.LowPart != 0 || luid_.HighPart != 0) &&
			SUCCEEDED(factory->EnumAdapterByLuid(luid_, IID_PPV_ARGS(&found))) && found != nullptr) {
			if (SUCCEEDED(found->QueryInterface(IID_PPV_ARGS(&adapter_))))
				adapter_matched_ = true;
			safe_release(found);
		}
	}
	safe_release(factory);

	if (game->get_api() == reshade::api::device_api::d3d12) {
		device12_ = reinterpret_cast<ID3D12Device *>(game->get_native());
		if (device12_ == nullptr) {
			last_error = L"the Direct3D 12 device could not be read back from ReShade";
			shutdown();
			return false;
		}
		device12_->AddRef();
		borrowed_ = true;
		adapter_matched_ = true;
		return true;
	}

	return finish_device();
}

bool EngineDevice::finish_device()
{
	if (device12_ == nullptr) {
		const D3D_FEATURE_LEVEL levels[] = {
			D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
			D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
		};
		HRESULT hr = E_FAIL;
		for (const D3D_FEATURE_LEVEL fl : levels) {
			hr = D3D12CreateDevice(adapter_, fl, IID_PPV_ARGS(&device12_));
			if (SUCCEEDED(hr))
				break;
		}
		if (FAILED(hr) || device12_ == nullptr) {
			last_error = L"no Direct3D 12 device on this adapter. The add-on runs its "
				L"upscalers and its own passes on Direct3D 12 whatever the game uses, so "
				L"a driver without it cannot be bridged.";
			shutdown();
			return false;
		}
	}

	D3D12_COMMAND_QUEUE_DESC qdesc{};
	qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	bool ok = SUCCEEDED(device12_->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue12_)));
	for (ID3D12CommandAllocator *&a : allocators_)
		ok = ok && SUCCEEDED(device12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
	ok = ok && SUCCEEDED(device12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0], nullptr,
		IID_PPV_ARGS(&list_)));
	ok = ok && SUCCEEDED(device12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence12_)));
	if (!ok) {
		last_error = L"the engine device refused a command queue";
		shutdown();
		return false;
	}
	list_->Close();

	fence_value_ = 0;
	last_gpu_value_ = 0;
	for (uint64_t &v : allocator_value_)
		v = 0;
	slot_ = 0;
	return true;
}

void EngineDevice::shutdown()
{
	if (fence12_ != nullptr && last_gpu_value_ > 0)
		wait_cpu(last_gpu_value_);

	safe_release(drain_fence_);
	drain_value_ = 0;
	borrowed_queue_ = nullptr;
	safe_release(list_);
	for (ID3D12CommandAllocator *&a : allocators_)
		safe_release(a);
	safe_release(fence12_);
	safe_release(queue12_);
	safe_release(device12_);

	safe_release(context11_);
	safe_release(device11_5_);
	safe_release(device11_);
	helper11_failed_ = false;

	safe_release(adapter_);
	luid_ = LUID{};
	adapter_matched_ = false;
	borrowed_ = false;
	fence_value_ = 0;
	last_gpu_value_ = 0;
	for (uint64_t &v : allocator_value_)
		v = 0;
	slot_ = 0;
	list_open_ = false;
	begin_stalls_ = 0;
	begin_stall_ms_ = 0.0;
}

ID3D11Device *EngineDevice::device11()
{
	if (device11_ != nullptr || helper11_failed_)
		return device11_;
	if (!ready()) {
		helper11_failed_ = true;
		return nullptr;
	}

	const D3D_FEATURE_LEVEL levels[] = {
		D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
	};
	const HRESULT hr = D3D11CreateDevice(adapter_,
		adapter_ != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
		nullptr, 0, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
		&device11_, nullptr, &context11_);
	if (FAILED(hr) || device11_ == nullptr) {
		safe_release(context11_);
		safe_release(device11_);
		helper11_failed_ = true;
		last_error = L"the helper Direct3D 11 device could not be created";
		return nullptr;
	}
	if (FAILED(device11_->QueryInterface(IID_PPV_ARGS(&device11_5_))))
		device11_5_ = nullptr;
	return device11_;
}

ID3D11DeviceContext *EngineDevice::context11()
{
	return device11() != nullptr ? context11_ : nullptr;
}

ID3D11Device5 *EngineDevice::device11_5()
{
	return device11() != nullptr ? device11_5_ : nullptr;
}

bool EngineDevice::wait_cpu(uint64_t value)
{
	if (fence12_ == nullptr || value == 0)
		return true;
	const bool ok = fence_wait(fence12_, value, 5000);
	if (!ok)
		last_error = L"the engine device stopped responding (a GPU wait timed out)";
	return ok;
}

bool EngineDevice::drain()
{
	if (!borrowed_)
		return wait_cpu(last_gpu_value_);
	if (device12_ == nullptr || borrowed_queue_ == nullptr)
		return true;
	if (drain_fence_ == nullptr &&
			FAILED(device12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain_fence_)))) {
		drain_fence_ = nullptr;
		return false;
	}
	const uint64_t value = ++drain_value_;
	if (FAILED(borrowed_queue_->Signal(drain_fence_, value)))
		return false;
	const bool ok = fence_wait(drain_fence_, value, 5000);
	if (!ok)
		last_error = L"the game's queue stopped responding (a GPU wait timed out)";
	return ok;
}

ID3D12GraphicsCommandList *EngineDevice::begin_list()
{
	if (!ready())
		return nullptr;
	if (list_ == nullptr || allocators_[slot_] == nullptr) {
		last_error = L"the engine has no command list of its own on a borrowed device";
		return nullptr;
	}
	if (list_open_) {
		last_error = L"the engine command list was opened twice without a submit";
		return nullptr;
	}
	const uint64_t need = allocator_value_[slot_];
	if (need > 0 && fence12_->GetCompletedValue() < need) {
		LARGE_INTEGER freq{}, t0{}, t1{};
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&t0);
		const bool ok = wait_cpu(need);
		QueryPerformanceCounter(&t1);
		++begin_stalls_;
		if (freq.QuadPart != 0)
			begin_stall_ms_ += 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
				static_cast<double>(freq.QuadPart);
		if (!ok)
			return nullptr;
	}
	ID3D12CommandAllocator *const allocator = allocators_[slot_];
	if (FAILED(allocator->Reset()) || FAILED(list_->Reset(allocator, nullptr))) {
		last_error = L"the engine command list could not be reset";
		return nullptr;
	}
	list_open_ = true;
	return list_;
}

void EngineDevice::abandon_list()
{
	if (list_ != nullptr && list_open_)
		list_->Close();
	list_open_ = false;
}

bool EngineDevice::submit_list()
{
	if (!ready())
		return false;
	if (FAILED(list_->Close())) {
		last_error = L"the engine command list could not be closed";
		list_open_ = false;
		return false;
	}
	list_open_ = false;
	ID3D12CommandList *lists[] = { list_ };
	queue12_->ExecuteCommandLists(1, lists);
	const bool signalled = SUCCEEDED(queue12_->Signal(fence12_, fence_value_ + 1));
	if (signalled) {
		++fence_value_;
		allocator_value_[slot_] = fence_value_;
		last_gpu_value_ = fence_value_;
	}
	slot_ = (slot_ + 1u) % kFramesInFlight;
	if (!signalled) {
		last_error = L"the engine queue refused a fence signal";
		return false;
	}
	return true;
}

}
