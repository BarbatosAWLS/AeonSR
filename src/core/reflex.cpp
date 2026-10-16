#include "aeon_sr/core/reflex.hpp"

#include <Windows.h>
#include <Unknwn.h>

#include <cstddef>
#include <cstring>
#include <memory>

namespace aeon_sr {

namespace {

struct SleepModeParamsV1 {
	uint32_t version;
	uint8_t low_latency_mode;
	uint8_t low_latency_boost;
	uint32_t minimum_interval_us;
	uint8_t use_markers_to_optimize;
	uint8_t use_min_queue_time;
	uint8_t rsvd[30];
};
static_assert(sizeof(SleepModeParamsV1) == 44, "NV_SET_SLEEP_MODE_PARAMS_V1 is 44 bytes");
static_assert(offsetof(SleepModeParamsV1, minimum_interval_us) == 8, "NV_SET_SLEEP_MODE_PARAMS_V1 moved");

struct SleepStatusParamsV1 {
	uint32_t version;
	uint8_t low_latency_mode;
	uint8_t fs_vrr;
	uint8_t cpl_vsync_on;
	uint32_t sleep_interval_us;
	uint8_t use_game_sleep;
	uint8_t fullscreen_iflip;
	uint8_t fg_multiplier;
	uint8_t dfg_control;
	uint32_t dfg_frame_time_target_us;
	uint8_t rsvd[114];
};
static_assert(sizeof(SleepStatusParamsV1) == 136, "NV_GET_SLEEP_STATUS_PARAMS_V1 is 136 bytes");
static_assert(offsetof(SleepStatusParamsV1, use_game_sleep) == 12, "NV_GET_SLEEP_STATUS_PARAMS_V1 moved");

struct LatencyMarkerParamsV1 {
	uint32_t version;
	uint64_t frame_id;
	uint32_t marker_type;
	uint64_t rsvd0;
	uint8_t rsvd[56];
};
static_assert(sizeof(LatencyMarkerParamsV1) == 88, "NV_LATENCY_MARKER_PARAMS_V1 is 88 bytes");
static_assert(offsetof(LatencyMarkerParamsV1, marker_type) == 16, "NV_LATENCY_MARKER_PARAMS_V1 moved");

struct LatencyResultParamsV1 {
	uint32_t version;
	struct FrameReport {
		uint64_t frame_id;
		uint64_t input_sample_time;
		uint64_t sim_start_time;
		uint64_t sim_end_time;
		uint64_t render_submit_start_time;
		uint64_t render_submit_end_time;
		uint64_t present_start_time;
		uint64_t present_end_time;
		uint64_t driver_start_time;
		uint64_t driver_end_time;
		uint64_t os_render_queue_start_time;
		uint64_t os_render_queue_end_time;
		uint64_t gpu_render_start_time;
		uint64_t gpu_render_end_time;
		uint32_t gpu_active_render_time_us;
		uint32_t gpu_frame_time_us;
		uint64_t camera_constructed_time;
		uint32_t cross_adapter_copy_time_us;
		uint32_t ai_frame_time_us;
		uint8_t rsvd[104];
	} frame_report[64];
	uint8_t rsvd[32];
};
static_assert(sizeof(LatencyResultParamsV1::FrameReport) == 240, "NV_LATENCY_RESULT_PARAMS_V1 frame report is 240 bytes");

constexpr uint32_t version_of(uint32_t size, uint32_t number) noexcept
{
	return size | (number << 16);
}

constexpr int kNvOk = 0;

}

bool Reflex::load()
{
	if (loaded_ || load_failed_)
		return loaded_;
	const wchar_t *const name = sizeof(void *) == 8 ? L"nvapi64.dll" : L"nvapi.dll";
	HMODULE m = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, name, &m) || m == nullptr) {
		m = LoadLibraryW(name);
		if (m != nullptr)
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, name, &m);
	}
	if (m == nullptr) {
		load_failed_ = true;
		why_ = std::wstring(L"Reflex needs an NVIDIA driver: ") + name + L" is not there";
		return false;
	}
	typedef void *(*Query)(unsigned int);
	const auto query = reinterpret_cast<Query>(GetProcAddress(m, "nvapi_QueryInterface"));
	const auto initialize = query != nullptr ? reinterpret_cast<int (*)()>(query(0x0150E828u)) : nullptr;
	if (initialize == nullptr || initialize() != kNvOk) {
		load_failed_ = true;
		why_ = L"Reflex needs an NVIDIA driver: NvAPI did not initialise";
		return false;
	}
	set_sleep_mode_ = reinterpret_cast<int (*)(IUnknown *, void *)>(query(0xAC1CA9E0u));
	sleep_ = reinterpret_cast<int (*)(IUnknown *)>(query(0x852CD1D2u));
	set_marker_ = reinterpret_cast<int (*)(IUnknown *, void *)>(query(0xD9984C05u));
	get_latency_ = reinterpret_cast<int (*)(IUnknown *, void *)>(query(0x1A587F9Cu));
	get_sleep_status_ = reinterpret_cast<int (*)(IUnknown *, void *)>(query(0xAEF96CA1u));
	if (set_sleep_mode_ == nullptr || sleep_ == nullptr || set_marker_ == nullptr) {
		load_failed_ = true;
		why_ = L"this NVIDIA driver has no Reflex calls in NvAPI";
		return false;
	}
	module_ = m;
	loaded_ = true;
	return true;
}

