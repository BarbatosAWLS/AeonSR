#include "aeon_sr/upscalers/backend_fsr.hpp"
#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/frame_inputs.hpp"
#include "aeon_sr/upscalers/fsr_warmup.hpp"
#include "aeon_sr/ngx/neural_params.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"
#include "aeon_sr/core/runtime_search.hpp"
#include "aeon_sr/core/settings.hpp"

#include <excpt.h>

#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>

namespace aeon_sr {

static_assert(kFsrColorSpaceSdr == static_cast<uint32_t>(NeuralColorSpace::Sdr),
	"fsr_color_flags() reads UpscalerParams::color_space as a NeuralColorSpace");

namespace {

std::wstring fsr_dll_path(const std::wstring &dir)
{
	return join_path(dir, L"amd_fidelityfx_upscaler_dx12.dll");
}

const wchar_t *ffx_result_name(ffxReturnCode_t code)
{
	switch (code) {
	case FFX_API_RETURN_OK: return L"OK";
	case FFX_API_RETURN_ERROR: return L"ERROR";
	case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return L"UNKNOWN_DESCTYPE";
	case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return L"RUNTIME_ERROR";
	case FFX_API_RETURN_NO_PROVIDER: return L"NO_PROVIDER";
	case FFX_API_RETURN_ERROR_MEMORY: return L"MEMORY";
	case FFX_API_RETURN_ERROR_PARAMETER: return L"PARAMETER";
	case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return L"PROVIDER_NO_SUPPORT";
	default: return L"Unknown";
	}
}

std::wstring ffx_format_result(const wchar_t *what, ffxReturnCode_t code)
{
	wchar_t buf[256]{};
	_snwprintf_s(buf, _TRUNCATE, L"%s: %s (0x%08X)", what, ffx_result_name(code), static_cast<unsigned>(code));
	return buf;
}

uint32_t ffx_quality_for(uint32_t upscale_mode)
{
	switch (static_cast<UpscaleMode>(upscale_mode)) {
	case UpscaleMode::UltraQuality:
	case UpscaleMode::Quality:
		return FFX_UPSCALE_QUALITY_MODE_QUALITY;
	case UpscaleMode::Balanced:
		return FFX_UPSCALE_QUALITY_MODE_BALANCED;
	case UpscaleMode::Performance:
		return FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
	case UpscaleMode::UltraPerformance:
		return FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
	case UpscaleMode::Dlaa:
	default:
		return FFX_UPSCALE_QUALITY_MODE_NATIVEAA;
	}
}

ffxReturnCode_t create_context_guarded(PfnFfxCreateContext fn, ffxContext *ctx,
	ffxCreateContextDescHeader *desc, unsigned long *seh_code)
{
	__try {
		return fn(ctx, desc, nullptr);
	} __except ((*seh_code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return FFX_API_RETURN_ERROR;
	}
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

std::wstring widen_utf8(const char *text)
{
	if (text == nullptr || *text == '\0')
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
	if (n <= 1)
		return {};
	std::wstring wide(static_cast<size_t>(n - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), n);
	return wide;
}

}

const char *Fsr4D3D12Backend::name() const { return "FSR 4"; }

reshade::api::device_api Fsr4D3D12Backend::api() const
{
	return reshade::api::device_api::d3d12;
}

void Fsr4D3D12Backend::scan_game_fsr_modules()
{
	std::wstring found;
	for (const wchar_t *m : { L"amd_fidelityfx_upscaler_dx12.dll", L"amd_fidelityfx_loader_dx12.dll",
			L"amd_fidelityfx_dx12.dll", L"OptiScaler.dll", L"libxess_dx11.dll" }) {
		if (GetModuleHandleW(m) != nullptr) {
			if (!found.empty())
				found += L", ";
			found += m;
		}
	}
	game_fsr_modules = found;
}

bool Fsr4D3D12Backend::ensure_dll_present()
{
	if (fsr_loading())
		return true;
	if (api_.module != nullptr) {
		dll_present_ = true;
		return true;
	}

	dll_present_ = false;
	dll_dir.clear();
	for (const std::wstring &dir : fsr_search_dirs(addon_dir, exe_directory_w())) {
		if (!file_exists_w(fsr_dll_path(dir)))
			continue;
		dll_present_ = true;
		dll_dir = dir;
		break;
	}
	if (!dll_present_) {
		status = UpscalerStatus::MissingRuntime;
		last_error = L"Place amd_fidelityfx_upscaler_dx12.dll next to AeonSR.addon64, "
			L"or in a runtime\\ folder beside it";
	}
	return dll_present_;
}

void Fsr4D3D12Backend::set_color_space(uint32_t color_space)
{
	color_flags_ = fsr_color_flags(color_space);
}

bool Fsr4D3D12Backend::runtime_present()
{
	return ensure_dll_present();
}

void Fsr4D3D12Backend::wait_gpu()
{
	gpu_fence_.signal_and_wait(device_, queue_);
}

void Fsr4D3D12Backend::release_scratch()
{
	for (ID3D12Resource **r : { &output_tex_, &color_copy_, &depth_scratch_, &color_full_ }) {
		if (*r) {
			(*r)->Release();
			*r = nullptr;
		}
	}
	created_backbuffer_format_ = DXGI_FORMAT_UNKNOWN;
}

void Fsr4D3D12Backend::destroy_context()
{
	if (output_tex_ != nullptr || color_copy_ != nullptr || depth_scratch_ != nullptr ||
		color_full_ != nullptr)
		wait_gpu();
	if (context_ != nullptr && api_.DestroyContext != nullptr) {
		api_.DestroyContext(&context_, nullptr);
		context_ = nullptr;
	}
	delete pso_cache_;
	pso_cache_ = nullptr;
	reset_pending_ = false;
	release_scratch();
	initialized_ = false;
	created_quality_mode_ = 0xFFFFFFFFu;
	created_render_scale_ = -1.0f;
	created_color_flags_ = 0xFFFFFFFFu;
}

void Fsr4D3D12Backend::on_destroy_swapchain()
{
	if (init_state_.load(std::memory_order_acquire) == kRunning)
		return;
	join_init();
	destroy_context();
}

void Fsr4D3D12Backend::shutdown()
{
	if (init_thread_.joinable()) {
		if (cancel_event_ != nullptr)
			SetEvent(cancel_event_);
		SetThreadPriority(init_thread_.native_handle(), THREAD_PRIORITY_NORMAL);
	}
	join_init();
	destroy_context();
	blit_.release();
	gpu_fence_.release();
	ffx_unload(api_);
	queue_ = nullptr;
	device_ = nullptr;
	width_ = height_ = render_width_ = render_height_ = 0;
	if (status != UpscalerStatus::MissingRuntime)
		status = UpscalerStatus::Idle;
}

bool Fsr4D3D12Backend::query_render_size(uint32_t display_w, uint32_t display_h, uint32_t quality,
	uint32_t &out_w, uint32_t &out_h)
{
	if (quality == FFX_UPSCALE_QUALITY_MODE_NATIVEAA) {
		out_w = display_w;
		out_h = display_h;
		return true;
	}
	if (api_.Query == nullptr)
		return false;

	uint32_t rw = 0, rh = 0;
	ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query{};
	query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
	query.displayWidth = display_w;
	query.displayHeight = display_h;
	query.qualityMode = quality;
	query.pOutRenderWidth = &rw;
	query.pOutRenderHeight = &rh;

	const ffxReturnCode_t qr = api_.Query(context_ != nullptr ? &context_ : nullptr, &query.header);
	if (qr != FFX_API_RETURN_OK || rw == 0 || rh == 0)
		return false;
	out_w = rw;
	out_h = rh;
	return true;
}

void Fsr4D3D12Backend::enumerate_providers(ID3D12Device *device)
{
	if (!providers.empty() || api_.Query == nullptr || device == nullptr)
		return;
	uint64_t count = 0;
	ffxQueryDescGetVersions q{};
	q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
	q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	q.device = device;
	q.outputCount = &count;
	if (api_.Query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0 || count > 32)
		return;
	std::vector<uint64_t> ids(static_cast<size_t>(count));
	std::vector<const char *> names(static_cast<size_t>(count));
	q.versionIds = ids.data();
	q.versionNames = names.data();
	if (api_.Query(nullptr, &q.header) != FFX_API_RETURN_OK)
		return;
	for (uint64_t i = 0; i < count; ++i) {
		FfxProvider p;
		p.id = ids[static_cast<size_t>(i)];
		p.name = names[static_cast<size_t>(i)] != nullptr ? names[static_cast<size_t>(i)] : "";
		if (!p.name.empty())
			providers.push_back(p);
	}
}

void Fsr4D3D12Backend::join_init()
{
	if (init_thread_.joinable())
		init_thread_.join();
	if (init_state_.load(std::memory_order_acquire) != kIdle) {
		status = worker_status_;
		last_error = worker_error_;
		if (worker_status_ == UpscalerStatus::Crashed)
			crashed = true;
		init_state_.store(kIdle, std::memory_order_release);
	}
}

bool Fsr4D3D12Backend::init_context(ID3D12Device *device, uint32_t w, uint32_t h,
	DXGI_FORMAT color_format, bool with_depth)
{
	const int state = init_state_.load(std::memory_order_acquire);
	if (state == kRunning) {
		status = UpscalerStatus::Loading;
		return false;
	}
	if (state == kFinished) {
		join_init();
		if (!init_ok_)
			return false;
	}

	if (!ensure_dll_present())
		return false;
	if (device == nullptr) {
		status = UpscalerStatus::InitFailed;
		last_error = L"null D3D12 device";
		return false;
	}

	if (initialized_ && device_ == device && width_ == w && height_ == h &&
		created_quality_mode_ == quality_mode_ &&
		created_render_scale_ == render_scale_ &&
		created_color_flags_ == color_flags_ &&
		created_provider == want_provider)
		return true;

	destroy_context();
	device_ = device;
	width_ = w;
	height_ = h;

	status = UpscalerStatus::Loading;
	worker_status_ = UpscalerStatus::Loading;
	worker_error_.clear();
	init_ok_ = false;
	build_quality_ = quality_mode_;
	build_scale_ = render_scale_;
	build_provider_ = want_provider;
	build_color_flags_ = color_flags_;
	build_dll_path_ = fsr_dll_path(!dll_dir.empty() ? dll_dir : addon_dir);
	build_addon_dir_ = addon_dir;
	if (cancel_event_ == nullptr)
		cancel_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	else
		ResetEvent(cancel_event_);
	init_state_.store(kRunning, std::memory_order_release);
	init_thread_ = std::thread([this, device, w, h, color_format, with_depth] {
		device_busy_.store(true, std::memory_order_release);
		init_ok_ = build_context(device, w, h, color_format, with_depth);
		device_busy_.store(false, std::memory_order_release);
		init_state_.store(kFinished, std::memory_order_release);
	});
	return false;
}

bool Fsr4D3D12Backend::build_context(ID3D12Device *device, uint32_t w, uint32_t h,
	DXGI_FORMAT color_format, bool with_depth)
{
	LARGE_INTEGER t_freq{}, t_begin{}, t_dll{}, t_ctx{}, t_warm{};
	QueryPerformanceFrequency(&t_freq);
	QueryPerformanceCounter(&t_begin);
	const auto ms_since = [&t_freq](const LARGE_INTEGER &from, const LARGE_INTEGER &to) {
		return t_freq.QuadPart == 0 ? 0.0
			: 1000.0 * static_cast<double>(to.QuadPart - from.QuadPart) /
				static_cast<double>(t_freq.QuadPart);
	};

	if (api_.module == nullptr) {
		const std::wstring &path = build_dll_path_;
		if (!ffx_load(api_, path, worker_error_)) {
			worker_status_ = UpscalerStatus::InitFailed;
			if (worker_error_.find(L"LoadLibraryW") != std::wstring::npos)
				worker_error_ += L" | If using official SDK split DLLs, also place amd_fidelityfx_loader_dx12.dll beside the upscaler";
			return false;
		}
	}

	QueryPerformanceCounter(&t_dll);

	scan_game_fsr_modules();
	enumerate_providers(device);

	if (build_scale_ > 0.0f) {
		scaled_render_size(w, h, build_scale_, &render_width_, &render_height_);
	} else {
		const uint32_t ffx_quality = ffx_quality_for(build_quality_);
		if (!query_render_size(w, h, ffx_quality, render_width_, render_height_)) {
			worker_status_ = UpscalerStatus::InitFailed;
			worker_error_ = L"FFX GetRenderResolutionFromQualityMode failed";
			return false;
		}
	}

	ffxCreateBackendDX12Desc backend{};
	backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backend.device = device;

	const uint32_t create_flags = kFsrCreateFlags;

	ffxOverrideVersion override_version{};
	bool pinned = false;
	if (!build_provider_.empty()) {
		for (const FfxProvider &p : providers) {
			if (p.name == build_provider_) {
				override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
				override_version.header.pNext = nullptr;
				override_version.versionId = p.id;
				pinned = true;
				break;
			}
		}
	}
	if (pinned)
		backend.header.pNext = &override_version.header;

	ffxCreateContextDescUpscale upscale{};
	upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	upscale.header.pNext = &backend.header;
	upscale.flags = create_flags;
	upscale.maxRenderSize.width = render_width_;
	upscale.maxRenderSize.height = render_height_;
	upscale.maxUpscaleSize.width = w;
	upscale.maxUpscaleSize.height = h;
	upscale.fpMessage = nullptr;

	unsigned long seh_code = 0;
	const ffxReturnCode_t cr = create_context_guarded(api_.CreateContext, &context_, &upscale.header, &seh_code);
	if (seh_code != 0) {
		worker_status_ = UpscalerStatus::Crashed;
		wchar_t buf[128]{};
		_snwprintf_s(buf, _TRUNCATE, L"FFX CreateContext CRASHED (exception 0x%08lX) - FSR disabled", seh_code);
		worker_error_ = buf;
		return false;
	}
	if (cr != FFX_API_RETURN_OK || context_ == nullptr) {
		worker_status_ = UpscalerStatus::InitFailed;
		worker_error_ = ffx_format_result(L"FFX CreateContext failed", cr);
		if (cr == FFX_API_RETURN_NO_PROVIDER)
			worker_error_ += L" | Missing amd_fidelityfx_loader_dx12.dll or incompatible upscaler build";
		return false;
	}

	QueryPerformanceCounter(&t_ctx);

	const std::wstring cache_dir = !cache_dir_override.empty() ? cache_dir_override : local_appdata_dir_w();
	std::wstring cache_file;
	if (!cache_dir.empty()) {
		CreateDirectoryW(cache_dir.c_str(), nullptr);
		cache_file = fsr_pipeline_cache_path(cache_dir, device);
	}
	FsrWarmupDesc warm{};
	warm.render_width = render_width_;
	warm.render_height = render_height_;
	warm.upscale_width = w;
	warm.upscale_height = h;
	warm.color_format = color_format;
	warm.with_depth = with_depth;
	warm.dispatch_flags = build_color_flags_;

	enum class Warm { Ready, Missed, Dirty, Crashed, Skipped };
	FsrWarmupTiming warm_time{};
	std::wstring warm_error;
	const auto warm_up = [&]() -> Warm {
		pso_cache_ = new FsrPipelineCache();
		if (!cache_file.empty())
			pso_cache_->open(device, cache_file);
		pso_cache_->load_only = true;
		unsigned long seh = 0;
		bool dirty = false;
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		const bool ok = fsr_warmup_dispatch(api_, &context_, device, warm, pso_cache_, &warm_time,
			&seh, &dirty, warm_error);
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
		if (seh != 0)
			return Warm::Crashed;
		if (dirty)
			return Warm::Dirty;
		if (pso_cache_->st.missed != 0)
			return Warm::Missed;
		return ok ? Warm::Ready : Warm::Skipped;
	};
	const auto rebuild = [&](bool destroy_old) -> bool {
		if (destroy_old) {
			if (context_ != nullptr)
				api_.DestroyContext(&context_, nullptr);
			delete pso_cache_;
		}
		context_ = nullptr;
		pso_cache_ = nullptr;
		unsigned long seh = 0;
		const ffxReturnCode_t r = create_context_guarded(api_.CreateContext, &context_, &upscale.header, &seh);
		if (seh != 0 || r != FFX_API_RETURN_OK || context_ == nullptr) {
			context_ = nullptr;
			worker_status_ = seh != 0 ? UpscalerStatus::Crashed : UpscalerStatus::InitFailed;
			worker_error_ = ffx_format_result(L"FFX CreateContext failed after the warm-up", r);
			return false;
		}
		return true;
	};

	Warm result = warm_up();
	uint32_t missing = 0;
	int prebuild_code = kPrebuildNotRun;
	double prebuild_ms = 0.0;
	if (result == Warm::Missed) {
		missing = pso_cache_->st.missed;
		diag_logf(DiagLevel::Info, "upscaler",
			L"FSR: %u of its pipelines are not in the shared cache yet; AeonSRPrebuild.exe is "
			L"building them outside the game (up to a minute, once per driver and resolution)",
			missing);
		if (!rebuild(true))
			return false;
		device_busy_.store(false, std::memory_order_release);
		prebuild_code = prebuild_given_up_ ? kPrebuildGivenUp : run_prebuild(device, warm, cache_file, &prebuild_ms);
		device_busy_.store(true, std::memory_order_release);
		result = prebuild_code == 0 ? warm_up() : Warm::Skipped;
		if (prebuild_code != 0)
			prebuild_given_up_ = true;
		if (result == Warm::Missed) {
			prebuild_given_up_ = true;
			if (!rebuild(true))
				return false;
			result = Warm::Skipped;
		}
	}
	if (result == Warm::Crashed) {
		context_ = nullptr;
		pso_cache_ = nullptr;
		worker_status_ = UpscalerStatus::Crashed;
		worker_error_ = warm_error + L" - FSR disabled";
		return false;
	}
	if (result == Warm::Dirty) {
		if (!rebuild(false))
			return false;
		result = Warm::Skipped;
	}
	QueryPerformanceCounter(&t_warm);
	const FsrPipelineCacheStats cs = pso_cache_ != nullptr ? pso_cache_->st : FsrPipelineCacheStats{};
	if (missing != 0) {
		diag_logf(prebuild_code == 0 ? DiagLevel::Info : DiagLevel::Warn, "upscaler",
			L"FSR: AeonSRPrebuild.exe %s after %.0f ms%s",
			prebuild_code == 0 ? L"built the missing pipelines" : prebuild_failure(prebuild_code),
			prebuild_ms, prebuild_code == 0 ? L"" : L"; the first FSR frame will build them, "
				L"and the game stands still while it does");
	}
	if (result == Warm::Skipped && missing == 0 && !warm_error.empty()) {
		diag_logf(DiagLevel::Warn, "upscaler", L"%s; the first FSR frame will build its pipelines",
			warm_error.c_str());
	}
	diag_logf(DiagLevel::Info, "upscaler",
		L"FSR came up in %.0f ms on a thread of its own, frames kept going: %.0f ms loading "
		L"the runtime DLL, %.0f ms in CreateContext, %.0f ms on its pipelines "
		L"(%u from the shared cache, %u compiled in the game%s) at %ux%u -> %ux%u",
		ms_since(t_begin, t_warm), ms_since(t_begin, t_dll), ms_since(t_dll, t_ctx),
		ms_since(t_ctx, t_warm), cs.loaded, cs.compiled,
		cs.file_stale ? L", the cache was from another driver and was rebuilt" : L"",
		render_width_, render_height_, w, h);
	reset_pending_ = true;

	initialized_ = true;
	created_quality_mode_ = build_quality_;
	created_render_scale_ = build_scale_;
	created_provider = build_provider_;
	created_color_flags_ = build_color_flags_;
	worker_status_ = UpscalerStatus::Ready;
	worker_error_.clear();

	provider_version.clear();
	if (api_.Query != nullptr) {
		ffxQueryGetProviderVersion pv{};
		pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
		if (api_.Query(&context_, &pv.header) == FFX_API_RETURN_OK)
			provider_version = widen_utf8(pv.versionName);
	}
	return true;
}

const wchar_t *Fsr4D3D12Backend::prebuild_failure(int code)
{
	switch (code) {
	case kPrebuildNoCache: return L"was not run: there is no folder for the shared cache";
	case kPrebuildMissing: return L"was not run: it is not beside AeonSR.addon64 (copy it from dist)";
	case kPrebuildNoStart: return L"would not start (antivirus software blocking it is the usual reason)";
	case kPrebuildStopped: return L"was left running on its own: the game stopped waiting for it";
	case kPrebuildGivenUp: return L"was not run again: it failed earlier this session";
	case 2: return L"refused its arguments";
	case 3: return L"could not load the system Direct3D 12";
	case 4: return L"found no adapter with the game's LUID";
	case 5: return L"could not create a Direct3D 12 device";
	case 6: return L"could not load the FSR runtime";
	case 7: return L"could not create an FSR context";
	case 8: return L"failed its warm-up";
	case 9: return L"crashed inside the FSR runtime";
	case 10: return L"could not store every pipeline";
	default: return L"failed";
	}
}

int Fsr4D3D12Backend::run_prebuild(ID3D12Device *device, const FsrWarmupDesc &d,
	const std::wstring &cache_file, double *ms)
{
	*ms = 0.0;
	if (cache_file.empty())
		return kPrebuildNoCache;
	const std::wstring exe = join_path(build_addon_dir_, L"AeonSRPrebuild.exe");
	if (!file_exists_w(exe))
		return kPrebuildMissing;

	const LUID luid = device->GetAdapterLuid();
	std::wstring provider;
	for (const char c : build_provider_)
		provider += static_cast<wchar_t>(static_cast<unsigned char>(c));
	wchar_t numbers[192]{};
	_snwprintf_s(numbers, _TRUNCATE, L" --luid %08lx:%08lx --render %u %u --upscale %u %u --format %u --color-flags %u",
		static_cast<unsigned long>(luid.HighPart), luid.LowPart, d.render_width, d.render_height,
		d.upscale_width, d.upscale_height, static_cast<unsigned>(d.color_format), d.dispatch_flags);
	std::wstring command = L"\"" + exe + L"\" --fsr \"" + build_dll_path_ +
		L"\"" + numbers + (d.with_depth ? L"" : L" --no-depth") +
		(provider.empty() ? std::wstring() : L" --provider \"" + provider + L"\"") +
		L" --cache \"" + cache_file + L"\"";
	std::vector<wchar_t> mutable_command(command.begin(), command.end());
	mutable_command.push_back(L'\0');
	const std::wstring cwd = cache_file.substr(0, cache_file.find_last_of(L"\\/"));

	LARGE_INTEGER freq{}, t0{}, t1{};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};
	if (!CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, cwd.c_str(), &si, &pi))
		return kPrebuildNoStart;
	CloseHandle(pi.hThread);

