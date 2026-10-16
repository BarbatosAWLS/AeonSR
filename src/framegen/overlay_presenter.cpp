#include "aeon_sr/framegen/overlay_presenter.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/dxgi_format_util.hpp"
#include "aeon_sr/core/settings.hpp"

#include <d3d11_4.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>

namespace aeon_sr {

namespace {

using CreateDevice2 = HRESULT(WINAPI *)(IUnknown *, REFIID, void **);

DXGI_FORMAT swap_format_for(DXGI_FORMAT family) noexcept
{
	switch (family) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

void barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *res, D3D12_RESOURCE_STATES before,
	D3D12_RESOURCE_STATES after)
{
	if (before == after)
		return;
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	cmd->ResourceBarrier(1, &b);
}

bool make_tex(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT f, ID3D12Resource **out, bool uav = false)
{
	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w;
	d.Height = h;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = f;
	d.SampleDesc.Count = 1;
	if (uav)
		d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, OverlayFeed::kRestState, nullptr,
		IID_PPV_ARGS(out)));
}

LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_NCHITTEST: return HTTRANSPARENT;
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_CLOSE: return 0;
	default: return DefWindowProcW(hwnd, msg, wp, lp);
	}
}

HINSTANCE self_module()
{
	HMODULE m = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&overlay_proc), &m);
	return m;
}

constexpr const wchar_t *kClass = L"AeonSRFrameGenerationWindow";

void give_overlays_their_event()
{
	static constexpr const wchar_t *kKick = L"AeonSROverlayInjection";
	const HINSTANCE inst = self_module();
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	if (!GetClassInfoExW(inst, kKick, &wc)) {
		wc = WNDCLASSEXW{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = inst;
		wc.lpszClassName = kKick;
		RegisterClassExW(&wc);
	}
	const HWND w = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kKick, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
		inst, nullptr);
	if (w != nullptr)
		DestroyWindow(w);
}

template <typename T>
void release(T *&p) noexcept
{
	if (p != nullptr)
		p->Release();
	p = nullptr;
}

}

namespace {

thread_local bool t_creating_device = false;
thread_local bool t_overlay_thread = false;
std::mutex g_own_mutex;
std::vector<uint64_t> g_own_devices;

constexpr GUID kUnwrappedObject = { 0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 } };

}

bool overlay_creating_device() noexcept
{
	return t_creating_device;
}

bool overlay_on_own_thread() noexcept
{
	return t_overlay_thread;
}

void overlay_note_own_device(uint64_t native) noexcept
{
	std::lock_guard<std::mutex> lock(g_own_mutex);
	if (std::find(g_own_devices.begin(), g_own_devices.end(), native) == g_own_devices.end())
		g_own_devices.push_back(native);
}

bool overlay_forget_own_device(uint64_t native) noexcept
{
	std::lock_guard<std::mutex> lock(g_own_mutex);
	const auto it = std::find(g_own_devices.begin(), g_own_devices.end(), native);
	if (it == g_own_devices.end())
		return false;
	g_own_devices.erase(it);
	return true;
}

std::wstring overlay_window_host_beside_addon()
{
	wchar_t v[MAX_PATH] = {};
	const DWORD n = GetEnvironmentVariableW(L"AEONSR_FG_WINDOW_HOST", v, MAX_PATH);
	if (n > 0 && n < MAX_PATH)
		return wcscmp(v, L"0") == 0 ? std::wstring() : std::wstring(v);
#if defined(_WIN64)
	return join_path(module_directory(self_module()), L"AeonSRHost.exe");
#else
	return std::wstring();
#endif
}

bool compositor_clock(double *vblank_ms, double *period_ms) noexcept
{
	using GetTiming = HRESULT(WINAPI *)(HWND, DWM_TIMING_INFO *);
	static const GetTiming get = [] {
		const HMODULE m = LoadLibraryW(L"dwmapi.dll");
		return m != nullptr ? reinterpret_cast<GetTiming>(GetProcAddress(m, "DwmGetCompositionTimingInfo")) : nullptr;
	}();
	static const double inv = [] {
		LARGE_INTEGER f{};
		QueryPerformanceFrequency(&f);
		return 1000.0 / static_cast<double>(f.QuadPart);
	}();
	if (get == nullptr)
		return false;
	DWM_TIMING_INFO ti{};
	ti.cbSize = sizeof(ti);
	if (FAILED(get(nullptr, &ti)) || ti.qpcRefreshPeriod == 0 || ti.qpcVBlank == 0)
		return false;
	const double period = static_cast<double>(ti.qpcRefreshPeriod) * inv;
	if (period < 2.0 || period > 60.0)
		return false;
	*vblank_ms = static_cast<double>(ti.qpcVBlank) * inv;
	*period_ms = period;
	return true;
}

OverlayPresenter::~OverlayPresenter()
{
	stop();
}

bool OverlayPresenter::make_ring(ID3D12Device *device, uint32_t w, uint32_t h, DXGI_FORMAT family, std::wstring *why)
{
	for (Slot &s : slots_) {
		if (!make_tex(device, w, h, family, &s.color) || !make_tex(device, w, h, DXGI_FORMAT_R16G16_FLOAT, &s.motion) ||
			!make_tex(device, 1, 1, DXGI_FORMAT_R16G16_FLOAT, &s.global)) {
			release_ring();
			if (why != nullptr)
				*why = L"frame generation: the textures of its own window could not be made";
			return false;
		}
		s.id = 0;
		s.read_until = 0;
	}
	return true;
}

void OverlayPresenter::release_ring() noexcept
{
	for (Slot &s : slots_) {
		release(s.color);
		release(s.motion);
		release(s.global);
		for (ID3D12Resource *&m : s.made)
			release(m);
		s.made_format = DXGI_FORMAT_UNKNOWN;
		s.id = 0;
		s.read_until = 0;
	}
}

void OverlayPresenter::free_retired(bool all) noexcept
{
	const uint64_t read = read_fence_ != nullptr ? read_fence_->GetCompletedValue() : 0;
	const uint64_t fed = feed_fence_ != nullptr ? feed_fence_->GetCompletedValue() : 0;
	for (Retired &r : retired_)
		if (all || (r.read_until <= read && r.feed_until <= fed))
			release(r.res);
	retired_.erase(std::remove_if(retired_.begin(), retired_.end(), [](const Retired &r) { return r.res == nullptr; }),
		retired_.end());
}

