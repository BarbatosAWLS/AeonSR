#include "aeon_sr/core/diagnostics.hpp"

#include "aeon_sr/core/gpu_vendor.hpp"
#include "aeon_sr/ngx/neural_hardware.hpp"
#include "aeon_sr/core/runtime_search.hpp"

#include <Windows.h>
#include <dxgi.h>

#include <cstdarg>
#include <cstdio>
#include <initializer_list>
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <share.h>

#pragma comment(lib, "version.lib")

#ifndef AEONSR_VERSION_STR
#define AEONSR_VERSION_STR "dev"
#endif

namespace aeon_sr {
namespace {

DiagEnvironment g_env;

struct LogState {
	std::mutex mutex;
	std::wstring path;
	FILE *file = nullptr;
	bool verbose = false;
	void (*sink)(DiagLevel, const char *) = nullptr;
	long long freq = 0;
	long long start = 0;
	std::deque<DiagEntry> history;
	std::map<std::string, std::wstring> states;
	std::wstring last_line;
	std::wstring last_error;
	uint32_t counts[4] = { 0, 0, 0, 0 };
};

LogState &state()
{
	static LogState s;
	return s;
}

float elapsed_locked(LogState &s)
{
	if (s.freq == 0) {
		LARGE_INTEGER f{};
		if (!QueryPerformanceFrequency(&f) || f.QuadPart == 0)
			return 0.0f;
		s.freq = f.QuadPart;
	}
	LARGE_INTEGER now{};
	if (!QueryPerformanceCounter(&now))
		return 0.0f;
	if (s.start == 0)
		s.start = now.QuadPart;
	return static_cast<float>(
		static_cast<double>(now.QuadPart - s.start) / static_cast<double>(s.freq));
}

std::wstring wformat(const wchar_t *fmt, ...)
{
	wchar_t buf[1024]{};
	va_list args;
	va_start(args, fmt);
	_vsnwprintf_s(buf, _TRUNCATE, fmt, args);
	va_end(args);
	return buf;
}

std::wstring widen(const char *s)
{
	std::wstring out;
	for (; s != nullptr && *s != '\0'; ++s)
		out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s)));
	return out;
}