	const HANDLE waits[2] = { pi.hProcess, cancel_event_ };
	const DWORD r = WaitForMultipleObjects(cancel_event_ != nullptr ? 2u : 1u, waits, FALSE, kPrebuildTimeoutMs);
	int code = kPrebuildStopped;
	if (r == WAIT_OBJECT_0) {
		DWORD exit_code = 0;
		if (GetExitCodeProcess(pi.hProcess, &exit_code))
			code = static_cast<int>(exit_code);
	}
	CloseHandle(pi.hProcess);
	QueryPerformanceCounter(&t1);
	*ms = freq.QuadPart == 0 ? 0.0
		: 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart);
	return code;
}

bool Fsr4D3D12Backend::ensure_scratch(ID3D12GraphicsCommandList * , uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	if (!initialized_ || device_ == nullptr)
		return false;

	uint32_t rw = 0, rh = 0;
	if (render_scale_ > 0.0f)
		scaled_render_size(w, h, render_scale_, &rw, &rh);
	else if (!query_render_size(w, h, ffx_quality_for(quality_mode_), rw, rh))
		return false;

	if (output_tex_ != nullptr && width_ == w && height_ == h && render_width_ == rw &&
		render_height_ == rh && scratch_format_ == fmt &&
		created_backbuffer_format_ == backbuffer_format_ && created_quality_mode_ == quality_mode_ &&
		created_render_scale_ == render_scale_)
		return true;

	wait_gpu();
	release_scratch();
	width_ = w;
	height_ = h;
	render_width_ = rw;
	render_height_ = rh;
	scratch_format_ = fmt;

	if (!create_tex12(device_, width_, height_, fmt, true, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &output_tex_) ||
		!create_tex12(device_, render_width_, render_height_, fmt, true, kFfxSrvState, &color_copy_)) {
		release_scratch();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create FSR D3D12 scratch textures";
		return false;
	}
	if (!create_tex12(device_, render_width_, render_height_, DXGI_FORMAT_R32_FLOAT, false, kFfxSrvState, &depth_scratch_)) {
		release_scratch();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create FSR D3D12 depth scratch";
		return false;
	}
	if (!create_tex12(device_, width_, height_, backbuffer_format_, false, D3D12_RESOURCE_STATE_COPY_DEST, &color_full_)) {
		release_scratch();
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create FSR D3D12 color staging";
		return false;
	}
	created_backbuffer_format_ = backbuffer_format_;
	created_quality_mode_ = quality_mode_;
	created_render_scale_ = render_scale_;
	return true;
}

