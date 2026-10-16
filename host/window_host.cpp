#include "window_host.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/framegen/window_host_protocol.hpp"

#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace aeon_sr::host {

namespace {

using namespace aeon_sr::window_host;

enum Exit : int {
	ExitOk = 0,
	ExitNoBlock = 20,
	ExitSetup = 21,
	ExitWaitFailed = 22,
};

template <typename T>
void release(T *&p) noexcept
{
	if (p != nullptr)
		p->Release();
	p = nullptr;
}

HANDLE as_handle(uint64_t v) noexcept
{
	return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(v));
}

void close(HANDLE &h) noexcept
{
	if (h != nullptr)
		CloseHandle(h);
	h = nullptr;
}

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

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_NCHITTEST: return HTTRANSPARENT;
	case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
	case WM_CLOSE: return 0;
	default: return DefWindowProcW(hwnd, msg, wp, lp);
	}
}

void physical_pixels()
{
	using SetContext = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
	const HMODULE user = GetModuleHandleW(L"user32.dll");
	const auto set = user != nullptr
		? reinterpret_cast<SetContext>(GetProcAddress(user, "SetProcessDpiAwarenessContext")) : nullptr;
	if (set != nullptr)
		set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

class WindowHost {
public:
	~WindowHost() { shutdown(); }
	bool start(Block *block, std::wstring *why);
	int serve();
	void shutdown() noexcept;

private:
	void answer(const Request &r) noexcept;
	void present(const Request &r) noexcept;
	void place(const Request &r) noexcept;
	void note_stats() noexcept;

	Block *block_ = nullptr;
	Setup setup_;
	HANDLE game_ = nullptr, request_ = nullptr, response_ = nullptr, released_ = nullptr;
	HWND window_ = nullptr;
	ID3D11Device5 *device_ = nullptr;
	ID3D11DeviceContext4 *ctx_ = nullptr;
	ID3D11Texture2D *shared_[kShared] = {};
	ID3D11Fence *ready_ = nullptr, *done_ = nullptr;
	IDXGISwapChain2 *swap_ = nullptr;
	HANDLE waitable_ = nullptr;
	bool waiting_release_ = false;
	bool shown_ = false;
	RECT shown_rect_{};
	uint64_t presents_ = 0, failed_presents_ = 0, moves_ = 0, release_timeouts_ = 0;
	std::vector<double> present_ms_;
	double since_ = 0.0;
};

bool WindowHost::start(Block *block, std::wstring *why)
{
	block_ = block;
	const uint32_t magic = block->magic, version = block->version, size = block->block_size;
	if (magic != kMagic || version != kVersion || size != kBlockSize) {
		*why = L"the add-on and AeonSRHost.exe are from different builds";
		return false;
	}
	game_ = as_handle(block->game_process);
	request_ = as_handle(block->request_event);
	response_ = as_handle(block->response_event);
	released_ = as_handle(block->released_event);
	HANDLE shared[kShared];
	for (uint32_t i = 0; i < kShared; ++i)
		shared[i] = as_handle(block->shared[i]);
	HANDLE ready = as_handle(block->ready_fence), done = as_handle(block->done_fence);
	const auto close_given = [&]() {
		for (HANDLE &h : shared)
			close(h);
		close(ready);
		close(done);
	};
	const wchar_t *bad = nullptr;
	if (!read_setup(*block, &setup_, &bad)) {
		*why = bad;
		close_given();
		return false;
	}
	if (game_ == nullptr || request_ == nullptr || response_ == nullptr || released_ == nullptr) {
		*why = L"the game handed over no events";
		close_given();
		return false;
	}
	physical_pixels();

	const HINSTANCE inst = GetModuleHandleW(nullptr);
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = window_proc;
	wc.hInstance = inst;
	wc.lpszClassName = L"AeonSRFrameGenerationWindow";
	RegisterClassExW(&wc);
	window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
		wc.lpszClassName, L"Aeon SR frame generation", WS_POPUP, 0, 0, static_cast<int>(setup_.width),
		static_cast<int>(setup_.height), nullptr, nullptr, inst, nullptr);
	if (window_ == nullptr) {
		*why = L"its window could not be made";
		close_given();
		return false;
	}
	SetLayeredWindowAttributes(window_, 0, 255, LWA_ALPHA);
	block_->host_window = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(window_));

	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter1 *adapter = nullptr;
	LUID luid{};
	luid.LowPart = setup_.adapter_luid_low;
	luid.HighPart = setup_.adapter_luid_high;
	if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
		release(factory);
		*why = L"the game's graphics card was not found";
		close_given();
		return false;
	}
	static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	const HRESULT hr_dev = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		levels, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
	adapter->Release();
	if (SUCCEEDED(hr_dev)) {
		dev->QueryInterface(IID_PPV_ARGS(&device_));
		ctx->QueryInterface(IID_PPV_ARGS(&ctx_));
	}
	release(ctx);
	release(dev);
	if (device_ == nullptr || ctx_ == nullptr) {
		release(factory);
		*why = L"no Direct3D 11.4 device";
		close_given();
		return false;
	}
	bool opened = true;
	for (uint32_t i = 0; i < kShared && opened; ++i)
		opened = SUCCEEDED(device_->OpenSharedResource1(shared[i], IID_PPV_ARGS(&shared_[i])));
	opened = opened && SUCCEEDED(device_->OpenSharedFence(ready, IID_PPV_ARGS(&ready_))) &&
		SUCCEEDED(device_->OpenSharedFence(done, IID_PPV_ARGS(&done_)));
	close_given();
	if (!opened) {
		release(factory);
		*why = L"the game's shared textures and fences could not be opened";
		return false;
	}

	DXGI_SWAP_CHAIN_DESC1 sd{};
	sd.Width = setup_.width;
	sd.Height = setup_.height;
	sd.Format = static_cast<DXGI_FORMAT>(setup_.format);
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = 3;
	sd.Scaling = DXGI_SCALING_STRETCH;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
	IDXGISwapChain1 *sc1 = nullptr;
	const HRESULT hr_sc = factory->CreateSwapChainForHwnd(device_, window_, &sd, nullptr, nullptr, &sc1);
	if (SUCCEEDED(hr_sc))
		factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
	factory->Release();
	if (FAILED(hr_sc) || FAILED(sc1->QueryInterface(IID_PPV_ARGS(&swap_)))) {
		release(sc1);
		wchar_t buf[96];
		swprintf(buf, 96, L"its swap chain could not be made (0x%08X)", static_cast<unsigned>(hr_sc));
		*why = buf;
		return false;
	}
	release(sc1);
	swap_->SetMaximumFrameLatency(1);
	waitable_ = swap_->GetFrameLatencyWaitableObject();
	IDXGISwapChain3 *sc3 = nullptr;
	if (setup_.color_space != ColorSpace::Sdr && SUCCEEDED(swap_->QueryInterface(IID_PPV_ARGS(&sc3)))) {
		sc3->SetColorSpace1(setup_.color_space == ColorSpace::Scrgb ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
			: DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
		sc3->Release();
	}
	if (waitable_ != nullptr)
		WaitForSingleObject(waitable_, 100);
	SetEvent(released_);
	since_ = qpc_ms();
	return true;
}