bool OverlayPresenter::ensure(ID3D12Device *device, HWND game_window, uint32_t w, uint32_t h, DXGI_FORMAT frame_format,
	VendorColor color_space, DisplayClock *clock, std::wstring *why)
{
	DXGI_FORMAT family = frame_format;
	switch (frame_format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: family = DXGI_FORMAT_R8G8B8A8_TYPELESS; break;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: family = DXGI_FORMAT_B8G8R8A8_TYPELESS; break;
	case DXGI_FORMAT_R10G10B10A2_UNORM: family = DXGI_FORMAT_R10G10B10A2_TYPELESS; break;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: family = DXGI_FORMAT_R16G16B16A16_TYPELESS; break;
	default: break;
	}
	if (sink_failed_.exchange(false, std::memory_order_acq_rel)) {
		stop();
		if (why != nullptr)
			*why = init_error_.empty() ? L"frame generation: its own window did not start" : init_error_;
		return false;
	}
	if (running()) {
		if (device == device_ && game_window == game_window_ && w == w_ && h == h_ && family == family_ &&
			color_space == color_space_ && init_ok_.load())
			return true;
		stop();
	}
	if (device == nullptr || game_window == nullptr || w == 0 || h == 0) {
		if (why != nullptr)
			*why = L"frame generation: no window to show its own frames over";
		return false;
	}
	const DXGI_FORMAT swap = swap_format_for(family);
	if (swap == DXGI_FORMAT_UNKNOWN) {
		if (why != nullptr)
			*why = L"frame generation: this game's frame format cannot be shown in a window of its own";
		return false;
	}
	if (!make_ring(device, w, h, family, why))
		return false;
	if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&feed_fence_))) ||
		FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&read_fence_)))) {
		release_ring();
		release(feed_fence_);
		release(read_fence_);
		if (why != nullptr)
			*why = L"frame generation: the fences of its own window could not be made";
		return false;
	}
	device_ = device;
	device_->AddRef();
	game_window_ = game_window;
	w_ = w;
	h_ = h;
	family_ = family;
	swap_format_ = swap;
	color_space_ = color_space;
	clock_ = clock;
	feed_value_ = 0;
	read_value_ = 0;
	next_id_ = 1;
	feed_open_ = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pending_.clear();
		reset_pending_ = false;
		stats_ = OverlayStats{};
	}
	quit_.store(false);
	init_done_.store(false);
	init_ok_.store(false);
	thread_ = std::thread([this] { run(); });
	for (int i = 0; i < 400 && !init_done_.load(std::memory_order_acquire); ++i)
		Sleep(5);
	if (!init_ok_.load(std::memory_order_acquire)) {
		stop();
		if (why != nullptr)
			*why = init_error_.empty() ? L"frame generation: its own window did not start" : init_error_;
		return false;
	}
	return true;
}

bool OverlayPresenter::begin_feed(ID3D12CommandQueue *queue, OverlayFeed *slot, uint32_t made,
	DXGI_FORMAT made_format)
{
	if (!running() || queue == nullptr || slot == nullptr)
		return false;
	free_retired(false);
	const uint32_t k = static_cast<uint32_t>(next_id_ % kSlots);
	made = made_format != DXGI_FORMAT_UNKNOWN ? std::min(made, kOverlayMaxMade) : 0u;
	uint64_t read_until = 0;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		slots_[k].id = 0;
		read_until = slots_[k].read_until;
		slot->color = slots_[k].color;
		slot->motion = slots_[k].motion;
		slot->global = slots_[k].global;
	}
	Slot &s = slots_[k];
	ID3D12Resource *fresh[kOverlayMaxMade] = {};
	const bool other_format = made > 0 && s.made_format != made_format;
	if (made_backoff_ > 0)
		--made_backoff_;
	uint32_t have = 0;
	for (; have < made; ++have) {
		if (!other_format && s.made[have] != nullptr)
			continue;
		if (made_backoff_ > 0 || !make_tex(device_, w_, h_, made_format, &fresh[have], true)) {
			if (made_backoff_ == 0)
				made_backoff_ = kMadeBackoff;
			break;
		}
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto retire = [&](ID3D12Resource *&m) {
			if (m != nullptr)
				retired_.push_back(Retired{ m, read_until, feed_value_ });
			m = nullptr;
		};
		if (other_format) {
			for (ID3D12Resource *&m : s.made)
				retire(m);
			s.made_format = made_format;
		}
		for (uint32_t i = 0; i < have; ++i)
			if (fresh[i] != nullptr)
				s.made[i] = fresh[i];
		for (uint32_t i = made; i < kOverlayMaxMade; ++i)
			retire(s.made[i]);
		for (uint32_t i = 0; i < kOverlayMaxMade; ++i)
			slot->made[i] = i < have ? s.made[i] : nullptr;
		slot->made_count = have;
		slot->made_format = have > 0 ? s.made_format : DXGI_FORMAT_UNKNOWN;
	}
	feed_read_until_ = read_until;
	if (read_until != 0 && FAILED(queue->Wait(read_fence_, read_until)))
		return false;
	feed_open_ = true;
	return true;
}

void OverlayPresenter::end_feed(ID3D12CommandQueue *queue, double presented_ms, bool pairs, uint32_t steps,
	uint32_t made, bool vendor)
{
	if (!feed_open_ || queue == nullptr)
		return;
	feed_open_ = false;
	if (FAILED(queue->Signal(feed_fence_, ++feed_value_)))
		return;
	const uint32_t k = static_cast<uint32_t>(next_id_ % kSlots);
	std::lock_guard<std::mutex> lock(mutex_);
	slots_[k].id = next_id_;
	pending_.push_back(Pending{ next_id_, presented_ms, feed_value_, pairs, steps, made, vendor });
	++next_id_;
}

void OverlayPresenter::abandon_feed(ID3D12CommandQueue *queue, bool made_dirty) noexcept
{
	if (!feed_open_)
		return;
	feed_open_ = false;
	if (queue != nullptr && SUCCEEDED(queue->Signal(feed_fence_, feed_value_ + 1)))
		++feed_value_;
	if (!made_dirty)
		return;
	Slot &s = slots_[next_id_ % kSlots];
	std::lock_guard<std::mutex> lock(mutex_);
	for (ID3D12Resource *&m : s.made) {
		if (m != nullptr)
			retired_.push_back(Retired{ m, feed_read_until_, feed_value_ });
		m = nullptr;
	}
}

void OverlayPresenter::reset_frames() noexcept
{
	std::lock_guard<std::mutex> lock(mutex_);
	reset_pending_ = true;
	pending_.clear();
}