bool Fsr4D3D12Backend::run(
	reshade::api::effect_runtime *runtime,
	const FrameInputs &inputs,
	const UpscalerParams &params)
{
	(void)runtime;
	if (crashed || !inputs.engine.ready() || inputs.engine.device == nullptr)
		return false;

	ID3D12Resource *const depth = inputs.depth_provider == DepthProvider::Normalized
		? inputs.engine.depth : nullptr;

	return run_native(inputs.engine.device, inputs.engine.queue, inputs.engine.cmd,
		inputs.engine.color, inputs.engine.motion, depth,
		inputs.engine.color_state, params);
}

bool Fsr4D3D12Backend::run_native(
	ID3D12Device *d3d12, ID3D12CommandQueue *queue,
	ID3D12GraphicsCommandList *cmd12,
	ID3D12Resource *backbuffer, ID3D12Resource *motion_vectors,
	ID3D12Resource *depth,
	D3D12_RESOURCE_STATES backbuffer_state,
	const UpscalerParams &params)
{
	if (crashed)
		return false;
	if (d3d12 == nullptr || cmd12 == nullptr)
		return false;

	quality_mode_ = params.quality_mode;
	render_scale_ = params.render_scale;
	sharpness_ = params.sharpness;
	frame_time_ms_ = params.frame_time_ms;
	set_color_space(params.color_space);

	if (!ensure_dll_present())
		return false;

	if (queue != nullptr)
		queue_ = queue;

	if (backbuffer == nullptr || motion_vectors == nullptr) {
		status = UpscalerStatus::NeedMotionVectors;
		return false;
	}
	const D3D12_RESOURCE_DESC bb_desc = backbuffer->GetDesc();
	backbuffer_format_ = bb_desc.Format;
	const DXGI_FORMAT fmt = upscaler_scratch_format(bb_desc.Format);
	const uint32_t w = static_cast<uint32_t>(bb_desc.Width);
	const uint32_t h = bb_desc.Height;

	if (!init_context(d3d12, w, h, fmt, depth != nullptr)) {
		if (!crashed)
			status = status_after_unready_init(status);
		return false;
	}
	if (!blit_.ensure(d3d12)) {
		status = UpscalerStatus::InitFailed;
		last_error = L"failed to create D3D12 blit pipeline";
		return false;
	}
	if (!ensure_scratch(cmd12, w, h, fmt)) {
		status = UpscalerStatus::InitFailed;
		return false;
	}

	bool ok = true;

	barrier12(cmd12, backbuffer, backbuffer_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	cmd12->CopyResource(color_full_, backbuffer);

	barrier12(cmd12, color_full_, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	const float shift_x = params.jitter_in_frame ? 0.0f : params.jitter_x;
	const float shift_y = params.jitter_in_frame ? 0.0f : params.jitter_y;
	const float jitter_u = render_width_ > 0 ? -shift_x / static_cast<float>(render_width_) : 0.0f;
	const float jitter_v = render_height_ > 0 ? -shift_y / static_cast<float>(render_height_) : 0.0f;

	barrier12(cmd12, color_copy_, kFfxSrvState, D3D12_RESOURCE_STATE_RENDER_TARGET);
	ok = blit_.draw_fullscreen(d3d12, cmd12, color_full_,
		color_copy_, scratch_format_, render_width_, render_height_,
		0.0f, 0u, jitter_u, jitter_v);
	barrier12(cmd12, color_copy_, D3D12_RESOURCE_STATE_RENDER_TARGET, kFfxSrvState);
	barrier12(cmd12, color_full_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
	if (!ok) {
		status = UpscalerStatus::EvaluateFailed;
		last_error = L"failed to blit color into FSR scratch";
	}

	ID3D12Resource *depth_input = nullptr;
	if (ok && depth_scratch_ != nullptr && depth != nullptr) {
		barrier12(cmd12, depth_scratch_, kFfxSrvState, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (blit_.draw_fullscreen(d3d12, cmd12, depth,
				depth_scratch_, DXGI_FORMAT_R32_FLOAT, render_width_, render_height_,
				0.0f, 0u, jitter_u, jitter_v))
			depth_input = depth_scratch_;
		barrier12(cmd12, depth_scratch_, D3D12_RESOURCE_STATE_RENDER_TARGET, kFfxSrvState);
	}

	if (ok) {
		barrier12(cmd12, output_tex_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

		ffxDispatchDescUpscale dispatch{};
		dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
		dispatch.commandList = cmd12;
		dispatch.color = ffxApiGetResourceDX12(color_copy_, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatch.depth = depth_input != nullptr
			? ffxApiGetResourceDX12(depth_input, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ)
			: FfxApiResource{};
		dispatch.motionVectors = ffxApiGetResourceDX12(motion_vectors, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatch.exposure = FfxApiResource{};
		dispatch.reactive = FfxApiResource{};
		dispatch.transparencyAndComposition = FfxApiResource{};
		dispatch.output = ffxApiGetResourceDX12(output_tex_, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
		dispatch.jitterOffset.x = params.jitter_x;
		dispatch.jitterOffset.y = params.jitter_y;
		dispatch.motionVectorScale.x = static_cast<float>(width_);
		dispatch.motionVectorScale.y = static_cast<float>(height_);
		dispatch.renderSize.width = render_width_;
		dispatch.renderSize.height = render_height_;
		dispatch.upscaleSize.width = width_;
		dispatch.upscaleSize.height = height_;
		dispatch.enableSharpening = false;
		dispatch.sharpness = 0.0f;
		dispatch.frameTimeDelta = frame_time_ms_;
		dispatch.preExposure = 1.0f;
		dispatch.reset = params.reset || reset_pending_;
		dispatch.cameraNear = 0.0f;
		dispatch.cameraFar = 0.0f;
		dispatch.cameraFovAngleVertical = 0.0f;
		dispatch.viewSpaceToMetersFactor = 1.0f;
		dispatch.flags = created_color_flags_;

		unsigned long seh_code = 0;
		const ffxReturnCode_t dr = dispatch_guarded(api_.Dispatch, &context_, &dispatch.header, &seh_code);
		barrier12(cmd12, output_tex_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

		if (seh_code != 0) {
			crashed = true;
			status = UpscalerStatus::Crashed;
			wchar_t buf[128]{};
			_snwprintf_s(buf, _TRUNCATE, L"FFX Dispatch CRASHED (exception 0x%08lX) - FSR disabled", seh_code);
			last_error = buf;
			ok = false;
		} else if (dr != FFX_API_RETURN_OK) {
			status = UpscalerStatus::EvaluateFailed;
			last_error = ffx_format_result(L"FFX Dispatch failed", dr);
			ok = false;
		} else {
			reset_pending_ = false;
		}
	}

	barrier12(cmd12, backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
	if (ok)
		ok = blit_.draw_fullscreen(d3d12, cmd12, output_tex_,
			backbuffer, backbuffer_format_, width_, height_, params.sharpness);

	if (backbuffer_state != D3D12_RESOURCE_STATE_RENDER_TARGET)
		barrier12(cmd12, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, backbuffer_state);

	if (ok) {
		status = UpscalerStatus::Ready;
		last_error.clear();
	}
	return ok && !crashed;
}

}
