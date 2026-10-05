#include "aeon_sr/upscalers/ffx_loader.hpp"
#include "aeon_sr/upscalers/fsr_warmup.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <dx12/ffx_api_dx12.h>
#include <ffx_upscale.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

using namespace aeon_sr;

enum Exit : int {
	ExitOk = 0,
	ExitArgs = 2,
	ExitSystemDlls = 3,
	ExitAdapter = 4,
	ExitDevice = 5,
	ExitRuntime = 6,
	ExitContext = 7,
	ExitWarmup = 8,
	ExitCrashed = 9,
	ExitIncomplete = 10,
};

struct Args {
	std::wstring dll, cache, provider;
	LUID luid{};
	bool have_luid = false;
	FsrWarmupDesc desc{};
};

bool parse(Args &a)
{
	int argc = 0;
	LPWSTR *const argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argv == nullptr)
		return false;
	for (int i = 1; i < argc; ++i) {
		const std::wstring s = argv[i];
		if (s == L"--fsr" && i + 1 < argc) {
			a.dll = argv[++i];
		} else if (s == L"--cache" && i + 1 < argc) {
			a.cache = argv[++i];
		} else if (s == L"--provider" && i + 1 < argc) {
			a.provider = argv[++i];
		} else if (s == L"--luid" && i + 1 < argc) {
			unsigned long high = 0, low = 0;
			if (swscanf_s(argv[++i], L"%lx:%lx", &high, &low) == 2) {
				a.luid.HighPart = static_cast<LONG>(high);
				a.luid.LowPart = low;
				a.have_luid = true;
			}
		} else if (s == L"--render" && i + 2 < argc) {
			a.desc.render_width = static_cast<uint32_t>(wcstoul(argv[++i], nullptr, 10));
			a.desc.render_height = static_cast<uint32_t>(wcstoul(argv[++i], nullptr, 10));
		} else if (s == L"--upscale" && i + 2 < argc) {
			a.desc.upscale_width = static_cast<uint32_t>(wcstoul(argv[++i], nullptr, 10));
			a.desc.upscale_height = static_cast<uint32_t>(wcstoul(argv[++i], nullptr, 10));
		} else if (s == L"--format" && i + 1 < argc) {
			a.desc.color_format = static_cast<DXGI_FORMAT>(wcstoul(argv[++i], nullptr, 10));
		} else if (s == L"--no-depth") {
			a.desc.with_depth = false;
		} else if (s == L"--color-flags" && i + 1 < argc) {
			a.desc.dispatch_flags = static_cast<uint32_t>(wcstoul(argv[++i], nullptr, 10));
		}
	}
	LocalFree(argv);
	return !a.dll.empty() && !a.cache.empty() && a.have_luid &&
		a.desc.render_width != 0 && a.desc.render_height != 0 &&
		a.desc.upscale_width != 0 && a.desc.upscale_height != 0;
}

void note(const Args &a, const wchar_t *fmt, ...)
{
	if (a.cache.empty())
		return;
	std::wstring path = a.cache;
	const size_t slash = path.find_last_of(L"\\/");
	path = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1)) + L"prebuild.log";
	FILE *f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") != 0 || f == nullptr)
		return;
	SYSTEMTIME t{};
	GetLocalTime(&t);
	fwprintf(f, L"%04u-%02u-%02u %02u:%02u:%02u  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
	va_list args;
	va_start(args, fmt);
	vfwprintf(f, fmt, args);
	va_end(args);
	fwprintf(f, L"\n");
	fclose(f);
}

bool load_system(const wchar_t *name)
{
	return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
}

