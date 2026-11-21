#include "aeon_sr/ngx/ngx_dlssg_unlocker.hpp"

#include "aeon_sr/core/diagnostics.hpp"
#include "aeon_sr/ngx/ngx_common.hpp"

#include <Windows.h>
#pragma comment(lib, "version.lib")

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <mutex>

namespace aeon_sr {
namespace {

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

std::wstring file_name_of(const std::wstring &p)
{
	const size_t s = p.find_last_of(L"\\/");
	return s == std::wstring::npos ? p : p.substr(s + 1);
}

std::wstring join(const std::wstring &dir, const std::wstring &name)
{
	if (dir.empty())
		return name;
	const wchar_t last = dir.back();
	return last == L'\\' || last == L'/' ? dir + name : dir + L"\\" + name;
}

std::wstring full_path(const std::wstring &p)
{
	wchar_t b[MAX_PATH * 2]{};
	const DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, b, nullptr);
	return n > 0 && n < MAX_PATH * 2 ? std::wstring(b, n) : p;
}

bool is_file(const std::wstring &p)
{
	const DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool is_dir(const std::wstring &p)
{
	const DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ends_with(const std::wstring &s, const wchar_t *tail)
{
	const size_t n = std::wcslen(tail);
	return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

bool under(const std::wstring &path, const std::wstring &dir)
{
	if (dir.empty())
		return false;
	const std::wstring p = canon(full_path(path));
	const std::wstring d = canon(dir) + L"\\";
	return p.size() > d.size() && p.compare(0, d.size(), d) == 0;
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

std::wstring foreign_owner(const std::wstring &path)
{
	const std::wstring company = lower(version_field(path, L"CompanyName"));
	if (company.find(L"microsoft") != std::wstring::npos)
		return L"Microsoft's";
	const std::wstring said = lower(version_field(path, L"ProductName") + L"|" + version_field(path, L"FileDescription") +
		L"|" + version_field(path, L"OriginalFilename"));
	static const struct {
		const wchar_t *word, *owner;
	} kOwners[] = {
		{ L"reshade", L"ReShade's" }, { L"optiscaler", L"OptiScaler's" }, { L"special k", L"Special K's" },
		{ L"dxvk", L"DXVK's" },
	};
	for (const auto &o : kOwners)
		if (said.find(o.word) != std::wstring::npos)
			return o.owner;
	return std::wstring();
}

std::wstring companion_family(const std::wstring &dir)
{
	if (is_file(join(dir, L"dlssg_sm86.ini")) || is_dir(join(dir, L"dlssg_sm86")))
		return L"dlssg_sm86";
	if (is_file(join(dir, L"rtx-encore.jsonc")))
		return L"RTX Encore";
	if (is_file(join(dir, L"RTXMFG-Universal.json")) || is_file(join(dir, L"RTX40MFG-UI.ini")))
		return L"MFG Unlocker";
	return std::wstring();
}

std::vector<std::wstring> files_in(const std::wstring &dir)
{
	std::vector<std::wstring> out;
	WIN32_FIND_DATAW fd{};
	const HANDLE h = FindFirstFileW(join(dir, L"*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE)
		return out;
	do {
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			out.emplace_back(fd.cFileName);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return out;
}

uint16_t own_machine() noexcept
{
#if defined(_M_ARM64)
	return IMAGE_FILE_MACHINE_ARM64;
#elif defined(_WIN64)
	return IMAGE_FILE_MACHINE_AMD64;
#else
	return IMAGE_FILE_MACHINE_I386;
#endif
}

std::wstring system_windows_dir()
{
	wchar_t b[MAX_PATH]{};
	const UINT n = GetSystemWindowsDirectoryW(b, MAX_PATH);
	return n > 0 && n < MAX_PATH ? std::wstring(b, n) : std::wstring();
}

std::wstring process_name()
{
	wchar_t b[MAX_PATH * 2]{};
	const DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
	return n > 0 ? file_name_of(std::wstring(b, n)) : std::wstring(L"this process");
}

std::wstring ngx_already_here()
{
	std::wstring found;
	for (const wchar_t *name : { L"_nvngx.dll", L"nvngx.dll", L"nvngx_dlssg.dll" })
		if (GetModuleHandleW(name) != nullptr)
			found += (found.empty() ? L"" : L", ") + std::wstring(name);
	return found;
}

constexpr DWORD kLoadFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;

HMODULE load_guarded(const wchar_t *path, DWORD *error, DWORD *seh) noexcept
{
	__try {
		HMODULE h = LoadLibraryExW(path, nullptr, kLoadFlags);
		*error = h == nullptr ? GetLastError() : 0;
		return h;
	} __except ((*seh = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

std::wstring short_machine(uint16_t machine)
{
	switch (machine) {
	case IMAGE_FILE_MACHINE_AMD64: return L"x64";
	case IMAGE_FILE_MACHINE_I386: return L"x86";
	case IMAGE_FILE_MACHINE_ARM64: return L"ARM64";
	default: return pe_machine_label(machine);
	}
}

void judge(UnlockerCandidate &c, uint16_t want, const std::wstring &windows_dir, bool proxy)
{
	c.header = pe_header_of_file(c.path);
	const auto refuse = [&](const std::wstring &why, const std::wstring &brief) {
		c.why = why;
		c.brief = brief;
	};
	if (under(c.path, windows_dir))
		refuse(L"a file in Windows' own folders, which is never loaded this way", L"in Windows' folders");
	else if (!is_file(c.path))
		refuse(L"no such file", L"no such file");
	else if (!c.header.pe)
		refuse(L"not a program file", L"not a program file");
	else if (!c.header.dll)
		refuse(L"a program, not a DLL", L"not a DLL");
	else if (c.header.machine != want)
		refuse(L"it is " + pe_machine_label(c.header.machine) + L", and " + process_name() + L" loads " +
			pe_machine_label(want) + L" only", short_machine(c.header.machine) + L", needs " + short_machine(want));
	else if (proxy) {
		const std::wstring owner = foreign_owner(c.path);
		if (!owner.empty())
			refuse(owner + L", not an unlocker", owner);
	}
	c.usable = c.why.empty();
}

std::wstring short_leaf(const std::wstring &path)
{
	const std::wstring l = file_name_of(path);
	if (l.size() <= 40)
		return l;
	size_t keep = 37;
	if (IS_HIGH_SURROGATE(l[keep - 1]))
		--keep;
	return l.substr(0, keep) + L"...";
}

std::wstring dir_of(const std::wstring &p)
{
	const size_t s = p.find_last_of(L"\\/");
	return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

std::wstring unlocker_confirmed(const UnlockerCandidate &c, HMODULE module)
{
	if (c.family == L"set in AeonSR.ini")
		return L"the file AeonSR.ini names";
	if (!dlssg_unlocker_family(file_name_of(c.path)).empty())
		return L"its own file name";
	if (c.family == L"dlssg_sm86") {
		wchar_t name[64]{};
		_snwprintf_s(name, _TRUNCATE, L"loader_%lu.jsonl", GetCurrentProcessId());
		const std::wstring log = join(join(join(dir_of(c.path), L"dlssg_sm86"), L"logs"), name);
		if (is_file(log))
			return L"its log " + log;
	}
	if (c.family == L"MFG Unlocker" && module != nullptr && GetProcAddress(module, "XeFGModelName") != nullptr)
		return L"its export XeFGModelName";
	return std::wstring();
}

struct State {
	std::mutex lock;
	UnlockerConfig config;
	bool done = false;
	std::wstring note;
};

State &state()
{
	static State s;
	return s;
}

}

PeHeader pe_header_of(const uint8_t *bytes, size_t size) noexcept
{
	PeHeader h;
	if (bytes == nullptr || size < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z')
		return h;
	uint32_t lfanew = 0;
	std::memcpy(&lfanew, bytes + 0x3C, sizeof(lfanew));
	if (lfanew < 0x40 || lfanew > size || size - lfanew < 24)
		return h;
	if (std::memcmp(bytes + lfanew, "PE\0\0", 4) != 0)
		return h;
	uint16_t machine = 0, characteristics = 0;
	std::memcpy(&machine, bytes + lfanew + 4, sizeof(machine));
	std::memcpy(&characteristics, bytes + lfanew + 22, sizeof(characteristics));
	h.pe = true;
	h.machine = machine;
	h.dll = (characteristics & IMAGE_FILE_DLL) != 0;
	return h;
}

PeHeader pe_header_of_file(const std::wstring &path)
{
	const HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE)
		return PeHeader{};
	std::vector<uint8_t> b(64 * 1024);
	DWORD got = 0;
	const BOOL ok = ReadFile(f, b.data(), static_cast<DWORD>(b.size()), &got, nullptr);
	CloseHandle(f);
	return ok ? pe_header_of(b.data(), got) : PeHeader{};
}

std::wstring pe_machine_label(uint16_t machine)
{
	switch (machine) {
	case IMAGE_FILE_MACHINE_AMD64: return L"x64";
	case IMAGE_FILE_MACHINE_I386: return L"x86 (32-bit)";
	case IMAGE_FILE_MACHINE_ARM64: return L"ARM64";
	case IMAGE_FILE_MACHINE_ARMNT: return L"ARM (32-bit)";
	case 0: return L"not a program file";
	default: {
		wchar_t b[32]{};
		_snwprintf_s(b, _TRUNCATE, L"machine 0x%04X", static_cast<unsigned>(machine));
		return b;
	}
	}
}

std::wstring dlssg_unlocker_family(const std::wstring &file_name)
{
	const std::wstring n = lower(file_name);
	if (!ends_with(n, L".dll") && !ends_with(n, L".asi"))
		return std::wstring();
	static const struct {
		const wchar_t *prefix, *family;
	} kNames[] = {
		{ L"dlssg_sm86", L"dlssg_sm86" }, { L"dlssg_for_sm86", L"dlssg_sm86" },
		{ L"rtx-encore", L"RTX Encore" },
		{ L"mfgunlocker", L"MFG Unlocker" }, { L"mfg-unlocker", L"MFG Unlocker" },
	};
	for (const auto &k : kNames)
		if (n.compare(0, std::wcslen(k.prefix), k.prefix) == 0)
			return k.family;
	return std::wstring();
}

int dlssg_unlocker_proxy_rank(const std::wstring &file_name)
{
	static const wchar_t *const kProxies[] = {
		L"version.dll", L"winmm.dll", L"dbghelp.dll", L"dinput8.dll", L"dxgi.dll", L"d3d12.dll", L"d3d11.dll",
		L"xinput1_4.dll", L"xinput1_3.dll", L"dsound.dll", L"wininet.dll", L"winhttp.dll",
	};
	const std::wstring n = lower(file_name);
	for (int i = 0; i < static_cast<int>(std::size(kProxies)); ++i)
		if (n == kProxies[i])
			return i;
	return -1;
}

std::wstring resolve_unlocker_setting(const std::wstring &setting, const std::wstring &base_dir)
{
	std::wstring s = setting;
	for (wchar_t &ch : s)
		if (ch == L'/')
			ch = L'\\';
	const bool absolute = (s.size() >= 2 && s[1] == L':') || (!s.empty() && s[0] == L'\\');
	return full_path(absolute ? s : join(base_dir, s));
}

UnlockerPlan plan_dlssg_unlocker(const std::vector<std::wstring> &dirs, const std::wstring &setting,
	const std::wstring &base_dir, uint16_t want, const std::wstring &windows_dir)
{
	UnlockerPlan plan;
	const std::wstring s = lower(setting);
	if (s == L"0" || s == L"off" || s == L"none") {
		plan.disabled = true;
		return plan;
	}
	if (!setting.empty()) {
		plan.setting_path = resolve_unlocker_setting(setting, base_dir);
		plan.searched = plan.setting_path;
		UnlockerCandidate c;
		c.path = plan.setting_path;
		c.family = L"set in AeonSR.ini";
		judge(c, want, windows_dir, false);
		plan.found.push_back(c);
		plan.chosen = c.usable ? 0 : -1;
		return plan;
	}
	std::vector<std::wstring> seen;
	const auto looked = [&](const std::wstring &d) {
		const std::wstring c = canon(full_path(d));
		if (std::find(seen.begin(), seen.end(), c) != seen.end())
			return true;
		seen.push_back(c);
		return false;
	};
	for (const std::wstring &dir : dirs) {
		if (dir.empty())
			continue;
		for (const wchar_t *sub : { L"", L"plugins", L"scripts" }) {
			const std::wstring folder = *sub != L'\0' ? join(dir, sub) : full_path(dir);
			if (looked(folder) || !is_dir(folder))
				continue;
			plan.searched += (plan.searched.empty() ? L"" : L", ") + folder;
			std::vector<std::wstring> names = files_in(folder);
			std::sort(names.begin(), names.end(),
				[](const std::wstring &a, const std::wstring &b) { return lower(a) < lower(b); });
			for (const std::wstring &n : names) {
				const std::wstring family = dlssg_unlocker_family(n);
				if (family.empty())
					continue;
				UnlockerCandidate c;
				c.path = join(folder, n);
				c.family = family;
				judge(c, want, windows_dir, false);
				plan.found.push_back(c);
			}
			const std::wstring companion = *sub == L'\0' ? companion_family(folder) : std::wstring();
			if (companion.empty())
				continue;
			std::vector<std::wstring> proxies;
			for (const std::wstring &n : names)
				if (dlssg_unlocker_proxy_rank(n) >= 0)
					proxies.push_back(n);
			std::sort(proxies.begin(), proxies.end(), [](const std::wstring &a, const std::wstring &b) {
				return dlssg_unlocker_proxy_rank(a) < dlssg_unlocker_proxy_rank(b);
			});
			for (const std::wstring &n : proxies) {
				UnlockerCandidate c;
				c.path = join(folder, n);
				c.family = companion;
				judge(c, want, windows_dir, true);
				plan.found.push_back(c);
			}
		}
	}
	for (size_t i = 0; i < plan.found.size(); ++i)
		if (plan.found[i].usable) {
			plan.chosen = static_cast<int>(i);
			break;
		}
	return plan;
}

std::wstring read_unlocker_setting(const std::wstring &ini_path)
{
	std::ifstream in(ini_path.c_str(), std::ios::binary);
	if (!in)
		return std::wstring();
	std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
		static_cast<unsigned char>(text[2]) == 0xBF)
		text.erase(0, 3);
	const auto trim = [](std::string v) {
		const size_t a = v.find_first_not_of(" \t\r\n");
		const size_t b = v.find_last_not_of(" \t\r\n");
		return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
	};
	size_t at = 0;
	std::string value;
	bool found = false;
	while (at <= text.size()) {
		const size_t end = std::min(text.find('\n', at), text.size());
		const std::string line = text.substr(at, end - at);
		at = end + 1;
		const size_t eq = line.find('=');
		if (eq == std::string::npos || trim(line.substr(0, eq)) != "FrameGenerationUnlocker")
			continue;
		value = trim(line.substr(eq + 1));
		found = true;
	}
	if (!found)
		return std::wstring();
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
		value = value.substr(1, value.size() - 2);
	if (value.empty())
		return std::wstring();
	for (const UINT cp : { static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP) }) {
		const DWORD flags = cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
		const int n = MultiByteToWideChar(cp, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
		if (n <= 0)
			continue;
		std::wstring w(static_cast<size_t>(n), L'\0');
		MultiByteToWideChar(cp, flags, value.data(), static_cast<int>(value.size()), w.data(), n);
		return w;
	}
	return std::wstring();
}

void configure_dlssg_unlocker(const UnlockerConfig &config)
{
	State &s = state();
	std::lock_guard<std::mutex> guard(s.lock);
	s.config = config;
}

std::wstring prepare_dlssg_unlocker(const std::wstring &addon_dir)
{
	State &s = state();
	std::lock_guard<std::mutex> guard(s.lock);
	if (s.done)
		return s.note;
	s.done = true;
	const UnlockerConfig &cfg = s.config;
	const std::wstring game_dir = cfg.game_dir.empty() ? exe_directory_w() : cfg.game_dir;
	const std::wstring addon = cfg.addon_dir.empty() ? addon_dir : cfg.addon_dir;
	const std::wstring setting = cfg.setting_given ? cfg.setting : read_unlocker_setting(join(addon, L"AeonSR.ini"));
	const UnlockerPlan plan = plan_dlssg_unlocker({ game_dir, addon }, setting, addon, own_machine(),
		system_windows_dir());
	const bool load = cfg.mode == UnlockerMode::Load;
	const std::wstring proc = process_name();

	if (plan.disabled) {
		s.note = L"unlockers off (FrameGenerationUnlocker=0)";
		diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation unlocker: none loaded, AeonSR.ini says "
			L"FrameGenerationUnlocker=0");
		return s.note;
	}
	std::wstring list;
	for (const UnlockerCandidate &c : plan.found)
		list += (list.empty() ? L"" : L"; ") + c.path + L" (" + c.family + L", " +
			(c.header.pe ? pe_machine_label(c.header.machine) : std::wstring(L"not a program file")) +
			(c.usable ? L")" : L", not used: " + c.why + L")");
	diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation unlocker: %ls %ls; %ls",
		plan.setting_path.empty() ? L"looked in" : L"AeonSR.ini's FrameGenerationUnlocker names",
		plan.searched.empty() ? L"(no folder)" : plan.searched.c_str(),
		list.empty() ? L"none found (dlssg_sm86, RTX Encore and MFG Unlocker by their file names, or a proxy name "
			L"beside their own files)" : (L"found " + list).c_str());

	if (plan.found.empty()) {
		s.note = load ? L"no unlocker found" : L"no unlocker beside the game";
		return s.note;
	}
	if (plan.chosen < 0) {
		const UnlockerCandidate &c = plan.found.front();
		s.note = L"unlocker " + short_leaf(c.path) + L" not loaded: " + c.brief;
		diag_logf(DiagLevel::Warn, "framegen", L"DLSS Frame Generation unlocker: none %ls %ls: %ls",
			load ? L"loaded into" : L"usable in", proc.c_str(), list.c_str());
		return s.note;
	}
	if (!load) {
		const UnlockerCandidate *unconfirmed = nullptr;
		for (const UnlockerCandidate &c : plan.found) {
			if (!c.usable)
				continue;
			const HMODULE m = GetModuleHandleW(c.path.c_str());
			if (m == nullptr)
				continue;
			const std::wstring evidence = unlocker_confirmed(c, m);
			if (evidence.empty()) {
				if (unconfirmed == nullptr)
					unconfirmed = &c;
				continue;
			}
			s.note = L"unlocker " + short_leaf(c.path) + L" is loaded in the game";
			diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation unlocker: %ls (%ls) is loaded in %ls (%ls)",
				c.path.c_str(), c.family.c_str(), proc.c_str(), evidence.c_str());
			return s.note;
		}
		if (unconfirmed != nullptr) {
			s.note = L"proxy " + short_leaf(unconfirmed->path) + L" is loaded, not confirmed as the unlocker";
			diag_logf(DiagLevel::Warn, "framegen", L"DLSS Frame Generation unlocker: %ls, a proxy name beside %ls's "
				L"files, is loaded in %ls, but nothing shows it is %ls (no log of it for this process, no export of "
				L"its): another program may use that name", unconfirmed->path.c_str(), unconfirmed->family.c_str(),
				proc.c_str(), unconfirmed->family.c_str());
			return s.note;
		}
		const UnlockerCandidate &c = plan.found[static_cast<size_t>(plan.chosen)];
		s.note = L"unlocker " + short_leaf(c.path) + L" not loaded in the game";
		diag_logf(DiagLevel::Warn, "framegen", L"DLSS Frame Generation unlocker: %ls is not loaded in %ls: a 64-bit "
			L"game loads an unlocker itself, under a name it imports (version.dll ...) or through an ASI loader",
			c.path.c_str(), proc.c_str());
		return s.note;
	}
	const UnlockerCandidate &c = plan.found[static_cast<size_t>(plan.chosen)];
	for (size_t i = 0; i < plan.found.size(); ++i)
		if (static_cast<int>(i) != plan.chosen && plan.found[i].usable)
			diag_logf(DiagLevel::Warn, "framegen", L"DLSS Frame Generation unlocker: %ls left out: only one "
				L"unlocker is loaded (two hook the same calls)", plan.found[i].path.c_str());
	const std::wstring leaf = short_leaf(c.path);
	if (GetModuleHandleW(c.path.c_str()) != nullptr) {
		s.note = L"unlocker " + leaf + L" already loaded";
		diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation unlocker: %ls is already loaded in %ls",
			c.path.c_str(), proc.c_str());
		return s.note;
	}
	const std::wstring before = ngx_already_here();
	diag_logf(before.empty() ? DiagLevel::Info : DiagLevel::Warn, "framegen", L"DLSS Frame Generation unlocker: "
		L"loading %ls (%ls, %ls) into %ls; %ls", c.path.c_str(), c.family.c_str(),
		pe_machine_label(c.header.machine).c_str(), proc.c_str(), before.empty()
			? L"nothing of NGX is in this process yet" : (L"NGX is in this process already (" + before +
			L"), so the unlocker may come too late").c_str());
	LARGE_INTEGER f{}, t0{}, t1{};
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	DWORD error = 0, seh = 0;
	const HMODULE h = load_guarded(c.path.c_str(), &error, &seh);
	QueryPerformanceCounter(&t1);
	const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
	if (h == nullptr) {
		wchar_t b[96]{};
		if (seh != 0)
			_snwprintf_s(b, _TRUNCATE, L"exception 0x%08lX", seh);
		else
			_snwprintf_s(b, _TRUNCATE, L"error %lu", error);
		s.note = L"unlocker " + leaf + L" did not load (" + b + L")";
		diag_logf(DiagLevel::Warn, "framegen", L"DLSS Frame Generation unlocker: %ls did not load into %ls: %ls",
			c.path.c_str(), proc.c_str(), b);
		return s.note;
	}
	s.note = L"unlocker " + leaf + L" loaded before NGX";
	diag_logf(DiagLevel::Info, "framegen", L"DLSS Frame Generation unlocker: %ls loaded into %ls in %.1f ms, "
		L"before NGX started", c.path.c_str(), proc.c_str(), ms);
	return s.note;
}

std::wstring dlssg_unlocker_note()
{
	State &s = state();
	std::lock_guard<std::mutex> guard(s.lock);
	return s.note;
}

void reset_dlssg_unlocker_for_tests()
{
	State &s = state();
	std::lock_guard<std::mutex> guard(s.lock);
	s.config = UnlockerConfig{};
	s.done = false;
	s.note.clear();
}

}