bool Reflex::available()
{
	return load();
}

bool Reflex::already_on(IUnknown *device)
{
	if (device == nullptr || !load() || get_sleep_status_ == nullptr)
		return false;
	SleepStatusParamsV1 s{};
	s.version = version_of(sizeof(s), 1u);
	return get_sleep_status_(device, &s) == kNvOk && s.low_latency_mode != 0 && s.use_game_sleep != 0;
}

bool Reflex::attach(IUnknown *device, uint32_t interval_us, bool markers)
{
	markers_ = markers;
	if (device == nullptr || !load())
		return false;
	if (device_ == device)
		return set_interval(interval_us);
	detach();
	SleepModeParamsV1 p{};
	p.version = version_of(sizeof(p), 1u);
	p.low_latency_mode = 1;
	p.minimum_interval_us = interval_us;
	p.use_markers_to_optimize = markers_ ? 1 : 0;
	const int r = set_sleep_mode_(device, &p);
	if (r != kNvOk) {
		why_ = L"the driver refused Reflex's low latency mode on this device (NvAPI status " + std::to_wstring(r) + L")";
		return false;
	}
	device->AddRef();
	device_ = device;
	interval_us_ = interval_us;
	why_.clear();
	return true;
}

bool Reflex::set_interval(uint32_t interval_us)
{
	if (device_ == nullptr)
		return false;
	if (interval_us == interval_us_)
		return true;
	SleepModeParamsV1 p{};
	p.version = version_of(sizeof(p), 1u);
	p.low_latency_mode = 1;
	p.minimum_interval_us = interval_us;
	p.use_markers_to_optimize = markers_ ? 1 : 0;
	if (set_sleep_mode_(device_, &p) != kNvOk)
		return false;
	interval_us_ = interval_us;
	return true;
}

void Reflex::detach() noexcept
{
	if (device_ == nullptr)
		return;
	if (set_sleep_mode_ != nullptr) {
		SleepModeParamsV1 p{};
		p.version = version_of(sizeof(p), 1u);
		set_sleep_mode_(device_, &p);
	}
	device_->Release();
	device_ = nullptr;
	interval_us_ = 0;
}

bool Reflex::sleep()
{
	return device_ != nullptr && sleep_(device_) == kNvOk;
}

void Reflex::marker(ReflexMarker type, uint64_t frame)
{
	if (device_ == nullptr)
		return;
	LatencyMarkerParamsV1 p{};
	p.version = version_of(sizeof(p), 1u);
	p.frame_id = frame;
	p.marker_type = static_cast<uint32_t>(type);
	set_marker_(device_, &p);
}

bool Reflex::latency(ReflexLatency *out)
{
	if (out == nullptr || device_ == nullptr || get_latency_ == nullptr)
		return false;
	const auto r = std::make_unique<LatencyResultParamsV1>();
	std::memset(r.get(), 0, sizeof(*r));
	r->version = version_of(sizeof(*r), 1u);
	if (get_latency_(device_, r.get()) != kNvOk)
		return false;
	ReflexLatency acc{};
	for (const auto &f : r->frame_report) {
		if (f.frame_id == 0 || f.sim_start_time == 0 || f.gpu_render_end_time < f.sim_start_time)
			continue;
		++acc.frames;
		acc.pc_ms += static_cast<double>(f.gpu_render_end_time - f.sim_start_time) / 1000.0;
		acc.gpu_ms += static_cast<double>(f.gpu_active_render_time_us) / 1000.0;
		if (f.gpu_render_start_time >= f.render_submit_end_time)
			acc.queue_ms += static_cast<double>(f.gpu_render_start_time - f.render_submit_end_time) / 1000.0;
	}
	if (acc.frames == 0)
		return false;
	acc.pc_ms /= acc.frames;
	acc.gpu_ms /= acc.frames;
	acc.queue_ms /= acc.frames;
	*out = acc;
	return true;
}

}