bool path_exists(const std::wstring &path)
{
	const DWORD attrs = GetFileAttributesW(path.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring directory_of(const std::wstring &path)
{
	const auto slash = path.find_last_of(L"\\/");
	return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring under(const std::wstring &dir, const wchar_t *leaf)
{
	if (dir.empty())
		return leaf;
	if (dir.back() == L'\\' || dir.back() == L'/')
		return dir + leaf;
	return dir + L'\\' + leaf;
}

std::wstring module_path_of(HMODULE m)
{
	wchar_t buf[MAX_PATH]{};
	const DWORD n = GetModuleFileNameW(m, buf, MAX_PATH);
	return n > 0 && n < MAX_PATH ? std::wstring(buf) : std::wstring();
}

std::wstring file_stamp_of(const std::wstring &path)
{
	if (path.empty())
		return {};
	WIN32_FILE_ATTRIBUTE_DATA a{};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
		return {};
	SYSTEMTIME utc{}, local{};
	if (!FileTimeToSystemTime(&a.ftLastWriteTime, &utc) ||
		!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
		return {};
	static const wchar_t *const kMonth[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
		L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
	const unsigned m = local.wMonth >= 1 && local.wMonth <= 12 ? local.wMonth - 1u : 0u;
	wchar_t buf[64]{};
	_snwprintf_s(buf, _TRUNCATE, L"%s %2u %04u %02u:%02u",
		kMonth[m], local.wDay, local.wYear, local.wHour, local.wMinute);
	return buf;
}

std::wstring file_version_of(const std::wstring &path)
{
	DWORD handle = 0;
	const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
	if (size == 0)
		return {};
	std::vector<unsigned char> data(size);
	if (!GetFileVersionInfoW(path.c_str(), handle, size, data.data()))
		return {};
	VS_FIXEDFILEINFO *info = nullptr;
	UINT len = 0;
	if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &len) ||
		info == nullptr || len < sizeof(VS_FIXEDFILEINFO))
		return {};
	return wformat(L"%u.%u.%u.%u",
		HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
		HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

bool file_stat(const std::wstring &path, uint64_t *out_size, std::wstring *out_modified)
{
	WIN32_FILE_ATTRIBUTE_DATA a{};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a))
		return false;
	if (out_size != nullptr)
		*out_size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
	if (out_modified != nullptr) {
		SYSTEMTIME st{};
		FILETIME local{};
		if (FileTimeToLocalFileTime(&a.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &st))
			*out_modified = wformat(L"%04u-%02u-%02u", st.wYear, st.wMonth, st.wDay);
	}
	return true;
}

std::wstring read_os_version()
{
	typedef LONG(WINAPI * PfnRtlGetVersion)(PRTL_OSVERSIONINFOW);
	if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
		if (auto fn = reinterpret_cast<PfnRtlGetVersion>(GetProcAddress(ntdll, "RtlGetVersion"))) {
			RTL_OSVERSIONINFOW v{};
			v.dwOSVersionInfoSize = sizeof(v);
			if (fn(&v) == 0)
				return wformat(L"Windows %u.%u build %u", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
		}
	}
	return L"unknown";
}

void collect_adapters(std::vector<DiagAdapter> &out, uint32_t active_luid_low, int32_t active_luid_high)
{
	out.clear();
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) ||
		factory == nullptr)
		return;
	for (UINT i = 0;; ++i) {
		IDXGIAdapter *a = nullptr;
		if (factory->EnumAdapters(i, &a) != S_OK || a == nullptr)
			break;
		DXGI_ADAPTER_DESC d{};
		if (SUCCEEDED(a->GetDesc(&d))) {
			DiagAdapter r;
			r.name = d.Description;
			r.vendor_id = d.VendorId;
			r.device_id = d.DeviceId;
			r.subsys_id = d.SubSysId;
			r.revision = d.Revision;
			r.dedicated_vram = d.DedicatedVideoMemory;
			r.shared_memory = d.SharedSystemMemory;
			r.luid_low = d.AdapterLuid.LowPart;
			r.luid_high = d.AdapterLuid.HighPart;
			r.active = r.luid_low == active_luid_low && r.luid_high == active_luid_high;
			LARGE_INTEGER umd{};
			if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
				r.driver_version = wformat(L"%u.%u.%u.%u",
					HIWORD(umd.HighPart), LOWORD(umd.HighPart),
					HIWORD(umd.LowPart), LOWORD(umd.LowPart));
			out.push_back(std::move(r));
		}
		a->Release();
	}
	factory->Release();
}

void add_file(std::vector<DiagFile> &out, const wchar_t *label,
	const std::vector<std::wstring> &candidates)
{
	DiagFile f;
	f.label = label;
	for (const std::wstring &c : candidates) {
		if (c.empty())
			continue;
		if (f.path.empty())
			f.path = c;
		if (path_exists(c)) {
			f.path = c;
			f.present = true;
			break;
		}
	}
	if (f.present) {
		file_stat(f.path, &f.size, &f.modified);
		f.version = file_version_of(f.path);
	}
	out.push_back(std::move(f));
}

void collect_files(std::vector<DiagFile> &out, const std::wstring &addon_dir)
{
	out.clear();
	const std::wstring exe = directory_of(module_path_of(nullptr));

	const std::vector<std::wstring> stock = runtime_search_dirs(addon_dir, exe);
	const std::vector<std::wstring> fsr = fsr_search_dirs(addon_dir, exe);
	const std::vector<std::wstring> neural =
		preferred_search_dirs(addon_dir, exe, kNeuralPreferredSubdir);

	add_file(out, L"nvngx_dlss.dll", runtime_candidates(stock, L"nvngx_dlss.dll"));
	add_file(out, L"nvngx_dlssnr.dll", runtime_candidates(neural, L"nvngx_dlssnr.dll"));
	add_file(out, L"ngxshim\\nvngx.dll", {
		under(under(addon_dir, L"ngxshim"), L"nvngx.dll"),
		under(under(exe, L"ngxshim"), L"nvngx.dll") });
	add_file(out, L"amd_fidelityfx_upscaler_dx12.dll",
		runtime_candidates(fsr, L"amd_fidelityfx_upscaler_dx12.dll"));
	add_file(out, L"amd_fidelityfx_loader_dx12.dll",
		runtime_candidates(fsr, L"amd_fidelityfx_loader_dx12.dll"));
	add_file(out, L"libxess.dll", runtime_candidates(stock, L"libxess.dll"));
	add_file(out, L"libxess_dx11.dll", runtime_candidates(stock, L"libxess_dx11.dll"));
}

constexpr const wchar_t *kNvApiModule = sizeof(void *) == 8 ? L"nvapi64.dll" : L"nvapi.dll";
constexpr const wchar_t *kReShadeModule = sizeof(void *) == 8 ? L"ReShade64.dll" : L"ReShade32.dll";

std::wstring collect_foreign_modules()
{
	static const wchar_t *const kNames[] = {
		L"sl.interposer.dll", L"sl.dlss.dll", L"sl.dlss_g.dll",
		L"nvngx_dlssg.dll", L"nvngx_dlssd.dll", L"_nvngx.dll",
		L"OptiScaler.dll", L"amdxcffx64.dll",
		L"ffx_fsr3_dx11.dll", L"ffx_sdk_dx11.dll", L"amd_fidelityfx_dx12.dll",
		kNvApiModule,
	};
	std::wstring out;
	for (const wchar_t *n : kNames) {
		if (GetModuleHandleW(n) == nullptr)
			continue;
		if (!out.empty())
			out += L", ";
		out += n;
	}
	return out;
}

void write_locked(LogState &s, float time_s, DiagLevel level, const char *tag, const std::wstring &text)
{
	if (s.file == nullptr)
		return;
	fwprintf(s.file, L"[%9.3f] %-5hs %-9hs %s\n", static_cast<double>(time_s),
		diag_level_label(level), tag != nullptr ? tag : "-", text.c_str());
	fflush(s.file);
}

}