void OverlayPresenter::stop() noexcept
{
	if (thread_.joinable()) {
		quit_.store(true);
		thread_.join();
	}
	if (feed_fence_ != nullptr && feed_value_ != 0 && feed_fence_->GetCompletedValue() < feed_value_) {
		const HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (ev != nullptr && SUCCEEDED(feed_fence_->SetEventOnCompletion(feed_value_, ev)))
			WaitForSingleObject(ev, 1000);
		if (ev != nullptr)
			CloseHandle(ev);
	}
	release_ring();
	free_retired(true);
	release(feed_fence_);
	release(read_fence_);
	release(device_);
	game_window_ = nullptr;
	feed_open_ = false;
	std::lock_guard<std::mutex> lock(mutex_);
	pending_.clear();
	stats_.running = false;
	stats_.visible = false;
}

OverlayStats OverlayPresenter::stats() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return stats_;
}

bool OverlayPresenter::thread_init(std::wstring *why)
{
	if (FAILED(device_->QueryInterface(kUnwrappedObject, reinterpret_cast<void **>(&native_))) || native_ == nullptr) {
		native_ = device_;
		native_->AddRef();
	}
	D3D12_COMMAND_QUEUE_DESC qd{};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(native_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) {
		*why = L"frame generation: its own queue could not be made";
		return false;
	}
	for (uint32_t i = 0; i < kLists; ++i)
		if (FAILED(native_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_[i])))) {
			*why = L"frame generation: its command allocators could not be made";
			return false;
		}
	if (FAILED(native_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0], nullptr, IID_PPV_ARGS(&list_)))) {
		*why = L"frame generation: its command list could not be made";
		return false;
	}
	list_->Close();
	fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 ,
		TIMER_ALL_ACCESS);
	if (timer_ == nullptr)
		timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

	host_pending_ = !host_exe_.empty() && host_failures_ < kHostTries;
	if (host_pending_)
		return true;
	return init_local(why);
}

bool OverlayPresenter::start_sink()
{
	if (!host_pending_)
		return true;
	host_pending_ = false;
	std::wstring host_why;
	if (init_host(&host_why))
		return true;
	++host_failures_;
	diag_logf(DiagLevel::Warn, "framegen", L"frame generation: AeonSRHost.exe could not show its own window (%ls); "
		L"shown from the game's process", host_why.c_str());
	release_sink();
	std::wstring why;
	if (init_local(&why))
		return true;
	init_error_ = why;
	diag_logf(DiagLevel::Warn, "framegen", L"frame generation: its own window failed: %ls", why.c_str());
	return false;
}

bool OverlayPresenter::init_host(std::wstring *why)
{
	using SetContext = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);
	static const SetContext set_context = [] {
		const HMODULE user = GetModuleHandleW(L"user32.dll");
		return user != nullptr ? reinterpret_cast<SetContext>(GetProcAddress(user, "SetThreadDpiAwarenessContext")) : nullptr;
	}();
	if (set_context != nullptr && dpi_before_ == nullptr)
		dpi_before_ = set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	D3D12_HEAP_PROPERTIES hp{};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC d{};
	d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	d.Width = w_;
	d.Height = h_;
	d.DepthOrArraySize = 1;
	d.MipLevels = 1;
	d.Format = swap_format_;
	d.SampleDesc.Count = 1;
	d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	bool ok = true;
	for (uint32_t i = 0; i < kShared && ok; ++i)
		ok = SUCCEEDED(native_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON,
			nullptr, IID_PPV_ARGS(&shared12_[i])));
	ok = ok && SUCCEEDED(native_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ready12_))) &&
		SUCCEEDED(native_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&done12_)));
	if (!ok) {
		*why = L"the textures shared with it could not be made";
		return false;
	}
	static_assert(kShared == window_host::kShared, "the host takes three shared textures");
	const window_host::ColorSpace cs = color_space_ == VendorColor::Scrgb ? window_host::ColorSpace::Scrgb
		: color_space_ == VendorColor::Pq ? window_host::ColorSpace::Pq : window_host::ColorSpace::Sdr;
	host_ = std::make_unique<WindowHostLink>();
	if (!host_->start(host_exe_, native_, shared12_, ready12_, done12_, w_, h_, swap_format_, cs, why)) {
		host_.reset();
		return false;
	}
	host_lost_ = false;
	host_answer_ = WindowHostAnswer{};
	return true;
}

