#include "aeon_sr/ngx/ngx_dlssg_files.hpp"

#include "aeon_sr/core/runtime_search.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"

#include <Windows.h>
#pragma comment(lib, "version.lib")

#include <cwctype>
#include <vector>

namespace aeon_sr {
namespace {

constexpr const wchar_t *kDlssg = L"nvngx_dlssg.dll";
constexpr const wchar_t *kDlss = L"nvngx_dlss.dll";

std::wstring lower(std::wstring s)
{
	for (wchar_t &c : s)
		c = static_cast<wchar_t>(std::towlower(c));
	return s;
}

std::wstring canon(const std::wstring &p)
{
	std::wstring s = lower(p);
	for (wchar_t &c : s)
		if (c == L'/')
			c = L'\\';
	while (s.size() > 3 && s.back() == L'\\')
		s.pop_back();
	return s;
}

bool same_path(const std::wstring &a, const std::wstring &b)
{
	return !a.empty() && !b.empty() && canon(a) == canon(b);
}

std::wstring file_name_of(const std::wstring &p)
{
	const size_t s = p.find_last_of(L"\\/");
	return s == std::wstring::npos ? p : p.substr(s + 1);
}

std::wstring dir_of(const std::wstring &p)
{
	const size_t s = p.find_last_of(L"\\/");
	return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

bool ngx_own_place(const std::wstring &path)
{
	const std::wstring l = canon(path);
	return l.find(L"\\driverstore\\") != std::wstring::npos || l.find(L"\\nvidia\\ngx\\models\\") != std::wstring::npos;
}

std::wstring version_field(const std::wstring &path, const wchar_t *field)
{
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
	if (size == 0)
		return std::wstring();
	std::vector<unsigned char> data(size);
	if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
		return std::wstring();
	struct LangCodepage {
		WORD language, codepage;
	} *pairs = nullptr;
	UINT bytes = 0;
	if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&pairs), &bytes) ||
		bytes < sizeof(LangCodepage))
		return std::wstring();
	wchar_t key[96];
	_snwprintf_s(key, _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\%ls", pairs[0].language, pairs[0].codepage, field);
	wchar_t *value = nullptr;
	UINT len = 0;
	if (VerQueryValueW(data.data(), key, reinterpret_cast<void **>(&value), &len) && value != nullptr && len > 1)
		return std::wstring(value);
	return std::wstring();
}

std::wstring windows_dir()
{
	wchar_t b[MAX_PATH]{};
	const UINT n = GetWindowsDirectoryW(b, MAX_PATH);
	return n > 0 && n < MAX_PATH ? canon(std::wstring(b, n)) + L"\\" : std::wstring();
}

}

const wchar_t *dlssg_source_label(DlssgSource s) noexcept
{
	switch (s) {
	case DlssgSource::Loaded: return L"already loaded in the game";
	case DlssgSource::GameFolder: return L"in the game's folder";
	case DlssgSource::AddonRuntime: return L"the add-on's runtime copy";
	default: return L"none found";
	}
}

DlssgChoice choose_dlssg(const std::vector<std::wstring> &loaded, const std::wstring &exe_dir,
	const std::wstring &addon_file, const std::function<bool(const std::wstring &)> &exists)
{
	DlssgChoice c;
	c.addon_file = addon_file;
	for (const std::wstring &m : loaded) {
		if (lower(file_name_of(m)) != kDlssg || ngx_own_place(m) || same_path(m, addon_file))
			continue;
		c.source = DlssgSource::Loaded;
		c.file = m;
		c.dir = dir_of(m);
		c.why = L"already loaded in the game process (an unlocker's or the game's own), so the add-on asks NGX "
			L"for the same one";
		return c;
	}
	if (!exe_dir.empty()) {
		const std::wstring beside = path_under(exe_dir, kDlssg);
		if (!same_path(beside, addon_file) && exists(beside)) {
			c.source = DlssgSource::GameFolder;
			c.file = beside;
			c.dir = dir_of(beside);
			c.why = L"in the game's folder (the game's own or an unlocker's), ahead of the add-on's copy";
			return c;
		}
	}
	if (!addon_file.empty()) {
		c.source = DlssgSource::AddonRuntime;
		c.file = addon_file;
		c.dir = dir_of(addon_file);
		c.why = same_path(c.dir, exe_dir)
			? L"beside the game's executable, where the add-on sits, whose own runtime search looks there first "
			  L"(the game's copy or the add-on's: none is loaded in the game)"
			: L"the add-on's runtime copy: none is loaded in the game or beside it";
		return c;
	}
	c.why = L"none in the game, beside it or in the add-on's runtime folder: NGX falls back on the driver's own";
	return c;
}