const char *diag_level_label(DiagLevel level) noexcept
{
	switch (level) {
	case DiagLevel::Trace: return "trace";
	case DiagLevel::Info: return "info";
	case DiagLevel::Warn: return "WARN";
	case DiagLevel::Error: return "ERROR";
	}
	return "?";
}

std::string diag_narrow(const std::wstring &text)
{
	if (text.empty())
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
		nullptr, 0, nullptr, nullptr);
	if (n <= 0)
		return {};
	std::string out(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
		out.data(), n, nullptr, nullptr);
	return out;
}

std::wstring diag_widen(const char *text)
{
	std::wstring out;
	for (; text != nullptr && *text != 0; ++text)
		out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
	return out;
}

void diag_open_log(const std::wstring &path)
{
	LogState &s = state();
	std::lock_guard lock(s.mutex);
	if (s.file != nullptr) {
		fclose(s.file);
		s.file = nullptr;
	}
	s.path = path;
	if (path.empty())
		return;
	s.file = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
}

std::wstring diag_log_path()
{
	LogState &s = state();
	std::lock_guard lock(s.mutex);
	return s.path;
}

void diag_close_log()
{
	LogState &s = state();
	std::lock_guard lock(s.mutex);
	if (s.file != nullptr) {
		fclose(s.file);
		s.file = nullptr;
	}
}

void diag_set_verbose(bool on) noexcept
{
	LogState &s = state();
	std::lock_guard lock(s.mutex);
	s.verbose = on;
}

bool diag_verbose() noexcept
{
	LogState &s = state();
	std::lock_guard lock(s.mutex);
	return s.verbose;
}

void diag_set_sink(void (*sink)(DiagLevel, const char *)) noexcept
{
	LogState &s = state();
	std::lock_guard lock(s.mutex);
	s.sink = sink;
}

void diag_log(DiagLevel level, const char *tag, const std::wstring &text)
{
	LogState &s = state();
	std::string mirror;
	void (*sink)(DiagLevel, const char *) = nullptr;
	{
		std::lock_guard lock(s.mutex);
		if (level == DiagLevel::Trace && !s.verbose)
			return;

		const float time_s = elapsed_locked(s);
		s.counts[static_cast<unsigned>(level)]++;
		s.last_line = text;
		if (level == DiagLevel::Error)
			s.last_error = text;

		write_locked(s, time_s, level, tag, text);

		if (level != DiagLevel::Trace) {
			DiagEntry e;
			e.time_s = time_s;
			e.level = level;
			e.tag = tag != nullptr ? tag : "-";
			e.text = text;
			s.history.push_back(std::move(e));
			while (s.history.size() > kDiagHistory)
				s.history.pop_front();
		}

		if (s.sink != nullptr && level >= DiagLevel::Warn) {
			sink = s.sink;
			mirror = "[Aeon SR] " + diag_narrow(text);
		}
	}
	if (sink != nullptr)
		sink(level, mirror.c_str());
}

void diag_logf(DiagLevel level, const char *tag, const wchar_t *fmt, ...)
{
	if (fmt == nullptr)
		return;
	if (level == DiagLevel::Trace && !diag_verbose())
		return;
	wchar_t buf[1024]{};
	va_list args;
	va_start(args, fmt);
	_vsnwprintf_s(buf, _TRUNCATE, fmt, args);
	va_end(args);
	diag_log(level, tag, buf);
}