bool OverlayPresenter::init_local(std::wstring *why)
{
	const HINSTANCE inst = self_module();
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	if (!GetClassInfoExW(inst, kClass, &wc)) {
		wc = WNDCLASSEXW{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = overlay_proc;
		wc.hInstance = inst;
		wc.lpszClassName = kClass;
		wc.hCursor = nullptr;
		if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			*why = L"frame generation: its window class could not be registered";
			return false;
		}
	}
	window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT |
		WS_EX_NOREDIRECTIONBITMAP, kClass, L"Aeon SR frame generation", WS_POPUP, 0, 0, static_cast<int>(w_),
		static_cast<int>(h_), nullptr, nullptr, inst, nullptr);
	if (window_ == nullptr) {
		*why = L"frame generation: its window could not be made";
		return false;
	}
	SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA);

	wchar_t sys[MAX_PATH + 16] = {};
	const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
	if (n != 0 && n < MAX_PATH)
		wcscat_s(sys, L"\\dxgi.dll");
	dxgi_module_ = n != 0 && n < MAX_PATH ? LoadLibraryW(sys) : nullptr;
	using CreateFactory2 = HRESULT(WINAPI *)(UINT, REFIID, void **);
	const auto create_factory = dxgi_module_ != nullptr
		? reinterpret_cast<CreateFactory2>(GetProcAddress(dxgi_module_, "CreateDXGIFactory2")) : nullptr;
	IDXGIFactory2 *factory = nullptr;
	if (create_factory == nullptr || FAILED(create_factory(0, IID_PPV_ARGS(&factory)))) {
		*why = L"frame generation: no DXGI factory of the system's for its swap chain";
		return false;
	}
	IDXGIFactory4 *factory4 = nullptr;
	IDXGIAdapter1 *adapter = nullptr;
	if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory4))) ||
		FAILED(factory4->EnumAdapterByLuid(native_->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
		release(factory4);
		factory->Release();
		*why = L"frame generation: the graphics card of its own window was not found";
		return false;
	}
	release(factory4);
	wchar_t sys11[MAX_PATH + 16] = {};
	const UINT n11 = GetSystemDirectoryW(sys11, MAX_PATH);
	if (n11 != 0 && n11 < MAX_PATH)
		wcscat_s(sys11, L"\\d3d11.dll");
	d3d11_module_ = n11 != 0 && n11 < MAX_PATH ? LoadLibraryW(sys11) : nullptr;
	const auto create11 = d3d11_module_ != nullptr
		? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(d3d11_module_, "D3D11CreateDevice")) : nullptr;
	ID3D11Device *wrapper = nullptr;
	HRESULT hr11 = E_FAIL;
	if (create11 != nullptr) {
		static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		t_creating_device = true;
		hr11 = create11(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
			D3D11_SDK_VERSION, &wrapper, nullptr, nullptr);
		t_creating_device = false;
	}
	adapter->Release();
	ID3D11Device *native11 = nullptr;
	if (SUCCEEDED(hr11) && wrapper != nullptr &&
		(FAILED(wrapper->QueryInterface(kUnwrappedObject, reinterpret_cast<void **>(&native11))) || native11 == nullptr)) {
		native11 = wrapper;
		native11->AddRef();
	}
	if (native11 != nullptr && native11 != wrapper) {
		static constexpr GUID kReShadeD3D11Device = { 0x72299288, 0x2c68, 0x4ad8, { 0x94, 0x5d, 0x2b, 0xfb, 0x5a, 0xa9, 0xc6, 0x09 } };
		native11->SetPrivateData(kReShadeD3D11Device, 0, nullptr);
	}
	ID3D11Device5 *dev5 = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11DeviceContext4 *ctx4 = nullptr;
	if (native11 != nullptr && SUCCEEDED(native11->QueryInterface(IID_PPV_ARGS(&dev5)))) {
		dev5->GetImmediateContext(&ctx);
		if (ctx != nullptr)
			ctx->QueryInterface(IID_PPV_ARGS(&ctx4));
	}
	release(ctx);
	release(native11);
	d3d11_wrapper_ = wrapper;
	d3d11_ = dev5;
	ctx11_ = ctx4;
	if (dev5 == nullptr || ctx4 == nullptr) {
		factory->Release();
		*why = L"frame generation: no Direct3D 11.4 device for its own window";
		return false;
	}
	overlay_note_own_device(reinterpret_cast<uint64_t>(dev5));
	{
		D3D12_HEAP_PROPERTIES hp{};
		hp.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		d.Width = w_;
		d.Height = h_;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = swap_format_;
		d.SampleDesc.Count = 1;
		d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		bool ok = true;
		for (uint32_t i = 0; i < kShared && ok; ++i) {
			HANDLE h = nullptr;
			ok = SUCCEEDED(native_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON,
					nullptr, IID_PPV_ARGS(&shared12_[i]))) &&
				SUCCEEDED(native_->CreateSharedHandle(shared12_[i], nullptr, GENERIC_ALL, nullptr, &h)) &&
				SUCCEEDED(dev5->OpenSharedResource1(h, IID_PPV_ARGS(reinterpret_cast<ID3D11Texture2D **>(&shared11_[i]))));
			if (h != nullptr)
				CloseHandle(h);
		}
		HANDLE h = nullptr;
		ok = ok && SUCCEEDED(native_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ready12_))) &&
			SUCCEEDED(native_->CreateSharedHandle(ready12_, nullptr, GENERIC_ALL, nullptr, &h)) &&
			SUCCEEDED(dev5->OpenSharedFence(h, IID_PPV_ARGS(reinterpret_cast<ID3D11Fence **>(&ready11_))));
		if (h != nullptr)
			CloseHandle(h);
		h = nullptr;
		ok = ok && SUCCEEDED(dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(reinterpret_cast<ID3D11Fence **>(&done11_)))) &&
			SUCCEEDED(static_cast<ID3D11Fence *>(done11_)->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)) &&
			SUCCEEDED(native_->OpenSharedHandle(h, IID_PPV_ARGS(&done12_)));
		if (h != nullptr)
			CloseHandle(h);
		if (!ok) {
			factory->Release();
			*why = L"frame generation: the textures shared between its two devices could not be made";
			return false;
		}
	}
	DXGI_SWAP_CHAIN_DESC1 sd{};
	sd.Width = w_;
	sd.Height = h_;
	sd.Format = swap_format_;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 3;
	sd.Scaling = DXGI_SCALING_STRETCH;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
	IDXGISwapChain1 *sc1 = nullptr;
	const HRESULT hr = factory->CreateSwapChainForComposition(dev5, &sd, nullptr, &sc1);
	factory->Release();
	if (FAILED(hr) || FAILED(sc1->QueryInterface(IID_PPV_ARGS(&swap_)))) {
		release(sc1);
		*why = L"frame generation: its swap chain could not be made";
		return false;
	}
	swap_->SetMaximumFrameLatency(1);
	waitable_ = swap_->GetFrameLatencyWaitableObject();
	if (color_space_ == VendorColor::Scrgb)
		swap_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
	else if (color_space_ == VendorColor::Pq)
		swap_->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);

	dcomp_module_ = LoadLibraryW(L"dcomp.dll");
	const auto create = dcomp_module_ != nullptr
		? reinterpret_cast<CreateDevice2>(GetProcAddress(dcomp_module_, "DCompositionCreateDevice2")) : nullptr;
	IDCompositionDesktopDevice *dev = nullptr;
	IDCompositionTarget *target = nullptr;
	IDCompositionVisual2 *visual = nullptr;
	if (create == nullptr || FAILED(create(nullptr, IID_PPV_ARGS(&dev))) ||
		FAILED(dev->CreateTargetForHwnd(window_, TRUE, &target)) || FAILED(dev->CreateVisual(&visual)) ||
		FAILED(visual->SetContent(sc1)) || FAILED(target->SetRoot(visual)) || FAILED(dev->Commit())) {
		release(visual);
		release(target);
		release(dev);
		release(sc1);
		*why = L"frame generation: DirectComposition would not show its swap chain";
		return false;
	}
	release(sc1);
	dcomp_device_ = dev;
	dcomp_target_ = target;
	dcomp_visual_ = visual;
	return true;
}

void OverlayPresenter::wait_sink_done() noexcept
{
	if (host_ != nullptr) {
		if (done12_ != nullptr && done_value_ != 0 && done12_->GetCompletedValue() < done_value_ &&
			fence_event_ != nullptr && SUCCEEDED(done12_->SetEventOnCompletion(done_value_, fence_event_)))
			WaitForSingleObject(fence_event_, 1000);
		return;
	}
	if (ctx11_ != nullptr && done11_ != nullptr && done_value_ != 0) {
		auto *const ctx = static_cast<ID3D11DeviceContext4 *>(ctx11_);
		auto *const done = static_cast<ID3D11Fence *>(done11_);
		const HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		ctx->Flush();
		if (ev != nullptr && done->GetCompletedValue() < done_value_ && SUCCEEDED(done->SetEventOnCompletion(done_value_, ev)))
			WaitForSingleObject(ev, 1000);
		if (ev != nullptr)
			CloseHandle(ev);
	}
}

