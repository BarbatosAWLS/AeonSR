#include "aeon_sr/framegen/display_clock.hpp"

#include <winternl.h>
#include <d3dkmthk.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>

namespace aeon_sr {

double qpc_ms() noexcept
{
	static const double inv = [] {
		LARGE_INTEGER f{};
		QueryPerformanceFrequency(&f);
		return 1000.0 / static_cast<double>(f.QuadPart);
	}();
	LARGE_INTEGER t{};
	QueryPerformanceCounter(&t);
	return static_cast<double>(t.QuadPart) * inv;
}

namespace {

struct Kmt {
	using Open = NTSTATUS(APIENTRY *)(D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME *);
	using Close = NTSTATUS(APIENTRY *)(const D3DKMT_CLOSEADAPTER *);
	using Wait = NTSTATUS(APIENTRY *)(const D3DKMT_WAITFORVERTICALBLANKEVENT *);
	Open open = nullptr;
	Close close = nullptr;
	Wait wait = nullptr;
	Kmt()
	{
		const HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
		if (gdi == nullptr)
			return;
		open = reinterpret_cast<Open>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName"));
		close = reinterpret_cast<Close>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
		wait = reinterpret_cast<Wait>(GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent"));
	}
	bool ok() const noexcept { return open != nullptr && close != nullptr && wait != nullptr; }
};

const Kmt &kmt()
{
	static const Kmt k;
	return k;
}

constexpr double kShortest = 2.0, kLongest = 50.0;
constexpr uint32_t kNeed = 24;
constexpr double kAgree = 0.08;
constexpr double kAgreeShare = 0.8;

}

DisplayClock::~DisplayClock()
{
	stop();
}

void DisplayClock::watch(HWND window)
{
	if (window == nullptr || !kmt().ok())
		return;
	static const bool primary = [] {
		wchar_t v[16] = {};
		return GetEnvironmentVariableW(L"AEONSR_FG_VBLANK_MONITOR", v, 16) != 0 && std::wcscmp(v, L"primary") == 0;
	}();
	const HMONITOR m = primary ? MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY)
		: MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
	if (m == monitor_ && running_.load(std::memory_order_acquire))
		return;
	if (m == monitor_ && thread_.joinable() &&
		GetTickCount64() - ended_tick_.load(std::memory_order_acquire) < kRetryMs)
		return;
	stop();
	monitor_ = m;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		count_ = 0;
		grid_ = DisplayGrid{};
		period_ = spread_ = 0.0;
	}
	running_.store(true, std::memory_order_release);
	thread_ = std::thread([this, m] { run(m); });
}

void DisplayClock::stop() noexcept
{
	running_.store(false, std::memory_order_release);
	if (thread_.joinable()) {
		const HANDLE h = static_cast<HANDLE>(thread_.native_handle());
		if (WaitForSingleObject(h, 250) == WAIT_OBJECT_0) {
			thread_.join();
		} else {
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				reinterpret_cast<LPCWSTR>(&qpc_ms), &self);
			thread_.detach();
		}
	}
	monitor_ = nullptr;
	std::lock_guard<std::mutex> lock(mutex_);
	grid_ = DisplayGrid{};
}

DisplayGrid DisplayClock::grid() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return grid_;
}

double DisplayClock::measured_period_ms() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return period_;
}

double DisplayClock::spread() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return spread_;
}

void DisplayClock::run(HMONITOR monitor)
{
	struct Ended {
		DisplayClock &c;
		~Ended()
		{
			c.ended_tick_.store(GetTickCount64(), std::memory_order_release);
			c.running_.store(false, std::memory_order_release);
		}
	} ended{ *this };
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	MONITORINFOEXW info{};
	info.cbSize = sizeof(info);
	if (!GetMonitorInfoW(monitor, &info))
		return;
	D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME open{};
	wcsncpy_s(open.DeviceName, info.szDevice, _TRUNCATE);
	if (kmt().open(&open) != 0)
		return;
	D3DKMT_WAITFORVERTICALBLANKEVENT w{};
	w.hAdapter = open.hAdapter;
	w.hDevice = 0;
	w.VidPnSourceId = open.VidPnSourceId;

	double last = 0.0;
	uint32_t failures = 0;
	while (running_.load(std::memory_order_acquire)) {
		if (kmt().wait(&w) != 0) {
			if (++failures > 20)
				break;
			Sleep(10);
			continue;
		}
		failures = 0;
		const double t = qpc_ms();
		const double interval = last > 0.0 ? t - last : 0.0;
		last = t;
		std::lock_guard<std::mutex> lock(mutex_);
		if (interval < kShortest || interval > kLongest) {
			if (grid_.valid)
				grid_.vblank_ms = t;
			continue;
		}
		times_[count_ % kRing] = interval;
		++count_;
		const uint32_t n = std::min(count_, kRing);
		if (n < kNeed) {
			grid_.valid = false;
			continue;
		}
		double v[kRing];
		std::copy(times_, times_ + n, v);
		std::sort(v, v + n);
		const double med = v[n / 2];
		uint32_t agree = 0;
		double sum = 0.0;
		for (uint32_t i = 0; i < n; ++i) {
			if (std::fabs(v[i] - med) <= kAgree * med) {
				++agree;
				sum += v[i];
			}
		}
		period_ = agree > 0 ? sum / agree : med;
		spread_ = (v[n * 9 / 10] - v[n / 10]) / med;
		grid_.valid = agree >= static_cast<uint32_t>(kAgreeShare * n);
		grid_.period_ms = period_;
		grid_.vblank_ms = t;
	}
	D3DKMT_CLOSEADAPTER close{};
	close.hAdapter = open.hAdapter;
	kmt().close(&close);
	std::lock_guard<std::mutex> lock(mutex_);
	grid_ = DisplayGrid{};
}

}