void diag_state(const char *key, DiagLevel level, const char *tag, const std::wstring &text)
{
	if (key == nullptr)
		return;
	{
		LogState &s = state();
		std::lock_guard lock(s.mutex);
		auto it = s.states.find(key);
		if (it != s.states.end() && it->second == text)
			return;
		s.states[key] = text;
	}
	diag_log(level, tag, text);
}

std::vector<DiagEntry> diag_recent(size_t max_entries, DiagLevel min_level)
{
	LogState &s = state();
	std::lock_guard lock(s.mutex);
	std::vector<DiagEntry> out;
	for (auto it = s.history.rbegin(); it != s.history.rend() && out.size() < max_entries; ++it) {
		if (it->level >= min_level)
			out.push_back(*it);
	}
	std::reverse(out.begin(), out.end());
	return out;
}

uint32_t diag_count(DiagLevel level) noexcept
{
	LogState &s = state();
	std::lock_guard lock(s.mutex);
	return s.counts[static_cast<unsigned>(level)];
}

std::wstring diag_last_line()
{
	LogState &s = state();
	std::lock_guard lock(s.mutex);
	return s.last_line;
}

std::wstring diag_last_error()
{
	LogState &s = state();
	std::lock_guard lock(s.mutex);
	return s.last_error;
}

uint32_t nvidia_architecture_id()
{
	HMODULE nvapi = GetModuleHandleW(kNvApiModule);
	if (nvapi == nullptr)
		nvapi = LoadLibraryW(kNvApiModule);
	if (nvapi == nullptr)
		return 0u;
	typedef void *(*PfnQuery)(unsigned int);
	auto query = reinterpret_cast<PfnQuery>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
	if (query == nullptr)
		return 0u;
	auto initialize = reinterpret_cast<int (*)()>(query(0x0150E828u));
	auto enum_gpus = reinterpret_cast<int (*)(void **, unsigned int *)>(query(0xE5AC921Fu));
	auto get_arch = reinterpret_cast<int (*)(void *, void *)>(query(0xD8265D24u));
	if (initialize == nullptr || enum_gpus == nullptr || get_arch == nullptr)
		return 0u;
	if (initialize() != 0)
		return 0u;
	void *gpus[64]{};
	unsigned int count = 0;
	if (enum_gpus(gpus, &count) != 0 || count == 0)
		return 0u;
	struct ArchInfo { unsigned int version, architecture, implementation, revision; } info{};
	info.version = static_cast<unsigned int>(sizeof(ArchInfo)) | (2u << 16);
	if (get_arch(gpus[0], &info) != 0)
		return 0u;
	return info.architecture;
}

bool nvidia_driver_version(uint32_t luid_low, int32_t luid_high,
	uint32_t *out_major, uint32_t *out_minor)
{
	if (out_major == nullptr || out_minor == nullptr)
		return false;
	*out_major = 0;
	*out_minor = 0;
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) ||
		factory == nullptr)
		return false;
	bool got = false;
	for (UINT i = 0; !got; ++i) {
		IDXGIAdapter *a = nullptr;
		if (factory->EnumAdapters(i, &a) != S_OK || a == nullptr)
			break;
		DXGI_ADAPTER_DESC d{};
		LARGE_INTEGER umd{};
		if (SUCCEEDED(a->GetDesc(&d)) &&
			d.AdapterLuid.LowPart == luid_low && d.AdapterLuid.HighPart == luid_high &&
			d.VendorId == kVendorIdNvidia &&
			SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
			const uint32_t sub = static_cast<uint32_t>(HIWORD(umd.LowPart));
			const uint32_t build = static_cast<uint32_t>(LOWORD(umd.LowPart));
			const uint32_t nv = (sub % 10u) * 10000u + build;
			if (nv >= 10000u) {
				*out_major = nv / 100u;
				*out_minor = nv % 100u;
				got = true;
			}
		}
		a->Release();
	}
	factory->Release();
	return got;
}

void diag_collect_environment_local(void *addon_module)
{
	DiagEnvironment e;
	e.addon_version = widen(AEONSR_VERSION_STR);
	e.addon_path = module_path_of(static_cast<HMODULE>(addon_module));
	e.build_stamp = file_stamp_of(e.addon_path);
	if (e.build_stamp.empty())
		e.build_stamp = widen(__DATE__ " " __TIME__);
	e.addon_dir = directory_of(e.addon_path);
	e.exe_path = module_path_of(nullptr);
	e.os_version = read_os_version();

	static const wchar_t *const kReShadeNames[] = {
		L"dxgi.dll", L"d3d11.dll", L"d3d12.dll", L"d3d9.dll", L"opengl32.dll", kReShadeModule,
	};
	for (const wchar_t *n : kReShadeNames) {
		const HMODULE m = GetModuleHandleW(n);
		if (m == nullptr)
			continue;
		const std::wstring path = module_path_of(m);
		if (path.empty() || GetProcAddress(m, "ReShadeRegisterAddon") == nullptr)
			continue;
		e.reshade_path = path;
		e.reshade_version = file_version_of(path);
		break;
	}

	collect_files(e.files, e.addon_dir);
	e.foreign_modules = collect_foreign_modules();
	e.collected = true;
	g_env = std::move(e);
}