void OverlayPresenter::release_sink(bool gpu_idle) noexcept
{
	if (host_ != nullptr) {
		host_->stop(!host_->alive());
		host_.reset();
		if (done12_ != nullptr && done_value_ != 0 && done12_->GetCompletedValue() < done_value_)
			done12_->Signal(done_value_ + kDoneMargin);
	}
	if (!gpu_idle) {
		diag_logf(DiagLevel::Warn, "framegen", L"frame generation: its queue did not finish; the textures shared "
			L"with the window are left allocated");
		for (ID3D12Resource *&r : shared12_)
			r = nullptr;
		ready12_ = nullptr;
		done12_ = nullptr;
	}
	if (dcomp_visual_ != nullptr)
		static_cast<IDCompositionVisual2 *>(dcomp_visual_)->Release();
	if (dcomp_target_ != nullptr)
		static_cast<IDCompositionTarget *>(dcomp_target_)->Release();
	if (dcomp_device_ != nullptr)
		static_cast<IDCompositionDesktopDevice *>(dcomp_device_)->Release();
	dcomp_visual_ = dcomp_target_ = dcomp_device_ = nullptr;
	if (waitable_ != nullptr)
		CloseHandle(waitable_);
	waitable_ = nullptr;
	release(swap_);
	for (uint32_t i = 0; i < kShared; ++i) {
		release(shared12_[i]);
		if (shared11_[i] != nullptr)
			static_cast<ID3D11Texture2D *>(shared11_[i])->Release();
		shared11_[i] = nullptr;
		shared_free_[i] = 0;
	}
	shared_at_ = 0;
	release(ready12_);
	release(done12_);
	if (ready11_ != nullptr)
		static_cast<ID3D11Fence *>(ready11_)->Release();
	if (done11_ != nullptr)
		static_cast<ID3D11Fence *>(done11_)->Release();
	ready11_ = done11_ = nullptr;
	ready_value_ = done_value_ = 0;
	if (ctx11_ != nullptr) {
		static_cast<ID3D11DeviceContext4 *>(ctx11_)->ClearState();
		static_cast<ID3D11DeviceContext4 *>(ctx11_)->Flush();
		static_cast<ID3D11DeviceContext4 *>(ctx11_)->Release();
	}
	ctx11_ = nullptr;
	if (d3d11_ != nullptr)
		static_cast<ID3D11Device5 *>(d3d11_)->Release();
	d3d11_ = nullptr;
	if (d3d11_wrapper_ != nullptr)
		static_cast<ID3D11Device *>(d3d11_wrapper_)->Release();
	d3d11_wrapper_ = nullptr;
	if (d3d11_module_ != nullptr)
		FreeLibrary(d3d11_module_);
	d3d11_module_ = nullptr;
	if (window_ != nullptr)
		DestroyWindow(window_);
	window_ = nullptr;
	if (dcomp_module_ != nullptr)
		FreeLibrary(dcomp_module_);
	dcomp_module_ = nullptr;
	if (dxgi_module_ != nullptr)
		FreeLibrary(dxgi_module_);
	dxgi_module_ = nullptr;
	if (dpi_before_ != nullptr) {
		using SetContext = DPI_AWARENESS_CONTEXT(WINAPI *)(DPI_AWARENESS_CONTEXT);
		const HMODULE user = GetModuleHandleW(L"user32.dll");
		const auto set = user != nullptr ? reinterpret_cast<SetContext>(GetProcAddress(user, "SetThreadDpiAwarenessContext"))
			: nullptr;
		if (set != nullptr)
			set(static_cast<DPI_AWARENESS_CONTEXT>(dpi_before_));
		dpi_before_ = nullptr;
	}
	host_answer_ = WindowHostAnswer{};
	stats_count_ = 0;
	for (ShownEntry &e : shown_ring_)
		e = ShownEntry{};
	shown_ring_at_ = 0;
	offset_n_ = offset_at_ = 0;
	shown_ = false;
	shown_rect_ = RECT{};
}

void OverlayPresenter::lose_host(uint64_t promised) noexcept
{
	if (host_ == nullptr || host_lost_)
		return;
	const std::wstring why = host_->error();
	host_->stop(true);
	done_value_ = std::max(done_value_, promised) + kDoneMargin;
	if (done12_ != nullptr)
		done12_->Signal(done_value_);
	host_lost_ = true;
	++host_failures_;
	diag_logf(DiagLevel::Warn, "framegen", L"frame generation: AeonSRHost.exe stopped showing its own window (%ls); "
		L"shown from the game's process from now on", why.c_str());
}

bool OverlayPresenter::switch_to_local()
{
	host_lost_ = false;
	bool idle = read_fence_ == nullptr || read_value_ == 0 || read_fence_->GetCompletedValue() >= read_value_;
	if (!idle && SUCCEEDED(read_fence_->SetEventOnCompletion(read_value_, fence_event_))) {
		WaitForSingleObject(fence_event_, 5000);
		idle = read_fence_->GetCompletedValue() >= read_value_;
	}
	release_sink(idle);
	std::wstring why;
	if (init_local(&why))
		return true;
	diag_logf(DiagLevel::Warn, "framegen", L"frame generation: its own window could not be shown from the game's "
		L"process either: %ls", why.c_str());
	return false;
}

void OverlayPresenter::thread_shutdown() noexcept
{
	if (queue_ != nullptr && read_fence_ != nullptr && read_value_ != 0 &&
		read_fence_->GetCompletedValue() < read_value_ && fence_event_ != nullptr &&
		SUCCEEDED(read_fence_->SetEventOnCompletion(read_value_, fence_event_)))
		WaitForSingleObject(fence_event_, 1000);
	interp_.release();
	blit_.release();
	wait_sink_done();
	release_sink();
	host_lost_ = false;
	release(list_);
	for (ID3D12CommandAllocator *&a : alloc_)
		release(a);
	for (uint64_t &d : alloc_done_)
		d = 0;
	alloc_at_ = 0;
	release(queue_);
	if (fence_event_ != nullptr)
		CloseHandle(fence_event_);
	fence_event_ = nullptr;
	release(native_);
	scheduler_.reset();
	waiting_.clear();
	last_ = Last::None;
	last_index_ = 0;
	made_shown_ = 0;
	late_ = 0;
	pace_.reset();
	if (timer_ != nullptr)
		CloseHandle(timer_);
	timer_ = nullptr;
	shown_ = had_content_ = false;
	wake_n_ = wake_at_ = 0;
	last_wake_ = next_display_ = 0.0;
}