ffxReturnCode_t create_guarded(PfnFfxCreateContext fn, ffxContext *ctx, ffxCreateContextDescHeader *desc,
	unsigned long *seh)
{
	__try {
		return fn(ctx, desc, nullptr);
	} __except ((*seh = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return FFX_API_RETURN_ERROR;
	}
}

uint64_t provider_id(const FfxApi &api, ID3D12Device *device, const std::wstring &want)
{
	if (want.empty() || api.Query == nullptr)
		return 0;
	uint64_t count = 0;
	ffxQueryDescGetVersions q{};
	q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
	q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	q.device = device;
	q.outputCount = &count;
	if (api.Query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0 || count > 32)
		return 0;
	std::vector<uint64_t> ids(static_cast<size_t>(count));
	std::vector<const char *> names(static_cast<size_t>(count));
	q.versionIds = ids.data();
	q.versionNames = names.data();
	if (api.Query(nullptr, &q.header) != FFX_API_RETURN_OK)
		return 0;
	for (size_t i = 0; i < ids.size(); ++i) {
		if (names[i] == nullptr)
			continue;
		std::wstring n;
		for (const char *c = names[i]; *c != '\0'; ++c)
			n += static_cast<wchar_t>(static_cast<unsigned char>(*c));
		if (n == want)
			return ids[i];
	}
	return 0;
}

int run(const Args &a)
{
	const ULONGLONG t0 = GetTickCount64();
	if (!load_system(L"dxgi.dll") || !load_system(L"d3d12.dll")) {
		note(a, L"FAIL the System32 dxgi.dll or d3d12.dll would not load");
		return ExitSystemDlls;
	}

	IDXGIFactory4 *factory = nullptr;
	IDXGIAdapter1 *adapter = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
		FAILED(factory->EnumAdapterByLuid(a.luid, IID_PPV_ARGS(&adapter)))) {
		if (factory != nullptr)
			factory->Release();
		note(a, L"FAIL no adapter with LUID %08lx:%08lx", static_cast<unsigned long>(a.luid.HighPart),
			a.luid.LowPart);
		return ExitAdapter;
	}
	factory->Release();
	ID3D12Device *device = nullptr;
	const HRESULT dr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
	adapter->Release();
	if (FAILED(dr) || device == nullptr) {
		note(a, L"FAIL D3D12CreateDevice 0x%08lx", static_cast<unsigned long>(dr));
		return ExitDevice;
	}

	FfxApi api{};
	std::wstring error;
	if (!ffx_load(api, a.dll, error)) {
		note(a, L"FAIL %s", error.c_str());
		device->Release();
		return ExitRuntime;
	}

	ffxCreateBackendDX12Desc backend{};
	backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
	backend.device = device;
	ffxOverrideVersion pin{};
	const uint64_t pin_id = provider_id(api, device, a.provider);
	if (pin_id != 0) {
		pin.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
		pin.versionId = pin_id;
		backend.header.pNext = &pin.header;
	}
	ffxCreateContextDescUpscale upscale{};
	upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
	upscale.header.pNext = &backend.header;
	upscale.flags = kFsrCreateFlags;
	upscale.maxRenderSize = { a.desc.render_width, a.desc.render_height };
	upscale.maxUpscaleSize = { a.desc.upscale_width, a.desc.upscale_height };

	ffxContext ctx = nullptr;
	unsigned long seh = 0;
	const ffxReturnCode_t cr = create_guarded(api.CreateContext, &ctx, &upscale.header, &seh);
	if (seh != 0 || cr != FFX_API_RETURN_OK || ctx == nullptr) {
		note(a, L"FAIL CreateContext 0x%08x, exception 0x%08lx", static_cast<unsigned>(cr), seh);
		ffx_unload(api);
		device->Release();
		return seh != 0 ? ExitCrashed : ExitContext;
	}

	int code = ExitOk;
	{
		FsrPipelineCache cache;
		cache.open(device, a.cache);
		FsrWarmupTiming timing{};
		const bool ok = fsr_warmup_dispatch(api, &ctx, device, a.desc, &cache, &timing, &seh, nullptr, error);
		const FsrPipelineCacheStats &s = cache.st;
		wchar_t dxgi_path[MAX_PATH]{};
		GetModuleFileNameW(GetModuleHandleW(L"dxgi.dll"), dxgi_path, MAX_PATH);
		note(a, L"%s %ux%u -> %ux%u%s flags %u provider %s: %u asked, %u loaded, %u compiled, %u stored%s, "
			L"%llu ms (dxgi: %s)",
			ok ? L"ok  " : L"FAIL", a.desc.render_width, a.desc.render_height, a.desc.upscale_width,
			a.desc.upscale_height, a.desc.with_depth ? L"" : L" no depth", a.desc.dispatch_flags,
			a.provider.empty() ? L"auto" : a.provider.c_str(), s.requested, s.loaded, s.compiled, s.stored,
			s.saved ? L", saved" : L"", GetTickCount64() - t0, dxgi_path);
		if (seh != 0)
			code = ExitCrashed;
		else if (!ok)
			code = ExitWarmup;
		else if (!s.hooked || s.compiled > s.stored || (s.stored != 0 && !s.saved))
			code = ExitIncomplete;
	}
	if (seh == 0)
		api.DestroyContext(&ctx, nullptr);
	ffx_unload(api);
	device->Release();
	return code;
}

}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
	SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
	Args a;
	if (!parse(a))
		return ExitArgs;
	return run(a);
}