void WindowHost::answer(const Request &r) noexcept
{
	block_->answered_seq = r.seq;
	MemoryBarrier();
	SetEvent(response_);
}

void WindowHost::present(const Request &r) noexcept
{
	const double t0 = qpc_ms();
	ctx_->Wait(ready_, r.ready_value);
	ID3D11Texture2D *back = nullptr;
	if (SUCCEEDED(swap_->GetBuffer(0, IID_PPV_ARGS(&back)))) {
		ctx_->CopyResource(back, shared_[r.shared_index]);
		back->Release();
	}
	ctx_->Signal(done_, r.done_value);
	const HRESULT hr = swap_->Present(r.sync_interval, 0);
	UINT count = 0;
	swap_->GetLastPresentCount(&count);
	DXGI_FRAME_STATISTICS fs{};
	const bool stats = SUCCEEDED(swap_->GetFrameStatistics(&fs));
	block_->present_hr = static_cast<int32_t>(hr);
	block_->present_count = count;
	block_->stats_valid = stats ? 1u : 0u;
	block_->stats_present_count = stats ? fs.PresentCount : 0u;
	block_->stats_present_refresh_count = stats ? fs.PresentRefreshCount : 0u;
	block_->stats_sync_refresh_count = stats ? fs.SyncRefreshCount : 0u;
	block_->stats_sync_qpc = stats ? fs.SyncQPCTime.QuadPart : 0;
	block_->window_shown = shown_ ? 1u : 0u;
	answer(r);
	waiting_release_ = SUCCEEDED(hr);
	if (SUCCEEDED(hr))
		++presents_;
	else
		++failed_presents_;
	present_ms_.push_back(qpc_ms() - t0);
}