void OverlayPresenter::read_frame_statistics()
{
	static const double inv = [] {
		LARGE_INTEGER f{};
		QueryPerformanceFrequency(&f);
		return 1000.0 / static_cast<double>(f.QuadPart);
	}();
	DXGI_FRAME_STATISTICS fs{};
	if (host_ != nullptr) {
		if (!host_answer_.stats_valid)
			return;
		fs.PresentCount = host_answer_.stats_present_count;
		fs.SyncQPCTime.QuadPart = host_answer_.stats_sync_qpc;
	} else if (swap_ == nullptr || FAILED(swap_->GetFrameStatistics(&fs))) {
		return;
	}
	if (fs.PresentCount == stats_count_ || fs.SyncQPCTime.QuadPart == 0)
		return;
	stats_count_ = fs.PresentCount;
	const double sync_ms = static_cast<double>(fs.SyncQPCTime.QuadPart) * inv;
	for (const ShownEntry &e : shown_ring_) {
		if (e.count != fs.PresentCount)
			continue;
		if (e.planned_ms > 0.0 && paced_period_ > 0.0) {
			const double offset = sync_ms - e.planned_ms;
			offsets_[offset_at_] = offset;
			offset_at_ = (offset_at_ + 1) % kOffsets;
			offset_n_ = std::min(offset_n_ + 1, kOffsets);
			if (offset_n_ >= 16) {
				double o[kOffsets];
				std::copy(offsets_, offsets_ + offset_n_, o);
				std::nth_element(o, o + offset_n_ / 2, o + offset_n_);
				if (offset > o[offset_n_ / 2] + 0.5 * paced_period_)
					++late_;
			}
		}
		if (e.content_ms <= 0.0)
			break;
		const double ms = sync_ms - e.content_ms;
		if (ms > 0.0 && ms < 1000.0) {
			screen_all_.push_back(ms);
			if (e.real)
				screen_real_.push_back(ms);
		}
		break;
	}
}

