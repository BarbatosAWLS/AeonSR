#pragma once

#include "aeon_sr/core/imgui_reshade.hpp"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aeon_sr {

enum class BridgeKind : uint8_t {
	None = 0,
	Native12,
	Shared11,
	Legacy10,
	Legacy9,
	OpenGl,
	Vulkan,
	Staging,
};

const char *bridge_kind_label(BridgeKind kind) noexcept;

enum class BridgeStep : uint8_t {
	Ok = 0,
	Init,
	Import,
	Begin,
	Core,
	Finish,
	Export,
};

const char *bridge_step_label(BridgeStep step) noexcept;

class EngineDevice {
public:
	bool init(reshade::api::device *game);
	bool init_standalone(LUID adapter);
	void shutdown();

	bool ready() const noexcept
	{
		return device12_ != nullptr && (borrowed_ ? borrowed_queue_ != nullptr : queue12_ != nullptr);
	}
	ID3D12Device *device() const noexcept { return device12_; }
	ID3D12CommandQueue *queue() const noexcept { return borrowed_ ? borrowed_queue_ : queue12_; }
	void set_borrowed_queue(ID3D12CommandQueue *q) noexcept { borrowed_queue_ = q; }
	bool borrowed() const noexcept { return borrowed_; }
	bool adapter_matched() const noexcept { return adapter_matched_; }
	const LUID &luid() const noexcept { return luid_; }

	ID3D11Device *device11();
	ID3D11DeviceContext *context11();
	ID3D11Device5 *device11_5();

	static constexpr uint32_t kFramesInFlight = 2;

	ID3D12GraphicsCommandList *begin_list();
	bool submit_list();
	void abandon_list();
	uint64_t last_submitted() const noexcept { return last_gpu_value_; }
	ID3D12Fence *fence() const noexcept { return fence12_; }
	bool wait_cpu(uint64_t value);
	uint64_t begin_stalls() const noexcept { return begin_stalls_; }
	double begin_stall_ms() const noexcept { return begin_stall_ms_; }
	bool drain();

	std::wstring last_error;

	~EngineDevice() { shutdown(); }

private:
	ID3D12Device *device12_ = nullptr;
	ID3D12CommandQueue *queue12_ = nullptr;
	ID3D12CommandQueue *borrowed_queue_ = nullptr;
	ID3D12CommandAllocator *allocators_[kFramesInFlight] = {};
	uint64_t allocator_value_[kFramesInFlight] = {};
	uint32_t slot_ = 0;
	ID3D12GraphicsCommandList *list_ = nullptr;
	ID3D12Fence *fence12_ = nullptr;
	uint64_t fence_value_ = 0;
	uint64_t last_gpu_value_ = 0;
	uint64_t begin_stalls_ = 0;
	double begin_stall_ms_ = 0.0;
	ID3D12Fence *drain_fence_ = nullptr;
	uint64_t drain_value_ = 0;
	bool list_open_ = false;

	ID3D11Device *device11_ = nullptr;
	ID3D11Device5 *device11_5_ = nullptr;
	ID3D11DeviceContext *context11_ = nullptr;
	bool helper11_failed_ = false;

	IDXGIAdapter1 *adapter_ = nullptr;
	LUID luid_{};
	bool adapter_matched_ = false;
	bool borrowed_ = false;
	bool initialising_ = false;

	bool finish_device();
};

struct SharedPlane {
	reshade::api::resource game = { 0 };
	ID3D12Resource *engine = nullptr;
	ID3D11Texture2D *helper = nullptr;

	uint32_t width = 0;
	uint32_t height = 0;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

	static constexpr D3D12_RESOURCE_STATES kState = D3D12_RESOURCE_STATE_COMMON;

	bool matches(uint32_t w, uint32_t h, DXGI_FORMAT fmt) const noexcept
	{
		return engine != nullptr && width == w && height == h && format == fmt;
	}
};

struct BridgeInputs {
	reshade::api::resource color = { 0 };
	reshade::api::resource depth = { 0 };
	reshade::api::resource_view depth_view = { 0 };
	reshade::api::resource_usage depth_state = reshade::api::resource_usage::shader_resource;
	reshade::api::resource motion_vectors = { 0 };
	reshade::api::resource scene = { 0 };
	reshade::api::resource_usage scene_state = reshade::api::resource_usage::copy_dest;
};

struct BridgeFrame {
	ID3D12Device *device = nullptr;
	ID3D12CommandQueue *queue = nullptr;
	ID3D12GraphicsCommandList *cmd = nullptr;

	ID3D12Resource *color = nullptr;
	ID3D12Resource *depth = nullptr;
	ID3D12Resource *motion_vectors = nullptr;
	ID3D12Resource *scene = nullptr;

	D3D12_RESOURCE_STATES color_state = SharedPlane::kState;
	D3D12_RESOURCE_STATES depth_state = SharedPlane::kState;
	D3D12_RESOURCE_STATES motion_state = SharedPlane::kState;
	D3D12_RESOURCE_STATES scene_state = SharedPlane::kState;

	uint32_t width = 0;
	uint32_t height = 0;

	bool native = false;
};

class FrameBridge {
public:
	virtual ~FrameBridge() = default;

	virtual BridgeKind kind() const noexcept = 0;
	virtual const char *name() const noexcept = 0;

	virtual bool init(reshade::api::device *game, EngineDevice &engine) = 0;
	virtual void shutdown() = 0;
	virtual bool ready() const noexcept = 0;

	virtual void on_swapchain_reset() {}

	virtual BridgeStep begin(reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list, const BridgeInputs &in, BridgeFrame &out) = 0;

	virtual BridgeStep end(reshade::api::effect_runtime *runtime,
		reshade::api::command_list *cmd_list, reshade::api::resource game_color) = 0;

	virtual bool request_readable(SharedPlane &plane, uint32_t w, uint32_t h, DXGI_FORMAT fmt) = 0;

	virtual const char *sync_name() const noexcept = 0;

	std::wstring last_error;

	std::wstring depth_note;

	bool device_destroying = false;
};

FrameBridge *make_bridge(reshade::api::device *game, EngineDevice &engine, std::wstring *error,
	bool system_memory_only = false);

enum class BridgeRelease : uint8_t { Ordinary, GameDeviceGone };

void retire_bridge(FrameBridge *&bridge, BridgeRelease how);

std::optional<BridgeRelease> bridge_release_on_destroy(const reshade::api::device *destroyed,
	const reshade::api::device *bridge_device, bool destroyed_is_engine) noexcept;

bool adopt_on_init(reshade::api::device_api api, const reshade::api::device *bridge_device) noexcept;

class PendingDevices {
public:
	void add(const reshade::api::device *device);
	void remove(const reshade::api::device *device);
	bool take(const reshade::api::device *device);
	size_t size();

private:
	std::mutex mutex_;
	std::vector<const reshade::api::device *> devices_;
};

DXGI_FORMAT dxgi_format_of(reshade::api::format fmt) noexcept;
reshade::api::format reshade_format_of(DXGI_FORMAT fmt) noexcept;

D3D12_RESOURCE_STATES d3d12_states_of(reshade::api::resource_usage usage) noexcept;

}