void WindowHost::place(const Request &r) noexcept
{
	if (!r.visible) {
		if (shown_)
			ShowWindow(window_, SW_HIDE);
		shown_ = false;
	} else {
		const RECT rect{ r.x, r.y, r.x + r.cw, r.y + r.ch };
		if (!shown_ || !EqualRect(&rect, &shown_rect_)) {
			SetWindowPos(window_, HWND_TOPMOST, r.x, r.y, r.cw, r.ch, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
			shown_rect_ = rect;
			shown_ = true;
			++moves_;
		}
	}
	block_->present_hr = 0;
	block_->window_shown = shown_ ? 1u : 0u;
	answer(r);
}

void WindowHost::note_stats() noexcept
{
	const double now = qpc_ms();
	if (now - since_ < 10000.0)
		return;
	double p50 = 0.0, pmax = 0.0;
	if (!present_ms_.empty()) {
		std::sort(present_ms_.begin(), present_ms_.end());
		p50 = present_ms_[present_ms_.size() / 2];
		pmax = present_ms_.back();
	}
	diag_logf(DiagLevel::Info, "window", L"last %.1f s: %llu presents (%llu failed), each %.2f ms (slowest %.2f), "
		L"%llu releases not seen within 100 ms, window moved %llu times, %ls", (now - since_) / 1000.0,
		static_cast<unsigned long long>(presents_), static_cast<unsigned long long>(failed_presents_), p50, pmax,
		static_cast<unsigned long long>(release_timeouts_), static_cast<unsigned long long>(moves_),
		shown_ ? L"shown" : L"hidden");
	presents_ = failed_presents_ = moves_ = release_timeouts_ = 0;
	present_ms_.clear();
	since_ = now;
}

int WindowHost::serve()
{
	for (;;) {
		HANDLE waits[3] = { request_, game_, waitable_ };
		const DWORD n = waitable_ != nullptr ? 3 : 2;
		const DWORD hit = MsgWaitForMultipleObjects(n, waits, FALSE, waiting_release_ ? 100 : 1000, QS_ALLINPUT);
		if ((n == 3 && hit == WAIT_OBJECT_0 + 2) || (hit == WAIT_TIMEOUT && waiting_release_)) {
			if (hit == WAIT_TIMEOUT)
				++release_timeouts_;
			waiting_release_ = false;
			SetEvent(released_);
		} else if (hit == WAIT_OBJECT_0 + 1) {
			diag_info("window", L"the game exited; the window host is closing");
			return ExitOk;
		} else if (hit == WAIT_OBJECT_0) {
			MemoryBarrier();
			Request r;
			if (!read_request(*block_, &r)) {
				diag_error("window", L"a request this host cannot serve; closing");
				return ExitSetup;
			}
			if (r.command == Command::Quit) {
				answer(r);
				diag_info("window", L"the game asked the window host to close");
				return ExitOk;
			}
			if (r.command == Command::Present)
				present(r);
			else
				place(r);
		} else if (hit == WAIT_FAILED) {
			diag_error("window", L"the window host lost the handles it serves on");
			return ExitWaitFailed;
		}
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		note_stats();
	}
}

void WindowHost::shutdown() noexcept
{
	if (ctx_ != nullptr) {
		ctx_->ClearState();
		ctx_->Flush();
	}
	if (waitable_ != nullptr)
		CloseHandle(waitable_);
	waitable_ = nullptr;
	release(swap_);
	for (ID3D11Texture2D *&t : shared_)
		release(t);
	release(ready_);
	release(done_);
	release(ctx_);
	release(device_);
	if (window_ != nullptr)
		DestroyWindow(window_);
	window_ = nullptr;
	close(released_);
	close(response_);
	close(request_);
	close(game_);
}

}

int run_window_host(uint64_t block_handle)
{
	HANDLE mapping = as_handle(block_handle);
	auto *const block = mapping != nullptr
		? static_cast<Block *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Block))) : nullptr;
	if (block == nullptr) {
		diag_error("window", L"the game's block could not be mapped");
		if (mapping != nullptr)
			CloseHandle(mapping);
		return ExitNoBlock;
	}
	diag_logf(DiagLevel::Info, "window", L"AeonSRHost showing frame generation's window for game %u",
		static_cast<unsigned>(block->game_pid));
	int code = ExitOk;
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
		WindowHost host;
		std::wstring why;
		if (!host.start(block, &why)) {
			diag_error("window", L"the window could not be shown: " + why);
			if (block->magic != kMagic || block->version != kVersion || block->block_size != kBlockSize) {
				UnmapViewOfFile(block);
				CloseHandle(mapping);
				return ExitSetup;
			}
			const size_t n = std::min<size_t>(why.size(), kTextChars - 1);
			std::memcpy(block->message, why.c_str(), n * sizeof(wchar_t));
			block->message[n] = L'\0';
			block->host_state = static_cast<uint32_t>(HostState::Failed);
			MemoryBarrier();
			const HANDLE response = as_handle(block->response_event);
			if (response != nullptr)
				SetEvent(response);
			code = ExitSetup;
		} else {
			block->host_pid = GetCurrentProcessId();
			block->host_state = static_cast<uint32_t>(HostState::Ready);
			MemoryBarrier();
			SetEvent(as_handle(block->response_event));
			diag_info("window", L"window host ready");
			code = host.serve();
		}
	}
	UnmapViewOfFile(block);
	CloseHandle(mapping);
	return code;
}

}
