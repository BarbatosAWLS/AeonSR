#pragma once

#include <cstdint>
#include <string>

struct IUnknown;

namespace aeon_sr {

enum class ReflexMarker : uint32_t {
	SimulationStart = 0,
	SimulationEnd = 1,
	RenderSubmitStart = 2,
	RenderSubmitEnd = 3,
	PresentStart = 4,
	PresentEnd = 5,
	InputSample = 6,
};

struct ReflexLatency {
	uint32_t frames = 0;
	double pc_ms = 0.0;
	double gpu_ms = 0.0;
	double queue_ms = 0.0;
};

class Reflex {
public:
	~Reflex() { detach(); }

	bool available();
	bool already_on(IUnknown *device);
	bool attach(IUnknown *device, uint32_t interval_us, bool markers = false);
	void detach() noexcept;
	bool attached() const noexcept { return device_ != nullptr; }
	IUnknown *device() const noexcept { return device_; }
	bool set_interval(uint32_t interval_us);
	bool sleep();
	void marker(ReflexMarker type, uint64_t frame);
	bool latency(ReflexLatency *out);
	const std::wstring &why() const noexcept { return why_; }

private:
	bool load();

	void *module_ = nullptr;
	bool loaded_ = false, load_failed_ = false;
	int (*set_sleep_mode_)(IUnknown *, void *) = nullptr;
	int (*sleep_)(IUnknown *) = nullptr;
	int (*set_marker_)(IUnknown *, void *) = nullptr;
	int (*get_latency_)(IUnknown *, void *) = nullptr;
	int (*get_sleep_status_)(IUnknown *, void *) = nullptr;
	IUnknown *device_ = nullptr;
	uint32_t interval_us_ = 0;
	bool markers_ = false;
	std::wstring why_;
};

}