NgxPathList ngx_shared_path_list(const std::wstring &dlss_dir, const DlssgChoice &g,
	const std::function<bool(const std::wstring &)> &exists,
	const std::function<uint64_t(const std::wstring &)> &version_of)
{
	NgxPathList out;
	if (g.dir.empty() || same_path(g.dir, dlss_dir)) {
		if (!dlss_dir.empty())
			out.dirs.push_back(dlss_dir);
		else if (!g.dir.empty())
			out.dirs.push_back(g.dir);
		return out;
	}
	if (dlss_dir.empty()) {
		out.dirs.push_back(g.dir);
		return out;
	}
	if (exists(path_under(g.dir, kDlss))) {
		out.dirs = { dlss_dir, g.dir };
		out.note = L"its folder also holds an nvngx_dlss.dll (" + version_text(version_of(path_under(g.dir, kDlss))) +
			L"), which NGX would then hand to DLSS upscaling in place of " + path_under(dlss_dir, kDlss) + L" (" +
			version_text(version_of(path_under(dlss_dir, kDlss))) + L"); it goes after that folder, so nvngx_dlssg.dll "
			L"comes from " + dlss_dir + L" when it holds one";
		return out;
	}
	out.dirs = { g.dir, dlss_dir };
	return out;
}

std::wstring first_holding(const std::vector<std::wstring> &dirs, const wchar_t *file_name,
	const std::function<bool(const std::wstring &)> &exists)
{
	for (const std::wstring &d : dirs) {
		const std::wstring p = path_under(d, file_name);
		if (exists(p))
			return p;
	}
	return std::wstring();
}

uint64_t file_version_number(const std::wstring &path)
{
	DWORD handle = 0;
	const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
	if (size == 0)
		return 0;
	std::vector<unsigned char> data(size);
	if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data()))
		return 0;
	VS_FIXEDFILEINFO *info = nullptr;
	UINT len = 0;
	if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &len) || info == nullptr ||
		len < sizeof(VS_FIXEDFILEINFO))
		return 0;
	return (static_cast<uint64_t>(info->dwFileVersionMS) << 32) | info->dwFileVersionLS;
}

std::wstring version_text(uint64_t v)
{
	if (v == 0)
		return L"(no version)";
	wchar_t b[48]{};
	_snwprintf_s(b, _TRUNCATE, L"%u.%u.%u.%u", static_cast<unsigned>(v >> 48), static_cast<unsigned>((v >> 32) & 0xffff),
		static_cast<unsigned>((v >> 16) & 0xffff), static_cast<unsigned>(v & 0xffff));
	return b;
}

DlssgChoice find_dlssg(const std::wstring &addon_dir)
{
	const std::wstring exe = exe_directory_w();
	std::wstring addon_file;
	for (const std::wstring &p : runtime_candidates(runtime_search_dirs(addon_dir, exe), kDlssg))
		if (file_exists_w(p)) {
			addon_file = p;
			break;
		}
	return choose_dlssg(loaded_module_paths(), exe, addon_file, file_exists_w);
}

NgxPathList ngx_path_list_for(const std::wstring &dlss_dir, const DlssgChoice &g)
{
	return ngx_shared_path_list(dlss_dir, g, file_exists_w, file_version_number);
}

std::wstring describe_ngx_modules()
{
	static const wchar_t *const kProxyNames[] = { L"version.dll", L"winmm.dll", L"dinput8.dll", L"dxgi.dll",
		L"dbghelp.dll", L"d3d12.dll", L"d3d11.dll", L"winhttp.dll", L"wininet.dll", L"dsound.dll", L"nvngx.dll",
		L"nvapi64.dll" };
	static const wchar_t *const kWords[] = { L"unlock", L"dlssg", L"encore", L"mfg", L"enabler", L"optiscaler" };
	const std::wstring windows = windows_dir();
	std::wstring ngx, beside;
	for (const std::wstring &m : loaded_module_paths()) {
		const std::wstring name = lower(file_name_of(m));
		const std::wstring l = canon(m);
		const bool in_windows = !windows.empty() && l.rfind(windows, 0) == 0 && l.find(L"\\driverstore\\") ==
			std::wstring::npos;
		const bool core = name == L"_nvngx.dll" || (name == L"nvngx.dll" && l.find(L"\\driverstore\\") != std::wstring::npos);
		const bool dlssg = name == kDlssg || l.find(L"\\ngx\\models\\dlssg\\") != std::wstring::npos;
		if (core || dlssg) {
			if (!ngx.empty())
				ngx += L"; ";
			ngx += std::wstring(core ? L"NGX core " : L"DLSS-G ") + version_text(file_version_number(m)) + L" " + m;
			continue;
		}
		if (in_windows)
			continue;
		bool listed = name.size() > 4 && name.compare(name.size() - 4, 4, L".asi") == 0;
		for (const wchar_t *p : kProxyNames)
			listed = listed || name == p;
		for (const wchar_t *w : kWords)
			listed = listed || name.find(w) != std::wstring::npos;
		if (!listed)
			continue;
		std::wstring product = version_field(m, L"ProductName");
		if (product.empty())
			product = version_field(m, L"FileDescription");
		if (!beside.empty())
			beside += L"; ";
		beside += m + (product.empty() ? L"" : L" (" + product + L")");
	}
	return L"NGX modules: " + (ngx.empty() ? std::wstring(L"none") : ngx) + L" | loaders and proxies outside Windows: " +
		(beside.empty() ? std::wstring(L"none") : beside);
}

}