void diag_collect_environment_gpu()
{
	if (g_env.gpu_collected)
		return;
	g_env.gpu_collected = true;

	uint32_t active_vendor = 0, active_luid_low = 0;
	int32_t active_luid_high = 0;
	{
		IDXGIFactory1 *factory = nullptr;
		if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) &&
			factory != nullptr) {
			IDXGIAdapter *a = nullptr;
			if (factory->EnumAdapters(0, &a) == S_OK && a != nullptr) {
				DXGI_ADAPTER_DESC d{};
				if (SUCCEEDED(a->GetDesc(&d))) {
					active_vendor = d.VendorId;
					active_luid_low = d.AdapterLuid.LowPart;
					active_luid_high = d.AdapterLuid.HighPart;
					nvidia_driver_version(d.AdapterLuid.LowPart, d.AdapterLuid.HighPart,
						&g_env.nv_driver_major, &g_env.nv_driver_minor);
				}
				a->Release();
			}
			factory->Release();
		}
	}
	collect_adapters(g_env.adapters, active_luid_low, active_luid_high);
	bool any_nvidia = active_vendor == kVendorIdNvidia;
	for (const DiagAdapter &a : g_env.adapters)
		any_nvidia = any_nvidia || a.vendor_id == kVendorIdNvidia ||
			vendor_from_name(a.name) == GpuVendor::Nvidia;
	if (any_nvidia)
		g_env.nv_architecture = nvidia_architecture_id();
}

void diag_collect_environment(void *addon_module)
{
	diag_collect_environment_local(addon_module);
	diag_collect_environment_gpu();
}

const DiagEnvironment &diag_environment()
{
	return g_env;
}

void diag_log_environment()
{
	const DiagEnvironment &e = g_env;
	diag_logf(DiagLevel::Info, "aeonsr", L"Aeon SR %s (built %s)",
		e.addon_version.c_str(), e.build_stamp.c_str());
	diag_logf(DiagLevel::Info, "aeonsr", L"add-on: %s", e.addon_path.c_str());
	diag_logf(DiagLevel::Info, "aeonsr", L"game:   %s", e.exe_path.c_str());
	diag_logf(DiagLevel::Info, "aeonsr", L"host:   %s%s%s", e.os_version.c_str(),
		e.reshade_version.empty() ? L"" : L", ReShade ", e.reshade_version.c_str());
	for (const DiagFile &f : e.files) {
		if (f.present)
			diag_logf(DiagLevel::Info, "files", L"%s  %llu KB  %s  %s", f.label,
				static_cast<unsigned long long>(f.size / 1024ull),
				f.version.empty() ? L"no version" : f.version.c_str(), f.path.c_str());
		else
			diag_logf(DiagLevel::Info, "files", L"%s  not found", f.label);
	}
	if (!e.foreign_modules.empty())
		diag_logf(DiagLevel::Info, "files", L"already in this process: %s", e.foreign_modules.c_str());
}

void diag_log_environment_gpu()
{
	static bool logged = false;
	if (logged)
		return;
	logged = true;
	diag_collect_environment_gpu();
	const DiagEnvironment &e = g_env;
	for (const DiagAdapter &a : e.adapters) {
		diag_logf(DiagLevel::Info, "gpu", L"%s%s  vendor 0x%04X device 0x%04X  %llu MB  driver %s",
			a.name.c_str(), a.active ? L" (primary)" : L"",
			a.vendor_id, a.device_id,
			static_cast<unsigned long long>(a.dedicated_vram / (1024ull * 1024ull)),
			a.driver_version.empty() ? L"unknown" : a.driver_version.c_str());
	}
	if (e.nv_driver_major != 0)
		diag_logf(DiagLevel::Info, "gpu", L"NVIDIA driver %u.%02u, architecture 0x%03X (%hs)",
			e.nv_driver_major, e.nv_driver_minor, e.nv_architecture,
			neural_arch_name(e.nv_architecture));
}

}
