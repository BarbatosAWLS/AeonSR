#include "aeon_sr/ngx/ngx_override.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/core/settings.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"

#include <Windows.h>

#include <cwctype>
#include <iterator>
#include <mutex>

namespace aeon_sr {
namespace {

std::wstring full_path(const std::wstring &path)
{
	wchar_t buf[MAX_PATH * 2]{};
	const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buf)), buf, nullptr);
	return n == 0 || n >= std::size(buf) ? path : std::wstring(buf, n);
}

std::wstring env_value(const wchar_t *name)
{
	wchar_t buf[MAX_PATH * 2]{};
	const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
	return n == 0 || n >= std::size(buf) ? std::wstring() : std::wstring(buf, n);
}

std::wstring lower(std::wstring s)
{
	for (wchar_t &c : s)
		c = static_cast<wchar_t>(std::towlower(c));
	return s;
}

}

std::wstring ngx_override_path_from(const std::wstring &env, const std::wstring &exe_dir,
	const std::wstring &addon_dir, bool under_wine)
{
	if (!env.empty())
		return file_exists_w(env) ? full_path(env) : std::wstring();
	if (!under_wine)
		return std::wstring();
	for (const std::wstring *dir : { &exe_dir, &addon_dir }) {
		if (dir->empty())
			continue;
		const std::wstring candidate = join_path(join_path(*dir, L"d4r"), L"nvngx.dll");
		if (file_exists_w(candidate))
			return full_path(candidate);
	}
	return std::wstring();
}

bool running_under_wine() noexcept
{
	const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

const std::wstring &ngx_override_path(const std::wstring &addon_dir)
{
	static std::once_flag once;
	static std::wstring path;
	std::call_once(once, [&addon_dir] {
		const std::wstring env = env_value(L"AEONSR_NGX_RUNTIME");
		path = ngx_override_path_from(env, exe_directory_w(), addon_dir, running_under_wine());
		if (!env.empty() && path.empty())
			diag_warn("dlss", L"AEONSR_NGX_RUNTIME names " + env + L", which does not exist; DLSS uses NVIDIA's "
				L"own NGX");
	});
	return path;
}

std::wstring ngx_override_dlss_dir(const std::wstring &override_path)
{
	const size_t slash = override_path.find_last_of(L"\\/");
	if (slash == std::wstring::npos)
		return std::wstring();
	const std::wstring dir = override_path.substr(0, slash);
	return file_exists_w(ngx_dll_path(dir)) ? dir : std::wstring();
}

bool ngx_same_file(const std::wstring &a, const std::wstring &b)
{
	const auto id = [](const std::wstring &p, BY_HANDLE_FILE_INFORMATION *out) {
		HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		const bool ok = GetFileInformationByHandle(h, out) != FALSE;
		CloseHandle(h);
		return ok;
	};
	BY_HANDLE_FILE_INFORMATION x{}, y{};
	if (id(a, &x) && id(b, &y))
		return x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh &&
			x.nFileIndexLow == y.nFileIndexLow;
	return lower(full_path(a)) == lower(full_path(b));
}

}
