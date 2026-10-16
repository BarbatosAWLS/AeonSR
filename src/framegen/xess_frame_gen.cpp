#include "aeon_sr/framegen/vendor_frame_gen_impl.hpp"
#include "aeon_sr/framegen/xess_fake_swapchain.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/interop/blit_d3d12.hpp"

#include <xell/xell.h>
#include <xell/xell_d3d12.h>
#include <xess_fg/xefg_swapchain.h>
#include <xess_fg/xefg_swapchain_d3d12.h>
#include <xess_fg/xefg_swapchain_debug.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

namespace aeon_sr {
namespace {

struct XeApi {
	HMODULE xell = nullptr, xefg = nullptr;
	decltype(&::xellD3D12CreateContext) xell_create = nullptr;
	decltype(&::xellDestroyContext) xell_destroy = nullptr;
	decltype(&::xellSetSleepMode) xell_sleep_mode = nullptr;
	decltype(&::xellAddMarkerData) xell_marker = nullptr;
	decltype(&::xellSetLoggingCallback) xell_log = nullptr;
	decltype(&::xefgSwapChainGetVersion) version = nullptr;
	decltype(&::xefgSwapChainD3D12CreateContext) create = nullptr;
	decltype(&::xefgSwapChainSetLoggingCallback) set_log = nullptr;
	decltype(&::xefgSwapChainSetLatencyReduction) latency = nullptr;
	decltype(&::xefgSwapChainD3D12InitFromSwapChainDesc) init_desc = nullptr;
	decltype(&::xefgSwapChainD3D12GetSwapChainPtr) swapchain = nullptr;
	decltype(&::xefgSwapChainSetEnabled) enable = nullptr;
	decltype(&::xefgSwapChainD3D12TagFrameResource) tag_resource = nullptr;
	decltype(&::xefgSwapChainTagFrameConstants) tag_constants = nullptr;
	decltype(&::xefgSwapChainSetPresentId) present_id = nullptr;
	decltype(&::xefgSwapChainGetLastPresentStatus) last_status = nullptr;
	decltype(&::xefgSwapChainDestroy) destroy = nullptr;
	decltype(&::xefgSwapChainEnableDebugFeature) debug_feature = nullptr;
};

template <typename F>
void resolve(HMODULE m, const char *name, F &out, std::wstring *missing)
{
	out = reinterpret_cast<F>(GetProcAddress(m, name));
	if (out == nullptr)
		*missing += std::wstring(name, name + std::strlen(name)) + L" ";
}

bool load_api(XeApi *x, std::wstring *error)
{
	const std::wstring fg_path = find_vendor_runtime(L"libxess_fg.dll");
	const std::wstring ll_path = find_vendor_runtime(L"libxell.dll");
	if (fg_path.empty() || ll_path.empty()) {
		*error = L"Intel XeSS frame generation needs libxess_fg.dll and libxell.dll in the add-on's runtime folder";
		return false;
	}
	const DWORD flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
	x->xell = LoadLibraryExW(ll_path.c_str(), nullptr, flags);
	x->xefg = LoadLibraryExW(fg_path.c_str(), nullptr, flags);
	if (x->xell == nullptr || x->xefg == nullptr) {
		wchar_t b[160]{};
		_snwprintf_s(b, _TRUNCATE, L"Intel XeSS frame generation: its DLLs did not load (error %lu)", GetLastError());
		*error = b;
		return false;
	}
	std::wstring missing;
	resolve(x->xell, "xellD3D12CreateContext", x->xell_create, &missing);
	resolve(x->xell, "xellDestroyContext", x->xell_destroy, &missing);
	resolve(x->xell, "xellSetSleepMode", x->xell_sleep_mode, &missing);
	resolve(x->xell, "xellAddMarkerData", x->xell_marker, &missing);
	resolve(x->xell, "xellSetLoggingCallback", x->xell_log, &missing);
	resolve(x->xefg, "xefgSwapChainGetVersion", x->version, &missing);
	resolve(x->xefg, "xefgSwapChainD3D12CreateContext", x->create, &missing);
	resolve(x->xefg, "xefgSwapChainSetLoggingCallback", x->set_log, &missing);
	resolve(x->xefg, "xefgSwapChainSetLatencyReduction", x->latency, &missing);
	resolve(x->xefg, "xefgSwapChainD3D12InitFromSwapChainDesc", x->init_desc, &missing);
	resolve(x->xefg, "xefgSwapChainD3D12GetSwapChainPtr", x->swapchain, &missing);
	resolve(x->xefg, "xefgSwapChainSetEnabled", x->enable, &missing);
	resolve(x->xefg, "xefgSwapChainD3D12TagFrameResource", x->tag_resource, &missing);
	resolve(x->xefg, "xefgSwapChainTagFrameConstants", x->tag_constants, &missing);
	resolve(x->xefg, "xefgSwapChainSetPresentId", x->present_id, &missing);
	resolve(x->xefg, "xefgSwapChainGetLastPresentStatus", x->last_status, &missing);
	resolve(x->xefg, "xefgSwapChainDestroy", x->destroy, &missing);
	resolve(x->xefg, "xefgSwapChainEnableDebugFeature", x->debug_feature, &missing);
	if (!missing.empty()) {
		*error = L"Intel XeSS frame generation: its DLLs lack " + missing;
		return false;
	}
	return true;
}

void on_xefg_log(const char *message, xefg_swapchain_logging_level_t level, void *)
{
	if (level < XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING || message == nullptr)
		return;
	std::wstring w;
	for (const char *c = message; *c != '\0'; ++c)
		w += static_cast<wchar_t>(static_cast<unsigned char>(*c));
	diag_logf(level == XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR ? DiagLevel::Error : DiagLevel::Warn, "framegen",
		L"XeSS-FG: %ls", w.c_str());
}

void on_xell_log(const char *message, xell_logging_level_t level)
{
	if (level < XELL_LOGGING_LEVEL_WARNING || message == nullptr)
		return;
	std::wstring w;
	for (const char *c = message; *c != '\0'; ++c)
		w += static_cast<wchar_t>(static_cast<unsigned char>(*c));
	diag_logf(DiagLevel::Warn, "framegen", L"XeLL: %ls", w.c_str());
}

const wchar_t *xefg_result(xefg_swapchain_result_t r)
{
	switch (static_cast<int>(r)) {
	case 0: return L"SUCCESS";
	case 2: return L"WARNING_OLD_DRIVER";
	case 3: return L"WARNING_TOO_FEW_FRAMES";
	case 4: return L"WARNING_FRAMES_ID_MISMATCH";
	case 5: return L"WARNING_MISSING_PRESENT_STATUS";
	case 6: return L"WARNING_RESOURCE_SIZES_MISMATCH";
	case -1: return L"ERROR_UNSUPPORTED_DEVICE";
	case -2: return L"ERROR_UNSUPPORTED_DRIVER";
	case -3: return L"ERROR_UNINITIALIZED";
	case -4: return L"ERROR_INVALID_ARGUMENT";
	case -5: return L"ERROR_DEVICE_OUT_OF_MEMORY";
	case -6: return L"ERROR_DEVICE";
	case -7: return L"ERROR_NOT_IMPLEMENTED";
	case -8: return L"ERROR_INVALID_CONTEXT";
	case -9: return L"ERROR_OPERATION_IN_PROGRESS";
	case -10: return L"ERROR_UNSUPPORTED";
	default: return L"ERROR";
	}
}

void projection(float aspect, float *m)
{
	const float near_z = 0.1f;
	const float ys = 1.0f / std::tan(3.14159265f / 6.0f), xs = ys / aspect;
	const float p[16] = { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, 1, 1, 0, 0, -near_z, 0 };
	std::memcpy(m, p, sizeof p);
}

double qpc_now_ms()
{
	LARGE_INTEGER f{}, t{};
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

class XessFrameGenerator final : public VendorFrameGenerator {
public:
	~XessFrameGenerator() override
	{
		join();
		teardown();
		if (api_.xefg != nullptr)
			FreeLibrary(api_.xefg);
		if (api_.xell != nullptr)
			FreeLibrary(api_.xell);
	}

	FrameGenerator kind() const noexcept override { return FrameGenerator::Xess; }
	std::wstring version() const override { return state_.load() == kReady ? version_ : std::wstring(); }
	void reset() noexcept override { frames_ = 0; }
	bool busy() const noexcept override { return state_.load(std::memory_order_acquire) == kBuilding; }

	VendorStep record(const VendorFrame &f, std::wstring *note) override
	{
		int s = state_.load(std::memory_order_acquire);
		if (s == kBuilding)
			return VendorStep::Building;
		if (s == kFailed) {
			*note = error_;
			return VendorStep::Unavailable;
		}
		if (f.color_space == VendorColor::Scrgb) {
			*note = L"Intel XeSS frame generation does not take scRGB HDR frames";
			return VendorStep::Unavailable;
		}
		if (s == kReady && (f.device != device_ || f.width != w_ || f.height != h_ || f.format != format_)) {
			join();
			if (order_fence_ != nullptr) {
				f.queue->Signal(order_fence_, ++order_value_);
				HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				(void)wait_fence(order_fence_, order_value_, e, 5000);
				if (e != nullptr)
					CloseHandle(e);
			}
			teardown();
			state_.store(kIdle, std::memory_order_release);
			s = kIdle;
		}
		if (s == kIdle) {
			if (f.flush == nullptr || f.queue == nullptr) {
				*note = L"Intel XeSS frame generation cannot run on this route";
				return VendorStep::Unavailable;
			}
			start_build(f);
			return VendorStep::Building;
		}
		join();
		XefgCaptures &c = *captures_;
		if (c.broken.load()) {
			*note = L"Intel XeSS frame generation: its swap chain could not copy a frame";
			return VendorStep::Unavailable;
		}

		f.queue->Signal(c.consumed, consumed_up_to_);
		ledger_.settle(c.presents.load(std::memory_order_acquire));

		const uint32_t id = ++present_id_;
		api_.xell_marker(xell_, id, XELL_SIMULATION_START);
		api_.xell_marker(xell_, id, XELL_SIMULATION_END);
		api_.xell_marker(xell_, id, XELL_RENDERSUBMIT_START);

		ID3D12Resource *bb = nullptr;
		if (FAILED(app_swapchain_->GetBuffer(app_swapchain_->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb))) ||
			bb == nullptr) {
			*note = L"Intel XeSS frame generation: no back buffer";
			return VendorStep::Failed;
		}
		if (id > kBuffers && fg_done_value_ > 0)
			f.queue->Wait(fg_done_, std::min<uint64_t>(id - kBuffers, fg_done_value_));
		barrier12(f.cmd, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
		barrier12(f.cmd, f.color, kInputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		f.cmd->CopyResource(bb, f.color);
		barrier12(f.cmd, f.color, D3D12_RESOURCE_STATE_COPY_SOURCE, kInputState);
		barrier12(f.cmd, bb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
		bb->Release();

		const auto tag = [&](xefg_swapchain_resource_type_t type, ID3D12Resource *res, D3D12_RESOURCE_STATES st) {
			xefg_swapchain_d3d12_resource_data_t rd{};
			rd.type = type;
			rd.validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
			rd.resourceBase = { 0, 0 };
			rd.resourceSize = { f.width, f.height };
			rd.pResource = res;
			rd.incomingState = st;
			return api_.tag_resource(fg_, f.cmd, id, &rd);
		};
		xefg_swapchain_result_t r = tag(XEFG_SWAPCHAIN_RES_MOTION_VECTOR, f.motion, f.motion_state);
		if (r >= 0)
			r = tag(XEFG_SWAPCHAIN_RES_DEPTH, f.depth, kInputState);
		const bool fresh = f.reset || frames_ == 0;
		xefg_swapchain_frame_constant_data_t k{};
		for (int i = 0; i < 4; ++i)
			k.viewMatrix[i * 5] = 1.0f;
		projection(static_cast<float>(f.width) / static_cast<float>(f.height), k.projectionMatrix);
		k.motionVectorScaleX = static_cast<float>(f.width);
		k.motionVectorScaleY = static_cast<float>(f.height);
		k.resetHistory = fresh ? 1u : 0u;
		k.frameRenderTime = static_cast<float>(f.frame_ms);
		if (r >= 0)
			r = api_.tag_constants(fg_, id, &k);
		if (r < 0) {
			*note = std::wstring(L"Intel XeSS frame generation: tagging the frame: ") + xefg_result(r);
			frames_ = 0;
			return VendorStep::Failed;
		}

		if (!f.flush()) {
			*note = L"Intel XeSS frame generation: the frame's list could not be executed";
			frames_ = 0;
			return VendorStep::ListLost;
		}
		f.queue->Signal(order_fence_, ++order_value_);
		fg_queue_->Wait(order_fence_, order_value_);

		api_.present_id(fg_, id);
		api_.xell_marker(xell_, id, XELL_RENDERSUBMIT_END);
		api_.xell_marker(xell_, id, XELL_PRESENT_START);
		const double t0 = qpc_now_ms();
		const HRESULT hr = app_swapchain_->Present(0, 0);
		fg_queue_->Signal(fg_done_, id);
		fg_done_value_ = id;
		api_.xell_marker(xell_, id, XELL_PRESENT_END);
		xefg_swapchain_present_status_t st{};
		api_.last_status(fg_, &st);
		const uint64_t first = ledger_.add(st.framesPresented);
		if (FAILED(hr)) {
			*note = L"Intel XeSS frame generation: its swap chain refused the frame";
			frames_ = 0;
			return VendorStep::Failed;
		}
		++frames_;
		const bool generated = st.framesPresented > 0 && st.isFrameGenEnabled != 0 &&
			st.frameGenResult == XEFG_SWAPCHAIN_RESULT_SUCCESS && !fresh;
		if (!generated) {
			if (!fresh && refusals_ < 3) {
				++refusals_;
				diag_logf(DiagLevel::Warn, "framegen", L"XeSS-FG made no frame for %u: %u presented, %ls, %ls", id,
					st.framesPresented, xefg_result(st.frameGenResult), st.isFrameGenEnabled != 0 ? L"enabled" : L"disabled");
			}
			consumed_up_to_ = c.presents.load(std::memory_order_acquire);
			return VendorStep::Warming;
		}

		const DWORD budget = static_cast<DWORD>(std::clamp(f.frame_ms, 8.0, 50.0));
		const double deadline = t0 + budget;
		while (c.presents.load(std::memory_order_acquire) < first && !c.broken.load()) {
			const double left = deadline - qpc_now_ms();
			if (left <= 0.0)
				break;
			WaitForSingleObject(c.event, static_cast<DWORD>(std::ceil(left)));
		}
		const uint64_t seen = c.presents.load(std::memory_order_acquire);
		consumed_up_to_ = std::max(consumed_up_to_, first);
		if (seen < first) {
			++late_;
			*note = L"Intel XeSS frame generation: the generated frame did not arrive in time";
			return VendorStep::Failed;
		}
		note_wait(qpc_now_ms() - t0);

		ID3D12Resource *const slot = c.slot[first % XefgCaptures::kRing];
		f.queue->Wait(c.captured, first);
		barrier12(f.cmd, slot, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
		barrier12(f.cmd, f.output, kOutputState, D3D12_RESOURCE_STATE_COPY_DEST);
		f.cmd->CopyResource(f.output, slot);
		barrier12(f.cmd, f.output, D3D12_RESOURCE_STATE_COPY_DEST, kOutputState);
		barrier12(f.cmd, slot, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
		consumed_up_to_ = std::max(consumed_up_to_, first);
		return VendorStep::Generated;
	}

private:
	enum : int { kIdle = 0, kBuilding, kReady, kFailed };

	void join()
	{
		if (builder_.joinable())
			builder_.join();
	}

	void note_wait(double ms)
	{
		wait_sum_ += ms;
		wait_max_ = std::max(wait_max_, ms);
		if (++waits_ == 200) {
			diag_logf(DiagLevel::Info, "framegen", L"XeSS-FG: generated frame arrived %.2f ms after the frame on "
				L"average (max %.2f) over %u frames; %u late", wait_sum_ / waits_, wait_max_, waits_, late_);
			wait_sum_ = wait_max_ = 0.0;
			waits_ = late_ = 0;
		}
	}

	void start_build(const VendorFrame &f)
	{
		join();
		device_ = f.device;
		w_ = f.width;
		h_ = f.height;
		format_ = f.format;
		window_ = f.window;
		frames_ = 0;
		state_.store(kBuilding, std::memory_order_release);
		device_->AddRef();
		builder_ = std::thread([this] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			const bool ok = build();
			if (!ok)
				teardown();
			device_->Release();
			state_.store(ok ? kReady : kFailed, std::memory_order_release);
		});
	}

	bool build()
	{
		if (api_.xefg == nullptr && !load_api(&api_, &error_))
			return false;
		xefg_swapchain_version_t v{};
		if (api_.version(&v) >= 0) {
			wchar_t b[48]{};
			_snwprintf_s(b, _TRUNCATE, L"%u.%u.%u", v.major, v.minor, v.patch);
			version_ = b;
		}
		captures_ = std::make_shared<XefgCaptures>();
		D3D12_COMMAND_QUEUE_DESC qd{};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (!captures_->create(device_, w_, h_, format_) ||
			FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&fg_queue_))) ||
			FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&order_fence_))) ||
			FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fg_done_)))) {
			error_ = L"Intel XeSS frame generation: its queue and textures could not be made";
			return false;
		}
		if (api_.xell_create(device_, &xell_) != XELL_RESULT_SUCCESS || xell_ == nullptr) {
			error_ = L"Intel XeSS frame generation: XeLL did not start on this device";
			return false;
		}
		api_.xell_log(xell_, XELL_LOGGING_LEVEL_WARNING, on_xell_log);
		xell_sleep_params_t sp{};
		sp.bLowLatencyMode = 1;
		api_.xell_sleep_mode(xell_, &sp);

		xefg_swapchain_result_t r = api_.create(device_, &fg_);
		if (r < 0 || fg_ == nullptr) {
			error_ = std::wstring(L"Intel XeSS frame generation refused this graphics card: ") + xefg_result(r);
			return false;
		}
		api_.set_log(fg_, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, on_xefg_log, nullptr);
		r = api_.latency(fg_, xell_);
		if (r < 0) {
			error_ = std::wstring(L"Intel XeSS frame generation: XeLL: ") + xefg_result(r);
			return false;
		}
		IDXGIFactory7 *real = nullptr;
		if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&real))) || real == nullptr) {
			error_ = L"Intel XeSS frame generation: no DXGI factory";
			return false;
		}
		factory_ = make_xefg_capture_factory(real, captures_);
		real->Release();

		DXGI_SWAP_CHAIN_DESC1 sd{};
		sd.Width = w_;
		sd.Height = h_;
		sd.Format = format_;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = kBuffers;
		sd.Scaling = DXGI_SCALING_STRETCH;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
		DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs{};
		fs.Windowed = TRUE;
		xefg_swapchain_d3d12_init_params_t p{};
		p.maxInterpolatedFrames = 1;
		p.initFlags = XEFG_SWAPCHAIN_INIT_FLAG_NONE;
		p.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
		HWND hwnd = window_ != nullptr && IsWindow(window_) ? window_ : GetDesktopWindow();
		r = api_.init_desc(fg_, hwnd, &sd, &fs, fg_queue_, factory_, &p);
		if (r < 0) {
			error_ = std::wstring(L"Intel XeSS frame generation did not start: ") + xefg_result(r);
			return false;
		}
		if (api_.swapchain(fg_, IID_PPV_ARGS(&app_swapchain_)) < 0 || app_swapchain_ == nullptr) {
			error_ = L"Intel XeSS frame generation gave no swap chain";
			return false;
		}
		api_.debug_feature(fg_, XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION, 1, nullptr);
		api_.enable(fg_, 1);
		present_id_ = 0;
		ledger_ = XefgPresentLedger{};
		fg_done_value_ = 0;
		consumed_up_to_ = 0;
		diag_logf(DiagLevel::Info, "framegen", L"XeSS-FG %ls ready, %ux%u", version_.c_str(), w_, h_);
		return true;
	}

	void teardown() noexcept
	{
		if (fg_ != nullptr && api_.enable != nullptr)
			api_.enable(fg_, 0);
		if (fg_queue_ != nullptr && order_fence_ != nullptr) {
			fg_queue_->Signal(order_fence_, ++order_value_);
			HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			(void)wait_fence(order_fence_, order_value_, e, 5000);
			if (e != nullptr)
				CloseHandle(e);
		}
		if (app_swapchain_ != nullptr)
			app_swapchain_->Release();
		app_swapchain_ = nullptr;
		if (fg_ != nullptr && api_.destroy != nullptr)
			api_.destroy(fg_);
		fg_ = nullptr;
		if (xell_ != nullptr && api_.xell_destroy != nullptr)
			api_.xell_destroy(xell_);
		xell_ = nullptr;
		if (factory_ != nullptr)
			factory_->Release();
		factory_ = nullptr;
		if (order_fence_ != nullptr)
			order_fence_->Release();
		order_fence_ = nullptr;
		if (fg_done_ != nullptr)
			fg_done_->Release();
		fg_done_ = nullptr;
		order_value_ = 0;
		if (fg_queue_ != nullptr)
			fg_queue_->Release();
		fg_queue_ = nullptr;
		captures_.reset();
	}

	XeApi api_;
	std::atomic<int> state_{ kIdle };
	std::thread builder_;
	std::wstring error_, version_;
	ID3D12Device *device_ = nullptr;
	uint32_t w_ = 0, h_ = 0;
	DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
	HWND window_ = nullptr;

	xell_context_handle_t xell_ = nullptr;
	xefg_swapchain_handle_t fg_ = nullptr;
	IDXGIFactory2 *factory_ = nullptr;
	IDXGISwapChain4 *app_swapchain_ = nullptr;
	ID3D12CommandQueue *fg_queue_ = nullptr;
	ID3D12Fence *order_fence_ = nullptr;
	uint64_t order_value_ = 0;
	std::shared_ptr<XefgCaptures> captures_;

	static constexpr uint32_t kBuffers = 3;
	ID3D12Fence *fg_done_ = nullptr;
	uint64_t fg_done_value_ = 0;
	uint32_t present_id_ = 0;
	XefgPresentLedger ledger_;
	uint64_t consumed_up_to_ = 0;
	uint64_t frames_ = 0;
	double wait_sum_ = 0.0, wait_max_ = 0.0;
	uint32_t waits_ = 0, late_ = 0;
	uint32_t refusals_ = 0;
};

}

std::unique_ptr<VendorFrameGenerator> make_xess_frame_generator()
{
	return std::make_unique<XessFrameGenerator>();
}

}