void OverlayPresenter::wait_until(double t_ms) noexcept
{
	const double left = t_ms - qpc_ms();
	if (left <= 0.0)
		return;
	LARGE_INTEGER due{};
	due.QuadPart = -static_cast<LONGLONG>(left * 10000.0);
	if (timer_ != nullptr && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
		WaitForSingleObject(timer_, 100);
	else
		Sleep(static_cast<DWORD>(std::ceil(left)));
}

void OverlayPresenter::follow_window()
{
	const HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (fg != nullptr)
		GetWindowThreadProcessId(fg, &pid);
	static const bool always = [] {
		wchar_t v[8] = {};
		return GetEnvironmentVariableW(L"AEONSR_FG_OVERLAY_ALWAYS", v, 8) != 0 && v[0] == L'1';
	}();
	const bool game_in_front = IsWindow(game_window_) && !IsIconic(game_window_) &&
		(always || pid == GetCurrentProcessId());
	if (!game_in_front || !had_content_) {
		if (shown_) {
			if (host_ != nullptr) {
				if (!host_->place(false, 0, 0, 0, 0))
					lose_host(done_value_);
			} else {
				ShowWindow(window_, SW_HIDE);
			}
			shown_ = false;
		}
		return;
	}
	RECT client{};
	POINT origin{ 0, 0 };
	if (!GetClientRect(game_window_, &client) || !ClientToScreen(game_window_, &origin) || client.right <= 0 ||
		client.bottom <= 0)
		return;
	const RECT r{ origin.x, origin.y, origin.x + client.right, origin.y + client.bottom };
	if (!shown_ || !EqualRect(&r, &shown_rect_)) {
		if (host_ != nullptr) {
			if (!host_->place(true, r.left, r.top, client.right, client.bottom)) {
				lose_host(done_value_);
				return;
			}
			shown_rect_ = r;
			shown_ = true;
			return;
		}
		auto *const visual = static_cast<IDCompositionVisual2 *>(dcomp_visual_);
		D2D_MATRIX_3X2_F m{};
		m._11 = static_cast<float>(client.right) / static_cast<float>(w_);
		m._22 = static_cast<float>(client.bottom) / static_cast<float>(h_);
		visual->SetTransform(m);
		static_cast<IDCompositionDesktopDevice *>(dcomp_device_)->Commit();
		SetWindowPos(window_, HWND_TOPMOST, r.left, r.top, client.right, client.bottom,
			SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
		shown_rect_ = r;
		shown_ = true;
	}
}

bool OverlayPresenter::render(const OverlayChoice &c)
{
	ID3D12Resource *src = nullptr;
	DXGI_FORMAT src_format = family_;
	bool from_interp = false;
	showing_made_ = false;
	shown_new_ = false;
	uint32_t used[2] = { kSlots, kSlots };
	const uint64_t reserve = read_value_ + 1;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		const auto slot_of = [&](uint64_t id) -> Slot * {
			Slot &s = slots_[id % kSlots];
			return s.id == id ? &s : nullptr;
		};
		if (c.kind == OverlayChoice::Kind::Real) {
			if (Slot *s = slot_of(c.b)) {
				src = s->color;
				used[0] = static_cast<uint32_t>(c.b % kSlots);
				last_ = Last::Real;
				last_id_ = c.b;
			}
		} else if (c.kind == OverlayChoice::Kind::Generated && c.made) {
			Slot *b = slot_of(c.b);
			if (b != nullptr && c.index >= 1 && c.index <= kOverlayMaxMade && b->made[c.index - 1] != nullptr) {
				src = b->made[c.index - 1];
				src_format = b->made_format;
				used[0] = static_cast<uint32_t>(c.b % kSlots);
				showing_made_ = true;
				last_ = Last::Made;
				last_id_ = c.b;
				last_index_ = c.index;
			}
		} else if (c.kind == OverlayChoice::Kind::Generated) {
			Slot *a = slot_of(c.a), *b = slot_of(c.b);
			bool building = false;
			std::wstring err;
			if (a != nullptr && b != nullptr && interp_.ensure(native_, w_, h_, family_, &building, &err)) {
				used[0] = static_cast<uint32_t>(c.a % kSlots);
				used[1] = static_cast<uint32_t>(c.b % kSlots);
				from_interp = true;
			}
		}
		if (src == nullptr && !from_interp) {
			if (last_ == Last::Real) {
				if (Slot *s = slot_of(last_id_)) {
					src = s->color;
					used[0] = static_cast<uint32_t>(last_id_ % kSlots);
				}
			} else if (last_ == Last::Generated) {
				src = interp_.output();
			} else if (last_ == Last::Made) {
				Slot *s = slot_of(last_id_);
				if (s != nullptr && last_index_ >= 1 && s->made[last_index_ - 1] != nullptr) {
					src = s->made[last_index_ - 1];
					src_format = s->made_format;
					used[0] = static_cast<uint32_t>(last_id_ % kSlots);
				}
			}
		}
		for (uint32_t u : used)
			if (u < kSlots)
				slots_[u].read_until = reserve;
	}
	const auto signal = [&]() {
		if (SUCCEEDED(queue_->Signal(read_fence_, reserve)))
			read_value_ = reserve;
	};
	if (src == nullptr && !from_interp) {
		signal();
		return false;
	}

	if (!(alloc_done_[alloc_at_] == 0 || read_fence_->GetCompletedValue() >= alloc_done_[alloc_at_])) {
		if (FAILED(read_fence_->SetEventOnCompletion(alloc_done_[alloc_at_], fence_event_)))
			return false;
		WaitForSingleObject(fence_event_, 1000);
	}
	if (FAILED(alloc_[alloc_at_]->Reset()) || FAILED(list_->Reset(alloc_[alloc_at_], nullptr))) {
		signal();
		return false;
	}

	if (from_interp) {
		Slot &a = slots_[used[0]];
		Slot &b = slots_[used[1]];
		FrameInterpolatorD3D12::Inputs in;
		in.previous = a.color;
		in.previous_state = OverlayFeed::kRestState;
		in.current = b.color;
		in.current_state = OverlayFeed::kRestState;
		in.motion = b.motion;
		in.motion_state = OverlayFeed::kRestState;
		in.global_flow = b.global;
		in.global_state = OverlayFeed::kRestState;
		in.t = c.t;
		std::wstring err;
		if (!interp_.record(list_, in, &err)) {
			list_->Close();
			signal();
			return false;
		}
		src = interp_.output();
		last_ = Last::Generated;
	}

	const uint32_t si = shared_at_;
	ID3D12Resource *const back = shared12_[si];
	const bool interp_src = src == interp_.output();
	const D3D12_RESOURCE_STATES src_state = interp_src ? FrameInterpolatorD3D12::kOutputState : OverlayFeed::kRestState;
	const bool copies = interp_src ? interp_.copies_into(swap_format_)
		: format_family(src_format) == format_family(swap_format_);
	if (copies) {
		barrier(list_, src, src_state, interp_src ? D3D12_RESOURCE_STATE_COPY_SOURCE : src_state);
		barrier(list_, back, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
		list_->CopyResource(back, src);
		barrier(list_, back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		barrier(list_, src, interp_src ? D3D12_RESOURCE_STATE_COPY_SOURCE : src_state, src_state);
	} else {
		const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		barrier(list_, src, src_state, read);
		barrier(list_, back, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
		blit_.draw_fullscreen(native_, list_, src, back, swap_format_, w_, h_);
		barrier(list_, back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
		barrier(list_, src, read, src_state);
	}
	if (FAILED(list_->Close())) {
		signal();
		return false;
	}
	if (shared_free_[si] != 0)
		queue_->Wait(done12_, shared_free_[si]);
	ID3D12CommandList *lists[] = { list_ };
	queue_->ExecuteCommandLists(1, lists);
	queue_->Signal(ready12_, ++ready_value_);
	HRESULT presented = E_FAIL;
	UINT count = 0;
	bool counted = false;
	if (host_ != nullptr) {
		signal();
		const uint64_t promised = done_value_ + 1;
		WindowHostAnswer a;
		if (!host_->present(si, ready_value_, promised, 1, &a) || FAILED(a.present_hr)) {
			if (host_->alive())
				diag_logf(DiagLevel::Warn, "framegen", L"frame generation: AeonSRHost.exe's present failed (0x%08X)",
					static_cast<unsigned>(a.present_hr));
			lose_host(promised);
			alloc_done_[alloc_at_] = reserve;
			alloc_at_ = (alloc_at_ + 1) % kLists;
			return false;
		}
		done_value_ = promised;
		host_answer_ = a;
		presented = a.present_hr;
		count = a.present_count;
		counted = true;
	} else {
		auto *const ctx = static_cast<ID3D11DeviceContext4 *>(ctx11_);
		ctx->Wait(static_cast<ID3D11Fence *>(ready11_), ready_value_);
		ID3D11Texture2D *back11 = nullptr;
		if (SUCCEEDED(swap_->GetBuffer(0, IID_PPV_ARGS(&back11)))) {
			ctx->CopyResource(back11, static_cast<ID3D11Texture2D *>(shared11_[si]));
			back11->Release();
		}
		ctx->Signal(static_cast<ID3D11Fence *>(done11_), ++done_value_);
		presented = swap_->Present(1, 0);
		counted = SUCCEEDED(swap_->GetLastPresentCount(&count));
	}
	shared_free_[si] = done_value_;
	shared_at_ = (si + 1) % kShared;
	if (SUCCEEDED(presented) && c.kind == OverlayChoice::Kind::Real && c.held_ms >= 0.0)
		held_real_.push_back(c.held_ms);
	if (c.content_ms >= 0.0 && counted) {
		shown_ring_[shown_ring_at_] = ShownEntry{ count, c.content_ms, c.kind == OverlayChoice::Kind::Real,
			planned_display_ };
		shown_ring_at_ = (shown_ring_at_ + 1) % kShownRing;
	}
	if (showing_made_)
		++made_shown_;
	shown_new_ = from_interp || showing_made_;
	signal();
	alloc_done_[alloc_at_] = reserve;
	alloc_at_ = (alloc_at_ + 1) % kLists;
	return true;
}

void OverlayPresenter::run()
{
	t_overlay_thread = true;
	{
		wchar_t v[8] = {};
		pace_.set_decide_early(GetEnvironmentVariableW(L"AEONSR_FG_DECIDE_EARLY", v, 8) != 0 && v[0] == L'1');
		wchar_t l[16] = {};
		if (GetEnvironmentVariableW(L"AEONSR_FG_LEAD_MS", l, 16) != 0)
			pace_.set_lead(_wtof(l));
	}
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
	std::wstring why;
	const bool ok = thread_init(&why);
	init_error_ = why;
	if (!ok)
		diag_logf(DiagLevel::Warn, "framegen", L"frame generation: its own window failed: %ls", why.c_str());
	init_ok_.store(ok, std::memory_order_release);
	init_done_.store(true, std::memory_order_release);
	if (!ok) {
		thread_shutdown();
		return;
	}
	if (!start_sink()) {
		init_ok_.store(false, std::memory_order_release);
		sink_failed_.store(true, std::memory_order_release);
		thread_shutdown();
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pending_.clear();
		stats_.running = true;
		stats_.in_host = host_ != nullptr;
		stats_.host_pid = host_ != nullptr ? host_->host_pid() : 0u;
	}
	diag_logf(DiagLevel::Info, "framegen", L"frame generation: its own window started, shown from %ls",
		host_ != nullptr ? L"AeonSRHost.exe" : L"the game's process");
	second_start_ = qpc_ms();
	second_reals_ = second_generated_ = 0;
	kicks_left_ = 16;
	next_kick_ = 0.0;
	bool armed = true;
	double wait_period = 1000.0 / 60.0;
	while (!quit_.load(std::memory_order_acquire)) {
		const HANDLE released = host_ != nullptr ? host_->released() : waitable_;
		WaitForSingleObjectEx(released, armed ? 100 : static_cast<DWORD>(std::ceil(wait_period)), FALSE);
		MSG msg;
		while (PeekMessageW(&msg, window_, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (reset_pending_) {
				scheduler_.reset();
				waiting_.clear();
				reset_pending_ = false;
			}
			waiting_.insert(waiting_.end(), pending_.begin(), pending_.end());
			pending_.clear();
		}
		const double now = qpc_ms();
		if (last_wake_ > 0.0) {
			const double gap = now - last_wake_;
			if (gap > 2.0 && gap < 50.0) {
				wakes_[wake_at_] = gap;
				wake_at_ = (wake_at_ + 1) % kWakes;
				wake_n_ = std::min(wake_n_ + 1, kWakes);
			}
		}
		last_wake_ = now;
		double period = 1000.0 / 60.0;
		if (wake_n_ >= 8) {
			double w[kWakes];
			std::copy(wakes_, wakes_ + wake_n_, w);
			std::sort(w, w + wake_n_);
			period = w[wake_n_ / 2];
		} else if (clock_ != nullptr) {
			const DisplayGrid grid = clock_->grid();
			if (grid.valid && grid.period_ms > 0.0)
				period = grid.period_ms;
		}
		const double target = now + period;
		if (next_display_ <= 0.0 || std::fabs(target - (next_display_ + period)) > 0.5 * period)
			next_display_ = target;
		else
			next_display_ += period + 0.05 * (target - (next_display_ + period));
		double display = next_display_;
		double vblank = 0.0, refresh = 0.0;
		uint8_t source = OverlayStats::kPacedByReleases;
		if (clock_ != nullptr) {
			const DisplayGrid grid = clock_->grid();
			if (grid.valid && grid.period_ms > 2.0 && grid.vblank_ms > 0.0) {
				vblank = grid.vblank_ms;
				refresh = grid.period_ms;
				source = OverlayStats::kPacedByMonitor;
			}
		}
		if (source == OverlayStats::kPacedByReleases && compositor_clock(&vblank, &refresh))
			source = OverlayStats::kPacedByCompositor;
		const bool by_compositor = source != OverlayStats::kPacedByReleases;
		if (by_compositor) {
			double at = now;
			display = pace_.plan(now, vblank, refresh, &at);
			period = refresh;
			wait_until(at);
			planned_display_ = display;
			paced_period_ = refresh;
		} else {
			planned_display_ = 0.0;
		}
		wait_period = period;
		size_t taken = 0;
		while (taken < waiting_.size() && feed_fence_->GetCompletedValue() >= waiting_[taken].fence_value) {
			const Pending &w = waiting_[taken];
			scheduler_.add(w.id, w.presented_ms, w.pairs, display, w.steps, w.made, w.vendor);
			++taken;
		}
		waiting_.erase(waiting_.begin(), waiting_.begin() + static_cast<long>(taken));
		const OverlayChoice c = scheduler_.choose(display, period);
		const bool presented = (shown_ || !had_content_) && render(c);
		armed = presented;
		if (presented && by_compositor)
			pace_.presented();
		if (presented) {
			had_content_ = true;
			if (c.kind == OverlayChoice::Kind::Real)
				++second_reals_;
			else if (shown_new_)
				++second_generated_;
		} else {
			Sleep(1);
		}
		follow_window();
		read_frame_statistics();
		if (host_ != nullptr && kicks_left_ > 0 && now >= next_kick_) {
			give_overlays_their_event();
			--kicks_left_;
			next_kick_ = now + 2000.0;
		}
		if (host_lost_ && !switch_to_local()) {
			init_ok_.store(false, std::memory_order_release);
			sink_failed_.store(true, std::memory_order_release);
			break;
		}

		if (now - second_start_ >= 1000.0) {
			const double secs = (now - second_start_) / 1000.0;
			std::lock_guard<std::mutex> lock(mutex_);
			stats_.running = true;
			stats_.visible = shown_;
			stats_.latency_ms = scheduler_.latency_ms();
			stats_.usable_lag_ms = scheduler_.usable_lag_ms();
			stats_.reals = scheduler_.reals_shown();
			stats_.skipped = scheduler_.reals_skipped();
			stats_.generated = scheduler_.generated();
			stats_.held = scheduler_.held();
			stats_.repeated = scheduler_.repeated();
			stats_.made_shown = made_shown_;
			const auto pct = [](std::vector<double> &v, double q) {
				if (v.empty())
					return 0.0;
				std::sort(v.begin(), v.end());
				return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))];
			};
			stats_.screen_ms_p50 = pct(screen_all_, 0.5);
			stats_.screen_real_ms_p50 = pct(screen_real_, 0.5);
			stats_.screen_real_ms_p90 = pct(screen_real_, 0.9);
			stats_.screen_samples = static_cast<uint32_t>(screen_all_.size());
			stats_.held_real_ms_p50 = pct(held_real_, 0.5);
			stats_.lead_ms = pace_.lead_ms();
			stats_.missed = pace_.misses();
			if (offset_n_ > 0) {
				double o[kOffsets];
				std::copy(offsets_, offsets_ + offset_n_, o);
				std::sort(o, o + offset_n_);
				stats_.offset_p50_ms = o[offset_n_ / 2];
				stats_.offset_min_ms = o[0];
			}
			stats_.late = late_;
			screen_all_.clear();
			screen_real_.clear();
			held_real_.clear();
			stats_.period_ms = period;
			stats_.compositor_paced = by_compositor;
			stats_.paced_by = source;
			stats_.in_host = host_ != nullptr;
			stats_.host_pid = host_ != nullptr ? host_->host_pid() : 0u;
			stats_.real_per_s = second_reals_ / secs;
			stats_.generated_per_s = second_generated_ / secs;
			second_start_ = now;
			second_reals_ = second_generated_ = 0;
		}
	}
	thread_shutdown();
	std::lock_guard<std::mutex> lock(mutex_);
	stats_.running = false;
	stats_.visible = false;
	stats_.in_host = false;
	stats_.host_pid = 0;
}

}
